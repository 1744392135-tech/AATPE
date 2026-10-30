#include "aatpe_diag.h"
#include "aatped_logging.h"

extern bool atomicWriteFile(const char* path, const std::string& content);

#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctime>

namespace aatpe {
namespace diag {

static std::mutex g_diagMutex;
static SystemPressure g_snapshot;
static std::string g_testRoot;

static bool g_warnedNoPsi = false;
static bool g_warnedNoGpu = false;

static bool g_envChecked = false;
static bool g_runningAsRoot = false;
static bool g_selinuxEnforcing = false;

static std::string rootProc() { return g_testRoot.empty() ? "/proc" : (g_testRoot + "/proc"); }
static std::string rootSys()  { return g_testRoot.empty() ? "/sys"  : (g_testRoot + "/sys"); }

#ifdef AATPE_HOST_TEST
void setTestRoot(const std::string& root) {
    std::lock_guard<std::mutex> lk(g_diagMutex);
    g_testRoot = root;
    g_warnedNoPsi = false;
    g_warnedNoGpu = false;
    g_envChecked = false;
}
#endif

static std::string readFileStr(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string readTrim(const std::string& path);

static float parsePsiAvgField(const std::string& content, size_t pos, size_t keyLen) {
    const char* p = content.c_str() + pos + keyLen;
    char* end = nullptr;
    double v = strtod(p, &end);
    if (end == p) return 0.0f;
    if (!std::isfinite(v)) return 0.0f;
    if (v < 0.0 || v > 100.0) return 0.0f;
    return (float)v;
}

static void parsePsiSomeAll(const std::string& content,
                            float& avg10, float& avg60, float& avg300) {
    avg10 = avg60 = avg300 = 0.0f;
    auto pos = content.find("some");
    if (pos == std::string::npos) return;
    auto a10 = content.find("avg10=", pos);
    if (a10 != std::string::npos) avg10 = parsePsiAvgField(content, a10, 6);
    auto a60 = content.find("avg60=", pos);
    if (a60 != std::string::npos) avg60 = parsePsiAvgField(content, a60, 6);
    auto a300 = content.find("avg300=", pos);
    if (a300 != std::string::npos) avg300 = parsePsiAvgField(content, a300, 7);
}

static uint64_t parsePsiTotal(const std::string& content) {
    auto pos = content.find("total=");
    if (pos == std::string::npos) return 0;
    const char* p = content.c_str() + pos + 6;
    char* end = nullptr;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return 0;
    if (v > 0xFFFFFFFFFFFFFull) return 0;
    return (uint64_t)v;
}

static void collectPsi(SystemPressure& s) {
    std::string cpuPath = std::string(rootProc()) + "/pressure/cpu";
    std::string cpuContent = readFileStr(cpuPath);
    if (cpuContent.empty()) {
        s.psiAvailable = false;
        if (!g_warnedNoPsi) {
            LOG_DEBUG("diag: PSI not available at %s (will use /proc/stat fallback)", cpuPath.c_str());
            g_warnedNoPsi = true;
        }
        return;
    }
    s.psiAvailable = true;
    parsePsiSomeAll(cpuContent, s.psiCpuSomePct, s.psiCpuSome60Pct, s.psiCpuSome300Pct);
    s.psiCpuTotalUsec = parsePsiTotal(cpuContent);

    std::string ioContent = readFileStr(std::string(rootProc()) + "/pressure/io");
    if (!ioContent.empty())
        parsePsiSomeAll(ioContent, s.psiIoSomePct, s.psiIoSome60Pct, s.psiIoSome300Pct);

    std::string memContent = readFileStr(std::string(rootProc()) + "/pressure/mem");
    if (!memContent.empty())
        parsePsiSomeAll(memContent, s.psiMemSomePct, s.psiMemSome60Pct, s.psiMemSome300Pct);
}

static std::string findGpuDevfreqNode() {
    std::string devfreqRoot = std::string(rootSys()) + "/class/devfreq";
    DIR* d = opendir(devfreqRoot.c_str());
    if (!d) return std::string();
    struct dirent* ent;
    std::string best;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);
        bool nameHint =
            name.find("gpu") != std::string::npos ||
            name.find("mali") != std::string::npos ||
            name.find("pvrsrv") != std::string::npos ||
            name.find("ged") != std::string::npos;
        if (nameHint) {
            best = name;
            break;
        }
    }
    closedir(d);
    return best;
}

static int parseVendorGpuUtil(const std::string& raw) {
    if (raw.empty()) return -1;
    auto colon = raw.find(':');
    std::string valPart = (colon != std::string::npos) ? raw.substr(colon + 1) : raw;
    size_t a = valPart.find_first_not_of(" \t\n\r");
    size_t b = valPart.find_last_not_of(" \t\n\r,");
    if (a == std::string::npos) return -1;
    valPart = valPart.substr(a, b - a + 1);
    auto comma = valPart.find(',');
    if (comma != std::string::npos) {
        std::string second = valPart.substr(comma + 1);
        a = second.find_first_not_of(" \t\n\r");
        b = second.find_last_not_of(" \t\n\r");
        if (a != std::string::npos) valPart = second.substr(a, b - a + 1);
    }
    int v = atoi(valPart.c_str());
    if (v < 0 || v > 100) return -1;
    return v;
}

static void probeEnvOnce() {
    if (g_envChecked) return;
    g_runningAsRoot = (geteuid() == 0);
    std::string en = readTrim(std::string(rootSys()) + "/fs/selinux/enforce");
    g_selinuxEnforcing = (en == "1");
    if (!g_testRoot.empty()) {
        std::string forceRoot = readTrim(g_testRoot + "/.aatpe_env_root");
        if (forceRoot == "1") g_runningAsRoot = true;
        else if (forceRoot == "0") g_runningAsRoot = false;
    }
    g_envChecked = true;
}
static bool trustWritableProbe() {
    probeEnvOnce();
    return !g_runningAsRoot;
}

static int probeNodeAccess(const std::string& path, std::string* outTrim = nullptr) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (outTrim) outTrim->clear();
        return 0;
    }
    std::string val = readTrim(path);
    if (outTrim) *outTrim = val;
    bool haveValue = !val.empty();
    bool writable = (trustWritableProbe() && access(path.c_str(), W_OK) == 0);
    if (writable) return 2;
    if (haveValue) return 1;
    return 0;
}

static std::string readTrim(const std::string& path) {
    std::string c = readFileStr(path);
    size_t a = c.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return std::string();
    size_t b = c.find_last_not_of(" \t\n\r");
    return c.substr(a, b - a + 1);
}

static const int kGedFreqLevelMax = 64;
static const int kGedFreqMinKhz   = 20000;
static int classifyGedFreq(int raw, bool* isLevel) {
    *isLevel = false;
    if (raw <= 0) return 0;
    if (raw <= kGedFreqLevelMax) { *isLevel = true; return 0; }
    if (raw >= kGedFreqMinKhz) return raw;
    return raw * 1000;
}

static void probeAppend(std::string& out, const char* tag, int accessLvl,
                        const std::string& value, bool withValue) {
    if (!out.empty()) out += ";";
    char sc = (accessLvl == 2) ? 'W' : (accessLvl == 1 ? 'R' : 'N');
    out += tag;
    out += "=";
    out += sc;
    if (withValue && accessLvl >= 1 && !value.empty()) {
        std::string v = value;
        if (v.size() > 16) v.resize(16);
        out += ":";
        out += v;
    }
}

static void collectGpu(SystemPressure& s) {
    std::string gpuNode = findGpuDevfreqNode();
    std::string gpuSource;
    bool hasFreqCtrl = false;
    std::string probe;

    if (!gpuNode.empty()) {
        s.gpuDevName = gpuNode;
        gpuSource = "devfreq/" + gpuNode;
        std::string base = std::string(rootSys()) + "/class/devfreq/" + gpuNode;

        std::string curFreq = readFileStr(base + "/cur_freq");
        if (!curFreq.empty()) {
            s.gpuFreqKhz = atoi(curFreq.c_str());
            s.gpuFreqAvail = true;
            s.gpuFreqRaw = s.gpuFreqKhz;
        }
        probeAppend(probe, "devfreq", s.gpuFreqAvail ? 1 : 0, "", false);
        if (probeNodeAccess(base + "/governor") == 2 ||
            probeNodeAccess(base + "/max_freq") == 2 ||
            probeNodeAccess(base + "/min_freq") == 2) {
            hasFreqCtrl = true;
        }

        std::string avail = readFileStr(base + "/available_frequencies");
        if (!avail.empty()) {
            std::istringstream iss(avail);
            int f;
            long mx = 0;
            while (iss >> f) if (f > mx) mx = f;
            s.gpuMaxKhz = (float)mx;
        }
        if (s.gpuMaxKhz == 0) {
            std::string maxF = readFileStr(base + "/max_freq");
            if (!maxF.empty()) s.gpuMaxKhz = atoi(maxF.c_str());
        }

        std::string gov = readFileStr(base + "/governor");
        if (!gov.empty()) {
            gov.erase(gov.find_last_not_of(" \n\r\t") + 1);
            s.gpuGovernor = gov;
        }

        std::string loadStr = readFileStr(base + "/load");
        if (!loadStr.empty()) {
            int v = atoi(loadStr.c_str());
            if (v > 100) v /= 10;
            if (v >= 0 && v <= 100) s.gpuUtilPct = v;
        }
    }

    struct GedNode { const char* tag; const char* name; bool key; };
    static const GedNode kGedNodes[] = {
        {"util",       "gpu_utilization",            true },
        {"curfreq",    "current_freqency",            true },
        {"prevfreq",   "previous_freqency",           true },
        {"boostlevel", "gpu_boost_level",            true },
        {"boost",      "custom_boost_gpu_freq",       true },
        {"upbound",    "custom_upbound_gpu_freq",     true },
        {"levelcnt",   "total_gpu_freq_level_count",  true },
        {"loadmode",   "dvfs_loading_mode",           false},
        {"margin",     "dvfs_margin_value",          false},
        {"kpi",        "ged_kpi",                    false},
        {"loadbase",   "loading_base_dvfs_step",      false},
        {"opplogs",    "opp_logs",                   false},
        {"timermarg",  "timer_base_dvfs_margin",      false},
    };
    std::string gedHal = std::string(rootSys()) + "/kernel/ged/hal";
    for (const GedNode& gn : kGedNodes) {
        std::string path = gedHal + "/" + gn.name;
        std::string val;
        int acc = probeNodeAccess(path, &val);
        probeAppend(probe, gn.tag, acc, val, gn.key);

        std::string baseTag = std::string("ged/hal/") + gn.name;
        if (strcmp(gn.name, "gpu_utilization") == 0 && s.gpuUtilPct < 0 && acc >= 1) {
            int v = parseVendorGpuUtil(val);
            if (v >= 0) { s.gpuUtilPct = v; if (gpuSource.empty()) gpuSource = baseTag; }
        }
        if (strcmp(gn.name, "current_freqency") == 0 && acc >= 1 && s.gpuFreqKhz == 0) {
            int raw = atoi(val.c_str());
            if (raw > 0) {
                s.gpuFreqRaw = raw;
                bool isLevel = false;
                s.gpuFreqKhz = classifyGedFreq(raw, &isLevel);
                s.gpuFreqIsLevel = isLevel;
                s.gpuFreqAvail = true;
                gpuSource = baseTag;
            }
        }
        if (strcmp(gn.name, "custom_upbound_gpu_freq") == 0 && acc >= 1 && s.gpuMaxKhz == 0) {
            int raw = atoi(val.c_str());
            bool isLevel = false;
            int khz = classifyGedFreq(raw, &isLevel);
            if (!isLevel && khz > 0) s.gpuMaxKhz = khz;
        }
        if (strcmp(gn.name, "custom_boost_gpu_freq") == 0 ||
            strcmp(gn.name, "custom_upbound_gpu_freq") == 0 ||
            strcmp(gn.name, "gpu_boost_level") == 0) {
            if (acc == 2) hasFreqCtrl = true;
        }
    }
    if (s.gpuFreqKhz == 0) {
        std::string gpuFreq = readTrim(gedHal + "/gpu_freq");
        if (!gpuFreq.empty()) {
            int raw = atoi(gpuFreq.c_str());
            s.gpuFreqRaw = raw;
            bool isLevel = false;
            s.gpuFreqKhz = classifyGedFreq(raw, &isLevel);
            s.gpuFreqIsLevel = isLevel;
            s.gpuFreqAvail = true;
            if (gpuSource.empty()) gpuSource = "ged/hal/gpu_freq";
        }
    }
    if (s.gpuMaxKhz == 0) {
        std::string cap = readTrim(gedHal + "/gpu_freq_cap");
        if (!cap.empty()) s.gpuMaxKhz = atoi(cap.c_str());
    }

    if (s.gpuUtilPct < 0) {
        std::string maliUtil = readFileStr(std::string(rootSys()) + "/class/misc/mali0/device/utilization");
        if (!maliUtil.empty()) {
            int v = parseVendorGpuUtil(maliUtil);
            if (v >= 0) {
                s.gpuUtilPct = v;
                if (gpuSource.empty()) gpuSource = "mali0/utilization";
            }
        }
    }
    if (s.gpuUtilPct < 0) {
        std::string kbase = readFileStr(std::string(rootSys()) + "/module/mali_kbase/parameters/gpu_utilization");
        if (!kbase.empty()) {
            int v = parseVendorGpuUtil(kbase);
            if (v >= 0) s.gpuUtilPct = v;
        }
    }

    static const char* kPlatDirs[] = {
        "gpufreq",
        "13000000.mali",
        "13fbb000.mali_dvfs_hint",
        "1100b000.eemgpu_fsm",
    };
    for (const char* pd : kPlatDirs) {
        std::string dir = std::string(rootSys()) + "/bus/platform/devices/" + pd;
        DIR* d = opendir(dir.c_str());
        int readable = 0, writable = 0;
        std::string firstReadable;
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string fp = dir + "/" + ent->d_name;
                struct stat st;
                if (stat(fp.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
                int acc = probeNodeAccess(fp);
                if (acc == 1) {
                    readable++;
                    if (firstReadable.empty()) firstReadable = ent->d_name;
                } else if (acc == 2) {
                    writable++;
                }
            }
            closedir(d);
        }
        if (writable > 0 && strcmp(pd, "gpufreq") == 0) hasFreqCtrl = true;
        int lvl = (writable > 0) ? 2 : (readable > 0) ? 1 : 0;
        std::string tag = std::string("plat_") + pd;
        probeAppend(probe, tag.c_str(), lvl, firstReadable, true);
    }

    if (s.gpuMaxKhz > 0 && s.gpuFreqKhz > 0 && !s.gpuFreqIsLevel) {
        s.gpuThrottled = (s.gpuFreqKhz < s.gpuMaxKhz * 95 / 100);
    }

    s.gpuCtrl = hasFreqCtrl;
    s.gpuProbe = probe;

    if (gpuSource.empty()) {
        s.gpuDevName = "none";
        if (!g_warnedNoGpu) {
            LOG_DEBUG("diag: no GPU node found (devfreq/ged/mali all empty)");
            g_warnedNoGpu = true;
        }
    } else {
        s.gpuDevName = gpuSource;
    }
}

static constexpr int kMaxCpusEnumerated = 256;

static std::vector<int> enumerateOnlineCpus() {
    std::vector<int> cpus;
    std::string onlinePath = rootSys() + "/devices/system/cpu/online";
    std::string online = readFileStr(onlinePath);
    while (!online.empty() && (online.back()=='\n'||online.back()=='\r'||
                               online.back()==' '||online.back()=='\t'))
        online.pop_back();
    if (!online.empty()) {
        size_t i = 0;
        while (i < online.size() && cpus.size() < (size_t)kMaxCpusEnumerated) {
            while (i < online.size() && (online[i]==','||online[i]==' ')) i++;
            if (i >= online.size()) break;
            if (online[i] < '0' || online[i] > '9') break;
            int lo = 0;
            while (i < online.size() && online[i] >= '0' && online[i] <= '9') {
                lo = lo*10 + (online[i]-'0'); i++;
            }
            int hi = lo;
            if (i < online.size() && online[i] == '-') {
                i++;
                hi = 0;
                while (i < online.size() && online[i] >= '0' && online[i] <= '9') {
                    hi = hi*10 + (online[i]-'0'); i++;
                }
            }
            if (lo > hi) std::swap(lo, hi);
            if (hi > kMaxCpusEnumerated) hi = kMaxCpusEnumerated;
            for (int c = lo; c <= hi && (int)cpus.size() < kMaxCpusEnumerated; c++)
                cpus.push_back(c);
        }
    }
    if (!cpus.empty()) return cpus;

    std::string base = rootSys() + "/devices/system/cpu/";
    DIR* d = ::opendir(base.c_str());
    if (d) {
        struct dirent* de;
        while ((de = ::readdir(d)) != nullptr && (int)cpus.size() < kMaxCpusEnumerated) {
            const char* p = de->d_name;
            if (p[0]!='c' || p[1]!='p' || p[2]!='u') continue;
            if (p[3] < '0' || p[3] > '9') continue;
            int n = 0;
            for (int k = 3; p[k] >= '0' && p[k] <= '9'; k++) n = n*10 + (p[k]-'0');
            cpus.push_back(n);
        }
        ::closedir(d);
    }
    return cpus;
}

static void collectCpuThrottle(SystemPressure& s) {
    int hwMax = 0;
    int curCap = 0;
    bool throttled = false;

    std::vector<int> cpus = enumerateOnlineCpus();
    for (int cpu : cpus) {
        std::string cpudir = std::string(rootSys()) + "/devices/system/cpu/cpu" +
                             std::to_string(cpu) + "/cpufreq";
        struct stat st;
        if (stat(cpudir.c_str(), &st) != 0) continue;

        std::string hwMaxStr = readFileStr(cpudir + "/cpuinfo_max_freq");
        std::string curMaxStr = readFileStr(cpudir + "/scaling_max_freq");
        if (hwMaxStr.empty() || curMaxStr.empty()) continue;

        int hw = atoi(hwMaxStr.c_str());
        int cur = atoi(curMaxStr.c_str());
        if (hw == 0) continue;

        if (hw > hwMax) hwMax = hw;
        if (curCap == 0 || cur < curCap) curCap = cur;

        if (cur < hw * 95 / 100) throttled = true;
    }
    s.cpuHwMaxKhz = hwMax;
    s.cpuCapKhz = curCap;
    s.cpuThrottled = throttled;
}

int64_t getThreadRqWaitNs(int tid) {
    std::string path = std::string(rootProc()) + "/" + std::to_string(tid) + "/schedstat";
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    int64_t run = 0, wait = 0, sumExec = 0;
    f >> run >> wait >> sumExec;
    if (f.fail()) return -1;
    return wait;
}

int64_t getThreadRunNs(int tid) {
    std::string path = std::string(rootProc()) + "/" + std::to_string(tid) + "/schedstat";
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    int64_t run = 0, wait = 0, sumExec = 0;
    f >> run >> wait >> sumExec;
    if (f.fail()) return -1;
    return run;
}

void collect() {
    SystemPressure s;
    try { collectPsi(s); } catch (...) { LOG_WARN("diag: collectPsi threw, skipped"); }
    try { collectGpu(s); } catch (...) { LOG_WARN("diag: collectGpu threw, skipped"); }
    try { collectCpuThrottle(s); } catch (...) { LOG_WARN("diag: collectCpuThrottle threw, skipped"); }
    s.sampleUptimeSec = (int64_t)time(nullptr);

    std::lock_guard<std::mutex> lk(g_diagMutex);
    s.rqWaitTop = g_snapshot.rqWaitTop;
    g_snapshot = s;
}

SystemPressure getSystemPressureSnapshot() {
    std::lock_guard<std::mutex> lk(g_diagMutex);
    return g_snapshot;
}

void setRqWaitTop(const std::vector<SystemPressure::RqWaitEntry>& top) {
    std::lock_guard<std::mutex> lk(g_diagMutex);
    g_snapshot.rqWaitTop = top;
}

static void writeOneLine(const char* path, const std::string& content) {
    atomicWriteFile(path, content);
}

void writeStateFiles(const char* stateDir) {
    SystemPressure s = getSystemPressureSnapshot();
    std::string dir = stateDir;

    {
        std::string line =
            "cpu_some=" + std::to_string(s.psiCpuSomePct) +
            ";cpu_some_60=" + std::to_string(s.psiCpuSome60Pct) +
            ";cpu_some_300=" + std::to_string(s.psiCpuSome300Pct) +
            ";io_some=" + std::to_string(s.psiIoSomePct) +
            ";io_some_60=" + std::to_string(s.psiIoSome60Pct) +
            ";io_some_300=" + std::to_string(s.psiIoSome300Pct) +
            ";mem_some=" + std::to_string(s.psiMemSomePct) +
            ";mem_some_60=" + std::to_string(s.psiMemSome60Pct) +
            ";mem_some_300=" + std::to_string(s.psiMemSome300Pct) +
            ";cpu_total_usec=" + std::to_string(s.psiCpuTotalUsec) +
            ";available=" + std::string(s.psiAvailable ? "1" : "0");
        writeOneLine((dir + "/pressure").c_str(), line);
    }

    {
        const char* freqUnit = s.gpuFreqIsLevel ? "level"
                             : (s.gpuFreqAvail  ? "khz" : "none");
        std::string line = "gpu_util=" + std::to_string(s.gpuUtilPct) +
                          ";gpu_freq=" + std::to_string(s.gpuFreqKhz) +
                          ";gpu_max=" + std::to_string(s.gpuMaxKhz) +
                          ";governor=" + s.gpuGovernor +
                          ";ctrl=" + std::string(s.gpuCtrl ? "1" : "0") +
                          ";freq_avail=" + std::string(s.gpuFreqAvail ? "1" : "0") +
                          ";freq_raw=" + std::to_string(s.gpuFreqRaw) +
                          ";freq_unit=" + freqUnit +
                          ";source=" + s.gpuDevName +
                          ";available=" + std::string(s.gpuDevName == "none" ? "0" : "1");
        writeOneLine((dir + "/gpu_load").c_str(), line);
    }

    {
        writeOneLine((dir + "/gpu_probe").c_str(), s.gpuProbe);
    }

    {
        std::string line = "cpu_throttled=" + std::string(s.cpuThrottled ? "1" : "0") +
                          ";cpu_cap_khz=" + std::to_string(s.cpuCapKhz) +
                          ";cpu_hw_max_khz=" + std::to_string(s.cpuHwMaxKhz) +
                          ";gpu_throttled=" + std::string(s.gpuThrottled ? "1" : "0");
        writeOneLine((dir + "/throttle_state").c_str(), line);
    }

    if (s.rqWaitTop.empty()) {
        writeOneLine((dir + "/rq_wait_top").c_str(), "");
    } else {
        std::string line;
        for (size_t i = 0; i < s.rqWaitTop.size() && i < 5; i++) {
            if (i) line += ",";
            line += std::to_string(s.rqWaitTop[i].tid) + ":" +
                    std::to_string(s.rqWaitTop[i].waitMs);
        }
        writeOneLine((dir + "/rq_wait_top").c_str(), line);
    }
}

}
}

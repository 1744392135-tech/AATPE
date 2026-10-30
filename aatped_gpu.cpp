#include "aatped.h"

std::atomic<int> g_gpuBusyPercent{0};
std::atomic<bool> g_gpuBottleneck{false};

GpuDualState g_gpuDual;
std::mutex   g_gpuDualMutex;
const long long kGpuSampleMinMs = 1000;

namespace {

long long nowMs(void) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string pickFirstExisting(const std::vector<std::string>& candidates) {
    for (const auto& p : candidates) {
        if (p.empty()) continue;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::string v = readFile(p.c_str());
            if (!v.empty()) return p;
        }
    }
    return std::string();
}

long long readNodeLl(const std::string& path) {
    if (path.empty()) return -1;
    std::string v = readFile(path.c_str());
    if (v.empty()) return -1;
    try {
        size_t pos = 0;
        long long val = std::stoll(v, &pos);
        (void)pos;
        return val;
    } catch (...) {
        return -1;
    }
}

std::string readNodeStr(const std::string& path) {
    if (path.empty()) return std::string();
    return readFile(path.c_str());
}

int normalizeBusyPct(long long raw) {
    if (raw < 0) return -1;
    long long u = raw;
    if (u > 1000) u = 100;
    else if (u > 100) u = u / 10;
    if (u < 0) u = 0;
    if (u > 100) u = 100;
    return (int)u;
}

void fillKgslNodes(const std::string& root) {
    g_gpuDual.nodeRoot    = root;
    g_gpuDual.nodeCurFreq  = pickFirstExisting({
        root + "/cur_freq",
        root + "/gpu_clock",
        root + "/gpuclk",
    });
    g_gpuDual.nodeMaxFreq  = pickFirstExisting({
        root + "/max_freq",
        root + "/gpu_max_clock",
    });
    g_gpuDual.nodeMinFreq  = pickFirstExisting({
        root + "/min_freq",
    });
    g_gpuDual.nodeUtil     = pickFirstExisting({
        root + "/gpu_busy_percentage",
        root + "/gpu_busy",
        root + "/gpu_gpu_busy_percentage",
    });
    g_gpuDual.nodeTemp     = pickFirstExisting({
        root + "/gpu_temp",
        root + "/temp",
    });
    g_gpuDual.nodeVoltage = pickFirstExisting({
        root + "/gpu_gfxrail_voltage",
        root + "/gpu_voltage",
    });
    g_gpuDual.nodeModel   = pickFirstExisting({
        root + "/gpu_model",
        root + "/gpu_model_name",
    });
}

static std::string pickMaliNode(const std::string& root,
                                 const std::vector<std::string>& names) {
    std::vector<std::string> candidates;
    candidates.reserve(names.size() * 2);
    for (const auto& n : names) candidates.push_back(root + "/" + n);
    for (const auto& n : names) candidates.push_back(root + "/device/" + n);
    return pickFirstExisting(candidates);
}

void fillMaliNodes(const std::string& root) {
    g_gpuDual.nodeRoot    = root;
    g_gpuDual.nodeCurFreq  = pickMaliNode(root, {
        "cur_freq", "gpu_freq", "mali_freq", "clock", "frequency",
        "gpu_cur_freq", "clk_freq", "mali_clk", "actual_freq",
    });
    g_gpuDual.nodeMaxFreq  = pickMaliNode(root, {
        "max_freq", "gpu_max_freq", "mali_max_freq", "max_clock",
    });
    g_gpuDual.nodeMinFreq  = pickMaliNode(root, {
        "min_freq", "gpu_min_freq", "min_clock",
    });
    g_gpuDual.nodeUtil     = pickMaliNode(root, {
        "gpu_busy", "gpu_utilization", "mali_utilization", "utilization", "load",
        "gpu_loading", "loading", "mali_util", "gpu_util", "busy",
    });
    g_gpuDual.nodeTemp     = pickMaliNode(root, {
        "gpu_temp", "temp", "gpu_temperature", "temperature",
    });
    g_gpuDual.nodeVoltage = pickMaliNode(root, {
        "gpu_voltage", "mali_voltage", "volt_now",
    });
    g_gpuDual.nodeModel   = pickMaliNode(root, {
        "gpu_model", "gpu_name", "mali_model",
    });
}

static bool fillMaliProcNodes(void) {
    std::string freq = pickFirstExisting({
        "/proc/mali/mali_freq",
        "/proc/mali/gpu_freq",
        "/proc/mali/clock",
    });
    std::string util = pickFirstExisting({
        "/proc/mali/mali_util",
        "/proc/mali/utilization",
        "/proc/mali/loading",
    });
    if (freq.empty() && util.empty()) return false;
    if (!freq.empty()) g_gpuDual.nodeCurFreq = freq;
    if (!util.empty()) g_gpuDual.nodeUtil = util;
    if (g_gpuDual.nodeRoot.empty()) g_gpuDual.nodeRoot = "/proc/mali";
    return true;
}

std::string findDevfreqGpuRoot(void) {
    DIR* d = opendir("/sys/class/devfreq");
    if (!d) return std::string();
    std::string best;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string name(de->d_name);
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        bool looksGpu =
            lower.find("gpu")   != std::string::npos ||
            lower.find("mali")  != std::string::npos ||
            lower.find("kgsl")  != std::string::npos ||
            lower.find("3d0")   != std::string::npos ||
            lower.find("gfx")   != std::string::npos;
        if (!looksGpu) continue;
        std::string root = std::string("/sys/class/devfreq/") + name;
        std::string cur = pickFirstExisting({
            root + "/cur_freq",
            root + "/gpu_cur_freq",
        });
        if (!cur.empty()) { best = root; break; }
    }
    closedir(d);
    return best;
}

GpuPlatform detectFromCpuinfo(std::string& outModel) {
    std::ifstream f("/proc/cpuinfo");
    if (!f) return GPU_PLATFORM_UNKNOWN;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("Hardware") == std::string::npos) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string hw = line.substr(colon + 1);
        trim(hw);
        outModel = hw;
        if (hw.find("Qualcomm")   != std::string::npos ||
            hw.find("Snapdragon") != std::string::npos ||
            hw.find("SM")         != std::string::npos ||
            hw.find("MSM")        != std::string::npos) {
            return GPU_PLATFORM_KGSL;
        }
        if (hw.find("MT")       != std::string::npos ||
            hw.find("Mediatek") != std::string::npos ||
            hw.find("MediaTek") != std::string::npos) {
            return GPU_PLATFORM_MALI;
        }
        break;
    }
    return GPU_PLATFORM_UNKNOWN;
}

}

void gpuDetectPlatform(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (g_gpuDual.detected) return;

    const char* kgslRoots[] = {
        "/sys/class/kgsl/kgsl-3d0",
        "/sys/class/kgsl/kgsl-3d0/device",
    };
    for (const char* root : kgslRoots) {
        std::string cur = pickFirstExisting({
            std::string(root) + "/cur_freq",
            std::string(root) + "/gpu_clock",
        });
        if (cur.empty()) continue;
        fillKgslNodes(root);
        g_gpuDual.platform   = GPU_PLATFORM_KGSL;
        g_gpuDual.platformTag = "kgsl";
        g_gpuDual.detected    = true;
        g_gpuDual.available   = true;
        LOG_INFO("GPU: detected Qualcomm KGSL (%s), cur=%s util=%s",
                 root, g_gpuDual.nodeCurFreq.c_str(),
                 g_gpuDual.nodeUtil.c_str());
        break;
    }

    if (!g_gpuDual.detected) {
        const char* maliRoots[] = {
            "/sys/class/misc/mali0",
            "/sys/class/misc/mali0/device",
            "/sys/kernel/gpu",
            "/sys/class/devfreq/mali0",
            "/sys/class/devfreq/mali-gpu",
            "/sys/class/devfreq/13000000.mali",
            "/sys/class/devfreq/13040000.mali",
            "/sys/class/devfreq/130a0000.mali",
            "/sys/class/devfreq/130b0000.mali",
            "/sys/class/devfreq/mali",
        };
        LOG_INFO("GPU: probing Mali roots: misc/mali0, misc/mali0/device, "
                 "/sys/kernel/gpu, devfreq/mali0, devfreq/13000000.mali ...");
        for (const char* root : maliRoots) {
            std::string cur = pickFirstExisting({
                std::string(root) + "/cur_freq",
                std::string(root) + "/gpu_freq",
                std::string(root) + "/gpu_cur_freq",
                std::string(root) + "/mali_freq",
                std::string(root) + "/clock",
            });
            if (cur.empty()) continue;
            fillMaliNodes(root);
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mali";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: selected root=%s freq_node=%s util_node=%s",
                     root, g_gpuDual.nodeCurFreq.c_str(),
                     g_gpuDual.nodeUtil.c_str());
            break;
        }
    if (!g_gpuDual.detected) {
        const char* mtkRoots[] = {
            "/sys/class/devfreq/13fbb000.mali_dvfs_hint",
            "/sys/class/devfreq/1100b000.eemgpu_fsm",
            "/sys/class/devfreq/13000000.mali",
        };
        for (const char* root : mtkRoots) {
            std::string cur = pickFirstExisting({
                std::string(root) + "/cur_freq",
                std::string(root) + "/gpu_freq",
                std::string(root) + "/load",
                std::string(root) + "/utilization",
            });
            if (cur.empty()) continue;
            fillMaliNodes(root);
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mtk_ged";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: MTK GED root=%s freq_node=%s", root, cur.c_str());
            break;
        }
        if (!g_gpuDual.detected) {
            std::string oppIdx = pickFirstExisting({
                "/proc/gpufreq/gpufreq_opp_index",
                "/proc/gpufreq/gpufreq_cur_opp",
            });
            std::string oppFreq = pickFirstExisting({
                "/proc/gpufreq/gpufreq_opp_freq",
                "/proc/gpufreq/gpufreq_cur_freq",
            });
            if (!oppFreq.empty()) {
                g_gpuDual.platform   = GPU_PLATFORM_MALI;
                g_gpuDual.platformTag = "mtk_gpufreq";
                g_gpuDual.nodeCurFreq = oppFreq;
                g_gpuDual.nodeGovernor = "/proc/gpufreq/gpufreq_governor";
                g_gpuDual.nodeMaxFreq  = "/proc/gpufreq/gpufreq_max_opp";
                g_gpuDual.nodeMinFreq  = "/proc/gpufreq/gpufreq_min_opp";
                g_gpuDual.detected    = true;
                g_gpuDual.available   = true;
                LOG_INFO("GPU: MTK /proc/gpufreq freq node=%s", oppFreq.c_str());
            } else if (!oppIdx.empty()) {
                g_gpuDual.platform   = GPU_PLATFORM_MALI;
                g_gpuDual.platformTag = "mtk_gpufreq_index";
                g_gpuDual.nodeCurFreq = "";
                g_gpuDual.nodeUtil = oppIdx;
                g_gpuDual.detected    = true;
                g_gpuDual.available   = false;
                LOG_INFO("GPU: MTK /proc/gpufreq index-only (no real freq node); "
                         "current_freq unavailable");
            }
        }
    }

        if (!g_gpuDual.detected && fillMaliProcNodes()) {
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mali";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: selected /proc/mali freq_node=%s util_node=%s",
                     g_gpuDual.nodeCurFreq.c_str(),
                     g_gpuDual.nodeUtil.c_str());
        }
    }

    if (!g_gpuDual.detected) {
        std::string root = findDevfreqGpuRoot();
        if (!root.empty()) {
            fillMaliNodes(root);
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mali";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: detected GPU via devfreq (%s)", root.c_str());
        }
    }

    if (!g_gpuDual.detected) {
        std::string hw;
        GpuPlatform fb = detectFromCpuinfo(hw);
        if (fb != GPU_PLATFORM_UNKNOWN) {
            g_gpuDual.platform   = fb;
            g_gpuDual.platformTag = (fb == GPU_PLATFORM_KGSL) ? "kgsl" : "mali";
            g_gpuDual.model       = hw;
            g_gpuDual.detected    = true;
            g_gpuDual.available   = false;
            LOG_INFO("GPU: cpuinfo fallback platform=%s model=%s (no sysfs nodes)",
                     g_gpuDual.platformTag.c_str(), hw.c_str());
        } else {
            g_gpuDual.platform   = GPU_PLATFORM_UNKNOWN;
            g_gpuDual.platformTag = "unknown";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = false;
            LOG_WARN("GPU: no KGSL/Mali/devfreq nodes found, platform=unknown");
        }
    }

    if (!g_gpuDual.nodeModel.empty()) {
        std::string m = readNodeStr(g_gpuDual.nodeModel);
        if (!m.empty()) g_gpuDual.model = m;
    }
    if (g_gpuDual.model.empty()) {
        g_gpuDual.model = g_gpuDual.platformTag;
    }
}

void gpuSampleAll(void) {
    gpuDetectPlatform();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (!g_gpuDual.detected) return;

    long long now = nowMs();
    if (g_gpuDual.lastSampleMs != 0 &&
        now - g_gpuDual.lastSampleMs < kGpuSampleMinMs) {
        return;
    }
    g_gpuDual.lastSampleMs = now;

    long long curHz = readNodeLl(g_gpuDual.nodeCurFreq);
    long long maxHz = readNodeLl(g_gpuDual.nodeMaxFreq);
    long long minHz = readNodeLl(g_gpuDual.nodeMinFreq);
    g_gpuDual.curFreqMhz = (curHz >= 0) ? (int)(curHz / 1000000LL) : -1;
    g_gpuDual.maxFreqMhz = (maxHz >= 0) ? (int)(maxHz / 1000000LL) : -1;
    g_gpuDual.minFreqMhz = (minHz >= 0) ? (int)(minHz / 1000000LL) : -1;

    auto rescaleKhz = [](long long raw, int mhz) -> int {
        if (raw >= 0 && mhz < 10 && raw >= 100 && raw <= 5000000LL) {
            return (int)(raw / 1000LL);
        }
        return mhz;
    };
    {
        int afterHz = (maxHz >= 0) ? (int)(maxHz / 1000000LL) : -1;
        bool looksLikeKhz = (maxHz >= 0 && afterHz >= 0 && afterHz < 10 &&
                             maxHz >= 100 && maxHz <= 5000000LL);
        if (looksLikeKhz) g_gpuDual.nodeFreqIsKhz = true;
    }
    g_gpuDual.curFreqMhz = rescaleKhz(curHz, g_gpuDual.curFreqMhz);
    g_gpuDual.maxFreqMhz = rescaleKhz(maxHz, g_gpuDual.maxFreqMhz);
    g_gpuDual.minFreqMhz = rescaleKhz(minHz, g_gpuDual.minFreqMhz);

    if (g_gpuDual.savedDefaultMaxHz < 0 && maxHz > 0) {
        g_gpuDual.savedDefaultMaxHz = maxHz;
    }

    long long util = readNodeLl(g_gpuDual.nodeUtil);
    g_gpuDual.utilPct = normalizeBusyPct(util);

    long long t = readNodeLl(g_gpuDual.nodeTemp);
    if (t < 0) {
        g_gpuDual.tempC = -1;
    } else {
        int tc;
        if (t >= 1000) {
            tc = (int)(t / 1000LL);
        } else {
            tc = (int)t;
        }
        if (tc < 0 || tc > 150) {
            LOG_WARN("GPU: temp reading %lld out of range (tc=%d) -> invalid", t, tc);
            g_gpuDual.tempC = -1;
        } else {
            g_gpuDual.tempC = tc;
        }
    }

    long long v = readNodeLl(g_gpuDual.nodeVoltage);
    if (v < 0) {
        g_gpuDual.voltageUv = -1;
    } else {
        if (v > 2000000LL) v = 2000000LL;
        g_gpuDual.voltageUv = (int)v;
    }

    if (g_gpuDual.curFreqMhz >= 0 || g_gpuDual.utilPct >= 0) {
        g_gpuDual.available = true;
    }

    {
        static long long s_lastDiagLogMs = 0;
        if (s_lastDiagLogMs == 0 || (now - s_lastDiagLogMs) >= 30000) {
            s_lastDiagLogMs = now;
            LOG_INFO("GPU-DIAG: platform=%s root=%s freq_node=%s util_node=%s "
                     "cur=%dMHz util=%d%% temp=%dC avail=%d",
                     g_gpuDual.platformTag.c_str(),
                     g_gpuDual.nodeRoot.c_str(),
                     g_gpuDual.nodeCurFreq.c_str(),
                     g_gpuDual.nodeUtil.c_str(),
                     g_gpuDual.curFreqMhz, g_gpuDual.utilPct,
                     g_gpuDual.tempC, (int)g_gpuDual.available);
        }
    }
}

int aatpeParseGedFreqMhz(const std::string& content) {
    size_t pos = 0;
    while (pos < content.size() && !isdigit((unsigned char)content[pos])) pos++;
    if (pos >= content.size()) return 0;
    long val = strtol(content.c_str() + pos, nullptr, 10);
    if (val <= 0) return 0;
    return (val > 100000) ? (int)(val / 1000) : (int)val;
}

void gpuWriteStateFiles(void) {
    std::string platformTag, model, nodeRoot;
    int pubFreq, pubMax, pubMin, pubUtil, pubTemp, pubVolt;
    {
        std::lock_guard<std::mutex> lk(g_gpuDualMutex);
        std::string dir = STATE_DIR;
        (void)dir;

        pubFreq  = (g_gpuDual.curFreqMhz  >= 0) ? g_gpuDual.curFreqMhz  : 0;
        pubMax   = (g_gpuDual.maxFreqMhz  >= 0) ? g_gpuDual.maxFreqMhz  : 0;
        pubMin   = (g_gpuDual.minFreqMhz  >= 0) ? g_gpuDual.minFreqMhz  : 0;
        pubUtil  = (g_gpuDual.utilPct     >= 0) ? g_gpuDual.utilPct     : 0;
        pubTemp  = (g_gpuDual.tempC       >= 0) ? g_gpuDual.tempC       : 0;
        pubVolt  = (g_gpuDual.voltageUv   >= 0) ? g_gpuDual.voltageUv   : 0;
        platformTag = g_gpuDual.platformTag;
        model       = g_gpuDual.model;
        nodeRoot    = g_gpuDual.nodeRoot;
    }

    std::string dir = STATE_DIR;

    static int s_gedCachedFreq = 0;
    static int s_gedCachedMax = 0;
    static long long s_gedLastReadMs = 0;
    auto readGedFreqNode = [](const char* path) -> int {
        FILE* f = fopen(path, "r");
        if (!f) return 0;
        char buf[256] = {0};
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
        fclose(f);
        return aatpeParseGedFreqMhz(buf);
    };
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
    bool gedCacheValid = (nowMs - s_gedLastReadMs) < 1000;
    if (pubFreq == 0) {
        if (gedCacheValid && s_gedCachedFreq > 0) {
            pubFreq = s_gedCachedFreq;
        } else {
            pubFreq = readGedFreqNode("/sys/kernel/ged/hal/gpu_freq");
            s_gedCachedFreq = pubFreq;
            s_gedLastReadMs = nowMs;
        }
    }
    if (pubMax == 0) {
        if (gedCacheValid && s_gedCachedMax > 0) {
            pubMax = s_gedCachedMax;
        } else {
            pubMax = readGedFreqNode("/sys/kernel/ged/hal/gpu_max_freq");
            s_gedCachedMax = pubMax;
        }
    }

    writeFile((dir + "/gpu_platform").c_str(),  platformTag);
    writeFile((dir + "/gpu_model").c_str(),     model);
    writeFile((dir + "/gpu_root").c_str(),      nodeRoot);
    writeFile((dir + "/gpu_freq").c_str(),      pubFreq);
    writeFile((dir + "/gpu_max_freq").c_str(),  pubMax);
    writeFile((dir + "/gpu_min_freq").c_str(),  pubMin);
    writeFile((dir + "/gpu_util").c_str(),      pubUtil);
    writeFile((dir + "/gpu_temp").c_str(),     pubTemp);
    writeFile((dir + "/gpu_voltage").c_str(),  pubVolt);
}

void gpuAppendMetrics(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (!g_gpuDual.available) return;

    std::string path = std::string(METRICS_DIR) + "/gpu_metrics.csv";
    const char* header =
        "epoch,platform,cur_mhz,max_mhz,min_mhz,util_pct,temp_c,voltage_uv\n";

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    struct stat st;
    bool needHeader = (fstat(fd, &st) != 0 || st.st_size == 0);
    bool roll = (!needHeader && st.st_size > (256L * 1024L));

    if (roll) {
        close(fd);
        int rfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (rfd < 0) return;
        dprintf(rfd, "%s", header);
        dprintf(rfd, "%lld,%s,%d,%d,%d,%d,%d,%d\n",
            (long long)time(nullptr), g_gpuDual.platformTag.c_str(),
            g_gpuDual.curFreqMhz, g_gpuDual.maxFreqMhz,
            g_gpuDual.minFreqMhz, g_gpuDual.utilPct,
            g_gpuDual.tempC, g_gpuDual.voltageUv);
        ::close(rfd);
        return;
    }

    FILE* af = ::fdopen(fd, "a");
    if (!af) { ::close(fd); return; }
    if (needHeader) std::fputs(header, af);
    fprintf(af, "%lld,%s,%d,%d,%d,%d,%d,%d\n",
        (long long)time(nullptr), g_gpuDual.platformTag.c_str(),
        g_gpuDual.curFreqMhz, g_gpuDual.maxFreqMhz,
        g_gpuDual.minFreqMhz, g_gpuDual.utilPct,
        g_gpuDual.tempC, g_gpuDual.voltageUv);
    std::fclose(af);
    (void)0;
}

bool gpuSetMaxFreqMhz(int mhz) {
    gpuDetectPlatform();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (g_gpuDual.nodeMaxFreq.empty()) {
        LOG_WARN("GPU: setMaxFreq no writable max_freq node (platform=%s)",
                 g_gpuDual.platformTag.c_str());
        return false;
    }
    if (g_gpuDual.maxFreqMhz > 0 && mhz > g_gpuDual.maxFreqMhz) {
        mhz = g_gpuDual.maxFreqMhz;
    }
    if (g_gpuDual.minFreqMhz > 0 && mhz < g_gpuDual.minFreqMhz) {
        mhz = g_gpuDual.minFreqMhz;
    }
    long long outVal = g_gpuDual.nodeFreqIsKhz
        ? (long long)mhz * 1000LL
        : (long long)mhz * 1000000LL;
    std::string s = std::to_string(outVal);
    if (!writeFile(g_gpuDual.nodeMaxFreq.c_str(), s)) {
        LOG_WARN("GPU: setMaxFreq write failed (%s = %lld %s) errno=%d (%s)",
                 g_gpuDual.nodeMaxFreq.c_str(), outVal,
                 g_gpuDual.nodeFreqIsKhz ? "kHz" : "Hz", errno, aatpeErrStr(errno));
        return false;
    }
    LOG_INFO("GPU: max freq capped to %d MHz (%lld %s) via %s",
             mhz, outVal, g_gpuDual.nodeFreqIsKhz ? "kHz" : "Hz",
             g_gpuDual.nodeMaxFreq.c_str());
    return true;
}

bool gpuRestoreDefaultFreq(void) {
    gpuDetectPlatform();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (g_gpuDual.savedDefaultMaxHz < 0 || g_gpuDual.nodeMaxFreq.empty()) {
        LOG_WARN("GPU: restoreFreq no saved default / node unavailable");
        return false;
    }
    std::string s = std::to_string(g_gpuDual.savedDefaultMaxHz);
    if (!writeFile(g_gpuDual.nodeMaxFreq.c_str(), s)) {
        LOG_WARN("GPU: restoreFreq write failed errno=%d (%s)",
                 errno, aatpeErrStr(errno));
        return false;
    }
    LOG_INFO("GPU: restored default max freq to %lld Hz",
             g_gpuDual.savedDefaultMaxHz);
    return true;
}

int gpuGetPlatform(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    return (int)g_gpuDual.platform;
}

std::string gpuGetPlatformTag(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    return g_gpuDual.platformTag;
}

std::string gpuGetModel(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    return g_gpuDual.model;
}

static std::string s_gpuTypeOwner = "unknown";

void readGpuUtilization(void) {
    gpuSampleAll();
    gpuWriteStateFiles();
    gpuAppendMetrics();

    {
        std::lock_guard<std::mutex> lk(g_gpuDualMutex);
        int u = g_gpuDual.utilPct;
        if (u < 0) u = 0;
        g_gpuBusyPercent = u;
        g_gpuUtilization.store((float)u, std::memory_order_relaxed);
        if (g_gpuDual.platformTag != s_gpuTypeOwner) {
            s_gpuTypeOwner = g_gpuDual.platformTag;
            char* published = strdup(s_gpuTypeOwner.c_str());
            g_gpuType.store(published, std::memory_order_relaxed);
        }
        g_gpuState.available   = g_gpuDual.available;
        g_gpuState.utilization = u;
        g_gpuState.type        = g_gpuDual.platformTag;
    }
}

bool isGpuBottleneck(float cpuUtilization) {
    g_gpuBottleneck = (g_gpuBusyPercent.load(std::memory_order_relaxed) > 80 &&
                       cpuUtilization < 50);
    return g_gpuBottleneck;
}

void aatpeGpuRefreshUtilForBoost(void) {
    gpuSampleAll();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    int u = g_gpuDual.utilPct;
    if (u >= 0) {
        g_gpuUtilization.store((float)u, std::memory_order_relaxed);
    }
}

int getGpuBottleneckDecisionInterval(void) {
    return g_gpuBottleneck ? 200 : 100;
}

static std::atomic<long long> s_gpuPeakMax{0};
static std::atomic<long long> s_gpuThrottleLastMs{0};
static constexpr long long kGpuThrottleMinMs = 1000;

bool aatpeGpuNodeAvailable(int vendor) {
    if (vendor == 0) {
        return access("/sys/class/misc/mali0/device/devfreq/cur_freq", F_OK) == 0;
    } else {
        return access("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", F_OK) == 0;
    }
}

static std::string resolveMaliDevfreq() {
    static std::mutex cachedMtx;
    static std::string cached;
    std::lock_guard<std::mutex> lk(cachedMtx);
    if (!cached.empty()) return cached;
    if (access("/sys/class/misc/mali0/device/devfreq/cur_freq", F_OK) == 0) {
        cached = "/sys/class/misc/mali0/device/devfreq"; return cached;
    }
    DIR* d = opendir("/sys/class/devfreq");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strstr(e->d_name, "mali")) {
                std::string p = std::string("/sys/class/devfreq/") + e->d_name + "/cur_freq";
                if (access(p.c_str(), F_OK) == 0) {
                    cached = std::string("/sys/class/devfreq/") + e->d_name;
                    closedir(d); return cached;
                }
            }
        }
        closedir(d);
    }
    return cached;
}

void aatpeGpuThrottleTick() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = s_gpuThrottleLastMs.load(std::memory_order_relaxed);
    if (last != 0 && (now - last) < kGpuThrottleMinMs) return;
    s_gpuThrottleLastMs.store(now, std::memory_order_relaxed);

    std::string mali = resolveMaliDevfreq();
    if (!mali.empty()) {
        int cur = readInt((mali + "/cur_freq").c_str(), -1);
        int mx = readInt((mali + "/max_freq").c_str(), -1);
        if (cur >= 0 && mx >= 0) {
            long long peak = s_gpuPeakMax.load(std::memory_order_relaxed);
            if (mx > peak) s_gpuPeakMax.store(mx, std::memory_order_relaxed);
            peak = s_gpuPeakMax.load(std::memory_order_relaxed);
            float util = g_gpuUtilization.load();
            bool gpuBusy = (util >= 50.0f);
            writeFileAtomic((std::string(STATE_DIR) + "/gpu_hard_throttle").c_str(),
                (gpuBusy && cur < (int)(mx*0.95)) ? "1" : "0");
            writeFileAtomic((std::string(STATE_DIR) + "/gpu_thermal_cap").c_str(),
                (peak>0 && mx < (int)(peak*0.95)) ? "1" : "0");
            return;
        }
    }
    int cur = readInt("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", -1);
    int mx = readInt("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", -1);
    if (cur >= 0 && mx >= 0) {
        long long peak = s_gpuPeakMax.load(std::memory_order_relaxed);
        if (mx > peak) s_gpuPeakMax.store(mx, std::memory_order_relaxed);
        peak = s_gpuPeakMax.load(std::memory_order_relaxed);
        float util = g_gpuUtilization.load();
        bool gpuBusy = (util >= 50.0f);
        writeFileAtomic((std::string(STATE_DIR) + "/gpu_hard_throttle").c_str(),
            (gpuBusy && cur < (int)(mx*0.95)) ? "1" : "0");
        writeFileAtomic((std::string(STATE_DIR) + "/gpu_thermal_cap").c_str(),
            (peak>0 && mx < (int)(peak*0.95)) ? "1" : "0");
    }
}

#include "aatpe_caps.h"
#include "aatped_logging.h"
#include "aatpe_sched_abi.h"

extern bool atomicWriteFile(const char* path, const std::string& content);
extern bool writeFile(const char* path, const std::string& content);
extern std::string readFile(const char* path);

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <cerrno>
#include <fcntl.h>
#include <ctime>

namespace aatpe {
namespace caps {

static std::mutex g_capsMutex;
static DeviceCaps g_caps;
static std::string g_testRoot;

static std::string rootProc() { return g_testRoot.empty() ? "/proc" : (g_testRoot + "/proc"); }
static std::string rootSys()  { return g_testRoot.empty() ? "/sys"  : (g_testRoot + "/sys"); }
static std::string rootDev()  { return g_testRoot.empty() ? "/dev"  : (g_testRoot + "/dev"); }

#ifdef AATPE_HOST_TEST
void setTestRoot(const std::string& root) {
    std::lock_guard<std::mutex> lk(g_capsMutex);
    g_testRoot = root;
    g_caps = DeviceCaps{};
}
#endif

static std::string readFileStr(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void probeKernel(DeviceCaps& c) {
    struct utsname u;
    if (uname(&u) == 0) {
        c.kernelVersion = u.release;
        sscanf(u.release, "%d.%d", &c.kernelMajor, &c.kernelMinor);
    }
}

static void probeCgroup(DeviceCaps& c) {
    std::string mountinfo = readFileStr(rootProc() + "/self/mountinfo");
    bool hasCgroup2 = mountinfo.find("cgroup2") != std::string::npos;
    bool hasCgroup1 = mountinfo.find("cgroup") != std::string::npos && !hasCgroup2;

    if (!hasCgroup2 && !hasCgroup1) {
        if (pathExists(rootSys() + "/fs/cgroup/cgroup.controllers")) {
            hasCgroup2 = true;
        } else if (pathExists(rootDev() + "/cpuset")) {
            hasCgroup1 = true;
        }
    }

    if (hasCgroup2) {
        c.cgroupVer = DeviceCaps::CgroupVersion::V2;
        c.cpusetRoot = rootSys() + "/fs/cgroup";
        c.cpuRoot    = rootSys() + "/fs/cgroup";
    } else if (hasCgroup1) {
        c.cgroupVer = DeviceCaps::CgroupVersion::V1;
        c.cpusetRoot = rootDev() + "/cpuset";
        c.cpuRoot    = rootDev() + "/cpuctl";
    }

    std::string topAppPath = c.cpusetRoot + "/top-app";
    if (pathExists(topAppPath + "/cpus")) {
        c.cpusetTopAppExists = true;
        c.cpusetTopAppCpus = readFileStr(topAppPath + "/cpus");
        c.cpusetTopAppCpus.erase(c.cpusetTopAppCpus.find_last_not_of(" \n\r\t") + 1);
    }
}

static int s_uclampProbeErrno = 0;

static void probeUclampTask(DeviceCaps& c) {
#ifdef __ANDROID__
    struct sched_attr attr = {};
    attr.size = sizeof(attr);
    attr.sched_policy = 0;
    attr.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN;
    attr.sched_util_min = 0;
    long rc = syscall(SYS_sched_setattr, 0, &attr, 0);
    if (rc == 0) {
        c.uclampTaskAvailable = true;
        s_uclampProbeErrno = 0;
    } else {
        c.uclampTaskAvailable = false;
        s_uclampProbeErrno = errno;
    }
#else
    c.uclampTaskAvailable = (s_uclampProbeErrno == 0);
#endif
}

static void probeUclamp(DeviceCaps& c) {
    probeUclampTask(c);

    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1) {
        if (pathExists(c.cpuRoot + "/top-app/cpu.uclamp.min")) {
            c.uclampCgroupV1 = true;
        }
    } else if (c.cgroupVer == DeviceCaps::CgroupVersion::V2) {
        if (pathExists(c.cpuRoot + "/cpu.uclamp.min")) {
            c.uclampCgroupV2 = true;
        }
    }
    c.uclampAvailable = c.uclampTaskAvailable || c.uclampCgroupV1 || c.uclampCgroupV2;
}

const char* inferCgroupUclampScaleReadOnly(const std::string& nodeValue) {
    if (nodeValue.empty()) return "unknown";
    const char* s = nodeValue.c_str();
    char* end = nullptr;
    float v = std::strtof(s, &end);
    if (end == s || !std::isfinite(v)) return "unknown";
    if (v > 101.5f) return "raw1024";
    return "percent";
}

static void probeCgroupUclampScale(DeviceCaps& c) {
    if (!c.uclampCgroupV1 && !c.uclampCgroupV2) {
        c.cgroupUclampScale = "unknown";
        return;
    }
    std::string path;
    if (c.uclampCgroupV2) {
        path = c.cpuRoot + "/cpu.uclamp.min";
    } else {
        path = c.cpuRoot + "/top-app/cpu.uclamp.min";
    }
    std::string cur = readFile(path.c_str());
    c.cgroupUclampScale = inferCgroupUclampScaleReadOnly(cur);
}

#ifdef AATPE_HOST_TEST
void setTaskUclampProbeResultForTest(bool available, int probeErrno) {
    s_uclampProbeErrno = probeErrno;
    std::lock_guard<std::mutex> lk(g_capsMutex);
    g_caps.uclampTaskAvailable = available;
    g_caps.uclampAvailable = available || g_caps.uclampCgroupV1 || g_caps.uclampCgroupV2;
}
#endif

static void probeSchedtune(DeviceCaps& c) {
    if (pathExists(rootDev() + "/stune")) {
        c.schedtuneAvailable = true;
        c.schedtuneRoot = rootDev() + "/stune";
    }
}

static void probePsi(DeviceCaps& c) {
    if (pathExists(rootProc() + "/pressure/cpu")) {
        c.psiAvailable = true;
    }
}

static void probeGpu(DeviceCaps& c) {
    bool devfreqGpu = false, devfreqMali = false, devfreqKgsl = false;
    std::string devfreqRoot = rootSys() + "/class/devfreq";
    DIR* d = opendir(devfreqRoot.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name(ent->d_name);
            if (name.find("kgsl") != std::string::npos) { devfreqKgsl = true; }
            else if (name.find("mali") != std::string::npos) { devfreqMali = true; }
            else if (name.find("gpu") != std::string::npos) { devfreqGpu = true; }
        }
        closedir(d);
    }
    bool kgslNode = pathExists(rootSys() + "/class/kgsl/kgsl-3d0/gpubusy") ||
                    pathExists(rootSys() + "/class/kgsl/kgsl-3d0");
    bool gedHal = false;
    const char* gedCandidates[] = {
        "kernel/ged/hal/gpu_freq",
        "kernel/ged/hal/current_freqency",
        "kernel/ged/hal/gpu_utilization",
    };
    for (const char* rel : gedCandidates) {
        if (pathExists(rootSys() + "/" + rel)) { gedHal = true; break; }
    }
    bool gedDev = pathExists("/dev/ged");

    const char* tag = classifyGpuPlatform(devfreqGpu, devfreqMali, devfreqKgsl,
                                         kgslNode, gedHal, gedDev);
    if (strcmp(tag, "none") != 0) {
        c.gpuDetected = true;
        c.gpuSource = tag;
    }
}

static void probeLoader(DeviceCaps& c) {
    const char* env = getenv("INIT_RC");
    if (env && strstr(env, "magisk")) {
        c.loader = DeviceCaps::Loader::MAGISK;
        return;
    }
    env = getenv("KSU");
    if (env) {
        c.loader = DeviceCaps::Loader::KSU;
        return;
    }
    env = getenv("APATCH");
    if (env) {
        c.loader = DeviceCaps::Loader::APATCH;
        return;
    }
    if (pathExists("/data/adb/magisk")) c.loader = DeviceCaps::Loader::MAGISK;
    else if (pathExists("/data/adb/ksu")) c.loader = DeviceCaps::Loader::KSU;
    else if (pathExists("/data/adb/apatch")) c.loader = DeviceCaps::Loader::APATCH;
}

static void probePageSize(DeviceCaps& c) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) c.pageSize = (int)ps;
    c.pageSize16K = (c.pageSize >= 16384);
}

void probe() {
    try {
        DeviceCaps c;
        probeKernel(c);
        probeCgroup(c);
        probeUclamp(c);
        probeCgroupUclampScale(c);
        probeSchedtune(c);
        probePsi(c);
        probeGpu(c);
        probeLoader(c);
        probePageSize(c);
        c.probeComplete = true;
        c.probeUptimeSec = (int64_t)time(nullptr);

        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_caps = c;
        LOG_INFO("caps: kernel=%s cgroup=%s uclamp=%d psi=%d gpu=%d loader=%d page=%d",
                 c.kernelVersion.c_str(),
                 c.cgroupVer == DeviceCaps::CgroupVersion::V1 ? "v1" :
                 c.cgroupVer == DeviceCaps::CgroupVersion::V2 ? "v2" : "unknown",
                 c.uclampAvailable, c.psiAvailable, c.gpuDetected,
                 (int)c.loader, c.pageSize);
    } catch (const std::exception& e) {
        LOG_ERROR("caps::probe() threw '%s' — continuing with default (all features off)", e.what());
        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_caps = DeviceCaps();
        g_caps.probeComplete = true;
    } catch (...) {
        LOG_ERROR("caps::probe() threw unknown exception — continuing with default (all features off)");
        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_caps = DeviceCaps();
        g_caps.probeComplete = true;
    }
}

DeviceCaps getCaps() {
    std::lock_guard<std::mutex> lk(g_capsMutex);
    return g_caps;
}

std::string resolveCgroupProcsPath(const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1) {
        return c.cpusetRoot + "/" + group + "/tasks";
    } else if (c.cgroupVer == DeviceCaps::CgroupVersion::V2) {
        return c.cpusetRoot + "/" + group + "/cgroup.procs";
    }
    return std::string();
}

std::string resolveCgroupThreadPath(const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1) {
        return c.cpusetRoot + "/" + group + "/tasks";
    } else if (c.cgroupVer == DeviceCaps::CgroupVersion::V2) {
        return c.cpusetRoot + "/" + group + "/cgroup.threads";
    }
    return std::string();
}

static ssize_t writeAllRetryEintr(int fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t off = 0;
    for (int i = 0; i < 8; ++i) {
        ssize_t w = ::write(fd, p + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            if (off == len) return (ssize_t)off;
            continue;
        }
        if (w == 0) return (ssize_t)off;
        if (errno == EINTR) continue;
        return -1;
    }
    return (ssize_t)off;
}

int writeTidToCgroupGroup(int tid, const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V2 && !c.perThreadCgroupV2) {
        return -2;
    }
    std::string path = resolveCgroupThreadPath(group);
    if (path.empty()) return -1;
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    std::string tidStr = std::to_string(tid);
    errno = 0;
    ssize_t n = writeAllRetryEintr(fd, tidStr.c_str(), tidStr.size());
    int savedErrno = errno;
    close(fd);
    if (n == (ssize_t)tidStr.size()) return 0;
    if (savedErrno == EOPNOTSUPP || savedErrno == EBUSY || savedErrno == EINVAL) {
        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_caps.perThreadCgroupV2 = false;
        return -2;
    }
    return -1;
}

std::string resolveCpuCgroupProcsPath(const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1)
        return c.cpuRoot + "/" + group + "/tasks";
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V2)
        return c.cpuRoot + "/" + group + "/cgroup.procs";
    return std::string();
}

std::string resolveCpuCgroupThreadPath(const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1)
        return c.cpuRoot + "/" + group + "/tasks";
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V2)
        return c.cpuRoot + "/" + group + "/cgroup.threads";
    return std::string();
}

int writeTidToCpuCgroupGroup(int tid, const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V2 && !c.perThreadCgroupV2) {
        return -2;
    }
    std::string path = resolveCpuCgroupThreadPath(group);
    if (path.empty()) return -1;
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    std::string tidStr = std::to_string(tid);
    errno = 0;
    ssize_t n = writeAllRetryEintr(fd, tidStr.c_str(), tidStr.size());
    int savedErrno = errno;
    close(fd);
    if (n == (ssize_t)tidStr.size()) return 0;
    if (savedErrno == EOPNOTSUPP || savedErrno == EBUSY || savedErrno == EINVAL) {
        std::lock_guard<std::mutex> lk(g_capsMutex);
        g_caps.perThreadCgroupV2 = false;
        return -2;
    }
    return -1;
}

void writeStateFiles(const char* stateDir) {
    DeviceCaps c = getCaps();
    std::string dir = stateDir;

    const bool threadsV2Real =
        (c.cgroupVer == DeviceCaps::CgroupVersion::V2 && c.perThreadCgroupV2);
    const bool uclampNodePresent = c.uclampCgroupV1 || c.uclampCgroupV2;

    std::string line = "kernel=" + c.kernelVersion +
                      ";cgroup=" + std::string(c.cgroupVer == DeviceCaps::CgroupVersion::V1 ? "v1" :
                                               c.cgroupVer == DeviceCaps::CgroupVersion::V2 ? "v2" : "unknown") +
                      ";task_uclamp=" + std::string(c.uclampTaskAvailable ? "1" : "0") +
                      ";cgroup_uclamp_v1=" + std::string(c.uclampCgroupV1 ? "1" : "0") +
                      ";cgroup_uclamp_v2=" + std::string(c.uclampCgroupV2 ? "1" : "0") +
                      ";cgroup_threads_v2=" + std::string(threadsV2Real ? "1" : "0") +
                      ";cgroup_uclamp_scale=" + std::string(uclampNodePresent ? c.cgroupUclampScale : "unknown") +
                      ";schedtune=" + std::string(c.schedtuneAvailable ? "1" : "0") +
                      ";psi=" + std::string(c.psiAvailable ? "1" : "0") +
                      ";gpu=" + std::string(c.gpuDetected ? "1" : "0") +
                      ";gpu_source=" + c.gpuSource +
                      ";loader=" + std::string(c.loader == DeviceCaps::Loader::MAGISK ? "magisk" :
                                              c.loader == DeviceCaps::Loader::KSU ? "ksu" :
                                              c.loader == DeviceCaps::Loader::APATCH ? "apatch" : "unknown") +
                      ";page_size=" + std::to_string(c.pageSize);

    atomicWriteFile((dir + "/capabilities").c_str(), line);

    atomicWriteFile((dir + "/task_uclamp").c_str(),
                    c.uclampTaskAvailable ? "1" : "0");
    atomicWriteFile((dir + "/cgroup_uclamp_v1").c_str(),
                    c.uclampCgroupV1 ? "1" : "0");
    atomicWriteFile((dir + "/cgroup_uclamp_v2").c_str(),
                    c.uclampCgroupV2 ? "1" : "0");
    atomicWriteFile((dir + "/cgroup_threads_v2").c_str(),
                    threadsV2Real ? "1" : "0");
    atomicWriteFile((dir + "/cgroup_uclamp_scale").c_str(),
                    uclampNodePresent ? c.cgroupUclampScale.c_str() : "");
}

}
}

#include <iterator>
extern const char* STATE_DIR;
namespace aatpe_ml { std::string computeDeviceTopologyHash(); }

namespace aatpe {
namespace caps {

static std::mutex g_autocalMutex;
static float g_autocalCache = -1.0f;
static std::string g_autocalFpOverride;

static constexpr float kAutocalDefaultRatio = 0.87f;

static std::string autocalCurrentFingerprint() {
    if (!g_autocalFpOverride.empty()) return g_autocalFpOverride;
    return aatpe_ml::computeDeviceTopologyHash();
}

static bool loadAutocalLocked(float& out) {
    std::string path = std::string(STATE_DIR) + "/autocal.json";
    std::ifstream f(path);
    if (!f.is_open()) { out = kAutocalDefaultRatio; return false; }
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    float ratio = -1.0f;
    std::string fp;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind("ratio=", 0) == 0)      ratio = strtof(line.c_str() + 6, nullptr);
        else if (line.rfind("fingerprint=", 0) == 0) fp = line.substr(12);
    }
    if (ratio < 0.50f || ratio > 1.0f) { out = kAutocalDefaultRatio; return false; }
    if (fp != autocalCurrentFingerprint()) { out = kAutocalDefaultRatio; return false; }
    out = ratio;
    return true;
}

float autocalBigRatio() {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    if (g_autocalCache >= 0.0f) return g_autocalCache;
    float r;
    (void)loadAutocalLocked(r);
    g_autocalCache = r;
    return g_autocalCache;
}

void autocalSetTestRatio(float r) {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    g_autocalCache = r;
}

static bool autocalScreenOff() {
    FILE* f = fopen("/sys/class/graphics/fb0/blank", "r");
    if (!f) return false;
    int blank = 0;
    if (fscanf(f, "%d", &blank) != 1) blank = 0;
    fclose(f);
    return blank != 0;
}

static bool autocalCharging() {
    FILE* f = fopen("/sys/class/power_supply/battery/status", "r");
    if (!f) return false;
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf) - 1, f)) {}
    fclose(f);
    std::string s(buf);
    return s.find("Charging") != std::string::npos ||
           s.find("AC")       != std::string::npos ||
           s.find("USB")      != std::string::npos;
}

static bool autocalIdle() {
    FILE* f = fopen("/proc/loadavg", "r");
    if (!f) return false;
    double l1 = 0.0;
    if (fscanf(f, "%lf", &l1) != 1) l1 = 99.0;
    fclose(f);
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu <= 0) ncpu = 1;
    return l1 < 0.20 * (double)ncpu;
}

static bool autocalMeasureFromSysfs(float& ratioOut) {
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu <= 0 || ncpu > 16) return false;
    std::vector<int> caps;
    for (long c = 0; c < ncpu; c++) {
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpu_capacity";
        std::ifstream f(p);
        int cap = 0;
        if (f && (f >> cap) && cap > 0) caps.push_back(cap);
    }
    if (caps.size() < 2) return false;
    std::vector<int> uniq = caps;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    if (uniq.size() < 2) return false;
    int maxCap = uniq.back();
    int bigCap = uniq[uniq.size() - 2];
    if (maxCap <= 0 || bigCap <= 0) return false;
    float r = (float)bigCap / (float)maxCap;
    if (r < 0.50f || r > 1.0f) return false;
    ratioOut = r;
    return true;
}

void runAutocalIfDue() {
    if (!autocalCharging() || !autocalScreenOff() || !autocalIdle()) return;
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    float existing = kAutocalDefaultRatio;
    if (loadAutocalLocked(existing)) {
        g_autocalCache = existing;
        return;
    }
    float ratio = kAutocalDefaultRatio;
    if (!autocalMeasureFromSysfs(ratio)) return;
    std::string body = "ratio=" + std::to_string(ratio) + "\n"
                     + "fingerprint=" + autocalCurrentFingerprint() + "\n";
    atomicWriteFile((std::string(STATE_DIR) + "/autocal.json").c_str(), body);
    g_autocalCache = ratio;
    writeFile((std::string(STATE_DIR) + "/autocal_ratio").c_str(),
              std::to_string((int)(ratio * 1000.0f)));
}

#ifdef AATPE_HOST_TEST
void autocalSetFingerprintForTest(const std::string& fp) {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    g_autocalFpOverride = fp;
    g_autocalCache = -1.0f;
}
void autocalResetForTest() {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    g_autocalCache = -1.0f;
    g_autocalFpOverride.clear();
}
#endif

}
}

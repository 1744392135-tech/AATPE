// AATPE ROUND-010: Universal device capabilities probe layer.
#include "aatpe_caps.h"
#include "aatped_logging.h"
#include "aatpe_sched_abi.h"   // sched_attr + SCHED_FLAG_* constants

// ROUND-010: forward-declare atomicWriteFile to avoid heavy header deps.
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

// ---- 1. Kernel version (uname) ----
static void probeKernel(DeviceCaps& c) {
    struct utsname u;
    if (uname(&u) == 0) {
        c.kernelVersion = u.release;
        // Parse major.minor from "4.14.186-xxx"
        sscanf(u.release, "%d.%d", &c.kernelMajor, &c.kernelMinor);
    }
}

// ---- 2. Cgroup version detection ----
static void probeCgroup(DeviceCaps& c) {
    // Method 1: check /proc/self/mountinfo for cgroup2.
    std::string mountinfo = readFileStr(rootProc() + "/self/mountinfo");
    bool hasCgroup2 = mountinfo.find("cgroup2") != std::string::npos;
    bool hasCgroup1 = mountinfo.find("cgroup") != std::string::npos && !hasCgroup2;

    // Method 2: fallback to path existence.
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

    // Check cpuset top-app group.
    std::string topAppPath = c.cpusetRoot + "/top-app";
    if (pathExists(topAppPath + "/cpus")) {
        c.cpusetTopAppExists = true;
        c.cpusetTopAppCpus = readFileStr(topAppPath + "/cpus");
        // Trim whitespace.
        c.cpusetTopAppCpus.erase(c.cpusetTopAppCpus.find_last_not_of(" \n\r\t") + 1);
    }
}

// ---- 3. UCLAMP detection ----
// ROUND-013b: REAL syscall probe for task-level uclamp.
// sched_setattr(0, &attr, 0) with util_min=0 + KEEP_PARAMS has ZERO effect
// on scheduling (we're not setting any lower bound), so it's safe to probe.
// Kernel responses:
//   rc==0       → task uclamp supported
//   errno==E2BIG → kernel doesn't know UTIL_CLAMP flag (e.g. MTK 4.14 D1100)
//   errno==EPERM → enforcing environment blocks self-setattr
//   other       → not supported, record errno
static int s_uclampProbeErrno = 0;  // recorded for diagnostics/testing

static void probeUclampTask(DeviceCaps& c) {
#ifdef __ANDROID__
    struct sched_attr attr = {};
    attr.size = sizeof(attr);
    attr.sched_policy = 0;
    attr.sched_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN;
    attr.sched_util_min = 0;  // zero = no effect, safe probe
    long rc = syscall(SYS_sched_setattr, 0, &attr, 0);
    if (rc == 0) {
        c.uclampTaskAvailable = true;
        s_uclampProbeErrno = 0;
    } else {
        c.uclampTaskAvailable = false;
        s_uclampProbeErrno = errno;
    }
#else
    // Host: weak stub, tests can override via setTaskUclampProbeResultForTest.
    c.uclampTaskAvailable = (s_uclampProbeErrno == 0);
#endif
}

static void probeUclamp(DeviceCaps& c) {
    // Task-level: real syscall probe (one-time, cached in g_caps).
    probeUclampTask(c);

    // Cgroup-level: purely node-existence check (independent from task probe).
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1) {
        if (pathExists(c.cpuRoot + "/top-app/cpu.uclamp.min")) {
            c.uclampCgroupV1 = true;
        }
    } else if (c.cgroupVer == DeviceCaps::CgroupVersion::V2) {
        if (pathExists(c.cpuRoot + "/cpu.uclamp.min")) {
            c.uclampCgroupV2 = true;
        }
    }
    // uclampAvailable = task OR cgroup present (generic on/off switch).
    c.uclampAvailable = c.uclampTaskAvailable || c.uclampCgroupV1 || c.uclampCgroupV2;
}

// RC6 (B1): PURE, READ-ONLY scale inference. Given the CURRENT value already sitting
// on the cgroup cpu.uclamp.min node, decide the scale WITHOUT ever writing it.
//   value > 101.5  -> "raw1024"  (a percent-scale kernel clamps every write to
//                                  0..100; a resting value above 101 is only possible
//                                  on a raw 0..1024 scale)
//   0..101.5      -> "percent"   (safe default; if a raw1024 node merely rests low we
//                                  under-boost instead of over-pinning the group)
//   unreadable/'' -> "unknown"   (callers treat as percent = safe direction)
// The old probe wrote "101" to a SHARED cgroup node and restored with an unchecked
// writeFile(); a failed/short restore permanently pinned the whole group's
// uclamp.min at 100%. That real-write path is deleted entirely (zero writes now).
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
    // RC6 (B1): READ-ONLY. Open the node O_RDONLY at most; never open it for write,
    // never write "101", never rely on a restore. The scale is inferred from the
    // value already present. Failures resolve to "unknown" (safe = percent).
    std::string cur = readFile(path.c_str());
    c.cgroupUclampScale = inferCgroupUclampScaleReadOnly(cur);
}

// Test seam: override probe result for host hermetic tests.
#ifdef AATPE_HOST_TEST
void setTaskUclampProbeResultForTest(bool available, int probeErrno) {
    s_uclampProbeErrno = probeErrno;
    std::lock_guard<std::mutex> lk(g_capsMutex);
    g_caps.uclampTaskAvailable = available;
    g_caps.uclampAvailable = available || g_caps.uclampCgroupV1 || g_caps.uclampCgroupV2;
}
#endif

// ---- 4. Schedtune ----
static void probeSchedtune(DeviceCaps& c) {
    if (pathExists(rootDev() + "/stune")) {
        c.schedtuneAvailable = true;
        c.schedtuneRoot = rootDev() + "/stune";
    }
}

// ---- 5. PSI ----
static void probePsi(DeviceCaps& c) {
    if (pathExists(rootProc() + "/pressure/cpu")) {
        c.psiAvailable = true;
    }
}

// ---- 6. GPU ----
// v4.4.4 (C4 hardening): PURE classifier classifyGpuPlatform() is header-only
// (aatpe_caps.h); probeGpu() only does the sysfs I/O to build the flags.
static void probeGpu(DeviceCaps& c) {
    bool devfreqGpu = false, devfreqMali = false, devfreqKgsl = false;
    // Quick devfreq scan.
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
    // Explicit KGSL node (Adreno): /sys/class/kgsl/kgsl-3d0/.
    bool kgslNode = pathExists(rootSys() + "/class/kgsl/kgsl-3d0/gpubusy") ||
                    pathExists(rootSys() + "/class/kgsl/kgsl-3d0");
    // GED hal fallback (MTK 4.14 vendor kernels).
    bool gedHal = false;
    const char* gedCandidates[] = {
        "kernel/ged/hal/gpu_freq",          // older MTK kernels
        "kernel/ged/hal/current_freqency", // vendor spelling (chopin/D1100)
        "kernel/ged/hal/gpu_utilization",
    };
    for (const char* rel : gedCandidates) {
        if (pathExists(rootSys() + "/" + rel)) { gedHal = true; break; }
    }
    // GED device node (MTK): /dev/ged.
    bool gedDev = pathExists("/dev/ged");

    const char* tag = classifyGpuPlatform(devfreqGpu, devfreqMali, devfreqKgsl,
                                         kgslNode, gedHal, gedDev);
    if (strcmp(tag, "none") != 0) {
        c.gpuDetected = true;
        c.gpuSource = tag;
    }
}

// ---- 7. Loader environment ----
static void probeLoader(DeviceCaps& c) {
    const char* env = getenv("INIT_RC");  // Magisk sets this
    if (env && strstr(env, "magisk")) {
        c.loader = DeviceCaps::Loader::MAGISK;
        return;
    }
    env = getenv("KSU");  // KernelSU
    if (env) {
        c.loader = DeviceCaps::Loader::KSU;
        return;
    }
    env = getenv("APATCH");
    if (env) {
        c.loader = DeviceCaps::Loader::APATCH;
        return;
    }
    // Check common paths.
    if (pathExists("/data/adb/magisk")) c.loader = DeviceCaps::Loader::MAGISK;
    else if (pathExists("/data/adb/ksu")) c.loader = DeviceCaps::Loader::KSU;
    else if (pathExists("/data/adb/apatch")) c.loader = DeviceCaps::Loader::APATCH;
}

// ---- 8. Page size ----
static void probePageSize(DeviceCaps& c) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) c.pageSize = (int)ps;
    c.pageSize16K = (c.pageSize >= 16384);
}

// ---- main probe ----
void probe() {
    // P0 startup-hardening: never let a probe throw out to main. On any
    // unexpected exception (bad_alloc, unexpected sysfs format, etc.) we log
    // and ship with a default-constructed DeviceCaps (all features off), so
    // the daemon starts in degraded mode instead of crashing in a loop.
    try {
        DeviceCaps c;
        probeKernel(c);
        probeCgroup(c);
        probeUclamp(c);
        probeCgroupUclampScale(c);  // ROUND-014 P3: 101 readback scale probe
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

// ROUND-013: thread-level cgroup path. v2 uses cgroup.threads (not procs,
// which is process-level and would migrate the whole thread group).
std::string resolveCgroupThreadPath(const std::string& group) {
    DeviceCaps c = getCaps();
    if (c.cgroupVer == DeviceCaps::CgroupVersion::V1) {
        return c.cpusetRoot + "/" + group + "/tasks";  // v1 tasks = thread-level
    } else if (c.cgroupVer == DeviceCaps::CgroupVersion::V2) {
        return c.cpusetRoot + "/" + group + "/cgroup.threads";
    }
    return std::string();
}

// GPUTOP-06: EINTR-retry helper for short writes to cgroup files. tid strings
// are tiny (<=7 bytes), but a signal between open() and write() used to return
// -1/EINTR and be classified as a generic IO error, causing an unnecessary
// fallback. Retry on EINTR up to a bounded number of times.
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
    return (ssize_t)off;  // short write after bounded retries
}

// Returns: 0=success, -1=IO error, -2=kernel rejected thread-level granularity.
// On -2, sets perThreadCgroupV2=false (cached conclusion, no retry).
int writeTidToCgroupGroup(int tid, const std::string& group) {
    DeviceCaps c = getCaps();
    // If we already know v2 threads are unsupported, don't retry.
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
    // EOPNOTSUPP/EBUSY/EINVAL on threaded cgroup = kernel doesn't support
    // per-thread migration on this domain.
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

    // capabilities: single-line semicolon-separated (exec safe).
    std::string line = "kernel=" + c.kernelVersion +
                      ";cgroup=" + std::string(c.cgroupVer == DeviceCaps::CgroupVersion::V1 ? "v1" :
                                               c.cgroupVer == DeviceCaps::CgroupVersion::V2 ? "v2" : "unknown") +
                      ";task_uclamp=" + std::string(c.uclampTaskAvailable ? "1" : "0") +
                      ";cgroup_uclamp_v1=" + std::string(c.uclampCgroupV1 ? "1" : "0") +
                      ";cgroup_uclamp_v2=" + std::string(c.uclampCgroupV2 ? "1" : "0") +
                      ";cgroup_threads_v2=" + std::string(c.perThreadCgroupV2 ? "1" : "0") +
                      ";cgroup_uclamp_scale=" + c.cgroupUclampScale +
                      ";schedtune=" + std::string(c.schedtuneAvailable ? "1" : "0") +
                      ";psi=" + std::string(c.psiAvailable ? "1" : "0") +
                      ";gpu=" + std::string(c.gpuDetected ? "1" : "0") +
                      ";gpu_source=" + c.gpuSource +
                      ";loader=" + std::string(c.loader == DeviceCaps::Loader::MAGISK ? "magisk" :
                                              c.loader == DeviceCaps::Loader::KSU ? "ksu" :
                                              c.loader == DeviceCaps::Loader::APATCH ? "apatch" : "unknown") +
                      ";page_size=" + std::to_string(c.pageSize);

    // ROUND-010: use project-unified atomic write (tmp + rename, 0600).
    atomicWriteFile((dir + "/capabilities").c_str(), line);
}

} // namespace caps
} // namespace aatpe

// ============================================================================
// v4.2 A3: capacity self-calibration (autocal). Lives at the caps layer because
// it is the only place that owns the cpu_capacity sysfs truth. DEFAULT-OFF:
// autocalBigRatio() returns 0.87f whenever state/autocal.json is absent / corrupt
// / topology-fingerprint mismatched — byte-identical to the v4.1.1 hardcoded
// literal. There is NO runtime switch: absence of a valid file IS the off state.
// ============================================================================
#include <iterator>
extern const char* STATE_DIR;
namespace aatpe_ml { std::string computeDeviceTopologyHash(); }

namespace aatpe {
namespace caps {

static std::mutex g_autocalMutex;
static float g_autocalCache = -1.0f;     // <0 = not loaded yet
static std::string g_autocalFpOverride;  // host-test seam

static constexpr float kAutocalDefaultRatio = 0.87f;  // v4.1.1 literal

static std::string autocalCurrentFingerprint() {
    if (!g_autocalFpOverride.empty()) return g_autocalFpOverride;
    return aatpe_ml::computeDeviceTopologyHash();
}

// Parse state/autocal.json (simple "key=value" lines, no JSON dep).
// F07[P3]: returns true and sets *out to the stored ratio ONLY when a valid,
// fingerprint-matched calibration is on disk. Returns false (and *out = the
// conservative default) for missing/corrupt/out-of-range/fingerprint-mismatch.
// Distinguishing "no calibration yet" from "calibrated to 0.87" matters: the old
// code collapsed both to 0.87 and cached it, so runAutocalIfDue()'s
// `loadAutocalLocked() > 0.869` guard treated a missing file as "already
// calibrated" and never measured on a fresh device. Caller holds g_autocalMutex.
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
    // Sanity: a capacity ratio must be a believable fraction; else treat as corrupt.
    if (ratio < 0.50f || ratio > 1.0f) { out = kAutocalDefaultRatio; return false; }
    // OTA / topology-change binding: stored fingerprint must match the current one.
    if (fp != autocalCurrentFingerprint()) { out = kAutocalDefaultRatio; return false; }
    out = ratio;
    return true;
}

float autocalBigRatio() {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    if (g_autocalCache >= 0.0f) return g_autocalCache;
    float r;
    (void)loadAutocalLocked(r);  // on failure r == default; callers see the safe value
    g_autocalCache = r;
    return g_autocalCache;
}

// v6.0.0 round4c test seam: force autocalBigRatio() to return a fixed ratio so
// host tests don't depend on the dev VM's real cpu_capacity probe (which yields
// 0.6953 on x86 cloud vs 0.87 on real MTK). r<0 resets to "not injected".
void autocalSetTestRatio(float r) {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    g_autocalCache = r;
}

// ---- measurement helpers (device only; defensive no-ops on failure) -------
static bool autocalScreenOff() {
    FILE* f = fopen("/sys/class/graphics/fb0/blank", "r");
    if (!f) return false;
    int blank = 0;
    if (fscanf(f, "%d", &blank) != 1) blank = 0;
    fclose(f);
    return blank != 0;  // 1 = screen off
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
    // idle: 1-min load < 20% of total capacity
    return l1 < 0.20 * (double)ncpu;
}

// Read /sys/.../cpuN/cpu_capacity for all cpus; compute the BIG-cluster capacity
// ratio = (highest sub-max cluster capacity) / (max cluster capacity). Returns
// false (and leaves ratio untouched) when there is no LITTLE/BIG contrast
// (all-big SoC) or sysfs is missing — caller keeps the 0.87f default.
// GPUTOP-12: this duplicates /sys/.../cpuN/cpu_capacity reads that the topology
// layer also does. We intentionally do NOT reuse topology's internal cache
// (g_cpuMetrics is file-static in aatped_topology.cpp, not exported) because:
//   (a) autocal runs rarely (charging + screen-off + idle, one-shot per boot);
//   (b) coupling to topology internals would create a load-order dependency;
//   (c) sysfs reads here are fast (<1ms total for <=16 cpus).
// Decision: keep as-is (DEGRADED — redundant read, not a correctness bug).
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
    if (uniq.size() < 2) return false;              // single tier -> no LITTLE control
    int maxCap = uniq.back();
    int bigCap = uniq[uniq.size() - 2];            // cluster just below PRIME
    // Require a real contrast and a sane ratio in [0.5, 1.0]; else bail to default.
    if (maxCap <= 0 || bigCap <= 0) return false;
    float r = (float)bigCap / (float)maxCap;
    if (r < 0.50f || r > 1.0f) return false;
    ratioOut = r;
    return true;
}

void runAutocalIfDue() {
    // Safe window: charging + screen off + idle. Any failure -> silent no-op.
    if (!autocalCharging() || !autocalScreenOff() || !autocalIdle()) return;
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    // F07[P3]: only skip when a VALID on-disk calibration is present. A missing/
    // corrupt first read leaves g_autocalCache at the conservative default; that
    // must NOT count as "already calibrated" (the old `>0.869` guard treated a
    // missing file as calibrated and never measured on a fresh device).
    float existing = kAutocalDefaultRatio;
    if (loadAutocalLocked(existing)) {
        g_autocalCache = existing;
        return;
    }
    // No valid calibration yet: measure from sysfs and persist one.
    float ratio = kAutocalDefaultRatio;
    if (!autocalMeasureFromSysfs(ratio)) return;   // no usable truth -> keep default
    // Persist: simple key=value + topology fingerprint (OTA invalidation).
    std::string body = "ratio=" + std::to_string(ratio) + "\n"
                     + "fingerprint=" + autocalCurrentFingerprint() + "\n";
    atomicWriteFile((std::string(STATE_DIR) + "/autocal.json").c_str(), body);
    g_autocalCache = ratio;
    // State key (x1000) for Batch C doctor/WebUI.
    writeFile((std::string(STATE_DIR) + "/autocal_ratio").c_str(),
              std::to_string((int)(ratio * 1000.0f)));
}

#ifdef AATPE_HOST_TEST
void autocalSetFingerprintForTest(const std::string& fp) {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    g_autocalFpOverride = fp;
    g_autocalCache = -1.0f;  // force re-read with the new fingerprint
}
void autocalResetForTest() {
    std::lock_guard<std::mutex> lk(g_autocalMutex);
    g_autocalCache = -1.0f;
    g_autocalFpOverride.clear();
}
#endif

} // namespace caps
} // namespace aatpe

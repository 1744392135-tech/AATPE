#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <cerrno>
#include <climits>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <spawn.h>
#include <sched.h>
#include <dirent.h>
#include <map>
#include <set>
#include "cpu_zone_selector.h"
#include "aatpe-constants.h"
#include "aatpe-version.h"
#include "aatpe_pid_identity.h"

using namespace aatpe;
extern char** environ;

static const char* LOG_FILE     = "/dev/aatpe/aatpe-watchdog.log";
static const char* WD_PID_FILE  = "/dev/aatpe/watchdog.pid";
static const char* WD_LOCK_FILE = "/dev/aatpe/watchdog.lock";
static const char* DAEMON_PATH  = "/system/bin/aatped";
static const char* STOP_REQUEST_FILE = "/dev/aatpe/state/stop_request";

static std::atomic<sig_atomic_t> g_shouldExit{0};
static void signalHandler(int sig) {
    if (sig == SIGTERM || sig == SIGINT)
        g_shouldExit.store(1, std::memory_order_relaxed);
}

static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static int readInt(const char* path, int def) {
    std::string s = readFile(path);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

static bool readHardCrashSignal(std::string& sigOut) {
    std::string v = readFile((std::string(aatpe::paths::kStateDir) + "/crash_signal").c_str());
    sigOut = v;
    return v == "SIGABRT" || v == "SIGSEGV" || v == "SIGBUS" ||
           v == "SIGILL" || v == "SIGFPE";
}
static void clearCrashSignal() {
    ::unlink((std::string(aatpe::paths::kStateDir) + "/crash_signal").c_str());
}
static constexpr int kStaleStrikeBeforeKill = 3;

static time_t bootTimeSec() {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
        return time(nullptr);
    }
    return (time_t)ts.tv_sec;
}

static bool writeFile(const char* path, const std::string& content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t total = content.size();
    size_t off = 0;
    while (off < total) {
        ssize_t n = write(fd, content.c_str() + off, total - off);
        if (n <= 0) { close(fd); return false; }
        off += (size_t)n;
    }
    close(fd);
    return true;
}

static void trim(std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
}

static FILE* g_wdLogFp = nullptr;
static void wdLogInit() {
    g_wdLogFp = fopen(LOG_FILE, "a");
    if (!g_wdLogFp) {
        fprintf(stderr, "watchdog: failed to open log file %s: %s\n", LOG_FILE, strerror(errno));
    }
}

static void rotateLogIfNeeded(FILE** ppFp) {
    if (!ppFp || !*ppFp) return;
    struct stat st;
    if (fstat(fileno(*ppFp), &st) != 0) return;
    if (st.st_size <= aatpe::logging::kMaxLogSizeBytes) return;
    fclose(*ppFp);
    *ppFp = fopen(LOG_FILE, "w");
    if (!*ppFp) return;
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(*ppFp, "[%s] [WATCHDOG] log rotated (size limit %lld bytes)\n",
            ts, (long long)aatpe::logging::kMaxLogSizeBytes);
    fflush(*ppFp);
}

static void logWatchdog(const char* fmt, ...) {
    bool tempOpened = false;
    FILE* fp = g_wdLogFp;
    if (!fp) {
        fp = fopen(LOG_FILE, "a");
        if (!fp) return;
        tempOpened = true;
    }
    if (!tempOpened) rotateLogIfNeeded(&g_wdLogFp);
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    fprintf(fp, "[%s] [WATCHDOG] ", ts);
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fflush(fp);
    if (tempOpened) fclose(fp);
}

static void writeState(const char* key, const char* val) {
    std::string path = std::string(aatpe::paths::kStateDir) + "/" + key;
    writeFile(path.c_str(), val ? val : "");
}

static void sendControlCommand(const std::string& cmd) {
    std::string path = std::string(aatpe::paths::kRunDir) + "/control";
    std::ofstream f(path, std::ios::app);
    if (f) {
        f << cmd << "\n";
    } else {
        logWatchdog("WARN: cannot write control pipe %s (daemon down?)", path.c_str());
    }
}

enum class WdRuntimeState {
    NORMAL, DEGRADED, PASSIVE, EMERGENCY, RECOVERY
};
static WdRuntimeState g_wdState = WdRuntimeState::NORMAL;
static time_t g_recoverySince = 0;

static std::set<std::string> g_degradedSet;
static std::string degradedReasonsString() {
    std::string v;
    for (const auto& r : g_degradedSet) {
        if (!v.empty()) v += "+";
        v += r;
    }
    return v.empty() ? "none" : v;
}
static void publishDegraded() {
    writeState("watchdog_degraded", degradedReasonsString().c_str());
}
static void addDegradedReason(const std::string& r) {
    if (g_degradedSet.insert(r).second) publishDegraded();
}
static void removeDegradedReason(const std::string& r) {
    if (g_degradedSet.erase(r) > 0) publishDegraded();
}
static void clearAllDegradedReasons() {
    if (!g_degradedSet.empty()) { g_degradedSet.clear(); publishDegraded(); }
}

static const char* wdStateName(WdRuntimeState s) {
    switch (s) {
        case WdRuntimeState::NORMAL:    return "NORMAL";
        case WdRuntimeState::DEGRADED:  return "DEGRADED";
        case WdRuntimeState::PASSIVE:   return "PASSIVE";
        case WdRuntimeState::EMERGENCY: return "EMERGENCY";
        case WdRuntimeState::RECOVERY: return "RECOVERY";
    }
    return "UNKNOWN";
}

static const char* wdControlModeFor(WdRuntimeState s) {
    switch (s) {
        case WdRuntimeState::NORMAL:    return "active";
        case WdRuntimeState::DEGRADED:  return "assist";
        case WdRuntimeState::RECOVERY: return "assist";
        case WdRuntimeState::PASSIVE:   return "passive";
        case WdRuntimeState::EMERGENCY: return "emergency";
    }
    return "assist";
}

static std::string sanitizeReason(const std::string& reason) {
    std::string out;
    out.reserve(reason.size());
    for (char c : reason) {
        if (c >= 'A' && c <= 'Z') out.push_back((char)(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        else out.push_back('_');
    }
    if (out.empty()) out = "unspecified";
    return out;
}

static void wdTransition(WdRuntimeState target, const std::string& reason) {
    WdRuntimeState prev = g_wdState;
    if (prev == target) return;
    const char* mode = wdControlModeFor(target);
    std::string safeReason = sanitizeReason(reason);
    logWatchdog("WSTATE: %s -> %s (control_mode=%s, reason=%s)",
                wdStateName(prev), wdStateName(target), mode, safeReason.c_str());
    writeState("watchdog_runtime_state", wdStateName(target));
    writeState("watchdog_state_reason", safeReason.c_str());
    sendControlCommand("set_control_mode=" + std::string(mode) + "&reason=" + safeReason);
    g_wdState = target;
    if (target == WdRuntimeState::RECOVERY) {
        g_recoverySince = time(nullptr);
    } else {
        g_recoverySince = 0;
    }
}

struct WdEvidence {
    bool daemonAlive;
    bool thermalEmergency;
    bool migOver;
    bool loadOver;
    bool explicitStop;
};

static WdRuntimeState wdSteadyTransition(WdRuntimeState cur, const WdEvidence& ev,
                                         long long now,
                                         long long emergClearSince,
                                         long long recoverySince,
                                         int recoveryStableSec,
                                         int emergencyClearSec) {
    if (ev.explicitStop) return cur;

    if (ev.thermalEmergency) return WdRuntimeState::EMERGENCY;
    if (ev.migOver || ev.loadOver) return WdRuntimeState::PASSIVE;

    switch (cur) {
        case WdRuntimeState::EMERGENCY:
            if (emergClearSince > 0 && (now - emergClearSince) >= emergencyClearSec)
                return WdRuntimeState::RECOVERY;
            return WdRuntimeState::EMERGENCY;
        case WdRuntimeState::RECOVERY:
            if (recoverySince > 0 && (now - recoverySince) >= recoveryStableSec)
                return WdRuntimeState::NORMAL;
            return WdRuntimeState::RECOVERY;
        default:
            return cur;
    }
}

static bool explicitStopRequested() {
    struct stat st;
    return ::stat(STOP_REQUEST_FILE, &st) == 0;
}

enum class WdTempRead { HOT, SAFE, UNKNOWN };

static int wdComputeEmergencyWallC(int thresholdCfg, int emergencyCfg, int overrideCfg) {
    int wall = thresholdCfg > 0 ? thresholdCfg : emergencyCfg;
    if (wall <= 0) wall = 120;
    if (wall > 120) wall = 120;
    if (overrideCfg > 0 && overrideCfg > wall) wall = overrideCfg;
    return wall;
}

static WdTempRead wdClassifyIndTemp(long long maxC, int wallC, int marginC) {
    if (maxC < 0) return WdTempRead::UNKNOWN;
    if (maxC >= wallC) return WdTempRead::HOT;
    if (maxC <= wallC - marginC) return WdTempRead::SAFE;
    return WdTempRead::UNKNOWN;
}

static bool wdIndTempSample(WdTempRead r, int& safeStreak, int needSamples) {
    if (r == WdTempRead::SAFE) {
        if (++safeStreak >= needSamples) return true;
        return false;
    }
    safeStreak = 0;
    return false;
}

static long long wdReadMaxTempCelsiusUnder(const char* root) {
    DIR* base = opendir(root);
    if (!base) return -1;

    std::vector<thermzone::ZoneReading> tz;
    struct dirent* de;
    while ((de = readdir(base)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string basePath = std::string(root) + "/" + name;

        std::string typeLower;
        {
            FILE* tf = fopen((basePath + "/type").c_str(), "r");
            if (!tf) continue;
            char buf[128] = {0};
            if (!fgets(buf, sizeof(buf), tf)) { fclose(tf); continue; }
            fclose(tf);
            typeLower = buf;
            while (!typeLower.empty() &&
                   (typeLower.back() == '\n' || typeLower.back() == '\r' ||
                    typeLower.back() == ' '  || typeLower.back() == '\t'))
                typeLower.pop_back();
            for (auto& c : typeLower) c = (char)std::tolower((unsigned char)c);
        }

        long long raw = 0;
        bool haveRaw = false;
        {
            FILE* f = fopen((basePath + "/temp").c_str(), "r");
            if (f) {
                if (fscanf(f, "%lld", &raw) == 1) haveRaw = true;
                fclose(f);
            }
        }

        thermzone::ZoneReading r;
        r.name = name;
        r.typeLower = typeLower;
        if (haveRaw && raw > -50 && raw < 200000) {
            int c;
            if (raw > 1500)      c = (int)(raw / 1000);
            else if (raw > 150)  c = (int)(raw / 10);
            else                 c = (int)raw;
            r.valid = (c > 0 && c < 150);
            r.tempC = r.valid ? c : INT_MIN;
        } else {
            r.valid = false;
            r.tempC = INT_MIN;
        }
        tz.push_back(r);
    }
    closedir(base);

    static std::map<std::string, thermzone::ZoneHist> s_wdTzHist;
    thermzone::SelectResult sel = thermzone::select(tz, s_wdTzHist);
    if (sel.trustedCpuTempC > 0) return (long long)sel.trustedCpuTempC;
    return -1;
}

static long long wdReadMaxTempCelsius() {
    return wdReadMaxTempCelsiusUnder("/sys/class/thermal");
}

static int readConfigInt(const char* key, int def) {
    static const char* confPaths[] = {
        "/system/etc/aatpe/aatpe.conf",
        "/data/adb/modules/aatpe/aatpe.conf",
        "/data/adb/ksu/modules/aatpe/aatpe.conf",
        "/data/adb/ap/modules/aatpe/aatpe.conf",
        "/data/adb/modules_update/aatpe/aatpe.conf",
        "/data/aatpe/aatpe.conf",
        "/dev/aatpe/aatpe.conf",
        nullptr
    };
    for (int i = 0; confPaths[i]; i++) {
        std::ifstream f(confPaths[i]);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t hashPos = line.find('#');
            if (hashPos != std::string::npos) line = line.substr(0, hashPos);
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            trim(k);
            trim(v);
            if (k == key) {
                try { return std::stoi(v); } catch (...) { return def; }
            }
        }
    }
    return def;
}

static int countCpuDirs(const char* sysBase) {
    std::string base = std::string(sysBase) + "/devices/system/cpu/";
    DIR* d = ::opendir(base.c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        const char* p = de->d_name;
        if (p[0] != 'c' || p[1] != 'p' || p[2] != 'u') continue;
        if (p[3] < '0' || p[3] > '9') continue;
        n++;
    }
    ::closedir(d);
    return n;
}

static int getCpuCount() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
    int sysN = countCpuDirs("/sys");
    if (sysN > 0) return sysN;
    logWatchdog("WARN: getCpuCount sysconf and /sys enumeration both failed; "
                "using conservative fallback=8 (may under/over-count on >8 cores)");
    return 8;
}

static std::string g_topologySource = "unknown";

static int g_unkillableStreak = 0;
static int g_staleStreak = 0;
static pid_t g_unkillablePid = -1;
static time_t g_unkillableSinceH = 0;

static int g_thermalStaleStreak = 0;
static bool thermalStaleTriggered(int streak) { return streak >= 5; }

static bool unkillableStreakTriggered(int streak) { return streak >= 3; }

static std::vector<int> selectBigPoolFromCapacities(std::vector<std::pair<int,int>> cpuCapacities) {
    std::vector<int> pool;
    if (cpuCapacities.empty()) return pool;
    std::sort(cpuCapacities.begin(), cpuCapacities.end(),
              [](const std::pair<int,int>& a, const std::pair<int,int>& b) { return a.first > b.first; });
    int maxCap = cpuCapacities.front().first;

    size_t topCount = 0;
    for (const auto& cc : cpuCapacities) if (cc.first == maxCap) topCount++;
    int secondCap = 0;
    for (const auto& cc : cpuCapacities) { if (cc.first < maxCap) { secondCap = cc.first; break; } }

    bool hasPrimeTop = false;
    if (secondCap > 0 && topCount <= 2 && maxCap * 100 / secondCap >= 120) {
        hasPrimeTop = true;
    }

    if (hasPrimeTop) {
        int bigFloor = secondCap * topology::kMidMaxRatio / 100;
        for (const auto& cc : cpuCapacities) {
            if (cc.first == maxCap) continue;
            if (cc.first >= bigFloor) pool.push_back(cc.second);
        }
        if (!pool.empty()) return pool;
        for (const auto& cc : cpuCapacities) if (cc.first < maxCap) pool.push_back(cc.second);
        if (!pool.empty()) return pool;
    }

    for (const auto& cc : cpuCapacities) {
        int ratio = cc.first * 100 / maxCap;
        if (ratio > topology::kMidMaxRatio) pool.push_back(cc.second);
    }
    if (!pool.empty()) return pool;
    pool.push_back(cpuCapacities.front().second);
    return pool;
}

static std::vector<int> detectBigCorePool() {
    {
        std::string sp = std::string(aatpe::paths::kStateDir) + "/big_pool";
        std::string s = readFile(sp.c_str());
        if (!s.empty()) {
            std::vector<int> pool;
            std::stringstream ss(s);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try { pool.push_back(std::stoi(tok)); } catch (const std::exception&) {  }
            }
            if (!pool.empty()) {
                g_topologySource = "daemon_state_file";
                return pool;
            }
        }
    }

    g_topologySource = "local_fallback";
    std::vector<std::pair<int, int>> cpuCapacities;
    int maxCap = 0;
    std::vector<int> cpusToProbe;
    {
        std::string oc = readFile((std::string(aatpe::paths::kStateDir) + "/online_cpus").c_str());
        if (!oc.empty()) {
            std::stringstream ss(oc);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                try { cpusToProbe.push_back(std::stoi(tok)); } catch (const std::exception&) {  }
            }
        }
    }
    if (cpusToProbe.empty()) {
        DIR* sysCpuDir = opendir("/sys/devices/system/cpu");
        if (sysCpuDir) {
            struct dirent* de;
            while ((de = readdir(sysCpuDir)) != nullptr) {
                if (strncmp(de->d_name, "cpu", 3) != 0) continue;
                char* end = nullptr;
                errno = 0;
                long id = strtol(de->d_name + 3, &end, 10);
                if (*end != '\0' || errno == ERANGE || id < 0 || id > 1023) continue;
                cpusToProbe.push_back((int)id);
            }
            closedir(sysCpuDir);
        }
    }
    for (int cpu : cpusToProbe) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity";
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        std::ifstream f(path);
        if (!f.good()) continue;
        int cap = 0;
        f >> cap;
        if (cap > 0) {
            cpuCapacities.push_back({cap, cpu});
            if (cap > maxCap) maxCap = cap;
        }
    }
    if (!cpuCapacities.empty() && maxCap > 0) {
        return selectBigPoolFromCapacities(cpuCapacities);
    }
    int total = getCpuCount();
    std::vector<int> pool;
    if (total >= 7) {
        pool.push_back(total - 1);
    } else {
        for (int i = total / 2; i < total; i++) pool.push_back(i);
    }
    return pool;
}

static bool isCriticalThreadComm(const std::string& comm) {
    static const char* kCriticalComms[] = {
        "surfaceflinger", "RenderThread", "hwuiTask0", "hwuiTask1", "hwuiTask2",
        "UnityMain", "UnityGfxDevice", "mali-utility", "mali-csf", "kgsl_worker",
        ".game",
        nullptr
    };
    for (int i = 0; kCriticalComms[i]; i++) {
        if (comm.find(kCriticalComms[i]) != std::string::npos) return true;
    }
    return false;
}

static void watchdogEmergencyMigrate() {
    std::vector<int> bigPool = detectBigCorePool();
    writeState("watchdog_topology_source", g_topologySource.c_str());
    if (bigPool.empty()) return;

    cpu_set_t bigMask;
    CPU_ZERO(&bigMask);
    for (int cpu : bigPool) {
        if (cpu < CPU_SETSIZE) CPU_SET(cpu, &bigMask);
    }

    DIR* procDir = opendir("/proc");
    if (!procDir) return;
    struct dirent* entry;
    int migratedThreads = 0;
    while ((entry = readdir(procDir)) != nullptr) {
        int pid = parseIntSafe(entry->d_name);
        if (pid <= 0) continue;

        std::string taskDirPath = "/proc/" + std::to_string(pid) + "/task";
        DIR* taskDir = opendir(taskDirPath.c_str());
        if (!taskDir) continue;
        struct dirent* taskEntry;
        while ((taskEntry = readdir(taskDir)) != nullptr) {
            int tid = parseIntSafe(taskEntry->d_name);
            if (tid <= 0) continue;
            std::string commPath = taskDirPath + "/" + std::to_string(tid) + "/comm";
            std::ifstream cf(commPath);
            if (!cf.good()) continue;
            std::string comm;
            std::getline(cf, comm);
            cf.close();
            if (!isCriticalThreadComm(comm)) continue;
            if (sched_setaffinity(tid, sizeof(bigMask), &bigMask) == 0) {
                migratedThreads++;
            }
        }
        closedir(taskDir);
    }
    closedir(procDir);
    if (migratedThreads > 0) {
        logWatchdog("Emergency migration: pinned %d critical threads to big-core pool",
                    migratedThreads);
    }
}

static bool isAatpedIdentity(int pid) {
    return aatpe::pid_identity::verifyAatpedPid((pid_t)pid);
}

static bool killAndWaitDaemon(int dpid) {
    if (dpid <= 0) return true;
    if (!isAatpedIdentity(dpid)) {
        logWatchdog("refusing to SIGKILL pid=%d (identity check failed; stale/reused PID or wrong exe)",
                    dpid);
        return true;
    }
    kill(dpid, SIGKILL);
    bool dead = false;
    for (int i = 0; i < 30; i++) {
        if (kill(dpid, 0) != 0) { dead = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!dead) {
        logWatchdog("CRITICAL: SIGKILL pid=%d still alive after 3s (D-state?). "
                    "NOT spawning new daemon to avoid double-instance.", dpid);
        return false;
    }
    int status;
    pid_t reaped = waitpid(dpid, &status, WNOHANG);
    if (reaped == 0) {
        for (int i = 0; i < 10; i++) {
            reaped = waitpid(dpid, &status, WNOHANG);
            if (reaped != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    while (waitpid(-1, &status, WNOHANG) > 0) {
    }
    return true;
}

static void interruptibleSleepSec(long long secs) {
    long long waitedMs = 0;
    const long long totalMs = secs * 1000;
    while (waitedMs < totalMs && !g_shouldExit.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        waitedMs += 200;
    }
}

static bool restartDaemon() {
    {
        int existing = readInt(aatpe::paths::kPidFile, 0);
        if (existing > 0 && isAatpedIdentity(existing)) {
            logWatchdog("supervised daemon already healthy (PID=%d) — adopting, NOT spawning duplicate",
                        existing);
            return true;
        }
    }
    umask(022);
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        logWatchdog("posix_spawn_file_actions_init failed: %s", strerror(errno));
        return false;
    }

    int logFd = open(aatpe::paths::kLogFile, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (logFd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, logFd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, logFd, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, logFd);
    }

    pid_t child = 0;
    char* spawnArgv[] = {(char*)"aatped", (char*)"--daemon", nullptr};
    int spawnRet = posix_spawn(&child, DAEMON_PATH, &fa, nullptr, spawnArgv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (logFd >= 0) close(logFd);

    if (spawnRet != 0 || child <= 0) {
        logWatchdog("posix_spawn failed: ret=%d errno=%d (%s)",
                    spawnRet, errno, strerror(errno));
        return false;
    }

    {
        int st;
        for (int i = 0; i < 30; i++) {
            pid_t w = waitpid(child, &st, WNOHANG);
            if (w == child || w < 0) break;
            if (g_shouldExit.load(std::memory_order_relaxed)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    interruptibleSleepSec(watchdog::kRestartWaitSec);
    int newPid = readInt(aatpe::paths::kPidFile, 0);
    if (newPid > 0 && kill(newPid, 0) == 0) {
        logWatchdog("Daemon restarted successfully (PID=%d)", newPid);
        return true;
    }
    logWatchdog("Daemon restart failed: new PID not found or not alive (newPid=%d)", newPid);
    return false;
}

static int checkBinaryUsable() {
    struct stat st;
    if (stat(DAEMON_PATH, &st) != 0) return 1;
    if (!(st.st_mode & S_IXUSR)) return 2;
    return 0;
}

static const char* classifyDeath(time_t daemonStartedAt, bool wasHeartbeatStale,
                                 bool wasMemoryPressure, time_t now) {
    if (checkBinaryUsable() != 0) return "binary_broken";
    if (wasHeartbeatStale) return "stuck";
    if (wasMemoryPressure) return "oom";
    long long aliveSec = (long long)(now - daemonStartedAt);
    if (daemonStartedAt > 0 && aliveSec < watchdog::kRapidDeathThresholdSec)
        return "rapid_crash";
    return "clean_exit";
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v" || a == "version") {
            printf("%s\n", AATPE_VERSION);
            return 0;
        }
    }

    if (::getuid() != 0) {
        fprintf(stderr, "aatpe-watchdog: must run as root\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    if (mkdir("/dev/aatpe", 0700) != 0 && errno != EEXIST)
        logWatchdog("mkdir /dev/aatpe failed: %s", strerror(errno));
    if (mkdir(aatpe::paths::kStateDir, 0700) != 0 && errno != EEXIST)
        logWatchdog("mkdir %s failed: %s", aatpe::paths::kStateDir, strerror(errno));
    wdLogInit();

    int wdLockFd = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        wdLockFd = open(WD_LOCK_FILE, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        if (wdLockFd >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (wdLockFd < 0) {
        logWatchdog("FATAL: cannot open lock file %s: %s — exiting to avoid double-instance",
                    WD_LOCK_FILE, strerror(errno));
        return 1;
    }
    if (lockf(wdLockFd, F_TLOCK, 0) < 0) {
        logWatchdog("Another watchdog instance is already running, exiting");
        close(wdLockFd);
        return 1;
    }

    {
        std::string pidStr = std::to_string(getpid());
        std::string tmpPid = std::string(WD_PID_FILE) + ".tmp";
        int tfd = open(tmpPid.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC, 0600);
        if (tfd >= 0) {
            ssize_t w = write(tfd, pidStr.c_str(), pidStr.size());
            bool wroteAll = (w == (ssize_t)pidStr.size());
            close(tfd);
            if (wroteAll) {
                if (rename(tmpPid.c_str(), WD_PID_FILE) != 0) {
                    logWatchdog("WARN: atomic rename %s -> %s failed: %s; "
                                "leaving tmp (will be retried next start)",
                                tmpPid.c_str(), WD_PID_FILE, strerror(errno));
                }
            } else {
                logWatchdog("WARN: short write on pid tmp (%zd/%zu); unlinking",
                            w, pidStr.size());
                unlink(tmpPid.c_str());
            }
        }
    }
    logWatchdog("AATPE Watchdog %s starting (PID=%d)", AATPE_VERSION, getpid());

    writeState("crash_loop_detected", "0");

    writeState("watchdog_runtime_state", wdStateName(g_wdState));
    writeState("watchdog_state_reason", "watchdog_startup");
    clearAllDegradedReasons();
    publishDegraded();

    int INTERVAL = readConfigInt("WATCHDOG_INTERVAL", watchdog::kDefaultIntervalSec);
    if (INTERVAL < watchdog::kMinIntervalSec) INTERVAL = watchdog::kDefaultIntervalSec;
    if (INTERVAL > 60) {
        logWatchdog("WATCHDOG_INTERVAL=%d out of range, using default %ds",
                    INTERVAL, watchdog::kDefaultIntervalSec);
        INTERVAL = watchdog::kDefaultIntervalSec;
    }
    int MAX_RESTARTS = readConfigInt("WATCHDOG_MAX_RESTARTS", watchdog::kDefaultMaxRestarts);
    int RESTART_WINDOW = readConfigInt("WATCHDOG_RESTART_WINDOW", watchdog::kDefaultRestartWindow);
    int HEARTBEAT_TIMEOUT = readConfigInt("WATCHDOG_HEARTBEAT_TIMEOUT",
                                         watchdog::kHeartbeatTimeoutSec);
    if (HEARTBEAT_TIMEOUT < 10 || HEARTBEAT_TIMEOUT > 600) {
        logWatchdog("WATCHDOG_HEARTBEAT_TIMEOUT=%d out of range, using default %ds",
                    HEARTBEAT_TIMEOUT, watchdog::kHeartbeatTimeoutSec);
        HEARTBEAT_TIMEOUT = watchdog::kHeartbeatTimeoutSec;
    }

    int restartCount = 0;
    time_t lastRestart = 0;
    int hardCrashCount = 0;
    time_t lastHardCrash = 0;
    bool passiveMode = false;
    int crashLoopCycles = 0;
    time_t passiveSince = 0;
    int consecutiveMemoryWarns = 0;
    int consecutiveMigWarns    = 0;
    int consecutiveLoadWarns  = 0;
    time_t tempPassiveUntil = 0;
    time_t daemonStartedAt = 0;
    int rapidCrashCount = 0;
    bool degradedMode = false;
    time_t degradedSince = 0;
    time_t degradedHealthySince = 0;
    bool lastMemWasOver = false;
    bool stopLogged = false;
    time_t emergClearSince = 0;
    const int emergencyClearSec = thermal_runtime::kMinRecoverySec;
    int indThermalWallC = wdComputeEmergencyWallC(
        readConfigInt("THERMAL_EMERGENCY_THRESHOLD", 0),
        readConfigInt("THERMAL_EMERGENCY", 0),
        readConfigInt("WATCHDOG_INDEP_THERMAL_WALL", 0));
    const int indThermalMarginC = thermal::kEmergencyHysteresis;
    int emergIndSafeStreak = 0;
    const int emergIndNeedSamples =
        (emergencyClearSec + INTERVAL - 1) / INTERVAL < 1
            ? 1
            : (emergencyClearSec + INTERVAL - 1) / INTERVAL;

    logWatchdog("Config: interval=%ds max_restarts=%d window=%ds hb_timeout=%ds cooldown=%ds",
                INTERVAL, MAX_RESTARTS, RESTART_WINDOW,
                HEARTBEAT_TIMEOUT, watchdog::kPassiveCooldownSec);

    {
        std::vector<int> warm = detectBigCorePool();
        (void)warm;
        writeState("watchdog_topology_source", g_topologySource.c_str());
        if (g_topologySource == "local_fallback") {
            logWatchdog("WARN: watchdog using LOCAL topology fallback (daemon state file "
                        "'big_pool' unavailable at start). Pools inferred locally and mirrored "
                        "to daemon rules; on-device validation recommended. "
                        "Published state: watchdog_topology_source=local_fallback");
        } else {
            logWatchdog("Topology source: %s (big_pool consumed from daemon state file)",
                        g_topologySource.c_str());
        }
    }

    while (!g_shouldExit.load(std::memory_order_relaxed)) {
        interruptibleSleepSec(INTERVAL);

        {
            int st;
            int supervisedPid = readInt(aatpe::paths::kPidFile, 0);
            int w;
            while ((w = waitpid(-1, &st, WNOHANG)) > 0) {
                if (w != supervisedPid) {
                    logWatchdog("ignoring exit of non-supervised child pid=%d (launcher/duplicate)", w);
                }
            }
        }

        if (explicitStopRequested()) {
            if (!stopLogged) {
                logWatchdog("Explicit stop requested (%s present): not restarting daemon until cleared",
                            STOP_REQUEST_FILE);
                stopLogged = true;
            }
            continue;
        } else {
            stopLogged = false;
        }

        bool inBootGrace = watchdog::inBootGrace(bootTimeSec(), watchdog::kBootGraceSec);
        int hbTimeoutLimit = watchdog::effectiveHeartbeatTimeout(HEARTBEAT_TIMEOUT, inBootGrace);

        int dpid = readInt(aatpe::paths::kPidFile, 0);
        bool daemonAlive = (dpid > 0 && isAatpedIdentity(dpid));

        bool heartbeatStale = false;
        int hb = 0;
        if (daemonAlive) {
            hb = readInt((std::string(aatpe::paths::kStateDir) + "/heartbeat").c_str(), 0);
            if (hb > 0) {
                time_t nowHb = bootTimeSec();
                time_t age = nowHb - (time_t)hb;
                if (age > hbTimeoutLimit) {
                    g_staleStreak++;
                    if (g_staleStreak < kStaleStrikeBeforeKill) {
                        logWatchdog("heartbeat stale (%lds old, limit=%ds) streak=%d/%d — deferring SIGKILL",
                                    (long)age, hbTimeoutLimit, g_staleStreak, kStaleStrikeBeforeKill);
                        heartbeatStale = false;
                    } else {
                        heartbeatStale = true;
                        logWatchdog("DAEMON STUCK: alive (PID=%d) but heartbeat stale (%lds old, limit=%ds%s, streak=%d). Killing for restart.",
                                    dpid, (long)age, hbTimeoutLimit,
                                    inBootGrace ? " [boot-grace widened]" : "", g_staleStreak);
                    bool killed = killAndWaitDaemon(dpid);
                    if (!killed) {
                        logWatchdog("Old daemon pid=%d survived SIGKILL, skipping restart this cycle",
                                    dpid);
                        if (g_unkillablePid != dpid) {
                            g_unkillablePid = dpid;
                            g_unkillableStreak = 0;
                            g_unkillableSinceH = bootTimeSec();
                        }
                        g_unkillableStreak++;
                        long waited = (long)(bootTimeSec() - g_unkillableSinceH);
                        if (unkillableStreakTriggered(g_unkillableStreak)) {
                            writeState("watchdog_unkillable", "1");
                            pid_t dpgid = getpgid(dpid);
                            pid_t mypgid = getpgid(0);
                            if (dpgid > 0 && dpgid != mypgid && dpgid != getpid()) {
                                logWatchdog("UnkillableDstate: cycle %d, group SIGKILL pgid=%d (pid=%d, waited=%lds, hb age=%lds)",
                                            g_unkillableStreak, (int)dpgid, dpid, waited, (long)age);
                                kill(-dpgid, SIGKILL);
                            } else {
                                logWatchdog("UnkillableDstate: cycle %d, pid=%d waited=%lds (group-skip: pgid=%d mine=%d)",
                                            g_unkillableStreak, dpid, waited, (int)dpgid, (int)mypgid);
                            }
                        }
                        heartbeatStale = false;
                        continue;
                    }
                    if (g_unkillableStreak > 0) {
                        writeState("watchdog_unkillable", "0");
                        g_unkillableStreak = 0;
                        g_unkillablePid = -1;
                    }
                    unlink(aatpe::paths::kPidFile);
                    }
                }
            }
        }
        bool heartbeatFresh = (daemonAlive && hb > 0 && !heartbeatStale);
        bool heartbeatReallyFresh = daemonAlive && hb > 0 &&
            (bootTimeSec() - (time_t)hb) <= hbTimeoutLimit;
        if (heartbeatReallyFresh) g_staleStreak = 0;

        if (heartbeatFresh && restartCount > 0) {
            time_t nowHb = bootTimeSec();
            long long sinceRestart = (lastRestart > 0) ? (long long)(nowHb - lastRestart) : 0;
            bool latched = passiveMode || (crashLoopCycles >= 3);
            if (sinceRestart >= (long long)watchdog::kRecoveryStableSec * 4 && !latched) {
                logWatchdog("Healthy for %llds — clearing stale restart telemetry "
                            "(restart_count was %d, hard_crash_count was %d); hard-crash "
                            "latch untouched",
                            sinceRestart, restartCount, hardCrashCount);
                restartCount = 0;
                hardCrashCount = 0;
                lastHardCrash = 0;
                writeState("restart_count", "0");
                writeState("hard_crash_count", "0");
                writeState("watchdog_restart_reason", "normal");
            }
        }

        if (daemonAlive && !heartbeatStale) {
            std::string tf = readFile((std::string(aatpe::paths::kStateDir) + "/thermal_freshness").c_str());
            if (tf.rfind("stale", 0) == 0) {
                g_thermalStaleStreak++;
                if (thermalStaleTriggered(g_thermalStaleStreak)) {
                    addDegradedReason("thermal_thread_stale");
                }
            } else {
                if (g_thermalStaleStreak > 0) {
                    removeDegradedReason("thermal_thread_stale");
                }
                g_thermalStaleStreak = 0;
            }
        }

        if (!daemonAlive || heartbeatStale) {
            degradedHealthySince = 0;
            time_t nowC = bootTimeSec();
            const char* deathReason = classifyDeath(daemonStartedAt, heartbeatStale,
                                                     lastMemWasOver, nowC);
            logWatchdog("Daemon death classified as: %s", deathReason);
            std::string deathSig;
            bool thisDeathIsHard = readHardCrashSignal(deathSig);

            if (std::string(deathReason) == "binary_broken") {
                int br = checkBinaryUsable();
                logWatchdog("BINARY BROKEN (check=%d): %s — skipping restart until file restored",
                            br, DAEMON_PATH);
                addDegradedReason("binary_broken");
                interruptibleSleepSec(double(INTERVAL) * 5.0);
                continue;
            }
            removeDegradedReason("binary_broken");

            if (std::string(deathReason) == "rapid_crash" && thisDeathIsHard) {
                if (watchdog::countRapidCrash(inBootGrace)) {
                    rapidCrashCount++;
                    logWatchdog("Rapid crash %d/%d (daemon died < %ds after start)",
                                rapidCrashCount, watchdog::kMaxRapidCrashesBeforeDegrade,
                                watchdog::kRapidDeathThresholdSec);
                    if (watchdog::shouldEscalateDegraded(inBootGrace, rapidCrashCount,
                                                         watchdog::kMaxRapidCrashesBeforeDegrade,
                                                         degradedMode)) {
                        logWatchdog("ESCALATING to DEGRADED mode: %d rapid crashes in a row. "
                                    "Likely config error or binary corruption. Cooldown %ds.",
                                    rapidCrashCount, watchdog::kDegradedCooldownSec);
                        degradedMode = true;
                        degradedSince = nowC;
                        wdTransition(WdRuntimeState::DEGRADED, "rapid_crash_loop");
                        addDegradedReason("rapid_crash_loop");
                    }
                } else {
                    logWatchdog("Rapid death (died < %ds after start) DURING boot grace "
                                "(bootNow=%lds/%ds): not counting, not escalating — almost "
                                "certainly the service.sh+watchdog double-start flock race "
                                "under the boot storm, not a broken binary.",
                                watchdog::kRapidDeathThresholdSec,
                                (long)nowC, watchdog::kBootGraceSec);
                }
            } else if (std::string(deathReason) == "rapid_crash" && !thisDeathIsHard) {
                rapidCrashCount = 0;
                logWatchdog("Soft rapid death (signal=%s, class=soft) — benign, resetting rapid streak, NOT degrading",
                            deathSig.c_str());
            } else {
                rapidCrashCount = 0;
            }

            if (degradedMode) {
                if ((nowC - degradedSince) < watchdog::kDegradedCooldownSec) {
                    logWatchdog("Degraded mode: waiting %lds/%ds before retry",
                                (long)(nowC - degradedSince), watchdog::kDegradedCooldownSec);
                    continue;
                }
                logWatchdog("Degraded cooldown elapsed (%lds), attempting recovery restart",
                            (long)(nowC - degradedSince));
                degradedMode = false;
                degradedSince = 0;
                rapidCrashCount = 0;
                hardCrashCount = 0;
                removeDegradedReason("rapid_crash_loop");
                wdTransition(WdRuntimeState::RECOVERY, "degraded_cooldown_retry");
            }

            time_t nowR = bootTimeSec();

            std::string crashSig;
            bool isStuckKill = (std::string(deathReason) == "stuck");
            bool hardCrash = !isStuckKill && readHardCrashSignal(crashSig);
            writeState("watchdog_restart_reason",
                       (std::string(deathReason) + " class=" +
                        (hardCrash ? "hard_crash" : "soft_restart") +
                        " signal=" + (crashSig.empty() ? "-" : crashSig)).c_str());
            clearCrashSignal();

            if (lastRestart > 0 && (nowR - lastRestart) < RESTART_WINDOW)
                restartCount++;
            else
                restartCount = 1;
            lastRestart = nowR;

            if (hardCrash) {
                if (lastHardCrash > 0 && (nowR - lastHardCrash) < RESTART_WINDOW)
                    hardCrashCount++;
                else
                    hardCrashCount = 1;
                lastHardCrash = nowR;
            } else {
                hardCrashCount = 0;
            }

            writeState("restart_count", std::to_string(restartCount).c_str());
            writeState("hard_crash_count", std::to_string(hardCrashCount).c_str());

            if (hardCrashCount > MAX_RESTARTS) {
                if (!passiveMode) {
                    crashLoopCycles++;
                    logWatchdog("FATAL: Too many HARD crashes (%d > %d). Entering passive mode (crash-loop cycle %d).",
                                hardCrashCount, MAX_RESTARTS, crashLoopCycles);
                    wdTransition(WdRuntimeState::PASSIVE, "crash_loop_detected");
                    writeState("crash_loop_detected", "1");
                    passiveMode = true;
                    passiveSince = nowR;
                }
                int shiftCycles = crashLoopCycles;
                if (shiftCycles < 1) shiftCycles = 1;
                if (shiftCycles > 63) shiftCycles = 63;
                long passiveCooldown =
                    (long)watchdog::kPassiveCooldownSec * (1L << (shiftCycles - 1));
                if (passiveCooldown > 3600L) passiveCooldown = 3600L;
                bool manualUnlocked = false;
                if (crashLoopCycles >= 3) {
                    std::string v = readFile(
                        (std::string(aatpe::paths::kStateDir) + "/watchdog_passive_locked").c_str());
                    if (v == "0") manualUnlocked = true;
                }
                bool exitPassive = false;
                if (passiveMode) {
                    if (crashLoopCycles >= 3 && !manualUnlocked) {
                        if ((nowR - passiveSince) >= passiveCooldown) {
                            logWatchdog("crash-loop latched after %d cycles; staying PASSIVE, "
                                        "waiting for manual unlock_watchdog (watchdog_passive_locked=0)",
                                        crashLoopCycles);
                            passiveSince = nowR;
                        }
                        continue;
                    }
                    if (manualUnlocked && crashLoopCycles >= 3) {
                        logWatchdog("manual unlock_watchdog observed after %d crash-loop cycles; leaving PASSIVE",
                                    crashLoopCycles);
                        exitPassive = true;
                    } else if ((nowR - passiveSince) >= passiveCooldown) {
                        logWatchdog("passive cooldown elapsed (%lds >= %lds), entering RECOVERY",
                                    (long)(nowR - passiveSince), passiveCooldown);
                        exitPassive = true;
                    }
                }
                if (exitPassive) {
                    restartCount = 0;
                    hardCrashCount = 0;
                    lastHardCrash = 0;
                    crashLoopCycles = 0;
                    passiveMode = false;
                    passiveSince = 0;
                    writeState("crash_loop_detected", "0");
                    writeState("restart_count", "0");
                    clearAllDegradedReasons();
                    wdTransition(WdRuntimeState::RECOVERY, "crash_loop_cooldown_elapsed");
                } else {
                    continue;
                }
            }

            if (!daemonAlive && dpid <= 0)
                logWatchdog("Daemon PID missing/invalid (dpid=%d). Starting.", dpid);
            else if (!daemonAlive)
                logWatchdog("Daemon dead (was PID=%d). Restarting.", dpid);

            logWatchdog("Restart attempt %d/%d", restartCount, MAX_RESTARTS);

            if (restartDaemon()) {
                passiveMode = false;
                daemonStartedAt = bootTimeSec();
                lastMemWasOver = false;
                watchdogEmergencyMigrate();
            }
            continue;
        }

        bool wdDegradedOnDisk =
            (readFile((std::string(aatpe::paths::kStateDir) + "/watchdog_degraded").c_str())
             .find("rapid_crash_loop") != std::string::npos);
        if (degradedMode || wdDegradedOnDisk) {
            if (daemonAlive && heartbeatFresh) {
                time_t nowDh = bootTimeSec();
                if (degradedHealthySince == 0) degradedHealthySince = nowDh;
                long long healthyStreak = (long long)(nowDh - degradedHealthySince);
                if (watchdog::degradedSelfHeal(true, healthyStreak,
                                              watchdog::kRecoveryStableSec)) {
                    logWatchdog("DEGRADED self-heal: daemon alive + heartbeat-fresh %llds "
                                ">= %lds while latched (%s) — clearing degraded/rapid latch WITHOUT "
                                "killing, stepping to RECOVERY (control_mode assist -> active "
                                "after the recovery-stable window)",
                                healthyStreak, watchdog::kRecoveryStableSec,
                                degradedMode ? "in-memory" : "stale on-disk");
                    degradedMode = false;
                    degradedSince = 0;
                    rapidCrashCount = 0;
                    hardCrashCount = 0;
                    lastHardCrash = 0;
                    degradedHealthySince = 0;
                    removeDegradedReason("rapid_crash_loop");
                    writeState("healthy_since", "");
                    wdTransition(WdRuntimeState::RECOVERY, "degraded_healthy_self_heal");
                }
            }
        }

        if (watchdog::shouldAdoptExistingDaemon(daemonAlive, heartbeatFresh, daemonStartedAt > 0)) {
            long long ticks = aatpe::pid_identity::readPidStarttime(dpid);
            long hz = sysconf(_SC_CLK_TCK);
            time_t adoptedAt = (ticks > 0 && hz > 0) ? (time_t)(ticks / hz) : bootTimeSec();
            logWatchdog("ADOPT: pidfile PID=%d is live + heartbeat-fresh and we did not start "
                        "it (bootNow=%lds) — adopting as our daemon (daemonStartedAt=%lds), "
                        "NOT spawning a second instance",
                        dpid, (long)bootTimeSec(), (long)adoptedAt);
            daemonStartedAt = adoptedAt;
        }

        if (daemonStartedAt > 0 && !degradedMode) {
            long long aliveSec = (long long)(bootTimeSec() - daemonStartedAt);
            if (aliveSec > watchdog::kRapidDeathThresholdSec && rapidCrashCount > 0) {
                logWatchdog("Daemon stable for %llds — resetting rapid crash counter", aliveSec);
                rapidCrashCount = 0;
                removeDegradedReason("rapid_crash_loop");
            }
            if (aliveSec > 600 && crashLoopCycles != 0 && crashLoopCycles < 3) {
                logWatchdog("Daemon healthy for %llds — resetting crash-loop backoff cycles (%d -> 0)",
                            aliveSec, crashLoopCycles);
                crashLoopCycles = 0;
            }
        }

        time_t nowH = bootTimeSec();

        if (tempPassiveUntil != 0 && nowH >= tempPassiveUntil) {
            logWatchdog("temporary passive expired (%lds), entering RECOVERY",
                        (long)(nowH - (tempPassiveUntil - watchdog::kTempPassiveDurationSec)));
            wdTransition(WdRuntimeState::RECOVERY, "temp_passive_expired");
            tempPassiveUntil = 0;
        }

        int memWarnKb = readConfigInt("DAEMON_MEMORY_WARN_KB", watchdog::kDefaultMemWarnKb);
        bool memOver = false;
        std::string statusPath = "/proc/" + std::to_string(dpid) + "/status";
        std::ifstream sf(statusPath);
        if (sf) {
            std::string line;
            while (std::getline(sf, line)) {
                if (line.find("VmRSS:") == 0) {
                    size_t pos = line.find_first_of("0123456789");
                    if (pos != std::string::npos) {
                        try {
                            int memKb = std::stoi(line.substr(pos));
                            if (memKb > memWarnKb) {
                                memOver = true;
                                logWatchdog("WARNING: Daemon memory %dKB > %dKB (consecutive=%d)",
                                            memKb, memWarnKb, consecutiveMemoryWarns + 1);
                            }
                        } catch (...) {
                            static int s_vmRssParseWarned = 0;
                            if (!s_vmRssParseWarned) {
                                s_vmRssParseWarned = 1;
                                logWatchdog("WARNING: could not parse VmRSS line, leaving memWarn unchanged: %s",
                                            line.c_str());
                            }
                        }
                    }
                    break;
                }
            }
        }
        if (memOver) {
            consecutiveMemoryWarns++;
            lastMemWasOver = true;
            if (consecutiveMemoryWarns >= watchdog::kConsecutiveThreshold) {
                logWatchdog("ACTION: memory over limit %d consecutive times, SIGTERM graceful restart",
                            consecutiveMemoryWarns);
                if (!isAatpedIdentity(dpid)) {
                    logWatchdog("refusing to SIGTERM pid=%d (identity check failed; stale/reused PID)",
                                dpid);
                } else {
                    kill(dpid, SIGTERM);
                    bool graceful = false;
                    for (int i = 0; i < 20 && kill(dpid, 0) == 0; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (kill(dpid, 0) != 0) graceful = true;
                    if (!graceful) {
                        logWatchdog("SIGTERM pid=%d ignored, escalating to SIGKILL", dpid);
                        bool killed = killAndWaitDaemon(dpid);
                        if (!killed) {
                            logWatchdog("CRITICAL: pid=%d survived SIGKILL, NOT spawning new daemon",
                                        dpid);
                            consecutiveMemoryWarns = 0;
                            lastMemWasOver = false;
                            continue;
                        }
                    }
                }
                unlink(aatpe::paths::kPidFile);
                restartCount = 0;
                watchdogEmergencyMigrate();
                if (restartDaemon()) {
                    daemonStartedAt = bootTimeSec();
                } else {
                    logWatchdog("CRITICAL: OOM-path restartDaemon() failed; no daemon running this cycle");
                    daemonStartedAt = 0;
                }
                consecutiveMemoryWarns = 0;
                lastMemWasOver = false;
            }
        } else {
            consecutiveMemoryWarns = 0;
            lastMemWasOver = false;
        }

        int migCount = readInt((std::string(aatpe::paths::kStateDir) + "/migration_rate_current").c_str(), 0);
        int migLimit = readInt((std::string(aatpe::paths::kStateDir) + "/safety_migration_limit").c_str(),
                                watchdog::kDefaultMigrationLimit);
        if (migLimit <= 0) migLimit = watchdog::kDefaultMigrationLimit;
        if (migCount >= migLimit) {
            consecutiveMigWarns++;
            logWatchdog("WARNING: Migration rate %d/s exceeds limit %d (consecutive=%d)",
                        migCount, migLimit, consecutiveMigWarns);
            if (consecutiveMigWarns >= watchdog::kConsecutiveThreshold && tempPassiveUntil == 0) {
                logWatchdog("ACTION: migration rate over limit, entering temporary passive for %ds",
                            watchdog::kTempPassiveDurationSec);
                wdTransition(WdRuntimeState::PASSIVE, "migration_rate");
                tempPassiveUntil = nowH + watchdog::kTempPassiveDurationSec;
            }
        } else {
            consecutiveMigWarns = 0;
        }

        int loadLimit = readConfigInt("SAFETY_LOAD_LIMIT", watchdog::kDefaultLoadLimit);
        std::ifstream lf("/proc/loadavg");
        if (lf) {
            float load1 = 0.0f;
            lf >> load1;
            int loadInt = (int)(load1 * 100);
            if (loadInt >= loadLimit * 100) {
                consecutiveLoadWarns++;
                logWatchdog("WARNING: System load critical: %.2f (consecutive=%d)",
                            load1, consecutiveLoadWarns);
                if (consecutiveLoadWarns >= watchdog::kConsecutiveThreshold && tempPassiveUntil == 0) {
                    logWatchdog("ACTION: load over limit, entering temporary passive for %ds",
                                watchdog::kTempPassiveDurationSec);
                    wdTransition(WdRuntimeState::PASSIVE, "system_load");
                    tempPassiveUntil = nowH + watchdog::kTempPassiveDurationSec;
                }
            } else {
                consecutiveLoadWarns = 0;
            }
        }

        int thermalEmergency = readInt((std::string(aatpe::paths::kStateDir) + "/thermal_emergency").c_str(), 0);

        bool indForcedClear = false;
        bool effThermal = (thermalEmergency == 1);
        if (g_wdState == WdRuntimeState::EMERGENCY) {
            long long indMaxC = wdReadMaxTempCelsius();
            WdTempRead tr = wdClassifyIndTemp(indMaxC, indThermalWallC, indThermalMarginC);
            if (wdIndTempSample(tr, emergIndSafeStreak, emergIndNeedSamples)) {
                indForcedClear = true;
                logWatchdog("WARN: daemon thermal flag stale (==1), watchdog independent "
                            "max temp=%lldC <= %dC (wall=%dC margin=%dC) for %ds — exiting EMERGENCY",
                            indMaxC, indThermalWallC - indThermalMarginC,
                            indThermalWallC, indThermalMarginC, emergencyClearSec);
            }
        } else {
            long long indMaxC = wdReadMaxTempCelsius();
            if (indMaxC >= (long long)indThermalWallC) {
                logWatchdog("WARN: watchdog independent scan: max temp=%lldC >= wall=%dC "
                            "but daemon flag=0 — forcing emergency (daemon thermal mgmt failed)",
                            indMaxC, indThermalWallC);
                effThermal = true;
            }
            emergIndSafeStreak = 0;
        }
        if (indForcedClear) effThermal = false;

        if (effThermal) {
            emergClearSince = 0;
        } else if (g_wdState == WdRuntimeState::EMERGENCY && emergClearSince == 0) {
            emergClearSince = nowH;
            if (indForcedClear) {
                emergClearSince = nowH - emergencyClearSec;
                logWatchdog("Thermal cleared (daemon flag=%d, watchdog independent read authoritative); "
                            "stepping toward RECOVERY", thermalEmergency);
            } else {
                logWatchdog("Thermal cleared; starting %lds exit window before leaving EMERGENCY",
                            (long)emergencyClearSec);
            }
        }

        WdEvidence ev;
        ev.daemonAlive    = daemonAlive;
        ev.thermalEmergency = effThermal;
        ev.migOver        = (tempPassiveUntil != 0);
        ev.loadOver       = false;
        ev.explicitStop  = false;
        WdRuntimeState nextState = wdSteadyTransition(g_wdState, ev, nowH,
                                                     emergClearSince, g_recoverySince,
                                                     watchdog::kRecoveryStableSec,
                                                     emergencyClearSec);
        if (nextState != g_wdState) {
            const char* reason =
                (nextState == WdRuntimeState::RECOVERY && g_wdState == WdRuntimeState::EMERGENCY)
                    ? (indForcedClear ? "thermal_stale_flag_independent_read" : "thermal_cleared")
                    : (nextState == WdRuntimeState::NORMAL ? "recovery_stable" : "recover");
            wdTransition(nextState, reason);
            if (g_wdState == WdRuntimeState::NORMAL) emergClearSince = 0;
        }
    }

    logWatchdog("Watchdog stopping");
    unlink(WD_PID_FILE);
    unlink(WD_LOCK_FILE);
    if (g_wdLogFp) fclose(g_wdLogFp);
    return 0;
}

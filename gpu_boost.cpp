#include "gpu_boost.h"
#include "aatped.h"
#include "aatped_state.h"
#include "aatped_config.h"
#include "aatped_logging.h"
#include "aatped_gpu.h"

#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>
#include <atomic>
#include <dirent.h>
#include <cerrno>
#include <unordered_map>

namespace aatpe {
int aatpeThermalHalLevel();
bool aatpeThermalDegraded();

static constexpr float kPegEnterPct   = 95.0f;
static constexpr float kPegExitPct    = 80.0f;
static constexpr long long kBoostReleaseMs = 2500;
static constexpr long long kBoostMinHoldMs = 2000;
static constexpr int   kDevfreqTargetPctOfRange = 70;
static constexpr int   kHardReleaseTempAboveCritical = 5;
static constexpr int   kMinHeadroomPctForBoost       = 20;

namespace {
std::string s_boostNode;
std::mutex  s_boostNodeMutex;
std::atomic<long long> s_nextProbeMs{0};
std::atomic<int> s_consecWriteFails{0};
static constexpr long long kReprobeIntervalMs = 30000;
static constexpr int kWriteFailReprobeThreshold = 5;

std::mutex s_boostMutex;
int  s_touchLevel = 0;
int  s_gpuLevel    = 0;
std::unordered_map<std::string, int> s_sourceLevels;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> boostCandidates() {
    return {
        "/dev/stune/top-app/schedtune.boost",
        "/dev/stune/foreground/schedtune.boost",
        "/sys/fs/cgroup/stune/top-app/schedtune.boost",
        "/sys/fs/cgroup/top-app/schedtune.boost",
    };
}

bool boostNodeWritable(const char* path) {
    int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

std::string pickBoostNode(const std::vector<std::string>& cands,
                          bool(*writable)(const char*)) {
    for (const std::string& c : cands) {
        if (writable(c.c_str())) return c;
    }
    return std::string();
}

void probeTick(long long nowMs, bool(*writable)(const char*)) {
    std::string current;
    { std::lock_guard<std::mutex> lk(s_boostNodeMutex); current = s_boostNode; }

    if (!current.empty()) {
        if (s_consecWriteFails.load(std::memory_order_relaxed) < kWriteFailReprobeThreshold) {
            return;
        }
        LOG_WARN("[boost]: chosen node failed %d writes; re-probing candidates",
                 s_consecWriteFails.load(std::memory_order_relaxed));
        std::string picked = pickBoostNode(boostCandidates(), writable);
        {
            std::lock_guard<std::mutex> lk(s_boostNodeMutex);
            s_boostNode = picked;
            current = picked;
        }
        s_consecWriteFails.store(0);
        if (!picked.empty()) {
            LOG_INFO("[boost]: re-probe adopted node %s", picked.c_str());
        }
    }

    if (!current.empty()) return;
    if (nowMs < s_nextProbeMs.load(std::memory_order_relaxed)) return;
    std::string picked = pickBoostNode(boostCandidates(), writable);
    if (!picked.empty()) {
        {
            std::lock_guard<std::mutex> lk(s_boostNodeMutex);
            s_boostNode = picked;
        }
        s_nextProbeMs.store(0, std::memory_order_relaxed);
        LOG_INFO("[boost]: boost node discovered %s", picked.c_str());
    } else {
        s_nextProbeMs.store(nowMs + kReprobeIntervalMs, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_noNodeWarn{0};
        uint32_t c = s_noNodeWarn.fetch_add(1, std::memory_order_relaxed);
        if (c == 0 || (c % 10U) == 0U) {
            LOG_WARN("[boost]: no writable schedtune.boost node; retry in %llds",
                     (long long)(kReprobeIntervalMs / 1000));
        }
    }
}

bool writeBoostLocked(int v) {
    std::string node;
    { std::lock_guard<std::mutex> lk(s_boostNodeMutex); node = s_boostNode; }
    if (node.empty()) return false;
    int fd = ::open(node.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        s_consecWriteFails.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", v);
    bool ok = false;
    if (n > 0) {
        size_t total = (size_t)n;
        size_t off = 0;
        ssize_t w = 0;
        while (off < total) {
            w = ::write(fd, buf + off, total - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (w == 0) break;
            off += (size_t)w;
        }
        ok = (off == total);
        if (ok) {
            s_consecWriteFails.store(0, std::memory_order_relaxed);
        } else {
            s_consecWriteFails.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<uint32_t> s_writeErrCounter{0};
            uint32_t c = s_writeErrCounter.fetch_add(1, std::memory_order_relaxed);
            if ((c & 63U) == 0U) {
                LOG_WARN("boost: write(%s) short/err (wrote=%zd/%zu errno=%d) for value %d",
                         node.c_str(), w, total, errno, v);
            }
        }
    }
    ::close(fd);
    return ok;
}

int recomputeBestLocked() {
    int best = 0;
    if (s_touchLevel > best) best = s_touchLevel;
    if (s_gpuLevel > best) best = s_gpuLevel;
    for (const auto& kv : s_sourceLevels) {
        if (kv.second > best) best = kv.second;
    }
    return best;
}

std::atomic<int> s_lastWrittenLevel{-1};

static constexpr int64_t kRetryBackoffMs = 500;
std::atomic<int64_t> s_nextRetryAllowedMs{0};

static std::mutex g_markerMtx;
static bool        s_schedtuneActive = false;
static std::string s_schedtuneActivePath;
static bool        s_devfreqActive = false;
static std::string s_devfreqActivePath;

static void rebuildGpuBoostMarkerLocked() {
    std::string payload;
    if (s_schedtuneActive) {
        payload += "schedtune=";
        payload += s_schedtuneActivePath;
    }
    if (s_devfreqActive) {
        if (!payload.empty()) payload += ";";
        payload += "devfreq=";
        payload += s_devfreqActivePath;
    }
    if (payload.empty()) {
        unlink((std::string(STATE_DIR) + "/gpu_boost_active").c_str());
    } else {
        writeFile((std::string(STATE_DIR) + "/gpu_boost_active").c_str(), payload);
    }
}

static void markSchedtuneActive(bool active, const std::string& path) {
    std::lock_guard<std::mutex> lk(g_markerMtx);
    s_schedtuneActive = active;
    s_schedtuneActivePath = active ? path : std::string();
    rebuildGpuBoostMarkerLocked();
}

static void markDevfreqActive(bool active, const std::string& path) {
    std::lock_guard<std::mutex> lk(g_markerMtx);
    s_devfreqActive = active;
    s_devfreqActivePath = active ? path : std::string();
    rebuildGpuBoostMarkerLocked();
}

void pushBoostWrite(int best) {
    writeFile((std::string(STATE_DIR) + "/schedtune_boost_current").c_str(),
              std::to_string(best));
    if (best == s_lastWrittenLevel.load(std::memory_order_relaxed)) return;
    int64_t nowMsVal = nowMs();
    if (nowMsVal < s_nextRetryAllowedMs.load(std::memory_order_relaxed)) return;
    if (writeBoostLocked(best)) {
        s_lastWrittenLevel.store(best, std::memory_order_relaxed);
        s_nextRetryAllowedMs.store(0, std::memory_order_relaxed);
        if (best > 0) {
            std::string node;
            { std::lock_guard<std::mutex> lk(s_boostNodeMutex); node = s_boostNode; }
            markSchedtuneActive(true, node);
        } else {
            markSchedtuneActive(false, std::string());
        }
    } else {
        s_nextRetryAllowedMs.store(nowMsVal + kRetryBackoffMs, std::memory_order_relaxed);
    }
}
}

static std::mutex g_devfreqMtx;
static std::string g_devfreqMinNode;
static unsigned long g_devfreqSavedMin = 0;
static bool g_devfreqBoosted = false;
static int g_devfreqOffFails = 0;

std::atomic<bool> s_boostEngaged{false};
std::atomic<long long> s_boostEngagedMs{0};
std::atomic<long long> s_lastPeggedMs{0};

#ifdef AATPE_GPU_BOOST_TEST
void devfreqBoostOn(unsigned long);
void devfreqBoostOff();
namespace boost_test {
void resetForTest() {
    { std::lock_guard<std::mutex> lk(s_boostNodeMutex); s_boostNode.clear(); }
    s_nextProbeMs.store(0);
    s_consecWriteFails.store(0);
}
void tickForTest(long long nowMs, bool(*writable)(const char*)) { probeTick(nowMs, writable); }
std::string nodeForTest() {
    std::lock_guard<std::mutex> lk(s_boostNodeMutex); return s_boostNode;
}
int consecFailsForTest() { return s_consecWriteFails.load(); }
void devfreqSetNodeForTest(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    g_devfreqMinNode = p;
}
std::string devfreqNodeForTest() {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    return g_devfreqMinNode;
}
bool devfreqBoostedForTest() {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    return g_devfreqBoosted;
}
unsigned long devfreqSavedMinForTest() {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    return g_devfreqSavedMin;
}
int devfreqOffFailsForTest() {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    return g_devfreqOffFails;
}
std::string devfreqMarkerPathForTest() {
    return std::string(STATE_DIR) + "/gpu_boost_active";
}
void devfreqResetForTest() {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    g_devfreqMinNode.clear(); g_devfreqBoosted=false; g_devfreqSavedMin=0; g_devfreqOffFails=0;
}
void devfreqBoostOnForTest() { devfreqBoostOn(0); }
void devfreqBoostOffForTest() { devfreqBoostOff(); }
bool boostEngagedForTest() { return s_boostEngaged.load(std::memory_order_relaxed); }
void boostResetHysteresisForTest() {
    s_boostEngaged.store(false, std::memory_order_relaxed);
    s_boostEngagedMs.store(0, std::memory_order_relaxed);
    s_lastPeggedMs.store(0, std::memory_order_relaxed);
}
long long boostEngagedMsForTest() { return s_boostEngagedMs.load(std::memory_order_relaxed); }
}
#endif

void aatpeBoostSet(const char* source, int level) {
    if (!source) return;
    probeTick(nowMs(), boostNodeWritable);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    int best;
    {
        std::lock_guard<std::mutex> lock(s_boostMutex);
        std::string s(source);
        if (s == "touch") {
            s_touchLevel = level;
        } else if (s == "gpu") {
            s_gpuLevel = level;
        } else {
            s_sourceLevels[s] = level;
        }
        best = recomputeBestLocked();
    }
    pushBoostWrite(best);
}

void aatpeBoostClear(const char* source) {
    if (!source) return;
    probeTick(nowMs(), boostNodeWritable);
    int best;
    {
        std::lock_guard<std::mutex> lock(s_boostMutex);
        std::string s(source);
        if (s == "touch") {
            s_touchLevel = 0;
        } else if (s == "gpu") {
            s_gpuLevel = 0;
        } else {
            s_sourceLevels.erase(s);
        }
        best = recomputeBestLocked();
    }
    pushBoostWrite(best);
}

void devfreqBoostOn(unsigned long maxFreq);
void devfreqBoostOff();

void gpuBoostTick() {
    probeTick(nowMs(), boostNodeWritable);
    aatpeGpuRefreshUtilForBoost();
    long long now = nowMs();

    int cpuTempC      = g_thermalCpu.load(std::memory_order_relaxed);
    int headroomPct   = g_thermalHeadroomPct.load(std::memory_order_relaxed);
    bool emergency    = (getSystemState() == SystemState::EMERGENCY);
    bool halSevere    = (aatpeThermalHalLevel() >= 1);
    bool sensorDegraded = aatpeThermalDegraded();

    int warningWall  = g_cfg.thermalWarningThreshold;
    int criticalWall = g_cfg.thermalCriticalThreshold;
    int hardReleaseTemp = criticalWall + kHardReleaseTempAboveCritical;

    int tier;
    if (emergency || halSevere) {
        tier = 3;
    } else if (cpuTempC <= 0 || sensorDegraded) {
        tier = 2;
    } else if (cpuTempC >= hardReleaseTemp || headroomPct < kMinHeadroomPctForBoost) {
        tier = 2;
    } else if (cpuTempC >= criticalWall) {
        tier = 2;
    } else if (cpuTempC >= warningWall) {
        tier = 1;
    } else {
        tier = 0;
    }

    if (tier == 2 || tier == 3) {
        aatpeBoostClear("gpu");
        devfreqBoostOff();
        s_boostEngaged.store(false, std::memory_order_relaxed);
        return;
    }

    std::string node;
    { std::lock_guard<std::mutex> lk(s_boostNodeMutex); node = s_boostNode; }
    if (node.empty()) return;

    bool game = isPackageInGameList(aatpeGetForegroundPackage());
    float util = g_gpuUtilization.load(std::memory_order_relaxed);

    bool engaged = s_boostEngaged.load(std::memory_order_relaxed);
    bool wantBoost = false;
    if (!engaged) {
        if (game && util >= kPegEnterPct) wantBoost = true;
    } else {
        long long heldFor = now - s_boostEngagedMs.load(std::memory_order_relaxed);
        bool minHoldOk = (heldFor >= kBoostMinHoldMs);
        if (!game) {
            wantBoost = false;
        } else if (util >= kPegExitPct) {
            wantBoost = true;
        } else if (!minHoldOk) {
            wantBoost = true;
        } else {
            wantBoost = false;
        }
    }

    if (wantBoost) {
        if (!engaged) {
            s_boostEngaged.store(true, std::memory_order_relaxed);
            s_boostEngagedMs.store(now, std::memory_order_relaxed);
        }
        s_lastPeggedMs.store(now, std::memory_order_relaxed);

        int stuneLevel = (tier == 1) ? (g_cfg.schedtuneCriticalBoost / 2)
                                     : g_cfg.schedtuneCriticalBoost;
        aatpeBoostSet("gpu", stuneLevel);
        if (tier == 0) {
            devfreqBoostOn(0);
        } else {
            devfreqBoostOff();
        }
        LOG_INFO("[P2-5] GPU boost tier=%d util=%.0f%% temp=%dC hr=%d%%",
                 tier, (double)util, cpuTempC, headroomPct);
    } else {
        long long last = s_lastPeggedMs.load(std::memory_order_relaxed);
        devfreqBoostOff();
        int best;
        {
            std::lock_guard<std::mutex> lock(s_boostMutex);
            if (s_gpuLevel > 0 && now - last >= kBoostReleaseMs) {
                s_gpuLevel = 0;
                s_boostEngaged.store(false, std::memory_order_relaxed);
                best = recomputeBestLocked();
            } else {
                return;
            }
        }
        pushBoostWrite(best);
    }
}

bool gpuBoostActive() {
    std::lock_guard<std::mutex> lock(s_boostMutex);
    return s_gpuLevel > 0;
}

void aatpeGpuBoostForceZero(const char* reason) {
    (void)reason;
    aatpeBoostClear("gpu");
    aatpeBoostClear("touch");
    pushBoostWrite(0);
    devfreqBoostOff();
}

static int64_t s_devfreqNotFoundCacheMs = 0;
static constexpr int64_t kDevfreqNegCacheMs = 30000;

static void detectDevfreqNode() {
    int64_t now = nowMs();
    if (s_devfreqNotFoundCacheMs != 0 &&
        (now - s_devfreqNotFoundCacheMs) < kDevfreqNegCacheMs) {
        return;
    }
    DIR* d = opendir("/sys/class/devfreq");
    if (!d) {
        s_devfreqNotFoundCacheMs = now;
        return;
    }
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.find("mali") != std::string::npos || name.find("kgsl") != std::string::npos) {
            std::string p = "/sys/class/devfreq/" + name + "/min_freq";
            if (access(p.c_str(), W_OK) == 0) { g_devfreqMinNode = p; found = true; break; }
        }
    }
    closedir(d);
    if (!found) s_devfreqNotFoundCacheMs = now;
    else s_devfreqNotFoundCacheMs = 0;
}

#ifdef AATPE_HOST_TEST
int devfreqNegCacheAgeSecForTest() {
    if (s_devfreqNotFoundCacheMs == 0) return -1;
    return (int)((nowMs() - s_devfreqNotFoundCacheMs) / 1000);
}
#endif

void devfreqBoostOn(unsigned long ) {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    if (g_devfreqMinNode.empty()) detectDevfreqNode();
    if (g_devfreqMinNode.empty() || g_devfreqBoosted) return;
    unsigned long savedMin = 0;
    int fd = open(g_devfreqMinNode.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32] = {0};
        ssize_t r = read(fd, buf, sizeof(buf)-1);
        close(fd);
        if (r > 0) savedMin = strtoul(buf, nullptr, 10);
    }
    if (savedMin == 0) {
        LOG_WARN("devfreqBoostOn: min_freq baseline unreadable (savedMin=0) -> NOT pinning (fail-closed)");
        return;
    }
    unsigned long maxF = 0;
    std::string maxNode = g_devfreqMinNode.substr(0, g_devfreqMinNode.rfind('/') + 1) + "max_freq";
    fd = open(maxNode.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32] = {0};
        if (read(fd, buf, sizeof(buf)-1) > 0) maxF = strtoul(buf, nullptr, 10);
        close(fd);
    }
    if (maxF == 0) return;
    unsigned long targetF = savedMin + (maxF - savedMin) * (unsigned long)kDevfreqTargetPctOfRange / 100UL;
    if (targetF <= savedMin) targetF = savedMin + 1;
    if (targetF > maxF) targetF = maxF;
    g_devfreqSavedMin = savedMin;
    fd = open(g_devfreqMinNode.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "%lu", targetF);
        if (write(fd, buf, n) == n) {
            g_devfreqBoosted = true;
            markDevfreqActive(true, g_devfreqMinNode);
        }
        close(fd);
    }
}

void devfreqBoostOff() {
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    if (!g_devfreqBoosted) return;
    bool writeOk = false;
    bool readbackOk = false;

    int fd = open(g_devfreqMinNode.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lu", g_devfreqSavedMin);
        ssize_t w = write(fd, buf, (size_t)n);
        if (w == n) writeOk = true;
        close(fd);
    }

    if (writeOk) {
        fd = open(g_devfreqMinNode.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            char rbuf[32] = {0};
            ssize_t r = read(fd, rbuf, sizeof(rbuf) - 1);
            close(fd);
            if (r > 0) {
                unsigned long v = strtoul(rbuf, nullptr, 10);
                if (v == g_devfreqSavedMin) readbackOk = true;
            }
        }
    }

    if (writeOk && readbackOk) {
        g_devfreqBoosted = false;
        g_devfreqOffFails = 0;
        markDevfreqActive(false, std::string());
    } else {
        g_devfreqOffFails++;
        LOG_WARN("devfreqBoostOff: release failed (writeOk=%d readbackOk=%d, fail#%d) — "
                 "keeping boosted state for retry",
                 (int)writeOk, (int)readbackOk, g_devfreqOffFails);
    }
}
}

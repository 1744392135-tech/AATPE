// AATPE CPU/GPU joint scheduling — shared boost bridge implementation
// (P2-5 GPU-pegged dwell; P1-4/P1-5/P1-14/P2 write-error hardening)
#include "gpu_boost.h"
#include "aatped.h"
#include "aatped_state.h"     // g_gpuUtilization, g_foregroundApp
#include "aatped_config.h"    // isPackageInGameList
#include "aatped_logging.h"
#include "aatped_gpu.h"       // F-07: aatpeGpuRefreshUtilForBoost

#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>
#include <atomic>
#include <dirent.h>
#include <cerrno>
#include <unordered_map>

namespace aatpe {
int aatpeThermalHalLevel();   // ROUND-014 P3: defined in thermal.cpp
bool aatpeThermalDegraded();  // T-06: sensor-outage flag (thermal.cpp)

// ============================================================================
// T-01/T-02/T-03/T-05/T-06/T-07/T-08 rework (v6.0.0 thermal拔河 fix):
//
// Old policy: GPU boost gate was `EMERGENCY(120C) || HAL level>=1`. On MTK
// D1100 the kernel thermal-engine does NOT report to the AOSP Thermal HAL, so
// s_halCache.level is permanently 0, and the module kept devfreq min_freq
// pinned at max_freq + schedtune=60 all the way from 80C to 120C — directly
// fighting the kernel's ~94C hardware throttle. AB test showed the module
// running hotter than stock.
//
// New policy (temperature tiers, driven by REAL sysfs temp g_thermalCpu and
// g_thermalHeadroomPct, not by the always-zero MTK HAL level):
//   tier 0  temp < warningWall(80)                 : full boost (stune=60 + devfreq floor@70%)
//   tier 1  warningWall <= temp < criticalWall(90)  : stune=30, devfreq floor RELEASED
//   tier 2  temp >= criticalWall+5(95) OR headroom<20% OR sensor-degraded OR cpuTemp<=0
//                                                   : full release (cooling-first)
//   tier 3  EMERGENCY OR HAL level>=1               : full release (existing hard wall)
//
// Load hysteresis (T-08): enter on util>=95%, exit only after util<80% for
// kBoostReleaseMs(2500ms). Minimum hold kBoostMinHoldMs(2000ms) after entry so
// a 50ms-decision-cycle util jitter cannot flip the boost on/off within a tick.
//
// Devfreq floor (T-02): pin min_freq to savedMin + 70%*(maxF-savedMin), NOT to
// maxF. This gives the GPU headroom without locking the die at the top OPP
// (which on Mali D1100 is the dominant heat source).
// ============================================================================
static constexpr float kPegEnterPct   = 95.0f;  // % busy -> engage boost
static constexpr float kPegExitPct    = 80.0f;  // % busy -> disengage (hysteresis band)
static constexpr long long kBoostReleaseMs = 2500;   // dwell after util drops below exit band
static constexpr long long kBoostMinHoldMs = 2000;   // minimum hold after engage
static constexpr int   kDevfreqTargetPctOfRange = 70; // floor = savedMin + 70%*(maxF-savedMin)
static constexpr int   kHardReleaseTempAboveCritical = 5; // >= critical+5 -> full release
static constexpr int   kMinHeadroomPctForBoost       = 20; // <20% headroom -> release

namespace {
// ROUND-002 audit G5-001: the old code probed the boost node exactly once via
// std::call_once. If that first probe found no writable path (vendor ships a
// 5th schedtune path, or a transient SELinux denial at boot), s_boostNode stayed
// empty for the ENTIRE session and GPU/CPU boost silently died — same shape as the
// v3.4.7 healthy-device regression. The node selection is now RE-PROBED:
//   * on a 30s throttle until the first writable node appears;
//   * again after kWriteFailReprobeThreshold consecutive write failures (in case
//     the first pick was the wrong path).
std::string s_boostNode;                 // chosen node; empty = none discovered yet
std::mutex  s_boostNodeMutex;           // s_boostNode is mutable again
std::atomic<long long> s_nextProbeMs{0}; // earliest time to run the next probe
std::atomic<int> s_consecWriteFails{0}; // consecutive write failures on chosen node
static constexpr long long kReprobeIntervalMs = 30000;
static constexpr int kWriteFailReprobeThreshold = 5;

// P1-5: per-producer active levels. Protected by s_boostMutex. The bridge
// writes max() across producers to the node, so touch and GPU never clobber.
std::mutex s_boostMutex;
int  s_touchLevel = 0;
int  s_gpuLevel    = 0;
std::unordered_map<std::string, int> s_sourceLevels;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> boostCandidates() {
    // P1-4: four candidates aligned with input_monitor's probe list.
    return {
        "/dev/stune/top-app/schedtune.boost",
        "/dev/stune/foreground/schedtune.boost",
        "/sys/fs/cgroup/stune/top-app/schedtune.boost",
        "/sys/fs/cgroup/top-app/schedtune.boost",
    };
}

bool boostNodeWritable(const char* path) {
    // P1 round7: access(W_OK) is short-circuited by CAP_DAC_OVERRIDE (root),
    // so it reports writable even when the node is not actually openable. Use a
    // real open(O_WRONLY) + immediate close — that reflects true writability.
    int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

// Pure selection seam: first candidate the predicate accepts. Injectable in tests.
std::string pickBoostNode(const std::vector<std::string>& cands,
                          bool(*writable)(const char*)) {
    for (const std::string& c : cands) {
        if (writable(c.c_str())) return c;
    }
    return std::string();
}

// Re-probe driver. `writable` is the access() predicate (injectable in tests).
// Does ::access syscalls OUTSIDE s_boostMutex.
void probeTick(long long nowMs, bool(*writable)(const char*)) {
    std::string current;
    { std::lock_guard<std::mutex> lk(s_boostNodeMutex); current = s_boostNode; }

    // Chosen node but writes keep failing -> re-probe once (wrong-path recovery).
    if (!current.empty()) {
        if (s_consecWriteFails.load(std::memory_order_relaxed) < kWriteFailReprobeThreshold) {
            return;  // node stable, no re-probe
        }
        LOG_WARN("[boost]: chosen node failed %d writes; re-probing candidates",
                 s_consecWriteFails.load(std::memory_order_relaxed));
        std::string picked = pickBoostNode(boostCandidates(), writable);
        {
            std::lock_guard<std::mutex> lk(s_boostNodeMutex);
            s_boostNode = picked;   // may be empty -> falls into periodic retry
            current = picked;
        }
        s_consecWriteFails.store(0);
        if (!picked.empty()) {
            LOG_INFO("[boost]: re-probe adopted node %s", picked.c_str());
        }
    }

    // Still no node: throttled periodic probe until one appears.
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
        if (c == 0 || (c % 10U) == 0U) {  // first attempt + every ~10th retry
            LOG_WARN("[boost]: no writable schedtune.boost node; retry in %llds",
                     (long long)(kReprobeIntervalMs / 1000));
        }
    }
}

// P2 write-error checking. P3-25 short-write retry. G5-004 returns true only on
// a full write; G5-001(ROUND-002) hooks consecutive failures into re-probing.
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
            if (w == 0) break;  // P3 round7-rework
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

// Recompute max(touch, gpu, other sources) — returns the new best level.
// Caller must hold s_boostMutex. Does NOT write; call pushBoostWrite() after
// releasing the lock to avoid doing syscalls under the mutex (LED-054/R4-010).
int recomputeBestLocked() {
    int best = 0;
    if (s_touchLevel > best) best = s_touchLevel;
    if (s_gpuLevel > best) best = s_gpuLevel;
    for (const auto& kv : s_sourceLevels) {
        if (kv.second > best) best = kv.second;
    }
    return best;
}

// LED-053 (R4-009): remember the last value SUCCESSFULLY written to the node so
// repeated set() calls with the same level don't open/write/close the cgroup
// file every decision cycle. pushBoostWrite() is called AFTER releasing
// s_boostMutex, so this must be atomic rather than a plain int.
std::atomic<int> s_lastWrittenLevel{-1};

// G5-004: bounded retry backoff after a write failure. A failed level must be
// retried (the node may recover after a transient EACCES / replug), but we must
// not open/write/close the node on every tick when it is permanently broken.
static constexpr int64_t kRetryBackoffMs = 500;
std::atomic<int64_t> s_nextRetryAllowedMs{0};

// GPUTOP-02: gpu_boost_active marker was previously written by TWO producers
// (schedtune bridge and devfreq pin) with DIFFERENT content, and either side
// could unlink the file while the other was still active. We now keep two
// independent active bits and rebuild the marker as an aggregate
// "schedtune=<path>;devfreq=<path>" string whenever either flips. The marker
// is unlinked only when BOTH sides are inactive. All access goes through
// rebuildGpuBoostMarkerLocked() under g_markerMtx.
static std::mutex g_markerMtx;
static bool        s_schedtuneActive = false;
static std::string s_schedtuneActivePath;
static bool        s_devfreqActive = false;
static std::string s_devfreqActivePath;

static void rebuildGpuBoostMarkerLocked() {
    // Caller holds g_markerMtx.
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
    // v4.2 A2: publish the aggregated schedtune.boost level for doctor/WebUI. This
    // bridge is the ONLY place that knows the true max(touch,gpu,placement); the
    // placement producer alone cannot compute it. Written before the dedup skip so
    // the state key always reflects the current desired level. Additive telemetry —
    // the Set/Clear/recompute/max contract is unchanged.
    writeFile((std::string(STATE_DIR) + "/schedtune_boost_current").c_str(),
              std::to_string(best));
    // LED-053 (R4-009): skip when the node already holds this value.
    if (best == s_lastWrittenLevel.load(std::memory_order_relaxed)) return;
    // G5-004: respect the post-failure backoff so a persistently-broken node is
    // not hammered every tick.
    int64_t nowMsVal = nowMs();
    if (nowMsVal < s_nextRetryAllowedMs.load(std::memory_order_relaxed)) return;
    if (writeBoostLocked(best)) {
        s_lastWrittenLevel.store(best, std::memory_order_relaxed);
        s_nextRetryAllowedMs.store(0, std::memory_order_relaxed);
        // GPUTOP-02: marker lifecycle now aggregates schedtune + devfreq bits.
        // schedtune side flips here; devfreq side flips in devfreqBoostOn/Off.
        if (best > 0) {
            std::string node;
            { std::lock_guard<std::mutex> lk(s_boostNodeMutex); node = s_boostNode; }
            markSchedtuneActive(true, node);
        } else {
            markSchedtuneActive(false, std::string());
        }
    } else {
        // Leave s_lastWrittenLevel unchanged so the next eligible tick retries;
        // arm a short backoff to bound the retry rate.
        s_nextRetryAllowedMs.store(nowMsVal + kRetryBackoffMs, std::memory_order_relaxed);
    }
}
}  // namespace

// GPUTOP-01: devfreq state. Guarded by g_devfreqMtx so the main-loop tick and
// the thermal-emergency force-zero path cannot race on g_devfreqBoosted /
// g_devfreqSavedMin (plain bool / unsigned long were previously unguarded).
static std::mutex g_devfreqMtx;
static std::string g_devfreqMinNode;
static unsigned long g_devfreqSavedMin = 0;
static bool g_devfreqBoosted = false;
// #16: consecutive failed release attempts. Used for diagnostics; retries are
// driven by gpuBoostTick calling devfreqBoostOff() every tick while armed.
static int g_devfreqOffFails = 0;

// T-08 hysteresis state machine. Declared here (before the AATPE_GPU_BOOST_TEST
// block) so the test seams below can reference them.
std::atomic<bool> s_boostEngaged{false};
std::atomic<long long> s_boostEngagedMs{0};
// P2-4: was a plain `long long` written without the mutex by gpuBoostTick()
// and read under s_boostMutex on the release path. atomic<long long> with
// relaxed order is sufficient (no dependent state).
std::atomic<long long> s_lastPeggedMs{0};

#ifdef AATPE_GPU_BOOST_TEST
// forward decls (real impls below)
void devfreqBoostOn(unsigned long);
void devfreqBoostOff();
namespace boost_test {
// Test seam: drive the re-probe state machine with an injected writable predicate.
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
// ROUND-014 P3: devfreq test seam.
// GPUTOP-01: all test accessors take g_devfreqMtx so tests observe consistent state.
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
// T-08 hysteresis test seams.
bool boostEngagedForTest() { return s_boostEngaged.load(std::memory_order_relaxed); }
void boostResetHysteresisForTest() {
    s_boostEngaged.store(false, std::memory_order_relaxed);
    s_boostEngagedMs.store(0, std::memory_order_relaxed);
    s_lastPeggedMs.store(0, std::memory_order_relaxed);
}
long long boostEngagedMsForTest() { return s_boostEngagedMs.load(std::memory_order_relaxed); }
}  // namespace boost_test
#endif

// ---- Shared boost bridge (P1-5) ------------------------------------------
void aatpeBoostSet(const char* source, int level) {
    if (!source) return;
    // G5-001(ROUND-002): (re-)probe the node on every call; re-probe is throttled
    // inside probeTick (30s, plus re-probe after write failures).
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
    // LED-054 (R4-010): do the open/write/close syscall OUTSIDE the mutex.
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
    // LED-054: write outside the lock.
    pushBoostWrite(best);
}

void devfreqBoostOn(unsigned long maxFreq);
void devfreqBoostOff();

void gpuBoostTick() {
    // G5-001(ROUND-002): re-probe (throttled) instead of one-shot call_once.
    probeTick(nowMs(), boostNodeWritable);
    // F-07: pull a fresh util reading. Self-throttled to 1s inside; on host
    // tests this resolves to a no-op stub that leaves g_gpuUtilization alone.
    aatpeGpuRefreshUtilForBoost();
    long long now = nowMs();

    // ------------------------------------------------------------------
    // T-01/T-03: temperature tier decision. On MTK D1100 the AOSP Thermal
    // HAL level is permanently 0 (kernel thermal-engine owns throttling), so
    // the old gate `EMERGENCY || HAL>=1` never fired before 120C. We now read
    // the REAL sysfs temp (g_thermalCpu, refreshed by thermal.cpp's 1s scan)
    // and the headroom pct, and tier the boost down BEFORE the kernel's ~94C
    // hardware throttle point.
    // ------------------------------------------------------------------
    int cpuTempC      = g_thermalCpu.load(std::memory_order_relaxed);
    int headroomPct   = g_thermalHeadroomPct.load(std::memory_order_relaxed);
    bool emergency    = (getSystemState() == SystemState::EMERGENCY);
    bool halSevere    = (aatpeThermalHalLevel() >= 1);
    bool sensorDegraded = aatpeThermalDegraded();

    int warningWall  = g_cfg.thermalWarningThreshold;    // 80
    int criticalWall = g_cfg.thermalCriticalThreshold;   // 90
    int hardReleaseTemp = criticalWall + kHardReleaseTempAboveCritical; // 95

    // Tier computation (0=full, 1=partial, 2=release, 3=hard-release).
    int tier;
    if (emergency || halSevere) {
        tier = 3;
    } else if (cpuTempC <= 0 || sensorDegraded) {
        // T-06 fail-safe: no valid reading or sensor degraded -> do NOT boost.
        // The old code fail-opened to full perf, which on a sensor outage meant
        // "treat as 0C = boost hard". Cooling-first wins.
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

    // Hard release (tier 2/3): clear everything, reset hysteresis.
    if (tier == 2 || tier == 3) {
        aatpeBoostClear("gpu");
        devfreqBoostOff();
        s_boostEngaged.store(false, std::memory_order_relaxed);
        return;
    }

    std::string node;
    { std::lock_guard<std::mutex> lk(s_boostNodeMutex); node = s_boostNode; }
    if (node.empty()) return;  // no node yet: stay inert (will retry)

    bool game = isPackageInGameList(aatpeGetForegroundPackage());
    float util = g_gpuUtilization.load(std::memory_order_relaxed);

    // T-08 hysteresis state machine.
    bool engaged = s_boostEngaged.load(std::memory_order_relaxed);
    bool wantBoost = false;
    if (!engaged) {
        // Enter only on the enter threshold (95%).
        if (game && util >= kPegEnterPct) wantBoost = true;
    } else {
        // Already engaged: stay engaged while util is above exit band (80%),
        // and always hold for at least kBoostMinHoldMs after entry.
        long long heldFor = now - s_boostEngagedMs.load(std::memory_order_relaxed);
        bool minHoldOk = (heldFor >= kBoostMinHoldMs);
        if (!game) {
            wantBoost = false;
        } else if (util >= kPegExitPct) {
            wantBoost = true;
        } else if (!minHoldOk) {
            wantBoost = true;  // still inside minimum hold window
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

        // T-07: tier 1 (80..90C) halves schedtune and releases devfreq floor.
        // tier 0 (<80C) keeps full schedtune + devfreq floor.
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
        // Not wanted: dwell then release.
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
        // LED-054: write outside the mutex.
        pushBoostWrite(best);
    }
}

bool gpuBoostActive() {
    std::lock_guard<std::mutex> lock(s_boostMutex);
    return s_gpuLevel > 0;
}

// ROUND-014 P3: emergency edge -> push node to 0 and clear marker. G1 calls on
// thermal EMERGENCY entry; normal path re-arms on next tick if GPU still pegged.
// GPUTOP-02: the explicit unlink is removed; pushBoostWrite(0) clears the
// schedtune bit and devfreqBoostOff() clears the devfreq bit, and the aggregate
// marker is rebuilt (unlinked only when both bits are false).
void aatpeGpuBoostForceZero(const char* reason) {
    (void)reason;
    aatpeBoostClear("gpu");
    aatpeBoostClear("touch");
    pushBoostWrite(0);
    devfreqBoostOff();
}


// H3: negative cache for devfreq detection. On D1100 /sys/class/devfreq is empty
// (no GPU devfreq nodes). Without caching, devfreqBoostOn() calls detectDevfreqNode()
// every tick (every main loop cycle) -> wasted opendir. Cache "not found" for 30s
// before retrying (allows hot-plug / late init detection).
// H12: use monotonic clock (nowMs() = steady_clock ms), not wall clock — NTP
// jumps must not artificially extend/shorten the negative cache.
static int64_t s_devfreqNotFoundCacheMs = 0;   // monotonic ms, 0 = no cache
static constexpr int64_t kDevfreqNegCacheMs = 30000;

static void detectDevfreqNode() {
    int64_t now = nowMs();
    // H3: negative cache hit -> skip re-probe.
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
    else s_devfreqNotFoundCacheMs = 0;  // success: no negative cache
}

// Test seam: expose negative-cache age in seconds for v94 unit tests.
#ifdef AATPE_HOST_TEST
int devfreqNegCacheAgeSecForTest() {
    if (s_devfreqNotFoundCacheMs == 0) return -1;  // no cache entry
    return (int)((nowMs() - s_devfreqNotFoundCacheMs) / 1000);
}
#endif

void devfreqBoostOn(unsigned long /*maxFreqUnused*/) {
    // GPUTOP-01: hold g_devfreqMtx for the whole On/Off so the main-loop tick
    // and the thermal-emergency force-zero path cannot race on
    // g_devfreqBoosted / g_devfreqSavedMin / g_devfreqMinNode.
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    if (g_devfreqMinNode.empty()) detectDevfreqNode();
    if (g_devfreqMinNode.empty() || g_devfreqBoosted) return;
    // C-005: read the CURRENT min_freq baseline BEFORE raising it. We MUST be able
    // to restore the real stock floor on Off. If the baseline is unreadable
    // (open/read/parse fails -> savedMin stays 0), fail-closed and do NOT pin:
    // pinning without a known floor would later write 0 back on Off, silently
    // replacing the real stock min_freq floor with "no floor".
    unsigned long savedMin = 0;
    int fd = open(g_devfreqMinNode.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32] = {0};
        ssize_t r = read(fd, buf, sizeof(buf)-1);
        close(fd);
        if (r > 0) savedMin = strtoul(buf, nullptr, 10);
    }
    if (savedMin == 0) {
        // No usable baseline: do not pin, do not touch g_devfreqSavedMin (it may
        // still hold a previously-good value; we are simply not boosting now).
        LOG_WARN("devfreqBoostOn: min_freq baseline unreadable (savedMin=0) -> NOT pinning (fail-closed)");
        return;
    }
    // ROUND-014 P3: read max_freq from the same devfreq dir (not caller arg).
    unsigned long maxF = 0;
    std::string maxNode = g_devfreqMinNode.substr(0, g_devfreqMinNode.rfind('/') + 1) + "max_freq";
    fd = open(maxNode.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32] = {0};
        if (read(fd, buf, sizeof(buf)-1) > 0) maxF = strtoul(buf, nullptr, 10);
        close(fd);
    }
    if (maxF == 0) return;
    // T-02: do NOT pin min_freq=maxF. On Mali D1100 locking the top OPP is the
    // dominant heat source and fights the kernel's 94C throttle. Instead we
    // raise the floor to savedMin + 70%*(maxF-savedMin) — enough headroom to
    // avoid DVFS latency spikes in a GPU-pegged game, but 30% below the top
    // OPP. The floor is still fully released by devfreqBoostOff() on un-peg
    // or thermal tier drop.
    unsigned long targetF = savedMin + (maxF - savedMin) * (unsigned long)kDevfreqTargetPctOfRange / 100UL;
    if (targetF <= savedMin) targetF = savedMin + 1;  // sanity: must raise
    if (targetF > maxF) targetF = maxF;
    // Only now commit the VALIDATED baseline and pin min_freq=targetF. This guarantees
    // devfreqBoostOff() writes back the real stock floor (never 0).
    g_devfreqSavedMin = savedMin;
    fd = open(g_devfreqMinNode.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "%lu", targetF);
        if (write(fd, buf, n) == n) {
            g_devfreqBoosted = true;
            // GPUTOP-02: aggregate marker; do not unlink schedtune bit.
            markDevfreqActive(true, g_devfreqMinNode);
        }
        close(fd);
    }
}

void devfreqBoostOff() {
    // GPUTOP-01: hold g_devfreqMtx for the whole Off.
    std::lock_guard<std::mutex> lk(g_devfreqMtx);
    if (!g_devfreqBoosted) return;
    bool writeOk = false;
    bool readbackOk = false;

    // #16: write the saved min_freq back, validating that ALL bytes were written.
    // Previously the write() return value was discarded; a short write or EIO
    // left the hardware min_freq pinned at maxF while the state was reset to
    // "not boosted" — the next On would then read the stuck maxF as the new
    // savedMin, permanently corrupting the original floor.
    int fd = open(g_devfreqMinNode.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%lu", g_devfreqSavedMin);
        ssize_t w = write(fd, buf, (size_t)n);
        if (w == n) writeOk = true;
        close(fd);
    }

    // Readback verification: re-open and confirm the node now equals savedMin.
    // Some vendors' devfreq nodes accept the write but round/clamp it internally;
    // readback catches that case too.
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
        // GPUTOP-02: aggregate marker; do not unlink schedtune bit.
        markDevfreqActive(false, std::string());
    } else {
        // #16: release failed — keep the boosted state AND the marker so the
        // next tick retries. Do NOT touch g_devfreqSavedMin (would poison the
        // saved value with the currently-stuck maxF). The marker lets a reboot
        // recovery / WebUI see the boost is still armed.
        g_devfreqOffFails++;
        LOG_WARN("devfreqBoostOff: release failed (writeOk=%d readbackOk=%d, fail#%d) — "
                 "keeping boosted state for retry",
                 (int)writeOk, (int)readbackOk, g_devfreqOffFails);
    }
}
}  // namespace aatpe

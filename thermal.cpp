// AATPE - Thermal Management
// battery temp unit auto-detection (20-80 no div, 200-800 div10, >800 div1000), clamp [0,100]
// s_lastZoneTemp stale entry cleanup after each scan
// readThermal global state protected by g_thermalMutex
// comprehensive code comments added
// unified hysteresis + thermal pressure duration/slope/headroom +
//             separate skin/battery/CPU pressure modeling
#include "aatped.h"
#include <climits>
#include <deque>
// Shared CPU thermal-zone selector (W13/W14 red-team approved). PURE header,
// state carried in caller-owned history map. Included by BOTH this file and
// aatpe-watchdog.cpp so the two binaries derive the trusted CPU temperature
// from the SAME code path (see shared/cpu_zone_selector.h; Makefile adds -I shared).
#include "cpu_zone_selector.h"
// v4.3.6: temperature-trend feedforward predictor. The core (thermal_ff.h/.c)
// is a verified, logic-frozen PURE module (extern "C", no product dependency, no
// global state); thermal_ff_adapter.h is the host-testable wiring glue. We do
// NOT change the predictor logic -- only feed it each thermal sample and compose
// its additive gear with the existing reactive hard wall.
#include "thermal_ff/thermal_ff.h"
#include "thermal_ff/thermal_ff_adapter.h"
// C3 exploration: continuous headroom derating (default off).
#include "thermal_headroom.h"
using namespace aatpe;
using namespace aatpe_ml;

static time_t g_emergencyStartTime = 0;

// rel15 oscillation fix: rising-edge tracker for "we are in EMERGENCY".
// g_emergencyStartTime below used to be reset ONLY on the thermal self-detect
// path (the over-wall block). The watchdog/setRuntimeControlMode path flips
// SystemState to EMERGENCY between readThermal cycles without touching this
// private timestamp, leaving the OLD (or zero) value in place. The next recovery
// scan then saw `(now - g_emergencyStartTime) >= 30s` already true and bailed
// out of EMERGENCY ~6s after the watchdog forced it; the watchdog re-forced it,
// producing a 40-50s NORMAL->EMERGENCY->DEGRADED->NORMAL oscillation on real
// hardware. We now observe the EMERGENCY entry edge ourselves and reset the clock
// on it regardless of entry path.
static bool s_emergencyObserved = false;

// mutex protecting all global state modified by readThermal.
static std::mutex g_thermalMutex;

// ============================================================================
// Unified thermal hysteresis state machine.
//
// Each thermal level (WARNING / CRITICAL / EMERGENCY) uses a unified hysteresis
// model:
//   - entryThreshold: temperature at which we ENTER the level
//   - exitThreshold:  temperature at which we EXIT (entry - hysteresisDelta)
//   - minHoldSec:    minimum time to stay in the level once entered
//   - minRecoverySec: minimum time below exitThreshold before re-entering
//
// This prevents threshold thrashing when temperature hovers near the boundary.
// Previously each level had ad-hoc hysteresis (10°C for emergency, 5°C for
// profile, 3-read debounce). Now all levels share the same structural model.
// ============================================================================
struct ThermalLevelState {
    bool active = false;          // currently in this thermal level
    time_t enteredAt = 0;         // when we entered this level
    time_t exitedAt = 0;          // when we last exited (for recovery tracking)
    int streak = 0;               // consecutive reads above entry threshold
};

static ThermalLevelState s_warningState;    // WARNING level (profile switch)
static ThermalLevelState s_criticalState;   // CRITICAL level (migration pressure)

// ============================================================================
// v4.3.6: temperature-trend feedforward predictor state (thermal_ff).
//
// Caller-owned predictor state. Like s_thermalHistory below, this is touched
// ONLY from readThermal() on the single thermal sampling thread (the thermal
// mutex is released before that tail; documented single-writer domain) -- so no
// extra lock is needed, and the predictor core itself holds NO global/static
// state. Config thresholds are re-synced from the live (normalized) walls every
// tick so hot-reload / runtime wall edits are honoured; all other tunables keep
// the verified defaults.
//
// Composition contract (INTEGRATION.md §2.3): the predictor proposes an
// ADDITIVE pre-throttle gear; the actuator composes
//      final_gear = max(wall_gear, ff_gear)
// The EMERGENCY hard wall always wins. Feedforward may ease load EARLY (before a
// wall trips) but can never cancel or weaken a wall. On sensor fault the core
// yields ff_gear=NONE and all protection falls back to the reactive wall.
// ============================================================================
static ThermFfConfig s_thermFfCfg;
static ThermFfState s_thermFfState;
static bool s_thermFfReady = false;

// ROUND-009e: load% hook. Production samples /proc/stat aggregate cpu jiffies
// ROUND-009e: hermetic test root. Empty = production paths.
// RC7(C6): g_thermalTestRoot stays an always-compiled empty-by-default static
// (tSys() reads it on every sysfs path); only the test-only setter is guarded
// so the release binary carries no thermalSetTestRoot symbol.
static std::string g_thermalTestRoot;
#ifdef AATPE_HOST_TEST
void thermalSetTestRoot(const std::string& r) { g_thermalTestRoot = r; }
#endif

// audit5 N2: published thermal trip point in °C of the selected CPU die zone.
// 0 = not read / unknown. Read lock-free by the arbitrator each tick to feed
// device_profile_observe()'s trip EWMA. Defined here (thermal.cpp) so the
// producer owns it; the arbitrator forward-declares it extern.
std::atomic<int> g_thermalTripC{0};
static std::function<unsigned long long()> s_testClockFn;
#ifdef AATPE_HOST_TEST
void thermalSetClockForTest(std::function<unsigned long long()> fn) { s_testClockFn = std::move(fn); }
#endif
static std::string tSys(const std::string& sub) { return g_thermalTestRoot.empty() ? sub : (g_thermalTestRoot + sub); }

// THM-03: map a zoneRec name back to its real sysfs temp-node path.
//   "thermal_zoneN"      -> /sys/class/thermal/thermal_zoneN/temp
//   "hwmonN/tempM"      -> /sys/class/hwmon/hwmonN/tempM_input
// (Previously both were blindly prefixed /sys/class/thermal/<name>/temp, which
//  produced a bogus /sys/class/thermal/hwmon0/temp1/temp for hwmon-sourced CPU zones.)
static std::string zoneRecToTempPath(const std::string& recName) {
    if (recName.rfind("thermal_zone", 0) == 0) {
        return "/sys/class/thermal/" + recName + "/temp";
    }
    if (recName.rfind("hwmon", 0) == 0) {
        return "/sys/class/hwmon/" + recName + "_input";
    }
    return "/sys/class/thermal/" + recName + "/temp";
}

// THM-02: strip the temp-node suffix from a zone temp path to get the zone base
// directory (where trip_point_N_temp siblings live).
//   ".../thermal_zone4/temp"      -> ".../thermal_zone4"
//   ".../hwmon0/temp1_input"     -> ".../hwmon0"
static std::string zoneBaseFromTempPath(const std::string& tempPath) {
    if (tempPath.size() > 5 && tempPath.compare(tempPath.size()-5, 5, "/temp") == 0) {
        return tempPath.substr(0, tempPath.size()-5);
    }
    auto slash = tempPath.rfind('/');
    if (slash != std::string::npos) return tempPath.substr(0, slash);
    return tempPath;
}

// #19: is ANY online CPU cluster currently throttled (scaling_max_freq pinned below
// its hardware cpuinfo_max_freq)? The clamp-invalidation gate used to read ONLY cpu0
// (the LITTLE core). When thermal throttling hits the BIG/PRIME cluster via a
// per-cluster policy but leaves cpu0's max untouched, the gate missed the real
// throttle and — combined with a constant reading — falsely invalidated a healthy die
// zone. Now scan every CPU; any cluster whose policy max < hardware max counts.
// When topology is unknown (g_allCpus empty, e.g. early boot / tests) we keep the
// cpu0 fallback so single-cluster devices do not regress, but it is no longer the
// only thing we look at.
static bool anyCpuScalingMaxThrottled() {
    for (int cpuId : g_allCpus) {
        std::string base = tSys("/sys/devices/system/cpu/cpu" + std::to_string(cpuId));
        int online = readInt((base + "/online").c_str(), 1);
        if (online == 0) continue;  // offline core: not throttling, skip
        int smax = readInt((base + "/cpufreq/scaling_max_freq").c_str(), -1);
        int pmax = readInt((base + "/cpufreq/cpuinfo_max_freq").c_str(), -1);
        if (smax > 0 && pmax > 0 && smax < pmax) return true;
    }
    if (g_allCpus.empty()) {
        int a = readInt(tSys("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq").c_str(), -1);
        int b = readInt(tSys("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq").c_str(), -1);
        if (a > 0 && b > 0 && a < b) return true;
    }
    return false;
}

// #19: is ANY cpufreq cooling device actively throttling (cur_state > 0)? ...
static bool anyCpufreqCoolingActive() {
    DIR* dir = opendir(tSys("/sys/class/thermal").c_str());
    if (!dir) {
        int cs0 = readInt(tSys("/sys/class/thermal/cooling_device0/cur_state").c_str(), -1);
        return cs0 > 0;
    }
    bool any = false;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        std::string name = de->d_name;
        if (name.rfind("cooling_device", 0) != 0) continue;
        std::string base = tSys("/sys/class/thermal/") + name;
        std::string type = readFirstLine((base + "/type").c_str());
        // On a real device every cooling_device has a `type`. We credit only CPU-freq
        // nodes (a GPU/DRAM cooling ramp does not mean the CPU die is clamped). If the
        // type file is unreadable (minimal test env / early boot), fall back to
        // CREDITING: an active cur_state is better credited than missed, because this
        // gate exists to AVOID false-invalidating a thermally-stressed zone.
        bool credit = true;
        if (!type.empty()) {
            bool isCpu    = (type.find("cpufreq") != std::string::npos) ||
                            (type.find("cpu") != std::string::npos);
            bool isNonCpu = (type.find("gpu") != std::string::npos) ||
                            (type.find("ddr") != std::string::npos) ||
                            (type.find("dram") != std::string::npos);
            credit = isCpu && !isNonCpu;
        }
        int cs = readInt((base + "/cur_state").c_str(), -1);
        if (cs > 0 && credit) { any = true; break; }
    }
    closedir(dir);
    return any;
}

#ifdef AATPE_HOST_TEST
// Isolated test accessors (#19): expose the two evidence predicates so host tests can
// drive them against a fake sysfs tree without running the whole readThermal() loop.
namespace aatpe {
bool thermalAnyCpuScalingMaxThrottledForTest() { return anyCpuScalingMaxThrottled(); }
bool thermalAnyCpufreqCoolingActiveForTest() { return anyCpufreqCoolingActive(); }
// THM-03: expose zone-name -> sysfs temp path mapping for host tests.
std::string thermalZoneRecToTempPathForTest(const std::string& recName) { return zoneRecToTempPath(recName); }
// THM-02: expose temp-path -> zone-base derivation for host tests.
std::string thermalZoneBaseFromTempPathForTest(const std::string& p) { return zoneBaseFromTempPath(p); }
}  // namespace aatpe
#endif

// directly (self-contained, no cross-module coupling). Tests can override by
// linking their own symbol, or set the test root to a stub /proc/stat.
int thermalBusyPctHook() {
    static long s_prevBusy = -1, s_prevTotal = -1;
    FILE* f = fopen(tSys("/proc/stat").c_str(), "r");
    if (!f) return -1;  // R4 BLOCKER: EMFILE/missing node must NOT fscanf(NULL) SIGSEGV
    long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, sirq=0;
    if (fscanf(f, " cpu %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &system, &idle, &iowait, &irq, &sirq) != 7) {
        fclose(f); return -1;
    }
    fclose(f);
    long busy = user + nice + system + irq + sirq;
    long total = busy + idle + iowait;
    if (s_prevTotal >= 0) {
        long db = busy - s_prevBusy, dt = total - s_prevTotal;
        // ROUND-010: 32-bit jiffies wrap correction. On 32-bit kernels unsigned
        // long wraps; a negative delta means wrap-around.
        if (dt <= 0 || dt > 100000000L) {
            s_prevBusy = busy; s_prevTotal = total;
            return -1;
        }
        if (db < 0) db += (long)4294967296UL;  // 32-bit wrap
        if (db < 0 || db > dt) db = dt;
        s_prevBusy = busy; s_prevTotal = total;
        return (int)(db * 100 / dt);
    }
    s_prevBusy = busy; s_prevTotal = total;
    return -1;
}

// Temperature history for slope and duration calculation.
// Stores recent (timestamp, cpu_temp, skin_temp, battery_temp) samples.
struct ThermalSample {
    uint64_t timestampMs = 0;
    int cpuTemp = 0;
    int skinTemp = -1;
    int batteryTemp = 0;
};
// Thread-safety: s_thermalHistory is accessed ONLY from readThermal(), which is
// called exclusively from the daemon main loop. No other thread may push/pop
// samples. If a future refactor moves thermal history to a worker thread, add a
// dedicated mutex here.
static std::deque<ThermalSample> s_thermalHistory;
static constexpr size_t kMaxThermalHistorySamples = 30;  // ~30 seconds at 1s cache TTL

// ---------------------------------------------------------------------------
// 温控墙三档解析（WARNING / CRITICAL / EMERGENCY）。
// 优先使用新字段 thermalWarningThreshold / thermalCriticalThreshold /
// thermalEmergencyThreshold（默认值已在工作点上整体上调，见 aatped_types.h）；
// 若新字段 <= 0（例如单元测试只显式赋值旧字段），回退到旧字段，保证
// 既有 aatpe.conf 与测试行为不变。
// 单位：°C（readThermal() 已完成毫摄氏度/分摄氏度/摄氏度三级单位归一）。
// ---------------------------------------------------------------------------
static inline int effWarningThresholdC(const Config& c) {
    return c.thermalWarningThreshold > 0 ? c.thermalWarningThreshold
                                         : c.thermalProfileThreshold;
}
static inline int effCriticalThresholdC(const Config& c) {
    return c.thermalCriticalThreshold > 0 ? c.thermalCriticalThreshold
                                         : c.thermalLimitCpu;
}
static inline int effEmergencyThresholdC(const Config& c) {
    return c.thermalEmergencyThreshold > 0 ? c.thermalEmergencyThreshold
                                           : c.thermalEmergency;
}

// P2: Thermal read cache with 1-second TTL (real original)
// readThermal is called every daemon cycle (~50-200ms), but sysfs thermal
// zone reads are slow and values don't change that fast. Cache the last
// successful read and skip re-scanning if within TTL.
static unsigned long long g_thermalCacheLastMs = 0;
// migrated to aatpe-constants.h thermal_runtime::kCacheTtlMs

// Per-zone temperature jump-detection baseline and over-temp debounce streak.
// Hoisted from function-local statics (they were already `static` at function
// scope) to file scope so thermal_resetRuntimeStateForTest() can clear them
// between integration scenarios. Production behavior is unchanged.
static std::map<std::string, int> s_lastZoneTemp;
static int s_overTempStreak = 0;

// G6-001: thermal sensor liveness / fail-safe state.
// If a full scan yields ZERO valid temperature readings (all sysfs nodes
// unreadable / SELinux deny / driver unloaded), the subsystem must NOT treat
// that as "0°C = cold" and boost. We hold the last known good temperature and
// mark the data stale; if we never got a reading we degrade to a conservative
// (near-critical) policy so the scheduler never speeds up on bogus "cold".
static int s_lastGoodMaxTemp = -1;     // last plausibly-read maxTemp (-1 = never)
static int s_consecutiveReadFails = 0;
// P2-1: thermal-degraded reason flags. g_thermalDegraded used to be a plain bool
// written from TWO threads (sampling thread readThermal() ~:692/:705 and the main
// loop thermalFreshnessTick() ~:1634/:1640) and read by the ML freeze gate
// (~:1514) with no synchronization — a data race. It also conflated two
// independent reasons, and the freshness tick (which only proves the sampling
// LOOP is alive) used to clear the flag whenever the scan stamp was fresh,
// wiping the "no valid sensor reading" fail-safe within ~1s even though every
// sensor was still dead.
//
// We now keep TWO independent atomic reason bits; degraded is their OR:
//   - g_thDegNoValidSensor : set by the sampling thread when a scan yields ZERO
//                            plausible sensor readings; cleared the first scan
//                            that produces a valid reading. Owned by readThermal().
//   - g_thDegScanStuck     : set by the main loop when the scan stamp goes stale
//                            (>10s); cleared the moment the stamp is fresh again.
//                            Owned by thermalFreshnessTick().
// The freshness tick can now only CLEAR the SCAN_STUCK bit; it must never touch
// NO_VALID_SENSOR, so a sensor outage persists until sensors actually recover.
static std::atomic<bool> g_thDegNoValidSensor{false};
static std::atomic<bool> g_thDegScanStuck{false};
static inline bool thermalDegradedAny() {
    return g_thDegNoValidSensor.load(std::memory_order_acquire)
        || g_thDegScanStuck.load(std::memory_order_acquire);
}

// v4.2.3 P0: sysfs max CPU temp (°C), updated every scan. For ML freeze gate.
static std::atomic<int> g_sysfsCpuMaxTempC{0};
static std::atomic<bool> g_mlFreezeHysteresis{false};
// v4.2.4: authoritative freeze-decision reason (written every decision cycle).
static std::atomic<const char*> s_mlFreezeReason{"none"};
// R9终检: host-only seam (only tests/v94/test_thermal.cpp calls it). Was sitting
// unguarded between the earlier #endif and the #ifdef AATPE_HOST_TEST block below.
#ifdef AATPE_HOST_TEST
bool thermalDegradedForTest() { return thermalDegradedAny(); }
#endif
// G6-005c: frequency-read failure tracking. Unlike temperature (where a missing
// sensor must degrade conservatively to avoid boosting on bogus "cold"), a
// transient scaling_cur_freq read failure on a SINGLE online core must NOT be
// read as "throttled to 0" -- that would spuriously trigger thermal throttle.
// We skip that core (fail-neutral). Only when EVERY online core fails to read
// freq across consecutive rounds do we log a conservative UNKNOWN.
static int s_consecutiveFreqAllFails = 0;

// Test-only: wipe all thermal runtime caches/baselines so scenarios that
// inspect thermal (S4 emergency, S10 ramp) start from a clean slate. Does NOT
// touch g_cfg thresholds or the 120°C production default. Called by
// itResetToFreshState(); a no-op in production (never referenced on-device).
#ifdef AATPE_HOST_TEST
void thermal_resetRuntimeStateForTest() {
    std::lock_guard<std::mutex> lock(g_thermalMutex);
    s_lastZoneTemp.clear();
    s_overTempStreak = 0;
    g_thermalCacheLastMs = 0;
    g_emergencyStartTime = 0;
    s_emergencyObserved = false;
    // G6-001: reset fail-safe bookkeeping between test scenarios.
    s_lastGoodMaxTemp = -1;
    s_consecutiveReadFails = 0;
    g_thDegNoValidSensor.store(false, std::memory_order_release);
    g_thDegScanStuck.store(false, std::memory_order_release);
    s_consecutiveFreqAllFails = 0;
}

// rel15 oscillation fix: test seams for the EMERGENCY entry-edge clock reset.
// Production readThermal() calls the equivalent logic inline every scan; these
// expose it so host tests can prove BOTH emergency entry paths reset the clock
// without walking a fake sysfs tree.
#ifdef AATPE_HOST_TEST
// Read the current emergency-start timestamp (0 = never armed).
time_t thermalEmergencyStartTimeForTest() { return g_emergencyStartTime; }
// Force a specific (e.g. deliberately stale) start timestamp.
void thermalSetEmergencyStartTimeForTest(time_t t) { g_emergencyStartTime = t; }
// Drive the entry-edge observation for one scan. Returns true iff this scan was a
// rising edge (we just entered EMERGENCY) and therefore reset the clock to now.
bool thermalObserveEmergencyEdgeForTest(SystemState cur) {
    bool inEmergencyNow = (cur == SystemState::EMERGENCY);
    bool edge = inEmergencyNow && !s_emergencyObserved;
    if (edge) g_emergencyStartTime = time(nullptr);
    s_emergencyObserved = inEmergencyNow;
    return edge;
}
#endif
#endif

std::string aatpe_getThermalCpuZonePath() {
    std::lock_guard<std::mutex> lock(g_thermalMutex);
    return g_thermalCpuZonePath;
}

// ---------------------------------------------------------------------------
// HOTFIX v4.1.0: shared CPU-die zone classifier.
//
// The main scan and the no-CPU-zone fallback scanner must agree on what counts
// as a CPU/AP/SoC die sensor. Previously the fallback (thermal.cpp:~585) picked
// the FIRST readable zone by readdir order regardless of type, which on
// MT6891 selected thermal_zone26/mtktscharger (the CHARGER temperature) as the
// "CPU" source.
//
// D-规范缺口 (v4.3.4): thermal.cpp previously carried its OWN hand-rolled copy of
// the allow-list (mtktscpu/cpu/soc/cluster/tscpu/.../pm8) and denylist
// (charger/battery/pmic/lcd/wmt/conn/bms/quiet/tzimgs), while the shared
// selector (shared/cpu_zone_selector.h, used for the W13/W14 emergency/warning
// wall) maintained a SECOND list that had already started to drift
// (cpuss/cpu_/cpu- prefixes; skin/gpuss/gpu_ denylist; tzimg vs tzimgs). Two
// copies meant zoneRecs[].isCpu (driving cpuMaxTemp / thermal_zones WebUI) and
// the selector's own reclassification could disagree. We now delegate to the
// SINGLE implementation in thermzone::isCpuDieType() so the legacy cpuMaxTemp
// path and the selector always classify identically.
//
// typeLower MUST already be lower-cased by the caller.
// ---------------------------------------------------------------------------
static bool thermalIsCpuDieType(const std::string& typeLower) {
    return thermzone::isCpuDieType(typeLower);
}

// P3-1 (lesson #20): apply the emergency-recovery control-mode policy.
//
//  On EMERGENCY entry transitionToState() snapshots g_preEmergencyCfg. On recovery
//  we normally restore g_cfg.controlMode from that snapshot. But if the user
//  explicitly picked a mode WHILE the latch was active (the set_mode= channel set
//  g_emergencyUserOverride=true and wrote g_cfg.controlMode), that explicit choice
//  wins and must NOT be silently overwritten by the stale snapshot.
//
//  Extracted from readThermal() so the host test drives the real policy (not a
//  reimplementation). Returns the restored control mode.
std::string thermalApplyEmergencyModeRecovery() {
    std::string restoredControlMode;
    bool restoredAutoProfile = false;
    bool userOverride = g_emergencyUserOverride.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        if (userOverride) {
            // g_cfg.controlMode already == the user's in-emergency choice.
            restoredControlMode = g_cfg.controlMode;
        } else {
            g_cfg.controlMode = g_preEmergencyCfg.controlMode;
            restoredControlMode = g_preEmergencyCfg.controlMode;
        }
        g_cfg.autoProfile = g_preEmergencyAutoProfile;
        restoredAutoProfile = g_cfg.autoProfile;
    }
    if (userOverride) {
        g_emergencyUserOverride.store(false, std::memory_order_relaxed);
    }
    writeFileAtomic((std::string(STATE_DIR) + "/control_mode").c_str(), restoredControlMode.c_str());
    writeFileAtomic((std::string(STATE_DIR) + "/auto_profile").c_str(), restoredAutoProfile ? "1" : "0");
    if (getSystemState() != SystemState::EMERGENCY) {
        atomicWriteFile((std::string(STATE_DIR) + "/control_mode_effective").c_str(),
                        restoredControlMode.c_str());
    }
    return restoredControlMode;
}

// PURE emergency-exit predicate (rel15 oscillation fix). Extracted from the
// inline recovery block so host tests drive the exact debounce policy without a
// fake sysfs tree. Returns true iff it is safe to leave EMERGENCY for DEGRADED:
//   (1) we are actually in EMERGENCY;
//   (2) the current temperature has FALLEN below the exit wall
//       (emergencyWall - kEmergencyHysteresis); AND
//   (3) we have been CONTINUOUSLY in EMERGENCY for >= kEmergencyRecoverySec.
//
// The `secSinceEmergencyEntry` argument MUST be derived from an entry timestamp
// that is refreshed on EVERY emergency entry -- both the thermal self-detect path
// AND the external watchdog/setRuntimeControlMode path. Passing a stale private
// timestamp is exactly the 6-second early-exit defect this fixes. A pure function
// takes the elapsed seconds explicitly so the test can assert the gate without
// sleeping.
bool thermalEmergencyShouldExit(SystemState cur, int emergencyTempC,
                                int emergencyWallC, long secSinceEmergencyEntry) {
    if (cur != SystemState::EMERGENCY) return false;
    // Temperature has not dropped below the exit wall yet: hold.
    if (emergencyTempC >= emergencyWallC - thermal::kEmergencyHysteresis) return false;
    // Cooled, but not long enough yet: the 30s anti-flap debounce is not satisfied.
    if (secSinceEmergencyEntry < (long)kEmergencyRecoverySec) return false;
    return true;
}

void readThermal() {
    // thermalEmergency<=0时使用配置默认值120, 与thermal120基线一致
    // (旧v88硬编码105已废弃, 避免与THERMAL_EMERGENCY=120配置不一致)
    // take g_cfgMutex around the read-modify-write of
    // thermalEmergency (it is also written by loadConfig on hot-reload).
    //
    // === LOCK ORDER (see aatped.h GLOBAL LOCK ORDERING block) ===
    //   g_cfgMutex  -> g_thermalMutex  (cfg higher, never reverse)
    // This function:
    //   1. takes g_cfgMutex briefly for the thermalEmergency RMW, releases it.
    //   2. snapshots g_cfg to a local Config (cfgSnap) so all subsequent reads
    //      use the snapshot WITHOUT holding any lock.
    //   3. takes g_thermalMutex for the sysfs scan + thermal-state writes.
    //   4. RELEASES g_thermalMutex before the emergency/recovery/profile tail,
    //      because that tail calls transitionToState()/loadProfile() which take
    //      g_cfgMutex. Holding g_thermalMutex across those calls would reverse
    //      the documented order and deadlock.
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        if (g_cfg.thermalEmergency <= 0) {
            LOG_WARN("readThermal: thermalEmergency=%d (<=0), resetting to config default 120", g_cfg.thermalEmergency);
            g_cfg.thermalEmergency = 120;
        }
    }
    // thread-safe config snapshot. All g_cfg reads below use
    // cfgSnap instead of touching g_cfg under g_thermalMutex (which would be
    // racy against loadConfig() on the hot-reload thread).
    //
    // === CONTRACT (concurrency fix) ===
    // After this snapshotConfig(cfgSnap) call, the REST of this function MUST
    // NOT reference g_cfg.<field> directly. All config reads go through cfgSnap.
    // The only g_cfg references below are inside explicit g_cfgMutex critical
    // sections (the recovery block at ~line 364). No other direct g_cfg access
    // is allowed in readThermal().
    Config cfgSnap;
    snapshotConfig(cfgSnap);

    std::unique_lock<std::mutex> tLock(g_thermalMutex);
    // ROUND-009e: injectable clock for tests; production uses steady_clock.
    unsigned long long nowMs;
    if (s_testClockFn) nowMs = s_testClockFn();
    else nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_thermalCacheLastMs > 0 && (nowMs - g_thermalCacheLastMs) < aatpe::thermal_runtime::kCacheTtlMs) {
        return;
    }

    time_t now = time(nullptr);
    int maxTemp = 0, skinTemp = -1, batteryTemp = 0;
    bool throttle = false;
    bool foundSkinZone = false;
    // M1: battery sensor presence. False when NO power_supply node exposes a
    // readable temp; batteryTemp then stays -1 (unknown) and we publish an EMPTY
    // thermal_battery state file (mirrors the skin-unknown pattern at ~1345).
    // The in-memory g_thermalBattery atomic holds -1; scheduling gates on
    // `batteryTemp > 0` so -1/0 are both inert and scheduling behavior is unchanged.
    bool batterySensorKnown = false;
    int cpuMaxTemp = 0;
    bool foundCpuZone = false;
    // P3-c: track WHICH zone feeds primary/cpuMax (not just the value) so
    // stale-sensor revocation is by-name, not by-°C (ambiguous on multi-zone).
    static std::string s_primaryCpuZoneName;
    static std::string s_cpuMaxZoneName;
    // G6-001: count zones that produced a plausibly-normalized temperature.
    // Zero at end of scan => all sensors unreadable => fail-safe (see below).
    int validZoneReads = 0;
    // Bug2 (real-device on Redmi Note10 Pro / Dimensity 1100): the trusted CPU die
    // sensor is thermal_zone4, type "mtktscpu" (raw SoC ADC, in millicelsius).
    // The old code only classified CPU zones by loose substrings (cpu/soc/cluster/
    // adsp/cdsp/pm8...). On this device that loose set picked up a smoothed
    // mitigation proxy (~92°C) while the authoritative mtktscpu ADC (99.1°C) was
    // not reliably feeding the max — thermal_cpu_max read 92°C, 7°C below the real
    // die temperature. Fix: track mtktscpu explicitly as the PRIMARY CPU sensor and
    // guarantee its reading is included as the authoritative CPU temperature.
    int primaryCpuTemp = 0;
    bool foundPrimaryCpu = false;
    // ROUND-011: local selection path — published to g_thermalCpuZonePath only
    // AFTER the validity pass (constant-clamp / jump detection). Prevents
    // sticky selection of a zone that later gets marked invalid.
    std::string localPrimaryCpuPath;
    std::string localCpuFallbackPath;

    // Per-zone temperature jump detection.
    // If a zone's reading jumps >25°C from its last valid value, ignore it as
    // a likely sensor glitch.
    // kMaxTemp migrated to aatpe-constants.h thermal_runtime::kMaxTempJumpC
    // (s_lastZoneTemp hoisted to file scope for test reset).

    // track which zones are present in this scan so we can
    // clean up stale entries from s_lastZoneTemp afterward.
    std::set<std::string> currentZones;

    // ROUND-009d: per-zone record for variability check + thermal_zones file.
    // HOTFIX-B: tRawMilli holds the RAW reading normalized to millicelsius
    // (0.1°C nodes *100, °C nodes *1000). The constant/load validity windows
    // run on this m°C series, NOT on integer °C, so a real MTK die reporting
    // 50250..50980 m°C (always °C=50) is no longer falsely clamped-invalid.
    struct ZoneRec { std::string name, type; int t; int tRawMilli; bool invalid; bool isCpu; };
    std::vector<ZoneRec> zoneRecs;
    static std::map<std::string,int> s_prevScanTemp;  // zone -> last scan temp

    DIR* tzDir = opendir(tSys("/sys/class/thermal").c_str());
    if (tzDir) {
        struct dirent* e;
        while ((e = readdir(tzDir)) != nullptr) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            std::string zoneName = e->d_name;
            currentZones.insert(zoneName);  // v66 audit P2#15: track for cleanup

            std::string basePath = tSys("/sys/class/thermal/") + e->d_name;
            std::string typePath = basePath + "/type";
            std::string typeStr = readFile(typePath.c_str());
            std::string tempPath = basePath + "/temp";
            int tRaw = readInt(tempPath.c_str(), INT_MIN);
            if (tRaw == INT_MIN) continue;

            // intelligent unit conversion (three-tier)
            // P2-11: Boundary assumptions —
            //   |tRaw| > 1500  → millicelsius (m°C), divide by 1000
            //   |tRaw| >  150  → deci-Celsius (0.1°C),  divide by 10
            //   otherwise      → Celsius (°C),         use as-is
            // Boundary tRaw == 1500 falls into the second tier (/10 → 150°C),
            // which is the correct interpretation: a millicelsius sensor at
            // 150°C would report 150000 (>1500), not 1500. A value of exactly
            // 1500 from a deci-Celsius sensor means 150°C (extreme but possible
            // on some SoC sensors); the sanity check below rejects >150°C.
            // The (150, 1500] window is unambiguous: deci-Celsius values in
            // this range map to 15.1°C–150°C, all physically plausible.
            // P3-16: negative readings must go through the SAME unit judgment.
            // Previously a cold sensor reporting e.g. -5000 m°C (-5°C) hit the
            // bare `else` and stayed -5000, which then failed the t>150
            // sanity check (it didn't, but it also wasn't normalized). Use
            // abs() for the branch, preserving the sign on conversion.
            int t;
            int tRawMilli;   // HOTFIX-B: raw reading normalized to millicelsius
            int tAbs = tRaw < 0 ? -tRaw : tRaw;
            if (tAbs > 1500) {
                t = tRaw / 1000;  // millicelsius
                tRawMilli = tRaw;
            } else if (tAbs > 150) {
                t = tRaw / 10;    // 0.1°C
                tRawMilli = tRaw * 100;
            } else {
                t = tRaw;         // Celsius
                tRawMilli = tRaw * 1000;
            }
            // Sanity check: reject obviously bogus readings.
            // Upper bound 150°C (SoC thermal shutdown threshold).
            // Lower bound -50°C (matches the fallback zone scanner at line ~366;
            // a failed sensor returning e.g. -273°C after unit conversion must
            // not feed the state machine).
            if (t > 150 || t < -50) continue;

            // HOTFIX-A: boot-time / not-ready sensor reports 0 (|t|<1°C). Treat
            // as "sensor not ready": do NOT seed the jump baseline, do NOT enter
            // any validity window, and do NOT count it as a valid zone read.
            // Previously 0 was seeded as the first-frame baseline, so the later
            // real 46-50°C read looked like a >45°C jump and was rejected as a
            // glitch forever (WebUI stuck at 0°C for the first 3 minutes after
            // boot on MT6891).
            if (t > -1 && t < 1) {  // integer °C => t == 0
                continue;
            }

            // G6-001: this zone produced a valid, normalized reading.
            validZoneReads++;

            // Temperature jump detection — ignore transient sensor glitches
            // 跳变阈值从25°C提高到45°C, 游戏场景(combat/loading)放宽至50°C
            // 避免忽略游戏时合法的快速升温(如40°C→70°C)
            {
                auto lastIt = s_lastZoneTemp.find(zoneName);
                if (lastIt != s_lastZoneTemp.end() && lastIt->second > 0) {
                    int jump = abs(t - lastIt->second);
                    // game-scene-aware threshold — combat/loading场景放宽至50°C
                    int jumpThreshold = aatpe::thermal_runtime::kMaxTempJumpC;
                    if ((GameScene)g_currentScene.load(std::memory_order_acquire) == GameScene::COMBAT ||
                        (GameScene)g_currentScene.load(std::memory_order_acquire) == GameScene::LOADING) {
                        jumpThreshold = aatpe::thermal_runtime::kMaxTempJumpGameC;
                    }
                    if (jump > jumpThreshold) continue;  // ignore as glitch
                    s_lastZoneTemp[zoneName] = t;
                } else if (lastIt != s_lastZoneTemp.end()) {
                    // HOTFIX-A: previous baseline was a not-ready/<=0 sentinel
                    // (e.g. a boot-time 0 that slipped through). Do NOT treat the
                    // first plausible reading as a giant jump; accept it as the
                    // new baseline directly so the zone recovers instead of being
                    // rejected forever.
                    s_lastZoneTemp[zoneName] = t;
                } else {
                    // P2 : first-frame baseline seeding. A bogus-high first
                    // read (>100°C, e.g. an uninitialized ADC / 0xFFFF-ish sensor)
                    // must NOT become the baseline — otherwise every later
                    // legitimate read looks like a huge drop and gets rejected as
                    // a glitch forever (the zone never recovers). Only seed the
                    // baseline when the first reading is plausible (<=100°C).
                    // P3-19: previously this branch `continue`d, which also
                    // dropped the reading from maxTemp / skin / primary-cpu
                    // accumulation — so a legitimate >100°C boot-time die temp
                    // was invisible to the thermal state machine. Now we only
                    // skip baseline seeding; the reading still feeds maxTemp
                    // and the per-domain trackers below.
                    if (t <= 100) {
                        s_lastZoneTemp[zoneName] = t;
                    }
                }
            }

            // Track skin temp separately
            std::string typeLower = typeStr;
            // P3-17: std::transform must cast to unsigned char before tolower()
            // — passing a negative char (e.g. UTF-8 bytes >0x7F) is UB.
            std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            bool isSkinZone = (typeLower.find("skin") != std::string::npos ||
                               // HOTFIX-D: mtktsAP moved OUT of skin and INTO the
                               // secondary CPU-die set (thermalIsCpuDieType) below.
                               // It is the AP/SoC die temperature, not a skin probe.
                               // P3: "ap_therm" likewise denotes the AP/SoC die, NOT a
                               // skin probe -- it now routes to the CPU classifier below
                               // (isCpuDieType("ap_therm")==true) instead of being hijacked
                               // into skinTemp. Only a literal "skin" probe stays here.
                               typeLower.find("pmic_therm") != std::string::npos);
            if (isSkinZone) {
                foundSkinZone = true;
                if (t > skinTemp) skinTemp = t;
                continue;
            }
            // ROUND-009d: skip a zone whose reading equals ANY trip point.
            // Some MTK zones return a constant clamp (e.g. 92000=92°C) that is
            // neither live die temp nor trip_point_0. Compare trip_point_N for
            // N=0..10; if within 1°C, treat as threshold/clamp and skip.
            {
                bool isTrip = false;
                for (int tp = 0; tp <= 10; tp++) {
                    std::string tripPath = basePath + "/trip_point_" + std::to_string(tp) + "_temp";
                    int tripRaw = readInt(tripPath.c_str(), INT_MIN);
                    if (tripRaw == INT_MIN) continue;
                    int tripT = (tripRaw > 1500) ? tripRaw/1000 : (tripRaw > 150 ? tripRaw/10 : tripRaw);
                    if (abs(t - tripT) <= 1) { isTrip = true; break; }
                }
                if (isTrip) continue;
            }

            // Bug2: explicit primary CPU sensor. On MediaTek SoCs the raw die
            // temperature is exposed as type "mtktscpu". Match it by name BEFORE
            // the loose substring classification so it always feeds the result.
            bool isPrimaryCpuZone = (typeLower.find("mtktscpu") != std::string::npos);
            if (isPrimaryCpuZone) {
                foundPrimaryCpu = true;
                if (t > primaryCpuTemp) { primaryCpuTemp = t; s_primaryCpuZoneName = zoneName; }
                // ROUND-011: don't publish g_thermalCpuZonePath here — the zone
                // may be marked invalid later (constant clamp). Collect locally
                // and publish after the validity pass.
                localPrimaryCpuPath = tempPath;
            }
            // Check if this is a CPU-related zone.
            // HOTFIX-C: route BOTH the main scan and the fallback scanner through
            // the shared thermalIsCpuDieType() classifier (allow-list SoC die +
            // explicit denylist charger/battery/pmic/lcd/wmt/conn/bms/quiet/
            // tzimgs). mtktsAP/mtktspa are secondary CPU die zones here.
            bool isCpuZone = thermalIsCpuDieType(typeLower);
            if (isCpuZone) {
                foundCpuZone = true;
                if (t > cpuMaxTemp) { cpuMaxTemp = t; s_cpuMaxZoneName = zoneName; }
                // ROUND-011: record locally; publish after validity pass.
                if (localCpuFallbackPath.empty()) {
                    localCpuFallbackPath = tempPath;
                }
            }
            if (t > maxTemp) maxTemp = t;
            zoneRecs.push_back({zoneName, typeStr, t, tRawMilli, false, isCpuZone});
        }
        closedir(tzDir);
    }

    // ROUND-009e: hwmon candidates. Some MTK/Qualcomm expose CPU die temp only
    // under /sys/class/hwmon/hwmonN/temp*_input (millicelsius, name/label).
    // P2·hwmon-blind: previously only temp1_input was read. On devices whose
    // ONLY CPU-die temperature sits on a non-first channel (tempN_input, N>1)
    // the emergency wall was blind to the real die temp. Now iterate every
    // temp1..tempN_input in each hwmon dir (N bounded), classify by combining
    // the per-channel tempN_label with the hwmon /name through the SAME shared
    // thermzone::isCpuDieType() classifier used for thermal_zone nodes. A device
    // with only temp1 behaves exactly as before.
    {
        DIR* hw = opendir(tSys("/sys/class/hwmon").c_str());
        if (hw) {
            struct dirent* he;
            while ((he = readdir(hw)) != nullptr) {
                if (strncmp(he->d_name, "hwmon", 5) != 0) continue;
                std::string bp = tSys("/sys/class/hwmon/") + he->d_name;
                std::string name = readFile((bp + "/name").c_str());
                // tempN channels are typically temp1..temp10; bound the scan.
                for (int ch = 1; ch <= 10; ++ch) {
                    std::string inPath = bp + "/temp" + std::to_string(ch) + "_input";
                    int tRaw = readInt(inPath.c_str(), INT_MIN);
                    if (tRaw == INT_MIN) continue;   // channel absent -> stop-less, next dir
                    int t = (tRaw > 1500) ? tRaw/1000 : (tRaw > 150 ? tRaw/10 : tRaw);
                    // D-003: upper bound 115 -> 150 to match the main thermal_zone
                    // sanity window ([-50,150]). On devices whose ONLY CPU die temp
                    // source is a hwmon node, a reading of 116..120C (near the 120C
                    // emergency wall) was previously dropped, blinding the wall.
                    if (t < 15 || t > 150) continue;
                    // A5: this hwmon channel produced an in-range reading. The
                    // thermal_zone path increments validZoneReads at line ~509 but
                    // the hwmon path did not. On devices whose ONLY CPU die temp is a
                    // hwmon node (no readable thermal_zone), validZoneReads stayed 0
                    // and the line-775 fail-safe discarded this valid reading. Count
                    // it so the fail-safe only triggers when NO zone/hwmon read worked.
                    validZoneReads++;
                    int tAbsH = tRaw < 0 ? -tRaw : tRaw;
                    int tRawMilli = (tAbsH > 1500) ? tRaw : (tAbsH > 150 ? tRaw*100 : tRaw*1000);
                    // Per-channel label (e.g. "cpu-0-0-0", "soc", "pm8937_die").
                    std::string label = readFile((bp + "/temp" + std::to_string(ch) + "_label").c_str());
                    std::string combined = name + " " + label;
                    std::transform(combined.begin(), combined.end(), combined.begin(),
                                   [](unsigned char c){ return (char)std::tolower(c); });
                    bool isCpu = thermalIsCpuDieType(combined);
                    std::string recName = std::string(he->d_name) + "/temp" + std::to_string(ch);
                    zoneRecs.push_back({recName, combined, t, tRawMilli, false, isCpu});
                    if (isCpu && t > cpuMaxTemp) { foundCpuZone = true; cpuMaxTemp = t; }
                    if (t > maxTemp) maxTemp = t;
                }
            }
            closedir(hw);
        }
    }

    // ROUND-009d: variability double-sampling. A CPU candidate whose reading is
    // byte-identical to the previous scan while at least one NON-CPU zone moved
    // is a clamped constant (e.g. 92000). Mark invalid and exclude from CPU max.
    // HOTFIX-B: compare RAW millicelsius with a tight 50 m°C deadband instead of
    // integer °C equality. A real MTK die reporting 50250..50980 m°C always
    // rounds to °C=50 and was previously mistaken for a byte-identical clamp the
    // moment a non-CPU zone moved a whole degree. A truly stuck sensor has ~0
    // m°C movement, so the 50 m°C deadband keeps it detectable while letting real
    // m°C jitter through.
    {
        static constexpr int kClampMilliDeadband = 50;
        bool anyNonCpuMoved = false;
        for (auto& z : zoneRecs) {
            if (z.isCpu) continue;
            auto it = s_prevScanTemp.find(z.name);
            if (it != s_prevScanTemp.end() &&
                abs(it->second - z.tRawMilli) > kClampMilliDeadband) anyNonCpuMoved = true;
        }
        if (anyNonCpuMoved) {
            for (auto& z : zoneRecs) {
                if (!z.isCpu) continue;
                auto it = s_prevScanTemp.find(z.name);
                if (it != s_prevScanTemp.end() &&
                    abs(it->second - z.tRawMilli) <= kClampMilliDeadband) {
                    z.invalid = true;
                    // P3-c: revoke by zone NAME (not by °C value, which is
                    // ambiguous when multiple zones read the same integer °C).
                    if (z.name == s_primaryCpuZoneName) { foundPrimaryCpu = false; primaryCpuTemp = 0; }
                    if (z.name == s_cpuMaxZoneName) { foundCpuZone = false; cpuMaxTemp = 0; }
                }
            }
        }
        // Persist current scan (RAW m°C) for next round.
        s_prevScanTemp.clear();
        for (auto& z : zoneRecs) s_prevScanTemp[z.name] = z.tRawMilli;

        // ROUND-011: publish zone selection AFTER validity pass.
        // If the primary mtktscpu zone was marked invalid, fall back to the
        // highest-validity CPU zone. If no CPU zone is valid, clear the path.
        {
            std::string selectedPath;
            // Check if the primary path is still valid.
            bool primaryValid = !localPrimaryCpuPath.empty();
            if (primaryValid) {
                for (const auto& z : zoneRecs) {
                    if (z.name.find("mtktscpu") != std::string::npos && z.invalid) {
                        primaryValid = false;
                        break;
                    }
                }
            }
            if (primaryValid && foundPrimaryCpu && primaryCpuTemp > 0) {
                selectedPath = localPrimaryCpuPath;
            } else if (foundCpuZone && cpuMaxTemp > 0) {
                // Fallback: find the first valid CPU zone.
                for (const auto& z : zoneRecs) {
                    if (z.isCpu && !z.invalid) {
                        selectedPath = zoneRecToTempPath(z.name);  // THM-03: hwmon-aware
                        break;
                    }
                }
            }
            g_thermalCpuZonePath = selectedPath;
        }
        // Write thermal_zones: idx:type:temp:invalid separated by ';'.
        {
            std::string zf;
            for (size_t i = 0; i < zoneRecs.size(); i++) {
                if (i) zf += ";";
                zf += zoneRecs[i].name.substr(12) + ":" + zoneRecs[i].type + ":" +
                      std::to_string(zoneRecs[i].t) + ":" + (zoneRecs[i].invalid ? "1" : "0");
            }
            writeFileAtomic((std::string(STATE_DIR) + "/thermal_zones").c_str(), zf);
        }
    }

    // clean up stale zone entries no longer present.
    // Without this, s_lastZoneTemp grows unboundedly if zones are hotplugged
    // or the sysfs topology changes.
    for (auto it = s_lastZoneTemp.begin(); it != s_lastZoneTemp.end(); ) {
        if (currentZones.find(it->first) == currentZones.end()) {
            it = s_lastZoneTemp.erase(it);
        } else {
            ++it;
        }
    }

    // Bug2: authoritative CPU temperature. On MediaTek the primary "mtktscpu"
    // die sensor wins; take the max across the primary sensor and the loose CPU
    // set so we never under-report below the real die temperature. When no primary
    // sensor exists (non-MTK / different OEM), fall back to the historical loose
    // CPU-zone max, then to the all-zone max.
    // P3-18: previously this block OVERWROTE maxTemp (= primaryCpuTemp or
    // cpuMaxTemp), discarding any higher reading from non-CPU zones (GPU,
    // charger, PMIC, etc). Now we take max({maxTemp, primaryCpuTemp,
    // cpuMaxTemp}) so the hottest zone in the scan always wins.
    if (foundPrimaryCpu && primaryCpuTemp > 0) {
        maxTemp = std::max({maxTemp, primaryCpuTemp, cpuMaxTemp});
    } else if (foundCpuZone && cpuMaxTemp > 0) {
        maxTemp = std::max(maxTemp, cpuMaxTemp);
    }

    // G6-001: FAIL-SAFE on zero valid sensor reads.
    // If no zone produced a plausible temperature this scan, maxTemp would be 0
    // (= "cold") and the scheduler would boost on bogus data. Instead:
    //   - hold the last known good temperature while sensors recover;
    //   - if we never got a reading, degrade to a conservative near-critical
    //     policy (do NOT boost). inBootGrace suppresses a false EMERGENCY latch
    //     during the first 20s, so this is safe at boot.
    if (validZoneReads == 0) {
        s_consecutiveReadFails++;
        // P2-1: this is the NO_VALID_SENSOR reason, owned solely by the sampling
        // thread. It stays set for as long as every scan keeps producing zero
        // valid reads; the main-loop freshness tick cannot clear it.
        g_thDegNoValidSensor.store(true, std::memory_order_release);
        if (s_lastGoodMaxTemp >= 0) {
            maxTemp = s_lastGoodMaxTemp;
        } else {
            int consCritical = effCriticalThresholdC(cfgSnap);
            maxTemp = (consCritical > 0) ? consCritical : 90;
        }
        LOG_WARN("readThermal: zero valid thermal zone reads (%d consecutive), "
                 "fail-safe: holding conservative temp=%dC (no boost)",
                 s_consecutiveReadFails, maxTemp);
    } else {
        s_consecutiveReadFails = 0;
        s_lastGoodMaxTemp = maxTemp;
        // At least one plausible reading this scan -> NO_VALID_SENSOR cleared.
        // (The SCAN_STUCK reason is owned by the freshness tick and cleared there.)
        g_thDegNoValidSensor.store(false, std::memory_order_release);
    }

    // D-004: removed the premature `g_sysfsCpuMaxTempC.store(maxTemp, relaxed)`
    // that used to live here. It was a dead store: the shared CPU-zone selector
    // block below (~line 978) unconditionally re-stores g_sysfsCpuMaxTempC with
    // decisionTemp (the trusted high side) every scan, overwriting this value.
    // The only consumer is aatpeThermalMayFreezeTraining() (ML freeze gate),
    // which loads g_sysfsCpuMaxTempC under a `cpuTemp > 0` guard: before the
    // selector block runs on the very first scan it sees the atomic's initial 0
    // (= "not hot", the fail-safe direction), and on every later scan it sees the
    // previous scan's decisionTemp (one scan stale, never the all-zone maxTemp).
    // Neither window can falsely freeze ML; the authoritative store remains the
    // selector block's `g_sysfsCpuMaxTempC.store(decisionTemp, relaxed)`.

    // populate global thermal stats for state.json and polling fallback
    // LED-098 (R7-013): state.json's thermal.cpu must be the CPU DIE temperature,
    // not the hottest zone in the whole system. maxTemp above deliberately folds
    // in GPU/PMIC/charger zones (it drives the conservative emergency/headroom
    // decision), but publishing it as "cpu" makes WebUI show a misleadingly high
    // "CPU" temp. Pick the authoritative CPU sensor: primary mtktscpu wins, then
    // the loose CPU-zone max; only fall back to the all-zone max when no CPU-typed
    // zone was discovered at all.
    int cpuDieTemp;
    if (foundPrimaryCpu && primaryCpuTemp > 0)      cpuDieTemp = primaryCpuTemp;
    else if (foundCpuZone && cpuMaxTemp > 0)        cpuDieTemp = cpuMaxTemp;
    // HOTFIX-C: when no CPU/AP/SoC die zone produced a valid reading, report
    // -1 (unknown) instead of the all-zone max. The old code published e.g. the
    // charger/battery max as the "CPU" temperature, which on MT6891 made WebUI
    // show the charger temp (mtktscharger) as CPU. Never fake CPU temp from a
    // non-die sensor.
    else                                            cpuDieTemp = -1;
    g_thermalCpu.store(cpuDieTemp, std::memory_order_relaxed);
    // ROUND-009a: log which zone feeds the CPU temp (state aggregation in
    // aatped_state.cpp owns the state-file writes; no direct file IO here).
    LOG_INFO("readThermal: CPU die temp=%d°C source=%s", cpuDieTemp, g_thermalCpuZonePath.c_str());

    // audit5 N2: publish the trip point of the selected CPU die zone (°C).
    // Prefer trip_point_0_temp; fall back to trip_point_1/_2 when 0/unreadable.
    // Raw is millidegrees; normalise to whole °C. 0 stays published when no
    // trip is readable so the arbitrator treats it as "unknown" (device_profile
    // only learns trip when trip>0 && trip<150).
    if (!g_thermalCpuZonePath.empty()) {
        // THM-02: g_thermalCpuZonePath points at the *temp* node. trip_point_N_temp
        // is a SIBLING, not a child — derive the zone base first.
        std::string zoneBase = zoneBaseFromTempPath(g_thermalCpuZonePath);
        int tripC = 0;
        for (int tp = 0; tp <= 2 && tripC == 0; tp++) {
            std::string tripPath = zoneBase + "/trip_point_" +
                                   std::to_string(tp) + "_temp";
            int raw = readInt(tripPath.c_str(), 0);
            if (raw > 1000)            tripC = raw / 1000;        // millidegrees
            else if (raw > 150 && raw <= 1000) tripC = raw / 10;  // deci-degrees
            else if (raw > 0 && raw <= 150)    tripC = raw;       // already °C
        }
        g_thermalTripC.store(tripC, std::memory_order_relaxed);
    }

    // (问题10): If no CPU-typed zone found, scan ALL thermal zones and select a
    // readable CPU/AP/SoC die zone. HOTFIX-C: the selection is now TYPE-FILTERED
    // through the shared thermalIsCpuDieType() classifier. The old code picked the
    // first readable zone by readdir order regardless of type, which on MT6891
    // selected thermal_zone26/mtktscharger (charger temp as CPU temp). We now
    // explicitly reject charger/battery/bms/pmic/lcd/wmt/conn/quiet/tzimgs; if no
    // real die zone exists the path stays empty so callers report "temperature
    // unknown" and thermal policy is NOT driven on a fake sensor.
    if (g_thermalCpuZonePath.empty()) {
        DIR* fbDir = opendir(tSys("/sys/class/thermal").c_str());
        if (fbDir) {
            struct dirent* fe;
            while ((fe = readdir(fbDir)) != nullptr) {
                if (strncmp(fe->d_name, "thermal_zone", 12) != 0) continue;
                std::string fbBase = tSys("/sys/class/thermal/") + fe->d_name;
                // Classify by `type` BEFORE accepting the zone.
                std::string fbType = readFile((fbBase + "/type").c_str());
                std::string fbTypeL = fbType;
                std::transform(fbTypeL.begin(), fbTypeL.end(), fbTypeL.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (!thermalIsCpuDieType(fbTypeL)) continue;  // reject charger/battery/pmic/...
                std::string fbTempPath = fbBase + "/temp";
                int fbRaw = readInt(fbTempPath.c_str(), -999999);
                if (fbRaw <= -999999) continue;  // unreadable
                // Unit auto-detection (same three-tier as main scan)
                int fbT;
                if (fbRaw > 1500)      fbT = fbRaw / 1000;
                else if (fbRaw > 150)  fbT = fbRaw / 10;
                else                   fbT = fbRaw;
                // HOTFIX-A: also reject boot-time 0 / not-ready readings here.
                if (fbT >= -50 && fbT <= 150 && !(fbT > -1 && fbT < 1)) {
                    g_thermalCpuZonePath = fbTempPath;
                    LOG_WARN("readThermal: no primary CPU zone, fallback to %s (%s, temp=%d°C)",
                             fe->d_name, fbType.c_str(), fbT);
                    break;
                }
            }
            closedir(fbDir);
        }
        if (g_thermalCpuZonePath.empty()) {
            LOG_WARN("readThermal: no readable CPU/AP/SoC thermal zone — temperature unknown, thermal policy not based on fake data");
        }
    }

    // P2#8: removed hardcoded thermal_zone10~13 fallback
    // battery temp unit auto-detection with clamping.
    // Some drivers report in Celsius (20-80 range), others in 0.1°C (200-1500).
    // Raw value 20-80 → already Celsius, no division.
    // Raw value 200-1500 → 0.1°C, divide by 10.
    // Raw value >1500 → millicelsius, divide by 1000.
    // Result clamped to [0, 100] to reject bogus sensor readings.
    // THM-06: probe multiple power_supply supplies (battery/bms/battery/main)
    // instead of hardcoding "/sys/class/power_supply/battery/temp" — on devices
    // where the battery temp lives under "bms" the old code always read 0.
    {
        static const char* kBatterySupplies[] = {"battery", "bms", "battery/main"};
        int batteryRaw = INT_MIN;
        for (const char* sup : kBatterySupplies) {
            std::string p = tSys(std::string("/sys/class/power_supply/") + sup + "/temp");
            int v = readInt(p.c_str(), INT_MIN);
            if (v != INT_MIN) { batteryRaw = v; break; }
        }
        bool batteryKnown = (batteryRaw != INT_MIN);
        batterySensorKnown = batteryKnown;
        if (batteryRaw >= 200 && batteryRaw <= 1500) {
            batteryTemp = batteryRaw / 10;
        } else if (batteryRaw >= 20 && batteryRaw <= 80) {
            batteryTemp = batteryRaw;
        } else if (batteryRaw > 1500) {
            batteryTemp = batteryRaw / 1000;
        } else if (batteryKnown) {
            // Node exists but the raw value is outside every recognized unit
            // range. Treat as an untrusted reading: do NOT fabricate a 0°C truth,
            // mark the sensor unknown so the state file is published empty.
            batterySensorKnown = false;
            batteryTemp = -1;
        } else {
            // ROUND-010 / M1: no battery temp node at all -> unknown.
            // Previously the sentinel -1 was clamped to 0 right below, which
            // then got written to thermal_battery as a fake "0°C" and surfaced
            // in WebUI/system.json as a truthful 0. We now keep -1 in memory and
            // let the publish block write an EMPTY state file (see ~1350).
            batteryTemp = -1;
        }
        if (batteryTemp > 100) batteryTemp = 100;
    }

    // ROUND-009e: load-correlation sliding-window validity. No battery anchor
    // (battery temp itself may be a fake constant). Hard invalid = equals any trip
    // point, out of [15,115]C, or unreadable. Soft invalid: a CPU candidate whose
    // temp never moves while overall CPU load swung >=20 percentage points across
    // the ~30-sample window is a clamped constant. Window not yet filled = pending
    // (publish unavailable, not a fake number).
    // Test hook: returns current CPU busy% 0..100, or -1 unknown.
    extern int thermalBusyPctHook();
    {
        static std::map<std::string, std::deque<int>> s_zoneTempWin;
        static std::deque<int> s_busyWin;
        int busy = thermalBusyPctHook();
        if (busy >= 0) s_busyWin.push_back(busy);
        const size_t kW = 30;
        if (s_busyWin.size() > kW) s_busyWin.pop_front();
        int busyMin = 100, busyMax = 0;
        for (int b : s_busyWin) { busyMin = std::min(busyMin, b); busyMax = std::max(busyMax, b); }
        bool windowFull = (s_busyWin.size() >= kW);
        bool loadSwing = (windowFull && (busyMax - busyMin) >= 20);
        // Only enforce the load-correlation gate once we have a real busy series
        // (>=2 samples). Before that (boot, /proc/stat unreadable, unit tests) we
        // trust the zone reading so we never block the first valid temperature.
        bool gateActive = (s_busyWin.size() >= 2);

        if (gateActive) {
            for (auto& z : zoneRecs) {
                if (!z.isCpu) continue;
                auto& w = s_zoneTempWin[z.name];
                w.push_back(z.tRawMilli);  // HOTFIX-B: store RAW m°C, not integer °C
                if (w.size() > kW) w.pop_front();
                if (!windowFull) continue;  // still observing, don't block
                int rminMilli = INT_MAX, rmaxMilli = INT_MIN;
                for (int v : w) { rminMilli = std::min(rminMilli, v); rmaxMilli = std::max(rmaxMilli, v); }
                int rngMilli = rmaxMilli - rminMilli;
                // HOTFIX-B: thresholds are now on the RAW m°C range.
                //   rationale: a real MTK die reports millicelsius and always has
                //   some m°C thermal/ADC jitter (e.g. 50250..50980 = 730 m°C over
                //   60s). A clamped fake returns bit-identical samples (range 0) or
                //   near-identical (<50 m°C = 0.05°C, far below any real thermal
                //   response). loadClamp tolerates up to 1000 m°C (1°C): under a
                //   >=20pp CPU load swing a real die moves well over 1°C, so a
                //   sub-1°C range is a clamped sensor, not a live one.
                //   Old code used integer °C (tmax-tmin), which rounded 50250..50980
                //   to a flat 51°C and falsely marked a healthy die invalid.
                bool loadClamp = (loadSwing && rngMilli < 1000);
                bool constantClamp = (windowFull && rngMilli < 50);
                // ROUND-014 P3: three-evidence gate. Read actual sysfs with guards.
                // #19: scan ALL clusters / ALL cpufreq cooling devices, not just
                // cpu0 / cooling_device0 (see helpers above).
                bool scalingMaxThrottled = anyCpuScalingMaxThrottled();
                bool coolingActive = anyCpufreqCoolingActive();
                bool otherZoneVarying = false;
                for (auto& o : zoneRecs) {
                    if (&o == &z || !o.isCpu) continue;
                    auto& ow = s_zoneTempWin[o.name];
                    if (ow.size() >= 2 && abs(ow.back() - ow.front()) > 50) otherZoneVarying = true;
                }
                bool clampConfirmed = constantClamp && !scalingMaxThrottled &&
                                      !coolingActive && otherZoneVarying;
                if (loadClamp || clampConfirmed) {
                    z.invalid = true;
                    LOG_WARN("readThermal: zone %s temp constant %dC -- invalid",
                             z.name.c_str(), z.t);
                }
            }
            // Recompute CPU aggregates from VALID cpu zones only (equality-based
            // reset could wipe a second valid zone sharing the max).
            foundPrimaryCpu = false; primaryCpuTemp = 0;
            foundCpuZone = false; cpuMaxTemp = 0;
            for (auto& z : zoneRecs) {
                if (!z.isCpu || z.invalid) continue;
                foundCpuZone = true;
                if (z.t > cpuMaxTemp) cpuMaxTemp = z.t;
            }
            // primary = highest valid cpu zone (mtktscpu preferred; use max here).
            if (foundCpuZone) { foundPrimaryCpu = true; primaryCpuTemp = cpuMaxTemp; }
            // HOTFIX-C: re-resolve the PUBLISHED source path against the final
            // validity set. The earlier ROUND-011 selection ran BEFORE this
            // gate; when the gate invalidated the primary mtktscpu zone,
            // g_thermalCpuZonePath still pointed at the now-invalid zone4 (the
            // on-device symptom: thermal_source stuck on an invalid mtktscpu while
            // cpu temp already came from the secondary mtktsAP). Prefer a valid
            // mtktscpu, else the first valid CPU die zone, else empty.
            {
                std::string revised;
                for (const auto& z : zoneRecs) {
                    if (z.isCpu && !z.invalid &&
                        z.name.find("mtktscpu") != std::string::npos) {
                        revised = "/sys/class/thermal/" + z.name + "/temp";
                        break;
                    }
                }
                if (revised.empty()) {
                    for (const auto& z : zoneRecs) {
                        if (z.isCpu && !z.invalid) {
                            revised = zoneRecToTempPath(z.name);  // THM-03: hwmon-aware
                            break;
                        }
                    }
                }
                g_thermalCpuZonePath = revised;
            }
        }
        // Recompute published CPU die temp.
        if (foundPrimaryCpu && primaryCpuTemp > 0) cpuDieTemp = primaryCpuTemp;
        else if (foundCpuZone && cpuMaxTemp > 0)  cpuDieTemp = cpuMaxTemp;
        else                                      cpuDieTemp = -1;
        g_thermalCpu.store(cpuDieTemp, std::memory_order_relaxed);
        // ROUND-011: write thermal_source — the ACTUAL selected path (may be
        // empty if no valid CPU zone). Previously gated on foundPrimaryCpu,
        // which would write empty even when a valid fallback zone was selected.
        writeFileAtomic((std::string(STATE_DIR) + "/thermal_source").c_str(),
                        g_thermalCpuZonePath);
    }

    // ========================================================================
    // W13/W14: shared CPU thermal-zone selector (shared/cpu_zone_selector.h).
    //
    // Root cause on Dimensity 1200 (Redmi K40 gaming): an intermittent/rogue CPU
    // zone (mtktscpu reads -1 for a scan or two, then drifts/spikes to ~87C) was
    // folded straight into maxTemp and triggered a FALSE 80C WARNING throttle.
    // The selector judges a zone ROGUE only when BOTH hold:
    //   (1) time-series defect  -- badStreak (consecutive invalid/-1) OR
    //                             spikeStreak (unfollowed single-point jumps); AND
    //   (2) statistical divergence -- deviates >12C from the CPU lower-median.
    // A clean, smoothly-ramping big core (85C vs LITTLE 65C) is NEVER rogue.
    //
    // Two temperatures emerge:
    //   trustedCpuTempC = max over NON-rogue valid CPU zones (robust high side).
    //                     This drives WARNING / profile / headroom / the published
    //                     CPU max, so a real 85C big core still triggers WARNING but
    //                     a rogue board sensor cannot.
    //   anyCpuTempC     = max over ALL valid CPU zones (rogue included).
    //                     This drives the EMERGENCY hard wall, so ANY CPU zone that
    //                     truly reaches >=110C still latches -- the rogue filter can
    //                     never swallow a real overheat.
    //
    // Fallback: if the selector found NO CPU-typed zone this scan, both drivers fall
    // back to the legacy all-zone maxTemp so non-target devices keep old behavior.
    int decisionTemp  = maxTemp;   // WARNING / profile / headroom / published CPU max
    int emergencyTemp = maxTemp;   // EMERGENCY hard wall (sees every CPU zone)
    {
        std::vector<thermzone::ZoneReading> tz;
        tz.reserve(zoneRecs.size());
        for (const auto& z : zoneRecs) {
            std::string tl = z.type;
            std::transform(tl.begin(), tl.end(), tl.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            thermzone::ZoneReading r;
            r.name      = z.name;
            r.typeLower = tl;
            bool plausible = (z.t > -50 && z.t < 150);
            r.valid    = (!z.invalid && plausible);
            r.tempC    = r.valid ? z.t : INT_MIN;   // INT_MIN => unreadable this scan
            tz.push_back(r);
        }
        // Cross-scan history (badStreak/spikeStreak/goodStreak) lives here.
        static std::map<std::string, thermzone::ZoneHist> s_tzHist;
        thermzone::SelectResult sel = thermzone::select(tz, s_tzHist);
        if (sel.anyCpuTempC > 0) {
            decisionTemp  = sel.trustedCpuTempC;
            emergencyTemp = sel.anyCpuTempC;
        }
        // ML freeze gate / published CPU max must follow the trusted high side too,
        // or a rogue spike would falsely freeze ML the same way it falsely throttled.
        g_sysfsCpuMaxTempC.store(decisionTemp, std::memory_order_relaxed);
        if (cfgSnap.logThermal) {
            std::string rn;
            for (const auto& n : sel.rogueNames) { if (!rn.empty()) rn += ","; rn += n; }
            LOG_INFO("readThermal: tz trusted=%dC anyCpu=%dC rogue=[%s] cpuZones=%zu",
                     decisionTemp, emergencyTemp, rn.c_str(), sel.cpuNames.size());
        }
    }

    // Check CPU frequency throttling.
    // G6-002: offline (hotplugged-out) cores report scaling_cur_freq==0 on many
    // kernels while scaling_max_freq persists. That must NOT be read as "throttled".
    // Read /online first and skip offline cores entirely.
    // G6-005c: fail-neutral on read failure. readInt returns -1 on failure; a
    // single online core whose cur/max freq node is unreadable is SKIPPED (it
    // must not produce throttle=true). Only a confirmed (cur/max) ratio below the
    // threshold sets throttle. If EVERY online core fails this scan, increment a
    // consecutive counter and LOG_WARN -- we still do NOT force throttle=true on
    // unknown data (frequency transients must not cause 误伤).
    int onlineCores = 0, freqReadOk = 0;
    for (int cpuId : g_allCpus) {
        // HOTFIX-E: route through tSys() so hermetic tests (and the production
        // test-root hook) control these nodes; production paths are unchanged.
        std::string base = tSys("/sys/devices/system/cpu/cpu" + std::to_string(cpuId));
        std::string onlinePath = base + "/online";
        int onlineVal = readInt(onlinePath.c_str(), 1);
        if (onlineVal == 0) continue;  // offline core: not throttling, skip
        onlineCores++;
        int cur = readInt((base + "/cpufreq/scaling_cur_freq").c_str(), -1);
        int maxf = readInt((base + "/cpufreq/scaling_max_freq").c_str(), -1);
        if (cur < 0 || maxf <= 0) {
            LOG_WARN("readThermal: CPU%d freq read failed (cur=%d max=%d) -- skipping as throttle evidence",
                     cpuId, cur, maxf);
            continue;  // fail-neutral: do not count as throttled
        }
        freqReadOk++;
        if (cur * 100 / maxf < thermal::kThrottleFreqPct) throttle = true;
    }
    if (onlineCores > 0 && freqReadOk == 0) {
        s_consecutiveFreqAllFails++;
        LOG_WARN("readThermal: all %d online cores freq-unreadable (%d consecutive) -- UNKNOWN, not forcing throttle",
                 onlineCores, s_consecutiveFreqAllFails);
    } else {
        s_consecutiveFreqAllFails = 0;
    }

    // HOTFIX-E: throttle evidence UNION. thermal_throttle is an informational key
    // consumed by WebUI/CLI/diag. It must read 1 ONLY when there is actual
    // throttling evidence:
    //   (1) a CPU online core running < kThrottleFreqPct of its policy max (above),
    //   (2) an active cpufreq cooling device (any cooling_device*/cur_state > 0), or
    //   (3) a GPU devfreq channel running <80% of its max_freq.
    // `throttle` is a fresh local every cache-miss scan (there is NO cross-scan
    // latch / TTL sticky flag), so the moment none of these fire the key is
    // rewritten to 0 on the next scan -- no stale "1" residue. This resolves the
    // on-device contradiction where thermal_throttle=1 while throttle_state
    // cpu_throttled=0 / gpu_throttled=0.
    // #19: use the all-cooling-device scan (not just cooling_device0).
    if (anyCpufreqCoolingActive()) throttle = true;
    {
        DIR* dv = opendir(tSys("/sys/class/devfreq").c_str());
        if (dv) {
            struct dirent* de;
            while ((de = readdir(dv)) != nullptr) {
                if (strncmp(de->d_name, "devfreq", 7) != 0) continue;
                std::string dp = tSys("/sys/class/devfreq/") + de->d_name;
                std::string nm = readFile((dp + "/name").c_str());
                if (nm.find("gpu") == std::string::npos) continue;  // only GPU channels
                int gc = readInt((dp + "/cur_freq").c_str(), -1);
                int gm = readInt((dp + "/max_freq").c_str(), -1);
                if (gc > 0 && gm > 0 && gc * 100 / gm < 80) { throttle = true; break; }
            }
            closedir(dv);
        }
    }

    // headroom 以 CRITICAL 档（thermalCriticalThreshold）为基准，
    // 旧字段 thermalLimitCpu 作为 fallback（新字段<=0 时）。
    int criticalWall = effCriticalThresholdC(cfgSnap);
    // G6-003/G6-004: runtime safety normalization.
    //  (a) EMERGENCY is a hard global ceiling: no source (main config up to 200,
    //      stale profile) may raise it above 120°C. Clamp here defensively because
    //      validateConfig() is not invoked on the hot-reload path (G2 owns wiring).
    //  (b) Enforce monotonicity WARNING < CRITICAL < EMERGENCY regardless of how
    //      the unvalidated config arrives, so the state machine never inverts.
    int warningWallRaw   = effWarningThresholdC(cfgSnap);
    int emergencyWallRaw = effEmergencyThresholdC(cfgSnap);
    const int kEmergencyHardCeilingC = 120;
    if (emergencyWallRaw > kEmergencyHardCeilingC) emergencyWallRaw = kEmergencyHardCeilingC;
    if (criticalWall > emergencyWallRaw) criticalWall = emergencyWallRaw - 1;
    if (warningWallRaw >= criticalWall)   warningWallRaw = criticalWall - 1;
    // Recompute effective walls used below from the normalized values by stashing
    // them into locals the rest of the function references.
    int warningWall    = warningWallRaw;
    int emergencyWall   = emergencyWallRaw;

    // Headroom is measured against the TRUSTED CPU temperature (robust high side),
    // not the all-zone maxTemp -- a rogue sensor must not collapse headroom and
    // trigger protective derating the way it falsely triggered WARNING before.
    int headroom = criticalWall - decisionTemp;
    if (headroom < 0) headroom = 0;
    int headroomPct = criticalWall > 0 ? headroom * 100 / criticalWall : 0;

    g_thermalCacheLastMs = nowMs;
    // Published CPU max follows the trusted high side (matches g_sysfsCpuMaxTempC
    // re-stored in the selector block above).
    writeFile((std::string(STATE_DIR) + "/thermal_cpu_max").c_str(), decisionTemp);
    // Real-device bug: when no thermal zone type matched "skin"/"pmic_therm"
    // (MTK names the skin sensor differently, and mtktsAP/ap_therm are deliberately
    // classified as CPU die), foundSkinZone was false and we wrote the raw sentinel
    // "-1" to thermal_skin. The WebUI numeric state reader parses "-1" as a valid
    // integer and rendered a bogus "-1°C". On unavailable we now write an EMPTY
    // state file: the reader renders empty/missing as "--" (readIntFile -> null).
    // g_thermalSkin stays -1 for the in-memory thermal state machine + host test.
    if (foundSkinZone) {
        writeFile((std::string(STATE_DIR) + "/thermal_skin").c_str(), skinTemp);
    } else {
        writeFile((std::string(STATE_DIR) + "/thermal_skin").c_str(), "");
    }
    // M1: battery unknown (no readable node, or node value outside every unit
    // range) publishes an EMPTY state file -- the WebUI numeric reader renders
    // empty/missing as "--" and webui_json.cpp emits JSON null. g_thermalBattery
    // below stores -1 so the in-memory state machine sees "unknown" (the
    // `batteryTemp > 0` scheduling gate treats -1 and 0 identically as inert).
    if (batterySensorKnown) {
        writeFile((std::string(STATE_DIR) + "/thermal_battery").c_str(), batteryTemp);
    } else {
        writeFile((std::string(STATE_DIR) + "/thermal_battery").c_str(), "");
    }
    // populate global thermal stats (previously always 0 in state.json)
    g_thermalSkin.store(foundSkinZone ? skinTemp : -1, std::memory_order_relaxed);
    g_thermalBattery.store(batterySensorKnown ? batteryTemp : -1, std::memory_order_relaxed);
    writeFile((std::string(STATE_DIR) + "/thermal_headroom").c_str(), headroom);
    writeFile((std::string(STATE_DIR) + "/thermal_headroom_pct").c_str(), headroomPct);
    // C1/C3 exploration: promote headroomPct to in-memory atomic so C1 thermal
    // gate and C3 continuous derating can read it without re-reading state files.
    // This write is unconditional (cheap, relaxed); the CONSUMERS gate on
    // preboostEnabled / thermalSmoothEnabled (default false) to preserve v4.4.0.
    g_thermalHeadroomPct.store(headroomPct, std::memory_order_relaxed);
    writeFile((std::string(STATE_DIR) + "/thermal_throttle").c_str(), throttle ? 1 : 0);
    // G6-001: expose sensor liveness so WebUI/operator can see fail-safe state.
    // THM-12: thermal_degraded is written exclusively by thermalFreshnessTick()
    // (main loop), which owns the file and reads the atomic bits below. The
    // sampling thread only updates g_thDegNoValidSensor / g_thDegScanStuck here.
    // thermal_emergency is STRONG-CONSISTENCY — watchdog
    // and WebUI read it to trigger emergency behavior. Atomic write prevents
    // torn reads during the 0<->1 transition.
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_emergency").c_str(), g_thermalEmergency ? "1" : "0");

    // Phase3 Batch1: Thermal emergency now drives the unified state machine.
    // EMERGENCY state handles saving pre-emergency config, setting control_mode,
    // and g_thermalEmergency flag internally via transitionToState().
    // Recovery goes to DEGRADED (not NORMAL) per state machine spec, requiring
    // temp < (emergency - hysteresis) for >= 30 seconds.
    // EMERGENCY 进入防抖 + 启动宽限。
    // 根因: 开机首次 readThermal() 时 s_lastZoneTemp 为空, fix31 跳变过滤对首帧
    // 不生效, 传感器刚初始化上报的陈旧/毛刺高温(如 110°C)被直接采纳并立刻 latch
    // 成 EMERGENCY → controlMode=passive; 且该毛刺值随后成为跳变基线, 把真实 ~38°C
    // 当成 >45°C 跳变过滤掉(见下方 jump check), 高温假象持续、recovery 永不触发,
    // control_mode 卡死在 passive 需手动改回(表现为"重启后偶尔抽风到省电")。
    // 修复: (1) daemon 启动后 kEmergencyBootGraceSec 内不响应高温触发(传感器未稳定);
    //       (2) 高温需连续 kEmergencyDebounceReads 次才真正进入 EMERGENCY, 滤掉毛刺。
    // (s_overTempStreak hoisted to file scope at top of thermal.cpp for test reset.)
    bool inBootGrace = ((now - g_startTime.load()) < thermal_runtime::kEmergencyBootGraceSec);

    // RELEASE g_thermalMutex before this tail.
    // All thermal-protected state (g_thermalCpu/Skin/Battery, g_thermalCpuZonePath,
    // g_thermalCacheLastMs, s_lastZoneTemp) has been written above under the lock.
    // The tail below calls transitionToState() and loadProfile(), both of which
    // take g_cfgMutex. Keeping g_thermalMutex held across those calls would reverse
    // the documented lock order (g_cfgMutex -> g_thermalMutex) and deadlock against
    // a concurrent loadConfig() that holds g_cfgMutex and wants g_thermalMutex
    // (via aatpe_getThermalCpuZonePath() or the next readThermal() iteration).
    tLock.unlock();

    // === rel15 oscillation fix: EMERGENCY entry-edge clock reset ===
    // Observe whether we are in EMERGENCY on this scan. The first scan that sees
    // EMERGENCY after a non-EMERGENCY scan is the entry edge -- regardless of who
    // flipped the state:
    //   * thermal self-detect (block below) also sets g_emergencyStartTime=now on
    //     the commit cycle;
    //   * the watchdog / setRuntimeControlMode("emergency") path flips
    //     SystemState to EMERGENCY between readThermal cycles and does NOT know
    //     about this private timestamp. This edge detector covers that path: on
    //     the next scan after an external entry, we reset the clock to now so the
    //     30s recovery debounce starts from the REAL entry instead of a stale
    //     value (which made recovery fire after 6s and oscillate).
    {
        bool inEmergencyNow = (getSystemState() == SystemState::EMERGENCY);
        if (inEmergencyNow && !s_emergencyObserved) {
            g_emergencyStartTime = now;
        }
        s_emergencyObserved = inEmergencyNow;
    }

    // EMERGENCY 触发阈值：normalized above (warningWall/emergencyWall) —
    // G6-005 corrected comment: default 120°C (thermalEmergencyThreshold),
    // hard ceiling 120 enforced at line ~489. emergencyWall already in scope.
    // SAFETY RED LINE: the hard wall compares emergencyTemp = anyCpuTempC = max over
    // ALL valid CPU zones (rogue included). The selector's rogue filtering never
    // feeds this comparison, so ANY CPU zone truly reaching the wall still latches;
    // real overheat can never be swallowed. (Only WARNING/profile/headroom use the
    // trusted, rogue-filtered temperature.)
    // v6.0.0 round5b P2: emergency hard wall is FAIL-SAFE — this predicate takes
    // NO thermalEnabled input, so thermalEnabled=0 only disables regular thermal
    // scheduling, never the 120°C die wall. emergencyWall already normalized above.
    if (aatpeThermalEmergencyHardWall(emergencyTemp, emergencyWall)) {
        s_overTempStreak++;
        if (getSystemState() != SystemState::EMERGENCY &&
            !inBootGrace &&
            s_overTempStreak >= thermal_runtime::kEmergencyDebounceReads) {
            g_emergencyStartTime = now;
            transitionToState(SystemState::EMERGENCY, "thermal_emergency");
        }
    } else {
        s_overTempStreak = 0;
    }
    // G6-006: re-emit thermal_emergency AFTER transitionToState() may have flipped
    // g_thermalEmergency, so the state file reflects this cycle's transition
    // immediately instead of lagging one ~1s cycle.
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_emergency").c_str(),
                    g_thermalEmergency ? "1" : "0");
    // EMERGENCY 温度回落后恢复。必须同时满足：(1) 温度已落到 exit wall 以下
    // (emergencyWall - kEmergencyHysteresis)；(2) 已在 EMERGENCY 持续冷却满
    // kEmergencyRecoverySec(30s)。二者缺一不可——只看温度会 6 秒抖动，只看时间
    // 会在高温下提前退出。secSinceEmergencyEntry 来自 g_emergencyStartTime，它在
    // 上面的 entry-edge 观测里被任意一条进入路径重置，不再是陈旧值。
    long secSinceEmergencyEntry = (g_emergencyStartTime > 0)
                                      ? (long)(now - g_emergencyStartTime) : 0;
    if (thermalEmergencyShouldExit(getSystemState(), emergencyTemp, emergencyWall,
                                    secSinceEmergencyEntry)) {
        // Recover to DEGRADED (not NORMAL) — system may still be warm
        transitionToState(SystemState::DEGRADED, "thermal_recovery_30s");
        thermalApplyEmergencyModeRecovery();
    }

    // Thermal profile auto-switch —— 临时保护, 温度恢复后自动切回用户 profile。
    // 原逻辑只在 maxTemp>=thermalProfileThreshold 时 loadProfile
    // ("thermal"), 温度降回后没有任何切回代码, 一旦触发就永久停在 thermal 档,
    // 需手动改回游戏。现在:
    //   - 切到 thermal 前把当前(用户)profile 存进 s_preThermalAutoProfile;
    //   - 温度回落到 (threshold - hysteresis) 后自动恢复该 profile;
    //
    // P2 Problem15 — EXPLICIT OVERRIDE PRIORITY (highest first):
    //   1. EMERGENCY (thermal_emergency latch): forces control_mode=passive via
    //      transitionToState(EMERGENCY) and saves g_preEmergencyCfg. The guard
    //      below (!g_thermalEmergency) ensures thermal auto-switch CANNOT run while
    //      EMERGENCY is active. On recovery, the user's pre-emergency control_mode
    //      and autoProfile are restored (see the recovery block above).
    //   2. Thermal profile auto-switch (this block): only switches PROFILE (not
    //      control_mode) to "thermal" as a temporary protection.
    //   3. Manual user selection (set_profile=<name>): sets g_profileLock=true and
    //      autoProfile=0. The guard below (!g_profileLock) means a manually locked
    //      profile is NEVER auto-overridden by thermal. (Design choice: explicit
    //      manual pick wins; thermal auto-protection only applies when the user has
    //      opted into AUTO_PROFILE.)
    //   4. Auto profile (g_cfg.autoProfile=1): the auto layer this block implements.
    //   5. Profile default (the base profile loaded at start).
    // Result: outside EMERGENCY, the user's manual control_mode/profile is preserved;
    // EMERGENCY is the only force that overrides it, and it restores on recovery.
    // the thermal warning layer now
    // VOTES for "thermal" instead of saving/restoring a historical profile string.
    // Over warningWall  => profileRequest("thermal", THERMAL).
    // Cooled below hysteresis => profileRelease(THERMAL), after which the arbiter
    // RE-COMPUTES the effective profile from the live sources (current foreground
    // app / APP override, manual lock, AUTO, GLOBAL) instead of blindly loading a
    // stale saved name. This fixes "thermal recovery restored gaming even though the
    // user already left App A".
    // WARNING 档：default 80°C (thermalWarningThreshold), normalized above
    // (warningWall already in scope; G6-005 corrected stale 84 comment).
    // Unified hysteresis — WARNING level uses the shared hysteresis
    // model: entry at warningWall, exit at warningWall - kHysteresisDeltaC,
    // with min hold time (kMinHoldSec) and min recovery time (kMinRecoverySec).
    int warningExitWall = warningWall - thermal_runtime::kHysteresisDeltaC;
    if (cfgSnap.autoProfile && !g_thermalEmergency && !g_profileLock.load()) {
        // WARNING/profile uses decisionTemp = trustedCpuTempC (robust high side over
        // NON-rogue CPU zones). A real 85C big core still enters WARNING; a rogue
        // board sensor spiking to 87C does not.
        if (decisionTemp >= warningWall) {
            // Entry: need streak of reads above threshold (debounce)
            s_warningState.streak++;
            if (!s_warningState.active && s_warningState.streak >= 2) {
                // Enter WARNING level
                s_warningState.active = true;
                s_warningState.enteredAt = now;
                aatpe_profile::profileRequest("thermal", aatpe_profile::ProfileSource::THERMAL);
                if (cfgSnap.logThermal) {
                    LOG_INFO("Thermal WARNING: temp=%dC >= %dC, entering thermal profile",
                             decisionTemp, warningWall);
                }
            }
        } else if (decisionTemp <= warningExitWall) {
            s_warningState.streak = 0;
            // Exit: must have been in WARNING for at least kMinHoldSec,
            // and temp must stay below exit wall for kMinRecoverySec before re-entry.
            if (s_warningState.active) {
                bool minHoldMet = (now - s_warningState.enteredAt) >= thermal_runtime::kMinHoldSec;
                if (minHoldMet) {
                    s_warningState.active = false;
                    s_warningState.exitedAt = now;
                    if (cfgSnap.logThermal) {
                        LOG_INFO("Thermal cooled down below warning exit wall (%dC); "
                                 "releasing THERMAL vote, arbiter will re-pick effective profile",
                                 warningExitWall);
                    }
                    aatpe_profile::profileRelease(aatpe_profile::ProfileSource::THERMAL);
                }
            }
        } else {
            // In hysteresis band: between exit wall and entry wall.
            // Keep current state (don't toggle). Reset streak since we're not
            // clearly above entry.
            s_warningState.streak = 0;
        }
    } else {
        // autoProfile disabled or emergency/profile lock: reset warning state
        if (s_warningState.active) {
            s_warningState.active = false;
            s_warningState.exitedAt = now;
            aatpe_profile::profileRelease(aatpe_profile::ProfileSource::THERMAL);
        }
        s_warningState.streak = 0;
    }

    // ========================================================================
    // Enhanced thermal pressure computation.
    //
    // Previously pressure was purely value-based:
    //   pressure = 50 + (temp - limit) * 5, capped at 100.
    //
    // Now we combine four factors:
    //   1. Value component: how far above threshold (base pressure)
    //   2. Slope component: how fast temperature is rising (adds urgency)
    //   3. Duration component: how long we've been above threshold (adds persistence)
    //   4. Headroom component: remaining thermal headroom to critical wall
    //
    // Example: 83°C for 1 second vs 83°C for 30 seconds should NOT be equal.
    // The duration component adds pressure the longer we stay above threshold.
    //
    // Skin → user experience / performance limit
    // Battery → battery safety / derating
    // CPU → scheduler pressure (separate output, not mixed with skin/battery)
    // ========================================================================

    // Record temperature history sample.
    // NOTE: tLock was released before this point (to avoid deadlock with
    // transitionToState() which takes g_cfgMutex; documented lock order is
    // g_cfgMutex -> g_thermalMutex). The following state variables
    // (s_thermalHistory, s_overTempStreak, g_emergencyStartTime, s_warningState,
    // s_criticalState) are ONLY accessed from readThermal() which runs on the
    // daemon main loop single-threaded. No concurrent access is possible.
    // Use nowMs (steady_clock) for timestamp consistency with g_thermalCacheLastMs;
    // previously used epoch time (now*1000) which could diverge on NTP adjustments.
    {
        uint64_t nowSampleMs = nowMs;
        ThermalSample sample;
        sample.timestampMs = nowSampleMs;
        // History/slope/duration are built on the TRUSTED CPU temp so a rogue spike
        // does not forge a fast-heating slope or a long "above critical" duration.
        sample.cpuTemp = decisionTemp;
        sample.skinTemp = foundSkinZone ? skinTemp : -1;
        sample.batteryTemp = batteryTemp;
        s_thermalHistory.push_back(sample);
        while (s_thermalHistory.size() > kMaxThermalHistorySamples) {
            s_thermalHistory.pop_front();
        }
    }

    // Compute temperature slope (°C per second) from history
    int cpuSlopeCps = 0;  // positive = heating up, negative = cooling down
    if (s_thermalHistory.size() >= 3) {
        const auto& oldest = s_thermalHistory.front();
        const auto& newest = s_thermalHistory.back();
        uint64_t dtMs = newest.timestampMs - oldest.timestampMs;
        if (dtMs > 0) {
            double dtSec = (double)dtMs / 1000.0;
            cpuSlopeCps = (int)round((double)(newest.cpuTemp - oldest.cpuTemp) / dtSec);
        }
    }

    // Compute duration above threshold (how many seconds we've been hot).
    // P3-20: this used to increment a counter per sample and call it "Sec".
    // Samples are ~1s apart (cache TTL), so the count was close to seconds,
    // but on fast loops or after a cache miss it could drift. Now compute
    // the REAL elapsed time by summing dtMs between consecutive above-threshold
    // samples walking backwards.
    int durationAboveCriticalSec = 0;
    // G6-004: use the runtime-normalized critical wall (monotonic-safe).
    int criticalWallLocal = criticalWall;
    if (criticalWallLocal > 0 && s_thermalHistory.size() >= 2) {
        auto it = s_thermalHistory.rbegin();
        if (it->cpuTemp >= criticalWallLocal) {
            uint64_t accMs = 0;
            auto prev = it;
            ++it;
            for (; it != s_thermalHistory.rend() && it->cpuTemp >= criticalWallLocal; ++it, ++prev) {
                accMs += (prev->timestampMs - it->timestampMs);
            }
            durationAboveCriticalSec = (int)(accMs / 1000ULL);
        }
    }

    // Unified critical level hysteresis state.
    // Tracks entry/exit of CRITICAL zone with min hold/recovery timing.
    int criticalExitWall = criticalWallLocal - thermal_runtime::kHysteresisDeltaC;
    if (criticalWallLocal > 0) {
        // CRITICAL (migration pressure) is a protective tier below the EMERGENCY
        // hard wall: use the trusted CPU temp, same rationale as WARNING.
        if (decisionTemp >= criticalWallLocal) {
            s_criticalState.streak++;
            if (!s_criticalState.active && s_criticalState.streak >= 2) {
                s_criticalState.active = true;
                s_criticalState.enteredAt = now;
                LOG_WARN("Thermal CRITICAL: temp=%dC >= %dC, migration pressure activated",
                         decisionTemp, criticalWallLocal);
            }
        } else if (decisionTemp <= criticalExitWall) {
            s_criticalState.streak = 0;
            if (s_criticalState.active) {
                bool minHoldMet = (now - s_criticalState.enteredAt) >= thermal_runtime::kMinHoldSec;
                if (minHoldMet) {
                    s_criticalState.active = false;
                    s_criticalState.exitedAt = now;
                    LOG_INFO("Thermal cooled below critical exit wall (%dC), migration pressure released",
                             criticalExitWall);
                }
            }
        } else {
            s_criticalState.streak = 0;  // in hysteresis band, hold state
        }
    }

    // --- Skin pressure (user experience / performance limit) ---
    int skinPressure = 0;
    if (foundSkinZone && skinTemp >= 0 && cfgSnap.thermalLimitSkin > 0) {
        if (skinTemp >= cfgSnap.thermalLimitSkin) {
            int excess = skinTemp - cfgSnap.thermalLimitSkin;
            skinPressure = std::min(100, 50 + excess * 5);
            // Duration bonus: skin temp sustained above limit adds pressure
            // (e.g. holding at 45°C for 30s = uncomfortable even if not extreme)
            if (durationAboveCriticalSec > 10) {
                skinPressure = std::min(100, skinPressure + 10);
            }
        }
    }

    // --- Battery pressure (battery safety / derating) ---
    int batteryPressure = 0;
    if (batteryTemp > 0 && cfgSnap.thermalLimitBattery > 0) {
        if (batteryTemp >= cfgSnap.thermalLimitBattery) {
            int excess = batteryTemp - cfgSnap.thermalLimitBattery;
            batteryPressure = std::min(100, 50 + excess * 5);
            // Battery duration bonus: sustained high battery temp accelerates
            // degradation even if the instantaneous reading isn't extreme.
            if (durationAboveCriticalSec > 15) {
                batteryPressure = std::min(100, batteryPressure + 15);
            }
        }
    }

    // --- CPU pressure (scheduler pressure) —: separate output ---
    // CPU pressure combines temperature level + slope + remaining headroom.
    // This drives migration throttling independently of skin/battery.
    int cpuPressure = 0;
    if (criticalWallLocal > 0 && decisionTemp >= (criticalWallLocal - 10)) {
        // Within 10°C of critical wall: start building pressure
        int proximity = criticalWallLocal - decisionTemp;  // headroom in °C
        if (proximity <= 0) {
            // Already at or above critical wall
            cpuPressure = 70;
        } else {
            // Linear: 10°C below = 0, at wall = 70
            cpuPressure = std::max(0, 70 - proximity * 7);
        }
        // Slope bonus: rapid heating adds urgency
        if (cpuSlopeCps > 1) {
            cpuPressure = std::min(100, cpuPressure + cpuSlopeCps * 5);
        }
        // Duration bonus: sustained high temp adds pressure
        if (durationAboveCriticalSec > 20) {
            cpuPressure = std::min(100, cpuPressure + 10);
        }
    }

    // ========================================================================
    // v4.3.6: temperature-trend feedforward -- feed this sample and compose.
    //
    // Inputs (all already normalized by the existing reactive state machine):
    //   temp_c    = decisionTemp (trusted CPU temp, deg C). The WARNING /
    //               EMERGENCY walls are CPU trip points, so the ETA predictor is
    //               anchored to the CPU trajectory. Skin has its own separate
    //               limit (thermalLimitSkin) handled above by skinPressure and is
    //               NOT on the same scale as these walls, so it is not mixed in.
    //   now_ms    = nowMs (monotonic, steady_clock ms). Non-monotonic samples are
    //               rejected by the core.
    //   hard_wall = current reactive wall (passthrough; never modified here).
    // The predictor only proposes an ADDITIVE gear; it never moves a wall.
    // ========================================================================
    if (!s_thermFfReady) {
        therm_ff_config_default(&s_thermFfCfg);
        therm_ff_state_init(&s_thermFfState);
        s_thermFfReady = true;
    }
    // Re-sync the ETA trip thresholds from the runtime-normalized walls every
    // tick (hot-reload / runtime wall edits are honoured; other tunables keep
    // the verified defaults).
    s_thermFfCfg.warning_temp_c   = (float)warningWall;
    s_thermFfCfg.emergency_temp_c = (float)emergencyWall;

    ThermHardWall ffWall = aatpe_therm_ff_map_wall(
        g_thermalEmergency.load(std::memory_order_relaxed),
        s_criticalState.active, s_warningState.active);

    ThermFfOut ffOut;
    therm_ff_update(&s_thermFfState, &s_thermFfCfg,
                    (double)nowMs, (float)decisionTemp, ffWall, &ffOut);

    FfGear ffWallGear = ffOut.wall_gear;  // hard wall mapped to gear (0..3)
    FfGear ffGear     = ffOut.ff_gear;    // additive pre-throttle suggestion
    FfGear ffFinal    = aatpe_therm_ff_final_gear(ffWallGear, ffGear);

    // SAFETY RED LINE (belt-and-suspenders): whenever the EMERGENCY hard wall is
    // active the final gear MUST be at least SEVERE. max(wall,ff) already
    // guarantees this (EMERGENCY -> wall_gear=SEVERE=3, ff<=3), but clamp it
    // explicitly so no future change can weaken the hard wall at this landing.
    if (ffWall == THERM_HW_EMERGENCY && ffFinal < FF_GEAR_SEVERE) {
        ffFinal = FF_GEAR_SEVERE;
    }

    bool ffPreThrottle = aatpe_therm_ff_pre_throttle(ffFinal, ffWallGear) &&
                         (ffOut.sensor_ok != 0);

    // Actuator landing -- pre-throttle only when the predictor is LEADING and the
    // sensor is healthy: begin easing load EARLY, before any wall trips (降 boost /
    // 限 devfreq / 稳帧 via the existing placement & gpu_boost consumers of CPU
    // pressure). This is a bounded, ADDITIVE max() bump to cpuPressure: it can
    // only RAISE easing, never lower it, and it goes inert the instant a wall is
    // active (ffPreThrottle then false; the wall path's own pressure governs).
    if (ffPreThrottle) {
        int ffPressure = 30 + (int)ffGear * 15;  // LIGHT=45 MODERATE=60 SEVERE=75
        if (ffPressure > cpuPressure) cpuPressure = ffPressure;
    }

    // Write individual pressure outputs
    writeFile((std::string(STATE_DIR) + "/thermal_skin_pressure").c_str(), skinPressure);
    writeFile((std::string(STATE_DIR) + "/thermal_battery_pressure").c_str(), batteryPressure);
    writeFile((std::string(STATE_DIR) + "/thermal_cpu_pressure").c_str(), cpuPressure);
    writeFile((std::string(STATE_DIR) + "/thermal_cpu_slope").c_str(), cpuSlopeCps);
    writeFile((std::string(STATE_DIR) + "/thermal_duration_above_critical_sec").c_str(),
              durationAboveCriticalSec);

    // Combined migration pressure: take the MAX of all three independent
    // pressure sources. They are NOT simply weighted — each sensor domain has
    // its own threshold and action:
    //   - skinPressure → user-perceived warmth (performance limit)
    //   - batteryPressure → battery safety (derating)
    //   - cpuPressure → scheduler pressure (migration throttling)
    // The max() ensures the most constrained domain dictates the throttle level.
    int combinedPressure = std::max({skinPressure, batteryPressure, cpuPressure});
    writeFile((std::string(STATE_DIR) + "/thermal_migration_pressure").c_str(), combinedPressure);

    // v4.3.6: publish the feedforward predictor view for diag / WebUI / CLI.
    // These are informational + the pre-throttle contract; the hard wall remains
    // authoritative (thermal_emergency / SystemState). slope/ETA use the float
    // overload so the trend is visible without quantization to whole C / whole s.
    writeFile((std::string(STATE_DIR) + "/thermal_ff_slope_cps").c_str(),
              ffOut.slope_cps);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_eta_warning_s").c_str(),
              ffOut.eta_warning_s);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_eta_emergency_s").c_str(),
              ffOut.eta_emergency_s);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_gear").c_str(),        (int)ffGear);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_wall_gear").c_str(),  (int)ffWallGear);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_final_gear").c_str(),  (int)ffFinal);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_sensor_ok").c_str(),   ffOut.sensor_ok);
    writeFile((std::string(STATE_DIR) + "/thermal_ff_pre_throttle").c_str(), ffPreThrottle ? 1 : 0);

    // === C3 exploration: continuous headroom smooth derating (DEFAULT OFF) ===
    // When thermalSmoothEnabled=false (default), this block is completely inert:
    // g_thermalPlacementScale stays at 1.0f and no behavior changes vs v4.4.0.
    // When enabled, we reuse therm_ff's slope/ETA + existing walls to compute a
    // continuous placementScale factor [0.4,1.0] that downstream consumers
    // (placement.cpp proactive BIG push, aatpe_priority.cpp uclamp) can optionally
    // multiply into their decisions. The hard wall remains authoritative.
    extern std::atomic<bool> thermalSmoothEnabled;  // defined in aatped_main.cpp, default false
    if (thermalSmoothEnabled.load(std::memory_order_relaxed)) {
        using namespace aatpe::thermh;
        static float  s_smoothPrev     = 1.0f;
        static int    s_smoothRelaxStrk = 0;

        ThermalHeadroomInput hIn{};
        hIn.tempC          = (float)decisionTemp;
        hIn.warningWallC   = (float)warningWall;
        hIn.criticalWallC  = (float)criticalWall;
        hIn.emergencyWallC = (float)emergencyWall;
        hIn.slopeCps       = ffOut.slope_cps;
        hIn.etaWarningS    = ffOut.eta_warning_s;
        hIn.sensorOk       = (ffOut.sensor_ok != 0);
        hIn.thermalEmergency = g_thermalEmergency.load(std::memory_order_relaxed);

        HeadroomOutput hOut = thermalHeadroom(hIn);
        smoothPlacementScale(hOut.placementScale, s_smoothPrev, s_smoothRelaxStrk);
        g_thermalPlacementScale.store(s_smoothPrev, std::memory_order_relaxed);

        writeFile((std::string(STATE_DIR) + "/thermal_headroom01").c_str(), hOut.headroom01);
        writeFile((std::string(STATE_DIR) + "/thermal_placement_scale").c_str(), s_smoothPrev);
    }
}

// ROUND-011: independent thermal sampling thread.
#include <condition_variable>
static std::thread s_thermalThread;
static std::atomic<bool> s_thermalRun{false};
// ROUND-011: CLOCK_BOOTTIME seconds of last successful full thermal scan.
// Written ONLY by the sampling thread on success; read by the main loop
// to compute freshness (which detects thread stuck in sysfs read).
static std::atomic<long long> s_lastScanBootSec{0};
static std::condition_variable s_thermalCv;
static std::mutex s_thermalMutex;
static std::atomic<bool> s_thermalDone{false};      // ROUND-014 P3: thread exited
static std::atomic<bool> s_thermalDetached{false};  // refuse restart after detach

// CLOCK_BOOTTIME seconds (monotonic across suspend).
static std::function<long long()> s_testBootClockFn;
static long long bootTimeSec() {
    if (s_testBootClockFn) return s_testBootClockFn();
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long)ts.tv_sec;
}
// Test seam: override boot clock for freshness tick tests.
#ifdef AATPE_HOST_TEST
void thermalSetBootClockForTest(std::function<long long()> fn) { s_testBootClockFn = std::move(fn); }
// Test seam: set last scan stamp directly (bypass thread).
void thermalSetScanStampForTest(long long sec) { s_lastScanBootSec.store(sec); }
#endif

// ROUND-014 P3: cached HAL result + TTL.
struct ThermalHalInfo { int level; int skinC; int headroom; };
ThermalHalInfo parseThermalServiceDump(const std::string& dump);
static std::mutex s_halMutex;
static ThermalHalInfo s_halCache{-1,-1,-1};

// v4.2.3 P0: authoritative ML training freeze gate.
// P1 round7: ML training freeze thresholds are DERIVED from the emergency hard
// wall (config-driven, no fixed chip temperature). We pause ML training well
// below the emergency wall and resume with a fixed hysteresis. If the emergency
// threshold is unset we fall back to a conservative generic sentinel
// (105/100 °C) — never a model-specific number.
static int mlFreezePauseC() {
    int wall = g_cfg.thermalEmergencyThreshold;  // config (°C), 0=unset
    if (wall <= 0) wall = 105;            // generic sentinel
    return wall - 15;                     // pause 15°C below emergency
}
static int mlFreezeResumeC() { return mlFreezePauseC() - 5; }  // 5°C hysteresis

// ROUND-014 P3: read-only accessor consumed by gpu_boost/placement gating.
namespace aatpe {
int aatpeThermalHalLevel() { std::lock_guard<std::mutex> lk(s_halMutex); return s_halCache.level; }
// v4.2.1 T1-fix: accessor for sensor-health gate. When true, our sysfs scan
// is stale/degraded (>10s); ML training should not be frozen by HAL>=3 alone.
bool aatpeThermalDegraded() { return thermalDegradedAny(); }

// v4.2.3 P0: authoritative ML training freeze decision.
// Returns true only when BOTH:
//   1. thermal is enabled (disabled = no thermal constraints = no freeze)
//   2. Sysfs CPU temp >= pause threshold (mlFreezePauseC() = emergency wall - 15°C,
//      not a hardcoded 85°C) — primary evidence.
//   OR: HAL level >= 3 (SEVERE) AND sysfs confirms hot (>= pause threshold).
// HAL alone with invalid data (-1 headroom/skin) does NOT freeze.
// Hysteresis: once frozen, only resume when temp drops below resume threshold
// (mlFreezeResumeC() = pause - 5°C, not a hardcoded 80°C).
const char* aatpeThermalFreezeReason() {
    return s_mlFreezeReason.load(std::memory_order_relaxed);
}
// v6.0.0 round5b P2: the 120°C emergency hard wall is FAIL-SAFE: it MUST fire
// regardless of the user's thermalEnabled switch. thermalEnabled=0 only disables
// *regular* thermal-aware scheduling / profile auto-derating. The predicate
// aatpeThermalEmergencyHardWall lives inline in aatped.h (no thermalEnabled input).
bool aatpeThermalMayFreezeTraining(bool thermalEnabled) {
    if (!thermalEnabled) {
        g_mlFreezeHysteresis.store(false);  // disabled = reset hysteresis
        s_mlFreezeReason.store("disabled", std::memory_order_relaxed);
        return false;
    }
    if (aatpeThermalDegraded()) {
        s_mlFreezeReason.store("sensor_degraded", std::memory_order_relaxed);
        return false;  // stale sensors = don't freeze
    }

    int cpuTemp = g_sysfsCpuMaxTempC.load(std::memory_order_relaxed);
    int halLevel = aatpeThermalHalLevel();

    // Hysteresis: if already frozen, check resume threshold.
    if (g_mlFreezeHysteresis.load(std::memory_order_relaxed)) {
        if (cpuTemp > 0 && cpuTemp < mlFreezeResumeC()) {
            g_mlFreezeHysteresis.store(false);
            s_mlFreezeReason.store("resumed", std::memory_order_relaxed);
            return false;  // cooled down, resume
        }
        s_mlFreezeReason.store(cpuTemp >= mlFreezePauseC() ? "cpu_hot" : "hal_severe",
                               std::memory_order_relaxed);
        return true;  // still hot, stay frozen
    }

    // Not frozen yet: check pause threshold.
    // Sysfs primary evidence.
    bool sysfsHot = (cpuTemp > 0 && cpuTemp >= mlFreezePauseC());
    // HAL secondary: only count if HAL level is SEVERE+ AND sysfs confirms.
    bool halConfirms = (halLevel >= 3 && cpuTemp > 0 && cpuTemp >= mlFreezePauseC());

    if (sysfsHot || halConfirms) {
        g_mlFreezeHysteresis.store(true);
        s_mlFreezeReason.store(halConfirms ? "hal_severe" : "cpu_hot",
                               std::memory_order_relaxed);
        return true;
    }
    // Not frozen: HAL SEVERE but sysfs not hot / invalid headroom -> no freeze.
    if (halLevel >= 3) s_mlFreezeReason.store("hal_invalid", std::memory_order_relaxed);
    else s_mlFreezeReason.store("none", std::memory_order_relaxed);
    return false;
}
}  // namespace aatpe
static long long s_halLastForkMs = 0;
static int s_scanCycle = 0;

// #9 fix: was a bare fork + blocking read() + waitpid(...,0) with NO poll/deadline/
// kill — a hung thermalservice could wedge the sampling thread forever (and, because
// the thread never reached s_thermalDone, stopThermalSampling had to detach() it — #7).
// Now delegates to the shared hardened aatpeExecArgv() (3000ms wall-clock deadline +
// setpgid + killpg + bounded WNOHANG reap). The caller parse logic is unchanged.
static std::string aatpeExecCapture(const char* cmd) {
    return aatpeExecArgv({cmd, "thermalservice"}, "thermal-hal");
}

static void refreshThermalHal() {
    long long now = bootTimeSec() * 1000LL;
    if (now - s_halLastForkMs < 5000) return;  // 5s TTL
    std::string dump = aatpeExecCapture("dumpsys");
    if (dump.empty()) dump = aatpeExecCapture("cmd");
    s_halLastForkMs = now;
    ThermalHalInfo info{-1,-1,-1};
    if (!dump.empty()) info = parseThermalServiceDump(dump);
    {
        std::lock_guard<std::mutex> lk(s_halMutex);
        s_halCache = info;
    }
    // Write state keys.
    static const char* lvlNames[] = {"NONE","LIGHT","MODERATE","SEVERE","CRITICAL","EMERGENCY","SHUTDOWN"};
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_hal_status").c_str(),
        info.level < 0 ? "UNAVAILABLE" : lvlNames[info.level]);
    writeFile((std::string(STATE_DIR) + "/thermal_hal_skin").c_str(), info.skinC);
    writeFile((std::string(STATE_DIR) + "/thermal_hal_headroom").c_str(), info.headroom);
    // THM-01: do NOT clobber thermal_source (the sysfs temp-node path owned by
    // readThermal). The HAL provenance goes to its own file so the path and the
    // "hal/sysfs" label no longer fight over the same file.
    writeFileAtomic((std::string(STATE_DIR) + "/thermal_hal_source").c_str(),
        info.level < 0 ? "sysfs" : "hal");
}


void startThermalSampling() {
    if (s_thermalRun.exchange(true)) return;
    if (s_thermalDetached.load()) return;  // after detach, refuse restart
    s_lastScanBootSec.store(bootTimeSec());  // seed so main loop sees fresh at boot
    s_thermalDone.store(false);
    s_thermalThread = std::thread([](){
        while (s_thermalRun.load()) {
            try {
                readThermal();
                s_lastScanBootSec.store(bootTimeSec());
                if ((++s_scanCycle % 3) == 0) refreshThermalHal();
            } catch (...) {
                // Best-effort thermal scan must not kill the thread.
                // Low-frequency warn: log at most once per 10 cycles (~10s).
                static int s_exceptCycleCount = 0;
                if ((++s_exceptCycleCount % 10) == 1) {
                    LOG_WARN("thermal: unexpected exception in scan loop (cycle #%d), continuing",
                             s_exceptCycleCount);
                }
            }
            std::unique_lock<std::mutex> lk(s_thermalMutex);
            s_thermalCv.wait_for(lk, std::chrono::milliseconds(1000),
                                 []{ return !s_thermalRun.load(); });
        }
        s_thermalDone.store(true);
        // #7 fail-safe: after #9 the sampling thread can no longer wedge in a
        // timeout-less exec, so this point is reached within one wait_for and the
        // 3s join in stopThermalSampling() succeeds — the detach branch is now
        // unreachable in practice. As belt-and-suspenders: if the main thread ever
        // DID give up and detach us, the process is in static-teardown (the daemon
        // shutdown path returns from main(), so C++ static destructors run) and
        // s_thermalCv may already be destroyed. notify_all on a destroyed CV is UB,
        // so skip it when we have been detached. The atomic store above is always
        // safe (trivial destructor).
        if (!s_thermalDetached.load()) s_thermalCv.notify_all();
    });
}

// ROUND-011: called by main loop every second. Reads the atomic stamp,
// computes age, writes thermal_freshness state file, and manages the
// SCAN_STUCK degraded reason if >10s stale. Thread-safe (only reads/writes
// atomics, no mutex).
//
// P2-1: this tick ONLY owns the SCAN_STUCK reason. A fresh stamp proves the
// sampling LOOP is alive — it does NOT prove any sensor actually returned a
// reading. So on the fresh branch we clear SCAN_STUCK but leave NO_VALID_SENSOR
// (owned by readThermal()) untouched, and publish the OR of the two reasons
// instead of blindly writing degraded=0. Previously this branch wrote
// degraded=false whenever the stamp was fresh, which silently wiped the
// "every sensor dead" fail-safe within ~1s while the loop kept refreshing the
// stamp.
void thermalFreshnessTick() {
    long long last = s_lastScanBootSec.load(std::memory_order_acquire);
    if (last == 0) return;  // not yet seeded
    long long age = bootTimeSec() - last;
    if (age < 0) age = 0;  // clock skew guard
    if (age > 10) {
        // Sampling loop not making progress -> SCAN_STUCK reason.
        g_thDegScanStuck.store(true, std::memory_order_release);
        writeFile((std::string(STATE_DIR) + "/thermal_degraded").c_str(), 1);
        writeFileAtomic((std::string(STATE_DIR) + "/thermal_freshness").c_str(),
                        std::string("stale=") + std::to_string(age));
    } else {
        // Loop alive: clear the SCAN_STUCK reason. NO_VALID_SENSOR is untouched.
        g_thDegScanStuck.store(false, std::memory_order_release);
        // Publish the OR: sensors may still be dead even though the loop runs.
        bool degraded = thermalDegradedAny();
        writeFile((std::string(STATE_DIR) + "/thermal_degraded").c_str(), degraded ? 1 : 0);
        // freshness reflects scan-loop liveness; thermal_degraded carries the
        // sensor-outage bit (the two are independent by design).
        writeFileAtomic((std::string(STATE_DIR) + "/thermal_freshness").c_str(), "fresh");
    }
}

void stopThermalSampling() {
    if (!s_thermalRun.exchange(false)) return;
    s_thermalCv.notify_all();
    if (!s_thermalThread.joinable()) return;
    // ROUND-014 P3: spin on atomic done flag up to 3s (no mutex held -> no
    // double-lock with the sampling thread's own s_thermalMutex wait).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!s_thermalDone.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (s_thermalDone.load()) {
        s_thermalThread.join();
    } else {
        // #9 bounded the HAL exec to 3s, so the thread cannot block past one
        // wait_for and this detach branch is effectively dead code. We keep it as a
        // last-resort (a pathological sysfs read hanging beyond 3s). After detach the
        // thread skips the CV notify (see lambda above), so it cannot touch the
        // (possibly-destroyed) s_thermalCv during static teardown.
        s_thermalThread.detach();
        s_thermalDetached.store(true);
    }
}

// ROUND-014 P3: thermal HAL (dumpsys thermalservice) structured parser.
// AOSP dump line format: Temperature{mValue=32.1, mType=SKIN, mName=..., mStatus=0, ...}
// mStatus: 0=NONE 1=LIGHT 2=MODERATE 3=SEVERE 4=CRITICAL 5=EMERGENCY 6=SHUTDOWN.

static int halStatusToLevel(int s) {
    if (s < 0 || s > 6) return -1;
    return s;
}

ThermalHalInfo parseThermalServiceDump(const std::string& dump) {
    ThermalHalInfo r{-1, -1, -1};
    std::stringstream ss(dump);
    std::string line;
    while (std::getline(ss, line)) {
        // Find mStatus=NN
        size_t sp = line.find("mStatus=");
        if (sp == std::string::npos) continue;
        int st = atoi(line.c_str() + sp + 8);
        int lvl = halStatusToLevel(st);
        if (lvl < 0) continue;
        // Skip BATTERY-only entries for worst level; still read skin value.
        bool isBattery = line.find("mType=BATTERY") != std::string::npos;
        if (!isBattery && lvl > r.level) r.level = lvl;
        // mValue=NN.N
        size_t vp = line.find("mValue=");
        if (vp != std::string::npos) {
            double v = atof(line.c_str() + vp + 7);
            if (!std::isfinite(v)) continue;  // ROUND-014 P3: reject NaN/inf
            if (line.find("mType=SKIN") != std::string::npos) {
                int s = (int)v;
                if (s < -40) s = -40;
                if (s > 150) s = 150;
                r.skinC = s;
            }
            if (line.find("mType=HEADROOM") != std::string::npos ||
                line.find("mType=BRAIN") != std::string::npos) {
            // ROUND-014 P3: headroom per AOSP Thermal HAL = fraction of the way
            // to the throttling threshold (1.0 = throttling starts). Larger =
            // worse, NOT remaining margin. Aggregate multi-channel by max (worst
            // channel wins), consistent with mStatus worst-level logic. If the HAL
            // already pre-aggregates to a single HEADROOM channel, max is a no-op.
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            int h = (int)(v * 100.0);
            if (h > r.headroom) r.headroom = h;
            }
        }
    }
    return r;
}


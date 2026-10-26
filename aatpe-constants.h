// AATPE - Adaptive Android Task Placement Engine
// Magic Number Constants Header
// All magic numbers extracted and named for readability.
// Values are UNCHANGED - only names are introduced.
// Version: (see ./VERSION)

#ifndef AATPE_CONSTANTS_H
#define AATPE_CONSTANTS_H

#include <cstdlib>
#include <cerrno>
#include <climits>
#include <string>

namespace aatpe {

// ============================================================================
// Safe integer parsing helper (replaces unsafe atoi across production code)
// strtol + endptr validation; failure returns def (default 0, matching atoi
// failure semantics where atoi("abc") returns 0).
// ============================================================================
inline int parseIntSafe(const char* s, int def = 0) {
    if (!s || !*s) return def;
    char* end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || v < INT_MIN || v > INT_MAX) return def;
    return (int)v;
}
inline int parseIntSafe(const std::string& s, int def = 0) {
    return parseIntSafe(s.c_str(), def);
}

// ============================================================================
// SCORING ENGINE - Latency Scores (per role)
// ============================================================================
namespace latency {
    constexpr int kScoreUi          = 200;  // ROLE_S1_UI
    constexpr int kScoreRender      = 180;  // ROLE_S2_RENDER
    constexpr int kScoreGame        = 150;  // ROLE_S3_GAME
    constexpr int kScoreThroughput  = 50;   // ROLE_S4_THROUGHPUT
    constexpr int kScoreSystem      = 0;    // ROLE_S0_SYSTEM
    constexpr int kScoreBackground  = 0;    // ROLE_S5_BACKGROUND
}

// ============================================================================
// SCORING ENGINE - Thermal Scores
// ============================================================================
namespace thermal {
    constexpr int kHighTempThreshold   = 70;  // Above this: penalty applies
    constexpr int kPenaltyPerDegree    = 3;   // Penalty multiplier per degree over threshold
    constexpr int kLowTempThreshold    = 50;  // Below this: boost applies
    constexpr int kBoostPerDegree      = 2;   // Boost multiplier per degree under threshold
    constexpr int kThrottleFreqPct     = 80;  // CPU freq < 80% max = throttling
    constexpr int kEmergencyHysteresis = 10;  // Recovery requires temp < emergency - 10
}

// ============================================================================
// CPU TOPOLOGY - Pool Classification Ratios
// ============================================================================
namespace topology {
    constexpr int kLittleMaxRatio  = 70;   // capacity ratio <= 70% → LITTLE (A55/A510 typically 20-65%, e.g. 天玑1100 cpu0-3=241/1024=23.5%)
    constexpr int kMidMaxRatio     = 90;   // capacity ratio <= 90% → MID (non-Prime A78/A710 typically 75-90%, e.g. 天玑1100 cpu4-6=917/1024=89.6%), >90% → BIG (Prime core=1024, e.g. 天玑1100 cpu7)
    constexpr int kMaxCapacityRef  = 1024; // Reference max capacity for ratio calc
    constexpr int kDefaultCapacity = 380;   // Default CPU capacity when unavailable
    constexpr int kMidFallbackMinLittle = 4; // If no mid pool and >3 little cores, split
    constexpr int kHexBitWidth     = 64;    // Hex affinity mask bit width

    // ---- ROUND-004 R2: generic N-cluster RATIO gap clustering ----
    // Cut between adjacent strength-sorted cores when EITHER axis jumps by a
    // multiplicative factor. Multiplicative (not absolute normalized gap) because
    // absolute gaps are sensitive to the vendor kernel's max capacity value and
    // merge near-equal A78 clusters (Exynos2100: 825 vs 1024, 250 vs 260MHz).
    // Validated across: D1100, SD888, SD870, Exynos2100, Dim900, SD845, SD835,
    // TV 4+A73+4+A53, 1+2+1+4.
    //   hi.cap / lo.cap >= kGapCapRatio  OR  hi.freq / lo.freq >= kGapFreqRatio
    constexpr double kGapCapRatio   = 1.20;  // 20% relative capacity jump
    constexpr double kGapFreqRatio  = 1.15;  // 15% relative freq jump
    // PRIME: top cluster exists only when >=3 clusters AND a small lead cluster
    // (size<=2) with a freq/cap lead. "size" is soft, not a hard gate.
    constexpr double kPrimeMinFreqRatio = 1.15;  // top.freq / next.freq
    constexpr double kPrimeMinCapRatio  = 1.20;  // top.cap  / next.cap
}

// ============================================================================
// THREAD CLASSIFICATION - Schedstat Thresholds
// ============================================================================
namespace thread_classify {
    constexpr int kThroughputAvgRunUs  = 5000; // avgRun > 5000us → throughput
    constexpr int kUiAvgRunUs          = 500;  // avgRun > 500us → candidate UI
    constexpr int kUiMinSwitches       = 100;  // nrSwitches > 100 → UI (combined with above)
    constexpr int kGameAvgRunUs        = 2000; // avgRun > 2000us -> GAME (high-util worker, not BG)
    constexpr int kDefaultPriority     = 50;   // Default thread priority
}

// ============================================================================
// SCORING ENGINE - Cache Affinity Score
// ============================================================================
namespace cache_score {
    // reduced from 20 to 5. Combined with kSamePoolPenalty(10→3),
    // current-CPU inherent advantage drops from +30 to +8 (same-pool).
    // At +30, other same-pool CPUs needed 40+ point edge (30 advantage + 10 cost threshold),
    // impossible under high load where all same-pool CPUs have identical contention/capacity/pool.
    // At +8, a 18+ point edge suffices; e.g. current util 60%(-20) vs other util<=40%(+10) = 30pt gap.
    constexpr int kAffinityHit = 5;
}

// ============================================================================
// SCORING ENGINE - Power Scores (per profile / role)
// ============================================================================
namespace power {
    // battery profile
    constexpr int kBatteryBgScore        = 30;
    constexpr int kBatteryThroughputScore = -20;
    constexpr int kBatteryOtherScore      = -10;
    // gaming profile
    constexpr int kgamingInteractiveScore = 20;   // UI/Render/Game
    constexpr int kgamingBgScore          = -30;
    constexpr int kgamingOtherScore       = 0;
    // thermal profile
    constexpr int kThermalBgScore         = 40;
    constexpr int kThermalOtherScore      = -15;
}

// ============================================================================
// SCORING ENGINE - Frame Critical Score
// ============================================================================
namespace frame_score {
    constexpr int kCriticalBoost = 100;  // UI/Render/Game during frame critical
}

// ============================================================================
// SCORING ENGINE - Pool Preference Scores (per role / pool)
// ============================================================================
namespace pool_score {
    // ================= G7 ROUND-004 B2 (科学正确性修复) =====================
    // 历史两簇(小/大)时代按天玑1100标定，升级到通用多级(LITTLE/MID/BIG/PRIME)后
    // 出现两处大小倒挂：kUiBig(30)<kUiMid(32)、kThroughputBig(20)<kThroughputMid(30)，
    // 导致 UI/吞吐线程被池分量推向中核而非超大核(PRIME/BIG)，真机表现为"超大核不发力、
    // 其余核高负载"。本区块改为对层级单调：所有延迟敏感 role 必须满足
    //   PRIME >= BIG >= MID >= LITTLE。
    // 数值依据：相邻层级差 ≥ 归一化后可压过跨池迁移粘性(migration 权重 0.3×~10=3 分)，
    //   但不超过 capacity 分量(~100)，使容量主导、池偏好"同档内"微调。
    //   层级间距取 ~6-10 分(归一化 -50..50 后 ~6-10%)，避免任一维独大。
    // PRIME 层(D1100 cpu7, A78 超大核)：比 BIG 再多 +10，奖励最高频交互线程上超大核。

    // UI (S1) — 修复前 kUiBig=30 < kUiMid=32 (倒挂)。现 PRIME>BIG>MID>LITTLE 单调。
    constexpr int kUiPrime      = 48;   // 新增：超大核
    constexpr int kUiBig        = 38;   // 30→38 (确保 > kUiMid)
    constexpr int kUiMid        = 32;   // 保留 A78 中核 bump
    constexpr int kUiLittle     = -20;
    // Render (S2) — 原顺序已正确，补 PRIME。
    constexpr int kRenderPrime  = 55;   // 新增
    constexpr int kRenderBig   = 44;   // 40→44
    constexpr int kRenderMid   = 35;
    constexpr int kRenderLittle = -30;
    // Game (S3) — 原顺序已正确，补 PRIME。
    constexpr int kGamePrime   = 65;   // 新增
    constexpr int kGameBig     = 55;   // 50→55
    constexpr int kGameMid     = 38;   // 35→38
    constexpr int kGameLittle  = -40;
    // Throughput (S4) — 修复前 kThroughputBig=20 < kThroughputMid=30 (倒挂)。单调化。
    constexpr int kThroughputPrime   = 35;   // 新增
    constexpr int kThroughputBig    = 28;   // 20→28 (确保 > MID)
    constexpr int kThroughputMid    = 22;   // 30→22 (回退倒挂)
    constexpr int kThroughputLittle = 5;    // 10→5 (单调最低)
    // Background (S5) — 有意偏好小核、避开超大核/大核，单调反向(小>中>大>超大)。
    constexpr int kBackgroundPrime  = -60;  // 新增：后台最不该上超大核
    constexpr int kBackgroundLittle = 40;
    constexpr int kBackgroundMid    = -10;
    constexpr int kBackgroundBig    = -50;
}

// ============================================================================
// SCORING ENGINE - Migration Penalty
// ============================================================================
namespace migration {
    // kCrossPoolPenalty 10→7 — 降低跨池迁移粘性屏障。
    //   原值 10 归一化后 -30 分, 加上 cache affinity +15 分 = 45 分粘性,
    //   超过中核(A78)相对小核(A55)的性能优势(~40分), 导致线程滞留小核。
    //   改为 7 后粘性 = 21+15 = 36 分, 中核性能优势(修复后~46分)可胜出。
    //   same-pool 保持 3 (同池共享 L2, 迁移成本极低)。
    constexpr int kCrossPoolPenalty = 7;   // 10→7, 降低跨池迁移粘性
    constexpr int kSamePoolPenalty  = 3;   // Moving within same pool type
}

// ============================================================================
// B3 task2/8: PRIME spillover — promote top critical threads to the highest
// cluster (PRIME) when the second-highest cluster is saturated and PRIME has
// headroom. Solves the D1100 symptom: prime idle while 7 lower cores carry
// light-but-spread load. Thresholds are tier-count-generic (no hardcoded 3).
// Three control modes (passive/assist/active = 省电/正常/激进) are consumed
// here — no "empty" mode: every knob differs.
// ============================================================================
namespace spillover {
    // Second-highest-cluster avg util (%) above which we consider spilling to
    // PRIME. ROUND-007b: re-anchored to EAS ~25% headroom research — passive 85
    // (battery), assist 75 (balanced), active 65 (reach early under load).
    constexpr float kUtilThresholdPassive = 85.0f;
    constexpr float kUtilThresholdAssist  = 75.0f;
    constexpr float kUtilThresholdActive  = 65.0f;
    // PRIME must have this much free headroom (util <= kPrimeHeadroom%) to take
    // another thread.
    constexpr float kPrimeHeadroom        = 55.0f;
    // ROUND-008b: LITTLE-cluster aggregate pressure threshold per mode.
    // active reaches early, passive conservative.
    constexpr float kClusterPressureActive  = 75.0f;
    constexpr float kClusterPressureAssist = 80.0f;
    constexpr float kClusterPressurePassive = 85.0f;
    // How many of the heaviest critical threads to lift per decision.
    constexpr int kTopNPassive = 1;
    constexpr int kTopNAssist   = 2;
    constexpr int kTopNActive  = 3;
    // Dwell (ms) a promoted thread must stay on PRIME before downgrade.
    // ROUND-007b: rescaled from 500/300/200 to 150/100/60 — aligned to PELT 32ms
    // half-decay (2-5x). The old 200ms+ dwell kept threads parked on PRIME long
    // after a burst ended (battery). Upgrade still fast; downgrade now quick but
    // debounce (kStableCycles) and min-dwell remain to prevent ping-pong.
    constexpr long long kDwellMsPassive = 150;
    constexpr long long kDwellMsAssist  = 100;
    constexpr long long kDwellMsActive  = 60;
    // Require the saturated condition to persist this many decision cycles
    // before promoting (debounce).
    constexpr int kStableCycles = 2;
    // ROUND-007b: input-driven immediate boost window (ms). Aligned to industry
    // INTERACTION boost 40-100ms (QTI powerhint, EWMA decay). After an input/
    // cold-start event we do ONE lightweight thread-level boost (uclamp/top-app
    // cpuset/affinity — NEVER a global min_freq raise, which research shows is
    // power-costly and back-pressured by EAS/thermal), then auto-release.
    constexpr long long kInputBoostWindowMs = 80;
}


// ============================================================================
// SCORING ENGINE - Contention Penalty (CPU utilization)
// ============================================================================
namespace contention {
    constexpr int kHighUtilThreshold   = 80;  // util > 80%
    constexpr int kHighUtilPenalty     = -200;   // -40 -> -200, offset capacity gap (BIG=1024 vs MID=917)
    constexpr int kMidUtilThreshold    = 60;  // util > 60%
    constexpr int kMidUtilPenalty      = -80;    // -20 -> -80
    constexpr int kLowUtilThreshold    = 40;  // util > 40%
    constexpr int kLowUtilPenalty      = -20;    // -5 -> -20
    // kIdleBoost reduced from 80 to 40.
    // ROOT CAUSE of "小核满载大核闲置": At 80, contention dimension has 100-point
    // dynamic range (idle=+80 -> nBase=100, high=-200 -> nBase=0) at weight 1.0,
    // which DOMINATES capacity (A78=380 vs A55=160 -> only 21.5 pts) and poolScore
    // (UI BIG vs LITTLE -> 20 pts after 0.4 weight). Idle little cores thus scored
    // higher than loaded big cores, pulling game/UI threads to little cores.
    // fix84's 80 was calibrated for BIG=1024/MID=917 (10% capacity gap), but real
    // 天玑1100 has A78=380/A55=160 (58% gap) where capacity should dominate.
    // At 40, idle nBase=(40+200)/280*100=85.7, contention range=85.7 pts, still
    // meaningful for load balancing but no longer dominates capacity+poolScore.
    constexpr int kIdleBoost           = 40;     // 80 -> 40, prevent idle little cores from outscoring loaded big cores
}

// ============================================================================
// SCORING ENGINE - Capacity Score
// ============================================================================
namespace capacity {
    constexpr int kBackgroundInverseRef = 1024; // BG score = 1024 - capacity
}

// ============================================================================
// REFRESH RATE / VSYNC Constants
// ============================================================================
namespace refresh {
    constexpr int kMinRate       = 30;    // Minimum valid refresh rate (Hz)
    constexpr int kMaxRate       = 240;   // Maximum valid refresh rate (Hz)
    constexpr int kDefaultRate   = 60;    // Fallback default (Hz)
    constexpr int kCurrentCacheTtl = 3;   // Cache TTL for current mode (seconds)
    constexpr int kPeakCacheTtl    = 10;  // Cache TTL for peak/auto mode (seconds)
    constexpr int kOneSecondNs     = 1000000000; // 1 second in nanoseconds
    constexpr int kOneSecondUs     = 1000000;     // 1 second in microseconds
    constexpr int kMinVsyncPeriodNs = 1000000;   // Min valid vsync period
    // P3-40: align with kMinRate=30Hz. 1/30Hz = 33.33ms = 33333333ns.
    // Was 50ms (20Hz) which contradicted kMinRate=30Hz.
    constexpr int kMaxVsyncPeriodNs = 33333333;   // Max valid vsync period (30Hz)
    constexpr float kFrameCriticalMultiplier = 1.5f; // dynamic threshold = frameTime * 1.5
}

// ============================================================================
// SF LATENCY Constants
// ============================================================================
namespace sf_latency {
    constexpr int kMinUs   = 1000;    // Minimum valid latency (us)
    constexpr int kMaxUs   = 100000;  // Maximum valid latency (us)
    constexpr int kCacheTtl = 3;       // Cache TTL (seconds)
    constexpr int kMaxMultiplier = 2;  // maxLat = avgLat * 2
}

// ============================================================================
// LAYER COUNT Constants
// ============================================================================
namespace layer {
    constexpr int kCacheTtl = 2;  // Cache TTL for layer count (seconds)
}

// ============================================================================
// FOREGROUND APP Constants
// ============================================================================
namespace foreground {
    constexpr int kCacheTtl = 3;  // Cache TTL for foreground app detection (seconds)
}

// ============================================================================
// THREAD CURRENT CPU Cache
// ============================================================================
namespace cpu_cache {
    constexpr int kCacheTtlSec = 1;  // Cache TTL for current CPU reading (seconds)
}

// ============================================================================
// DAEMON LOOP / TIMING Constants
// ============================================================================
namespace daemon {
    constexpr int kDecisionCleanupIntervalSec = 60;   // Clean decisions dir every 60s
    constexpr int kDecisionFileMaxAgeSec       = 300;  // Delete decisions older than 300s
    constexpr int kHistoryDecayLoopCount       = 120;  // Decay history every 120 loops
    constexpr int kMinSleepMs                  = 20; // Minimum sleep to prevent busy-wait (10→20ms to halve poll wake frequency; imperceptible to users)
    constexpr int kHistoryScoreMinThreshold    = 5;    // Below this, score clamped to 0
    constexpr int kHistoryEntryMaxAgeSec       = 3600; // Clean history entries older than 1h
    // MAIN-08: idle-tick heuristic threshold. The daemon has no direct screen-on/off
    // API on this platform; the idle detector (which relaxes poll timeout up to 5s
    // for power saving) fires only when BOTH (a) the eBPF-tracked thread table has
    // shrunk below this count AND (b) there is no foreground app. A typical active
    // foreground/game workload keeps >= 50 tracked threads; an idle/standby box
    // collapses to ~5-10. 10 is deliberately generous so a background-heavy but
    // foreground-active session is NOT misclassified as idle.
    constexpr size_t kIdleTickMaxTrackedThreads = 10;
    // Poll timeout cap (ms) while the idle heuristic says the box is quiet.
    constexpr int kIdleTickPollTimeoutMs       = 5000;
}

// ============================================================================
// LOG ROTATION Constants
// ============================================================================
// LOGGING-LEVEL POLICY (debug/diagnostic vs production).
//
//   ERROR (0) : MUST print in production. Crashes, fatal init failures, hard
//               errors that leave the subsystem unusable.
//   WARN  (1) : SHOULD print in production. Abnormal-but-recoverable:
//               fallback activations, sensor reads failed once, degraded mode.
//   INFO  (2) : production-OPTIONAL. Only COARSE state changes (daemon start,
//               topology rebuild, foreground app change, mode transitions).
//               Never log per-event / per-decision / per-migration here.
//   DEBUG (3) : debugging ONLY, suppressed by default. Per eBPF event
//               (fork/exit/switch/exec), per migration, detailed counters.
//   TRACE (4) : most verbose, sampled.
//
// Default level = INFO (aatped.cpp g_logLevel{2}); DEBUG/TRACE require an
// explicit opt-in. 问题44: any high-frequency path must be DEBUG so the log
// itself does not become a latency/throughput problem under game load.
namespace logging {
    // P2-12: single canonical log rotation threshold (8MB). Was previously
    // duplicated as 1MB here, 8MB in logging.cpp, and 1MB in log_const —
    // now all reference this value.
    constexpr long long kMaxLogSizeBytes = 8LL * 1024LL * 1024LL; // 8 MB
    constexpr int kMaxLevel = 4;  // Valid log levels 0-4
    constexpr int kDefaultLevel = 2; // v94 fix / 问题45: default = INFO (matches g_logLevel{2})
}

// ============================================================================
// THREAD ROLE NAMES (human-readable role names for CLI output)
// ============================================================================
namespace role_names {
    constexpr int kRoleCount = 6;
    constexpr const char* kNames[kRoleCount] = {
        "S0_SYSTEM",      // 0 - system critical threads
        "S1_UI",          // 1 - UI / main threads
        "S2_RENDER",      // 2 - render / graphics threads
        "S3_GAME",        // 3 - game threads
        "S4_THROUGHPUT",  // 4 - high-throughput threads
        "S5_BACKGROUND"   // 5 - background threads
    };
    constexpr const char* kUnknown = "S0_UNKNOWN";

    inline const char* getName(int role) {
        if (role >= 0 && role < kRoleCount) return kNames[role];
        return kUnknown;
    }
}

// ============================================================================
// CLI DISPLAY Constants
// ============================================================================
namespace cli_display {
    constexpr int kThreadsTruncate = 50;   // cmdThreads max display
    constexpr int kTopTruncate     = 30;   // cmdTop max display
    constexpr int kBenchmarkMaxThreads = 100; // cmdBenchmark thread limit
    constexpr int kLogChunkSize    = 4096; // Tail log read chunk size
    constexpr int kThermalCrit     = 90;   // CRIT display threshold
    constexpr int kThermalHot      = 80;   // HOT display threshold
    constexpr int kThermalWarn     = 60;   // YELLOW display threshold
    constexpr int kTempMilliScale  = 1000; // thermal_zone temp is in milli-degrees
}

// ============================================================================
// PROBE Constants
// ============================================================================
namespace probe {
    constexpr int kMaxAllowedFailures = 5;  // Exit code 1 if failures > 5
    constexpr int kFullCompatMaxWarn  = 2;  // warn <= 2 → fully compatible
    constexpr int kBasicCompatMaxFail  = 3;  // fail <= 3 → basically compatible
}

// ============================================================================
// WATCHDOG Constants (defaults when config missing)
// ============================================================================
namespace watchdog {
    constexpr int kDefaultIntervalSec    = 3;     // Default check interval
    constexpr int kMinIntervalSec        = 1;     // Minimum allowed interval
    constexpr int kDefaultMaxRestarts    = 5;     // Default max restarts in window
    constexpr int kDefaultRestartWindow  = 60;    // Default restart window (seconds)
    constexpr int kDefaultMemWarnKb      = 65536; // Default memory warning threshold (64MB)
    constexpr int kDefaultMigrationLimit = 120;    // Default migration rate limit
    constexpr int kDefaultLoadLimit      = 95;   // P3-4: aligned to Config::safetyLoadLimit default 95 (shipped aatpe.conf work point). Both read SAFETY_LOAD_LIMIT; fallback unified so a missing key can't diverge watchdog vs main loop.
    constexpr int kRestartWaitSec        = 1;      // Wait after spawn before checking
    // Watchdog self-healing & health-action constants
    constexpr int kHeartbeatTimeoutSec   = 30;     // Heartbeat stale timeout (sec)
    constexpr int kPassiveCooldownSec    = 300;   // Passive mode cooldown before recovery (5 min)
    constexpr int kConsecutiveThreshold  = 3;      // Consecutive samples before health action
    constexpr int kTempPassiveDurationSec = 10;    // Temporary passive on mig/load limit (sec)
    // error classification constants
    constexpr int kRapidDeathThresholdSec = 10;    // Daemon dies within this many seconds of start = likely config/binary error
    constexpr int kMaxRapidCrashesBeforeDegrade = 2; // Consecutive rapid deaths → degraded mode
    constexpr int kDegradedCooldownSec = 120;      // Stay in degraded mode for this long before retry
    // P3-55: recovery hysteresis. After leaving PASSIVE/DEGRADED the watchdog
    // enters RECOVERY (control_mode=assist) and only promotes back to NORMAL
    // (control_mode=active) once health has stayed clean CONTINUOUSLY for this
    // many seconds. A fault during RECOVERY drops straight back to PASSIVE/
    // EMERGENCY. This breaks the abnormal->passive->active->abnormal ping-pong.
    constexpr int kRecoveryStableSec = 30;

    // ---- Hotfix v4.1.1: boot-grace suppression + degraded self-heal -----------
    // Real-device evidence (Redmi chopin/D1100, 2026-10-05): during the MIUI boot
    // storm loadavg hit 41. service.sh and the watchdog can race-launch aatped
    // while daemon.pid is not yet written (up to a ~5s window); the two instances
    // flock-collide and one exits < kRapidDeathThresholdSec=10s. The watchdog read
    // that benign double-start exit as TWO rapid deaths -> latched DEGRADED
    // (control_mode=assist) even though the surviving daemon was healthy
    // (PID alive 200s+, heartbeat fresh, crash_loop_detected=0). Worse, DEGRADED
    // had NO exit edge while the daemon stayed alive: it could only clear via the
    // death-path cooldown (which requires killing the healthy daemon), so it got
    // stuck in the assist latch forever.
    //
    // kBootGraceSec: CLOCK_BOOTTIME seconds after boot during which (a) a sub-10s
    // daemon exit is NOT counted toward the rapid-crash tally and NEVER escalates
    // to DEGRADED, and (b) the heartbeat-stale timeout is widened. 120s matches the
    // observed 120s audit cadence and comfortably exceeds the 5s pidfile-ready
    // window plus a slow eBPF load under a 41-load boot storm.
    constexpr int kBootGraceSec = 120;
    // During the boot grace, scheduling delays under the boot storm must not make a
    // still-warming-up daemon look "stuck" and get SIGKILLed. Widen the heartbeat
    // stale limit by this factor (30s -> 90s). Multiplier (not absolute) so an
    // operator-tuned WATCHDOG_HEARTBEAT_TIMEOUT is still respected, just relaxed.
    constexpr int kBootGraceHeartbeatMultiplier = 3;

    // ---- Pure, side-effect-free boundary decisions (G11-002) ----------------
    // These encode the exact comparisons the watchdog's supervision loop in
    // aatpe-watchdog.cpp main() makes. They live here (next to the thresholds
    // they consume) so the host integration test can exercise the REAL boundary
    // contract without forking the watchdog process, and so a threshold change in
    // one place automatically updates both the runtime loop and the test. When the
    // inline comparison in main() changes, update these in lockstep.
    // heartbeat older than timeout => daemon stuck (alive but wedged) -> kill/restart
    inline bool heartbeatIsStale(long hbAgeSec, int timeoutSec) { return hbAgeSec >= timeoutSec; }
    // restartCount beyond the window budget => latch PASSIVE (migrations blocked)
    inline bool shouldLatchPassive(int restartCount, int maxRestarts) { return restartCount > maxRestarts; }
    // still inside the passive cooldown window before recovery may begin
    inline bool inPassiveCooldown(long passiveAgeSec, int cooldownSec) { return passiveAgeSec < cooldownSec; }
    // resident memory above the warn threshold
    inline bool memoryOverWarn(long memKb, int warnKb) { return memKb > warnKb; }
    // migration rate above the safety limit
    inline bool migrationRateOver(long migPerSec, int limitPerSec) { return migPerSec > limitPerSec; }

    // ---- Hotfix v4.1.1 pure boundary decisions ------------------------------
    // still inside the post-boot grace window (bootNowSec = CLOCK_BOOTTIME seconds
    // since boot). During grace the boot-storm / double-start race is expected, so
    // transient sub-threshold deaths must not trip the crash-loop escalation.
    inline bool inBootGrace(long bootNowSec, int graceSec) { return bootNowSec < graceSec; }
    // heartbeat stale limit to enforce this cycle: widened by kBootGrace... only
    // while in boot grace, so a slow eBPF load under loadavg=41 is not SIGKILLed.
    inline int effectiveHeartbeatTimeout(int baseTimeoutSec, bool inBootGrace) {
        return inBootGrace ? baseTimeoutSec * kBootGraceHeartbeatMultiplier : baseTimeoutSec;
    }
    // a death classified rapid_crash counts toward the escalation tally ONLY when
    // we are past boot grace. Inside grace we observe it but neither count nor
    // escalate (the exit is almost certainly the service.sh/watchdog double-start
    // flock race, not a genuinely broken binary/config).
    inline bool countRapidCrash(bool inBootGrace) { return !inBootGrace; }
    // promote to DEGRADED only when NOT in boot grace, not already degraded, and
    // the tally has reached the threshold. (The !inBootGrace guard is the hotfix;
    // the rest preserves the pre-existing escalation contract.)
    inline bool shouldEscalateDegraded(bool inBootGrace, int rapidCount,
                                       int threshold, bool alreadyDegraded) {
        return !inBootGrace && !alreadyDegraded && rapidCount >= threshold;
    }
    // DEGRADED self-heal: while latched, if the daemon is alive AND its heartbeat
    // has been continuously fresh for >= kRecoveryStableSec, the crash loop was a
    // transient (boot race) — clear the latch WITHOUT killing the healthy process.
    // healthyNow must be true AND the continuous-fresh streak must already be
    // counted by the caller; this only compares the streak against the stable
    // window. Returns false the moment the daemon dies or a heartbeat goes stale.
    inline bool degradedSelfHeal(bool healthyNow, long healthyStreakSec, int stableSec) {
        return healthyNow && healthyStreakSec >= stableSec;
    }
    // High-load DEGRADED gating (v4.1.2-hotfix). Expected foreground heavy load
    // (a game / heavy app, jank=0, low PSI) must NOT flap NORMAL<->DEGRADED every
    // few seconds. A high-CPU transition to DEGRADED requires EITHER an abnormal
    // stability signal (active jank / dropped frames) OR the high load to have
    // persisted for a dwell window. Plain, brief, clean foreground high utilisation
    // holds at NORMAL. genuine crash loops / thermal emergency / memory pressure
    // trigger through SEPARATE paths and are unaffected by this gate.
    //   jankAbnormal     : active jank detected this cycle (abnormal signal).
    //   dwellMs          : how long CPU has been continuously over the limit.
    //   dwellThresholdMs : minimum sustained window for a clean high-load degrade.
    inline bool highLoadShouldDegrade(bool jankAbnormal, long long dwellMs,
                                      long long dwellThresholdMs) {
        return jankAbnormal || dwellMs >= dwellThresholdMs;
    }
    // pidfile points to a live, heartbeat-FRESH aatped that THIS watchdog process
    // did not spawn this uptime (weStartedDaemon == daemonStartedAt>0). Adopt it
    // (record its real start time) instead of posix_spawn-ing a second instance —
    // spawning a second aatped on the boot double-start race flock-collides and
    // exits <10s, which the watchdog would then misread as a rapid crash loop.
    inline bool shouldAdoptExistingDaemon(bool daemonAlive, bool heartbeatFresh,
                                          bool weStartedDaemon) {
        return daemonAlive && heartbeatFresh && !weStartedDaemon;
    }
}

// ============================================================================
// FILE PERMISSIONS
// ============================================================================
namespace file_perm {
    // P3-39: actual code uses 0700/0600 (root-only). Was 0755/0644 which
    // contradicted the fchmod/open(0600) calls throughout the daemon.
    constexpr int kDirMode  = 0700;  // Directory creation mode (root-only)
    constexpr int kFileMode = 0600;  // File creation mode (root-only)
}

// ============================================================================
// BENCHMARK Constants (aatped internal)
// ============================================================================
namespace benchmark {
    constexpr int kMaxOps = 1000;  // cmdBenchmark max score computations
}

// ============================================================================
// PATH Constants (shared by daemon and CLI, single source of truth)
// ============================================================================
namespace paths {
    constexpr const char* kRunDir      = "/dev/aatpe";
    constexpr const char* kStateDir    = "/dev/aatpe/state";
    constexpr const char* kConfDir     = "/system/etc/aatpe";
    constexpr const char* kDecisionDir = "/dev/aatpe/decisions";
    constexpr const char* kLogFile     = "/dev/aatpe/aatpe.log";
    constexpr const char* kPidFile     = "/dev/aatpe/daemon.pid";
}

// ============================================================================
// DECISION INTERVAL Constants (adaptive scheduling)
// ============================================================================
namespace decision_interval {
    constexpr int kHighLoadMs    = 50;   // system_load > 80%
    constexpr int kMediumLoadMs  = 100;  // 40% <= system_load <= 80%
    constexpr int kLowLoadMs     = 500;  // system_load < 40% && no foreground app
    constexpr int kJankForcedMs  = 50;   // jank detected -> forced high frequency
}

// ============================================================================
// LOAD THRESHOLD Constants (adaptive decision interval)
// ============================================================================
namespace load_threshold {
    constexpr float kHighLoadPercent   = 80.0f;  // >80% -> high load
    constexpr float kMediumLoadPercent = 40.0f;  // 40-80% -> medium load
}

// ============================================================================
// HMAC KEY Constants (security)
// ============================================================================
namespace hmac {
    constexpr int kKeySizeBytes     = 128;   // increased from 64 for 508-bit entropy
    constexpr int kMinKeyLenChars   = 64;    // minimum valid key length after hex encoding
    constexpr int kRotateIntervalSec = 7 * 24 * 3600;  // 7 days
}

// ============================================================================
// THERMAL Constants (temperature thresholds)
// ============================================================================
namespace thermal_const {
    constexpr int kCpuMaxThresholdC    = 120;  // CPU throttling threshold
    constexpr int kSkinMaxThresholdC   = 45;   // Skin temperature threshold
    constexpr int kBatteryMaxThresholdC = 50;  // Battery temperature threshold
}

// ============================================================================
// LOG Constants (log rotation and levels)
// ============================================================================
namespace log_const {
    // P2-12: unified to 8MB (matches logging::kMaxLogSizeBytes and the actual
    // rotation threshold in aatped_logging.cpp).
    constexpr long long kMaxLogSizeBytes = 8LL * 1024LL * 1024LL;  // 8MB rotation threshold
    constexpr int kLevelError = 0;
    constexpr int kLevelWarn  = 1;
    constexpr int kLevelInfo  = 2;
    constexpr int kLevelDebug = 3;
    constexpr int kLevelTrace = 4;
    constexpr int kDebugSamplePercent = 1;  // 1% sampling for DEBUG level
}

namespace thermal_runtime {
    // ============================================================================
    // P2 Problem12: thermal / emergency LATENCY & FILTER parameters.
    // Centralized here on purpose. They are compile-time constexpr (NOT yet
    // aatpe.conf runtime knobs) because correct values depend on the specific
    // device's sensor behavior and cannot be tuned from host.
    //
    // !!! REAL-DEVICE VALIDATION REQUIRED !!!
    // Every value below was chosen by reasoning, NOT measured on a target phone.
    // On a new device they MUST be validated with on-device logs (thermal_cpu_max,
    // thermal_skin, jank, EMERGENCY transitions) and adjusted:
    //   - too aggressive (short TTL / small debounce / small jump filter) ->
    //     false EMERGENCY / false passive on sensor glitches, plus extra sysfs I/O.
    //   - too conservative (long TTL / large debounce / large jump filter) ->
    //     real overheat delayed, or a legal fast warmup filtered out as a glitch.
    // Do NOT change these values without on-device measurement data.
    // ============================================================================

    // Per-zone temperature jump filter (°C). A zone reading that jumps more than
    // this from its last valid value is treated as a sensor glitch and ignored.
    // Tuning: lower -> catches more glitches but may drop legitimate fast warmups;
    // raise -> keeps more real spikes but lets glitches through.
    constexpr int kMaxTempJumpC = 45;
    // Game-scene override (combat/loading): legit fast warmups are common, relax
    // the filter. Tuning: raise only if real overheat is being suppressed during
    // gameplay; lower if glitches reach EMERGENCY while gaming.
    constexpr int kMaxTempJumpGameC = 50;
    // Thermal read cache TTL (ms). readThermal() runs every daemon loop (~50-100ms)
    // but sysfs thermal_zone scans are slow and temperature moves slowly. Cache
    // the last scan to cut /sys I/O. Tuning: lower -> fresher temp but more sysfs
    // reads; raise -> less I/O but slower overheat reaction.
    constexpr unsigned long long kCacheTtlMs = 1000;
    constexpr int kSpikeTriggerDeltaC = 2;
    // EMERGENCY boot grace (seconds). High-temp triggers are ignored for this long
    // after daemon start because boot sensors are still initializing and report
    // stale/garbage values; without grace a bogus 110 first reading would latch
    // EMERGENCY and force control_mode=passive. Tuning: must exceed sensor
    // stabilization time on the target; raise if devices falsely enter EMERGENCY
    // within the first seconds of boot.
    constexpr int kEmergencyBootGraceSec = 20;
    // EMERGENCY entry debounce (consecutive reads). A high-temp reading only
    // triggers EMERGENCY after N consecutive over-threshold scans (cache TTL=1s,
    // so N seconds). Filters single-read glitches the jump filter cannot catch
    // (first frame has no baseline). Tuning: 1 = no debounce (risky), large N =
    // slow real-overheat reaction. Validate: real overheat must still trigger
    // well before hardware throttle.
    constexpr int kEmergencyDebounceReads = 3;
    // Thermal profile auto-switch restore hysteresis (°C). User profile restored
    // only after temp drops below (thermalProfileThreshold - this). Prevents
    // thrashing at the threshold. Tuning: smaller -> faster restore but more
    // toggling; larger -> stable but stays in battery profile after cooling.
    constexpr int kThermalProfileRestoreHysteresis = 5;

    // ========================================================================
    // Unified thermal hysteresis parameters.
    //
    // All thermal levels (WARNING / CRITICAL / EMERGENCY) follow the same
    // structural model:
    //   entryThreshold → temperature to enter
    //   exitThreshold  = entryThreshold - kHysteresisDeltaC
    //   minHoldSec     → minimum time to stay in the level
    //   minRecoverySec → minimum time below exitThreshold before re-entering
    //
    // These constants define the shared hysteresis band and timing. The actual
    // entry/exit thresholds are computed from the per-level wall values in
    // Config (thermalWarningThreshold / thermalCriticalThreshold /
    // thermalEmergencyThreshold).
    // ========================================================================
    constexpr int kHysteresisDeltaC = 4;         // 4°C hysteresis band (entry - exit)
    constexpr int kMinHoldSec = 10;               // minimum hold time in a level
    constexpr int kMinRecoverySec = 30;           // minimum recovery time before re-entry
}
namespace frame_latency {
    constexpr int kJankMaxMultiplierNum = 3;
    constexpr int kJankMaxMultiplierDen = 2;
    constexpr int kSfLatCacheTtlSec = 1;
    // ROUND-007c: consecutive jank samples required to arm the bounded boost vote.
    // Debounces a single dropped frame; 2 frames ~= one PELT window (32ms).
    constexpr int kJankBoostConsecutive = 2;
}

// ============================================================================
// HMAC Security Constants
// ============================================================================
namespace hmac_const {
    constexpr int kTimestampWindowSec = 30;   // Replay protection: reject ts older than 30s
    constexpr int kTimestampSkewSec = 5;       // Allow up to 5s clock skew (future ts)
    constexpr int kKeyLengthHex = 64;           // 256-bit key as 64 hex chars
    constexpr int kDaemonTokenLength = 64;      // Daemon auth token length
}

// ============================================================================
// Hybrid Learning Constants
// ============================================================================
namespace hybrid_const {
    constexpr float kFineTuneWeight = 0.20f;    // Hybrid fine-tune influence weight
    constexpr float kPredictNormOffset = 1.0f;   // (adj + offset) / 2 maps [-1,1] to [0,1]
    constexpr int kFeatureCount = 15;             // Number of hybrid features
    constexpr float kAvgRunUsScale = 50000.0f;   // Scale factor for avg_run_us feature
    constexpr float kNrSwitchesScale = 1000.0f;  // (legacy) cumulative nr_switches divisor
    // R005c: f[2] is now SWITCH RATE (switches/sec), not a cumulative counter.
    // A cumulative nr_switches saturates to 1 for any long-lived thread, making the
    // feature a useless constant. We normalize (delta_nrSwitches / elapsed_sec)
    // by a conservative saturation rate. Basis: on a mobile SoC a typical UI/render/
    // game thread voluntarily+involuntarily switches on the order of 50-250/s; a
    // pathologically jittery thread peaks near ~300-500/s. Saturating at 300/s keeps
    // normal threads in [0,1) and flags only the most chattery as 1.0.
    constexpr float kSwitchesPerSecScale = 300.0f;
}

// ============================================================================
// eBPF Ring Buffer Constants
// ============================================================================
namespace ebpf_ring_const {
    constexpr int kDefaultPageCount = 32;         // Default perf ring mmap pages (128KB, v83优化)
    constexpr int kDefaultPageSize = 4096;        // Fallback page size
}

// ============================================================================
// FALLBACK CENTRAL REGISTRY
// ----------------------------------------------------------------------------
// Historically each subsystem defined its own degraded-mode constant inline:
//   - uevent fallback ......... aatped.cpp checkUeventFallback() (5000ms)
//   - topology fallback ...... aatped.cpp detectCpuTopology() (sysconf)
//   - thermal fallback ....... thermal.cpp readThermal() (no CPU zone -> scan all)
//   - eBPF loader fallback ... aatpe_ebpf.cpp (embedded -> syscall -> bpftool -> polling)
//   - cpuset fallback mask ... aatped.cpp buildDynamicFallbackMask()
//   - log path fallback ...... aatped.cpp (primary log -> /data/local/tmp)
// These are NOT refactored here (no large rewrite per 问题40); this namespace is
// the single documented home for the trigger conditions + tunables, and the
// obvious magic numbers are wired to the names below so behavior stays the same
// while becoming grep-able.
namespace fallback {
    // uevent (netlink) never fires -> drop to 500ms sysfs polling.
    // Trigger: after daemon start, if 0 uevent events within this window.
    constexpr long long kUeventActivateAfterMs = 5000;
    // Polling cadence once the uevent fallback is active.
    constexpr long long kUeventPollIntervalMs  = 500;

    // Topology: if /sys/devices/system/cpu is unreadable, use sysconf(online
    // CPUs) as a degraded view and retry /sys on later cycles (per-cycle).
    constexpr int kTopologySysconfRetryEveryNLoops = 1; // retry every loop (cheap)

    // Thermal: if no CPU-typed zone found, scan all thermal zones for the first
    // readable value in [-50,200]C; if none, disable thermal policy (do not
    // latch on a bogus reading). See thermal.cpp readThermal().

    // eBPF loader degradation order:
    //   embedded_syscall -> syscall_precompiled -> bpftool_precompiled ->
    //   polling (daemon still schedules; just not event-driven).
    // Any single attach failure must NOT leave the system half-attached; the
    // loader tears down partial state and steps down one notch (see 问题51).
}
} // namespace aatpe

#endif // AATPE_CONSTANTS_H

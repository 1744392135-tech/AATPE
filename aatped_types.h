// AATPE - Core type definitions (extracted from aatped.h)
#ifndef AATPED_TYPES_H
#define AATPED_TYPES_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <atomic>
#include <ctime>
#include <cstdint>
#include <sched.h>
#include "aatpe-constants.h"

// enum types for refresh rate mode and profile
enum class RefreshRateMode { UNKNOWN, PEAK, CURRENT, AUTO };
enum class ProfileType { UNKNOWN, BALANCED, GAMING, BATTERY, THERMAL };

// Unified System State Machine
enum class SystemState {
    NORMAL    = 0,
    DEGRADED  = 1,
    PASSIVE   = 2,
    EMERGENCY = 3
};

struct StateHistoryEntry {
    SystemState fromState;
    SystemState toState;
    time_t timestamp;
    char reason[128];
};

static constexpr int kMaxStateHistory = 32;
static constexpr int kDegradedRecoveryCycles = 3;
static constexpr int kEmergencyRecoverySec = 30;
// minimum dwell (ms) after committing a state transition before
// another NON-emergency transition is allowed. Stops the EMERGENCY<->DEGRADED
// ping-pong (thermal_emergency vs high_cpu_usage) that pinned the daemon at
// ~85% CPU. Escalation to EMERGENCY always bypasses this (see transitionToState).
static constexpr long long kStateDwellMs = 5000;

// Pool identifier for per-pool ML and placement decisions
enum class CpuPool {
    UNKNOWN = -1,
    LITTLE = 0,
    MID = 1,
    BIG = 2
};

// ROUND-004 B1: generic cluster tier. Ordered from weakest to strongest.
// PRIME=3 is the highest tier (typically the single lead core on 1+3+4 / 1+4+3
// flagships). The legacy CpuPool enum is retained unchanged for compatibility;
// CpuTier adds the missing "prime" layer on top of the new cluster model.
enum class CpuTier {
    UNKNOWN = -1,
    LITTLE  = 0,
    MID     = 1,
    BIG     = 2,
    PRIME   = 3
};

// ROUND-004 B1: one physical CPU cluster (shared voltage/frequency domain, or a
// detected natural group). cpus is ordered ascending by cpu id.
struct CpuCluster {
    std::vector<int> cpus;
    int      capacity = 0;   // representative cpu_capacity of the cluster
    int      minKhz   = 0;   // min cpuinfo_max_freq in cluster
    int      maxKhz   = 0;   // max cpuinfo_max_freq in cluster
    int      policyId = -1;   // cpufreq policy id if homogeneous domain, else -1
    CpuTier tier     = CpuTier::UNKNOWN;
};

// Thread classification
enum ThreadRole {
    ROLE_S0_SYSTEM = 0,
    ROLE_S1_UI = 1,
    ROLE_S2_RENDER = 2,
    ROLE_S3_GAME = 3,
    ROLE_S4_THROUGHPUT = 4,
    ROLE_S5_BACKGROUND = 5
};

struct ThreadRule {
    std::string procPattern;
    std::string threadPattern;
    ThreadRole role;
    std::string subtype;
    int priority;
};

// P2-1 fix: default affinity = all CPUs (no CPU restriction). If a thread is
// ever rolled back before its real original mask was successfully saved,
// pinning to all-CPUs is a no-op instead of replaying uninitialized garbage.
// C-021/OPT-04: cache as static const - was iterating CPU_SETSIZE(1024) on every
// ThreadInfo construction. Now computed once at first use and reused.
inline const cpu_set_t& aatpeDefaultAllCpuSet() {
    static const cpu_set_t kAllCpuSet = []() {
        cpu_set_t s; CPU_ZERO(&s);
        for (int i = 0; i < CPU_SETSIZE; i++) CPU_SET(i, &s);
        return s;
    }();
    return kAllCpuSet;
}

struct ThreadInfo {
    int pid = 0;
    int tid = 0;
    unsigned long long starttime = 0;
    std::string pname;
    std::string tname;
    ThreadRole role = ROLE_S5_BACKGROUND;
    std::string subtype;
    int priority = aatpe::thread_classify::kDefaultPriority;
    bool protect = false;
    unsigned long long runTime = 0;
    unsigned long long nrSwitches = 0;
    int currentCpu = 0;
    int nice = 0;
    cpu_set_t originalAffinity = aatpeDefaultAllCpuSet();
    bool affinityModified = false;
    // multi-feature classifier confidence in [0,100]. Threads below
    // threadRoleConfidenceThreshold (default 50) are never aggressively migrated
    // or RT-boosted — the placement layer treats them as low-certainty and keeps
    // the conservative heuristic placement instead of force-migrating on a guess.
    int roleConfidence = 0;
    // set by onFork() when a thread is only a placeholder (starttime=0,
    // empty comm, role=S5_BACKGROUND). The next incremental scanThreads() sees this
    // flag and forces a full populateNewThread() (read comm/role/schedstat/starttime
    // from /proc) instead of treating the placeholder as a fully-tracked thread.
    bool needsHydration = false;
    // P2-17+19: tiered scan bookkeeping.
    // idleRefreshes: consecutive refresh cycles where nrSwitches did NOT change
    //   (thread made no sched switches -> idle). Once >= kIdleSkipThreshold we
    //   skip the schedstat open() on subsequent cycles (use cached metrics).
    // longIdleRescan: counter used to force one schedstat read every
    //   kLongIdleRescan cycles even while idle, so we detect when an idle thread
    //   wakes up.
    int idleRefreshes = 0;
    int longIdleRescan = 0;
    // R005c: switches/sec derived at refresh from (nrSwitches_delta / elapsed_sec).
    // Feeds hybrid feature f[2] as a RATE (was cumulative nr_switches/1000, which
    // saturated to 1 for long-lived threads). lastSwitchStampNs = steady_clock ns
    // of the previous refresh, used to measure the interval.
    double switchesPerSec = 0.0;
    uint64_t lastSwitchStampNs = 0;
};

// CPU metrics
struct CpuMetrics {
    int capacity = aatpe::topology::kDefaultCapacity;
    int curFreq = 0;
    int maxFreq = 1;
    int freqPct = 0;
    int util = 0;
    int temp = 0;
};

// Per-App configuration override
struct PerAppConfig {
    std::string packageName;
    std::string profile;
    int decisionIntervalMs = 0;
    int maxFpsHint = 0;
    bool valid = false;
};

// Learnable scoring weights
struct DynamicScoreWeights {
    float capacityMul = 1.0f;
    float latencyMul = 1.0f;
    float thermalMul = 1.0f;
    float contentionMul = 1.0f;
    float powerMul = 1.0f;
    bool valid = false;
};

// Schedstat CPU data
struct SchedstatCpuData {
    int cpuId = -1;
    unsigned long long running = 0;
    unsigned long long waiting = 0;
    unsigned long long timeslices = 0;
};

// Migration reject reason bitmask
enum MigrateReason : uint32_t {
    REASON_NONE = 0,
    REASON_PROTECTED = 1 << 0,
    REASON_PASSIVE = 1 << 1,
    REASON_KERNEL = 1 << 2,
    REASON_UNMIGRATABLE = 1 << 3,
    REASON_NO_CPU = 1 << 4,
    REASON_ALREADY_OPTIMAL = 1 << 5,
    REASON_HIGH_COST = 1 << 6,
    REASON_LOW_CONF = 1 << 7,
    REASON_COOLDOWN = 1 << 8,
    REASON_RATE_LIMIT = 1 << 9,
    REASON_CPUSET = 1 << 10,
    REASON_INTERVAL = 1 << 11,
    REASON_QUIET = 1 << 12,
};

// Unified Decision Fusion
struct DecisionFusion {
    float heuristic_score   = 0.0f;
    float ml_score          = 0.0f;
    float hybrid_score      = 0.0f;
    float history_score     = 0.0f;
    float dynamic_weight_adjustment = 0.0f;
    float w_heuristic = 0.0f;
    float w_ml        = 0.0f;
    float w_hybrid    = 0.0f;
    float w_history   = 0.0f;
    float final_score = 0.0f;
    float contrib_heuristic = 0.0f;
    float contrib_ml        = 0.0f;
    float contrib_hybrid    = 0.0f;
    float contrib_history   = 0.0f;
};

struct FusionWeightAccumulator {
    std::atomic<long long> count{0};
    std::atomic<double> sum_heuristic{0.0};
    std::atomic<double> sum_ml{0.0};
    std::atomic<double> sum_hybrid{0.0};
    std::atomic<double> sum_history{0.0};
    void record(const DecisionFusion& f);
    void reset();
    long long getAverageWeights(float& outHeuristic, float& outMl,
                                float& outHybrid, float& outHistory) const;
};

// C-020/OPT-01: Decision.action as enum class (was std::string heap alloc per decision).
enum class DecisionAction { KEEP, MIGRATE };
inline const char* decisionActionToString(DecisionAction a) {
    return a == DecisionAction::MIGRATE ? "MIGRATE" : "KEEP";
}

struct Decision {
    // C-020/OPT-01: enum class instead of std::string — no heap alloc per decision.
    DecisionAction action = DecisionAction::KEEP;
    int targetCpu = -1;
    CpuPool targetPool = CpuPool::UNKNOWN;
    int preferredCpu = -1;
    // P1-15: cpu_set_t must be zero-initialized; reading uninitialized bits in
    // sched_getaffinity/CPU_COUNT is UB. Default to "all CPUs" via the existing
    // helper so callers see a valid mask even if they forget to set it.
    cpu_set_t requestedMask = aatpeDefaultAllCpuSet();
    cpu_set_t effectiveMask = aatpeDefaultAllCpuSet();
    bool applySuccess = false;
    bool effectiveMaskValid = false;
    bool actualPoolMatch = false;
    int score = 0;
    int confidence = 0;
    std::string reason;
    uint32_t rejectReasons = 0;
    aatpe_ml::ThreadFeatures mlFeatures;
    bool mlActive = false;
    DecisionFusion fusion;
    // P2: decidePlacement records the isKernelThread() verdict so applyPlacement
    // (and WebUI consumers) do not re-run the ~80-name prefix scan + /proc/cmdline
    // probe for the same thread on the very next call.
    bool isKernel = false;
};

// History
struct HistoryEntry {
    int preferredCpu = -1;
    int score = 0;
    int samples = 0;
    time_t lastUpdate = 0;
};

// WebUI
struct WebuiDecision {
    time_t timestamp;
    int tid;
    std::string name;
    std::string role;
    int from_cpu;
    int to_cpu;
    int score;
    int confidence;
    std::string type;
    std::string requested_pool;
    std::string effective_mask;
    int actual_cpu = -1;
};

// Migration budget
struct MigrationBudget {
    int maxPerSec = 10;
    int maxPerCycle = 5;
    int usedThisSec = 0;
    int usedThisCycle = 0;
    time_t windowStart = 0;
    bool tryAcquire(bool frameCritical);
};

// Config struct
#include <vector>
struct Config {
    std::string controlMode = "assist";
    int decisionInterval = 200;
    // ------------------------------------------------------------------
    // event-driven cadence tunables.
    //   decisionMinIntervalMs — minimum gap between two full placement decisions
    //     after an important-event wake (fork/exec/exit via self-pipe). If the
    //     main loop is woken but the last decision ran < this many ms ago
    //     (touch-event burst / fork storm), it sleeps the remainder before
    //     deciding. Default 33ms (~30 decisions/sec cap).
    //   decisionIdleIntervalMs — poll() timeout when NO important event is
    //     pending. The main loop blocks here until an event arrives or this
    //     elapses; this is the background re-balance cadence. Default 200ms.
    // Both are clamped to sane ranges in loadConfig()/validateConfig().
    // ------------------------------------------------------------------
    int decisionMinIntervalMs = 33;
    int decisionIdleIntervalMs = 200;
    int confidenceThreshold = 5;          // A2-012: aligned to shipped aatpe.conf (was 8)
    int migrationCostThreshold = 3;       // A2-012: aligned to shipped aatpe.conf (was 6)
    int maxMigrationsPerSec = 80;
    // ROUND-014 P3: tunable boost level calibration (aatpe.conf).
    int uclampPassivePct = 25;       // UCLAMP_PASSIVE_PCT
    int uclampCriticalMinPct = 80;  // UCLAMP_CRITICAL_MIN_PCT
    int uclampCriticalMaxPct = 100;  // UCLAMP_CRITICAL_MAX_PCT
    int uclampAssistMinPct = 70;    // UCLAMP_ASSIST_MIN_PCT
    int uclampSfPct = 20;            // UCLAMP_SF_PCT
    int inputBoostMs60 = 80;         // INPUT_BOOST_MS_60
    int inputBoostMs120 = 50;        // INPUT_BOOST_MS_120
    int inputBoostMs144 = 40;        // INPUT_BOOST_MS_144
    int schedtuneCriticalBoost = 60; // SCHEDTUNE_CRITICAL_BOOST
    int schedtunePassiveBoost = 20;  // SCHEDTUNE_PASSIVE_BOOST
    // v4.2 Step0 预占（Batch A 使用）：promote 时叠加的 schedtune.boost 辅助档。
    // 默认 50；当前无任何代码消费此字段，保持默认即与 v4.1.1 逐位一致（无运行态行为变化）。
    int schedtuneAssistBoost = 50;  // SCHEDTUNE_ASSIST_BOOST (range 0-100)
    int maxMigrationsPerCycle = 20;
    bool thermalEnabled = true;
    // v6.0.0 round4c: user-tunable feature switches (optimization class only;
    // security/thermal protection stays forced-ON). Defaults ON per user policy.
    bool c4FreezerEnabled = true;      // C4 后台冻结，默认 ON（带安全保护：前台/音频/IM白名单不冻）
    int thermalLimitCpu = 120;
    int thermalLimitSkin = 120;
    int thermalLimitBattery = 120;
    int thermalEmergency = 120;
    // ------------------------------------------------------------------
    // 温控墙三档（WARNING < CRITICAL < EMERGENCY）—— 默认值构成单调递增阶梯。
    //
    // 单位：°C（thermal.cpp readThermal() 已做三级单位自动检测——
    //   >1500 → 毫摄氏度 /1000；>150 → 分摄氏度 /10；否则直接 °C——
    //   所以 thermal_zone 报告的毫摄氏度读数会被归一到 °C 后再与下面
    //   阈值比较，无需在此处做单位换算）。
    //
    // 三档语义（必须严格单调递增 WARNING < CRITICAL < EMERGENCY；
    // validateConfig() 会校验此关系，不满足时自动拉平）：
    //   WARNING  —— 温度达到此值时自动切到 thermal 保护 profile；
    //               默认 80（与旧字段 thermalProfileThreshold 工作点一致）。
    //   CRITICAL —— CPU 温控压力墙，headroom / migration pressure 计算基准；
    //               默认 90（比 WARNING 高 10°C，留出压力缓冲带）。
    //   EMERGENCY—— 达到此值连续 N 次即 latch 进入 EMERGENCY 状态（全局停止
    //               普通迁移，control_mode 强制 passive）；默认 120（=禁用，
    //               设备不可能达到 120°C，需用户在 aatpe.conf 显式配置才生效）。
    //
    // 兼容策略：若下列三个字段 <= 0，则 thermal.cpp 回退使用旧字段
    //   thermalProfileThreshold / thermalLimitCpu / thermalEmergency，
    //   保证既有 aatpe.conf 与单元测试（显式赋值旧字段）行为不变。
    // ------------------------------------------------------------------
    int thermalWarningThreshold   = 80;  // WARNING 档（°C）：切 thermal 保护 profile
    int thermalCriticalThreshold  = 90;  // CRITICAL 档（°C）：温控压力墙基准
    int thermalEmergencyThreshold = 120;  // EMERGENCY 档（°C）：latch 紧急状态
    int frameAware = 1;
    int targetFrameTime = 16667;
    int frameCriticalThreshold = 12000;
    float jankThresholdPercent = 3.0f;
    bool learningEnabled = true;
    int historyWeight = 30;
    int learningDecay = 95;
    int minSamplesForLearning = 5;
    int threadScanInterval = 5;
    int maxTrackedThreads = 512;
    int ignoreSystemThreads = 1;
    bool watchdogEnabled = true;
    int watchdogInterval = 3;
    // parsed so loadConfig() doesn't WARN on these keys (the
    // standalone aatpe-watchdog process reads them itself; aatped doesn't use them).
    int watchdogMaxRestarts = 5;
    int watchdogRestartWindow = 60;
    bool federatedEnabled = true;  // v6.0.0 round4c: default ON (local aggregate/export)
    int safetyMigrationLimit = 120;
    // 500us (0.5ms) was unrealistically tight. avgLat here is the
    // SurfaceFlinger frame render latency; a healthy 60/120Hz panel already sits
    // at 1-3ms, so the old 500us threshold gated normal frame pacing as
    // "unsafe" and froze all regular placement. Raise to 5000us (5ms): only a
    // genuine multi-ms stall trips the latch. Emergency migration is unaffected
    // (placement.cpp applyPlacement/decidePlacement already bypass this when
    // emergency=true).
    // 5000us was far too strict — a normal 60fps frame is ~16.6ms and
    // sf latency commonly sits at 10-15ms, so the gate fired on healthy frames.
    // Raise to 20000us (20ms): only gate when latency is genuinely bad
    // (sub-30fps). emergency migrations still bypass this gate.
    int safetyLatencyLimit = 20000;
    int safetyLoadLimit = 95;              // A2-012: aligned to shipped aatpe.conf (was 120; conf=95 is the conservative work point)
    int cpuUsageLimit = 85;
    int logLevel = 2;
    int logDecisions = 1;
    int logThermal = 1;
    int logFrame = 1;
    int autoProfile = 1;
    int gameDetection = 1;
    std::string refreshRateMode = "current";  // A2-012: aligned to shipped aatpe.conf (was "peak")
    int batterySaverThreshold = 20;
    int thermalProfileThreshold = 80;      // A2-012: aligned to shipped aatpe.conf (was 120; conf=80 work point)
    int layerCriticalThreshold = 150;
    int daemonMemoryWarnKb = 65536;
    bool mlLearningEnabled = true;
    // v4.5.0: shadow sampling in observe-only / auxiliary (passive) mode. When on
    // (default), passive mode still records feature->outcome pairs into a separate
    // bounded shadow buffer (no migration applied, no live-model training). When
    // off, observe-only mode records nothing (old behavior). Independent of
    // mlLearningEnabled: shadow recording is pure telemetry.
    bool mlShadowSampling = true;
    float mlMaxAlpha = 0.3f;
    float mlInitialLR = 0.01f;
    float mlDecay = 0.999f;
    int mlSaveInterval = 30;
    float mlWeightClamp = 10.0f;
    bool mlHybridEnabled = false;
    bool hmacRequired = true;
    bool batteryAgingProtection = true;
    // A2-012 (deliberate divergence, NOT aligned to conf): aatpe.conf ships
    // BATTERY_AGING_PROTECTION=0 by the documented v88 product decision, but the
    // Config default stays true. Rationale: this default only applies when conf is
    // absent/corrupt; erring on the side of enabling a battery-safety protection on
    // a degraded boot is the conservative fail-safe. The shipped conf overrides to 0.
    bool mlColdStartProtection = true;
    int mlMinSamples = 2000;
    // A2-012 (deliberate divergence, NOT aligned to conf): aatpe.conf ships
    // ENABLE_HTTP_SERVER=1, but the Config default stays false. Rationale: opening a
    // network listener by default on a missing/corrupt-conf boot is a network-attack
    // surface; the daemon must NOT bind HTTP just because conf failed to parse. The
    // shipped conf opts in explicitly. (fail-closed: never auto-expose the control
    // plane.)
    bool enableHttpServer = false;
    bool mlFrameTimeOutcome = true;
    float mlFrameTimeWeight = 0.5f;
    // v4.2 Step0 预占（Batch B 使用）：ML 鲁棒裁剪 / 漂移再学习 / ε-贪心探索。
    // 默认全 off（0 / 0 / 0.0）：当前无任何代码消费这三个字段，保持默认即与
    // v4.1.1 逐位一致（纯长期权重 / 不探索 ≡ v4.1.1 行为，无运行态变化）。
    int mlOutcomeMadFilter = 0;     // ML_OUTCOME_MAD_FILTER 0=关(v4.1.1), 1=开 MAD k=3 裁剪
    int mlDriftRelearn = 0;         // ML_DRIFT_RELEARN 0=关, 1=开漂移再学习
    double mlEpsilonGreedy = 0.0;   // ML_EPSILON_GREEDY 0=关不探索, 允许范围 0.0-0.05
    std::string littlePool;
    std::string midPool;
    std::string bigPool;
    // ROUND-004: manual cpuset override for the PRIME (lead) cluster. Empty = auto.
    // e.g. "7". Validated at topology detection: must be a legal online cpu and
    // must not overlap the BIG pool; on violation we LOG_WARN and fall back to
    // auto-detected clustering.
    std::string primePoolCfg;
    std::string packageWhitelist;
    std::vector<std::string> gamePackages;
    bool ebpfEnabled = true;
    int ebpfRingPages = 128;  // A2-012: aligned to shipped aatpe.conf (was 64; conf comment documents 64 dropped events under load, 128 is the corrected work point; dynamic ring adjust scales further)
    bool ebpfPollFallback = true;

    // ------------------------------------------------------------------
    // 算力优先分配（aatped_priority.cpp）
    //   PRIORITY_BOOST_ENABLED: 1=开启 0=关闭。开启后 placement 成功迁移
    //     UI/RENDER/GAME 线程时，通过 sched_setattr() 给关键线程设置
    //     SCHED_FIFO 实时优先级（1~3，故意不高避免饿死系统）+ uclamp
    //     (util_min=50 保证至少 50% 算力，util_max=1024 不设上限)。
    //   PRIORITY_BOOST_LEVEL: 1=轻度(RR,prio=1) 2=中度(FIFO,prio=2)
    //     3=激进(FIFO,prio=3)。权限不足(EPERM)时自动降级为 nice 值调整。
    // ------------------------------------------------------------------
    bool priorityBoostEnabled = true;
    int  priorityBoostLevel   = 1;

    // ------------------------------------------------------------------
    // 智能绑核（placement.cpp computeCoreSuitability）
    //   SMART_PLACEMENT_ENABLED: 1=开启核心适合度评分融合到候选 CPU 排序。
    //   CORE_AFFINITY_WEIGHT / LOAD_BALANCE_WEIGHT / TEMPERATURE_WEIGHT:
    //     三项权重，求和应为 1.0（代码内会归一化）。
    //     核心类型(大/中/小核) × 当前负载 × 温度 × 同游戏线程相邻核心亲和性。
    // ------------------------------------------------------------------
    bool smartPlacementEnabled = true;
    float coreAffinityWeight = 0.3f;
    float loadBalanceWeight  = 0.4f;
    float temperatureWeight  = 0.3f;
    // ADRC (Active Disturbance Rejection Control) parameters
    bool adrcEnabled = true;
    float adrcH = 0.05f;           // sample period (seconds)
    float adrcR = 50.0f;            // tracking speed factor
    float adrcBeta01 = 100.0f;     // ESO bandwidth 1
    float adrcBeta02 = 300.0f;     // ESO bandwidth 2
    float adrcBeta03 = 1000.0f;    // ESO bandwidth 3
    float adrcBeta1 = 10.0f;       // NLSEF gain 1
    float adrcBeta2 = 0.5f;        // NLSEF gain 2
    float adrcAlpha1 = 0.75f;      // NLSEF nonlinear power 1
    float adrcAlpha2 = 1.25f;      // NLSEF nonlinear power 2
    float adrcDelta = 0.01f;       // linear interval threshold
    float adrcB0 = 1.0f;           // control gain
    float adrcUrgencyThreshold = 0.5f;
    float adrcRiskThreshold = 0.6f;
    float adrcClampFrameRatio = 0.7f;
    float adrcWeightBase = 0.4f;   // base ADRC fusion weight
    float adrcMlWeightBase = 0.6f; // base ML fusion weight

    // ========================================================================
    // (round-2 real-device data-driven tuning)
    // ------------------------------------------------------------------------
    // #34 placement hysteresis: only migrate when the NEW best CPU score beats
    // the currently-settled CPU score by this percent, and hold a thread on a
    // CPU for at least migrationCooldownMs before it may be pulled elsewhere.
    // WHY: on Dimensity 1100 (big=prime A78, mid=A78, little=A55) the fused
    // scores of the two A78 mid cores are within ~1-2% under variable load, so
    // a thread ping-ponged between cpu4 and cpu5 every decision cycle. The
    // hysteresis band + minimum dwell converts micro-fluctuations into a stable
    // placement until a genuinely better CPU emerges.
    // ========================================================================
    int migrationHysteresisPercent = 5;   // #34: newScore must beat settledScore by 5%
    int migrationCooldownMs        = 500; // #34: min dwell (ms) after a migration

    // ------------------------------------------------------------------
    // P2-2: migration dwell (seconds) — minimum time a thread must sit on
    // its assigned CPU before applyPlacement() will move it again.
    // Distinct from decisionCooldownMs (#34 above, which gates the *decision*
    // frequency) and from emergencyOverride (which bypasses this dwell when
    // emergency=true). Default 5s matches the old hard-coded
    // kMinMigrationIntervalSec; configurable via MIGRATION_DWELL_SEC (1-60).
    // ------------------------------------------------------------------
    int migrationDwellSec = 5;

    // #32 migration cost calibration from real schedstat: little<->big cross-pool
    // migration costs ~3x a same-pool hop (cache cold on the big L2 + runqueue
    // shift). Same-pool penalty stays at 1.0.
    float migrationPenaltyBigLittle = 3.0f;
    float migrationPenaltySame       = 1.0f;

    // #29/#30 multi-feature thread role classifier: every classified thread gets
    // a confidence score in [0,100]. Threads with confidence below this threshold
    // are NEVER aggressively migrated (no force-migrate, no RT boost); they keep
    // the conservative heuristic placement.
    // lowered default 50 -> 30. On-device the classifier
    // assigns ~20-45 confidence to in-app vendor threads (binder/worker) inside the
    // foreground game, so avg placement confidence sat at ~9 vs threshold 50 and
    // blocked essentially every aggressive migration (23万 decisions -> 30
    // migrations). 30 keeps the guardrail against truly unknown threads (conf<20)
    // while letting known foreground critical threads through. Foreground-app
    // critical threads additionally get a hard relaxation in decidePlacement.
    int threadRoleConfidenceThreshold = 30;

    // #38 ML anomaly drift detection: if the ML model keeps voting one pool as
    // best while real outcomes (frame time / run ratio) keep degrading for
    // N consecutive windows, automatically cut alpha and fall back to heuristic.
    bool mlDriftDetectionEnabled = true;
    int  mlDriftWindow           = 10;   // consecutive bad outcomes before latch

    // #39 ML topology binding: persist a topology hash (cpu count / capacity /
    // cpuset layout) inside model.dat; on load, mismatch forces cold-start so a
    // model trained on one ROM/device never pollutes another.
    bool mlTopologyBindingEnabled = true;

    // #28 refresh-rate event-driven refresh: listen for SurfaceFlinger display-mode
    // changes (inotify on the refresh-rate sysfs node) and refresh immediately,
    // with the 10s poll kept as a backstop.
    bool refreshRateEventDriven = true;

    // ========================================================================
    // round-3 architectural optimizations
    // ========================================================================

    // #77 state/cache TTL: sensitive history entries older than this (hours)
    // are auto-cleaned to prevent /dev/aatpe from growing without bound.
    int stateCacheTtlHours = 24;

    // #35 placement scoring three-value output: migration requires BOTH
    // gain > placementMinGainPercent AND confidence > placementMinConfidence.
    int placementMinGainPercent = 5;       // relative gain threshold (%)
    int placementMinConfidence  = 50;      // absolute confidence threshold (0-100)

    // #33 minimum useful capacity ratio: threads with util above this fraction
    // of max capacity are blocked from migrating to pools whose capacity is
    // less than ratio * maxCapacity. E.g. UI threads at >30% util cannot go to
    // little cores (capacity < 0.3 * 1024 ≈ 307).
    float minUsefulCapacityRatio = 0.3f;
};

#include <deque>

// GPU / Audio / Scene state structs
struct GpuState {
    bool available = false;
    int utilization = -1;
    std::string type = "unknown";
};

struct AudioState {
    bool modeActive = false;
    int isolatedCpu = -1;
    int serverPid = -1;
};

struct SceneState {
    int currentScene = 0;
};

// Key path tracing stats
struct TracingStats {
    long long count = 0;
    long long sumUs = 0;
    long long minUs = 0;
    long long maxUs = 0;
    std::deque<long long> samples;
};

#endif // AATPED_TYPES_H

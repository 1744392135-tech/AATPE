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

enum class RefreshRateMode { UNKNOWN, PEAK, CURRENT, AUTO };
enum class ProfileType { UNKNOWN, BALANCED, GAMING, BATTERY, THERMAL };

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
static constexpr long long kStateDwellMs = 5000;

enum class CpuPool {
    UNKNOWN = -1,
    LITTLE = 0,
    MID = 1,
    BIG = 2
};

enum class CpuTier {
    UNKNOWN = -1,
    LITTLE  = 0,
    MID     = 1,
    BIG     = 2,
    PRIME   = 3
};

struct CpuCluster {
    std::vector<int> cpus;
    int      capacity = 0;
    int      minKhz   = 0;
    int      maxKhz   = 0;
    int      policyId = -1;
    CpuTier tier     = CpuTier::UNKNOWN;
};

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
    int roleConfidence = 0;
    bool needsHydration = false;
    int idleRefreshes = 0;
    int longIdleRescan = 0;
    double switchesPerSec = 0.0;
    uint64_t lastSwitchStampNs = 0;
};

struct CpuMetrics {
    int capacity = aatpe::topology::kDefaultCapacity;
    int curFreq = 0;
    int maxFreq = 1;
    int freqPct = 0;
    int util = 0;
    int temp = 0;
};

struct PerAppConfig {
    std::string packageName;
    std::string profile;
    int decisionIntervalMs = 0;
    int maxFpsHint = 0;
    bool valid = false;
};

struct DynamicScoreWeights {
    float capacityMul = 1.0f;
    float latencyMul = 1.0f;
    float thermalMul = 1.0f;
    float contentionMul = 1.0f;
    float powerMul = 1.0f;
    bool valid = false;
};

struct SchedstatCpuData {
    int cpuId = -1;
    unsigned long long running = 0;
    unsigned long long waiting = 0;
    unsigned long long timeslices = 0;
};

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

enum class DecisionAction { KEEP, MIGRATE };
inline const char* decisionActionToString(DecisionAction a) {
    return a == DecisionAction::MIGRATE ? "MIGRATE" : "KEEP";
}

struct Decision {
    DecisionAction action = DecisionAction::KEEP;
    int targetCpu = -1;
    CpuPool targetPool = CpuPool::UNKNOWN;
    int preferredCpu = -1;
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
    bool isKernel = false;
};

struct HistoryEntry {
    int preferredCpu = -1;
    int score = 0;
    int samples = 0;
    time_t lastUpdate = 0;
};

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

struct MigrationBudget {
    int maxPerSec = 10;
    int maxPerCycle = 5;
    int usedThisSec = 0;
    int usedThisCycle = 0;
    time_t windowStart = 0;
    bool tryAcquire(bool frameCritical);
};

#include <vector>
struct Config {
    std::string controlMode = "assist";
    int decisionInterval = 200;
    int decisionMinIntervalMs = 33;
    int decisionIdleIntervalMs = 200;
    int confidenceThreshold = 5;
    int migrationCostThreshold = 3;
    int maxMigrationsPerSec = 80;
    int uclampPassivePct = 25;
    int uclampCriticalMinPct = 80;
    int uclampCriticalMaxPct = 100;
    int uclampAssistMinPct = 70;
    int uclampSfPct = 20;
    int inputBoostMs60 = 80;
    int inputBoostMs120 = 50;
    int inputBoostMs144 = 40;
    int schedtuneCriticalBoost = 60;
    int schedtunePassiveBoost = 20;
    int schedtuneAssistBoost = 50;
    int maxMigrationsPerCycle = 20;
    bool thermalEnabled = true;
    bool c4FreezerEnabled = true;
    int thermalLimitCpu = 120;
    int thermalLimitSkin = 120;
    int thermalLimitBattery = 120;
    int thermalEmergency = 120;
    int thermalWarningThreshold   = 80;
    int thermalCriticalThreshold  = 90;
    int thermalEmergencyThreshold = 120;
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
    int watchdogMaxRestarts = 5;
    int watchdogRestartWindow = 60;
    bool federatedEnabled = true;
    int safetyMigrationLimit = 120;
    int safetyLatencyLimit = 20000;
    int safetyLoadLimit = 95;
    int cpuUsageLimit = 85;
    int logLevel = 2;
    int logDecisions = 1;
    int logThermal = 1;
    int logFrame = 1;
    int autoProfile = 1;
    int gameDetection = 1;
    std::string refreshRateMode = "current";
    int batterySaverThreshold = 20;
    int thermalProfileThreshold = 80;
    int layerCriticalThreshold = 150;
    int daemonMemoryWarnKb = 65536;
    bool mlLearningEnabled = true;
    bool mlShadowSampling = true;
    float mlMaxAlpha = 0.3f;
    float mlInitialLR = 0.01f;
    float mlDecay = 0.999f;
    int mlSaveInterval = 30;
    float mlWeightClamp = 10.0f;
    bool mlHybridEnabled = false;
    bool hmacRequired = true;
    bool batteryAgingProtection = true;
    bool mlColdStartProtection = true;
    int mlMinSamples = 2000;
    bool enableHttpServer = false;
    bool mlFrameTimeOutcome = true;
    float mlFrameTimeWeight = 0.5f;
    int mlOutcomeMadFilter = 0;
    int mlDriftRelearn = 0;
    double mlEpsilonGreedy = 0.0;
    std::string littlePool;
    std::string midPool;
    std::string bigPool;
    std::string primePoolCfg;
    std::string packageWhitelist;
    std::vector<std::string> gamePackages;
    bool ebpfEnabled = true;
    int ebpfRingPages = 128;
    bool ebpfPollFallback = true;

    bool priorityBoostEnabled = true;
    int  priorityBoostLevel   = 1;

    bool smartPlacementEnabled = true;
    float coreAffinityWeight = 0.3f;
    float loadBalanceWeight  = 0.4f;
    float temperatureWeight  = 0.3f;
    bool adrcEnabled = true;
    float adrcH = 0.05f;
    float adrcR = 50.0f;
    float adrcBeta01 = 100.0f;
    float adrcBeta02 = 300.0f;
    float adrcBeta03 = 1000.0f;
    float adrcBeta1 = 10.0f;
    float adrcBeta2 = 0.5f;
    float adrcAlpha1 = 0.75f;
    float adrcAlpha2 = 1.25f;
    float adrcDelta = 0.01f;
    float adrcB0 = 1.0f;
    float adrcUrgencyThreshold = 0.5f;
    float adrcRiskThreshold = 0.6f;
    float adrcClampFrameRatio = 0.7f;
    float adrcWeightBase = 0.4f;
    float adrcMlWeightBase = 0.6f;

    int migrationHysteresisPercent = 5;
    int migrationCooldownMs        = 500;

    int migrationDwellSec = 5;

    float migrationPenaltyBigLittle = 3.0f;
    float migrationPenaltySame       = 1.0f;

    int threadRoleConfidenceThreshold = 30;

    bool mlDriftDetectionEnabled = true;
    int  mlDriftWindow           = 10;

    bool mlTopologyBindingEnabled = true;

    bool refreshRateEventDriven = true;

    int stateCacheTtlHours = 24;

    int placementMinGainPercent = 5;
    int placementMinConfidence  = 50;

    float minUsefulCapacityRatio = 0.3f;
};

#include <deque>

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

struct TracingStats {
    long long count = 0;
    long long sumUs = 0;
    long long minUs = 0;
    long long maxUs = 0;
    std::deque<long long> samples;
};

#endif

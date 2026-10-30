#ifndef AATPED_CONTROL_H
#define AATPED_CONTROL_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <atomic>
#include <ctime>
#include <mutex>
#include <set>

int capacityScore(int cpuId, ThreadRole role) noexcept;
int latencyScore(int cpuId, ThreadRole role) noexcept;
int cacheScore(int cpuId, int pid, int tid);
int thermalScore(int cpuId) noexcept;
int powerScore(int cpuId, ThreadRole role) noexcept;
int frameScore(int cpuId, ThreadRole role);
int poolScore(int cpuId, ThreadRole role);
int migrationPenalty(int cpuId, int pid, int tid);
int migrationPenaltyWithFrom(int cpuId, int pid, int tid, int knownFromCpu);
int contentionPenalty(int cpuId) noexcept;
int computeScore(int cpuId, const ThreadInfo& ti);
int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu);
void precomputeCpuBaseScores();
void initScoringArrays(int cpuCount);
void cleanupThreadScoringState(int tid);
void updateBaseScoreDynamicRange();
void calibrateCapacityNormRange();
void trackHeuristicScore(int score);
void updateSigmoidParams();

DecisionFusion fuseDecisions(float heuristicScore, float mlScore,
                              float hybridScore, float historyScore,
                              SystemState state, bool mlColdStart,
                              float mlAlphaScale = 1.0f);
extern FusionWeightAccumulator g_fusionWeightAccum;
void writeFusionWeightsState();
void writeFrameTimeOutcomeState();
void writeSnapshotJson();

Decision decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency = false);
bool applyPlacement(const ThreadInfo& ti, int targetCpu, const std::string& reason,
                    const Config& cfg, bool emergency = false);

extern MigrationBudget g_migrationBudget;
class RateLimitGuard {
public:
    explicit RateLimitGuard(bool frameCritical = false, bool emergency = false);
    ~RateLimitGuard();
    bool acquired() const { return acquired_; }
    void consume();
    RateLimitGuard(const RateLimitGuard&) = delete;
    RateLimitGuard& operator=(const RateLimitGuard&) = delete;
private:
    bool acquired_;
    bool consumed_;
};
extern int g_migrationCount;
extern time_t g_migrationWindowStart;
extern std::mutex g_lastMigMutex;
extern std::unordered_map<int, time_t> g_lastMigrationTime;
static constexpr int kMinMigrationIntervalSec = 5;
extern std::mutex g_migFailMutex;
extern std::unordered_map<int, time_t> g_migFailCooldown;
extern std::unordered_map<int, int> g_migFailCount;
extern std::unordered_map<int, int> g_migFailCpu;
extern time_t g_cooldownFloorWindowStart;
static constexpr int kMigCooldownFloorPerSec = 5;
static constexpr int kMigFailTriggerThreshold = 2;
static constexpr int kMigFailCooldownSec = 10;
extern std::atomic<long long> g_cooldownBypassCount;

extern std::unordered_map<int, long long> g_migFailBackoffUntilMs;
static constexpr int kMigFailBackoffTableMs[] = {100, 200, 500, 1000, 2000};
static constexpr int kMigFailBackoffTableLen = 5;
static inline int migFailBackoffMsForFail(int fails) {
    if (fails <= 1) return kMigFailBackoffTableMs[0];
    int idx = fails - 1;
    if (idx >= kMigFailBackoffTableLen) idx = kMigFailBackoffTableLen - 1;
    return kMigFailBackoffTableMs[idx];
}

#ifdef AATPE_HOST_TEST
void aatpeEvictDeadTidEntries();
#endif

struct ThreadSettleState {
    int settledCpu   = -1;
    int settledScore = 0;
    long long settledMs = 0;
};
extern std::mutex g_settleMutex;
extern std::unordered_map<int, ThreadSettleState> g_threadSettle;

bool placementHysteresisGate(int tid, int currentCpu, int bestCpu,
                             int currentHeuristic, int bestHeuristic,
                             const Config& cfg, Decision& d);
void placementRecordMigration(int tid, int cpu, int heuristicScore);
void placementInvalidateSettle(int tid);
long long placementGetAvgSettleDwellMs();
extern std::atomic<long long> g_settleMigrations;
extern std::atomic<long long> g_settleHysteresisRejects;
extern std::atomic<long long> g_settleCooldownRejects;

extern std::vector<WebuiDecision> g_decisionRing;
extern std::mutex g_decisionRingMutex;
extern std::atomic<int> g_explainTid;
void recordWebuiDecision(int tid, const std::string& name, const std::string& role,
                          int fromCpu, int toCpu, int score, int confidence,
                          const std::string& type);
void writeThreadsJson();
void writeDecisionsJson();
void writeC4Json();
void writeExplainJson();
void writeModelJson();
void writeConfigJson();
void writeSystemJson();
void writePerCpuJson();
void writeLogsJson();
void writeStateJson();

void applyHelperApkDetection();
void installCrashHandlers();
void reloadConfig();

struct ControlResult {
    bool success = false;
    int error_code = 0;
    std::string message;
    std::string applied_value;
};
ControlResult executeControlLine(const std::string& line);

bool isValidControlChars(const std::string& line);

void persistConfigKeyValue(const char* key, const char* value);

const char* mlModelPath();
void backupModelFile(const char* path);

bool setRuntimeControlMode(const std::string& mode, const std::string& reason);

extern std::string g_autoProfileMode;
extern std::mutex g_autoProfileMutex;

#endif

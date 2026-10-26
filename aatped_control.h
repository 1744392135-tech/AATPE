// AATPE - Control / scoring / placement / WebUI sub-header
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

// Scoring engine
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
void cleanupThreadScoringState(int tid);  // P1-4: erase per-tid map entries on thread exit
void updateBaseScoreDynamicRange();
void calibrateCapacityNormRange();
void trackHeuristicScore(int score);
void updateSigmoidParams();

// Decision fusion
// mlAlphaScale (default 1.0) is the CONTINUOUS ML maturity weight in
// [0,1]. When mlColdStart is false the base ML weight is scaled by it
// (wm *= mlAlphaScale) and the heuristic/hybrid/history weights are renormalized
// proportionally so the total weight is preserved. Default 1.0 reproduces the
// prior fixed-weight behavior exactly (unit tests call it without the new arg).
DecisionFusion fuseDecisions(float heuristicScore, float mlScore,
                              float hybridScore, float historyScore,
                              SystemState state, bool mlColdStart,
                              float mlAlphaScale = 1.0f);
extern FusionWeightAccumulator g_fusionWeightAccum;
void writeFusionWeightsState();
void writeFrameTimeOutcomeState();
void writeSnapshotJson();

// Placement
Decision decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency = false);
// P0-1: caller passes a pre-snapshotted Config& (no internal snapshotConfig).
bool applyPlacement(const ThreadInfo& ti, int targetCpu, const std::string& reason,
                    const Config& cfg, bool emergency = false);

// Migration budget / rate limit
extern MigrationBudget g_migrationBudget;
class RateLimitGuard {
public:
    // emergency=true: skip the per-second budget window reset and check entirely
    // (thermal spikes / jank events must always migrate; they must NOT reset the
    // ordinary migration-rate window that passive migrations rely on).
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
// P2-2: compile-time default for migrationDwellSec (MIGRATION_DWELL_SEC config).
// Runtime placement uses cfg.migrationDwellSec (loaded from aatpe.conf, range 1-60).
// This constant is kept as the documented default and fallback only.
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

// P3-52: exponential backoff for repeated sched_setaffinity failures.
//
// Before this change a failed migration was retried immediately on the next scan
// until kMigFailTriggerThreshold (2) consecutive failures, after which a flat
// kMigFailCooldownSec (10s) lockout was applied — an abrupt jump from "no wait"
// to "10s". Now the per-tid retry delay RAMPS with the consecutive failure
// count and resets to 0 on success:
//   fail#1 -> 100ms, #2 -> 200ms, #3 -> 500ms, #4 -> 1000ms, #>=5 -> 2000ms (cap).
// Values are steady_clock ms timestamps (expiry), not wall-clock seconds. The
// existing floor-bypass (kMigCooldownFloorPerSec) and cpu-changed early-release
// rules still apply on top of this throttle.
extern std::unordered_map<int, long long> g_migFailBackoffUntilMs;
static constexpr int kMigFailBackoffTableMs[] = {100, 200, 500, 1000, 2000};
static constexpr int kMigFailBackoffTableLen = 5;
// Returns the backoff delay (ms) for the n-th consecutive failure (1-based).
static inline int migFailBackoffMsForFail(int fails) {
    if (fails <= 1) return kMigFailBackoffTableMs[0];
    int idx = fails - 1;
    if (idx >= kMigFailBackoffTableLen) idx = kMigFailBackoffTableLen - 1;
    return kMigFailBackoffTableMs[idx];
}

// MAIN-01 (host test seam): evict dead-tid entries from the per-tid side maps.
// Defined in aatped_main.cpp. Production calls it from the calibration scan;
// host tests drive it concurrently with a g_threads writer to prove the
// snapshot-under-g_threadsMutex locking contract is race-free.
#ifdef AATPE_HOST_TEST
void aatpeEvictDeadTidEntries();
#endif

// ============================================================================
// #34 placement hysteresis 
// ----------------------------------------------------------------------------
// Per-thread "settled placement" bookkeeping. A thread is considered settled on
// a CPU once it has been migrated there (or at first observation). A NEW
// candidate CPU only wins if its heuristic score beats the settled CPU score by
// at least MIGRATION_HYSTERESIS_PERCENT, AND at least MIGRATION_COOLDOWN_MS has
// elapsed since the last migration to this thread. This converts fused-score
// micro-fluctuations between two near-identical CPUs (e.g. the two A78 mid cores
// on Dimensity 1100, cpu4 vs cpu5) into a stable placement instead of A/B/A/B.
// ============================================================================
struct ThreadSettleState {
    int settledCpu   = -1;   // CPU the thread is currently settled on
    int settledScore = 0;    // heuristic score observed on settledCpu at settle time
    long long settledMs = 0; // steady_clock ms of last settle (migration or first pin)
};
extern std::mutex g_settleMutex;
extern std::unordered_map<int, ThreadSettleState> g_threadSettle;

// Returns true if the hysteresis/cooldown gate rejects this migration (caller
// should KEEP). `bestHeuristic` is the raw heuristic score of bestCpu;
// `currentHeuristic` is the score of the CPU the thread currently runs on.
bool placementHysteresisGate(int tid, int currentCpu, int bestCpu,
                             int currentHeuristic, int bestHeuristic,
                             const Config& cfg, Decision& d);
// Called after a successful sched_setaffinity to (re)anchor the settled point.
void placementRecordMigration(int tid, int cpu, int heuristicScore);
// Called from scanThreads when a thread disappears, so recycled tids do not
// inherit stale settle state (mirrors invalidateAuxThreadState).
void placementInvalidateSettle(int tid);
// Diagnostic: average settle dwell (ms) over the last 60s window — written to
// state file placement_settle_avg_dwell_ms by the daemon loop.
long long placementGetAvgSettleDwellMs();
extern std::atomic<long long> g_settleMigrations;     // migrations allowed past hysteresis
extern std::atomic<long long> g_settleHysteresisRejects; // KEEPed by hysteresis band
extern std::atomic<long long> g_settleCooldownRejects;   // KEEPed by min-dwell cooldown

// WebUI JSON
extern std::vector<WebuiDecision> g_decisionRing;
extern std::mutex g_decisionRingMutex;
extern std::atomic<int> g_explainTid;
void recordWebuiDecision(int tid, const std::string& name, const std::string& role,
                          int fromCpu, int toCpu, int score, int confidence,
                          const std::string& type);
void writeThreadsJson();
void writeDecisionsJson();
void writeC4Json();   // v4.4.6: read-only C4 freezer status
void writeExplainJson();
void writeModelJson();
void writeConfigJson();
void writeSystemJson();
void writePerCpuJson();
void writeLogsJson();
void writeStateJson();

// Daemon helpers
void applyHelperApkDetection();
void installCrashHandlers();
void reloadConfig();

// item 11: concrete executor behind aatpe::CommandDispatcher. Runs one
// already-validated control line (the if/else command table). Called by the
// dispatcher, not directly by the control-pipe loop.
//
// P1-12 : returns the REAL execution result instead of void, so
// the dispatcher can propagate ok=true ONLY when the command actually executed
// (recognized + accepted), and ok=false (with a message) on unknown commands or
// rejected values. WebUI/CLI must never see ok=true for a command that did nothing.
struct ControlResult {
    bool success = false;
    int error_code = 0;       // 0 = ok; nonzero = rejected/unknown
    std::string message;      // human-readable detail (error or applied echo)
    std::string applied_value;
};
ControlResult executeControlLine(const std::string& line);

// P1-1: shared control-line character whitelist [a-z0-9_=.-] (max 256 bytes).
// Used by the root control pipe reader and by the on-disk command queue drain so
// queued commands are held to the same injection gate as live commands.
bool isValidControlChars(const std::string& line);

// Config persistence
void persistConfigKeyValue(const char* key, const char* value);

// ML model path
const char* mlModelPath();
void backupModelFile(const char* path);

// P1-1: unified daemon control-mode entry point. Every change to g_cfg.controlMode
// (whether from user WebUI, CLI, watchdog, or thermal recovery) MUST go through
// this function so that all four layers stay consistent:
//   1. g_cfg.controlMode (in-memory, under g_cfgMutex)
//   2. g_systemState atomic (via transitionToState)
//   3. STATE_DIR/control_mode file (atomic write)
//   4. WebUI display (reads the file)
// Supported modes: "active", "assist", "passive", "emergency".
// Returns true if the mode was applied (state transition may be deferred by
// dwell debounce but g_cfg.controlMode + file are always updated).
bool setRuntimeControlMode(const std::string& mode, const std::string& reason);

// Auto profile mode
// C-011: g_autoProfileMutex protects g_autoProfileMode (string cannot be atomic).
extern std::string g_autoProfileMode;
extern std::mutex g_autoProfileMutex;

#endif // AATPED_CONTROL_H

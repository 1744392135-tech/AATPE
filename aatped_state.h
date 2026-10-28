// AATPE - System state / metrics / counters sub-header
#ifndef AATPED_STATE_H
#define AATPED_STATE_H

#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <ctime>
#include <mutex>
#include <set>

#include <deque>
#include <atomic>

// State machine globals
extern std::atomic<int> g_systemState;
extern std::atomic<time_t> g_stateEnterTime;
extern char g_lastTransitionReason[128];
extern std::atomic<int> g_degradedNormalCycles;
extern std::mutex g_stateMutex;
extern std::deque<StateHistoryEntry> g_stateHistory;
extern std::atomic<bool> g_watchdogPassiveLocked;
// user has manually chosen NORMAL ("正常"). While this is set,
// checkSystemLoad() must NOT auto-downgrade to DEGRADED on high CPU, otherwise
// the user's manual selection is undone within 50-200ms on a sustained-load
// workload (e.g. gaming) and the UI appears stuck in battery-saver. Cleared
// when the user explicitly picks passive/powersave.
extern std::atomic<bool> g_userModeLock;

// P3-1 fix (lesson #20): set by the user set_mode= channel ONLY while the system
// is inside EMERGENCY, meaning the user explicitly picked a mode DURING the
// safety latch. On thermal recovery we must honor that choice instead of
// overwriting g_cfg.controlMode with the stale pre-emergency snapshot. Reset to
// false when EMERGENCY is entered (fresh snapshot) and consumed (cleared) when
// the recovery block applies.
extern std::atomic<bool> g_emergencyUserOverride;

// P2-5: leaf lock guarding the diagnostics scalars serialized into state.json
// (g_runtimeDecisionIntervalMs, g_audioState, g_sceneState, g_federated*,
// g_ab*, g_avgPredictionMAE). Defined in aatped_state.cpp.
extern std::mutex g_stateDiagMutex;

// GPU/Audio/Scene state struct instances
extern GpuState g_gpuState;
extern AudioState g_audioState;
extern SceneState g_sceneState;

// Runtime decision interval (adaptive)
extern int g_runtimeDecisionIntervalMs;

// Tracing stats
extern TracingStats g_traceDaemonLoop, g_traceDecidePlacement, g_traceComputeScore, g_traceApplyPlacement, g_traceEventResponse;
void traceUpdate(TracingStats& s, long long us);

// State machine
const char* systemStateToString(SystemState s);
int systemStatePriority(SystemState s);
bool transitionToState(SystemState newState, const char* reason);
SystemState getSystemState();
long getStateDurationSec();
// set both wall-clock and monotonic state-enter timestamps together.
// Called at startup and whenever the enter time must be refreshed outside a
// transitionToState() call.
void setStateEnterTimeNow();
std::string getLastTransitionReason();
std::vector<StateHistoryEntry> getStateHistory();
bool isWatchdogPassiveLocked();
void setWatchdogPassiveLocked(bool locked);
// test-only seam for the state-transition dwell debounce.
// Pass ms>=0 to override the dwell window (0 disables it) for hermetic/integration
// tests that walk the direct recovery path; pass <0 to restore the production
// default kStateDwellMs. No production caller uses this.
#ifdef AATPE_HOST_TEST
extern "C" void aatpe_state_setDwellMsForTest(long long ms);
#endif

// P3-1: real emergency-recovery control-mode policy (extracted from readThermal).
// Host test drives it directly; production calls it from the thermal recovery block.
std::string thermalApplyEmergencyModeRecovery();

// CpuMetrics
extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::mutex g_cpuMetricsMutex;
void readCpuMetrics();
extern std::map<int, std::pair<unsigned long long, unsigned long long>> g_prevCpuStat;

// daemon self CPU usage (% of one core, 0-100+).
// Updated by readCpuMetrics() on the 2s /proc/stat cadence.
// P1-1: atomic — read by eBPF poll thread and state.json writer across threads.
extern std::atomic<int> g_selfCpuUsagePercent;

// Thermal
void readThermal();
// ROUND-011: independent thermal sampling thread (decouples sysfs reads from
// the main loop so a stuck D-state sysfs read cannot freeze the daemon).
void startThermalSampling();
void stopThermalSampling();
// ROUND-011: called by main loop every second. Reads the atomic boot-time
// stamp from the sampling thread, computes age, writes thermal_freshness
// state file, and flips g_thermalDegraded if >10s stale.
void thermalFreshnessTick();
// Test-only: clear thermal runtime caches/baselines (s_lastZoneTemp, over-temp
// streak, cache timestamp) between integration scenarios. No-op on device;
// does not touch g_cfg thresholds or the 120°C production default.
void thermal_resetRuntimeStateForTest();
extern std::atomic<bool> g_thermalEmergency;
extern std::atomic<int> g_thermalMigrationPressure;
extern std::atomic<bool> g_frameCritical;
// SAFETY_LATENCY gate (independent atomic, NOT g_cfg.controlMode).
// Previously frame.cpp forced g_cfg.controlMode="passive" on safety-latency
// breach but never restored it on recovery, leaving the daemon stuck in
// passive (and clobbering the user's manual mode). Now the breach only latches
// this flag; placement.cpp's decidePlacement/applyPlacement gates migrations
// on it exactly like g_thermalEmergency. The user's controlMode is untouched.
extern std::atomic<bool> g_safetyLatencyActive;
extern std::atomic<bool> g_profileLock;
extern std::atomic<int> g_thermalCpu;
extern std::atomic<int> g_thermalSkin;
extern std::atomic<int> g_thermalBattery;
// C1/C3 exploration: headroom promoted from state-file-only to in-memory atomic
// so C1 thermal gate and C3 continuous derating can read it without sysfs.
// Default 0 (cool); written every thermal tick. Consumers MUST gate on
// preboostEnabled / thermalSmoothEnabled (default false) to preserve v4.4.0
// byte-identical behavior when the switches are off.
extern std::atomic<int> g_thermalHeadroomPct;
extern std::atomic<float> g_thermalPlacementScale;
// C1/C3 feature switches (default false). Parsed from aatpe.conf.
// Atomic: written by config loader, read by hot path (main/thermal/placement/priority).
extern std::atomic<bool> preboostEnabled;
extern std::atomic<bool> thermalSmoothEnabled;
// W5 scene_infer + C2 envelope switches (default false).
extern std::atomic<bool> sceneInferEnabled;
extern std::atomic<bool> c2EnvelopeEnabled;
extern std::string g_thermalCpuZonePath;
std::string aatpe_getThermalCpuZonePath();

// GPU state.json stats
// P2-7/A2: g_gpuType points to a permanently-owned buffer owned in aatped_gpu.cpp
// (s_gpuTypeOwner); the old buffer is intentionally leaked. It is now an atomic
// pointer because it is written from the GPU probe thread (aatped_gpu.cpp) and
// read from placement/diagnostic threads — a plain `const char*` store was a
// data race. Load/store with relaxed ordering (pointer-sized, word-aligned).
#include <atomic>
extern std::atomic<const char*> g_gpuType;
// atomic<float> — written by aatped_gpu.cpp poll thread, read by
// gpu_boost.cpp and aatped_gamescene.cpp. Relaxed order is sufficient (no
// dependent state needs ordering with the util value itself).
extern std::atomic<float> g_gpuUtilization;

// eBPF stats
extern std::string g_ebpfLoader;
// atomic<bool> — written at eBPF load/fail (init thread), read by
// state.json serializer, main loop, and WebUI. Release-on-store / acquire-on-load.
extern std::atomic<bool> g_ebpfActive;

extern std::string g_ebpfMode;
// P1-2 fix: g_ebpfLoader / g_ebpfMode are std::string globals written at eBPF
// load time (init thread) and read by writeStateJson() (main loop) + the
// state-file publisher. C++17 has no std::atomic<std::string>, so guard both
// strings with this dedicated mutex. Writers hold it briefly; readers snapshot
// to a local std::string under it before concatenating into JSON. Do NOT nest
// this under g_cfgMutex/g_threadsMutex (it is an inner, leaf lock).
extern std::mutex g_ebpfInfoMutex;
extern std::atomic<uint64_t> g_ebpfEventsTotal;
extern std::atomic<uint64_t> g_ebpfEventsFork;
extern std::atomic<uint64_t> g_ebpfEventsExit;
extern std::atomic<uint64_t> g_ebpfEventsSwitch;
extern std::atomic<uint64_t> g_ebpfRingLost;
extern std::atomic<double> g_ebpfEventRate;

// Federated stats
// HTTP-04: atomic<int> to eliminate TSan data race on cross-thread reads.
extern std::atomic<int> g_federatedRounds;
extern std::atomic<int> g_federatedExports;
extern std::atomic<int> g_federatedImports;
extern int g_federatedTrustedImports;

// A/B test stats
extern float g_abGroupAAvg;
extern float g_abGroupBAvg;
extern float g_abPValue;

// ML prediction stats
extern float g_avgPredictionMAE;

// Foreground / start time
extern std::string g_foregroundApp;
// P0-5: thread-safe reader for g_foregroundApp (defined in frame.cpp, takes the
// shared g_foregroundMutex). Cross-thread readers (gpu_boost.cpp, scoring.cpp,
// WebUI) must call this instead of reading g_foregroundApp directly.
std::string aatpeGetForegroundPackage();
extern std::atomic<time_t> g_startTime;

// Placement history
extern std::map<int, HistoryEntry> g_history;
extern std::mutex g_historyMutex;
int placementSuccessScore(int cpuId, int tid);
[[deprecated("use placementSuccessScore instead")]] int historyScore(int cpuId, int tid);
void recordPlacementResult(int tid, int cpuId, bool good);
[[deprecated("use recordPlacementResult instead")]] void recordOutcome(int tid, int cpuId, bool good);
void decayHistory();
// ML-10: drop stale per-tid history files stamped by a previous daemon boot
// (tid reuse across restarts). Called once at startup after RUN_DIR/history is
// created; safe to call when the dir does not exist.
void scoringHistorySweepStale();

// Migration counting
void recordMigrationCount(int tid);
unsigned long long getMigrationCount(int tid);

// ML helpers
extern aatpe_ml::LearningModel g_mlModel;
extern std::mutex g_mlMutex;
extern time_t g_mlLastSave;
extern std::atomic<unsigned long> g_mlSaveSuccess;
extern std::atomic<unsigned long> g_mlSaveFail;
void updateMlSaveStateFiles(bool success, const char* path);
aatpe_ml::ThreadFeatures extractMlFeatures(const ThreadInfo& ti, int targetCpu);
float normalizeHeuristicScore(int score);

// Frame awareness
void readSurfaceFlinger();
int aatpeDetectRefreshRate();
int aatpeDetectSFLatency(int refreshRate, int& outMaxLat, bool& outValid);
bool checkSystemLoad();
void autoSelectProfile();

// schedstat
extern std::vector<SchedstatCpuData> g_schedstatData;
bool readSchedstatCpuUtil(void);
float computeSchedstatCpuUtil(int cpuId);
float readCpuUtilizationSchedstat(int cpuId);

// Unmigratable threads
extern std::set<int> g_unmigratableThreads;
extern std::mutex g_unmigratableMutex;
void reprobeUnmigratableThreads();
// P1-6: atomic to avoid RMW data race.
#include <atomic>
extern std::atomic<long long> g_lastUnmigratableReprobe;
static constexpr int kUnmigratableReprobeIntervalSec = 60;

// Counters
extern std::atomic<long long> g_totalDecisions;
extern std::atomic<long long> g_totalMigrations;
extern std::atomic<long long> g_highLoadCycles;
extern std::atomic<long long> g_keepProtected;
extern std::atomic<long long> g_keepPassive;
extern std::atomic<long long> g_keepThermalEmergency;
extern std::atomic<long long> g_keepSafetyLatency;
extern std::atomic<long long> g_keepNoCpu;
extern std::atomic<long long> g_keepLowConf;
extern std::atomic<long long> g_keepHighCost;
extern std::atomic<long long> g_keepSafeMode;   // ordinary migration blocked by Safe Mode
extern std::atomic<long long> g_keepAlreadyOptimal;
// settle-band reject counters (defined in placement.cpp) surfaced to
// diagTick so migrate_reject_hysteresis / migrate_reject_cooldown are observable.
extern std::atomic<long long> g_settleHysteresisRejects;
extern std::atomic<long long> g_settleCooldownRejects;
extern std::atomic<long long> g_migrateActual;
extern std::atomic<long long> g_applySuccess;
extern std::atomic<long long> g_applyFailed;
extern std::atomic<long long> g_applyFailedEinval;
extern std::atomic<long long> g_applyFailedEsrch;
extern std::atomic<long long> g_applyFailedEperm;
extern std::atomic<long long> g_applyFailedOther;
extern std::atomic<long long> g_applyRateLimited;
extern std::atomic<long long> g_applyPassive;
extern std::atomic<long long> g_applyThermalEmergency;
extern std::atomic<long long> g_applySafetyLatency;
extern std::atomic<long long> g_applyCooldownSkip;
extern std::atomic<long long> g_applyCpusetSkip;
// v6.0.0 P1: most recent CPUSET_SKIP sample (thread refused big-core migration
// because it was parked in a narrower cpuset). Populated on the skip path.
struct CpusetSkipSample {
    long long tid = 0;
    int pid = 0;
    int targetCpu = -1;
    int maskCount = 0;
    char pname[64] = {0};
};
extern CpusetSkipSample g_lastCpusetSkip;
extern std::mutex g_lastCpusetSkipMutex;
extern std::atomic<long long> g_applyKernelSkip;
extern std::atomic<long long> g_applyUnmigratableSkip;
extern std::atomic<long long> g_applyIntervalSkip;
extern std::atomic<long long> g_mlPendingCreated;
extern std::atomic<long long> g_mlOutcomeMeasured;
extern std::atomic<long long> g_mlOutcomeValid;
extern std::atomic<long long> g_mlOutcomeSkipSleeping;
extern std::atomic<long long> g_mlOutcomeSkipSaturated;
extern std::atomic<long long> g_mlOutcomeSkipReset;
extern std::atomic<long long> g_mlOutcomeSkipOther;
extern std::atomic<long long> g_mlPendingExpired;
extern std::atomic<long long> g_mlPendingTidReused;
extern std::atomic<long long> g_mlPendingThreadExited;

// CPU load prediction
void recordCpuLoadSample(int cpuId, int util);
float predictCpuLoad(int cpuId);
int predictedLoadPenalty(int cpuId);
float predictCpuLoadMultiStep(int cpuId, int steps);
void recordPredictionError(int cpuId, float predicted, float actual);
float getPredictionConfidence(int cpuId);
float getAveragePredictionMAE();
int proactiveMigrationCheck();

#endif // AATPED_STATE_H

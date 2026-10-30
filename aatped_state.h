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

extern std::atomic<int> g_systemState;
extern std::atomic<time_t> g_stateEnterTime;
extern char g_lastTransitionReason[128];
extern std::atomic<int> g_degradedNormalCycles;
extern std::mutex g_stateMutex;
extern std::deque<StateHistoryEntry> g_stateHistory;
extern std::atomic<bool> g_watchdogPassiveLocked;
extern std::atomic<bool> g_userModeLock;

extern std::atomic<bool> g_emergencyUserOverride;

extern std::mutex g_stateDiagMutex;

extern GpuState g_gpuState;
extern AudioState g_audioState;
extern SceneState g_sceneState;

extern int g_runtimeDecisionIntervalMs;

extern TracingStats g_traceDaemonLoop, g_traceDecidePlacement, g_traceComputeScore, g_traceApplyPlacement, g_traceEventResponse;
void traceUpdate(TracingStats& s, long long us);

const char* systemStateToString(SystemState s);
int systemStatePriority(SystemState s);
bool transitionToState(SystemState newState, const char* reason);
SystemState getSystemState();
long getStateDurationSec();
void setStateEnterTimeNow();
std::string getLastTransitionReason();
std::vector<StateHistoryEntry> getStateHistory();
bool isWatchdogPassiveLocked();
void setWatchdogPassiveLocked(bool locked);
#ifdef AATPE_HOST_TEST
extern "C" void aatpe_state_setDwellMsForTest(long long ms);
#endif

std::string thermalApplyEmergencyModeRecovery();

extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::mutex g_cpuMetricsMutex;
void readCpuMetrics();
extern std::map<int, std::pair<unsigned long long, unsigned long long>> g_prevCpuStat;

extern std::atomic<int> g_selfCpuUsagePercent;

void readThermal();
void startThermalSampling();
void stopThermalSampling();
void thermalFreshnessTick();
void thermal_resetRuntimeStateForTest();
extern std::atomic<bool> g_thermalEmergency;
extern std::atomic<int> g_thermalMigrationPressure;
extern std::atomic<bool> g_frameCritical;
extern std::atomic<bool> g_safetyLatencyActive;
extern std::atomic<bool> g_profileLock;
extern std::atomic<int> g_thermalCpu;
extern std::atomic<int> g_thermalSkin;
extern std::atomic<int> g_thermalBattery;
extern std::atomic<int> g_thermalHeadroomPct;
extern std::atomic<float> g_thermalPlacementScale;
extern std::atomic<bool> preboostEnabled;
extern std::atomic<bool> thermalSmoothEnabled;
extern std::atomic<bool> sceneInferEnabled;
extern std::atomic<bool> c2EnvelopeEnabled;
extern std::string g_thermalCpuZonePath;
std::string aatpe_getThermalCpuZonePath();

#include <atomic>
extern std::atomic<const char*> g_gpuType;
extern std::atomic<float> g_gpuUtilization;

extern std::string g_ebpfLoader;
extern std::atomic<bool> g_ebpfActive;

extern std::string g_ebpfMode;
extern std::mutex g_ebpfInfoMutex;
extern std::atomic<uint64_t> g_ebpfEventsTotal;
extern std::atomic<uint64_t> g_ebpfEventsFork;
extern std::atomic<uint64_t> g_ebpfEventsExit;
extern std::atomic<uint64_t> g_ebpfEventsSwitch;
extern std::atomic<uint64_t> g_ebpfRingLost;
extern std::atomic<double> g_ebpfEventRate;

extern std::atomic<int> g_federatedRounds;
extern std::atomic<int> g_federatedExports;
extern std::atomic<int> g_federatedImports;
extern int g_federatedTrustedImports;

extern float g_abGroupAAvg;
extern float g_abGroupBAvg;
extern float g_abPValue;

extern float g_avgPredictionMAE;

extern std::string g_foregroundApp;
std::string aatpeGetForegroundPackage();
extern std::atomic<time_t> g_startTime;

extern std::map<int, HistoryEntry> g_history;
extern std::mutex g_historyMutex;
int placementSuccessScore(int cpuId, int tid);
[[deprecated("use placementSuccessScore instead")]] int historyScore(int cpuId, int tid);
void recordPlacementResult(int tid, int cpuId, bool good);
[[deprecated("use recordPlacementResult instead")]] void recordOutcome(int tid, int cpuId, bool good);
void decayHistory();
void scoringHistorySweepStale();

void recordMigrationCount(int tid);
unsigned long long getMigrationCount(int tid);

extern aatpe_ml::LearningModel g_mlModel;
extern std::mutex g_mlMutex;
extern time_t g_mlLastSave;
extern std::atomic<unsigned long> g_mlSaveSuccess;
extern std::atomic<unsigned long> g_mlSaveFail;
void updateMlSaveStateFiles(bool success, const char* path);
aatpe_ml::ThreadFeatures extractMlFeatures(const ThreadInfo& ti, int targetCpu);
float normalizeHeuristicScore(int score);

void readSurfaceFlinger();
int aatpeDetectRefreshRate();
int aatpeDetectSFLatency(int refreshRate, int& outMaxLat, bool& outValid);
bool checkSystemLoad();
void autoSelectProfile();

extern std::vector<SchedstatCpuData> g_schedstatData;
bool readSchedstatCpuUtil(void);
float computeSchedstatCpuUtil(int cpuId);
float readCpuUtilizationSchedstat(int cpuId);

extern std::set<int> g_unmigratableThreads;
extern std::mutex g_unmigratableMutex;
void reprobeUnmigratableThreads();
#include <atomic>
extern std::atomic<long long> g_lastUnmigratableReprobe;
static constexpr int kUnmigratableReprobeIntervalSec = 60;

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
extern std::atomic<long long> g_keepSafeMode;
extern std::atomic<long long> g_keepAlreadyOptimal;
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

void recordCpuLoadSample(int cpuId, int util);
float predictCpuLoad(int cpuId);
int predictedLoadPenalty(int cpuId);
float predictCpuLoadMultiStep(int cpuId, int steps);
void recordPredictionError(int cpuId, float predicted, float actual);
float getPredictionConfidence(int cpuId);
float getAveragePredictionMAE();
bool hasAnyPredictionErrorSamples();
int proactiveMigrationCheck();

#endif

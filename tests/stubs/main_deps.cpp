#include "aatped.h"
#include "aatpe_ml.h"
#include "event_monitor.h"

int aatpeDetectSFLatency(int, int& out, bool& v) { out = 0; v = false; return 0; }
int computeScore(int, const ThreadInfo&) { return 0; }
SystemState getSystemState() { return SystemState::NORMAL; }
bool transitionToState(SystemState, const char*) { return false; }
long getStateDurationSec() { return 0; }
void snapshotConfig(Config& out) { out = g_cfg; }

std::atomic<int> g_degradedNormalCycles{0};
std::atomic<bool> g_userModeLock{false};
std::atomic<bool> g_watchdogPassiveLocked{false};

#include <fstream>
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return "";
    std::getline(f, s); return s;
}
int readInt(const char* path, int def) {
    std::ifstream f(path); int v; if (f >> v) return v; return def;
}
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }

std::string g_foregroundApp;
char g_lastTransitionReason[128] = "boot";
std::atomic<int> g_systemState{0};
std::atomic<time_t> g_stateEnterTime{0};
std::string getLastTransitionReason() { return std::string(g_lastTransitionReason); }
const char* systemStateToString(SystemState) { return "NORMAL"; }
CpuPool getPoolForCpu(int) { return CpuPool::LITTLE; }
float predictCpuLoad(int) { return 0.0f; }
void recordCpuLoadSample(int, int) {}
void recordPredictionError(int, float, float) {}
void initHmacKey() {}
void loadConfig() {}
bool atomicWriteFile(const char*, const std::string&) { return true; }
void mkdirIfNotExist(const char*, mode_t) {}
#include <cstdio>
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}
namespace aatpe { namespace webui_http { void ensureDaemonToken() {} } }

void aatpeProbeEnvironment() {}
void applyHelperApkDetection() {}
void detectCpuTopology() {}
__attribute__((weak)) bool g_batteryAged = false;
void installCrashHandlers() {}
void loadProfile(const std::string&) {}
void loadThreadRules() {}
void processControlCommands() {}
void signalHandler(int) {}
void startConfigHotReload() {}
void stopConfigHotReload() {}
bool atomicWriteFile(const char*, int) { return true; }
ControlResult executeControlLine(const std::string&) { return ControlResult{}; }
bool atomicWriteFile(const char*, long long) { return true; }
std::mutex g_settleMutex;
std::unordered_map<int, ThreadSettleState> g_threadSettle;
int  g_stubHttpPort = 8888;
int  g_stubStartCalls = 0;
int  g_stubStopCalls = 0;
bool g_stubStartOk = true;
namespace aatpe { namespace webui_http {
    int getHttpPort() { return g_stubHttpPort; }
    bool startHttpServer() { ++g_stubStartCalls; return g_stubStartOk; }
    bool hasActiveClients() { return false; }
    void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
    void stopHttpServer() { ++g_stubStopCalls; }
} }

std::mutex g_topologyRwMutex;
int g_stubClusterCount = 3;
int clusterCount() { return g_stubClusterCount; }
uint64_t g_stubSpilloverPromo = 0;
int      g_stubSpilloverActive = 0;
namespace aatpe { namespace placement {
    uint64_t spilloverPromotionCount() noexcept { return g_stubSpilloverPromo; }
    int      spilloverActiveCount() noexcept    { return g_stubSpilloverActive; }
    void     onInputEvent() noexcept            {  }
    void     onForegroundSwitchEdge(const std::string&) noexcept {  }
} }
std::string aatpe_getThermalCpuZonePath() { return std::string(); }
bool topologyRetryDue(unsigned long) { return false; }
std::string clustersStateText() { return "[]"; }
std::vector<int> primePool() { return {}; }
namespace aatpe { namespace diag {
    struct SystemPressure { bool psiAvailable; float psiCpuSomePct, psiIoSomePct, psiMemSomePct; uint64_t psiCpuTotalUsec; int gpuUtilPct, gpuFreqKhz, gpuMaxKhz; std::string gpuGovernor, gpuDevName; bool cpuThrottled, gpuThrottled; int cpuCapKhz, cpuHwMaxKhz; struct RqWaitEntry { int tid; int waitMs; }; std::vector<RqWaitEntry> rqWaitTop; int64_t sampleUptimeSec; };
    void collect() {}
    void writeStateFiles(const char*) {}
    SystemPressure getSystemPressureSnapshot() { return SystemPressure{}; }
} }
namespace aatpe { namespace caps {
    struct DeviceCaps {};
    void probe() {}
    void writeStateFiles(const char*) {}
    DeviceCaps getCaps() { return DeviceCaps{}; }
} }
void startThermalSampling() {}
void stopThermalSampling() {}
void thermalFreshnessTick() {}
#include <atomic>
std::atomic<int> g_thermalTripC{0};
namespace aatpe_mech { __attribute__((weak)) void restoreAllCgroupUclamp() {} }
namespace aatpe { namespace placement { void releaseAllSpilloverLocked() {} } }
void aatpeGpuBoostForceZero(const char* reason) { (void)reason; }
namespace aatpe { namespace frameaware { bool jankBoostVoteActive() { return false; } } }
namespace aatpe { namespace frameaware { int getRefreshRate() { return 60; } } }
namespace aatpe { int aatpeThermalHalLevel() { return 0; } bool aatpeThermalDegraded() { return false; }
bool aatpeThermalMayFreezeTraining(bool) { return false; }
const char* aatpeThermalFreezeReason() { return "none"; }
}
void aatpeGpuThrottleTick() {}
namespace aatpe { namespace placement { void releaseForegroundDownpins(const std::string& pkg) { (void)pkg; } } }
namespace aatpe { namespace placement { void healForegroundCgroupMembership(const std::string& pkg) { (void)pkg; } } }
const std::vector<CpuCluster>& clusters() {
    static const std::vector<CpuCluster> empty;
    return empty;
}
std::vector<CpuCluster> clustersSnapshot() {
    return clusters();
}
void restoreUserControlMode() {}
int abTestAssignThread(int) { return 0; }
int abTestAssignByUid(int) { return 0; }
void hmacForceReadonly(const char*) {}
void hmacClearReadonly(const char*) {}
void abTestRecordJank(int) {}
int emergencyMigrateCriticalThreads() { return 0; }
void precomputeCpuBaseScores() {}
std::atomic<long long> g_settleHysteresisRejects{0};
std::atomic<long long> g_settleCooldownRejects{0};
int proactiveMigrationCheck() { return 0; }
void readSurfaceFlinger() {}
void readThermal() {}
namespace aatpe {
namespace frameaware {
    void executeFrameAwareDecision() {}
    bool shouldTriggerFrameAware(long long) { return false; }
    void updateRefreshRate() {}
    void writeStats(const std::string&) {}
    void refreshRateEventStart() {}
    void refreshRateEventStop() {}
}
}

#include <atomic>
std::atomic<uint64_t> g_stateGeneration{0};

std::mutex g_stateDiagMutex;

void scoringHistorySweepStale() {}

std::string aatpeDetectForegroundApp(bool) { return std::string(); }
void autoSelectProfile() {}
void checkAndPreloadNewApps() {}
void cleanupDecisionsDir() {}
bool detectAudioPlayback(void) { return false; }
GameScene detectGameScene(float, float, float) { return GameScene::UNKNOWN; }
float getAveragePredictionMAE() { return 0.0f; }
float getPredictionConfidence(int) { return 0.5f; }
bool hasAnyPredictionErrorSamples() { return false; }
__attribute__((weak)) void readBatteryHealth() {}
__attribute__((weak)) void readBatteryPower() {}
void readGpuUtilization() {}
void refreshCpuTopology() {}
void reprobeUnmigratableThreads() {}
void scanThreads() {}
void writeConfigJson() {}
void writeDecisionsJson() {}
void writeC4Json() {}
void writeExplainJson() {}
void writeFusionWeightsState() {}
void writeLogsJson() {}
void writeModelJson() {}
void writePerCpuJson() {}
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }

ABTestStats g_abStats;
__attribute__((weak)) int g_batteryHealthPercent = 100;
bool g_hmacActive = false;
std::string g_hmacKeyStatus = "none";
std::mutex g_hmacMutex;
int abTestDetermineWinner() { return 0; }
void abTestRecordMigration(int) {}
void recordMigrationCount(int) {}
aatpe_ml::ThreadFeatures extractMlFeatures(const ThreadInfo&, int) {
    return aatpe_ml::ThreadFeatures{};
}
void recordPlacementResult(int, int, bool) {}
void recordWebuiDecision(int, const std::string&, const std::string&, int, int, int, int, const std::string&) {}
void decayHistory() {}
void reloadConfig() {}
void reloadConfigIfChanged() {}
void loadPerAppConfigs(const char*) {}
void checkAndApplyPerAppConfig(const char*) {}
PerAppConfig g_currentAppConfig;
void stateCacheNoteRemove(const char*) {}
const char* sceneToString(GameScene) { return "UNKNOWN"; }
void updateMlSaveStateFiles(bool, const char*) {}
void writeFrameTimeOutcomeState() {}
void writeSnapshotJson() {}
void writeSystemJson() {}
void writeThreadsJson() {}
int getGpuBottleneckDecisionInterval() { return 100; }
bool isGpuBottleneck(float) { return false; }
void protectCriticalThreads() {}
bool applyPlacement(const ThreadInfo&, int, const std::string&, const Config&, bool) { return false; }
Decision decidePlacement(const ThreadInfo&, const Config&, bool) { return Decision{}; }

std::string aatpeExec(const std::string&, const char*) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return std::string(); }
void checkHmacKeyRotation() {}
ThreadRole classifyThread(int, int, const std::string&, const std::string&, int&, std::string&, int*, unsigned long long) { return ROLE_S5_BACKGROUND; }
__attribute__((weak)) int g_batteryCycleCount = 0;
TracingStats g_traceDaemonLoop, g_traceDecidePlacement, g_traceComputeScore, g_traceApplyPlacement, g_traceEventResponse;
int readThreadCurrentCpu(int, int) { return -1; }
void traceUpdate(TracingStats&, long long) {}
void writeStateJson() {}

void cleanupThreadScoringState(int) {}
bool isPackageInGameList(const std::string&) { return false; }

void setStateEnterTimeNow() {}

std::string aatpeGetForegroundPackage() { return g_foregroundApp; }

int aatpeAndroidSdkInt() { return 30; }

#include <string>
namespace aatpe { namespace frameaware {
int getFrameIntervalMs() { return 16; }
} }
void trim(std::string& s) {
    size_t st = s.find_first_not_of(" \t\r\n");
    if (st == std::string::npos) { s.clear(); return; }
    size_t en = s.find_last_not_of(" \t\r\n");
    s = s.substr(st, en - st + 1);
}

__attribute__((weak)) void aatpeGpuRefreshUtilForBoost(void) {}

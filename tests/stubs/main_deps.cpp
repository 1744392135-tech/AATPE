// main_deps.cpp - host link deps for aatped_main.cpp (after main() stripped).
// main.cpp defines nearly all daemon globals itself; this fills the handful of
// functions it calls from other TUs.
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
std::atomic<bool> g_watchdogPassiveLocked{false};  // P0: aatped_main passive-gate read

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

// more externals main.cpp references from other TUs
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
// round-4: aatpe_recovery.cpp (now linked via main recipe) needs a real
// writeFileAtomic (tmp+rename).
#include <cstdio>
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}
namespace aatpe { namespace webui_http { void ensureDaemonToken() {} } }

// remaining TU entry points main.cpp calls
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
// P1-12: executeControlLine now returns ControlResult (not void).
ControlResult executeControlLine(const std::string&) { return ControlResult{}; }
// state_ttl calls the long-long overload of atomicWriteFile.
bool atomicWriteFile(const char*, long long) { return true; }
// aatpe_state_ttl references settle tracking globals.
std::mutex g_settleMutex;
std::unordered_map<int, ThreadSettleState> g_threadSettle;
// ROUND-004: injectable HTTP start/stop counters for reload-hook tests.
int  g_stubHttpPort = 8888;
int  g_stubStartCalls = 0;
int  g_stubStopCalls = 0;
bool g_stubStartOk = true;
namespace aatpe { namespace webui_http {
    int getHttpPort() { return g_stubHttpPort; }
    bool startHttpServer() { ++g_stubStartCalls; return g_stubStartOk; }
    // P2-1: hermetic stub — no WebUI clients in the unit-test harness, so the
    // daemon loop skips the expensive threads/decisions/explain JSON writes.
    bool hasActiveClients() { return false; }
    // P3-42: dispatcher calls this after every command; no-op in hermetic test.
    void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
    void stopHttpServer() { ++g_stubStopCalls; }
} }

// remaining daemon entry points referenced by main.cpp
std::mutex g_topologyRwMutex;
// ROUND-004: synthetic cluster count for host unit tests (3 pools: LITTLE/MID/BIG).
int g_stubClusterCount = 3;
int clusterCount() { return g_stubClusterCount; }
// ROUND-004: G5 spillover observability stubs (controllable in tests).
uint64_t g_stubSpilloverPromo = 0;
int      g_stubSpilloverActive = 0;
namespace aatpe { namespace placement {
    uint64_t spilloverPromotionCount() noexcept { return g_stubSpilloverPromo; }
    int      spilloverActiveCount() noexcept    { return g_stubSpilloverActive; }
    void     onInputEvent() noexcept            { /* ROUND-007b: host stub */ }
    // ROUND-015: fg-edge cache-align + re-pin sweep (real impl in placement.cpp).
    void     onForegroundSwitchEdge(const std::string&) noexcept { /* host stub */ }
} }
std::string aatpe_getThermalCpuZonePath() { return std::string(); }
// ROUND-005c: G5/G6 topology accessor stubs for host main test.
bool topologyRetryDue(unsigned long) { return false; }
std::string clustersStateText() { return "[]"; }
std::vector<int> primePool() { return {}; }
// ROUND-009: diag collector stubs for host main test.
namespace aatpe { namespace diag {
    struct SystemPressure { bool psiAvailable; float psiCpuSomePct, psiIoSomePct, psiMemSomePct; uint64_t psiCpuTotalUsec; int gpuUtilPct, gpuFreqKhz, gpuMaxKhz; std::string gpuGovernor, gpuDevName; bool cpuThrottled, gpuThrottled; int cpuCapKhz, cpuHwMaxKhz; struct RqWaitEntry { int tid; int waitMs; }; std::vector<RqWaitEntry> rqWaitTop; int64_t sampleUptimeSec; };
    void collect() {}
    void writeStateFiles(const char*) {}
    SystemPressure getSystemPressureSnapshot() { return SystemPressure{}; }
} }
// ROUND-010: caps probe stubs for host main test.
namespace aatpe { namespace caps {
    struct DeviceCaps {};
    void probe() {}
    void writeStateFiles(const char*) {}
    DeviceCaps getCaps() { return DeviceCaps{}; }
} }
// ROUND-011: thermal sampling thread stubs.
void startThermalSampling() {}
void stopThermalSampling() {}
void thermalFreshnessTick() {}
// audit5 N2 (v4.4.0): aatped_arbitrator.cpp (DAEMON build) reads the real die
// trip point g_thermalTripC, owned in production by thermal.cpp. The v94/san
// `main` target links the thermal-function stubs above (not thermal.o), so supply
// the global here. 0 = unknown trip -> the arbitrator's trip-EWMA gate stays
// closed on the host (safe default; the real value comes from thermal.cpp on device).
#include <atomic>
std::atomic<int> g_thermalTripC{0};
// ROUND-013: cgroup uclamp restore stub.
// v4.3.9: weak so the real aatpe_placement_mech definition wins when linked.
namespace aatpe_mech { __attribute__((weak)) void restoreAllCgroupUclamp() {} }
// ROUND-014 P3: G5 shutdown function stubs (forward-declared in main.cpp).
namespace aatpe { namespace placement { void releaseAllSpilloverLocked() {} } }
void aatpeGpuBoostForceZero(const char* reason) { (void)reason; }
// ROUND-014 P3: frameaware jank vote stub.
namespace aatpe { namespace frameaware { bool jankBoostVoteActive() { return false; } } }
// ROUND-014 P3: frameaware refresh rate stub.
namespace aatpe { namespace frameaware { int getRefreshRate() { return 60; } } }
// ROUND-014 P3: thermal HAL level stub.
namespace aatpe { int aatpeThermalHalLevel() { return 0; } bool aatpeThermalDegraded() { return false; }
// v4.2.4: ML freeze-gate stubs (real impl lives in thermal.cpp).
bool aatpeThermalMayFreezeTraining(bool) { return false; }
const char* aatpeThermalFreezeReason() { return "none"; }
}
// ROUND-014 P3: GPU throttle tick stub.
void aatpeGpuThrottleTick() {}
// ROUND-014 P3: placement stub.
namespace aatpe { namespace placement { void releaseForegroundDownpins(const std::string& pkg) { (void)pkg; } } }
// v4.4.1 P0: edge-independent fg cgroup heal (host stub; real impl in placement.cpp).
namespace aatpe { namespace placement { void healForegroundCgroupMembership(const std::string& pkg) { (void)pkg; } } }
// ROUND-013: recovery refresh now provided by aatpe_recovery.cpp (prod object).
// D-P3-2: recoveryClearAllPersisted also provided by prod object — no stub needed.
// ROUND-007d: aatped_main.cpp mlPoolCount()/daemonInit() now read clusters() from
// aatped_topology.cpp. The hermetic main test covers wiring/state seeding, not
// clustering (dedicated topology target, 72 cases); return a fixed empty set.
const std::vector<CpuCluster>& clusters() {
    static const std::vector<CpuCluster> empty;
    return empty;
}
// B-002 (P2 residue): mlPoolCount()/mlDistinctClusterCount()/daemonInit()/
// daemonLoop() now read clustersSnapshot() instead of the bare ref. Hermetic main
// test returns the same empty copy (no lock in the stub).
std::vector<CpuCluster> clustersSnapshot() {
    return clusters();
}
// ROUND-007: crash-loop self-heal restore stub (production impl in aatped_control.cpp).
void restoreUserControlMode() {}
int abTestAssignThread(int) { return 0; }
int abTestAssignByUid(int) { return 0; }
void hmacForceReadonly(const char*) {}
void hmacClearReadonly(const char*) {}
void abTestRecordJank(int) {}
int emergencyMigrateCriticalThreads() { return 0; }
void precomputeCpuBaseScores() {}
// settle-band reject counters (normally defined in placement.cpp, which
// the main unit test does not link). diagTick references them for migrate_reject_*.
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

// #20: g_stateGeneration is now defined in aatped_utils.cpp; the main unit test
// does not link that TU, so provide a host-test definition here.
#include <atomic>
std::atomic<uint64_t> g_stateGeneration{0};

// MAIN-04/05/07: aatped_main.cpp now takes g_stateDiagMutex on the write side for
// g_runtimeDecisionIntervalMs, g_ab* and the flat audio/scene publish. The real
// mutex is defined in aatped_state.cpp (not linked by the `main` recipe); provide
// a host-test instance here so the lock_guard calls link cleanly.
std::mutex g_stateDiagMutex;

// Pre-existing: scoringHistorySweepStale() is called from daemonInit(); the
// `main` recipe does not link scoring.cpp. Provide a no-op host stub.
void scoringHistorySweepStale() {}

// remaining daemon functions referenced by main.cpp
std::string aatpeDetectForegroundApp(bool) { return std::string(); }
void autoSelectProfile() {}
void checkAndPreloadNewApps() {}
void cleanupDecisionsDir() {}
bool detectAudioPlayback(void) { return false; }
GameScene detectGameScene(float, float, float) { return GameScene::UNKNOWN; }
float getAveragePredictionMAE() { return 0.0f; }
float getPredictionConfidence(int) { return 0.5f; }
__attribute__((weak)) void readBatteryHealth() {}
__attribute__((weak)) void readBatteryPower() {}
void readGpuUtilization() {}
void refreshCpuTopology() {}
void reprobeUnmigratableThreads() {}
void scanThreads() {}
void writeConfigJson() {}
void writeDecisionsJson() {}
void writeC4Json() {}   // v4.4.6: host stub (real impl in webui_json.cpp)
void writeExplainJson() {}
void writeFusionWeightsState() {}
void writeLogsJson() {}
void writeModelJson() {}
void writePerCpuJson() {}
bool writeFile(const char*, float) { return true; }
bool writeFile(const char*, long long) { return true; }

// abtest / placement / hmac / scoring surface
ABTestStats g_abStats;
__attribute__((weak)) int g_batteryHealthPercent = 100;
bool g_hmacActive = false;
std::string g_hmacKeyStatus = "none";
std::mutex g_hmacMutex;
int abTestDetermineWinner() { return 0; }
void abTestRecordMigration(int) {}
void recordMigrationCount(int) {}
// v6.0.0 P1: the passive shadow sampler in aatped_main.cpp now calls the pure
// observation function extractMlFeatures(ti, ti.currentCpu), defined in
// scoring.cpp. The hermetic `main` recipe deliberately does NOT link scoring.o
// (it conflicts with these stubs and pulls in topology globals), so provide a
// host stub here. It returns value-initialized (all-zero) features: the passive
// sampler's cold-start isAllZeroFeatures() guard then skips booking shadow
// samples in this integration test — which is exactly the safe behavior. The
// REAL extractor contract is covered end-to-end by tests/v94/
// test_ml_shadow_passive_v600.cpp (links scoring.o + seeds g_cpuMetrics).
aatpe_ml::ThreadFeatures extractMlFeatures(const ThreadInfo&, int) {
    return aatpe_ml::ThreadFeatures{};
}
void recordPlacementResult(int, int, bool) {}
void recordWebuiDecision(int, const std::string&, const std::string&, int, int, int, int, const std::string&) {}
void decayHistory() {}
void reloadConfig() {}
void reloadConfigIfChanged() {}
// v94-fix#9: daemonLoop now calls checkAndApplyPerAppConfig() on every fg app
// update. The main hermetic test links aatped_config.cpp's peers as stubs here
// (see loadConfig/reloadConfig above), so mirror that with a no-op. The real
// bodies live in aatped_config.cpp on-device.
void loadPerAppConfigs(const char*) {}
void checkAndApplyPerAppConfig(const char*) {}
// 问题4 fix: daemonLoop reads g_currentAppConfig.decisionIntervalMs after
// checkAndApplyPerAppConfig(); main_deps must provide the symbol.
PerAppConfig g_currentAppConfig;
// v94-fix#12: daemon shutdown now calls stateCacheNoteRemove(); the main hermetic
// test does not link aatped_utils.o, so stub it here like the other util symbols.
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
// #8/#9: frame/cold_start/thermal now call the shared argv exec (moved out of frame.o).
std::string aatpeExecArgv(const std::vector<std::string>&, const char*, int*, bool) { return std::string(); }
void checkHmacKeyRotation() {}
ThreadRole classifyThread(int, int, const std::string&, const std::string&, int&, std::string&, int*, unsigned long long) { return ROLE_S5_BACKGROUND; }
__attribute__((weak)) int g_batteryCycleCount = 0;
TracingStats g_traceDaemonLoop, g_traceDecidePlacement, g_traceComputeScore, g_traceApplyPlacement, g_traceEventResponse;
int readThreadCurrentCpu(int, int) { return -1; }
void traceUpdate(TracingStats&, long long) {}
void writeStateJson() {}

// cleanupThreadScoringState (scoring.cpp) and isPackageInGameList
// (aatped_config.cpp) are referenced by aatped_ebpf_agg / cold_start_rules /
// gpu_boost but the main unit test does not link scoring.cpp / aatped_config.cpp.
void cleanupThreadScoringState(int) {}
bool isPackageInGameList(const std::string&) { return false; }

// P1-7: setStateEnterTimeNow() is defined in aatped_state.cpp (not linked
// by the main hermetic test). Stub it.
void setStateEnterTimeNow() {}

// P0-fix: gpu_boost.cpp calls aatpeGetForegroundPackage() (defined in
// frame.cpp, not linked by the main hermetic test). Stub it.
std::string aatpeGetForegroundPackage() { return g_foregroundApp; }

// frame_binder.cpp links surface for hermetic main test.
int aatpeAndroidSdkInt() { return 30; }

// v4.3.6: host stubs for the wiring objects linked into the `main` recipe.
//   * aatpe::frameaware::getFrameIntervalMs() -- real in frame.cpp (huge, pulls
//     the full vsync/frame pipeline); on host we return a sane 60Hz default so the
//     touch boost timing branch has a deterministic frame interval.
//   * trim(std::string&) -- real in aatped_utils.cpp; aatped_psi.cpp parses psi
//     cgroup files with it. Provide the identical trim semantics on host.
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
// (readInt already provided by main_deps.cpp line ~24; no duplicate needed.)

// F-07: gpu_boost.cpp calls aatpeGpuRefreshUtilForBoost(). The `main` recipe
// links gpu_boost.o but NOT aatped_gpu.o (where the real impl lives), so we
// provide a weak no-op. Recipes that DO link aatped_gpu.cpp get the strong
// real implementation; weak is overridden.
__attribute__((weak)) void aatpeGpuRefreshUtilForBoost(void) {}

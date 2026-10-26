// v94 unit-test host stubs (P2 issue #32): neutral defaults for the production
// globals that aatped.h declares `extern`.
//
// The v94 hermetic unit tests link one or a few production TUs in isolation;
// thermal.cpp and webui_json.cpp reference many globals that live in aatped.cpp
// (and a handful defined across scoring/placement/frame TUs) on device. This TU
// provides LINKER definitions for those externs with neutral (zero/empty)
// values. It does NOT mimic real daemon behavior — it exists only to satisfy the
// linker, exactly like the older tests/stubs/{em,iface}_deps.cpp.
//
// NOTE: g_mlModel (aatpe_ml::LearningModel) requires aatpe_ml.cpp to be linked
// for its ctor/dtor; build.sh does that for every recipe that pulls this stub.
#include "aatped.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---- path pointers ----------------------------------------------------------
const char* STATE_DIR = "/tmp/aatpe_v94_state";
const char* WEBUI_DIR = "/tmp/aatpe_v94_state";
const char* DECISION_DIR = "/tmp/aatpe_v94_state";
const char* METRICS_DIR = "/tmp/aatpe_v94_state";

// GPU globals (aatped_gpu.cpp linked by topology test).
// P1-6: atomic<float> to match production type.
std::atomic<float> g_gpuUtilization{0.0f};
std::atomic<const char*> g_gpuType{"unknown"};
GpuState g_gpuState;

int g_cpuCount = 8;
std::atomic<time_t> g_startTime{0};
std::atomic<uint64_t> g_stateGeneration{0};

// ---- thermal state ----------------------------------------------------------
std::atomic<int> g_thermalCpu{0};
std::atomic<int> g_thermalSkin{0};
std::atomic<int> g_thermalBattery{0};
std::string g_thermalCpuZonePath;   // start EMPTY (no stale default) so the
                                    // v4.1.1 hotfix fallback/selection logic is
                                    // observable (charger/battery-only scans must
                                    // yield an EMPTY thermal_source). The regular
                                    // scenarios call readThermal() which
                                    // re-selects zone0 itself.
// P1-4 fix: event_monitor.cpp calls this to resolve the CPU thermal zone path.
// In production thermal.cpp defines it (under g_thermalMutex); the hermetic test
// returns the stub path directly.
__attribute__((weak)) std::string aatpe_getThermalCpuZonePath() { return g_thermalCpuZonePath; }
// P1-6: atomic<int> storing GameScene (cast to int) to match production.
std::atomic<int> g_currentScene{(int)GameScene::UNKNOWN};
std::atomic<bool> g_thermalEmergency{false};
std::atomic<bool> g_profileLock{false};
// P3-1: in-latch user mode override flag (production defines it in aatped_state.o).
std::atomic<bool> g_emergencyUserOverride{false};
// C1/C3 exploration: stubs for new globals (default off).
std::atomic<int> g_thermalHeadroomPct{0};
std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};
// round-4: referenced by aatped_control.cpp (applyHelperApkDetection).
std::atomic<bool> g_helperAutoSuppressed{false};

// ---- config / profile -------------------------------------------------------
Config g_cfg{};
Config g_preEmergencyCfg{};
bool g_preEmergencyAutoProfile = false;
std::string g_currentProfile;

// ---- thread / CPU bookkeeping ----------------------------------------------
std::vector<int> g_allCpus;
std::vector<int> g_littlePool;
std::vector<int> g_midPool;
std::vector<int> g_bigPool;
// ROUND-007e: clusters()/g_clusters moved to aatped_topology.cpp on device; for
// the webui_json hermetic test they live in webui_clusters_deps.cpp. Do NOT
// define them here: the topology target links the real aatped_topology.o which
// already provides clusters(), causing a multiple-definition link error.
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;
// v94-fix#11: auxiliary per-thread state tables now referenced by thread_scan.cpp
// (invalidateAuxThreadState). Defined here because the thread_scan host test links
// v94_daemon_deps but not placement_deps (the on-device home of these symbols).
std::unordered_map<int, time_t> g_lastMigrationTime;
std::mutex g_lastMigMutex;
std::unordered_map<int, time_t> g_migFailCooldown;
std::unordered_map<int, int> g_migFailCount;
std::unordered_map<int, int> g_migFailCpu;
std::unordered_map<int, long long> g_migFailBackoffUntilMs;
std::mutex g_migFailMutex;
std::set<int> g_unmigratableThreads;
std::mutex g_unmigratableMutex;
std::mutex g_cpuMetricsMutex;
std::map<int, CpuMetrics> g_cpuMetrics;
std::mutex g_cfgMutex;

// ---- topology RW lock (defined in placement.cpp:22 on-device). webui_json.cpp
//   takes it briefly inside writeModelJson/writeSystemJson to snapshot the CPU
//   pools. Linking the whole placement.o pulls in dozens of daemon globals
//   (g_migrationCount, g_dynamicScoreWeights, logMsg, detectCpuTopology, ...)
//   and conflicts with our stub decidePlacement/g_fusionWeightAccum, so per the
//   task's fallback clause we define just this mutex here. ---------------------
std::mutex g_topologyRwMutex;

// ---- federated / fusion / ML counters --------------------------------------
std::atomic<int> g_federatedRounds{0};
std::atomic<int> g_federatedExports{0};
std::atomic<int> g_federatedImports{0};
int g_federatedTrustedImports = 0;
bool g_batteryAged = false;
FusionWeightAccumulator g_fusionWeightAccum{};
std::atomic<long long> g_mlPendingCreated{0};
std::atomic<long long> g_mlPendingExpired{0};
std::atomic<long long> g_mlPendingTidReused{0};
std::atomic<long long> g_mlOutcomeMeasured{0};
std::atomic<long long> g_mlOutcomeValid{0};
std::atomic<long long> g_mlOutcomeSkipOther{0};
std::atomic<long long> g_mlOutcomeSkipReset{0};
std::atomic<long long> g_mlOutcomeSkipSaturated{0};
std::atomic<long long> g_mlOutcomeSkipSleeping{0};

// ---- I/O helpers (REAL implementations, not stubs): tests that drive sysfs
//   (thermal) need readFile/readInt to actually open the fake sysfs files. ----
#include <fstream>
int readInt(const char* path, int def) {
    std::ifstream f(path); int v; if (f >> v) return v; return def;
}
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return std::string();
    std::getline(f, s); return s;
}
bool writeFile(const char* /*path*/, const std::string& /*content*/) { return true; }
// v4.1.1 hotfix: REAL integer write so thermal_throttle/thermal_cpu_max land on
// disk and test_thermal.cpp's throttle0/fallbackonly scenarios can read them back.
bool writeFile(const char* path, int val) {
    std::ofstream f(path, std::ios::binary); if (!f) return false; f << val; return true;
}
bool writeFile(const char* /*path*/, float /*value*/) { return true; }
bool writeFile(const char* /*path*/, long long /*value*/) { return true; }
bool writeFileAtomic(const char* path, const std::string& content) {
    // Real atomic write (tmp + rename) so the command-queue test can exercise
    // the on-disk FIFO mechanics end to end.
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}
// v4.2.9 C2: thermal.cpp:1101 atomic-writes control_mode_effective on the
// thermal-emergency-resolve path. Host stub: no-op success. Only the thermal
// family (thermal/thermal_clusters/thermal_gate) links this stub, none of which
// links another stub that defines atomicWriteFile, so no multiple-definition.
bool atomicWriteFile(const char*, const std::string&) { return true; }

// ---- config snapshot / state machine (STATEFUL: tracks current state so
//   thermal's EMERGENCY debounce transition is observable).
//   NOTE: aatped_state.h declares g_systemState as std::atomic<int> (numeric
//   encoding of SystemState); getSystemState()/transitionToState() are the
//   typed accessors. ----------------------------------------------------------
void snapshotConfig(Config& out) { out = g_cfg; }
std::atomic<int> g_systemState{static_cast<int>(SystemState::NORMAL)};
SystemState getSystemState() { return static_cast<SystemState>(g_systemState.load()); }
bool transitionToState(SystemState s, const char* /*why*/) {
    g_systemState.store(static_cast<int>(s));
    g_thermalEmergency = (s == SystemState::EMERGENCY);
    return true;
}
void loadProfile(const std::string& /*profile*/) {}

// ---- scoring / placement hooks (neutral) -----------------------------------
// P1-7: thread_scan.cpp now calls cleanupThreadScoringState() on the gone-thread
// branch (defined in scoring.cpp on-device). Provide a neutral stub here so the
// thread_scan hermetic test links without pulling in scoring.cpp.
void cleanupThreadScoringState(int /*tid*/) {}
int computeScore(int /*cpuId*/, const ThreadInfo& /*ti*/) { return 50; }
int computeScore(int /*cpuId*/, const ThreadInfo& /*ti*/, int /*knownFromCpu*/) { return 50; }
// webui_json.cpp calls the context-aware overload + buildScoringContext directly
// (writeExplainJson/writeThreadsJson). Provide neutral stubs so the webui_json
// hermetic test links without pulling in the full scoring.cpp TU.
#include "scoring_interface.h"
ScoringContext buildScoringContext(const Config& cfg) {
    ScoringContext ctx; ctx.cfg = &cfg; return ctx;
}
int computeScore(int /*cpuId*/, const ThreadInfo& /*ti*/, int /*knownFromCpu*/,
                 const ScoringContext& /*ctx*/) { return 50; }
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
int capacityScore(int, ThreadRole) noexcept { return 0; }
int latencyScore(int, ThreadRole) noexcept { return 0; }
int powerScore(int, ThreadRole) noexcept { return 0; }
int frameScore(int /*cpuId*/, ThreadRole /*role*/) { return 0; }
int poolScore(int /*cpuId*/, ThreadRole /*role*/) { return 0; }
int thermalScore(int) noexcept { return 0; }
int contentionPenalty(int) noexcept { return 0; }
float readCpuUtilizationSchedstat(int /*cpuId*/) { return 50.0f; }
Decision decidePlacement(const ThreadInfo& /*ti*/, const Config& /*cfg*/, bool /*emergency*/) { Decision d; return d; }
std::string aatpeExec(const std::string& /*cmd*/, const char* /*tag*/) { return std::string(); }
// #9: thermal.cpp's aatpeExecCapture now calls the shared argv exec.
std::string aatpeExecArgv(const std::vector<std::string>& /*argv*/, const char* /*tag*/, int*, bool) { return std::string(); }
// #19: anyCpufreqCoolingActive reads cooling_device*/type. On host the thermal test
// root has no cooling_device* nodes, so return "" (not-a-cpufreq device) — safe no-op.
#include <fstream>
std::string readFirstLine(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return ""; std::getline(f, s); return s;
}
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot() {
    aatpe_ml::FrameTimeOutcome o{}; return o;
}

// ---- ML model singleton (defined in aatped.cpp on-device) ------------------
std::mutex g_mlMutex;
aatpe_ml::LearningModel g_mlModel;

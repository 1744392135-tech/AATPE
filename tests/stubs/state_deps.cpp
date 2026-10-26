// state_deps.cpp - host link deps for aatped_state.cpp. state.cpp itself defines
// getSystemState/transitionToState/g_lastTransitionReason/g_stateMutex/g_stateHistory;
// this provides the remaining background globals without colliding with them.
#include "aatped.h"

Config g_cfg{};
std::mutex g_cfgMutex;
std::atomic<bool> g_thermalEmergency{false};
Config g_preEmergencyCfg{};
bool g_preEmergencyAutoProfile = false;
std::string g_preEmergencyProfile;
std::string g_currentProfile;

const char* STATE_DIR = "/tmp/aatpe_v94_state";
bool atomicWriteFile(const char*, const std::string&) { return true; }

// ROUND-005: G5 placement spillover accessor stubs for state snapshot tests.
namespace aatpe { namespace placement {
    std::uint64_t spilloverPromotionCount() noexcept { return 0; }
    int spilloverActiveCount() noexcept { return 0; }
} }
bool atomicWriteFile(const char*, long long) { return true; }

// remaining background externals referenced by state.cpp
std::atomic<uint64_t> g_stateGeneration{0};
std::atomic<time_t> g_startTime{0};
std::mutex g_foregroundMutex;
std::string g_foregroundApp;
std::atomic<int> g_thermalCpu{0}, g_thermalSkin{0};
std::atomic<long long> g_totalDecisions{0};
std::atomic<long long> g_totalMigrations{0};
std::unordered_map<int, ThreadInfo> g_threads;
// P1-2: writeSnapshotJson() now takes g_threadsMutex around g_threads.size();
// the state host test links aatped_state.cpp in isolation, so this stub must provide
// the mutex (other dep stubs already do) and g_selfCpuUsagePercent (readCpuMetrics).
std::mutex g_threadsMutex;
// P1-5: writeSnapshotJson() snapshots g_littlePool/mid/big under g_topologyRwMutex.
std::mutex g_topologyRwMutex;
// P1-1: atomic<int> to match production type.
std::atomic<int> g_selfCpuUsagePercent{0};
FusionWeightAccumulator g_fusionWeightAccum{};
std::mutex g_mlMutex;
aatpe_ml::LearningModel g_mlModel;

void snapshotConfig(Config& out) { out = g_cfg; }
// arbitrateAndApplyLocked() now calls loadProfile() on a profile
// request; the state host test links aatped_state.cpp in isolation and must
// provide this symbol (mirrors frame_deps/main_deps/v94_daemon_deps stubs).
void loadProfile(const std::string&) {}
#include <fstream>
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return "";
    std::getline(f, s); return s;
}
int readInt(const char* path, int def) {
    std::ifstream f(path); int v; if (f >> v) return v; return def;
}
// v6.0.0 P3-3: 64-bit sibling of readInt() (production now reads cumulative
// shadow counters via readInt64); mirror it here so the hermetic state test
// links. Same lenient read semantics.
long long readInt64(const char* path, long long def) {
    std::ifstream f(path); long long v; if (f >> v) return v; return def;
}

// more externals referenced by state.cpp (telemetry/serialization path)
int g_batteryCycleCount = 0, g_batteryHealthPercent = 100;
bool g_batteryAged = false;
// P1-6: atomic<bool> to match production type.
std::atomic<bool> g_ebpfActive{false};
std::string g_ebpfLoader;
// P1-2 fix: leaf mutexes referenced by aatped_state.cpp writeStateJson().
// The production aatped_main.cpp / aatped_gpu.cpp define the real ones; the
// hermetic state test links this stub TU instead.
std::mutex g_ebpfInfoMutex;
std::mutex g_gpuDualMutex;
std::vector<int> g_littlePool, g_midPool, g_bigPool;
int g_cpuCount = 8;
int g_runtimeDecisionIntervalMs = 100;
std::atomic<int> g_thermalBattery{0};
GpuState g_gpuState{};
AudioState g_audioState{};
SceneState g_sceneState{};
std::mutex g_hmacMutex;
// v94 stub-integration: aatped_state.cpp snapshots the battery triple under
// g_batteryStateMutex (real home aatped_battery.cpp:56) and the app-launch
// history under g_appLaunchHistoryMutex (real home aatped_applaunch.cpp:10).
// The state hermetic recipe links aatped_state.o (which self-defines
// g_stateDiagMutex) in isolation, without aatped_battery.o / aatped_applaunch.o,
// so provide both leaf mutexes here.
std::mutex g_batteryStateMutex;
std::mutex g_appLaunchHistoryMutex;
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot() { return {}; }
const char* sceneToString(GameScene) { return "UNKNOWN"; }
long long FusionWeightAccumulator::getAverageWeights(float& a, float& b, float& c, float& d) const {
    a = b = c = d = 0.25f; return 0;
}

bool g_hmacActive = false;
std::string g_hmacKeyStatus = "none";
std::atomic<int> g_federatedRounds{0}, g_federatedExports{0}, g_federatedImports{0};
int g_federatedTrustedImports = 0;
float g_abGroupAAvg = 0, g_abGroupBAvg = 0, g_abPValue = 1.0f, g_avgPredictionMAE = 0;
std::unordered_map<std::string, AppLaunchRecord> g_appLaunchHistory;

// ROUND-007d: aatped_state.cpp writeSnapshotJson() now reads clusters() from
// aatped_topology.cpp. The state hermetic test does not exercise clustering
// (covered by the dedicated topology target, 72 cases); return a fixed empty
// cluster set so writeSnapshotJson's serialization path still runs for real.
const std::vector<CpuCluster>& clusters() {
    static const std::vector<CpuCluster> empty;
    return empty;
}

// v4.2.5: frame_apps.json serializer stub (real in webui_json.cpp).
std::string aatpe_buildFrameAppsJson() { return "{\"current\":\"\",\"apps\":[]}"; }

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

namespace aatpe { namespace placement {
    std::uint64_t spilloverPromotionCount() noexcept { return 0; }
    int spilloverActiveCount() noexcept { return 0; }
} }
bool atomicWriteFile(const char*, long long) { return true; }

std::atomic<uint64_t> g_stateGeneration{0};
std::atomic<time_t> g_startTime{0};
std::mutex g_foregroundMutex;
std::string g_foregroundApp;
std::atomic<int> g_thermalCpu{0}, g_thermalSkin{0};
std::atomic<long long> g_totalDecisions{0};
std::atomic<long long> g_totalMigrations{0};
std::unordered_map<int, ThreadInfo> g_threads;
std::mutex g_threadsMutex;
std::mutex g_topologyRwMutex;
std::atomic<int> g_selfCpuUsagePercent{0};
FusionWeightAccumulator g_fusionWeightAccum{};
std::mutex g_mlMutex;
aatpe_ml::LearningModel g_mlModel;

void snapshotConfig(Config& out) { out = g_cfg; }
void loadProfile(const std::string&) {}
#include <fstream>
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return "";
    std::getline(f, s); return s;
}
int readInt(const char* path, int def) {
    std::ifstream f(path); int v; if (f >> v) return v; return def;
}
long long readInt64(const char* path, long long def) {
    std::ifstream f(path); long long v; if (f >> v) return v; return def;
}

int g_batteryCycleCount = 0, g_batteryHealthPercent = 100;
bool g_batteryAged = false;
std::atomic<bool> g_ebpfActive{false};
std::string g_ebpfLoader;
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

const std::vector<CpuCluster>& clusters() {
    static const std::vector<CpuCluster> empty;
    return empty;
}

std::string aatpe_buildFrameAppsJson() { return "{\"current\":\"\",\"apps\":[]}"; }

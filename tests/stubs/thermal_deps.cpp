#include "aatped.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

const char* STATE_DIR = "/tmp/aatpe_v94_state";
const char* WEBUI_DIR = "/tmp/aatpe_v94_state";
const char* DECISION_DIR = "/tmp/aatpe_v94_state";
const char* METRICS_DIR = "/tmp/aatpe_v94_state";

std::atomic<float> g_gpuUtilization{0.0f};
std::atomic<const char*> g_gpuType{"unknown"};
GpuState g_gpuState;

int g_cpuCount = 8;
std::atomic<time_t> g_startTime{0};
std::atomic<uint64_t> g_stateGeneration{0};

std::atomic<int> g_thermalCpu{0};
std::atomic<int> g_thermalSkin{0};
std::atomic<int> g_thermalBattery{0};
std::string g_thermalCpuZonePath;
__attribute__((weak)) std::string aatpe_getThermalCpuZonePath() { return g_thermalCpuZonePath; }
std::atomic<int> g_currentScene{(int)GameScene::UNKNOWN};
std::atomic<bool> g_thermalEmergency{false};
std::atomic<bool> g_profileLock{false};
std::atomic<bool> g_emergencyUserOverride{false};
std::atomic<int> g_thermalHeadroomPct{0};
std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};
std::atomic<bool> g_helperAutoSuppressed{false};

Config g_cfg{};
Config g_preEmergencyCfg{};
bool g_preEmergencyAutoProfile = false;
std::string g_currentProfile;

std::vector<int> g_allCpus;
std::vector<int> g_littlePool;
std::vector<int> g_midPool;
std::vector<int> g_bigPool;
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;
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

std::mutex g_topologyRwMutex;

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

#include <fstream>
int readInt(const char* path, int def) {
    std::ifstream f(path); int v; if (f >> v) return v; return def;
}
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return std::string();
    std::getline(f, s); return s;
}
bool writeFile(const char* , const std::string& ) { return true; }
bool writeFile(const char* path, int val) {
    std::ofstream f(path, std::ios::binary); if (!f) return false; f << val; return true;
}
bool writeFile(const char* , float ) { return true; }
bool writeFile(const char* , long long ) { return true; }
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}
bool atomicWriteFile(const char*, const std::string&) { return true; }

void snapshotConfig(Config& out) { out = g_cfg; }
std::atomic<int> g_systemState{static_cast<int>(SystemState::NORMAL)};
SystemState getSystemState() { return static_cast<SystemState>(g_systemState.load()); }
bool transitionToState(SystemState s, const char* ) {
    g_systemState.store(static_cast<int>(s));
    g_thermalEmergency = (s == SystemState::EMERGENCY);
    return true;
}
void loadProfile(const std::string& ) {}

void cleanupThreadScoringState(int ) {}
int computeScore(int , const ThreadInfo& ) { return 50; }
int computeScore(int , const ThreadInfo& , int ) { return 50; }
#include "scoring_interface.h"
ScoringContext buildScoringContext(const Config& cfg) {
    ScoringContext ctx; ctx.cfg = &cfg; return ctx;
}
int computeScore(int , const ThreadInfo& , int ,
                 const ScoringContext& ) { return 50; }
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
int capacityScore(int, ThreadRole) noexcept { return 0; }
int latencyScore(int, ThreadRole) noexcept { return 0; }
int powerScore(int, ThreadRole) noexcept { return 0; }
int frameScore(int , ThreadRole ) { return 0; }
int poolScore(int , ThreadRole ) { return 0; }
int thermalScore(int) noexcept { return 0; }
int contentionPenalty(int) noexcept { return 0; }
float readCpuUtilizationSchedstat(int ) { return 50.0f; }
Decision decidePlacement(const ThreadInfo& , const Config& , bool ) { Decision d; return d; }
std::string aatpeExec(const std::string& , const char* ) { return std::string(); }
std::string aatpeExecArgv(const std::vector<std::string>& , const char* , int*, bool) { return std::string(); }
#include <fstream>
std::string readFirstLine(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return ""; std::getline(f, s); return s;
}
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot() {
    aatpe_ml::FrameTimeOutcome o{}; return o;
}

std::mutex g_mlMutex;
aatpe_ml::LearningModel g_mlModel;

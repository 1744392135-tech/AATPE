#include "aatped.h"
#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"
#include "scoring_interface.h"
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <sched.h>
#include <cstdarg>
#include <cstdio>

using namespace aatpe;
using namespace aatpe_ml;

RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;
PerAppConfig g_currentAppConfig;
std::atomic<bool> g_frameCritical{false};

namespace aatpe { namespace frameaware { int getRefreshRate() { return 60; } } }

const char* RUN_DIR      = "/tmp/aatpe_v94_placement";
const char* STATE_DIR    = "/tmp/aatpe_v94_placement";
const char* DECISION_DIR = "/tmp/aatpe_v94_placement";

int g_cpuCount = 8;
std::vector<int> g_allCpus = {0,1,2,3,4,5,6,7};
std::vector<int> g_littlePool = {0,1,2,3};
std::vector<int> g_midPool    = {4,5,6};
std::vector<int> g_bigPool    = {7};
std::vector<CpuCluster> g_clusters = {
    {{0,1,2,3},  239,  400000, 1800000, 0, CpuTier::LITTLE},
    {{4,5,6},   800,   400000, 2000000, 4, CpuTier::BIG},
    {{7},       1024,  400000, 2600000, 7, CpuTier::PRIME},
};
CpuTier tierOfCpu(int cpu) {
    for (const auto& cl : g_clusters)
        if (std::find(cl.cpus.begin(), cl.cpus.end(), cpu) != cl.cpus.end()) return cl.tier;
    return CpuTier::UNKNOWN;
}
const std::vector<CpuCluster>& clusters() {
    return g_clusters;
}
std::vector<CpuCluster> clustersSnapshot() {
    return g_clusters;
}
std::vector<int> primePool() {
    if (!g_clusters.empty() && g_clusters.back().tier == CpuTier::PRIME)
        return g_clusters.back().cpus;
    return {};
}
std::vector<int> primePoolLocked() {
    if (!g_clusters.empty() && g_clusters.back().tier == CpuTier::PRIME)
        return g_clusters.back().cpus;
    return {};
}

std::mutex g_cpuMetricsMutex;
std::map<int, CpuMetrics> g_cpuMetrics;

std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

Config g_cfg{};
std::mutex g_cfgMutex;
ProfileType g_currentProfileEnum = ProfileType::BALANCED;

DynamicScoreWeights g_dynamicScoreWeights{};
std::mutex g_dynamicWeightsMutex;

std::atomic<int> g_thermalMigrationPressure{0};
std::atomic<bool> g_thermalEmergency{false};
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<long long> g_keepSafetyLatency{0};
std::atomic<long long> g_applySafetyLatency{0};

std::atomic<long long> g_totalMigrations{0};
std::atomic<long long> g_keepProtected{0};
std::atomic<long long> g_keepPassive{0};
std::atomic<long long> g_keepThermalEmergency{0};
std::atomic<long long> g_keepNoCpu{0};
std::atomic<long long> g_keepLowConf{0};
std::atomic<long long> g_keepHighCost{0};
std::atomic<long long> g_keepSafeMode{0};
std::atomic<long long> g_keepAlreadyOptimal{0};
std::atomic<long long> g_migrateActual{0};
std::atomic<long long> g_applySuccess{0};
std::atomic<long long> g_applyFailed{0};
std::atomic<long long> g_applyFailedEinval{0};
std::atomic<long long> g_applyFailedEsrch{0};
std::atomic<long long> g_applyFailedEperm{0};
std::atomic<long long> g_applyFailedOther{0};
std::atomic<long long> g_applyRateLimited{0};
std::atomic<long long> g_applyPassive{0};
std::atomic<long long> g_applyThermalEmergency{0};
std::atomic<long long> g_applyCooldownSkip{0};
std::atomic<long long> g_applyCpusetSkip{0};
CpusetSkipSample g_lastCpusetSkip;
std::mutex g_lastCpusetSkipMutex;
std::atomic<long long> g_applyKernelSkip{0};
std::atomic<long long> g_applyUnmigratableSkip{0};
std::atomic<long long> g_applyIntervalSkip{0};
std::atomic<unsigned int> g_debugSampleCounter{0};

std::atomic<long long> g_cooldownBypassCount{0};

std::set<int> g_unmigratableThreads;
std::mutex g_unmigratableMutex;

int g_migrationCount = 0;
time_t g_migrationWindowStart = 0;
std::mutex g_lastMigMutex;
std::unordered_map<int, time_t> g_lastMigrationTime;
std::mutex g_migFailMutex;
std::unordered_map<int, time_t> g_migFailCooldown;
std::unordered_map<int, int> g_migFailCount;
std::unordered_map<int, int> g_migFailCpu;
std::unordered_map<int, long long> g_migFailBackoffUntilMs;
time_t g_cooldownFloorWindowStart = 0;

std::mutex g_mlMutex;
LearningModel g_mlModel;

std::atomic<int> g_audioIsolatedCpu{-1};
int getAudioThreadMigrationPenalty(const char*) { return 0; }

std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;
int placementSuccessScore(int , int ) { return 0; }

float hybridScore(int, const ThreadInfo&) { return 0.0f; }
int computeScore(int, const ThreadInfo&, int) { return 0; }
ScoringContext buildScoringContext(const Config& cfg) {
    ScoringContext ctx;
    ctx.cfg = &cfg;
    return ctx;
}
int computeScore(int, const ThreadInfo&, int, const ScoringContext&) { return 0; }
aatpe_ml::ThreadFeatures extractMlFeaturesCtx(const ThreadInfo&, int, const ScoringContext&) {
    aatpe_ml::ThreadFeatures f{}; return f;
}
bool isCpuAllowed(int cpu, const cpu_set_t&) { return cpu >= 0; }
std::string readFirstLine(const char*) { return std::string(); }
SystemState getSystemState() { return SystemState::NORMAL; }
void snapshotConfig(Config& out) { std::lock_guard<std::mutex> lk(g_cfgMutex); out = g_cfg; }
void detectCpuTopology() {}
ThreadFeatures extractMlFeatures(const ThreadInfo&, int) {
    ThreadFeatures f{}; return f;
}
cpu_set_t getThreadAllowedCpus(int) {
    cpu_set_t s; CPU_ZERO(&s);
    for (int i = 0; i < 8; ++i) CPU_SET(i, &s);
    return s;
}
int readThreadCurrentCpu(int, int) { return 0; }
float getPredictionConfidence(int) { return 0.5f; }
float normalizeHeuristicScore(int) { return 0.5f; }
float predictCpuLoadMultiStep(int, int) { return 50.0f; }
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot() {
    aatpe_ml::FrameTimeOutcome o{}; return o;
}
void logMsg(int, const char*, ...) {  }
#include <fcntl.h>
#include <unistd.h>
bool writeFile(const char* path, const std::string& content) {
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, content.data(), content.size());
    ::close(fd);
    return n == (ssize_t)content.size();
}
bool writeFile(const char* p, int v) { return writeFile(p, std::to_string(v)); }
bool writeFile(const char* p, long long v) { return writeFile(p, std::to_string(v)); }
void trim(std::string& s) {
    while (!s.empty() && (s.back()==' '||s.back()=='\n'||s.back()=='\r'||s.back()=='\t'))
        s.pop_back();
    size_t p = 0; while (p < s.size() && (s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t')) ++p;
    s = s.substr(p);
}
cpu_set_t parseCpusetCpuList(const std::string&) {
    cpu_set_t s; CPU_ZERO(&s);
    for (int i = 0; i < 8; ++i) CPU_SET(i, &s);
    return s;
}

void invalidateAllScores() {}
void precomputeCpuBaseScores() {}
std::atomic<uint64_t> g_topologyGeneration{0};
std::mutex g_scoreCacheMutex;
std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
void stateCacheNoteRemove(const char*) {}
bool atomicWriteFile(const char*, long long) { return true; }
bool atomicWriteFile(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (n != (ssize_t)content.size()) return false;
    return ::rename(tmp.c_str(), path) == 0;
}

#include <fstream>
#include <cstdio>
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}

std::string g_cachedForegroundApp;
long long g_foregroundCacheTimeMs = 0;
std::mutex g_foregroundMutex;
std::string getCachedForegroundApp(void) {
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    return g_cachedForegroundApp;
}
namespace aatpe { namespace subsys { namespace safemode {
    bool isActive() { return false; }
    bool rtBoostAllowed() { return true; }
    bool aggressiveMigrationAllowed() { return true; }
}}}

namespace aatpe { namespace frameaware {
__attribute__((weak)) bool jankBoostVoteActive() { return false; }
}}

#include "aatpe_diag.h"
namespace aatpe { namespace diag {
__attribute__((weak)) SystemPressure getSystemPressureSnapshot() { return {}; }
__attribute__((weak)) int64_t getThreadRqWaitNs(int) { return -1; }
__attribute__((weak)) int64_t getThreadRunNs(int) { return -1; }
}}

#include <string>
namespace aatpe { namespace caps {
struct DeviceCaps { int cgroupVer=1; std::string cpuRoot="/dev/cpuctl"; bool uclampCgroupV1=false,uclampCgroupV2=false; };
__attribute__((weak)) DeviceCaps getCaps() { return {}; }
__attribute__((weak)) std::string resolveCgroupProcsPath(const std::string&) { return {}; }
__attribute__((weak)) bool writeTidToCgroupGroup(int, const std::string&) { return true; }
}}

#include <cstdint>
namespace aatpe { namespace recovery {
void recoveryRefreshAppliedAffinity(int, const std::string&) {}
bool recoveryRefreshAppliedSched(int, uint32_t, uint32_t, int, uint32_t, uint32_t) { return false; }
}}
std::string readFile(const char*) { return {}; }

namespace aatpe {
void aatpeBoostSet(const char* , int ) {}
void aatpeBoostClear(const char* ) {}
}

namespace aatpe {
__attribute__((weak)) void coldStartSetIoPressure(bool) {}
}
__attribute__((weak)) int readInt(const char* , int def) { return def; }
namespace aatpe { __attribute__((weak)) int aatpeThermalHalLevel() { return 0; } }

std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};

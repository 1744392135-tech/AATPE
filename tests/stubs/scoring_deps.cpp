#include "aatped.h"
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <fstream>
#include <sys/stat.h>

using namespace aatpe;
using namespace aatpe_ml;

const char* RUN_DIR = "/tmp/aatpe_v94_scoring";

std::atomic<time_t> g_startTime{0};

int g_cpuCount = 8;
std::vector<int> g_allCpus = {0,1,2,3,4,5,6,7};
std::vector<int> g_littlePool = {0,1,2,3};
std::vector<int> g_midPool    = {4,5};
std::vector<int> g_bigPool    = {6};
std::mutex g_topologyRwMutex;
std::atomic<uint64_t> g_topologyGeneration{0};
std::vector<CpuCluster> g_clusters = {
    { {0,1,2,3}, 153,  200000, 1800000, 0, CpuTier::LITTLE },
    { {4,5},     512,  200000, 2200000, 4, CpuTier::MID    },
    { {6},       768,  200000, 2500000, 6, CpuTier::BIG    },
    { {7},      1024,  200000, 2700000, 7, CpuTier::PRIME  },
};

std::mutex g_cpuMetricsMutex;
std::map<int, CpuMetrics> g_cpuMetrics;

std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

Config g_cfg{};
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
std::atomic<bool> g_frameCritical{false};

DynamicScoreWeights g_dynamicScoreWeights{};
std::mutex g_dynamicWeightsMutex;

std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;

std::string g_foregroundApp;
std::string aatpeGetForegroundPackage() { return g_foregroundApp; }

std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return std::string();
    std::getline(f, s); return s;
}
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
void mkdirIfNotExist(const char* path, mode_t mode) { (void)path; (void)mode; }

int findPidByPackage(const std::string&) { return -1; }
bool isPackageInGameList(const std::string&) { return false; }

int g_stubCurrentCpu = 0;
cpu_set_t g_stubAffinityMask;
static void initStubAffinityMask() {
    CPU_ZERO(&g_stubAffinityMask);
    for (int i = 0; i < 8; i++) CPU_SET(i, &g_stubAffinityMask);
}
static bool g_stubAffinityInit = [](){ initStubAffinityMask(); return true; }();
int readThreadCurrentCpu(int , int ) { return g_stubCurrentCpu; }
cpu_set_t getThreadAffinityCpus(int ) { return g_stubAffinityMask; }

int computePowerPenalty(int , int ) { return 0; }
int getBatteryAgePenalty(int ) { return 0; }

std::mutex g_cfgMutex;
void snapshotConfig(Config& out) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    out = g_cfg;
}

// scoring_deps.cpp — minimal host stubs for scoring.cpp's daemon-global deps.
//
// scoring.cpp is a big translation unit that reads/writes many globals that
// live in aatped.cpp on-device. The scoring unit test links scoring.cpp +
// aatpe_ml.cpp in isolation; this TU provides LINKER definitions for every
// extern scoring.cpp references. It does NOT mimic daemon behavior — it just
// gives neutral defaults so the pure scoring math can be exercised.
//
// IMPORTANT: this TU is used ONLY by test_scoring.cpp. The other v94 tests
// that need daemon deps keep using v94_daemon_deps.cpp; the two stubs must
// never be linked together (they both define g_cpuMetrics etc.).
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

// ---- path pointer ----------------------------------------------------------
const char* RUN_DIR = "/tmp/aatpe_v94_scoring";

// ML-10: canonical daemon boot timestamp (real definition lives in
// aatped_main.cpp). scoring.cpp now stamps per-tid history files with it and
// sweeps stale files at startup; the test drives the sweep via this global.
std::atomic<time_t> g_startTime{0};

// ---- CPU bookkeeping -------------------------------------------------------
int g_cpuCount = 8;
std::vector<int> g_allCpus = {0,1,2,3,4,5,6,7};
std::vector<int> g_littlePool = {0,1,2,3};
std::vector<int> g_midPool    = {4,5};
std::vector<int> g_bigPool    = {6};
std::mutex g_topologyRwMutex;
// G7 R004-B2 round2: topology generation counter used for capacity re-calibration.
std::atomic<uint64_t> g_topologyGeneration{0};
// G7 R004-B2: generic clusters (weak->strong). Synthetic 4L+2M+1B+1PRIME so the
// full prime/big/mid/little ordering in poolScore is exercised.
//   cpu0-3 LITTLE(153) / cpu4-5 MID(512) / cpu6 BIG(768) / cpu7 PRIME(1024).
std::vector<CpuCluster> g_clusters = {
    { {0,1,2,3}, 153,  200000, 1800000, 0, CpuTier::LITTLE },
    { {4,5},     512,  200000, 2200000, 4, CpuTier::MID    },
    { {6},       768,  200000, 2500000, 6, CpuTier::BIG    },
    { {7},      1024,  200000, 2700000, 7, CpuTier::PRIME  },
};

// ---- per-CPU metrics -------------------------------------------------------
std::mutex g_cpuMetricsMutex;
std::map<int, CpuMetrics> g_cpuMetrics;

// ---- thread bookkeeping (scoring.cpp touches g_threads in decayHistory) -----
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

// ---- config / profile ------------------------------------------------------
Config g_cfg{};
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
std::atomic<bool> g_frameCritical{false};

// ---- dynamic weights -------------------------------------------------------
DynamicScoreWeights g_dynamicScoreWeights{};   // valid=false by default
std::mutex g_dynamicWeightsMutex;

// ---- placement success history --------------------------------------------
std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;

// ---- foreground app (used by refreshGameSceneCache) -----------------------
std::string g_foregroundApp;
// P0-5: scoring.cpp now reads the foreground package through the locked getter.
std::string aatpeGetForegroundPackage() { return g_foregroundApp; }

// ---- neutral I/O / syscall helpers ---------------------------------------
std::string readFile(const char* path) {
    std::ifstream f(path); std::string s; if (!f) return std::string();
    std::getline(f, s); return s;
}
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int) { return true; }
void mkdirIfNotExist(const char* path, mode_t mode) { (void)path; (void)mode; }

int findPidByPackage(const std::string&) { return -1; }
bool isPackageInGameList(const std::string&) { return false; }

// readThreadCurrentCpu / getThreadAffinityCpus: return values the test can
// control via these globals. Default to "tid already on cpu 0, all CPUs allowed".
int g_stubCurrentCpu = 0;
cpu_set_t g_stubAffinityMask;
// Helper: set all 8 CPUs in the stub mask
static void initStubAffinityMask() {
    CPU_ZERO(&g_stubAffinityMask);
    for (int i = 0; i < 8; i++) CPU_SET(i, &g_stubAffinityMask);
}
static bool g_stubAffinityInit = [](){ initStubAffinityMask(); return true; }();
int readThreadCurrentCpu(int /*pid*/, int /*tid*/) { return g_stubCurrentCpu; }
cpu_set_t getThreadAffinityCpus(int /*tid*/) { return g_stubAffinityMask; }

int computePowerPenalty(int /*cpuId*/, int /*priority*/) { return 0; }
int getBatteryAgePenalty(int /*cpuId*/) { return 0; }

// snapshotConfig: scoring.cpp calls it from frameScore/decayHistory/
// placementSuccessScore/recordPlacementResult. On host there is no hot-reload
// thread, so a plain copy under g_cfgMutex is enough.
std::mutex g_cfgMutex;
void snapshotConfig(Config& out) {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    out = g_cfg;
}

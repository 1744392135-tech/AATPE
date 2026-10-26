// placement_deps.cpp — minimal host stubs for placement.cpp's daemon globals.
//
// placement.cpp is the largest v94 TU: it calls into ~50 daemon globals and
// ~15 helper functions that live in aatped.cpp / thread_scan.cpp /
// aatped_state.cpp on device. This TU provides LINKER definitions with
// neutral defaults so the pure placement math (fuseDecisions, pushDecisionRecord,
// FusionWeightAccumulator, applyColdStartExploration, selectOptimalPlacement,
// thresholdGate) can be exercised in isolation.
//
// Do NOT link this together with v94_daemon_deps.cpp or scoring_deps.cpp
// (multiple definitions of g_cpuMetrics etc.).
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

// New global from aatped_main.cpp (frame.cpp uses it)
RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;
PerAppConfig g_currentAppConfig;
std::atomic<bool> g_frameCritical{false};

// Stub for frameaware::getRefreshRate (frame.cpp, too many deps to link whole frame.o)
namespace aatpe { namespace frameaware { int getRefreshRate() { return 60; } } }

// ---- path pointers --------------------------------------------------------
const char* RUN_DIR      = "/tmp/aatpe_v94_placement";
const char* STATE_DIR    = "/tmp/aatpe_v94_placement";
const char* DECISION_DIR = "/tmp/aatpe_v94_placement";

// ---- CPU topology ---------------------------------------------------------
int g_cpuCount = 8;
std::vector<int> g_allCpus = {0,1,2,3,4,5,6,7};
std::vector<int> g_littlePool = {0,1,2,3};
std::vector<int> g_midPool    = {4,5,6};
std::vector<int> g_bigPool    = {7};
// B3: G6 cluster model — D1100 shape (LITTLE 0-3, BIG 4-6, PRIME 7).
// BIG capacity set to 800 (not 1024) so WS-C same-capacity guard doesn't
// block BIG->PRIME spillover between adjacent tiers with real headroom.
std::vector<CpuCluster> g_clusters = {
    {{0,1,2,3},  239,  400000, 1800000, 0, CpuTier::LITTLE},
    {{4,5,6},   800,   400000, 2000000, 4, CpuTier::BIG},
    {{7},       1024,  400000, 2600000, 7, CpuTier::PRIME},
};
// host stub for the topology accessor used by the spillover sweep.
CpuTier tierOfCpu(int cpu) {
    for (const auto& cl : g_clusters)
        if (std::find(cl.cpus.begin(), cl.cpus.end(), cpu) != cl.cpus.end()) return cl.tier;
    return CpuTier::UNKNOWN;
}
const std::vector<CpuCluster>& clusters() {
    return g_clusters;
}
// B-002: host stub mirrors the lock-protected value copy (no lock in the stub).
std::vector<CpuCluster> clustersSnapshot() {
    return g_clusters;
}
std::vector<int> primePool() {
    if (!g_clusters.empty() && g_clusters.back().tier == CpuTier::PRIME)
        return g_clusters.back().cpus;
    return {};
}
// h6 D1: caller-holds-the-lock variant, same body as primePool() (stub has no lock).
std::vector<int> primePoolLocked() {
    if (!g_clusters.empty() && g_clusters.back().tier == CpuTier::PRIME)
        return g_clusters.back().cpus;
    return {};
}

// ---- per-CPU metrics ------------------------------------------------------
std::mutex g_cpuMetricsMutex;
std::map<int, CpuMetrics> g_cpuMetrics;

// ---- thread bookkeeping ---------------------------------------------------
std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

// ---- config / profile -----------------------------------------------------
Config g_cfg{};
std::mutex g_cfgMutex;   // PLACE-01: production placement.cpp now locks this when reading g_cfg
ProfileType g_currentProfileEnum = ProfileType::BALANCED;

// ---- dynamic weights -----------------------------------------------------
DynamicScoreWeights g_dynamicScoreWeights{};
std::mutex g_dynamicWeightsMutex;

// ---- fusion accumulator: g_fusionWeightAccum is DEFINED in placement.cpp:35.

// ---- thermal state --------------------------------------------------------
std::atomic<int> g_thermalMigrationPressure{0};
std::atomic<bool> g_thermalEmergency{false};
// v95-fix: independent safety-latency gate + diagnostic counters referenced by
// placement.cpp (decidePlacement/applyPlacement gates mirror thermal emergency).
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<long long> g_keepSafetyLatency{0};
std::atomic<long long> g_applySafetyLatency{0};

// ---- placement counters (all atomic<long long> defaults) ------------------
std::atomic<long long> g_totalMigrations{0};
std::atomic<long long> g_keepProtected{0};
std::atomic<long long> g_keepPassive{0};
std::atomic<long long> g_keepThermalEmergency{0};
std::atomic<long long> g_keepNoCpu{0};
std::atomic<long long> g_keepLowConf{0};
std::atomic<long long> g_keepHighCost{0};
std::atomic<long long> g_keepSafeMode{0}; // ordinary migration blocked by Safe Mode
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

// ---- cooldown bypass counter (used by applyPlacement) --------------------
std::atomic<long long> g_cooldownBypassCount{0};

// ---- unmigratable threads ------------------------------------------------
std::set<int> g_unmigratableThreads;
std::mutex g_unmigratableMutex;

// ---- migration rate-limit state ------------------------------------------
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

// ---- ML model singleton (defined in aatped.cpp on device) ---------------
std::mutex g_mlMutex;
LearningModel g_mlModel;

// ---- audio ----------------------------------------------------------------
std::atomic<int> g_audioIsolatedCpu{-1};
int getAudioThreadMigrationPenalty(const char*) { return 0; }

// ---- placement success history -------------------------------------------
std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;
int placementSuccessScore(int /*cpuId*/, int /*tid*/) { return 0; }

// ---- neutral helper functions --------------------------------------------
float hybridScore(int, const ThreadInfo&) { return 0.0f; }
int computeScore(int, const ThreadInfo&, int) { return 0; }
// P0-opt1: context-aware overloads (stubbed — placement test mocks scoring)
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
void logMsg(int, const char*, ...) { /* test: silent */ }
// Real file IO so promoted_tids / cgroup uclamp files can be read back in tests.
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

// aatpe_ml_hybrid::instance() is DEFINED in aatpe_ml_hybrid_main.cpp; we link
// that TU (plus _predict/_train/_persist/_alpha) so no stub is needed here.

// ---- P1-13: symbols pulled in by aatpe_state_ttl.o (linked for placementAvgRecordGain) --
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

// round-4: aatpe_recovery.cpp (now linked via placement recipe) needs a real
// writeFileAtomic. tmp+rename so recovery record writes are exercised for real.
#include <fstream>
#include <cstdio>
bool writeFileAtomic(const char* path, const std::string& content) {
    std::string tmp = std::string(path) + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << content; }
    return ::rename(tmp.c_str(), path) == 0;
}

// placement.cpp references the foreground-app cache directly (foreground
// critical-thread confidence relaxation) and the subsystem safe-mode gates. The
// hermetic placement test does NOT link aatpe_subsystem.cpp / aatped_ebpf_agg.cpp,
// so provide neutral link-time stubs here.
std::string g_cachedForegroundApp;
// ROUND-015: onForegroundSwitchEdge publishes the fresh package AND the cache
// timestamp into the placement-side fg cache (defeats the stale 3s-TTL window).
// The hermetic placement test does not link aatped_ebpf_agg.cpp, so provide the
// timestamp global here (the stub's getCachedForegroundApp ignores the TTL and
// just returns g_cachedForegroundApp, so writing the timestamp is a no-op here).
long long g_foregroundCacheTimeMs = 0;
// G5: placement.cpp now calls the lock-under accessor getCachedForegroundApp()
// (declared in aatped_ebpf_agg.h, defined in aatped_ebpf_agg.cpp on device) instead
// of reading g_cachedForegroundApp directly. The hermetic placement test does not
// link aatped_ebpf_agg.cpp, so provide the accessor here: return the cached string
// (empty by default) under a lock, matching the production fast-path contract.
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

// ROUND-007c: frame-aware jank boost vote stub for placement tests.
namespace aatpe { namespace frameaware {
// h6/D psi_fallback: weak so the psi_fallback test can provide a strong override.
__attribute__((weak)) bool jankBoostVoteActive() { return false; }
}}

// ROUND-009c weak stubs for diag collector (host tests) — use real header ABI.
#include "aatpe_diag.h"
namespace aatpe { namespace diag {
__attribute__((weak)) SystemPressure getSystemPressureSnapshot() { return {}; }
__attribute__((weak)) int64_t getThreadRqWaitNs(int) { return -1; }
__attribute__((weak)) int64_t getThreadRunNs(int) { return -1; }
}}

// WS-C caps stubs.
#include <string>
namespace aatpe { namespace caps {
struct DeviceCaps { int cgroupVer=1; std::string cpuRoot="/dev/cpuctl"; bool uclampCgroupV1=false,uclampCgroupV2=false; };
__attribute__((weak)) DeviceCaps getCaps() { return {}; }
__attribute__((weak)) std::string resolveCgroupProcsPath(const std::string&) { return {}; }
__attribute__((weak)) bool writeTidToCgroupGroup(int, const std::string&) { return true; }
}}

// ROUND-013: recovery refresh + readFile stubs for placement/mech link.
#include <cstdint>
namespace aatpe { namespace recovery {
void recoveryRefreshAppliedAffinity(int, const std::string&) {}
bool recoveryRefreshAppliedSched(int, uint32_t, uint32_t, int, uint32_t, uint32_t) { return false; }
}}
std::string readFile(const char*) { return {}; }

// v4.2 A2: placement.cpp now joins the schedtune single-writer bridge via
// aatpeBoostSet("placement", v)/aatpeBoostClear("placement"). The hermetic
// placement graph does NOT link the real gpu_boost.o (it would pull in the whole
// gpu/thermal dep tree and risk multiple-definitions with these stubs), so provide
// neutral no-op bridge entries here. The real bridge + max() aggregation is
// covered end-to-end by tests/v94/test_a2_boost.cpp.
namespace aatpe {
void aatpeBoostSet(const char* /*source*/, int /*level*/) {}
void aatpeBoostClear(const char* /*source*/) {}
}

// aatpeStrError / aatpeErrStr are now provided inline by aatped_logging.h (via
// aatped.h, included above) — no host stub needed; do not redefine it here.

// v4.3.6: host stubs required by the PSI wiring layer (aatped_psi.o) when linked
// into the placement-family recipes. The production daemon links the real
// cold_start_rules.o / aatped_utils.o; on host we provide minimal no-ops to keep
// the link graph closed without pulling cold_start_rules' game-list/log deps.
// coldStartSetIoPressure is WEAK: recipes that link the real cold_start_rules.o
// (e.g. `coldstart`) get the real definition; placement-family recipes (no
// cold_start_rules.o) fall back to this no-op.
namespace aatpe {
__attribute__((weak)) void coldStartSetIoPressure(bool) {}   // real in cold_start_rules.cpp
}
__attribute__((weak)) int readInt(const char* /*path*/, int def) { return def; }  // real in aatped_utils.cpp
// v4.4.1 P0: heal calls aatpe::aatpeThermalHalLevel() (real in thermal.cpp).
namespace aatpe { __attribute__((weak)) int aatpeThermalHalLevel() { return 0; } }

// C1/C3 exploration stubs (placement.cpp references g_thermalPlacementScale when
// thermalSmoothEnabled gate is compiled in; default-off so neutral defaults).
std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};

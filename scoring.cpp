#include <cmath>
#include <atomic>
// AATPE - Scoring Engine (fix45 Problem 5: split from aatped.cpp)
#include <vector>
#include <set>
#include <dirent.h>
#include <cstring>
#include <cstdlib>
// P3-3: Moved #include "aatped.h" to top of file (was at line 127, mid-file)
#include "aatped.h"
#include "adrc.h"  // ADRC risk score dimension
#include "scoring_interface.h"  // P0-opt1: PoolSnapshot / ScoringContext / context-aware overloads
// .cpp files may use using namespace (not included elsewhere)
using namespace aatpe;
using namespace aatpe_ml;
using namespace aatpe::adrc;

// Lock order — see aatped.h "GLOBAL LOCK ORDERING" block.
// This file's hot path (computeScore) uses snapshot-then-compute:
//   - g_topologyRwMutex is taken briefly by snapshotPools() to copy the three
//     CPU pools, then released before any std::find / scoring math.
//   - g_cpuMetricsMutex is taken/released per leaf scoring function
//     (capacityScore/thermalScore/contentionPenalty/extractMlFeatures); never
//     held across another lock.
//   - g_historyMutex is taken by placementSuccessScore() after the topology
//     snapshot has been released.
//   - g_cfg reads in frameScore() are best-effort (frameAware toggle); the
//     caller (decidePlacement) snapshots g_cfg at entry.


// Thread-safe per-CPU rolling sample buffer.
// Previously g_cpuLoadHistory was vector<vector<int>>: recordCpuLoadSample()
// pushed_back (which could realloc the inner heap vector) while predictCpuLoad()
// held a const-ref to the same inner vector — a data race / UAF when called
// from different threads. Now each buffer is a fixed-capacity ring protected by
// its own mutex; readers take a small stack snapshot under lock.
template <typename T, size_t Cap>
struct RingBuffer {
    // unique_ptr<mutex> makes RingBuffer movable (the vector needs MoveInsertable
    // for reserve/realloc); the mutex itself is heap-allocated and stable.
    std::unique_ptr<std::mutex> mtx = std::make_unique<std::mutex>();
    T data[Cap];
    size_t head = 0;   // next write index
    size_t count = 0;   // number of valid samples (0..Cap)

    void push(const T& v) {
        std::lock_guard<std::mutex> lk(*mtx);
        data[head] = v;
        head = (head + 1) % Cap;
        if (count < Cap) count++;
    }
    // Return samples in chronological order (oldest first) as a vector snapshot.
    std::vector<T> snapshot() const {
        std::lock_guard<std::mutex> lk(*mtx);
        std::vector<T> out;
        out.reserve(count);
        size_t start = (count < Cap) ? 0 : head;
        for (size_t i = 0; i < count; i++) {
            out.push_back(data[(start + i) % Cap]);
        }
        return out;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(*mtx);
        return count;
    }
    T back() const {
        std::lock_guard<std::mutex> lk(*mtx);
        if (count == 0) return T{};
        size_t last = (head == 0) ? Cap - 1 : head - 1;
        return data[last];
    }
};

static constexpr int kLoadHistoryCap = 10;
static constexpr int kErrorHistoryCap = 20;

static std::vector<RingBuffer<int, kLoadHistoryCap>> g_cpuLoadHistory;
static std::vector<RingBuffer<float, kErrorHistoryCap>> g_predictionErrors;
static std::vector<int> g_precomputedCpuBaseScore;
static std::atomic<bool> g_precomputedValid{false};

// Initialize all per-CPU arrays after CPU topology detection.
// Must be called after detectCpuTopology() fills g_allCpus.
void initScoringArrays(int cpuCount) {
    if (cpuCount <= 0) cpuCount = 8;  // safe default
    // RingBuffer contains a std::mutex, so it is neither copyable nor movable.
    // We must default-construct each element in place via emplace_back rather
    // than vector::assign() (which requires copy/move).
    g_cpuLoadHistory.clear();
    g_cpuLoadHistory.reserve(cpuCount);
    for (int i = 0; i < cpuCount; i++) g_cpuLoadHistory.emplace_back();
    g_predictionErrors.clear();
    g_predictionErrors.reserve(cpuCount);
    for (int i = 0; i < cpuCount; i++) g_predictionErrors.emplace_back();
    g_precomputedCpuBaseScore.assign(cpuCount, 0);
    g_precomputedValid.store(false);
}

static constexpr int kPredictionHorizon = 2;

// P3-2: Extracted common linear regression from predictCpuLoad / predictCpuLoadMultiStep.
// Returns true on success, false if insufficient data or singular matrix.
static bool linearRegression(const std::vector<int>& history, float& outA, float& outB) {
    int n = (int)history.size();
    if (n < 3) return false;
    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    for (int i = 0; i < n; i++) {
        sumX += (float)i;
        sumY += (float)history[i];
        sumXY += (float)i * (float)history[i];
        sumX2 += (float)i * (float)i;
    }
    float denom = (float)n * sumX2 - sumX * sumX;
    if (std::abs(denom) < 0.001f) return false;
    outA = ((float)n * sumXY - sumX * sumY) / denom;
    outB = (sumY - outA * sumX) / (float)n;
    return true;
}

void recordCpuLoadSample(int cpuId, int util) {
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return;
    g_cpuLoadHistory[cpuId].push(util);
}

float predictCpuLoad(int cpuId) {
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 50.0f;
    std::vector<int> history = g_cpuLoadHistory[cpuId].snapshot();
    if ((int)history.size() < 3) return history.empty() ? 50.0f : (float)history.back();
    float a = 0, b = 0;
    if (!linearRegression(history, a, b)) return (float)history.back();
    float predicted = a * (float)(history.size() + kPredictionHorizon - 1) + b;
    return ((predicted) < (0.0f) ? (0.0f) : ((predicted) > (100.0f) ? (100.0f) : (predicted)));
}

int predictedLoadPenalty(int cpuId) {
    float predicted=predictCpuLoad(cpuId);
    if (predicted>80.0f) return (int)((predicted-80.0f)*2.0f);
    // G7 R004-B2: previously returned (30-predicted) when predicted<30, i.e. a
    // POSITIVE value that baseScore subtracts — actively penalizing IDLE cores and
    // suppressing wake-up of the prime/big cluster (symptom: 超大核不发力).
    // Now idle/medium cores are neutral; only already-saturated cores (>80%
    // predicted load) are discouraged. The contention idle boost (+40) already
    // rewards picking an idle core, so no extra idle reward is needed here.
    return 0;
}

// fix84 Feature 3: Multi-step prediction with confidence calibration
// Predicts future 3 time points (50ms/100ms/150ms), takes max (conservative).
// Tracks prediction error (MAE) for confidence estimation.

// kMultiStepHorizon: use function parameter instead

// Multi-step prediction: returns max of predicted loads across horizon (conservative)
float predictCpuLoadMultiStep(int cpuId, int steps) {
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 50.0f;
    if (steps <= 0) steps = 1;
    if (steps > 5) steps = 5;
    std::vector<int> history = g_cpuLoadHistory[cpuId].snapshot();
    if ((int)history.size() < 3) return history.empty() ? 50.0f : (float)history.back();
    float a = 0, b = 0;
    if (!linearRegression(history, a, b)) return (float)history.back();
    // Take max prediction across all steps (conservative, avoids underestimating overload)
    float maxPred = 0.0f;
    int n = (int)history.size();
    for (int s = 1; s <= steps; s++) {
        float pred = a * (float)(n + s - 1) + b;
        pred = ((pred) < (0.0f) ? (0.0f) : ((pred) > (100.0f) ? (100.0f) : (pred)));
        if (pred > maxPred) maxPred = pred;
    }
    return maxPred;
}

// Record prediction error for confidence calibration
void recordPredictionError(int cpuId, float predicted, float actual) {
    if (cpuId < 0 || cpuId >= (int)g_predictionErrors.size()) return;
    // P2: reject NaN/inf inputs — a single NaN in the error history poisons
    // the MAE sum and makes confidence NaN, which then propagates into scores.
    if (std::isnan(predicted) || std::isnan(actual) ||
        std::isinf(predicted) || std::isinf(actual)) return;
    float error = std::abs(predicted - actual);
    if (std::isnan(error)) return;
    g_predictionErrors[cpuId].push(error);
}

// Calculate prediction confidence (0-1) based on MAE
// confidence = max(0, 1 - MAE/50)
float getPredictionConfidence(int cpuId) {
    if (cpuId < 0 || cpuId >= (int)g_predictionErrors.size()) return 0.0f;
    std::vector<float> errors = g_predictionErrors[cpuId].snapshot();
    if (errors.empty()) return 0.5f;  // Default neutral confidence
    float sum = 0;
    for (float e : errors) sum += e;
    float mae = sum / (float)errors.size();
    float confidence = 1.0f - mae / 50.0f;
    return ((confidence) < (0.0f) ? (0.0f) : ((confidence) > (1.0f) ? (1.0f) : (confidence)));
}

// Get average MAE across all CPUs
float getAveragePredictionMAE() {
    float totalMAE = 0;
    int count = 0;
    for (int i = 0; i < (int)g_predictionErrors.size(); i++) {
        std::vector<float> errors = g_predictionErrors[i].snapshot();
        if (!errors.empty()) {
            float sum = 0;
            for (float e : errors) sum += e;
            totalMAE += sum / (float)errors.size();
            count++;
        }
    }
    return count > 0 ? totalMAE / count : 0.0f;
}
// Contains: capacity/latency/cache/thermal/power/frame/pool scores,
// migration/contention penalties, placement success tracking, ML feature helpers,
// and computeScore (the fused scoring function).


int capacityScore(int cpuId, ThreadRole role) noexcept {
    std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
    auto it = g_cpuMetrics.find(cpuId);
    if (it == g_cpuMetrics.end()) return 0;
    int cap = it->second.capacity;
    switch (role) {
        case ROLE_S0_SYSTEM: return 0;
        case ROLE_S5_BACKGROUND: return capacity::kBackgroundInverseRef - cap;
        default: return cap;
    }
}

int latencyScore(int cpuId, ThreadRole role) noexcept {
    (void)cpuId;
    switch (role) {
        case ROLE_S0_SYSTEM: return latency::kScoreSystem;
        case ROLE_S1_UI: return latency::kScoreUi;
        case ROLE_S2_RENDER: return latency::kScoreRender;
        case ROLE_S3_GAME: return latency::kScoreGame;
        case ROLE_S4_THROUGHPUT: return latency::kScoreThroughput;
        case ROLE_S5_BACKGROUND: return latency::kScoreBackground;
        default: return 0;
    }
}

// P0-1: use sched_getaffinity() instead of reading /proc/<pid>/task/<tid>/affinity file
// P1-1: uses cpu_set_t directly (CPU_ISSET bit ops) instead of set<int>.
// P3: cache the per-tid mask/setCount in thread_local — cacheScore is called
// once per (tid, cpuId) candidate in the decidePlacement loop, but the mask
// only depends on tid. Re-fetching and counting 1024 bits for every cpuId was
// O(n_cpus * CPU_SETSIZE) per decision cycle.
int cacheScore(int cpuId, int pid, int tid) {
    (void)pid;
    // thread_local cache: last tid we computed mask for
    static thread_local int tlCacheTid = -1;
    static thread_local cpu_set_t tlCacheMask;
    static thread_local int tlCacheSetCount = 0;

    if (tid != tlCacheTid) {
        tlCacheTid = tid;
        tlCacheMask = getThreadAffinityCpus(tid);
        tlCacheSetCount = 0;
        for (int i = 0; i < CPU_SETSIZE; i++) {
            if (CPU_ISSET(i, &tlCacheMask)) tlCacheSetCount++;
        }
    }
    int setCount = tlCacheSetCount;
    if (setCount == 0) return 0;
    // Snapshot g_cpuCount under the topology lock to avoid a racy read.
    int cpuCount;
    {
        std::lock_guard<std::mutex> lock(g_topologyRwMutex);
        cpuCount = g_cpuCount;
    }
    if (CPU_ISSET(cpuId, &tlCacheMask) || setCount >= cpuCount) return cache_score::kAffinityHit;
    return 0;
}

int thermalScore(int cpuId) noexcept {
    std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
    auto it = g_cpuMetrics.find(cpuId);
    if (it == g_cpuMetrics.end()) return 0;
    int temp = it->second.temp;
    // P2-13: temp==0 means uninitialized (thermal zone not read yet).
    // Return 0 to avoid false low-temp boost from kLowTempThreshold logic.
    if (temp == 0) return 0;
    int penalty = 0, boost = 0;
    if (temp > thermal::kHighTempThreshold) penalty = (temp - thermal::kHighTempThreshold) * thermal::kPenaltyPerDegree;
    if (temp < thermal::kLowTempThreshold) boost = (thermal::kLowTempThreshold - temp) * thermal::kBoostPerDegree;
    return boost - penalty;
}

int powerScore(int cpuId, ThreadRole role) noexcept {
    (void)cpuId;
    // P3: g_currentProfileEnum is NOT std::atomic (its definition lives in
    // aatped_main.cpp, outside this fix's scope). It is written ONLY by the
    // config/profile-reload path on the main loop thread (single writer); the
    // reads below are on the scoring hot path. ProfileType is a scoped enum with
    // int underlying (4-byte, naturally aligned), so a read/write is atomic on
    // all supported platforms; the worst case is one cycle of stale profile,
    // which is benign for power scoring.
    if (g_currentProfileEnum == ProfileType::BATTERY) {
        if (role == ROLE_S5_BACKGROUND) return power::kBatteryBgScore;
        if (role == ROLE_S4_THROUGHPUT) return power::kBatteryThroughputScore;
        return power::kBatteryOtherScore;
    } else if (g_currentProfileEnum == ProfileType::GAMING) {
        if (role == ROLE_S3_GAME || role == ROLE_S2_RENDER || role == ROLE_S1_UI) return power::kgamingInteractiveScore;
        if (role == ROLE_S5_BACKGROUND) return power::kgamingBgScore;
        return power::kgamingOtherScore;
    } else if (g_currentProfileEnum == ProfileType::THERMAL) {
        if (role == ROLE_S5_BACKGROUND) return power::kThermalBgScore;
        return power::kThermalOtherScore;
    }
    return 0;
}

int frameScore(int cpuId, ThreadRole role) {
    (void)cpuId;
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config scoreCfg;
    snapshotConfig(scoreCfg);
    if (!scoreCfg.frameAware || !g_frameCritical) return 0;
    if (role == ROLE_S1_UI || role == ROLE_S2_RENDER || role == ROLE_S3_GAME) return frame_score::kCriticalBoost;
    return 0;
}

// P0-opt1: PoolSnapshot is now defined in scoring_interface.h (exported).
// Helper to snapshot pools under g_topologyRwMutex (used by buildScoringContext
// and by legacy callers that still need a standalone snapshot).
static inline PoolSnapshot snapshotPools() {
    PoolSnapshot s;
    std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
    s.little = g_littlePool;
    s.mid    = g_midPool;
    s.big    = g_bigPool;
    // G7 R004-B2: snapshot the PRIME (highest) cluster under the already-held
    // topology lock. primePool() would re-lock g_topologyRwMutex (non-recursive)
    // and deadlock, so read g_clusters directly. Empty on 2-cluster devices.
    for (const auto& cl : g_clusters) {
        if (cl.tier == CpuTier::PRIME) { s.prime = cl.cpus; break; }
    }
    return s;
}

// ============================================================================
// P0-opt1: ScoringContext implementation — snapshot all hot-path globals once.
// ============================================================================
int ScoringContext::getCapacity(int cpuId) const {
    auto it = cpuMetrics.find(cpuId);
    return it != cpuMetrics.end() ? it->second.capacity : 0;
}
int ScoringContext::getTemp(int cpuId) const {
    auto it = cpuMetrics.find(cpuId);
    return it != cpuMetrics.end() ? it->second.temp : 0;
}
int ScoringContext::getUtil(int cpuId) const {
    auto it = cpuMetrics.find(cpuId);
    return it != cpuMetrics.end() ? it->second.util : 0;
}

ScoringContext buildScoringContext(const Config& cfg) {
    ScoringContext ctx;
    ctx.cfg = &cfg;
    // G7 R004-B2: activate the (previously dead) capacity normalization
    // calibration once, on first scoring decision, so nCap uses measured
    // min/max cpu_capacity instead of a hardcoded 0..1024.
    calibrateCapacityNormRange();
    // Snapshot pools once under topology lock
    ctx.pools = snapshotPools();
    // Snapshot all CPU metrics once under metrics lock
    {
        std::lock_guard<std::mutex> mlock(g_cpuMetricsMutex);
        ctx.cpuMetrics = g_cpuMetrics;
    }
    return ctx;
}

int poolScore(int cpuId, ThreadRole role) {
    // snapshot pools under topology lock, release before scoring.
    PoolSnapshot pools = snapshotPools();
    bool inPrime = std::find(pools.prime.begin(), pools.prime.end(), cpuId) != pools.prime.end();
    bool inLittle = std::find(pools.little.begin(), pools.little.end(), cpuId) != pools.little.end();
    bool inMid = std::find(pools.mid.begin(), pools.mid.end(), cpuId) != pools.mid.end();
    bool inBig = std::find(pools.big.begin(), pools.big.end(), cpuId) != pools.big.end();
    switch (role) {
        case ROLE_S0_SYSTEM: return 0;
        case ROLE_S1_UI:
            if (inPrime) return pool_score::kUiPrime;
            if (inBig) return pool_score::kUiBig;
            if (inMid) return pool_score::kUiMid;
            if (inLittle) return pool_score::kUiLittle;
            break;
        case ROLE_S2_RENDER:
            if (inPrime) return pool_score::kRenderPrime;
            if (inBig) return pool_score::kRenderBig;
            if (inMid) return pool_score::kRenderMid;
            if (inLittle) return pool_score::kRenderLittle;
            break;
        case ROLE_S3_GAME:
            if (inPrime) return pool_score::kGamePrime;
            if (inBig) return pool_score::kGameBig;
            if (inMid) return pool_score::kGameMid;
            if (inLittle) return pool_score::kGameLittle;
            break;
        case ROLE_S4_THROUGHPUT:
            if (inPrime) return pool_score::kThroughputPrime;
            if (inBig) return pool_score::kThroughputBig;
            if (inMid) return pool_score::kThroughputMid;
            if (inLittle) return pool_score::kThroughputLittle;
            break;
        case ROLE_S5_BACKGROUND:
            if (inLittle) return pool_score::kBackgroundLittle;
            if (inMid) return pool_score::kBackgroundMid;
            if (inBig) return pool_score::kBackgroundBig;
            if (inPrime) return pool_score::kBackgroundPrime;
            break;
    }
    return 0;
}

// P0-1: use sched_getaffinity() + cached readThreadCurrentCpu (no affinity file reads)
// P2-7: Cache currentCpu and affinity per tid to avoid repeated I/O in hot path.
//   decidePlacement calls computeScore -> migrationPenalty for every (thread, cpu) pair,
//   but currentCpu/affinity depend only on tid, not cpuId. thread_local cache is safe
//   because the daemon loop is single-threaded.
// P2-12: Full-affinity threads (affinity covers all CPUs) still incur kCrossPoolPenalty
//   when migrating across pools. Previously returned 0 immediately, bypassing cross-pool
//   cost. Same-pool migration remains free (pool-covered check below).
// P2 Problem6: cache state hoisted to file scope so migrationPenalty() and
//   migrationPenaltyWithFrom() share the SAME affinity cache. The latter lets
//   decidePlacement inject a freshly-read fromCpu (liveCpu) while still reusing
//   the cached affinity mask — affinity changes far slower than currentCpu.
static constexpr int kMigPenaltyCacheTtlSec = 30;
static thread_local int tlMigCacheTid = -1;
static thread_local int tlMigCacheCurrentCpu = -1;
static thread_local cpu_set_t tlMigCacheAffMask;  // P1-1: cpu_set_t instead of set<int>
static thread_local time_t tlMigCacheTimestamp = 0;
// G5-007: the legacy migrationPenalty() and the caller-provided
// migrationPenaltyWithFrom() share this thread_local cache. WithFrom deliberately
// does NOT know the thread's true currentCpu (it uses knownFromCpu), so it must
// mark the cached currentCpu invalid; otherwise a later legacy call would reuse a
// stale currentCpu behind a freshly-refreshed (tid, timestamp) pair.
static thread_local bool tlMigCacheHasCur = false;

// P2 Problem6: pure pool-decision core given a known fromCpu + affinity mask.
// Split out so callers holding a FRESH fromCpu (decidePlacement liveCpu) do not
// trust the 30s-TTL cached currentCpu.
// #32: runtime migration-penalty multipliers (real-schedstat calibrated).
// little<->big cross-pool hops cost ~3x a same-pool hop (cache cold on the big L2
// + full runqueue shift on Dimensity 1100). Read from g_cfg with a 1s TTL so the
// hot scoring loop does not snapshotConfig() (and take g_cfgMutex) per candidate.
static void getMigrationPenaltyFactors(float& outBigLittle, float& outSame) {
    static std::atomic<float> s_bigLittle{3.0f};
    static std::atomic<float> s_same{1.0f};
    static std::atomic<long long> s_cacheMs{0};
    // Fast path: hand back the cached factors without any clock read. The hot
    // scoring loop calls this once per (thread,cpu) candidate (~40x per
    // decision); reading clock_gettime every call was pure overhead. Gate the
    // TTL check behind a thread-local call counter so the clock is read at most
    // every 32 calls (~once per decision) while config still refreshes at most
    // once per second.
    outBigLittle = s_bigLittle.load(std::memory_order_relaxed);
    outSame      = s_same.load(std::memory_order_relaxed);
    thread_local int tlCheckCounter = 0;
    if (++tlCheckCounter < 32) return;
    tlCheckCounter = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long cached = s_cacheMs.load(std::memory_order_relaxed);
    if (cached == 0 || (nowMs - cached) > 1000) {
        Config cfg;
        snapshotConfig(cfg);
        s_bigLittle.store(cfg.migrationPenaltyBigLittle, std::memory_order_relaxed);
        s_same.store(cfg.migrationPenaltySame, std::memory_order_relaxed);
        s_cacheMs.store(nowMs, std::memory_order_relaxed);
        outBigLittle = s_bigLittle.load(std::memory_order_relaxed);
        outSame      = s_same.load(std::memory_order_relaxed);
    }
}

// P0-opt1: Pool-aware migration penalty core — caller provides a pre-snapshotted
// PoolSnapshot instead of re-taking g_topologyRwMutex.
// P1-1: affinity mask is cpu_set_t — pool-covered check uses CPU_ISSET (O(1) bit op).
int migrationPenaltyCoreWithPools(int cpuId, int fromCpu, const cpu_set_t& affMask, const PoolSnapshot& pools) {
    if (fromCpu == cpuId) return 0;
    bool fromLittle = std::find(pools.little.begin(), pools.little.end(), fromCpu) != pools.little.end();
    bool fromBig    = std::find(pools.big.begin(),    pools.big.end(),    fromCpu) != pools.big.end();
    bool fromMid    = std::find(pools.mid.begin(),    pools.mid.end(),    fromCpu) != pools.mid.end();
    bool toLittle = std::find(pools.little.begin(), pools.little.end(), cpuId) != pools.little.end();
    bool toBig    = std::find(pools.big.begin(),    pools.big.end(),    cpuId) != pools.big.end();
    bool toMid    = std::find(pools.mid.begin(),    pools.mid.end(),    cpuId) != pools.mid.end();
    bool fromUnknown = (!fromLittle && !fromBig && !fromMid);
    bool toUnknown   = (!toLittle && !toBig && !toMid);
    float multBigLittle = 1.0f, multSame = 1.0f;
    getMigrationPenaltyFactors(multBigLittle, multSame);
    if (fromUnknown || toUnknown) {
        return (int)(migration::kCrossPoolPenalty * multBigLittle);
    }
    if (fromLittle == toLittle && fromBig == toBig && fromMid == toMid) {
        const std::vector<int>* pool = fromLittle ? &pools.little : (fromBig ? &pools.big : &pools.mid);
        bool poolCovered = true;
        for (int cpu : *pool) {
            if (!CPU_ISSET(cpu, &affMask)) { poolCovered = false; break; }
        }
        if (poolCovered) return 0;
        return (int)(migration::kSamePoolPenalty * multSame);
    }
    bool isLittleBigJump = (fromLittle && toBig) || (fromBig && toLittle);
    if (isLittleBigJump) {
        return (int)(migration::kCrossPoolPenalty * multBigLittle);
    }
    return (int)(migration::kCrossPoolPenalty * 1.5f);
}

static int migrationPenaltyCore(int cpuId, int fromCpu, const cpu_set_t& affMask) {
    PoolSnapshot pools = snapshotPools();
    return migrationPenaltyCoreWithPools(cpuId, fromCpu, affMask, pools);
}

int migrationPenalty(int cpuId, int pid, int tid) {
    // P1-1: uses cpu_set_t (no heap set<int>).
    int currentCpu;
    cpu_set_t affMask;
    CPU_ZERO(&affMask);  // P2: defensive zero before any by-value assignment
    time_t now = time(nullptr);
    bool cacheValid = (tlMigCacheTid == tid) &&
                      (tlMigCacheTimestamp != 0) &&
                      tlMigCacheHasCur &&
                      (now - tlMigCacheTimestamp < kMigPenaltyCacheTtlSec);
    if (cacheValid) {
        currentCpu = tlMigCacheCurrentCpu;
        affMask = tlMigCacheAffMask;
    } else {
        currentCpu = readThreadCurrentCpu(pid, tid);
        affMask = getThreadAffinityCpus(tid);
        if (currentCpu < 0) {
            // Fallback: find first set CPU in mask
            for (int i = 0; i < CPU_SETSIZE; i++) {
                if (CPU_ISSET(i, &affMask)) { currentCpu = i; break; }
            }
            if (currentCpu < 0) currentCpu = 0;
        }
        tlMigCacheTid = tid;
        tlMigCacheCurrentCpu = currentCpu;
        tlMigCacheAffMask = affMask;
        tlMigCacheTimestamp = now;
        tlMigCacheHasCur = true;
    }
    return migrationPenaltyCore(cpuId, currentCpu, affMask);
}

// P2 Problem6: migration cost with a CALLER-PROVIDED fresh fromCpu.
int migrationPenaltyWithFrom(int cpuId, int pid, int tid, int knownFromCpu) {
    (void)pid;
    cpu_set_t affMask;
    time_t now = time(nullptr);
    bool cacheValid = (tlMigCacheTid == tid) &&
                      (tlMigCacheTimestamp != 0) &&
                      (now - tlMigCacheTimestamp < kMigPenaltyCacheTtlSec);
    if (cacheValid) {
        affMask = tlMigCacheAffMask;
    } else {
        affMask = getThreadAffinityCpus(tid);
        tlMigCacheTid = tid;
        tlMigCacheAffMask = affMask;
        tlMigCacheTimestamp = now;
        // G5-007: knownFromCpu is authoritative for this decision; we do not know
        // the thread's true currentCpu. Mark the cached currentCpu invalid so a
        // later legacy migrationPenalty() re-reads it instead of reusing a stale
        // value behind the freshly-refreshed tid/timestamp.
        tlMigCacheHasCur = false;
    }
    return migrationPenaltyCore(cpuId, knownFromCpu, affMask);
}


int contentionPenalty(int cpuId) noexcept {
    std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
    auto it = g_cpuMetrics.find(cpuId);
    if (it == g_cpuMetrics.end()) return 0;
    int util = it->second.util;
    if (util > contention::kHighUtilThreshold) return contention::kHighUtilPenalty;
    if (util > contention::kMidUtilThreshold) return contention::kMidUtilPenalty;
    if (util > contention::kLowUtilThreshold) return contention::kLowUtilPenalty;
    return contention::kIdleBoost;
}

// ============================================================================
// PLACEMENT SUCCESS TRACKING (fix20 P2-7: renamed from "learning system")
//
// NOTE: This is NOT a performance-learning system. recordOutcome() records
// whether a placement (sched_setaffinity) was SUCCESSFULLY APPLIED
// (currentCpu == targetCpu after migration), NOT whether performance
// improved. The history score therefore reflects "placement_success_rate",
// i.e. how often a given CPU was successfully assigned to a thread.
// This is useful for avoiding repeated failed migrations, but does NOT
// learn optimal CPU placement based on runtime performance metrics.
int placementSuccessScore(int cpuId, int tid) {
// ============================================================================
    // Lock-discipline check. Verified across the tree: g_history is
    // touched ONLY in this file, and every access holds g_historyMutex
    // (placementSuccessScore here; recordPlacementResult's update + read blocks;
    // decayHistory's full-map iteration). No WebUI/observability thread touches
    // g_history directly. These functions all run on the main daemon thread, so
    // they serialize trivially; the mutex remains the single correctness anchor.
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config learnCfg;
    snapshotConfig(learnCfg);
    if (!learnCfg.learningEnabled) return 0;
    std::lock_guard<std::mutex> lock(g_historyMutex);
    auto it = g_history.find(tid);
    if (it == g_history.end()) return 0;
    if (it->second.samples < learnCfg.minSamplesForLearning) return 0;
    if (it->second.preferredCpu == cpuId) return it->second.score;
    return 0;
}
// Problem 9: deprecated alias for backward compatibility
int historyScore(int cpuId, int tid) { return placementSuccessScore(cpuId, tid); }

// P1-4: File-scope throttle + migration-count state (declared here so
// recordPlacementResult can use them; cleanupThreadScoringState erases on exit).
static std::unordered_map<int, time_t> s_lastWrite;
static std::mutex s_lastWriteMutex;
static std::unordered_map<int, unsigned long long> g_threadMigrationCount;
// P2-9: migration count mutex. Declared up front so cleanupThreadScoringState
// (defined below) can take it; kept separate from s_lastWriteMutex so the two
// maps are erased under their OWN locks without nesting.
static std::mutex g_migCountMutex;

// ML-10: per-tid history files are named by tid, and the kernel recycles tids
// after thread/process exit. A previous daemon instance could therefore leave
// RUN_DIR/history/<tid> on disk; when a NEW daemon writes the same tid, the
// stale placement_success_rate would otherwise look like this thread's history.
// Every history file now carries THIS daemon's boot timestamp (g_startTime,
// set once at aatped_main startup); startup sweeps and drops files whose stamp
// is missing (legacy) or foreign (previous boot).
static time_t scoringBootTs() {
    time_t t = g_startTime.load(std::memory_order_relaxed);
    return t != 0 ? t : time(nullptr);  // pre-boot fallback before g_startTime is set
}

void recordPlacementResult(int tid, int cpuId, bool good) {
    // P2-7: This records placement_success (was affinity applied?),
    // NOT performance improvement. good = (currentCpu == targetCpu).
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config recCfg;
    snapshotConfig(recCfg);
    if (!recCfg.learningEnabled) return;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        auto& h = g_history[tid];
        int weight = recCfg.historyWeight;
        // P2: clamp to [0,100] so a bad config doesn't make (100-weight) negative.
        if (weight < 0) weight = 0;
        if (weight > 100) weight = 100;
        if (good) {
            h.score = (h.score * (100 - weight) + 100 * weight) / 100;
        } else {
            h.score = (h.score * (100 - weight)) / 100;
        }
        h.samples++;
        h.preferredCpu = cpuId;
        h.lastUpdate = time(nullptr);
    }
    // P2-8: Throttle file writes to at most once per second per tid.
    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> wlock(s_lastWriteMutex);
        auto it = s_lastWrite.find(tid);
        if (it != s_lastWrite.end() && (now - it->second) < 1) return;
        s_lastWrite[tid] = now;
    }
    // Read h under lock for file content (in-memory state may have changed)
    int preferredCpu = 0, score = 0, samples = 0;
    time_t lastUpdate = 0;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        auto it = g_history.find(tid);
        if (it == g_history.end()) return;
        preferredCpu = it->second.preferredCpu;
        score = it->second.score;
        samples = it->second.samples;
        lastUpdate = it->second.lastUpdate;
    }
    std::string histDir = std::string(RUN_DIR) + "/history";
    mkdirIfNotExist(histDir.c_str(), 0700); // history holds per-tid placement info
    std::string histPath = histDir + "/" + std::to_string(tid);
    std::string content = "preferred_cpu=" + std::to_string(preferredCpu) + "\n"
                         + "placement_success_rate=" + std::to_string(score) + "\n"
                         + "samples=" + std::to_string(samples) + "\n"
                         + "last_update=" + std::to_string(lastUpdate) + "\n"
                         + "daemon_boot_ts=" + std::to_string((long long)scoringBootTs()) + "\n"
                         + "note=score_is_placement_success_not_performance\n";
    writeFile(histPath.c_str(), content);
}

// ML-10: drop stale per-tid history files left by a PREVIOUS daemon boot
// (or legacy files from before the stamp existed). Called once at startup right
// after the history dir is created; the tid namespace is shared across daemon
// restarts, so without this a recycled tid inherits a foreign success-rate.
// Only numeric filenames (tids) are touched; every other file is left alone.
void scoringHistorySweepStale() {
    std::string histDir = std::string(RUN_DIR) + "/history";
    DIR* d = opendir(histDir.c_str());
    if (!d) return;  // no history dir yet -> nothing to sweep
    time_t boot = scoringBootTs();
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;  // "." / ".." / hidden
        bool numeric = true;
        for (size_t i = 0; n[i] != '\0'; i++) {
            if (n[i] < '0' || n[i] > '9') { numeric = false; break; }
        }
        if (!numeric) continue;
        std::string p = histDir + "/" + n;
        FILE* f = fopen(p.c_str(), "r");
        if (!f) continue;
        long long fileBoot = -1;
        char line[256];
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (std::strncmp(line, "daemon_boot_ts=", 15) == 0) {
                fileBoot = std::strtoll(line + 15, nullptr, 10);
                break;
            }
        }
        fclose(f);
        // Missing stamp (legacy file) or a different boot -> stale tid -> drop.
        if (fileBoot != (long long)boot) unlink(p.c_str());
    }
    closedir(d);
}

// P1-4: Call when a thread exits to clean up per-tid maps that would otherwise
// grow unbounded (s_lastWrite, g_threadMigrationCount). Called from onExit().
// P0-4: erase each map under its OWN mutex — never hold s_lastWriteMutex while
// touching g_threadMigrationCount (and vice versa), so the two leaf locks are
// never nested.
void cleanupThreadScoringState(int tid) {
    {
        std::lock_guard<std::mutex> w(s_lastWriteMutex);
        s_lastWrite.erase(tid);
    }
    std::lock_guard<std::mutex> m(g_migCountMutex);
    g_threadMigrationCount.erase(tid);
}

// Problem 9: deprecated alias for backward compatibility
void recordOutcome(int tid, int cpuId, bool good) { recordPlacementResult(tid, cpuId, good); }

// P2-9: migration count mutex (g_migCountMutex declared above, near the map).

void decayHistory() {
    // P1-1: snapshot g_cfg to avoid torn read racing config hot-reload.
    Config decayCfg;
    snapshotConfig(decayCfg);
    if (!decayCfg.learningEnabled) return;
    // P2: clamp learningDecay to [0,100] so a bad config doesn't invert or
    // overflow the history score decay.
    int decay = decayCfg.learningDecay;
    if (decay < 0) decay = 0;
    if (decay > 100) decay = 100;
    std::lock_guard<std::mutex> lock(g_historyMutex);
    time_t now = time(nullptr);
    for (auto it = g_history.begin(); it != g_history.end(); ) {
        it->second.score = it->second.score * decay / 100;
        if (it->second.score < daemon::kHistoryScoreMinThreshold) it->second.score = 0;
        // fix: clean entries older than 1 hour
        if (now - it->second.lastUpdate > daemon::kHistoryEntryMaxAgeSec) {
            std::string histPath = std::string(RUN_DIR) + "/history/" + std::to_string(it->first);
            unlink(histPath.c_str());
            it = g_history.erase(it);
        } else {
            ++it;
        }
    }
    // P2-9: Clean g_threadMigrationCount entries for threads no longer in g_threads.
    // P1-1: never hold g_migCountMutex and g_threadsMutex simultaneously. The
    // canonical order is g_threadsMutex -> ... -> g_migCountMutex, so we first
    // snapshot the live tid set under g_threadsMutex and RELEASE it, then erase
    // under g_migCountMutex. The two locks are never nested.
    {
        std::set<int> live;
        {
            std::lock_guard<std::mutex> threadLock(g_threadsMutex);
            for (const auto& kv : g_threads) live.insert(kv.first);
        }
        std::lock_guard<std::mutex> migLock(g_migCountMutex);
        for (auto it = g_threadMigrationCount.begin(); it != g_threadMigrationCount.end(); ) {
            if (live.count(it->first) == 0) {
                it = g_threadMigrationCount.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ============================================================================
// ML Online Learning - Helper Functions
// ============================================================================

void recordMigrationCount(int tid) {
    std::lock_guard<std::mutex> lock(g_migCountMutex);
    g_threadMigrationCount[tid]++;
}

unsigned long long getMigrationCount(int tid) {
    std::lock_guard<std::mutex> lock(g_migCountMutex);
    auto it = g_threadMigrationCount.find(tid);
    return (it != g_threadMigrationCount.end()) ? it->second : 0;
}

// Extract ML features from ThreadInfo for a given target CPU
// P1-8: Use candidate (target) CPU's real-time utilization, not current CPU.
// P1-3: Read nice from /proc/<tid>/stat field 19 (was hardcoded 0).
//   ThreadInfo cannot be extended (defined in aatped.h, Group1 file), so nice is
//   read on-demand here via readThreadNice() and passed directly to extractFeatures.
static int readThreadNice(int pid, int tid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return 0;
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
    // stat format: pid (comm) state ppid ... field 19 = nice
    // Find closing ')' of comm, then split remaining by spaces.
    // After ')', index 0=state, ..., index 16=nice (field 19 in 1-based).
    size_t rp = content.rfind(')');
    if (rp == std::string::npos) return 0;
    std::string rest = content.substr(rp + 1);
    std::istringstream iss(rest);
    std::string field;
    int idx = 0;
    while (iss >> field) {
        if (idx == 16) {
            try { return std::stoi(field); } catch (...) { return 0; }
        }
        idx++;
    }
    return 0;
}

// Game-scene-aware is_game feature.
// detectGameScene() writes /dev/aatpe/state/current_game_scene with values
// "combat"/"loading"/"menu"/"unknown". Previously is_game only checked
// ROLE_S3_GAME thread role, but game main threads are usually ROLE_S1_UI,
// so is_game=0 and w6 weight was always 0. We bridge detectGameScene → ML
// features here with a 2s cache to avoid frequent IO.
// Only UI/Render threads of the foreground app are marked; background threads
// of a game process are not. Frame-rate is intentionally NOT used (avoids
// misidentifying video/animation apps).
// P1-8: atomic to eliminate the RMW data race on the TTL check+set below.
static std::atomic<unsigned long long> g_lastSceneCheckMs{0};
static bool g_cachedIsGameScene = false;
static int g_cachedForegroundPid = -1;
static std::string g_cachedForegroundPackage;
// P1-3: these statics are written by refreshGameSceneCache() (decision loop)
// and read by extractMlFeatures*(). g_cachedForegroundPackage is a std::string —
// a concurrent read during realloc is a SIGSEGV. Guard all fields with one leaf
// mutex; readers take a cheap snapshot under it. The TTL fast-path on
// g_lastSceneCheckMs stays lock-free (benign 8-byte read; worst case one extra
// refresh).
static std::mutex g_sceneCacheMutex;

static void readSceneCache(bool& isGameScene, int& fgPid, std::string& fgPkg) {
    std::lock_guard<std::mutex> lk(g_sceneCacheMutex);
    isGameScene = g_cachedIsGameScene;
    fgPid = g_cachedForegroundPid;
    fgPkg = g_cachedForegroundPackage;
}

static void refreshGameSceneCache() {
    unsigned long long nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    unsigned long long lastMs = g_lastSceneCheckMs.load(std::memory_order_relaxed);
    if (lastMs > 0 && (nowMs - lastMs) < 2000) return;
    g_lastSceneCheckMs.store(nowMs, std::memory_order_relaxed);
    std::string scene = readFile("/dev/aatpe/state/current_game_scene");
    // trim trailing whitespace/newline
    while (!scene.empty() && (scene.back() == '\n' || scene.back() == '\r' || scene.back() == ' '))
        scene.pop_back();
    bool isGameScene = (scene == "combat" || scene == "loading" || scene == "menu");
    // Always cache foreground package + PID for Plan C (manual game list).
    // P0-5: read g_foregroundApp through the locked getter. Resolve the PID
    // (which scans /proc) BEFORE taking the lock so readers are never blocked.
    std::string pkg = aatpeGetForegroundPackage();
    int pid = -1;
    if (!pkg.empty()) pid = findPidByPackage(pkg);
    std::lock_guard<std::mutex> lk(g_sceneCacheMutex);
    g_cachedIsGameScene = isGameScene;
    g_cachedForegroundPackage = pkg;
    g_cachedForegroundPid = pid;
}

ThreadFeatures extractMlFeatures(const ThreadInfo& ti, int targetCpu) {
    // P2-4 fix: documented lock order — if we ever nest g_cpuMetricsMutex and
    // g_migCountMutex, the canonical order is g_cpuMetricsMutex -> g_migCountMutex.
    // Do NOT acquire g_migCountMutex then g_cpuMetricsMutex anywhere (ABBA deadlock).
    //
    // G5-006: only hold g_cpuMetricsMutex long enough to snapshot the per-CPU
    // metrics. Previously this lock was held across refreshGameSceneCache() (file
    // IO + possible /proc scan) and getMigrationCount(), blocking every other
    // candidate score on the hot path. Snapshot -> release -> do IO outside.
    int cpuUsage = 0;
    int thermal = 0;
    int capacity = topology::kDefaultCapacity;
    {
        std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
        auto mit = g_cpuMetrics.find(targetCpu);
        if (mit != g_cpuMetrics.end()) {
            cpuUsage = mit->second.util;
            thermal = mit->second.temp;
            capacity = mit->second.capacity;
        }
    }
    // P2-15: Use floating-point division to avoid double integer truncation.
    // Original: ti.runTime / ti.nrSwitches / 1000 (truncates twice).
    unsigned long long avgRunUs = 0;
    if (ti.nrSwitches > 0) {
        double avgRunUsD = (double)ti.runTime / (double)ti.nrSwitches / 1000.0;
        avgRunUs = (unsigned long long)avgRunUsD;
    }
    bool isGame = (ti.role == ROLE_S3_GAME);
    if (!isGame && ti.subtype.find("game") != std::string::npos) isGame = true;
    // Bridge detectGameScene → is_game feature.
    // If foreground app is in a game scene (combat/loading/menu), mark its
    // UI and Render threads as game threads. Background threads are excluded.
    // P1-3: snapshot the cache under the mutex into locals before comparing.
    bool sceneIsGame = false; int sceneFgPid = -1; std::string sceneFgPkg;
    if (!isGame) {
        refreshGameSceneCache();
        readSceneCache(sceneIsGame, sceneFgPid, sceneFgPkg);
        if (sceneIsGame && sceneFgPid > 0 && ti.pid == sceneFgPid &&
            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER)) {
            isGame = true;
        }
    }
    // Plan C — user-manual game package list.
    // If foreground app's package is in GAME_PACKAGES, mark its UI/Render
    // threads as game threads. Works when Plan A (detectGameScene) is unavailable.
    if (!isGame) {
        refreshGameSceneCache();
        readSceneCache(sceneIsGame, sceneFgPid, sceneFgPkg);
        if (!sceneFgPkg.empty() &&
            isPackageInGameList(sceneFgPkg) &&
            sceneFgPid > 0 && ti.pid == sceneFgPid &&
            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER)) {
            isGame = true;
        }
    }
    bool curMatch = (ti.currentCpu == targetCpu);
    unsigned long long migCount = getMigrationCount(ti.tid);
    // P0-opt4: use cached ti.nice (populated by thread_scan.cpp) instead of
    // re-opening /proc/<tid>/stat per candidate. Fall back to readThreadNice()
    // only for eBPF-created placeholders that haven't been scanned yet.
    int nice = ti.nice;
    if (nice == 0 && ti.needsHydration) {
        nice = readThreadNice(ti.pid, ti.tid);
    }
    // ROUND-010 D4-04: pass switches RATE (switchesPerSec) not cumulative nrSwitches,
    // matching extractFeatures' rate normalization so long-lived threads do not saturate.
    unsigned long long swRate = (unsigned long long)ti.switchesPerSec;
    return LearningModel::extractFeatures(
        avgRunUs, swRate, cpuUsage, ti.priority, nice,
        isGame, thermal, capacity, curMatch, migCount);
}

// P0-opt1: Context-aware ML feature extraction — reads cpuUsage/thermal/capacity
// from pre-snapshotted ctx.cpuMetrics instead of taking g_cpuMetricsMutex.
// Also uses ti.nice (cached by thread_scan) instead of re-opening /proc.
ThreadFeatures extractMlFeaturesCtx(const ThreadInfo& ti, int targetCpu, const ScoringContext& ctx) {
    unsigned long long avgRunUs = 0;
    if (ti.nrSwitches > 0) {
        double avgRunUsD = (double)ti.runTime / (double)ti.nrSwitches / 1000.0;
        avgRunUs = (unsigned long long)avgRunUsD;
    }
    // Read from ctx snapshot (no lock)
    int cpuUsage = ctx.getUtil(targetCpu);
    int thermal = ctx.getTemp(targetCpu);
    int capacity = topology::kDefaultCapacity;
    auto it = ctx.cpuMetrics.find(targetCpu);
    if (it != ctx.cpuMetrics.end()) capacity = it->second.capacity;

    bool isGame = (ti.role == ROLE_S3_GAME);
    if (!isGame && ti.subtype.find("game") != std::string::npos) isGame = true;
    // P1-3: snapshot the scene cache under the mutex instead of bare reads.
    bool sceneIsGame = false; int sceneFgPid = -1; std::string sceneFgPkg;
    if (!isGame) {
        refreshGameSceneCache();
        readSceneCache(sceneIsGame, sceneFgPid, sceneFgPkg);
        if (sceneIsGame && sceneFgPid > 0 && ti.pid == sceneFgPid &&
            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER)) {
            isGame = true;
        }
    }
    if (!isGame) {
        refreshGameSceneCache();
        readSceneCache(sceneIsGame, sceneFgPid, sceneFgPkg);
        if (!sceneFgPkg.empty() &&
            isPackageInGameList(sceneFgPkg) &&
            sceneFgPid > 0 && ti.pid == sceneFgPid &&
            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER)) {
            isGame = true;
        }
    }
    bool curMatch = (ti.currentCpu == targetCpu);
    unsigned long long migCount = getMigrationCount(ti.tid);
    // P0-opt4: use cached ti.nice instead of re-opening /proc/<tid>/stat
    int nice = ti.nice;
    // ROUND-010 D4-04: switches RATE, not cumulative nrSwitches.
    unsigned long long swRate2 = (unsigned long long)ti.switchesPerSec;
    return LearningModel::extractFeatures(
        avgRunUs, swRate2, cpuUsage, ti.priority, nice,
        isGame, thermal, capacity, curMatch, migCount);
}

// P1-12: Dynamic scoring normalization (from real original)
// Tracks P5/P95 of base scores across CPUs to adapt normalization range
// to actual hardware instead of hardcoded -200~80.
static std::vector<int> g_baseScoreHistory;
static constexpr int kBaseScoreHistoryMax = 256;
static float g_baseScoreP5 = -200.0f;
static float g_baseScoreP95 = 80.0f;
static std::mutex g_baseScoreMutex;

void updateBaseScoreDynamicRange() {
    // S-001: g_allCpus is rebuilt by detectCpuTopology() under g_topologyRwMutex.
    // Traversing it lock-free races with a topology realloc (SIGSEGV risk).
    // Snapshot it under the topology lock FIRST — per the documented lock order
    // g_topologyRwMutex is taken OUTER, before g_baseScoreMutex — then release
    // and take g_baseScoreMutex for the history bookkeeping. Never hold both.
    std::vector<int> cpusSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        cpusSnap = g_allCpus;
    }
    std::lock_guard<std::mutex> lock(g_baseScoreMutex);
    // Collect current precomputed base scores from the coherent topology snapshot
    for (int cpu : cpusSnap) {
        if (cpu < 0 || cpu >= (int)g_precomputedCpuBaseScore.size()) continue;
        g_baseScoreHistory.push_back(g_precomputedCpuBaseScore[cpu]);
    }
    if ((int)g_baseScoreHistory.size() > kBaseScoreHistoryMax) {
        g_baseScoreHistory.erase(g_baseScoreHistory.begin(),
            g_baseScoreHistory.begin() + (g_baseScoreHistory.size() - kBaseScoreHistoryMax));
    }
    if (g_baseScoreHistory.size() < 8) return;
    // Compute P5 and P95
    std::vector<int> sorted = g_baseScoreHistory;
    std::sort(sorted.begin(), sorted.end());
    int p5Idx = (int)(sorted.size() * 0.05f);
    int p95Idx = (int)(sorted.size() * 0.95f);
    if (p95Idx >= (int)sorted.size()) p95Idx = (int)sorted.size() - 1;
    // EMA smoothing to avoid jitter
    float newP5 = (float)sorted[p5Idx];
    float newP95 = (float)sorted[p95Idx];
    g_baseScoreP5 = g_baseScoreP5 * 0.9f + newP5 * 0.1f;
    g_baseScoreP95 = g_baseScoreP95 * 0.9f + newP95 * 0.1f;
    // Ensure sane range
    if (g_baseScoreP95 - g_baseScoreP5 < 50.0f) {
        float mid = (g_baseScoreP5 + g_baseScoreP95) / 2.0f;
        g_baseScoreP5 = mid - 25.0f;
        g_baseScoreP95 = mid + 25.0f;
    }
}

// P1-12: Calibrate capacity normalization range from actual cpu_capacity sysfs.
// Reads /sys/devices/system/cpu/cpuN/cpu_capacity for each CPU and uses
// min/max as normalization bounds instead of hardcoded 0~1024.
static float g_capNormMin = 0.0f;
static float g_capNormMax = 1024.0f;
static bool g_capNormCalibrated = false;
// G7 R004-B2 round2: recalibrate when topology generation advances (hotplug /
// cluster-count change). g_capNormTopoGen remembers the generation we last read.
static uint64_t g_capNormTopoGen = UINT64_MAX;

void calibrateCapacityNormRange() {
    uint64_t curGen = g_topologyGeneration.load(std::memory_order_relaxed);
    if (g_capNormCalibrated && curGen == g_capNormTopoGen) return;  // stable
    g_capNormTopoGen = curGen;
    int minCap = 1024, maxCap = 0;
    bool found = false;
    // P1-7: snapshot g_allCpus under topology lock to avoid iteration during rebuild.
    std::vector<int> cpus;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        cpus = g_allCpus;
    }
    for (int cpu : cpus) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity";
        std::ifstream f(path);
        if (!f) continue;
        int cap = 0;
        f >> cap;
        if (cap > 0) {
            found = true;
            if (cap < minCap) minCap = cap;
            if (cap > maxCap) maxCap = cap;
        }
    }
    if (found && maxCap > minCap) {
        g_capNormMin = (float)minCap;
        g_capNormMax = (float)maxCap;
        g_capNormCalibrated = true;
    }
}

// G7 R004-B2: normalization range for the raw capacity score.
// Interactive roles use the MEASURED min/max cpu_capacity so that the little
// core maps to ~0 and the strongest core to ~100 regardless of the SoC's EM
// scaling factor (some report max 512/850, not 1024). Background uses an
// inverse capacity (ref - cap) that lives in 0..1024, so keep its legacy range.
// Falls back to 0..1024 when sysfs capacity is unreadable (g_capNormCalibrated
// false) — matches the historical behaviour, no crash.
static inline void nCapRangeFor(ThreadRole role, float& lo, float& hi) {
    if (role != ROLE_S5_BACKGROUND && g_capNormCalibrated) {
        lo = g_capNormMin;
        hi = g_capNormMax;
    } else {
        lo = 0.0f;
        hi = 1024.0f;
    }
}

// P1-12: Dynamic sigmoid parameters for heuristic score normalization.
// Tracks heuristic score distribution and adjusts sigmoid center/scale.
static float g_sigmoidCenter = 100.0f;
static float g_sigmoidScale = 150.0f;
static std::vector<int> g_heuristicScoreHistory;
static constexpr int kHeuristicHistoryMax = 512;
static std::mutex g_heuristicMutex;

void trackHeuristicScore(int score) {
    std::lock_guard<std::mutex> lock(g_heuristicMutex);
    g_heuristicScoreHistory.push_back(score);
    // P3: erase(begin()) shifts every remaining element (O(n)) on every push once
    // the buffer is full. Trim ALL excess in one batch erase instead.
    if ((int)g_heuristicScoreHistory.size() > kHeuristicHistoryMax) {
        size_t excess = g_heuristicScoreHistory.size() - (size_t)kHeuristicHistoryMax;
        g_heuristicScoreHistory.erase(g_heuristicScoreHistory.begin(),
                                      g_heuristicScoreHistory.begin() + excess);
    }
}

void updateSigmoidParams() {
    std::lock_guard<std::mutex> lock(g_heuristicMutex);
    if (g_heuristicScoreHistory.size() < 16) return;
    std::vector<int> sorted = g_heuristicScoreHistory;
    std::sort(sorted.begin(), sorted.end());
    int median = sorted[sorted.size() / 2];
    int p16 = sorted[(int)(sorted.size() * 0.16f)];
    int p84 = sorted[(int)(sorted.size() * 0.84f)];
    // Center = median, scale = (P84 - P16) / 2 (approx 1 sigma)
    float newCenter = (float)median;
    float newScale = (float)(p84 - p16) / 2.0f;
    if (newScale < 20.0f) newScale = 20.0f;
    if (newScale > 500.0f) newScale = 500.0f;
    // EMA smoothing
    g_sigmoidCenter = g_sigmoidCenter * 0.95f + newCenter * 0.05f;
    g_sigmoidScale = g_sigmoidScale * 0.95f + newScale * 0.05f;
}

// Normalize heuristic score to 0-1 range using sigmoid (P1-12: dynamic params)
float normalizeHeuristicScore(int score) {
    float x = ((float)score - g_sigmoidCenter) / g_sigmoidScale;
    if (x > 20.0f) return 1.0f;
    if (x < -20.0f) return 0.0f;
    return 1.0f / (1.0f + std::exp(-x));
}

// Precomputed CPU base score (thermal + contention - predicted load).
// These terms depend only on cpuId, not on thread, so they can be computed once
// per daemonLoop cycle instead of per (thread, cpu) pair. Reduces computeScore
// calls from 40 (5 threads x 8 cpus) to 8 precomputations + 40 lookups.
// moved to top as std::vector<int>

void precomputeCpuBaseScores() {
    // P3-6: Iterate g_allCpus (actual CPU IDs) instead of 0..size-1.
    // On systems with non-contiguous CPU IDs (e.g. hotplugged cores),
    // 0..size-1 could index wrong or out-of-range CPUs.
    // P1-7: snapshot g_allCpus under topology lock.
    std::vector<int> cpus;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        cpus = g_allCpus;
    }
    for (int cpu : cpus) {
        if (cpu < 0 || cpu >= (int)g_precomputedCpuBaseScore.size()) continue;
        g_precomputedCpuBaseScore[cpu] = thermalScore(cpu)
                                        + contentionPenalty(cpu)
                                        - predictedLoadPenalty(cpu);
    }
    g_precomputedValid.store(true);
}

// fix: computeScore now includes historyScore (learning system was dead)
// Scoring engine physical meaning documentation.
// Score components (all additive, each ~0-1000 range, linear fusion justified by
// independent dimensions + empirical tuning + interpretability):
//   capacityScore     (~0-1024): CPU compute capacity, higher = more throughput
//   latencyScore      (~0-200):  cache/NUMA locality, higher = less migration cost
//   cacheScore        (~0-100):  shared cache affinity, higher = better cache hit
//   thermalScore      (~-200-0): thermal penalty, hotter = more negative
//   contentionPenalty (~-200-80): utilization-based contention, high util = negative
//   predictedLoadPenalty (~-100-0): ML-predicted future load penalty
//   powerScore        (~-100-0): power consumption penalty (fix94 Task9)
//   rolePoolScore     (~-50-50): thread role pool preference (UI->big, BG->little)
// Linear additivity is valid because dimensions are orthogonal; weights tuned empirically.
// P3: Normalize each dimension to 0-100 scale before weighted sum.
// Dimension ranges: capacity(0,1024), latency(0,200), cache(0,100),
// base(-200,80), power(-50,0), frame(0,50), pool(-50,50), success(0,100), migration(0,10)
static inline constexpr float normalizeScore101(float val, float minV, float maxV) noexcept {
    if (maxV <= minV || std::isnan(val) || std::isinf(val)) return 50.0f;
    float n = (val - minV) / (maxV - minV) * 100.0f;
    if (std::isnan(n) || std::isinf(n)) return 50.0f;
    if (n < 0.0f) n = 0.0f;
    if (n > 100.0f) n = 100.0f;
    return n;
}

int computeScore(int cpuId, const ThreadInfo& ti) {
    // P2 Problem6: two-arg overload delegates with knownFromCpu=-1 (legacy path,
    // migrationPenalty uses its 30s-TTL cache). decidePlacement calls the 3-arg
    // overload with a fresh liveCpu so the migration-cost term matches the
    // fromPool / hf[9] / ALREADY_OPTIMAL decision.
    return computeScore(cpuId, ti, -1);
}

int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu) {
    if (ti.role == ROLE_S0_SYSTEM) return 0;
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 0;
    // P3-8: Bounds check for precomputed base score array when valid.
    // g_precomputedCpuBaseScore is indexed by CPU ID; if cpuId is out of range
    // (e.g. non-contiguous CPU IDs), fall back to on-demand computation.
    bool usePrecomputed = g_precomputedValid.load() &&
                          (cpuId >= 0 && cpuId < (int)g_precomputedCpuBaseScore.size());
    float capMul = 1.0f, latMul = 1.0f, thermMul = 1.0f, contMul = 1.0f, powMul = 1.0f;
    // P2: lock + copy the whole struct under g_dynamicWeightsMutex — the struct
    // is 5 floats + bool (not atomic), and the ML thread writes it concurrently.
    // Reading field-by-field unlocked would tear-read (e.g. new floats with old
    // valid, or vice versa). Snapshot it whole under the mutex, then use the copy.
    {
        DynamicScoreWeights dwSnap;
        {
            std::lock_guard<std::mutex> dwlk(g_dynamicWeightsMutex);
            dwSnap = g_dynamicScoreWeights;
        }
        if (dwSnap.valid) {
            capMul = dwSnap.capacityMul;
            latMul = dwSnap.latencyMul;
            thermMul = dwSnap.thermalMul;
            contMul = dwSnap.contentionMul;
            powMul = dwSnap.powerMul;
        }
    }
    // Role-aware contention weight.
    // ROOT CAUSE of "小核满载大核闲置": contention dimension (idle boost + util
    // penalty) has ~86-100 pt dynamic range at weight 1.0, dominating capacity
    // (~22 pts) and poolScore (~20-36 pts after 0.4 weight). This pulled high-
    // priority threads toward idle little cores instead of loaded big cores.
    // Fix: For latency-sensitive roles (UI/Render/Game), reduce contention weight
    // to 0.4 so capacity+poolScore (performance) dominates placement. Background
    // and throughput threads keep full contention weight for load balancing — this
    // preserves non-game performance while fixing game scheduling.
    float effectiveContMul = contMul;
    if (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) {
        effectiveContMul = contMul * 0.4f;
    }
    int baseScore = usePrecomputed ? g_precomputedCpuBaseScore[cpuId]
                                   : (thermalScore(cpuId) + contentionPenalty(cpuId) - predictedLoadPenalty(cpuId));
    // P3: raw scores for each dimension
    int rawCap = capacityScore(cpuId, ti.role);
    int rawLat = latencyScore(cpuId, ti.role);
    int rawCache = cacheScore(cpuId, ti.pid, ti.tid);
    int rawPower = powerScore(cpuId, ti.role)
                  - computePowerPenalty(cpuId, ti.priority)   // P1-1 batch 2: power-aware scoring
                  - getBatteryAgePenalty(cpuId);               // P1-1 batch 2: battery-age penalty
    int rawFrame = frameScore(cpuId, ti.role);
    int rawPool = poolScore(cpuId, ti.role);
    int rawSuccess = placementSuccessScore(cpuId, ti.tid);
    // P2 Problem6: when the caller supplies a fresh fromCpu (decidePlacement
    // liveCpu), score migration cost against it instead of the 30s-TTL cached
    // currentCpu, so a thread that just switched CPU is not penalized against a
    // stale source CPU. knownFromCpu<0 preserves the legacy cached path.
    int rawMigration = (knownFromCpu >= 0)
        ? migrationPenaltyWithFrom(cpuId, ti.pid, ti.tid, knownFromCpu)
        : migrationPenalty(cpuId, ti.pid, ti.tid);
    // P3: normalize each dimension to 0-100, then weighted sum
    // P1-13: Corrected normalization ranges to match real original:
    //   cache=0~10, power=-50~50, frame=0~100
    float nCapLo, nCapHi; nCapRangeFor(ti.role, nCapLo, nCapHi);
    float nCap = normalizeScore101((float)rawCap, nCapLo, nCapHi);
    float nLat = normalizeScore101((float)rawLat, 0.0f, 200.0f);
    float nCache = normalizeScore101((float)rawCache, 0.0f, 10.0f);
    float nBase = normalizeScore101((float)baseScore, -200.0f, 80.0f);
    float nPower = normalizeScore101((float)rawPower, -50.0f, 50.0f);
    float nFrame = normalizeScore101((float)rawFrame, 0.0f, 100.0f);
    float nPool = normalizeScore101((float)rawPool, -50.0f, 50.0f);
    float nSuccess = normalizeScore101((float)rawSuccess, 0.0f, 100.0f);
    float nMigration = normalizeScore101((float)rawMigration, 0.0f, 10.0f);

    // ADRC risk score dimension: high-risk critical threads get priority boost.
    // For UI/RENDER/GAME threads, ADRC riskScore (0-1) adds a bonus to big-core
    // placement. For non-critical threads, ADRC risk has minimal impact.
    float nAdrcRisk = 0.0f;
    if (ti.role >= ROLE_S1_UI && ti.role <= ROLE_S3_GAME) {
        AdrcRole adrcRole = mapThreadRoleToAdrc(ti.role);
        AdrcOutput adrcOut = getRoleOutput(adrcRole);
        if (adrcOut.riskScore > 0.001) {
            // Snapshot pools for ADRC big/little pool check
            PoolSnapshot adrcPools = snapshotPools();
            bool inBig = std::find(adrcPools.big.begin(), adrcPools.big.end(), cpuId) != adrcPools.big.end();
            bool inLittle = std::find(adrcPools.little.begin(), adrcPools.little.end(), cpuId) != adrcPools.little.end();
            if (inBig && adrcOut.shouldBoost) {
                nAdrcRisk = adrcOut.riskScore * 100.0f;  // up to +100 raw score
            } else if (inLittle && adrcOut.shouldBoost) {
                nAdrcRisk = -adrcOut.riskScore * 50.0f;  // penalty for little core under high risk
            } else if (!inBig && !inLittle && adrcOut.shouldBoost) {
                nAdrcRisk = adrcOut.riskScore * 30.0f;   // mid-core moderate boost (only while boosting)
            } else {
                // G5-002: no directional adjustment when not boosting. The old
                // bare else gave +30*risk to EVERY core (incl. big/little) even
                // when shouldBoost==false, so the little-core penalty never
                // applied and risk injected a constant positive bias on all cores.
                nAdrcRisk = 0.0f;
            }
        }
    }
    float nAdrcRiskNorm = normalizeScore101(nAdrcRisk, -50.0f, 100.0f);

    // Weighted sum (capacity dominant, others scaled)
    float score = nCap * capMul
                + nLat * latMul * 0.5f
                + nCache * 0.3f
                + nBase * ((thermMul + effectiveContMul) / 2.0f)
                + nPower * powMul * 0.5f
                + nFrame * 0.4f
                + nPool * 0.4f
                + nSuccess * 0.2f
                - nMigration * 0.3f
                + nAdrcRiskNorm * 0.35f;  // ADRC risk dimension
    return (int)score;
}

// ============================================================================
// P0-opt1: Context-aware computeScore — reads all globals from pre-snapshotted
// ScoringContext instead of re-locking g_cpuMetricsMutex / g_topologyRwMutex /
// g_cfgMutex for every candidate. This is the hot path used by decidePlacement().
// Lock order preserved: g_historyMutex (only in placementSuccessScore) is taken
// AFTER all context reads; no nested locks.
// ============================================================================
int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu, const ScoringContext& ctx) {
    if (ti.role == ROLE_S0_SYSTEM) return 0;
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 0;

    bool usePrecomputed = g_precomputedValid.load() &&
                          (cpuId >= 0 && cpuId < (int)g_precomputedCpuBaseScore.size());
    float capMul = 1.0f, latMul = 1.0f, thermMul = 1.0f, contMul = 1.0f, powMul = 1.0f;
    // P2: lock + copy the whole struct under g_dynamicWeightsMutex — the struct
    // is 5 floats + bool (not atomic), and the ML thread writes it concurrently.
    // Reading field-by-field unlocked would tear-read (e.g. new floats with old
    // valid, or vice versa). Snapshot it whole under the mutex, then use the copy.
    {
        DynamicScoreWeights dwSnap;
        {
            std::lock_guard<std::mutex> dwlk(g_dynamicWeightsMutex);
            dwSnap = g_dynamicScoreWeights;
        }
        if (dwSnap.valid) {
            capMul = dwSnap.capacityMul;
            latMul = dwSnap.latencyMul;
            thermMul = dwSnap.thermalMul;
            contMul = dwSnap.contentionMul;
            powMul = dwSnap.powerMul;
        }
    }
    float effectiveContMul = contMul;
    if (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) {
        effectiveContMul = contMul * 0.4f;
    }

    // --- baseScore: from precomputed or compute inline from ctx ---
    int baseScore;
    if (usePrecomputed) {
        baseScore = g_precomputedCpuBaseScore[cpuId];
    } else {
        // Inline thermalScore from ctx (no lock)
        int temp = ctx.getTemp(cpuId);
        int thermPenalty = 0, thermBoost = 0;
        if (temp != 0) {
            if (temp > thermal::kHighTempThreshold) thermPenalty = (temp - thermal::kHighTempThreshold) * thermal::kPenaltyPerDegree;
            if (temp < thermal::kLowTempThreshold) thermBoost = (thermal::kLowTempThreshold - temp) * thermal::kBoostPerDegree;
        }
        int thermalVal = thermBoost - thermPenalty;
        // Inline contentionPenalty from ctx (no lock)
        int util = ctx.getUtil(cpuId);
        int contVal;
        if (util > contention::kHighUtilThreshold) contVal = contention::kHighUtilPenalty;
        else if (util > contention::kMidUtilThreshold) contVal = contention::kMidUtilPenalty;
        else if (util > contention::kLowUtilThreshold) contVal = contention::kLowUtilPenalty;
        else contVal = contention::kIdleBoost;
        baseScore = thermalVal + contVal - predictedLoadPenalty(cpuId);
    }

    // --- rawCap: inline capacityScore from ctx (no lock) ---
    int cap = ctx.getCapacity(cpuId);
    int rawCap;
    switch (ti.role) {
        case ROLE_S0_SYSTEM: rawCap = 0; break;
        case ROLE_S5_BACKGROUND: rawCap = capacity::kBackgroundInverseRef - cap; break;
        default: rawCap = cap; break;
    }

    int rawLat = latencyScore(cpuId, ti.role);
    int rawCache = cacheScore(cpuId, ti.pid, ti.tid);
    int rawPower = powerScore(cpuId, ti.role)
                  - computePowerPenalty(cpuId, ti.priority)
                  - getBatteryAgePenalty(cpuId);

    // --- rawFrame: use ctx.cfg directly (no snapshotConfig) ---
    int rawFrame = 0;
    if (ctx.cfg && ctx.cfg->frameAware && g_frameCritical.load(std::memory_order_relaxed)) {
        if (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME)
            rawFrame = frame_score::kCriticalBoost;
    }

    // --- rawPool: use ctx.pools directly (no snapshotPools) ---
    bool inPrime = std::find(ctx.pools.prime.begin(), ctx.pools.prime.end(), cpuId) != ctx.pools.prime.end();
    bool inLittle = std::find(ctx.pools.little.begin(), ctx.pools.little.end(), cpuId) != ctx.pools.little.end();
    bool inMid    = std::find(ctx.pools.mid.begin(),    ctx.pools.mid.end(),    cpuId) != ctx.pools.mid.end();
    bool inBig    = std::find(ctx.pools.big.begin(),    ctx.pools.big.end(),    cpuId) != ctx.pools.big.end();
    int rawPool = 0;
    switch (ti.role) {
        case ROLE_S0_SYSTEM: rawPool = 0; break;
        case ROLE_S1_UI:
            if (inPrime) rawPool = pool_score::kUiPrime;
            else if (inBig) rawPool = pool_score::kUiBig;
            else if (inMid) rawPool = pool_score::kUiMid;
            else if (inLittle) rawPool = pool_score::kUiLittle;
            break;
        case ROLE_S2_RENDER:
            if (inPrime) rawPool = pool_score::kRenderPrime;
            else if (inBig) rawPool = pool_score::kRenderBig;
            else if (inMid) rawPool = pool_score::kRenderMid;
            else if (inLittle) rawPool = pool_score::kRenderLittle;
            break;
        case ROLE_S3_GAME:
            if (inPrime) rawPool = pool_score::kGamePrime;
            else if (inBig) rawPool = pool_score::kGameBig;
            else if (inMid) rawPool = pool_score::kGameMid;
            else if (inLittle) rawPool = pool_score::kGameLittle;
            break;
        case ROLE_S4_THROUGHPUT:
            if (inPrime) rawPool = pool_score::kThroughputPrime;
            else if (inBig) rawPool = pool_score::kThroughputBig;
            else if (inMid) rawPool = pool_score::kThroughputMid;
            else if (inLittle) rawPool = pool_score::kThroughputLittle;
            break;
        case ROLE_S5_BACKGROUND:
            if (inLittle) rawPool = pool_score::kBackgroundLittle;
            else if (inMid) rawPool = pool_score::kBackgroundMid;
            else if (inBig) rawPool = pool_score::kBackgroundBig;
            else if (inPrime) rawPool = pool_score::kBackgroundPrime;
            break;
    }

    // --- rawSuccess: placementSuccessScore but use ctx.cfg for learning flags ---
    int rawSuccess = 0;
    if (ctx.cfg && ctx.cfg->learningEnabled) {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        auto it = g_history.find(ti.tid);
        if (it != g_history.end()) {
            if (it->second.samples >= ctx.cfg->minSamplesForLearning) {
                if (it->second.preferredCpu == cpuId) rawSuccess = it->second.score;
            }
        }
    }

    // --- rawMigration: migrationPenaltyCore with ctx.pools (no snapshotPools) ---
    int rawMigration;
    if (knownFromCpu >= 0) {
        // P1-1: use cpu_set_t affinity mask (no heap set<int>).
        cpu_set_t affMask;
        CPU_ZERO(&affMask);  // P2: defensive zero before any by-value assignment
        time_t now = time(nullptr);
        bool cacheValid = (tlMigCacheTid == ti.tid) &&
                          (tlMigCacheTimestamp != 0) &&
                          (now - tlMigCacheTimestamp < kMigPenaltyCacheTtlSec);
        if (cacheValid) {
            affMask = tlMigCacheAffMask;
        } else {
            affMask = getThreadAffinityCpus(ti.tid);
            tlMigCacheTid = ti.tid;
            tlMigCacheAffMask = affMask;
            tlMigCacheTimestamp = now;
        }
        rawMigration = migrationPenaltyCoreWithPools(cpuId, knownFromCpu, affMask, ctx.pools);
    } else {
        rawMigration = migrationPenalty(cpuId, ti.pid, ti.tid);
    }

    // --- Normalize ---
    float nCapLo, nCapHi; nCapRangeFor(ti.role, nCapLo, nCapHi);
    float nCap = normalizeScore101((float)rawCap, nCapLo, nCapHi);
    float nLat = normalizeScore101((float)rawLat, 0.0f, 200.0f);
    float nCache = normalizeScore101((float)rawCache, 0.0f, 10.0f);
    float nBase = normalizeScore101((float)baseScore, -200.0f, 80.0f);
    float nPower = normalizeScore101((float)rawPower, -50.0f, 50.0f);
    float nFrame = normalizeScore101((float)rawFrame, 0.0f, 100.0f);
    float nPool = normalizeScore101((float)rawPool, -50.0f, 50.0f);
    float nSuccess = normalizeScore101((float)rawSuccess, 0.0f, 100.0f);
    float nMigration = normalizeScore101((float)rawMigration, 0.0f, 10.0f);

    // --- ADRC risk: use ctx.pools (no snapshotPools) ---
    float nAdrcRisk = 0.0f;
    if (ti.role >= ROLE_S1_UI && ti.role <= ROLE_S3_GAME) {
        AdrcRole adrcRole = mapThreadRoleToAdrc(ti.role);
        AdrcOutput adrcOut = getRoleOutput(adrcRole);
        if (adrcOut.riskScore > 0.001) {
            bool adrcInBig = std::find(ctx.pools.big.begin(), ctx.pools.big.end(), cpuId) != ctx.pools.big.end();
            bool adrcInLittle = std::find(ctx.pools.little.begin(), ctx.pools.little.end(), cpuId) != ctx.pools.little.end();
            if (adrcInBig && adrcOut.shouldBoost) {
                nAdrcRisk = adrcOut.riskScore * 100.0f;
            } else if (adrcInLittle && adrcOut.shouldBoost) {
                nAdrcRisk = -adrcOut.riskScore * 50.0f;
            } else if (!adrcInBig && !adrcInLittle && adrcOut.shouldBoost) {
                nAdrcRisk = adrcOut.riskScore * 30.0f;   // mid-core boost (only while boosting)
            } else {
                // G5-002: no directional adjustment when not boosting (mirror of
                // the non-ctx path above).
                nAdrcRisk = 0.0f;
            }
        }
    }
    float nAdrcRiskNorm = normalizeScore101(nAdrcRisk, -50.0f, 100.0f);

    float score = nCap * capMul
                + nLat * latMul * 0.5f
                + nCache * 0.3f
                + nBase * ((thermMul + effectiveContMul) / 2.0f)
                + nPower * powMul * 0.5f
                + nFrame * 0.4f
                + nPool * 0.4f
                + nSuccess * 0.2f
                - nMigration * 0.3f
                + nAdrcRiskNorm * 0.35f;
    return (int)score;
}

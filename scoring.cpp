#include <cmath>
#include <atomic>
#include <vector>
#include <set>
#include <dirent.h>
#include <cstring>
#include <cstdlib>
#include "aatped.h"
#include "adrc.h"
#include "scoring_interface.h"
using namespace aatpe;
using namespace aatpe_ml;
using namespace aatpe::adrc;

template <typename T, size_t Cap>
struct RingBuffer {
    std::unique_ptr<std::mutex> mtx = std::make_unique<std::mutex>();
    T data[Cap];
    size_t head = 0;
    size_t count = 0;

    void push(const T& v) {
        std::lock_guard<std::mutex> lk(*mtx);
        data[head] = v;
        head = (head + 1) % Cap;
        if (count < Cap) count++;
    }
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

void initScoringArrays(int cpuCount) {
    if (cpuCount <= 0) cpuCount = 8;
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
    return 0;
}

float predictCpuLoadMultiStep(int cpuId, int steps) {
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 50.0f;
    if (steps <= 0) steps = 1;
    if (steps > 5) steps = 5;
    std::vector<int> history = g_cpuLoadHistory[cpuId].snapshot();
    if ((int)history.size() < 3) return history.empty() ? 50.0f : (float)history.back();
    float a = 0, b = 0;
    if (!linearRegression(history, a, b)) return (float)history.back();
    float maxPred = 0.0f;
    int n = (int)history.size();
    for (int s = 1; s <= steps; s++) {
        float pred = a * (float)(n + s - 1) + b;
        pred = ((pred) < (0.0f) ? (0.0f) : ((pred) > (100.0f) ? (100.0f) : (pred)));
        if (pred > maxPred) maxPred = pred;
    }
    return maxPred;
}

void recordPredictionError(int cpuId, float predicted, float actual) {
    if (cpuId < 0 || cpuId >= (int)g_predictionErrors.size()) return;
    if (std::isnan(predicted) || std::isnan(actual) ||
        std::isinf(predicted) || std::isinf(actual)) return;
    float error = std::abs(predicted - actual);
    if (std::isnan(error)) return;
    g_predictionErrors[cpuId].push(error);
}

float getPredictionConfidence(int cpuId) {
    if (cpuId < 0 || cpuId >= (int)g_predictionErrors.size()) return 0.0f;
    std::vector<float> errors = g_predictionErrors[cpuId].snapshot();
    if (errors.empty()) return 0.5f;
    float sum = 0;
    for (float e : errors) sum += e;
    float mae = sum / (float)errors.size();
    float confidence = 1.0f - mae / 50.0f;
    return ((confidence) < (0.0f) ? (0.0f) : ((confidence) > (1.0f) ? (1.0f) : (confidence)));
}

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

bool hasAnyPredictionErrorSamples() {
    for (int i = 0; i < (int)g_predictionErrors.size(); i++) {
        if (!g_predictionErrors[i].snapshot().empty()) return true;
    }
    return false;
}

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

int cacheScore(int cpuId, int pid, int tid) {
    (void)pid;
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
    if (temp == 0) return 0;
    int penalty = 0, boost = 0;
    if (temp > thermal::kHighTempThreshold) penalty = (temp - thermal::kHighTempThreshold) * thermal::kPenaltyPerDegree;
    if (temp < thermal::kLowTempThreshold) boost = (thermal::kLowTempThreshold - temp) * thermal::kBoostPerDegree;
    return boost - penalty;
}

int powerScore(int cpuId, ThreadRole role) noexcept {
    (void)cpuId;
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
    Config scoreCfg;
    snapshotConfig(scoreCfg);
    if (!scoreCfg.frameAware || !g_frameCritical) return 0;
    if (role == ROLE_S1_UI || role == ROLE_S2_RENDER || role == ROLE_S3_GAME) return frame_score::kCriticalBoost;
    return 0;
}

static inline PoolSnapshot snapshotPools() {
    PoolSnapshot s;
    std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
    s.little = g_littlePool;
    s.mid    = g_midPool;
    s.big    = g_bigPool;
    for (const auto& cl : g_clusters) {
        if (cl.tier == CpuTier::PRIME) { s.prime = cl.cpus; break; }
    }
    return s;
}

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
    calibrateCapacityNormRange();
    ctx.pools = snapshotPools();
    {
        std::lock_guard<std::mutex> mlock(g_cpuMetricsMutex);
        ctx.cpuMetrics = g_cpuMetrics;
    }
    return ctx;
}

int poolScore(int cpuId, ThreadRole role) {
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

static constexpr int kMigPenaltyCacheTtlSec = 30;
static thread_local int tlMigCacheTid = -1;
static thread_local int tlMigCacheCurrentCpu = -1;
static thread_local cpu_set_t tlMigCacheAffMask;
static thread_local time_t tlMigCacheTimestamp = 0;
static thread_local bool tlMigCacheHasCur = false;

static void getMigrationPenaltyFactors(float& outBigLittle, float& outSame) {
    static std::atomic<float> s_bigLittle{3.0f};
    static std::atomic<float> s_same{1.0f};
    static std::atomic<long long> s_cacheMs{0};
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
    int currentCpu;
    cpu_set_t affMask;
    CPU_ZERO(&affMask);
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

int placementSuccessScore(int cpuId, int tid) {
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
int historyScore(int cpuId, int tid) { return placementSuccessScore(cpuId, tid); }

static std::unordered_map<int, time_t> s_lastWrite;
static std::mutex s_lastWriteMutex;
static std::unordered_map<int, unsigned long long> g_threadMigrationCount;
static std::mutex g_migCountMutex;

static time_t scoringBootTs() {
    time_t t = g_startTime.load(std::memory_order_relaxed);
    return t != 0 ? t : time(nullptr);
}

void recordPlacementResult(int tid, int cpuId, bool good) {
    Config recCfg;
    snapshotConfig(recCfg);
    if (!recCfg.learningEnabled) return;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        auto& h = g_history[tid];
        int weight = recCfg.historyWeight;
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
    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> wlock(s_lastWriteMutex);
        auto it = s_lastWrite.find(tid);
        if (it != s_lastWrite.end() && (now - it->second) < 1) return;
        s_lastWrite[tid] = now;
    }
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
    mkdirIfNotExist(histDir.c_str(), 0700);
    std::string histPath = histDir + "/" + std::to_string(tid);
    std::string content = "preferred_cpu=" + std::to_string(preferredCpu) + "\n"
                         + "placement_success_rate=" + std::to_string(score) + "\n"
                         + "samples=" + std::to_string(samples) + "\n"
                         + "last_update=" + std::to_string(lastUpdate) + "\n"
                         + "daemon_boot_ts=" + std::to_string((long long)scoringBootTs()) + "\n"
                         + "note=score_is_placement_success_not_performance\n";
    writeFile(histPath.c_str(), content);
}

void scoringHistorySweepStale() {
    std::string histDir = std::string(RUN_DIR) + "/history";
    DIR* d = opendir(histDir.c_str());
    if (!d) return;
    time_t boot = scoringBootTs();
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;
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
        if (fileBoot != (long long)boot) unlink(p.c_str());
    }
    closedir(d);
}

void cleanupThreadScoringState(int tid) {
    {
        std::lock_guard<std::mutex> w(s_lastWriteMutex);
        s_lastWrite.erase(tid);
    }
    std::lock_guard<std::mutex> m(g_migCountMutex);
    g_threadMigrationCount.erase(tid);
}

void recordOutcome(int tid, int cpuId, bool good) { recordPlacementResult(tid, cpuId, good); }

void decayHistory() {
    Config decayCfg;
    snapshotConfig(decayCfg);
    if (!decayCfg.learningEnabled) return;
    int decay = decayCfg.learningDecay;
    if (decay < 0) decay = 0;
    if (decay > 100) decay = 100;
    std::lock_guard<std::mutex> lock(g_historyMutex);
    time_t now = time(nullptr);
    for (auto it = g_history.begin(); it != g_history.end(); ) {
        it->second.score = it->second.score * decay / 100;
        if (it->second.score < daemon::kHistoryScoreMinThreshold) it->second.score = 0;
        if (now - it->second.lastUpdate > daemon::kHistoryEntryMaxAgeSec) {
            std::string histPath = std::string(RUN_DIR) + "/history/" + std::to_string(it->first);
            unlink(histPath.c_str());
            it = g_history.erase(it);
        } else {
            ++it;
        }
    }
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

void recordMigrationCount(int tid) {
    std::lock_guard<std::mutex> lock(g_migCountMutex);
    g_threadMigrationCount[tid]++;
}

unsigned long long getMigrationCount(int tid) {
    std::lock_guard<std::mutex> lock(g_migCountMutex);
    auto it = g_threadMigrationCount.find(tid);
    return (it != g_threadMigrationCount.end()) ? it->second : 0;
}

static int readThreadNice(int pid, int tid) {
    std::string statPath = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return 0;
    std::string content;
    std::getline(f, content);
    if (content.empty()) return 0;
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

static std::atomic<unsigned long long> g_lastSceneCheckMs{0};
static bool g_cachedIsGameScene = false;
static int g_cachedForegroundPid = -1;
static std::string g_cachedForegroundPackage;
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
    while (!scene.empty() && (scene.back() == '\n' || scene.back() == '\r' || scene.back() == ' '))
        scene.pop_back();
    bool isGameScene = (scene == "combat" || scene == "loading" || scene == "menu");
    std::string pkg = aatpeGetForegroundPackage();
    int pid = -1;
    if (!pkg.empty()) pid = findPidByPackage(pkg);
    std::lock_guard<std::mutex> lk(g_sceneCacheMutex);
    g_cachedIsGameScene = isGameScene;
    g_cachedForegroundPackage = pkg;
    g_cachedForegroundPid = pid;
}

ThreadFeatures extractMlFeatures(const ThreadInfo& ti, int targetCpu) {
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
    unsigned long long avgRunUs = 0;
    if (ti.nrSwitches > 0) {
        double avgRunUsD = (double)ti.runTime / (double)ti.nrSwitches / 1000.0;
        avgRunUs = (unsigned long long)avgRunUsD;
    }
    bool isGame = (ti.role == ROLE_S3_GAME);
    if (!isGame && ti.subtype.find("game") != std::string::npos) isGame = true;
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
    int nice = ti.nice;
    if (nice == 0 && ti.needsHydration) {
        nice = readThreadNice(ti.pid, ti.tid);
    }
    unsigned long long swRate = (unsigned long long)ti.switchesPerSec;
    return LearningModel::extractFeatures(
        avgRunUs, swRate, cpuUsage, ti.priority, nice,
        isGame, thermal, capacity, curMatch, migCount);
}

ThreadFeatures extractMlFeaturesCtx(const ThreadInfo& ti, int targetCpu, const ScoringContext& ctx) {
    unsigned long long avgRunUs = 0;
    if (ti.nrSwitches > 0) {
        double avgRunUsD = (double)ti.runTime / (double)ti.nrSwitches / 1000.0;
        avgRunUs = (unsigned long long)avgRunUsD;
    }
    int cpuUsage = ctx.getUtil(targetCpu);
    int thermal = ctx.getTemp(targetCpu);
    int capacity = topology::kDefaultCapacity;
    auto it = ctx.cpuMetrics.find(targetCpu);
    if (it != ctx.cpuMetrics.end()) capacity = it->second.capacity;

    bool isGame = (ti.role == ROLE_S3_GAME);
    if (!isGame && ti.subtype.find("game") != std::string::npos) isGame = true;
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
    int nice = ti.nice;
    unsigned long long swRate2 = (unsigned long long)ti.switchesPerSec;
    return LearningModel::extractFeatures(
        avgRunUs, swRate2, cpuUsage, ti.priority, nice,
        isGame, thermal, capacity, curMatch, migCount);
}

static std::vector<int> g_baseScoreHistory;
static constexpr int kBaseScoreHistoryMax = 256;
static float g_baseScoreP5 = -200.0f;
static float g_baseScoreP95 = 80.0f;
static std::mutex g_baseScoreMutex;

void updateBaseScoreDynamicRange() {
    std::vector<int> cpusSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        cpusSnap = g_allCpus;
    }
    std::lock_guard<std::mutex> lock(g_baseScoreMutex);
    for (int cpu : cpusSnap) {
        if (cpu < 0 || cpu >= (int)g_precomputedCpuBaseScore.size()) continue;
        g_baseScoreHistory.push_back(g_precomputedCpuBaseScore[cpu]);
    }
    if ((int)g_baseScoreHistory.size() > kBaseScoreHistoryMax) {
        g_baseScoreHistory.erase(g_baseScoreHistory.begin(),
            g_baseScoreHistory.begin() + (g_baseScoreHistory.size() - kBaseScoreHistoryMax));
    }
    if (g_baseScoreHistory.size() < 8) return;
    std::vector<int> sorted = g_baseScoreHistory;
    std::sort(sorted.begin(), sorted.end());
    int p5Idx = (int)(sorted.size() * 0.05f);
    int p95Idx = (int)(sorted.size() * 0.95f);
    if (p95Idx >= (int)sorted.size()) p95Idx = (int)sorted.size() - 1;
    float newP5 = (float)sorted[p5Idx];
    float newP95 = (float)sorted[p95Idx];
    g_baseScoreP5 = g_baseScoreP5 * 0.9f + newP5 * 0.1f;
    g_baseScoreP95 = g_baseScoreP95 * 0.9f + newP95 * 0.1f;
    if (g_baseScoreP95 - g_baseScoreP5 < 50.0f) {
        float mid = (g_baseScoreP5 + g_baseScoreP95) / 2.0f;
        g_baseScoreP5 = mid - 25.0f;
        g_baseScoreP95 = mid + 25.0f;
    }
}

static float g_capNormMin = 0.0f;
static float g_capNormMax = 1024.0f;
static bool g_capNormCalibrated = false;
static uint64_t g_capNormTopoGen = UINT64_MAX;

void calibrateCapacityNormRange() {
    uint64_t curGen = g_topologyGeneration.load(std::memory_order_relaxed);
    if (g_capNormCalibrated && curGen == g_capNormTopoGen) return;
    g_capNormTopoGen = curGen;
    int minCap = 1024, maxCap = 0;
    bool found = false;
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

static inline void nCapRangeFor(ThreadRole role, float& lo, float& hi) {
    if (role != ROLE_S5_BACKGROUND && g_capNormCalibrated) {
        lo = g_capNormMin;
        hi = g_capNormMax;
    } else {
        lo = 0.0f;
        hi = 1024.0f;
    }
}

static float g_sigmoidCenter = 100.0f;
static float g_sigmoidScale = 150.0f;
static std::vector<int> g_heuristicScoreHistory;
static constexpr int kHeuristicHistoryMax = 512;
static std::mutex g_heuristicMutex;

void trackHeuristicScore(int score) {
    std::lock_guard<std::mutex> lock(g_heuristicMutex);
    g_heuristicScoreHistory.push_back(score);
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
    float newCenter = (float)median;
    float newScale = (float)(p84 - p16) / 2.0f;
    if (newScale < 20.0f) newScale = 20.0f;
    if (newScale > 500.0f) newScale = 500.0f;
    g_sigmoidCenter = g_sigmoidCenter * 0.95f + newCenter * 0.05f;
    g_sigmoidScale = g_sigmoidScale * 0.95f + newScale * 0.05f;
}

float normalizeHeuristicScore(int score) {
    float x = ((float)score - g_sigmoidCenter) / g_sigmoidScale;
    if (x > 20.0f) return 1.0f;
    if (x < -20.0f) return 0.0f;
    return 1.0f / (1.0f + std::exp(-x));
}

void precomputeCpuBaseScores() {
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

static inline constexpr float normalizeScore101(float val, float minV, float maxV) noexcept {
    if (maxV <= minV || std::isnan(val) || std::isinf(val)) return 50.0f;
    float n = (val - minV) / (maxV - minV) * 100.0f;
    if (std::isnan(n) || std::isinf(n)) return 50.0f;
    if (n < 0.0f) n = 0.0f;
    if (n > 100.0f) n = 100.0f;
    return n;
}

int computeScore(int cpuId, const ThreadInfo& ti) {
    return computeScore(cpuId, ti, -1);
}

int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu) {
    if (ti.role == ROLE_S0_SYSTEM) return 0;
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 0;
    bool usePrecomputed = g_precomputedValid.load() &&
                          (cpuId >= 0 && cpuId < (int)g_precomputedCpuBaseScore.size());
    float capMul = 1.0f, latMul = 1.0f, thermMul = 1.0f, contMul = 1.0f, powMul = 1.0f;
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
    int baseScore = usePrecomputed ? g_precomputedCpuBaseScore[cpuId]
                                   : (thermalScore(cpuId) + contentionPenalty(cpuId) - predictedLoadPenalty(cpuId));
    int rawCap = capacityScore(cpuId, ti.role);
    int rawLat = latencyScore(cpuId, ti.role);
    int rawCache = cacheScore(cpuId, ti.pid, ti.tid);
    int rawPower = powerScore(cpuId, ti.role)
                  - computePowerPenalty(cpuId, ti.priority)
                  - getBatteryAgePenalty(cpuId);
    int rawFrame = frameScore(cpuId, ti.role);
    int rawPool = poolScore(cpuId, ti.role);
    int rawSuccess = placementSuccessScore(cpuId, ti.tid);
    int rawMigration = (knownFromCpu >= 0)
        ? migrationPenaltyWithFrom(cpuId, ti.pid, ti.tid, knownFromCpu)
        : migrationPenalty(cpuId, ti.pid, ti.tid);
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

    float nAdrcRisk = 0.0f;
    if (ti.role >= ROLE_S1_UI && ti.role <= ROLE_S3_GAME) {
        AdrcRole adrcRole = mapThreadRoleToAdrc(ti.role);
        AdrcOutput adrcOut = getRoleOutput(adrcRole);
        if (adrcOut.riskScore > 0.001) {
            PoolSnapshot adrcPools = snapshotPools();
            bool inBig = std::find(adrcPools.big.begin(), adrcPools.big.end(), cpuId) != adrcPools.big.end();
            bool inLittle = std::find(adrcPools.little.begin(), adrcPools.little.end(), cpuId) != adrcPools.little.end();
            if (inBig && adrcOut.shouldBoost) {
                nAdrcRisk = adrcOut.riskScore * 100.0f;
            } else if (inLittle && adrcOut.shouldBoost) {
                nAdrcRisk = -adrcOut.riskScore * 50.0f;
            } else if (!inBig && !inLittle && adrcOut.shouldBoost) {
                nAdrcRisk = adrcOut.riskScore * 30.0f;
            } else {
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

int computeScore(int cpuId, const ThreadInfo& ti, int knownFromCpu, const ScoringContext& ctx) {
    if (ti.role == ROLE_S0_SYSTEM) return 0;
    if (cpuId < 0 || cpuId >= (int)g_cpuLoadHistory.size()) return 0;

    bool usePrecomputed = g_precomputedValid.load() &&
                          (cpuId >= 0 && cpuId < (int)g_precomputedCpuBaseScore.size());
    float capMul = 1.0f, latMul = 1.0f, thermMul = 1.0f, contMul = 1.0f, powMul = 1.0f;
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

    int baseScore;
    if (usePrecomputed) {
        baseScore = g_precomputedCpuBaseScore[cpuId];
    } else {
        int temp = ctx.getTemp(cpuId);
        int thermPenalty = 0, thermBoost = 0;
        if (temp != 0) {
            if (temp > thermal::kHighTempThreshold) thermPenalty = (temp - thermal::kHighTempThreshold) * thermal::kPenaltyPerDegree;
            if (temp < thermal::kLowTempThreshold) thermBoost = (thermal::kLowTempThreshold - temp) * thermal::kBoostPerDegree;
        }
        int thermalVal = thermBoost - thermPenalty;
        int util = ctx.getUtil(cpuId);
        int contVal;
        if (util > contention::kHighUtilThreshold) contVal = contention::kHighUtilPenalty;
        else if (util > contention::kMidUtilThreshold) contVal = contention::kMidUtilPenalty;
        else if (util > contention::kLowUtilThreshold) contVal = contention::kLowUtilPenalty;
        else contVal = contention::kIdleBoost;
        baseScore = thermalVal + contVal - predictedLoadPenalty(cpuId);
    }

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

    int rawFrame = 0;
    if (ctx.cfg && ctx.cfg->frameAware && g_frameCritical.load(std::memory_order_relaxed)) {
        if (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME)
            rawFrame = frame_score::kCriticalBoost;
    }

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

    int rawMigration;
    if (knownFromCpu >= 0) {
        cpu_set_t affMask;
        CPU_ZERO(&affMask);
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
                nAdrcRisk = adrcOut.riskScore * 30.0f;
            } else {
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

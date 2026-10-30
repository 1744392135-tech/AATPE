#include "aatped.h"
#include "aatpe_diag.h"
#include "scoring_interface.h"
#include "c4_freezer.h"
#include "webui_http.h"
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
using namespace aatpe;
using namespace aatpe_ml;

static void atomicWriteJson(const std::string& path, const std::string& content) {
    std::string tmpPath = path + ".tmp";
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        int ffd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (ffd >= 0) {
            size_t off = 0;
            while (off < content.size()) {
                ssize_t n = write(ffd, content.c_str() + off, content.size() - off);
                if (n < 0) { if (errno == EINTR) continue; break; }
                off += (size_t)n;
            }
            close(ffd);
        }
        return;
    }
    size_t written = 0;
    const char* data = content.c_str();
    size_t len = content.size();
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        written += (size_t)n;
    }
    if (fsync(fd) != 0) {
        close(fd); unlink(tmpPath.c_str());
        int ffd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (ffd >= 0) {
            size_t off = 0;
            while (off < content.size()) {
                ssize_t n = write(ffd, content.c_str() + off, content.size() - off);
                if (n < 0) { if (errno == EINTR) continue; break; }
                off += (size_t)n;
            }
            close(ffd);
        }
        return;
    }
    close(fd);
    if (written == len) {
        if (rename(tmpPath.c_str(), path.c_str()) != 0) {
            char ebuf[96];
            LOG_WARN("atomicWriteJson: rename(%s) failed: %s; direct-write fallback",
                     path.c_str(), aatpeStrError(errno, ebuf, sizeof(ebuf)));
            unlink(tmpPath.c_str());
            int ffd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
            if (ffd >= 0) {
                size_t off = 0;
                while (off < content.size()) {
                    ssize_t n = write(ffd, content.c_str() + off, content.size() - off);
                    if (n < 0) { if (errno == EINTR) continue; break; }
                    off += (size_t)n;
                }
                close(ffd);
            }
        }
    }
    else {
        unlink(tmpPath.c_str());
        int ffd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (ffd >= 0) {
            size_t off = 0;
            while (off < content.size()) {
                ssize_t n = write(ffd, content.c_str() + off, content.size() - off);
                if (n < 0) { if (errno == EINTR) continue; break; }
                off += (size_t)n;
            }
            close(ffd);
        }
    }
}

std::vector<WebuiDecision> g_decisionRing;
std::mutex g_decisionRingMutex;
static const size_t kDecisionRingMax = 20;

void recordWebuiDecision(int tid, const std::string& name, const std::string& role,
                                  int fromCpu, int toCpu, int score, int confidence,
                                  const std::string& type) {
    std::lock_guard<std::mutex> lock(g_decisionRingMutex);
    WebuiDecision d;
    d.timestamp = time(nullptr); d.tid = tid; d.name = name; d.role = role;
    d.from_cpu = fromCpu; d.to_cpu = toCpu; d.score = score; d.confidence = confidence; d.type = type;
    g_decisionRing.push_back(d);
    if (g_decisionRing.size() > kDecisionRingMax) g_decisionRing.erase(g_decisionRing.begin());
}

std::atomic<int> g_explainTid{-1};

const char* roleDisplayName(ThreadRole role) {
    switch (role) {
        case ROLE_S0_SYSTEM: return "系统线程"; case ROLE_S1_UI: return "UI线程";
        case ROLE_S2_RENDER: return "渲染线程"; case ROLE_S3_GAME: return "游戏线程";
        case ROLE_S4_THROUGHPUT: return "吞吐线程"; case ROLE_S5_BACKGROUND: return "后台线程";
        default: return "未知";
    }
}

static std::string fmtWeightJson(float v) {
    if (!std::isfinite(v)) return "0";
    float nv = v + 0.0f;
    if (nv != 0.0f && std::abs(nv) < 0.0005f) nv = 0.0f;
    if (nv == 0.0f) nv = 0.0f;
    return std::to_string(nv);
}

static std::string fmtJsonFloat(double v, int precision) {
    if (!std::isfinite(v)) return "0";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return std::string(buf);
}

static std::mutex g_webuiScoreCacheMutex;
static std::unordered_map<int, std::pair<long long,int>> g_webuiScoreCache;
static constexpr long long kWebuiScoreTtlMs = 2000;
static constexpr size_t kScoreCacheMax = 1024;

void writeThreadsJson() {
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::vector<std::pair<int, ThreadInfo>> threadsSnap;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        threadsSnap.reserve(g_threads.size());
        for (const auto& kv : g_threads) threadsSnap.push_back(kv);
    }
    int cpuN = g_cpuCount > 0 ? g_cpuCount : 1;
    std::vector<int> utilSnap(cpuN, -1);
    {
        std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
        for (const auto& kv : g_cpuMetrics) {
            if (kv.first >= 0 && kv.first < cpuN) utilSnap[kv.first] = kv.second.util;
        }
    }
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    ScoringContext ctx = buildScoringContext(cfgSnap);
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::string json = "{\"timestamp\":" + std::to_string((long long)time(nullptr)) + ",\"generation\":" + std::to_string(gen) + ",\"threads\":[";
    json.reserve(16384 + threadsSnap.size() * 128);
    bool first = true;
    for (const auto& kv : threadsSnap) {
        const int tid = kv.first; const ThreadInfo& ti = kv.second;
        if (!first) json += ",";
        first = false;
        int cpuUsage = 0;
        if (ti.currentCpu >= 0 && ti.currentCpu < cpuN) cpuUsage = utilSnap[ti.currentCpu];
        int score = 0;
        {
            std::lock_guard<std::mutex> sl(g_webuiScoreCacheMutex);
            auto it = g_webuiScoreCache.find(tid);
            if (it != g_webuiScoreCache.end() && it->second.first > nowMs) {
                score = it->second.second;
            } else {
                score = computeScore(ti.currentCpu, ti, ti.currentCpu, ctx);
                for (auto cit = g_webuiScoreCache.begin(); cit != g_webuiScoreCache.end(); ) {
                    if (cit->second.first <= nowMs) cit = g_webuiScoreCache.erase(cit);
                    else ++cit;
                }
                while (g_webuiScoreCache.size() >= kScoreCacheMax) {
                    auto oldest = g_webuiScoreCache.begin();
                    for (auto cit = g_webuiScoreCache.begin(); cit != g_webuiScoreCache.end(); ++cit) {
                        if (cit->second.first < oldest->second.first) oldest = cit;
                    }
                    g_webuiScoreCache.erase(oldest);
                }
                g_webuiScoreCache[tid] = {nowMs + kWebuiScoreTtlMs, score};
            }
        }
        json += "{\"tid\":" + std::to_string(tid) + ",\"name\":\"" + jsonEscape(ti.tname) + "\",\"role\":\"" + role_names::getName((int)ti.role) + "\",\"role_name\":\"" + roleDisplayName(ti.role) + "\",\"current_cpu\":" + std::to_string(ti.currentCpu) + ",\"cpu_usage\":" + std::to_string(cpuUsage) + ",\"score\":" + std::to_string(score) + ",\"confidence\":0}";
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/threads.json", json);
}

void writeDecisionsJson() {
    std::lock_guard<std::mutex> lock(g_decisionRingMutex);
    std::string json = "{\"timestamp\":" + std::to_string((long long)time(nullptr)) + ",\"decisions\":[";
    for (int i = (int)g_decisionRing.size() - 1; i >= 0; i--) {
        if (i < (int)g_decisionRing.size() - 1) json += ",";
        const WebuiDecision& d = g_decisionRing[i];
        json += "{\"timestamp\":" + std::to_string((long long)d.timestamp) + ",\"tid\":" + std::to_string(d.tid) + ",\"name\":\"" + jsonEscape(d.name) + "\",\"role\":\"" + jsonEscape(d.role) + "\",\"from_cpu\":" + std::to_string(d.from_cpu) + ",\"to_cpu\":" + std::to_string(d.to_cpu) + ",\"score\":" + std::to_string(d.score) + ",\"confidence\":" + std::to_string(d.confidence) + ",\"type\":\"" + jsonEscape(d.type) + "\"}";
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/decisions.json", json);
}

void writeC4Json() {
    std::string inner = aatpe::c4freezer::c4FreezerStateJson();
    std::string json = "{\"timestamp\":" +
        std::to_string((long long)time(nullptr)) + ",\"c4\":" + inner + "}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/c4.json", json);
}

void writeExplainJson() {
    int targetTid = g_explainTid.load();
    std::vector<std::pair<int, ThreadInfo>> threadsSnap;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        threadsSnap.reserve(g_threads.size());
        for (const auto& kv : g_threads) threadsSnap.push_back(kv);
    }
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    ScoringContext ctx = buildScoringContext(cfgSnap);
    std::vector<int> allCpuSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        allCpuSnap = g_allCpus;
    }

    auto snapMetric = [&ctx](int cpuId, int which) -> int {
        auto it = ctx.cpuMetrics.find(cpuId);
        if (it == ctx.cpuMetrics.end()) return 0;
        switch (which) {
            case 0: return it->second.util;
            case 1: return it->second.temp;
            case 2: return it->second.capacity;
            default: return 0;
        }
    };

    const ThreadInfo* ti = nullptr;
    if (targetTid < 0) {
        int maxUsage = -1;
        for (const auto& kv : threadsSnap) {
            int usage = snapMetric(kv.second.currentCpu, 0);
            if (usage > maxUsage) { maxUsage = usage; ti = &kv.second; }
        }
    } else {
        for (const auto& kv : threadsSnap) {
            if (kv.first == targetTid) { ti = &kv.second; break; }
        }
    }
    if (!ti) { atomicWriteJson(std::string(WEBUI_DIR)+"/explain.json", "{\"timestamp\":"+std::to_string((long long)time(nullptr))+",\"thread\":null,\"cpus\":[]}"); return; }
    ThreadRole role = ti->role;
    std::string json = "{\"timestamp\":" + std::to_string((long long)time(nullptr)) + ",\"thread\":{\"tid\":" + std::to_string(ti->tid) + ",\"name\":\"" + jsonEscape(ti->tname) + "\",\"role\":\"" + role_names::getName((int)role) + "\",\"role_name\":\"" + roleDisplayName(role) + "\",\"current_cpu\":" + std::to_string(ti->currentCpu) + "},\"cpus\":[";
    json.reserve(1024 + allCpuSnap.size() * 256);
    bool first = true;
    for (int cpuId : allCpuSnap) {
        if (!first) json += ",";
        first = false;
        int cap = snapMetric(cpuId, 2);
        int capacity_score;
        if (role == ROLE_S0_SYSTEM) capacity_score = 0;
        else if (role == ROLE_S5_BACKGROUND) capacity_score = capacity::kBackgroundInverseRef - cap;
        else capacity_score = cap;
        int latency_score;
        switch (role) {
            case ROLE_S0_SYSTEM: latency_score = latency::kScoreSystem; break;
            case ROLE_S1_UI:     latency_score = latency::kScoreUi; break;
            case ROLE_S2_RENDER: latency_score = latency::kScoreRender; break;
            case ROLE_S3_GAME:   latency_score = latency::kScoreGame; break;
            case ROLE_S4_THROUGHPUT: latency_score = latency::kScoreThroughput; break;
            case ROLE_S5_BACKGROUND: latency_score = latency::kScoreBackground; break;
            default: latency_score = 0; break;
        }
        int temp = snapMetric(cpuId, 1);
        int thermal_score = 0;
        if (temp != 0) {
            int penalty = 0, boost = 0;
            if (temp > thermal::kHighTempThreshold) penalty = (temp - thermal::kHighTempThreshold) * thermal::kPenaltyPerDegree;
            if (temp < thermal::kLowTempThreshold)  boost  = (thermal::kLowTempThreshold - temp) * thermal::kBoostPerDegree;
            thermal_score = boost - penalty;
        }
        int power_score = 0;
        if (g_currentProfileEnum == ProfileType::BATTERY) {
            if (role == ROLE_S5_BACKGROUND) power_score = power::kBatteryBgScore;
            else if (role == ROLE_S4_THROUGHPUT) power_score = power::kBatteryThroughputScore;
            else power_score = power::kBatteryOtherScore;
        } else if (g_currentProfileEnum == ProfileType::GAMING) {
            if (role==ROLE_S3_GAME||role==ROLE_S2_RENDER||role==ROLE_S1_UI) power_score = power::kgamingInteractiveScore;
            else if (role == ROLE_S5_BACKGROUND) power_score = power::kgamingBgScore;
            else power_score = power::kgamingOtherScore;
        } else if (g_currentProfileEnum == ProfileType::THERMAL) {
            if (role == ROLE_S5_BACKGROUND) power_score = power::kThermalBgScore;
            else power_score = power::kThermalOtherScore;
        }
        bool inLittle = std::find(ctx.pools.little.begin(), ctx.pools.little.end(), cpuId) != ctx.pools.little.end();
        bool inMid    = std::find(ctx.pools.mid.begin(),    ctx.pools.mid.end(),    cpuId) != ctx.pools.mid.end();
        bool inBig    = std::find(ctx.pools.big.begin(),    ctx.pools.big.end(),    cpuId) != ctx.pools.big.end();
        int pool_score = 0;
        switch (role) {
            case ROLE_S1_UI:
                pool_score = inBig ? pool_score::kUiBig : inMid ? pool_score::kUiMid : inLittle ? pool_score::kUiLittle : 0; break;
            case ROLE_S2_RENDER:
                pool_score = inBig ? pool_score::kRenderBig : inMid ? pool_score::kRenderMid : inLittle ? pool_score::kRenderLittle : 0; break;
            case ROLE_S3_GAME:
                pool_score = inBig ? pool_score::kGameBig : inMid ? pool_score::kGameMid : inLittle ? pool_score::kGameLittle : 0; break;
            case ROLE_S4_THROUGHPUT:
                pool_score = inBig ? pool_score::kThroughputBig : inMid ? pool_score::kThroughputMid : inLittle ? pool_score::kThroughputLittle : 0; break;
            case ROLE_S5_BACKGROUND:
                pool_score = inLittle ? pool_score::kBackgroundLittle : inMid ? pool_score::kBackgroundMid : inBig ? pool_score::kBackgroundBig : 0; break;
            default: pool_score = 0; break;
        }
        int util = snapMetric(cpuId, 0);
        int contention_penalty;
        if (util > contention::kHighUtilThreshold) contention_penalty = contention::kHighUtilPenalty;
        else if (util > contention::kMidUtilThreshold) contention_penalty = contention::kMidUtilPenalty;
        else if (util > contention::kLowUtilThreshold) contention_penalty = contention::kLowUtilPenalty;
        else contention_penalty = contention::kIdleBoost;
        int total_score = computeScore(cpuId, *ti, ti->currentCpu, ctx);
        json += "{\"cpu\":" + std::to_string(cpuId) + ",\"capacity_score\":" + std::to_string(capacity_score) + ",\"latency_score\":" + std::to_string(latency_score) + ",\"thermal_score\":" + std::to_string(thermal_score) + ",\"power_score\":" + std::to_string(power_score) + ",\"pool_score\":" + std::to_string(pool_score) + ",\"contention_penalty\":" + std::to_string(contention_penalty) + ",\"total_score\":" + std::to_string(total_score) + "}";
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/explain.json", json);
}

void writeModelJson() {
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::vector<int> littleSnap, midSnap, bigSnap, primeSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        littleSnap = g_littlePool;
        midSnap    = g_midPool;
        bigSnap    = g_bigPool;
        for (const auto& cl : clusters()) {
            if (cl.tier == CpuTier::PRIME) primeSnap = cl.cpus;
        }
    }
    std::lock_guard<std::mutex> lock(g_mlMutex);
    int poolCount = g_mlModel.getPoolCount();
    std::string json = "{\"timestamp\":" + std::to_string((long long)time(nullptr)) + ","
        "\"generation\":" + std::to_string(gen) + ","
        "\"total_samples\":" + std::to_string(g_mlModel.getTotalSamples()) + ","
        "\"funnel\":{\"pending_created\":" + std::to_string((long long)g_mlPendingCreated.load())
        + ",\"outcome_measured\":" + std::to_string((long long)g_mlOutcomeMeasured.load())
        + ",\"outcome_valid\":" + std::to_string((long long)g_mlOutcomeValid.load())
        + ",\"skip_sleeping\":" + std::to_string((long long)g_mlOutcomeSkipSleeping.load())
        + ",\"skip_saturated\":" + std::to_string((long long)g_mlOutcomeSkipSaturated.load())
        + ",\"skip_reset\":" + std::to_string((long long)g_mlOutcomeSkipReset.load())
        + ",\"skip_other\":" + std::to_string((long long)g_mlOutcomeSkipOther.load())
        + ",\"pending_expired\":" + std::to_string((long long)g_mlPendingExpired.load())
        + ",\"pending_tid_reused\":" + std::to_string((long long)g_mlPendingTidReused.load())
        + ",\"pending_skipped_existing\":" + std::to_string((long long)aatpe_ml::g_pendingSkippedExisting.load())
        + ",\"skip_update_thermal\":" + std::to_string((long long)aatpe_ml::g_mlUpdateSkipThermal.load())
        + ",\"skip_update_disabled\":" + std::to_string((long long)aatpe_ml::g_mlUpdateSkipDisabled.load())
        + ",\"skip_update_pool\":" + std::to_string((long long)aatpe_ml::g_droppedInvalidTargetPool.load())
        + ",\"skip_update_outcome\":" + std::to_string((long long)aatpe_ml::g_invalidOutcomeSkipped.load()) + "},"
        "\"alpha\":" + fmtJsonFloat((double)g_mlModel.getAlpha(), 4) + ","
        "\"learning_rate\":0,"
        "\"max_alpha\":" + std::to_string(cfgSnap.mlMaxAlpha) + ","
        "\"enabled\":" + (g_mlModel.isEnabled()?"true":"false") + ","
        "\"anomaly_latched\":" + (g_mlModel.isAnomalyLatched()?"true":"false") + ","
        "\"training_paused_by_thermal\":" + (aatpe_ml::g_mlTrainingPausedByThermal.load()?"true":"false") + ","
        "\"pool_count\":" + std::to_string(poolCount) + ","
        "\"cold_start_protection\":" + (cfgSnap.mlColdStartProtection?"true":"false") + ","
        "\"min_samples\":" + std::to_string(cfgSnap.mlMinSamples) + ","
        "\"training_target\":\"" + (cfgSnap.mlFrameTimeOutcome ? "frame_time+run_ratio" : "run_ratio_only") + "\","
        "\"frame_time_weight\":" + std::to_string(cfgSnap.mlFrameTimeWeight) + ","
        "\"frame_time_outcome\":{\"samples\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeSamples.load())
        + ",\"fallback\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeFallback.load())
        + ",\"improved\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeImproved.load())
        + ",\"degraded\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeDegraded.load())
        + ",\"gfxinfo_calls\":" + std::to_string((long long)aatpe_ml::g_ftGfxinfoCalls.load())
        + ",\"gfxinfo_cache_hits\":" + std::to_string((long long)aatpe_ml::g_ftGfxinfoCacheHits.load()) + "},"
        "\"pools\":[";
    const char* poolNames[] = {"LITTLE","MID","BIG","PRIME"};
    const std::vector<int>* poolCpuLists[] = {&littleSnap,&midSnap,&bigSnap,&primeSnap};
    bool first = true;
    for (int poolIdx = 0; poolIdx < poolCount; poolIdx++) {
        if (!first) json += ",";
        first = false;
        const bool knownPool = (poolIdx >= 0 && poolIdx < 4);
        PoolModel m;
        if (knownPool) {
            m = g_mlModel.getPoolModel(poolIdx);
        } else {
            for (int i = 0; i < ML_WEIGHT_COUNT; i++) m.weights[i] = 0.0f;
            m.learning_rate = 0.0f; m.sample_count = 0; m.alpha = 0.0f;
        }
        const char* pName = knownPool ? poolNames[poolIdx] : "UNKNOWN";
        const bool present = knownPool ? g_mlModel.isPoolPresent(poolIdx) : false;
        json += "{\"pool\":" + std::to_string(poolIdx) + ",\"name\":\"" + pName + "\",\"present\":" + (present?"true":"false") + ",\"cpus\":[";
        if (knownPool) { bool cf=true; for(int cpu:*poolCpuLists[poolIdx]){if(!cf)json+=",";cf=false;json+=std::to_string(cpu);} }
        json += "],\"weights\":[";
        for (int w=0;w<ML_WEIGHT_COUNT;w++){if(w>0)json+=",";json+=fmtWeightJson(m.weights[w]);}
        json += "],\"sample_count\":" + std::to_string(m.sample_count)
             + ",\"ready\":" + (knownPool ? (g_mlModel.isPoolReady(poolIdx)?"true":"false") : "false")
             + ",\"learning_rate\":" + std::to_string(m.learning_rate)
             + ",\"alpha\":" + fmtJsonFloat(knownPool ? (double)g_mlModel.getAlphaForPool(poolIdx) : 0.0, 4) + "}";
    }
    json += "],\"cpus\":[";
    bool cpuFirst = true;
    for (int poolIdx = 0; poolIdx < poolCount; poolIdx++) {
        PoolModel pm = g_mlModel.getPoolModel(poolIdx);
        if (poolIdx < 0 || poolIdx >= 4) continue;
        for (int cpu : *poolCpuLists[poolIdx]) {
            if (!cpuFirst) json += ",";
            cpuFirst = false;
            json += "{\"cpu\":" + std::to_string(cpu) + ",\"weights\":[";
            for (int w = 0; w < ML_WEIGHT_COUNT; w++) {
                if (w > 0) json += ",";
                json += fmtWeightJson(pm.weights[w]);
            }
            json += "],\"sample_count\":" + std::to_string(pm.sample_count)
                 + ",\"ready\":" + (g_mlModel.isPoolReady(poolIdx)?"true":"false") + "}";
        }
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/model.json", json);
}

void writeConfigJson() {
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::string json = "{";
    json += "\"generation\":" + std::to_string(gen) + ",";
    std::string curProfile = aatpe_profile::effectiveProfile();
    json += "\"profile\":\"" + jsonEscape(curProfile) + "\",";
    json += "\"migration_cost_threshold\":" + std::to_string(cfgSnap.migrationCostThreshold) + ",";
    json += "\"confidence_threshold\":" + std::to_string(cfgSnap.confidenceThreshold) + ",";
    json += "\"max_migrations_per_cycle\":" + std::to_string(cfgSnap.maxMigrationsPerCycle) + ",";
    json += "\"ml_learning_enabled\":" + std::string(cfgSnap.learningEnabled?"true":"false") + ",";
    json += "\"ml_max_alpha\":" + std::to_string(cfgSnap.mlMaxAlpha) + ",";
    json += "\"thermal_limit_cpu\":" + std::to_string(cfgSnap.thermalLimitCpu) + ",";
    json += "\"thermal_limit_skin\":" + std::to_string(cfgSnap.thermalLimitSkin) + ",";
    json += "\"thermal_limit_battery\":" + std::to_string(cfgSnap.thermalLimitBattery) + ",";
    json += "\"decision_interval_ms\":" + std::to_string(cfgSnap.decisionInterval) + ",";
    json += "\"thread_scan_interval_sec\":" + std::to_string(cfgSnap.threadScanInterval);
    json += "}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/config.json", json);
}

void writeSystemJson() {
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::vector<int> littleSnap, midSnap, bigSnap, primeSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        littleSnap = g_littlePool;
        midSnap    = g_midPool;
        bigSnap    = g_bigPool;
        for (const auto& cl : clusters()) {
            if (cl.tier == CpuTier::PRIME) primeSnap = cl.cpus;
        }
    }
    bool batteryAgedSnap;
    {
        std::lock_guard<std::mutex> battLock(g_batteryStateMutex);
        batteryAgedSnap = g_batteryAged;
    }
    int fps = readInt((std::string(STATE_DIR)+"/vsync_rate").c_str(),0);
    int frameCritical = readInt((std::string(STATE_DIR)+"/frame_critical").c_str(),0);
    int frameLatency = readInt((std::string(STATE_DIR)+"/sf_latency_avg").c_str(),0);
    int jankDetected = readInt((std::string(STATE_DIR)+"/jank_detected").c_str(),0);
    int safetyLatencyActive = readInt((std::string(STATE_DIR)+"/safety_latency_active").c_str(),0);
    int targetFrameTime = readInt((std::string(STATE_DIR)+"/target_frame_time").c_str(),0);
    int skinPressure = readInt((std::string(STATE_DIR)+"/thermal_skin_pressure").c_str(),0);
    int batteryPressure = readInt((std::string(STATE_DIR)+"/thermal_battery_pressure").c_str(),0);
    std::string apkStatus = readFile((std::string(STATE_DIR)+"/fg_apk_status").c_str());
    std::string apkAge = readFile((std::string(STATE_DIR)+"/fg_apk_age").c_str());
    std::string apkPath = readFile((std::string(STATE_DIR)+"/fg_apk_path").c_str());
    int cpuTemp = readInt((std::string(STATE_DIR)+"/thermal_cpu_max").c_str(),0);
    int skinTemp = readInt((std::string(STATE_DIR)+"/thermal_skin").c_str(),-1);
    std::string skinJson = (skinTemp >= 0) ? std::to_string(skinTemp) : "null";
    int batteryTemp = readInt((std::string(STATE_DIR)+"/thermal_battery").c_str(),-1);
    std::string batteryJson = (batteryTemp >= 0) ? std::to_string(batteryTemp) : "null";
    int thermalEmergency = readInt((std::string(STATE_DIR)+"/thermal_emergency").c_str(),0);
    int loadAvg = readInt((std::string(STATE_DIR)+"/system_load").c_str(),0);
    int uptime = readInt((std::string(STATE_DIR)+"/daemon_uptime").c_str(),0);
    std::string httpState = readFile((std::string(STATE_DIR)+"/http_state").c_str());
    if (httpState != "on" && httpState != "disabled_by_config" &&
        httpState != "failed_token" && httpState != "ports_busy") {
        httpState = "unknown";
    }
    int httpPortState = readInt((std::string(STATE_DIR)+"/http_port").c_str(), 0);
    int ebpfActive = readInt((std::string(STATE_DIR)+"/ebpf_active").c_str(),0);
    std::string offSwitch = readFile((std::string(STATE_DIR)+"/ebpf_offset_status_switch").c_str());
    std::string offFork   = readFile((std::string(STATE_DIR)+"/ebpf_offset_status_fork").c_str());
    std::string offExec   = readFile((std::string(STATE_DIR)+"/ebpf_offset_status_exec").c_str());
    std::string offExit   = readFile((std::string(STATE_DIR)+"/ebpf_offset_status_exit").c_str());
    long offFiltered = readInt((std::string(STATE_DIR)+"/ebpf_offset_filtered").c_str(), 0);
    if (offSwitch.empty()) offSwitch = "unknown";
    if (offFork.empty())   offFork   = "unknown";
    if (offExec.empty())   offExec   = "unknown";
    if (offExit.empty())   offExit   = "unknown";
    long onlineCpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (onlineCpus <= 0) onlineCpus = g_cpuCount;

    aatpe::diag::SystemPressure psi = aatpe::diag::getSystemPressureSnapshot();
    auto psiF = [](float v) -> std::string {
        if (!std::isfinite(v)) return "0";
        return std::to_string(v);
    };
    std::string psiBlock =
        "\"psi\":{\"available\":" + std::string(psi.psiAvailable ? "true" : "false") +
        ",\"cpu\":{\"some10\":" + psiF(psi.psiCpuSomePct) +
        ",\"some60\":" + psiF(psi.psiCpuSome60Pct) +
        ",\"some300\":" + psiF(psi.psiCpuSome300Pct) +
        ",\"total_usec\":" + std::to_string(psi.psiCpuTotalUsec) + "}," +
        "\"io\":{\"some10\":" + psiF(psi.psiIoSomePct) +
        ",\"some60\":" + psiF(psi.psiIoSome60Pct) +
        ",\"some300\":" + psiF(psi.psiIoSome300Pct) + "}," +
        "\"mem\":{\"some10\":" + psiF(psi.psiMemSomePct) +
        ",\"some60\":" + psiF(psi.psiMemSome60Pct) +
        ",\"some300\":" + psiF(psi.psiMemSome300Pct) + "}},";

    std::string json = "{\"timestamp\":" + std::to_string((long long)time(nullptr)) + ","
        "\"generation\":" + std::to_string(gen) + ","
        "\"fps\":" + std::to_string(fps) + ","
        "\"frame_critical\":" + (frameCritical?"true":"false") + ","
        "\"frame_latency\":" + std::to_string(frameLatency) + ","
        "\"jank_detected\":" + (jankDetected?"true":"false") + ","
        "\"safety_latency_active\":" + (safetyLatencyActive?"true":"false") + ","
        "\"target_frame_time\":" + std::to_string(targetFrameTime) + ","
        "\"apk_diagnostic\":{\"status\":\"" + jsonEscape(apkStatus) + "\",\"age\":\"" + jsonEscape(apkAge) + "\",\"path\":\"" + jsonEscape(apkPath) + "\"},"
        "\"thermal\":{\"cpu\":" + std::to_string(cpuTemp) + ",\"skin\":" + skinJson + ",\"battery\":" + batteryJson + ",\"emergency\":" + std::to_string(thermalEmergency) + ",\"skin_pressure\":" + std::to_string(skinPressure) + ",\"battery_pressure\":" + std::to_string(batteryPressure) + "}," +
        psiBlock +
        "\"thermal_limit\":" + std::to_string(cfgSnap.thermalLimitCpu) + ","
        "\"thermal_limit_cpu\":" + std::to_string(cfgSnap.thermalLimitCpu) + ","
        "\"thermal_limit_skin\":" + std::to_string(cfgSnap.thermalLimitSkin) + ","
        "\"thermal_limit_battery\":" + std::to_string(cfgSnap.thermalLimitBattery) + ","
        "\"loadavg\":" + std::to_string(loadAvg) + ","
        "\"online_cpus\":" + std::to_string((long long)onlineCpus) + ","
        "\"cpu_topology\":{\"little\":[";
    for (size_t i=0;i<littleSnap.size();i++){if(i>0)json+=",";json+=std::to_string(littleSnap[i]);}
    json += "],\"mid\":[";
    for (size_t i=0;i<midSnap.size();i++){if(i>0)json+=",";json+=std::to_string(midSnap[i]);}
    json += "],\"big\":[";
    for (size_t i=0;i<bigSnap.size();i++){if(i>0)json+=",";json+=std::to_string(bigSnap[i]);}
    json += "],\"prime\":[";
    for (size_t i=0;i<primeSnap.size();i++){if(i>0)json+=",";json+=std::to_string(primeSnap[i]);}
    json += "]},";
    int logLevel = readInt((std::string(STATE_DIR)+"/log_level").c_str(),2);
    std::string controlMode = readFile((std::string(STATE_DIR)+"/control_mode").c_str());
    if (controlMode.empty()) controlMode = "assist";
    std::string predMae = readFile((std::string(STATE_DIR)+"/prediction_mae").c_str());
    std::string predConf = readFile((std::string(STATE_DIR)+"/prediction_confidence").c_str());
    std::string predMaeJson;
    if (predMae.empty()) {
        predMaeJson = "null";
    } else {
        predMaeJson = "\"" + jsonEscape(predMae) + "\"";
    }
    if (predConf.empty()) predConf = "0.500000";
    json += "\"control_mode\":\"" + jsonEscape(controlMode) + "\","
        "\"log_level\":" + std::to_string(logLevel) + ","
        "\"daemon_uptime\":" + std::to_string(uptime) + ","
        "\"http_state\":\"" + jsonEscape(httpState) + "\","
        "\"http_port\":" + std::to_string(httpPortState) + ","
        "\"event_inotify_active\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_inotify_active").c_str(),0)) + ","
        "\"event_jank_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_jank_count").c_str(),0)) + ","
        "\"event_thermal_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_thermal_count").c_str(),0)) + ","
        "\"event_freq_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_freq_count").c_str(),0)) + ","
        "\"event_hotplug_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_hotplug_count").c_str(),0)) + ","
        "\"event_app_switch_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_app_switch_count").c_str(),0)) + ","
        "\"event_total\":" + std::to_string(readInt((std::string(STATE_DIR)+"/event_total").c_str(),0)) + ","
        "\"vsync_enabled\":" + std::to_string(readInt((std::string(STATE_DIR)+"/vsync_enabled").c_str(),0)) + ","
        "\"vsync_refresh_rate\":" + std::to_string(readInt((std::string(STATE_DIR)+"/vsync_refresh_rate").c_str(),60)) + ","
        "\"vsync_frame_interval\":" + std::to_string(readInt((std::string(STATE_DIR)+"/vsync_frame_interval").c_str(),16)) + ","
        "\"vsync_decisions\":" + std::to_string(readInt((std::string(STATE_DIR)+"/vsync_decisions").c_str(),0)) + ","
        "\"vsync_migrations\":" + std::to_string(readInt((std::string(STATE_DIR)+"/vsync_migrations").c_str(),0)) + ","
        "\"vsync_overruns\":" + std::to_string(readInt((std::string(STATE_DIR)+"/vsync_overruns").c_str(),0)) + ","
        "\"prediction_mae\":" + predMaeJson + ","
        "\"prediction_confidence\":\"" + jsonEscape(predConf) + "\","
        "\"prediction_proactive_migrations\":" + std::to_string(readInt((std::string(STATE_DIR)+"/prediction_proactive_migrations").c_str(),0)) + ","
        "\"battery_aging\":{\"enabled\":" + (cfgSnap.batteryAgingProtection?"true":"false") + ",\"aged\":" + (batteryAgedSnap?"true":"false") + "},"
        "\"ebpf\":{\"active\":" + (ebpfActive?"true":"false") +
            ",\"offset_switch\":\"" + jsonEscape(offSwitch) + "\"" +
            ",\"offset_fork\":\""   + jsonEscape(offFork)   + "\"" +
            ",\"offset_exec\":\""   + jsonEscape(offExec)   + "\"" +
            ",\"offset_exit\":\""   + jsonEscape(offExit)   + "\"" +
            ",\"offset_filtered\":" + std::to_string(offFiltered) + "},"
        "\"federated\":{\"rounds\":" + std::to_string(g_federatedRounds.load(std::memory_order_relaxed)) + ",\"exports\":" + std::to_string(g_federatedExports.load(std::memory_order_relaxed)) + ",\"imports\":" + std::to_string(g_federatedImports.load(std::memory_order_relaxed)) + "},"
        "\"uevent\":{\"active\":" + (readInt((std::string(STATE_DIR)+"/event_uevent_active").c_str(),0)?"true":"false") + "},"
        + ([](){
            aatpe_ml::FrameTimeOutcome ft = getCurrentFrameTimeSnapshot();
            char buf[256];
            snprintf(buf, sizeof(buf),
                "\"frame_time_snapshot\":{\"avg_ms\":%s,\"p95_ms\":%s,\"p99_ms\":%s,"
                "\"jank_count\":%d,\"sample_count\":%d,\"valid\":%s}",
                fmtJsonFloat(ft.frame_time_avg, 2).c_str(),
                fmtJsonFloat(ft.frame_time_p95, 2).c_str(),
                fmtJsonFloat(ft.frame_time_p99, 2).c_str(),
                ft.jank_count, ft.sample_count, ft.valid ? "true" : "false");
            return std::string(buf);
        })() + ","
        + ([](){
            long long cnt = g_fusionWeightAccum.count.load(std::memory_order_relaxed);
            if (cnt <= 0) return std::string("\"decision_fusion\":{\"count\":0,\"heuristic\":0,\"ml\":0,\"hybrid\":0,\"history\":0}");
            double sh = g_fusionWeightAccum.sum_heuristic.load(std::memory_order_relaxed);
            double sm = g_fusionWeightAccum.sum_ml.load(std::memory_order_relaxed);
            double shy = g_fusionWeightAccum.sum_hybrid.load(std::memory_order_relaxed);
            double shs = g_fusionWeightAccum.sum_history.load(std::memory_order_relaxed);
            char buf[256];
            snprintf(buf, sizeof(buf), "\"decision_fusion\":{\"count\":%lld,\"heuristic\":%s,\"ml\":%s,\"hybrid\":%s,\"history\":%s}",
                     cnt,
                     fmtJsonFloat(sh/(double)cnt, 4).c_str(),
                     fmtJsonFloat(sm/(double)cnt, 4).c_str(),
                     fmtJsonFloat(shy/(double)cnt, 4).c_str(),
                     fmtJsonFloat(shs/(double)cnt, 4).c_str());
            return std::string(buf);
        })()
        + ",\"v42\":{\"ml_weight_degraded_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/ml_weight_degraded_count").c_str(),0))
        + ",\"cold_start_rejected\":" + std::to_string(readInt((std::string(STATE_DIR)+"/cold_start_rejected").c_str(),0))
        + ",\"sf_thread_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/sf_thread_count").c_str(),0))
        + ",\"schedtune_boost_current\":" + std::to_string(readInt((std::string(STATE_DIR)+"/schedtune_boost_current").c_str(),0))
        + ",\"autocal_ratio\":" + ([](){
            char b[32]; snprintf(b, sizeof(b), "%.3f", (double)readInt((std::string(STATE_DIR)+"/autocal_ratio").c_str(),0)/1000.0);
            return std::string(b);
          })()
        + ",\"thermal_headroom_pct\":" + std::to_string(readInt((std::string(STATE_DIR)+"/thermal_headroom_pct").c_str(),0))
        + "}}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/system.json", json);
}

void writePerCpuJson() {
    std::vector<int> littleSnap, midSnap, bigSnap, allCpuSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        littleSnap = g_littlePool;
        midSnap    = g_midPool;
        bigSnap    = g_bigPool;
        allCpuSnap = g_allCpus;
    }
    std::map<int, CpuMetrics> metricsSnap;
    {
        std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
        metricsSnap = g_cpuMetrics;
    }

    std::unordered_map<int,std::string> cpuTypeMap;
    for (int c:littleSnap) cpuTypeMap[c]="little";
    for (int c:midSnap)    cpuTypeMap[c]="mid";
    for (int c:bigSnap)    cpuTypeMap[c]="big";
    std::string json = "{\"cpus\":[";
    bool first = true;
    size_t emitted = 0;
    for (int cpuId:allCpuSnap) {
        auto it = metricsSnap.find(cpuId);
        if (it==metricsSnap.end()) continue;
        if (!first) json+=",";
        first=false;
        emitted++;
        const CpuMetrics& m = it->second;
        auto typeIt = cpuTypeMap.find(cpuId);
        std::string cpuType = (typeIt!=cpuTypeMap.end())?typeIt->second:"unknown";
        json += "{\"cpu\":"+std::to_string(cpuId)+",\"type\":\""+cpuType+"\",\"util\":"+std::to_string(m.util)+",\"cur_freq\":"+std::to_string(m.curFreq)+",\"freq_pct\":"+std::to_string(m.freqPct)+",\"temp\":"+std::to_string(m.temp)+",\"capacity\":"+std::to_string(m.capacity)+"}";
    }
    std::string degraded;
    if (emitted == 0) degraded = allCpuSnap.empty() ? "no-topology" : "no-cpu-metrics";
    else              degraded = "ok";
    json += "],\"count\":" + std::to_string(emitted)
          + ",\"degraded\":\"" + degraded + "\"}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/percpu.json", json);
}

void writeLogsJson() {
    static const char* logPaths[] = {
        "/dev/aatpe/aatpe.log",
        "/data/adb/modules/aatpe/aatpe.log",
        "/data/adb/ksu/modules/aatpe/aatpe.log",
        "/data/adb/ap/modules/aatpe/aatpe.log",
        "/data/adb/modules_update/aatpe/aatpe.log",
        nullptr
    };
    const int kMaxLines = 100;
    const long long kReadBytes = 65536;

    std::string json = "{\"lines\":[";
    int fd = -1;
    for (int i = 0; logPaths[i]; i++) {
        fd = open(logPaths[i], O_RDONLY | O_CLOEXEC);
        if (fd >= 0) break;
    }
    if (fd < 0) {
        json += "]}";
        atomicWriteJson(std::string(WEBUI_DIR) + "/logs.json", json);
        return;
    }

    long long sz = lseek(fd, 0, SEEK_END);
    long long startOff = 0;
    if (sz > kReadBytes) startOff = sz - kReadBytes;
    lseek(fd, startOff, SEEK_SET);

    std::vector<char> buf(65536 + 4096);
    ssize_t nread = read(fd, buf.data(), buf.size());
    close(fd);
    if (nread <= 0) {
        json += "]}";
        atomicWriteJson(std::string(WEBUI_DIR) + "/logs.json", json);
        return;
    }
    std::string window(buf.data(), (size_t)nread);
    size_t firstLineStart = 0;
    if (startOff > 0) {
        size_t nl = window.find('\n');
        if (nl != std::string::npos) firstLineStart = nl + 1;
    }
    std::vector<std::string> lines;
    size_t pos = firstLineStart;
    while (pos < window.size()) {
        size_t nl = window.find('\n', pos);
        if (nl == std::string::npos) {
            std::string tail = window.substr(pos);
            if (!tail.empty()) lines.push_back(tail);
            break;
        }
        std::string line = window.substr(pos, nl - pos);
        while (!line.empty() && (line.back()=='\r')) line.pop_back();
        lines.push_back(line);
        pos = nl + 1;
    }

    size_t start = 0;
    if (lines.size() > (size_t)kMaxLines)
        start = lines.size() - (size_t)kMaxLines;

    bool first = true;
    for (size_t i = start; i < lines.size(); i++) {
        if (!lines[i].empty()) {
            if (!first) json += ",";
            first = false;
            json += "\"" + jsonEscape(lines[i]) + "\"";
        }
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/logs.json", json);
}

std::string aatpe_buildFrameAppsJson() {
    std::vector<AatpePerAppFrameStat> out;
    std::string currentPkg;
    aatpe_getPerAppFrameStats(out, currentPkg);

    std::string src = readFile((std::string(STATE_DIR) + "/frame_source").c_str());
    std::string rsn = readFile((std::string(STATE_DIR) + "/frame_unavailable_reason").c_str());
    std::string body = "{\"current\":\"" + jsonEscape(currentPkg) +
                       "\",\"source\":\"" + jsonEscape(src) +
                       "\",\"reason\":\"" + jsonEscape(rsn) + "\",\"apps\":[";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) body += ",";
        const AatpePerAppFrameStat& a = out[i];
        body += "{";
        body += "\"pkg\":\"" + jsonEscape(a.pkg) + "\",";
        body += "\"fps\":" + std::to_string(a.fps) + ",";
        body += "\"ft_avg_ms\":" + std::to_string(a.ft_avg_ms) + ",";
        body += "\"ft_p50\":" + std::to_string(a.ft_p50) + ",";
        body += "\"ft_p95\":" + std::to_string(a.ft_p95) + ",";
        body += "\"ft_p99\":" + std::to_string(a.ft_p99) + ",";
        body += "\"jank\":" + std::to_string(a.jank) + ",";
        body += "\"dropped\":" + std::to_string(a.dropped) + ",";
        body += "\"samples\":" + std::to_string(a.samples) + ",";
        body += "\"total_frames\":" + std::to_string(a.total_frames) + ",";
        body += "\"missed_vsyncs\":" + std::to_string(a.missed_vsyncs) + ",";
        body += "\"active_ms\":" + std::to_string(a.active_ms) + ",";
        body += "\"last_update_ms\":" + std::to_string(a.last_update_ms) + ",";
        body += "\"target_fps\":" + (a.target_fps > 0.01 ? std::to_string(a.target_fps) : std::string("null"));
        body += ",\"reason\":\"" + jsonEscape(a.reason) + "\"";
        body += ",\"frame_source\":\"" + jsonEscape(a.frameSource) + "\"";
        body += ",\"current\":" + std::string(a.isCurrent ? "true" : "false");
        body += ",\"last_seen_ms\":" + std::to_string(a.lastSeenMs);
        body += ",\"last_seen_epoch_ms\":" + std::to_string(a.lastSeenEpochMs);
        body += ",\"label\":null";
        body += "}";
    }
    body += "]}";
    return body;
}

// AATPE - WebUI JSON Output
#include "aatped.h"
#include "aatpe_diag.h"          // v4.2.2 P3-1: PSI avg10/60/300 three-tier snapshot
#include "scoring_interface.h"   // P0-5: ScoringContext / buildScoringContext (snapshot locks once)
#include "c4_freezer.h"          // v4.4.6: read-only C4 freezer state for WebUI
#include "webui_http.h"          // F-A6-08: canonical inline jsonEscape (single impl)
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
using namespace aatpe;
using namespace aatpe_ml;

// fix52 P2-23: Atomic JSON file write (tmp + fsync + rename).
// P2: tmp opened 0600 — these files carry thread names, TIDs and decisions.
static void atomicWriteJson(const std::string& path, const std::string& content) {
    std::string tmpPath = path + ".tmp";
    int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        // Issue-18: fallback must also use O_NOFOLLOW on the final path to avoid
        // following a symlink planted at the destination.
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
        if (n == 0) break;  // P1 round7: avoid infinite loop on zero write
        written += (size_t)n;
    }
    // fix105 P3-5: Check fsync return value; fall back on failure.
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
        // H8: check rename()==0. On failure log errno via aatpeStrError, clean the
        // tmp, and fall back to a direct O_NOFOLLOW write so state is never silently
        // dropped. Normal (rename succeeds) path is byte-for-byte unchanged.
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
// v94 fix33 (verified): g_decisionRing is a bounded ring. recordWebuiDecision
// pushes on and, once size exceeds kDecisionRingMax, evicts the OLDEST entry
// (erase begin()) so the vector never grows past kDecisionRingMax+1 = 21. Both
// the writer here and the JSON serializer take g_decisionRingMutex, so this is
// race-free. With a fixed 20-entry window the O(k) erase(begin()) cost is
// negligible; a writeIndex ring would be marginally faster but is unnecessary
// at this size.
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

// F-A6-08: jsonEscape() is the single inline canonical implementation in
// webui_http.h (included above). The local copy was removed so the escaping rules
// shared by HTTP responses and state.json cannot drift.

// v66-fix: Normalize negative zero and tiny values for weight JSON output.
// std::to_string(-0.0f) produces "-0.000000"; +0.0f converts -0 to +0.
// Tiny negatives (e.g. -0.0001) would round to "-0.000" in WebUI .toFixed(3),
// so clamp abs < 0.0005 to exactly 0.
// P2-8: also guard NaN/Inf — std::to_string(nan) emits "nan" which is illegal
// JSON and breaks JSON.parse() in the WebUI.
static std::string fmtWeightJson(float v) {
    if (!std::isfinite(v)) return "0";
    float nv = v + 0.0f;
    if (nv != 0.0f && std::abs(nv) < 0.0005f) nv = 0.0f;
    if (nv == 0.0f) nv = 0.0f; // ensure +0, not -0
    return std::to_string(nv);
}

// P2-8: format a double for JSON with %.Nf, substituting "0" for non-finite
// (NaN/Inf) values so we never emit illegal JSON tokens.
static std::string fmtJsonFloat(double v, int precision) {
    if (!std::isfinite(v)) return "0";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return std::string(buf);
}

// P0-4: per-tid display-score cache with a short TTL. writeThreadsJson used to
// call the legacy computeScore(currentCpu, ti) once per tracked thread (~500),
// and each call re-took g_cpuMetricsMutex / g_topologyRwMutex / g_historyMutex.
// For a pure display number that is wasteful. We now build ONE ScoringContext per
// refresh (2 lock acquisitions total) and reuse it, plus cache the per-tid result
// for kScoreCacheTtlMs so consecutive refreshes reuse it.
static std::mutex g_webuiScoreCacheMutex;
static std::unordered_map<int, std::pair<long long,int>> g_webuiScoreCache; // tid -> (expiryMs, score)
static constexpr long long kWebuiScoreTtlMs = 2000;
// HTTP-02: bound the cache. Android tids cycle constantly (process start/stop);
// without an eviction the map grows for the entire daemon uptime. We evict expired
// entries opportunistically, and if the map still exceeds kScoreCacheMax, evict
// the oldest-expiry entries until it fits.
static constexpr size_t kScoreCacheMax = 1024;

void writeThreadsJson() {
    // v94 改进包B 任务1: snapshot-then-serialize (see history below).
    // P0-4: build a single ScoringContext (snapshot pools + cpuMetrics + cfg once),
    // then score every thread from that snapshot instead of re-locking per thread.
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::vector<std::pair<int, ThreadInfo>> threadsSnap;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        threadsSnap.reserve(g_threads.size());
        for (const auto& kv : g_threads) threadsSnap.push_back(kv);
    }
    // P2: flat util array indexed by cpu id (cpu ids are 0..g_cpuCount-1) instead
    // of a std::map<int,int> — cheaper lookup, no per-entry heap node churn.
    int cpuN = g_cpuCount > 0 ? g_cpuCount : 1;
    std::vector<int> utilSnap(cpuN, -1);
    {
        std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
        for (const auto& kv : g_cpuMetrics) {
            if (kv.first >= 0 && kv.first < cpuN) utilSnap[kv.first] = kv.second.util;
        }
    }
    // One ScoringContext for the whole table: snapshots cfg + pools + cpuMetrics.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    ScoringContext ctx = buildScoringContext(cfgSnap);
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // P2: reserve a generous upper bound so the ~500-thread table does not
    // reallocate the JSON string dozens of times.
    std::string json = "{\"timestamp\":" + std::to_string((long long)time(nullptr)) + ",\"generation\":" + std::to_string(gen) + ",\"threads\":[";
    json.reserve(16384 + threadsSnap.size() * 128);
    bool first = true;
    for (const auto& kv : threadsSnap) {
        const int tid = kv.first; const ThreadInfo& ti = kv.second;
        if (!first) json += ",";
        first = false;
        int cpuUsage = 0;
        if (ti.currentCpu >= 0 && ti.currentCpu < cpuN) cpuUsage = utilSnap[ti.currentCpu];
        // P0-4: cached display score. Reuse within TTL; otherwise compute once from
        // the shared ScoringContext (no per-thread relock).
        int score = 0;
        {
            std::lock_guard<std::mutex> sl(g_webuiScoreCacheMutex);
            auto it = g_webuiScoreCache.find(tid);
            if (it != g_webuiScoreCache.end() && it->second.first > nowMs) {
                score = it->second.second;
            } else {
                score = computeScore(ti.currentCpu, ti, ti.currentCpu, ctx);
                // HTTP-02: eviction pass. Drop expired entries; if still over
                // capacity, evict the oldest-expiry entries until bounded.
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
        // fix105 P2-4: jsonEscape d.role and d.type
        json += "{\"timestamp\":" + std::to_string((long long)d.timestamp) + ",\"tid\":" + std::to_string(d.tid) + ",\"name\":\"" + jsonEscape(d.name) + "\",\"role\":\"" + jsonEscape(d.role) + "\",\"from_cpu\":" + std::to_string(d.from_cpu) + ",\"to_cpu\":" + std::to_string(d.to_cpu) + ",\"score\":" + std::to_string(d.score) + ",\"confidence\":" + std::to_string(d.confidence) + ",\"type\":\"" + jsonEscape(d.type) + "\"}";
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/decisions.json", json);
}

// v4.4.6: read-only C4 freezer status (no write operations exposed). When the
// feature is OFF the JSON reports {"enabled":false,...} and never errors.
void writeC4Json() {
    std::string inner = aatpe::c4freezer::c4FreezerStateJson();
    std::string json = "{\"timestamp\":" +
        std::to_string((long long)time(nullptr)) + ",\"c4\":" + inner + "}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/c4.json", json);
}

void writeExplainJson() {
    // P0-5 (Bug-3): previously this called FIVE locking score functions PER CPU
    // (capacityScore/thermalScore/contentionPenalty each re-took g_cpuMetricsMutex,
    // poolScore re-took g_topologyRwMutex, plus computeScore). For ~8 CPUs that was
    // ~48 lock/unlock pairs every 2s on the WebUI thread — enough to stall the
    // serialized write and leave explain.json stale ("加载中" forever).
    //
    // Fix: build ONE ScoringContext at entry (1 topology lock + 1 metrics lock,
    // same as decidePlacement), then compute every component breakdown INLINE from
    // the ctx snapshot (the per-component logic is a straight read of
    // capacity/temp/util + pool membership + the role/profile switch — identical
    // numbers to scoring.cpp). total_score uses the ctx-aware computeScore overload.
    // Zero per-CPU locking.
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
        // g_allCpus is rewritten by refreshCpuTopology() under g_topologyRwMutex;
        // snapshot it under that lock (buildScoringContext only snaps the pools).
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        allCpuSnap = g_allCpus;
    }

    // Helper: read a snapshot metric for cpuId (0 if unknown).
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
        // --- capacity_score (mirrors scoring.cpp::capacityScore, from ctx) ---
        int cap = snapMetric(cpuId, 2);
        int capacity_score;
        if (role == ROLE_S0_SYSTEM) capacity_score = 0;
        else if (role == ROLE_S5_BACKGROUND) capacity_score = capacity::kBackgroundInverseRef - cap;
        else capacity_score = cap;
        // --- latency_score (pure role table, no lock) ---
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
        // --- thermal_score (from ctx temp) ---
        int temp = snapMetric(cpuId, 1);
        int thermal_score = 0;
        if (temp != 0) {
            int penalty = 0, boost = 0;
            if (temp > thermal::kHighTempThreshold) penalty = (temp - thermal::kHighTempThreshold) * thermal::kPenaltyPerDegree;
            if (temp < thermal::kLowTempThreshold)  boost  = (thermal::kLowTempThreshold - temp) * thermal::kBoostPerDegree;
            thermal_score = boost - penalty;
        }
        // --- power_score (profile + role, no lock) ---
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
        // --- pool_score (from ctx.pools) ---
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
        // --- contention_penalty (from ctx util) ---
        int util = snapMetric(cpuId, 0);
        int contention_penalty;
        if (util > contention::kHighUtilThreshold) contention_penalty = contention::kHighUtilPenalty;
        else if (util > contention::kMidUtilThreshold) contention_penalty = contention::kMidUtilPenalty;
        else if (util > contention::kLowUtilThreshold) contention_penalty = contention::kLowUtilPenalty;
        else contention_penalty = contention::kIdleBoost;
        // --- total_score: ctx-aware overload (no relock) ---
        int total_score = computeScore(cpuId, *ti, ti->currentCpu, ctx);
        json += "{\"cpu\":" + std::to_string(cpuId) + ",\"capacity_score\":" + std::to_string(capacity_score) + ",\"latency_score\":" + std::to_string(latency_score) + ",\"thermal_score\":" + std::to_string(thermal_score) + ",\"power_score\":" + std::to_string(power_score) + ",\"pool_score\":" + std::to_string(pool_score) + ",\"contention_penalty\":" + std::to_string(contention_penalty) + ",\"total_score\":" + std::to_string(total_score) + "}";
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/explain.json", json);
}

void writeModelJson() {
    // v94 P2 问题17/19: snapshot g_cfg under g_cfgMutex so the config fields
    // embedded in model.json are a consistent same-generation copy.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    // v94 改进包B 任务1: snapshot the three pool vectors under g_topologyRwMutex
    // BEFORE taking g_mlMutex (mirrors decidePlacement(): topology and ML locks
    // are never held together). Previously poolCpuLists aliased g_littlePool/
    // g_midPool/g_bigPool directly while only g_mlMutex was held, racing
    // refreshCpuTopology().
    std::vector<int> littleSnap, midSnap, bigSnap, primeSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        littleSnap = g_littlePool;
        midSnap    = g_midPool;
        bigSnap    = g_bigPool;
        // R007e: PRIME cluster cpus (e.g. D1100 cpu7). Derive from clusters() while
        // the topology lock is held (primePool() would re-lock the non-recursive
        // g_topologyRwMutex -> deadlock).
        for (const auto& cl : clusters()) {
            if (cl.tier == CpuTier::PRIME) primeSnap = cl.cpus;
        }
    }
    std::lock_guard<std::mutex> lock(g_mlMutex);
    int poolCount = g_mlModel.getPoolCount();
    // fix104 v34: Added funnel diagnostics field; fix104 v35: Added per-CPU cpus array
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
        // v4.2.1 T1-fix: update() internal-gate rejection counters. outcome_valid
        // now means "TRAINED" (SGD actually executed). These show why finite
        // outcomes were NOT learned.
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
        // v66: ML cold start protection state for WebUI
        "\"cold_start_protection\":" + (cfgSnap.mlColdStartProtection?"true":"false") + ","
        "\"min_samples\":" + std::to_string(cfgSnap.mlMinSamples) + ","
        // v66-issue16: Frame-time outcome training target and diagnostics
        "\"training_target\":\"" + (cfgSnap.mlFrameTimeOutcome ? "frame_time+run_ratio" : "run_ratio_only") + "\","
        "\"frame_time_weight\":" + std::to_string(cfgSnap.mlFrameTimeWeight) + ","
        "\"frame_time_outcome\":{\"samples\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeSamples.load())
        + ",\"fallback\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeFallback.load())
        + ",\"improved\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeImproved.load())
        + ",\"degraded\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeDegraded.load())
        + ",\"gfxinfo_calls\":" + std::to_string((long long)aatpe_ml::g_ftGfxinfoCalls.load())
        + ",\"gfxinfo_cache_hits\":" + std::to_string((long long)aatpe_ml::g_ftGfxinfoCacheHits.load()) + "},"
        "\"pools\":[";
    // R007e: include PRIME (slot 3). poolCpuLists indexed by pool slot; guard any
    // out-of-range slot (e.g. a future 5th tier) with UNKNOWN/empty rather than
    // indexing past the array.
    const char* poolNames[] = {"LITTLE","MID","BIG","PRIME"};
    const std::vector<int>* poolCpuLists[] = {&littleSnap,&midSnap,&bigSnap,&primeSnap};
    bool first = true;
    for (int poolIdx = 0; poolIdx < poolCount; poolIdx++) {
        if (!first) json += ",";
        first = false;
        // HTTP-08 (P3, audit 2026-10-26): the ML accessors are internally bounded
        // against models.size(), but the boundary DECISION belongs at this call
        // site, not in the callee. For a known pool slot (<4) ask the model; for
        // an out-of-range slot (a future 5th tier) synthesize a zero PoolModel
        // locally instead of calling getPoolModel/isPoolReady/getAlphaForPool with
        // an index we have already classified as unknown. The UNKNOWN/empty JSON
        // shape from R007e is preserved.
        const bool knownPool = (poolIdx >= 0 && poolIdx < 4);
        PoolModel m;
        if (knownPool) {
            m = g_mlModel.getPoolModel(poolIdx);
        } else {
            for (int i = 0; i < ML_WEIGHT_COUNT; i++) m.weights[i] = 0.0f;
            m.learning_rate = 0.0f; m.sample_count = 0; m.alpha = 0.0f;
        }
        const char* pName = knownPool ? poolNames[poolIdx] : "UNKNOWN";
        json += "{\"pool\":" + std::to_string(poolIdx) + ",\"name\":\"" + pName + "\",\"cpus\":[";
        if (knownPool) { bool cf=true; for(int cpu:*poolCpuLists[poolIdx]){if(!cf)json+=",";cf=false;json+=std::to_string(cpu);} }
        json += "],\"weights\":[";
        for (int w=0;w<ML_WEIGHT_COUNT;w++){if(w>0)json+=",";json+=fmtWeightJson(m.weights[w]);}
        json += "],\"sample_count\":" + std::to_string(m.sample_count)
             + ",\"ready\":" + (knownPool ? (g_mlModel.isPoolReady(poolIdx)?"true":"false") : "false")
             + ",\"learning_rate\":" + std::to_string(m.learning_rate)
             + ",\"alpha\":" + fmtJsonFloat(knownPool ? (double)g_mlModel.getAlphaForPool(poolIdx) : 0.0, 4) + "}";
    }
    // fix104 v35: Expand per-pool weights to per-CPU for WebUI (expects data.cpus[])
    json += "],\"cpus\":[";
    bool cpuFirst = true;
    for (int poolIdx = 0; poolIdx < poolCount; poolIdx++) {
        PoolModel pm = g_mlModel.getPoolModel(poolIdx);
        // R007e: was `if (poolIdx >= 3) continue;` which dropped the PRIME pool's
        // cpus (cpu7) from the per-CPU weights table. Guard against out-of-range
        // pool slots instead of skipping slot 3.
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
    // v94 P2 问题17/19: snapshot g_cfg under g_cfgMutex and embed generation.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::string json = "{";
    json += "\"generation\":" + std::to_string(gen) + ",";
    // fix105 P2-5: jsonEscape current profile.
    // item 9: g_currentProfile is a non-atomic std::string written by loadProfile()
    // while the Profile Manager may be re-arbitrating. Do NOT read it directly here.
    // effectiveProfile() reads g_pmApplied under g_pmMutex (the committed winner),
    // which is kept in sync with g_currentProfile by the Profile Manager.
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
    // v94 fix / 问题41: SINGLE STATE SOURCE. The daemon is the ONLY producer of
    // observable state; watchdog and the WebUI HTTP layer are pure consumers.
    // This function reads only the state files the daemon wrote (it must NOT
    // itself re-detect topology, re-read sensors, or recompute pools). Version
    // strings come from the compile-time aatpe-version.h single source of truth.
    // v94 P2 问题17/19: snapshot g_cfg under g_cfgMutex and embed generation.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    // v94 改进包B 任务1: snapshot pool vectors under g_topologyRwMutex before
    // the topology JSON section below previously iterated g_littlePool/
    // g_midPool/g_bigPool unlocked.
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
    // HTTP-03: snapshot g_batteryAged under its writer's mutex. The battery thread
    // writes it under g_batteryStateMutex (aatped_battery.cpp:166-168); reading it
    // unlocked here was a TSan-flagged data race.
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
    int skinTemp = readInt((std::string(STATE_DIR)+"/thermal_skin").c_str(),0);
    int batteryTemp = readInt((std::string(STATE_DIR)+"/thermal_battery").c_str(),0);
    int thermalEmergency = readInt((std::string(STATE_DIR)+"/thermal_emergency").c_str(),0);
    int loadAvg = readInt((std::string(STATE_DIR)+"/system_load").c_str(),0);
    int uptime = readInt((std::string(STATE_DIR)+"/daemon_uptime").c_str(),0);
    // W-1: HTTP channel state produced by applyHttpServerState() in aatped_main.cpp.
    // Closed set: on / disabled_by_config / failed_token / ports_busy. Anything else
    // (missing file on a daemon that predates this, or a hand-edited value) degrades to
    // "unknown" so the WebUI keeps its legacy port-probe behavior instead of rendering a
    // bogus status. http_port mirrors STATE_DIR/http_port for the on-state display.
    std::string httpState = readFile((std::string(STATE_DIR)+"/http_state").c_str());
    if (httpState != "on" && httpState != "disabled_by_config" &&
        httpState != "failed_token" && httpState != "ports_busy") {
        httpState = "unknown";
    }
    int httpPortState = readInt((std::string(STATE_DIR)+"/http_port").c_str(), 0);
    // fix105 P1-2: eBPF status from state file
    int ebpfActive = readInt((std::string(STATE_DIR)+"/ebpf_active").c_str(),0);
    // v94-r3 fix: per-tracepoint offset-verification tokens. Default to
    // "unknown" so a missing file (daemon crashed before writing the status)
    // still renders a valid JSON string instead of "null". The tokens are
    // produced by EbpfScheduler::getOffsetStatus{Switch,Fork,Exec,Exit} and
    // written by aatped_main.cpp at eBPF load time.
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

    // v4.2.2 P3-1: PSI three-tier (avg10/avg60/avg300, "some" stall). cpu/io/mem.
    // Values come straight from the diag snapshot (atomic). unavailable resource /
    // tier stays 0.0; psi.available=0 means /proc/pressure missing -> frontend shows --.
    aatpe::diag::SystemPressure psi = aatpe::diag::getSystemPressureSnapshot();
    // G-003 fix: guard NaN/Inf exactly like fmtJsonFloat/fmtWeightJson. If a
    // /proc/pressure parse ever yields a non-finite value, std::to_string would
    // emit the bare token "nan"/"inf" which is illegal JSON and breaks
    // JSON.parse() on the system state page. Substitute "0" (PSI tier semantics:
    // 0% stall). Finite values keep the existing std::to_string formatting.
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
        "\"thermal\":{\"cpu\":" + std::to_string(cpuTemp) + ",\"skin\":" + std::to_string(skinTemp) + ",\"battery\":" + std::to_string(batteryTemp) + ",\"emergency\":" + std::to_string(thermalEmergency) + ",\"skin_pressure\":" + std::to_string(skinPressure) + ",\"battery_pressure\":" + std::to_string(batteryPressure) + "}," +
        psiBlock +
        "\"thermal_limit\":" + std::to_string(cfgSnap.thermalLimitCpu) + ","
        // v94-hotreload: emit the exact field names the frontend fieldDefs request
        // (thermal_limit_cpu / _skin / _battery) straight from the g_cfg snapshot.
        // Previously the system JSON only carried the legacy "thermal_limit" alias, so
        // aggData['thermal_limit_cpu'] was always undefined and the WebUI fell back to
        // the state file — which lagged on hot-reload. Emit both so the SSE push shows
        // the freshly-reloaded value immediately.
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
    if (predMae.empty()) predMae = "0.000000";
    if (predConf.empty()) predConf = "0.500000";
    // G-001 fix: the cpu_topology object was already fully closed above by
    //   json += "]},";   (closes the "prime" array then the cpu_topology object)
    // The old statement below was a refactor residue that re-appended:
    //   "]"            -> stray closing bracket (no open array at root level)
    //   "\"prime\":[]" -> a SECOND, root-level duplicate "prime" key
    //   "}"            -> prematurely closed the ROOT object
    // leaving control_mode and every later field stranded OUTSIDE the root object,
    // so system.json was always illegal JSON (audit verified via json.loads).
    // Continue emitting root-level fields directly; L637 already ended with a ",".
    json += "\"control_mode\":\"" + jsonEscape(controlMode) + "\","
        "\"log_level\":" + std::to_string(logLevel) + ","
        "\"daemon_uptime\":" + std::to_string(uptime) + ","
        // W-1: HTTP channel state (read-only observability; no auth/security change).
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
        // P1-14: predMae/predConf are free-text values read from STATE_DIR files
        // (prediction_mae / prediction_confidence). They could in principle contain
        // a quote/backslash; escape them before embedding in JSON.
        "\"prediction_mae\":\"" + jsonEscape(predMae) + "\","
        "\"prediction_confidence\":\"" + jsonEscape(predConf) + "\","
        "\"prediction_proactive_migrations\":" + std::to_string(readInt((std::string(STATE_DIR)+"/prediction_proactive_migrations").c_str(),0)) + ","
        // v66: Battery aging protection state
        "\"battery_aging\":{\"enabled\":" + (cfgSnap.batteryAgingProtection?"true":"false") + ",\"aged\":" + (batteryAgedSnap?"true":"false") + "},"
        // fix105 P1-2: eBPF status object
        // v94-r3: extend with per-tracepoint offset status + filtered-event
        // counter so the WebUI can show which lifecycle events are degraded.
        // offset_switch="degraded" means the whole eBPF subsystem fell back to
        // polling; offset_fork/exec/exit="degraded" means that event class is
        // being dropped in deliver*Event() due to a ctx-offset mismatch.
        "\"ebpf\":{\"active\":" + (ebpfActive?"true":"false") +
            ",\"offset_switch\":\"" + jsonEscape(offSwitch) + "\"" +
            ",\"offset_fork\":\""   + jsonEscape(offFork)   + "\"" +
            ",\"offset_exec\":\""   + jsonEscape(offExec)   + "\"" +
            ",\"offset_exit\":\""   + jsonEscape(offExit)   + "\"" +
            ",\"offset_filtered\":" + std::to_string(offFiltered) + "},"
        // P2: Federated learning status
        // HTTP-04: atomic loads to eliminate cross-thread data race.
        "\"federated\":{\"rounds\":" + std::to_string(g_federatedRounds.load(std::memory_order_relaxed)) + ",\"exports\":" + std::to_string(g_federatedExports.load(std::memory_order_relaxed)) + ",\"imports\":" + std::to_string(g_federatedImports.load(std::memory_order_relaxed)) + "},"
        // P2: uevent monitor status
        "\"uevent\":{\"active\":" + (readInt((std::string(STATE_DIR)+"/event_uevent_active").c_str(),0)?"true":"false") + "},"
        // v66-issue16: Current frame-time snapshot from gfxinfo provider
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
        // v66-deeprefactor (问题40): average decision fusion weights for explainability
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
        // v4.2 Batch C (C1): cross-batch STATE_DIR observability keys, hardcoded per-key
        // (this file is NOT a glob — every key is emitted explicitly). The files are
        // plain uint text produced by other batches (B: ml_weight_degraded_count;
        // cold_start_rules: cold_start_rejected; A: sf_thread_count/schedtune_boost_current/
        // autocal_ratio). readInt(path,0) degrades a missing/unwritten file to 0, so a
        // daemon that predates those batches still renders valid JSON. autocal_ratio is
        // stored ×1000 -> restored to a float ratio here.
        + ",\"v42\":{\"ml_weight_degraded_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/ml_weight_degraded_count").c_str(),0))
        + ",\"cold_start_rejected\":" + std::to_string(readInt((std::string(STATE_DIR)+"/cold_start_rejected").c_str(),0))
        + ",\"sf_thread_count\":" + std::to_string(readInt((std::string(STATE_DIR)+"/sf_thread_count").c_str(),0))
        + ",\"schedtune_boost_current\":" + std::to_string(readInt((std::string(STATE_DIR)+"/schedtune_boost_current").c_str(),0))
        + ",\"autocal_ratio\":" + ([](){
            char b[32]; snprintf(b, sizeof(b), "%.3f", (double)readInt((std::string(STATE_DIR)+"/autocal_ratio").c_str(),0)/1000.0);
            return std::string(b);
          })()
        // C3: thermal critical-wall headroom % (already collected by thermal.cpp:1000-1001;
        // only surfaced to the panel here — no re-collection).
        + ",\"thermal_headroom_pct\":" + std::to_string(readInt((std::string(STATE_DIR)+"/thermal_headroom_pct").c_str(),0))
        // 第一个 } 闭合 "v42" 对象，第二个 } 闭合顶层 system 对象。
        // 真机 ui_snapshot 验证：旧代码只有一个 }，导致顶层对象未闭合、
        // 整个 system.json 非法，WebUI 回退 per-file 逐文件 exec，首屏卡十几秒。
        + "}}";
    // fix105 P2-3: atomicWriteJson instead of writeFile
    atomicWriteJson(std::string(WEBUI_DIR) + "/system.json", json);
}

void writePerCpuJson() {
    // v94 改进包B 任务1: snapshot-then-serialize. Previously this function
    // iterated g_littlePool/g_midPool/g_bigPool/g_allCpus/g_cpuMetrics with NO
    // locks at all, racing readCpuMetrics() (which rewrites g_cpuMetrics under
    // g_cpuMetricsMutex) and refreshCpuTopology() (which rewrites the pools under
    // g_topologyRwMutex). That is a data race: a torn vector/map read could
    // crash the serializer or emit inconsistent JSON.
    //
    // Pattern (mirrors decidePlacement() in placement.cpp): snapshot each
    // protected container under its OWN mutex, release the mutex, then serialize
    // purely from local copies. Locks are NEVER held together (no nesting), so
    // no new AB/BA ordering is introduced.
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
    for (int cpuId:allCpuSnap) {
        auto it = metricsSnap.find(cpuId);
        if (it==metricsSnap.end()) continue;
        if (!first) json+=",";
        first=false;
        const CpuMetrics& m = it->second;
        auto typeIt = cpuTypeMap.find(cpuId);
        std::string cpuType = (typeIt!=cpuTypeMap.end())?typeIt->second:"unknown";
        json += "{\"cpu\":"+std::to_string(cpuId)+",\"type\":\""+cpuType+"\",\"util\":"+std::to_string(m.util)+",\"cur_freq\":"+std::to_string(m.curFreq)+",\"freq_pct\":"+std::to_string(m.freqPct)+",\"temp\":"+std::to_string(m.temp)+",\"capacity\":"+std::to_string(m.capacity)+"}";
    }
    json += "]}";
    atomicWriteJson(std::string(WEBUI_DIR) + "/percpu.json", json);
}

// v66 P2-2 fix: Read last N lines from daemon log file and output as JSON.
// P1-4: the old implementation std::getline()'d the ENTIRE log (up to 8MB, then
// rotates) into a std::vector<std::string> every 2s — a multi-MB heap churn on
// the WebUI thread. Now we fseek() to the last ~64KB, read only that window, and
// split into lines to keep the last kMaxLines. Bounded memory, fast.
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
    const long long kReadBytes = 65536;  // tail window

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

    // Seek to the tail window. If the file is smaller, read from 0.
    long long sz = lseek(fd, 0, SEEK_END);
    long long startOff = 0;
    if (sz > kReadBytes) startOff = sz - kReadBytes;
    lseek(fd, startOff, SEEK_SET);

    // P2: 64KB+4KB read window was a stack-local char[] — ~68KB on the WebUI
    // thread stack. Heap-allocate via std::vector so a tight stack limit (e.g.
    // an 8MB pthread stack is fine, but some constrained callers are smaller)
    // cannot blow out.
    std::vector<char> buf(65536 + 4096);
    ssize_t nread = read(fd, buf.data(), buf.size());
    close(fd);
    if (nread <= 0) {
        json += "]}";
        atomicWriteJson(std::string(WEBUI_DIR) + "/logs.json", json);
        return;
    }
    // Split the window into lines. If we did NOT start at offset 0, the first
    // fragment is a partial line — drop it so we don't emit a torn line.
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
        // strip trailing \r
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

// W2: serialize per-app frame stats for GET /api/frame_apps.
// Accessor declared in aatped.h:268 (aatpe_getPerAppFrameStats) returns entries
// in MRU order (most-recently-updated first); we preserve that order. Numerics
// are emitted as the struct's natural values (defaults 0); the WebUI renders
// null/missing as '--'. Field names match the frozen front-end contract exactly.
std::string aatpe_buildFrameAppsJson() {
    std::vector<AatpePerAppFrameStat> out;
    std::string currentPkg;
    aatpe_getPerAppFrameStats(out, currentPkg);

    // v4.2.8: surface the authoritative frame source/reason alongside current.
    // 07b will later wire these from the frame subsystem; for now read the
    // existing state files so the contract is stable on the wire.
    // HTTP-09 (P3, audit 2026-10-26): STATE_DIR is bound in aatped_main.cpp to the
    // string literal "/dev/aatpe/state" and is never reassigned, so the previous
    // `STATE_DIR ? STATE_DIR : ""` null-guard was dead code. Use it directly.
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
        // rel12 Bug1: raw missed-vsync accumulator (may exceed total_frames; shown
        // only in the detail panel with a "一帧可错过多个vsync" note).
        body += "\"missed_vsyncs\":" + std::to_string(a.missed_vsyncs) + ",";
        body += "\"active_ms\":" + std::to_string(a.active_ms) + ",";
        body += "\"last_update_ms\":" + std::to_string(a.last_update_ms) + ",";
        body += "\"target_fps\":" + (a.target_fps > 0.01 ? std::to_string(a.target_fps) : std::string("null"));
        // v4.3.1 (诉求2): per-entry honest state so every manual-list package is visible.
        body += ",\"reason\":\"" + jsonEscape(a.reason) + "\"";
        body += ",\"frame_source\":\"" + jsonEscape(a.frameSource) + "\"";
        body += ",\"current\":" + std::string(a.isCurrent ? "true" : "false");
        body += ",\"last_seen_ms\":" + std::to_string(a.lastSeenMs);
        body += ",\"last_seen_epoch_ms\":" + std::to_string(a.lastSeenEpochMs);
        body += ",\"label\":null";   // placeholder; frontend joins via /api/apps pkg->label
        body += "}";
    }
    body += "]}";
    return body;
}

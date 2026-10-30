#include "aatped.h"
#include "scoring_interface.h"
#include "aatpe_ml_hybrid.h"
#include <array>
#include "adrc.h"
#include "aatped_priority.h"
#include "aatpe_recovery.h"
#include "aatpe_placement_mech.h"
#include "aatpe_caps.h"
#include "aatped_threadutil.h"
#include "gpu_boost.h"
#include "aatpe_diag.h"
#include "aatped_psi.h"
#include "aatped_utils.h"
#include "aatpe_state_ttl.h"
#include "aatpe_subsystem.h"
#include <deque>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include <fstream>
extern float g_systemLoad;
namespace aatpe { int aatpeThermalHalLevel(); }
bool isKernelThread(const std::string& tname, int tid);
using namespace aatpe;
using namespace aatpe_ml;
using namespace aatpe_ml_hybrid;
using namespace aatpe::adrc;
extern "C" void aatpePruneDeadRtTids();

extern std::atomic<float> g_thermalPlacementScale;
extern std::atomic<bool> thermalSmoothEnabled;

static std::string cpuSetToListStrLocal(const cpu_set_t& m) {
    std::string out;
    int rangeStart = -1, prev = -1;
    bool inRange = false;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (CPU_ISSET(c, &m)) {
            if (!inRange) { inRange = true; rangeStart = c; }
            prev = c;
        } else if (inRange) {
            if (!out.empty()) out += ",";
            out += (rangeStart == prev) ? std::to_string(rangeStart)
                                         : (std::to_string(rangeStart) + "-" + std::to_string(prev));
            inRange = false;
        }
    }
    if (inRange) {
        if (!out.empty()) out += ",";
        out += (rangeStart == prev) ? std::to_string(rangeStart)
                                     : (std::to_string(rangeStart) + "-" + std::to_string(prev));
    }
    return out;
}

static int applySmartPlacementFallback(int bestCpu, const ThreadInfo& ti,
                                       const Config& cfgSnap,
                                       const cpu_set_t& allowedCpus);

std::mutex g_topologyRwMutex;

std::atomic<bool> g_topologyRebuilding{false};
static std::atomic<long long> s_lastTopologyRebuildMs{0};
static std::atomic<int> s_topologyRebuildCount{0};
static std::atomic<int> g_topologyDebounceMs{500};

static std::mutex g_migRateMutex;

FusionWeightAccumulator g_fusionWeightAccum;

void FusionWeightAccumulator::record(const DecisionFusion& f) {
    count.fetch_add(1, std::memory_order_relaxed);
    double cur, next;
    cur = sum_heuristic.load(std::memory_order_relaxed);
    do { next = cur + (double)f.w_heuristic; } while (!sum_heuristic.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    cur = sum_ml.load(std::memory_order_relaxed);
    do { next = cur + (double)f.w_ml; } while (!sum_ml.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    cur = sum_hybrid.load(std::memory_order_relaxed);
    do { next = cur + (double)f.w_hybrid; } while (!sum_hybrid.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    cur = sum_history.load(std::memory_order_relaxed);
    do { next = cur + (double)f.w_history; } while (!sum_history.compare_exchange_weak(cur, next, std::memory_order_relaxed));
}
void FusionWeightAccumulator::reset() {
    count.store(0, std::memory_order_relaxed);
    sum_heuristic.store(0.0, std::memory_order_relaxed);
    sum_ml.store(0.0, std::memory_order_relaxed);
    sum_hybrid.store(0.0, std::memory_order_relaxed);
    sum_history.store(0.0, std::memory_order_relaxed);
}

long long FusionWeightAccumulator::getAverageWeights(float& outHeuristic, float& outMl,
                                                      float& outHybrid, float& outHistory) const {
    long long cnt = count.load(std::memory_order_relaxed);
    if (cnt <= 0) {
        outHeuristic = outMl = outHybrid = outHistory = 0.0f;
        return 0;
    }
    double sh = sum_heuristic.load(std::memory_order_relaxed);
    double sm = sum_ml.load(std::memory_order_relaxed);
    double shy = sum_hybrid.load(std::memory_order_relaxed);
    double shs = sum_history.load(std::memory_order_relaxed);
    double d = (double)cnt;
    outHeuristic = (float)(sh / d);
    outMl        = (float)(sm / d);
    outHybrid    = (float)(shy / d);
    outHistory   = (float)(shs / d);
    return cnt;
}

void writeFusionWeightsState() {
    float wh, wm, why, whs;
    long long cnt = g_fusionWeightAccum.getAverageWeights(wh, wm, why, whs);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"heuristic_weight\":%.4f,\"ml_weight\":%.4f,\"hybrid_weight\":%.4f,"
        "\"history_weight\":%.4f,\"total_decisions\":%lld}",
        (double)wh, (double)wm, (double)why, (double)whs, cnt);
    writeFile((std::string(STATE_DIR) + "/fusion_weights").c_str(), buf);
    writeFile((std::string(STATE_DIR) + "/placement_settle_migrations").c_str(),
              (int)g_settleMigrations.load(std::memory_order_relaxed));
    writeFile((std::string(STATE_DIR) + "/placement_settle_hysteresis_rejects").c_str(),
              (int)g_settleHysteresisRejects.load(std::memory_order_relaxed));
    writeFile((std::string(STATE_DIR) + "/placement_settle_cooldown_rejects").c_str(),
              (int)g_settleCooldownRejects.load(std::memory_order_relaxed));
    writeFile((std::string(STATE_DIR) + "/placement_settle_avg_dwell_ms").c_str(),
              (int)placementGetAvgSettleDwellMs());
}

void writeFrameTimeOutcomeState() {
    writeFile((std::string(STATE_DIR) + "/ft_outcome_samples").c_str(),
              (int)aatpe_ml::g_ftOutcomeSamples.load());
    writeFile((std::string(STATE_DIR) + "/ft_outcome_improved").c_str(),
              (int)aatpe_ml::g_ftOutcomeImproved.load());
    writeFile((std::string(STATE_DIR) + "/ft_outcome_degraded").c_str(),
              (int)aatpe_ml::g_ftOutcomeDegraded.load());
    writeFile((std::string(STATE_DIR) + "/ft_outcome_fallback").c_str(),
              (int)aatpe_ml::g_ftOutcomeFallback.load());
    writeFile((std::string(STATE_DIR) + "/ft_gfxinfo_calls").c_str(),
              (int)aatpe_ml::g_ftGfxinfoCalls.load());
    writeFile((std::string(STATE_DIR) + "/ft_gfxinfo_cache_hits").c_str(),
              (int)aatpe_ml::g_ftGfxinfoCacheHits.load());
    aatpe_ml::FrameTimeOutcome ft = getCurrentFrameTimeSnapshot();
    char buf[256];
    snprintf(buf, sizeof(buf), "%.2f", (double)ft.frame_time_avg);
    writeFile((std::string(STATE_DIR) + "/frame_time_avg").c_str(), buf);
    snprintf(buf, sizeof(buf), "%.2f", (double)ft.frame_time_p95);
    writeFile((std::string(STATE_DIR) + "/frame_time_p95").c_str(), buf);
    snprintf(buf, sizeof(buf), "%.2f", (double)ft.frame_time_p99);
    writeFile((std::string(STATE_DIR) + "/frame_time_p99").c_str(), buf);
}

RateLimitGuard::RateLimitGuard(bool frameCritical, bool emergency) : acquired_(false), consumed_(false) {
    if (emergency) {
        acquired_ = true;
        return;
    }
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    int perSec = cfgSnap.maxMigrationsPerSec;
    if (perSec < 0) perSec = 0;
    int limit = frameCritical ? perSec * 2 : perSec;

    std::lock_guard<std::mutex> rateLock(g_migRateMutex);
    time_t now = time(nullptr);
    if (g_migrationWindowStart == 0 || now - g_migrationWindowStart >= 1) {
        g_migrationWindowStart = now;
        g_migrationCount = 0;
    }
    if (g_migrationCount >= limit) {
        g_applyRateLimited++;
        return;
    }
    acquired_ = true;
}
void RateLimitGuard::consume() {
    if (!acquired_ || consumed_) return;
    std::lock_guard<std::mutex> rateLock(g_migRateMutex);
    g_migrationCount++;
    consumed_ = true;
}
RateLimitGuard::~RateLimitGuard() {  }

DecisionFusion fuseDecisions(float heuristicScore, float mlScore,
                              float hybridScore, float historyScore,
                              SystemState state, bool mlColdStart,
                              float mlAlphaScale) {
    DecisionFusion f;
    if (std::isnan(heuristicScore)) heuristicScore = 0.0f;
    if (std::isnan(mlScore))        mlScore        = 0.0f;
    if (std::isnan(hybridScore))    hybridScore    = 0.0f;
    if (std::isnan(historyScore))   historyScore   = 0.0f;
    f.heuristic_score = ((heuristicScore) < (0.0f) ? (0.0f) : ((heuristicScore) > (1.0f) ? (1.0f) : (heuristicScore)));
    f.ml_score        = ((mlScore) < (0.0f) ? (0.0f) : ((mlScore) > (1.0f) ? (1.0f) : (mlScore)));
    f.hybrid_score    = ((hybridScore) < (0.0f) ? (0.0f) : ((hybridScore) > (1.0f) ? (1.0f) : (hybridScore)));
    f.history_score   = ((historyScore) < (0.0f) ? (0.0f) : ((historyScore) > (1.0f) ? (1.0f) : (historyScore)));
    {
        std::lock_guard<std::mutex> dwlk(g_dynamicWeightsMutex);
        f.dynamic_weight_adjustment = g_dynamicScoreWeights.valid ? 1.0f : 0.0f;
    }

    if (mlAlphaScale < 0.0f) mlAlphaScale = 0.0f;
    if (mlAlphaScale > 1.0f) mlAlphaScale = 1.0f;

    float wh, wm, why, whs;
    switch (state) {
        case SystemState::DEGRADED:
            wh = 0.60f; wm = 0.20f; why = 0.10f; whs = 0.10f; break;
        case SystemState::PASSIVE:
        case SystemState::EMERGENCY:
            wh = 1.0f; wm = 0.0f; why = 0.0f; whs = 0.0f; break;
        case SystemState::NORMAL:
        default:
            wh = 0.40f; wm = 0.30f; why = 0.15f; whs = 0.15f; break;
    }

    if (mlColdStart && wm > 0.0f) {
        float total = wh + wm + why + whs;
        float remaining = wh + why + whs;
        if (remaining > 0.01f) {
            wh  = wh  / remaining * total;
            why = why / remaining * total;
            whs = whs / remaining * total;
        }
        wm = 0.0f;
    } else if (!mlColdStart && wm > 0.0f && mlAlphaScale < 1.0f) {
        float total = wh + wm + why + whs;
        float wmBase = wm;
        wm = wmBase * mlAlphaScale;
        float nonMlBase = wh + why + whs;
        float nonMlNew  = (total - wm);
        if (nonMlBase > 0.0001f) {
            float factor = nonMlNew / nonMlBase;
            wh  *= factor;
            why *= factor;
            whs *= factor;
        }
    }

    f.w_heuristic = wh;
    f.w_ml        = wm;
    f.w_hybrid    = why;
    f.w_history   = whs;

    float totalW = wh + wm + why + whs;
    if (totalW < 0.0001f) {
        f.w_heuristic = 1.0f;
        f.w_ml        = 0.0f;
        f.w_hybrid    = 0.0f;
        f.w_history   = 0.0f;
        f.final_score = f.heuristic_score;
        f.contrib_heuristic = 100.0f;
        f.contrib_ml = 0.0f;
        f.contrib_hybrid = 0.0f;
        f.contrib_history = 0.0f;
        return f;
    }

    f.final_score = wh * f.heuristic_score
                   + wm * f.ml_score
                   + why * f.hybrid_score
                   + whs * f.history_score;

    if (f.final_score > 0.0001f) {
        f.contrib_heuristic = wh * f.heuristic_score / f.final_score * 100.0f;
        f.contrib_ml        = wm * f.ml_score        / f.final_score * 100.0f;
        f.contrib_hybrid    = why * f.hybrid_score   / f.final_score * 100.0f;
        f.contrib_history   = whs * f.history_score  / f.final_score * 100.0f;
    } else {
        f.contrib_heuristic = wh * 100.0f;
        f.contrib_ml        = wm * 100.0f;
        f.contrib_hybrid    = why * 100.0f;
        f.contrib_history   = whs * 100.0f;
    }
    return f;
}

struct DecisionRecord {
    int pid; int tid; int targetCpu; int role;
    std::string reason; long long timestamp;
};
static constexpr size_t kDecisionBufferSize = 100;
static std::array<DecisionRecord, kDecisionBufferSize> g_scoreRing;
static size_t g_scoreRingHead = 0;
static size_t g_scoreRingCount = 0;
static std::mutex g_scoreRingMutex;
void pushDecisionRecord(const DecisionRecord& rec) {
    std::lock_guard<std::mutex> lk(g_scoreRingMutex);
    g_scoreRing[g_scoreRingHead] = rec;
    g_scoreRingHead = (g_scoreRingHead + 1) % kDecisionBufferSize;
    if (g_scoreRingCount < kDecisionBufferSize) g_scoreRingCount++;
}
const std::deque<DecisionRecord> getDecisionBuffer() {
    std::lock_guard<std::mutex> lk(g_scoreRingMutex);
    std::deque<DecisionRecord> out;
    size_t start = (g_scoreRingHead + kDecisionBufferSize - g_scoreRingCount) % kDecisionBufferSize;
    for (size_t i = 0; i < g_scoreRingCount; i++) {
        out.push_back(g_scoreRing[(start + i) % kDecisionBufferSize]);
    }
    return out;
}

namespace {
struct MlPredictCacheEntry {
    float modelScore = 0.0f;
    uint64_t expireMs = 0;
};
std::unordered_map<uint64_t, MlPredictCacheEntry> s_mlPredictCache;
uint64_t s_mlPredictCacheSweepMs = 0;
constexpr uint64_t kMlPredictTtlMs = 800;
constexpr uint64_t kMlPredictSweepIntervalMs = 5000;
inline uint64_t mlPredictNowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
bool aatpeIsSystemCriticalThread(const ThreadInfo& ti) {
    if (ti.role == ROLE_S0_SYSTEM) return true;
    if (isKernelThread(ti.tname, ti.tid)) return true;
    const std::string& p = ti.pname;
    if (p == "system_server" || p == "surfaceflinger") return true;
    return false;
}

void fuseMlScore(int cpuId, const ThreadInfo& ti, int heuristic, int liveCpu,
                 bool mlActive, float mlAlpha, const ThreadFeatures& features,
                 int poolIdx, bool poolColdStartBlocked,
                 float& finalScore, DecisionFusion& fusion) {
        const unsigned cacheTid = (ti.tid > 0) ? (unsigned)ti.tid : 0u;
        if (aatpeIsSystemCriticalThread(ti)) {
            float normHeuristicCrit = normalizeHeuristicScore(heuristic);
            float historyCrit = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
            if (historyCrit < 0.0f) historyCrit = 0.0f;
            if (historyCrit > 1.0f) historyCrit = 1.0f;
            fusion = fuseDecisions(normHeuristicCrit, 0.0f, 0.0f, historyCrit,
                                   getSystemState(), true);
            finalScore = fusion.final_score;
            return;
        }
        if (mlActive && mlAlpha > 0.0f && !poolColdStartBlocked) {
            float modelScore = 0.0f;
            bool mlValidForCpu = (poolIdx >= 0 && poolIdx < g_mlModel.getPoolCount());
            if (mlValidForCpu) {
                bool criticalThread = (ti.role <= ROLE_S3_GAME);
                uint64_t nowMs = mlPredictNowMs();
                uint64_t cacheKey = ((uint64_t)cacheTid << 32) | (uint32_t)poolIdx;
                MlPredictCacheEntry* cached = nullptr;
                if (!criticalThread) {
                    auto cit = s_mlPredictCache.find(cacheKey);
                    if (cit != s_mlPredictCache.end() && nowMs < cit->second.expireMs) {
                        cached = &cit->second;
                    }
                }
                if (cached) {
                    modelScore = cached->modelScore;
                } else {
                    LearningModel::BatchLock batch(g_mlModel);
                    modelScore = g_mlModel.predict(poolIdx, features);
                    if (!g_mlModel.isValidPrediction(modelScore)) {
                        modelScore = normalizeHeuristicScore(heuristic);
                    }
                    if (!criticalThread) {
                        auto& e = s_mlPredictCache[cacheKey];
                        e.modelScore = modelScore;
                        e.expireMs = nowMs + kMlPredictTtlMs;
                    }
                }
                if (nowMs - s_mlPredictCacheSweepMs >= kMlPredictSweepIntervalMs) {
                    s_mlPredictCacheSweepMs = nowMs;
                    for (auto it = s_mlPredictCache.begin(); it != s_mlPredictCache.end(); ) {
                        if (nowMs >= it->second.expireMs) it = s_mlPredictCache.erase(it);
                        else ++it;
                    }
                }
                float normHeuristic = normalizeHeuristicScore(heuristic);
                float poolAlpha = mlAlpha;
                int pIdx = (int)getPoolForCpu(cpuId);
                if (pIdx >= 0 && pIdx < g_mlModel.getPoolCount()) {
                    poolAlpha = g_mlModel.getAlphaForPool(pIdx);
                }
                float hybridScoreVal = hybridScore(cpuId, ti);
                float historyScoreVal = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
                if (historyScoreVal < 0.0f) historyScoreVal = 0.0f;
                if (historyScoreVal > 1.0f) historyScoreVal = 1.0f;
                SystemState sysState = getSystemState();
                float maxAlpha = g_mlModel.getMaxAlpha();
                float mlAlphaScale;
                if (maxAlpha < 1e-6f) {
                    mlAlphaScale = 0.0f;
                } else {
                    mlAlphaScale = poolAlpha / maxAlpha;
                }
                if (mlAlphaScale < 0.0f) mlAlphaScale = 0.0f;
                if (mlAlphaScale > 1.0f) mlAlphaScale = 1.0f;
                if (std::isnan(mlAlphaScale)) mlAlphaScale = 0.0f;
                bool mlColdStart = poolColdStartBlocked;
                fusion = fuseDecisions(normHeuristic, modelScore, hybridScoreVal,
                                        historyScoreVal, sysState, mlColdStart,
                                        mlAlphaScale);
                finalScore = fusion.final_score;
                int fromPool = (int)getPoolForCpu(liveCpu);
                int toPool = poolIdx;
                if (fromPool >= 0 && toPool >= 0 && fromPool != toPool) {
                    float migrationGain = g_mlModel.predictMigrationGain(fromPool, toPool, features);
                    if (std::isnan(migrationGain) || std::isinf(migrationGain)) migrationGain = 0.0f;
                    finalScore += migrationGain * 0.5f;
                }
            } else {
                modelScore = normalizeHeuristicScore(heuristic);
                float normHeuristic = normalizeHeuristicScore(heuristic);
                float hybridScoreVal = hybridScore(cpuId, ti);
                float historyScoreVal = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
                if (historyScoreVal < 0.0f) historyScoreVal = 0.0f;
                if (historyScoreVal > 1.0f) historyScoreVal = 1.0f;
                SystemState sysState = getSystemState();
                fusion = fuseDecisions(normHeuristic, modelScore, hybridScoreVal,
                                        historyScoreVal, sysState, true);
                finalScore = fusion.final_score;
            }
        } else if (mlActive) {
            finalScore = normalizeHeuristicScore(heuristic);
            float normHeuristicCs = normalizeHeuristicScore(heuristic);
            float historyCs = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
            if (historyCs < 0.0f) historyCs = 0.0f;
            if (historyCs > 1.0f) historyCs = 1.0f;
            fusion = fuseDecisions(normHeuristicCs, 0.0f, 0.0f, historyCs,
                                   getSystemState(), true);
        } else {
            float normHeuristicOff = normalizeHeuristicScore(heuristic);
            float historyOff = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
            if (historyOff < 0.0f) historyOff = 0.0f;
            if (historyOff > 1.0f) historyOff = 1.0f;
            fusion = fuseDecisions(normHeuristicOff, 0.0f, 0.0f, historyOff,
                                   getSystemState(), true);
        }
}

void selectOptimalPlacement(int cpuId, float finalScore, int heuristic,
                            const DecisionFusion& fusion, bool featuresValid,
                            const ThreadFeatures& features,
                            int& bestCpu, float& bestFused, float& secondFused,
                            int& bestHeuristic, DecisionFusion& bestFusion,
                            ThreadFeatures& bestFeatures) {
        if (finalScore > bestFused) {
            secondFused = bestFused;
            bestFused = finalScore;
            bestCpu = cpuId;
            bestHeuristic = heuristic;
            bestFusion = fusion;
        } else if (std::fabs(finalScore - bestFused) < 1e-6f && bestCpu >= 0) {
            int curUtil = 0, bestUtil = 0;
            {
                std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
                auto curIt = g_cpuMetrics.find(cpuId);
                auto bestIt = g_cpuMetrics.find(bestCpu);
                if (curIt != g_cpuMetrics.end()) curUtil = curIt->second.util;
                if (bestIt != g_cpuMetrics.end()) bestUtil = bestIt->second.util;
            }
            if (curUtil < bestUtil) {
                secondFused = bestFused;
                bestCpu = cpuId;
                bestHeuristic = heuristic;
                bestFusion = fusion;
                if (featuresValid) bestFeatures = features;
            } else if (curUtil == bestUtil && cpuId < bestCpu) {
            }
        } else if (finalScore > secondFused) {
            secondFused = finalScore;
        }
}

static std::vector<ThreadInfo> collectCriticalThreads(int maxCount,
        const std::set<std::string>* priorityPnames = nullptr);
namespace aatpe_spillover {
struct ClusterLoad { int tier; float util; };
struct ThreadCand  { int tid; int role; bool critical; float weight; int curTier; };
struct PromoteTarget { int tid; int targetTier; };

inline float utilThresholdForMode(int mode) {
    using namespace aatpe::spillover;
    if (mode == 0) return kUtilThresholdPassive;
    if (mode == 2) return kUtilThresholdActive;
    return kUtilThresholdAssist;
}
inline float pressureThresholdForMode(int mode) {
    using namespace aatpe::spillover;
    if (mode == 0) return kClusterPressurePassive;
    if (mode == 2) return kClusterPressureActive;
    return kClusterPressureAssist;
}
inline int topNForMode(int mode) {
    using namespace aatpe::spillover;
    if (mode == 0) return kTopNPassive;
    if (mode == 2) return kTopNActive;
    return kTopNAssist;
}
namespace aatpe { namespace placement {
int uclampMinForTier(int targetTier, float bigCapacityRatio, int mode) {
    float sfPct; int critPct;
    {
        std::lock_guard<std::mutex> cfgLk(g_cfgMutex);
        sfPct     = g_cfg.uclampSfPct;
        critPct   = (mode == 2) ? g_cfg.uclampCriticalMinPct
                  : (mode == 0) ? g_cfg.uclampPassivePct
                                : g_cfg.uclampAssistMinPct;
    }
    if (mode == 3) {
        int sf = (int)(sfPct * 1024 / 100);
        int v = (int)(sf * bigCapacityRatio);
        if (v < 64) v = 64;
        if (v > 1024) v = 1024;
        return v;
    }
    int crit = critPct * 1024 / 100;
    if (targetTier >= (int)CpuTier::PRIME) return 1024;
    int v = (int)(crit * bigCapacityRatio);
    if (v < 64) v = 64;
    if (v > 1024) v = 1024;
    return v;
}
}}

inline int floorUclampForThreadLocked(const char* tname, int targetTier,
                                      float bigCapacityRatio, int mode) {
    if (aatpeIsSurfaceFlingerThread(tname)) {
        return aatpe::placement::uclampMinForTier(targetTier, bigCapacityRatio, 3);
    }
    return aatpe::placement::uclampMinForTier(targetTier, bigCapacityRatio, mode);
}

inline int placementSchedtuneDesiredLevel(bool hasPromoted, bool emergency,
                                           bool degraded, int mode) {
    if (emergency || degraded || !hasPromoted) return -1;
    int passiveLv, assistLv, criticalLv;
    {
        std::lock_guard<std::mutex> cfgLk(g_cfgMutex);
        passiveLv  = g_cfg.schedtunePassiveBoost;
        assistLv   = g_cfg.schedtuneAssistBoost;
        criticalLv = g_cfg.schedtuneCriticalBoost;
    }
    if (mode == 0) return passiveLv;
    if (mode == 2) return criticalLv;
    return assistLv;
}

inline bool sameCapacityClusters(float utilA, float utilB) {
    float hi = std::max(utilA, utilB), lo = std::min(utilA, utilB);
    if (hi < 1.0f) return true;
    return ((hi - lo) / hi) < 0.10f;
}

namespace aatpe { namespace placement { namespace pressurefb {
    constexpr float kPsiCpuArmPct   = 20.0f;
    constexpr float kPsiCpuClearPct = 10.0f;
    constexpr float kWaitArmRatio   = 0.25f;
    constexpr float kWaitClearRatio = 0.12f;
    constexpr int   kArmStreakReq   = 2;
    constexpr int   kClearStreakReq = 3;
}}}
static inline float placementThreadRqWaitRatio(int tid) {
    int64_t w = diag::getThreadRqWaitNs(tid);
    int64_t r = diag::getThreadRunNs(tid);
    if (w >= 0 && r >= 0 && (w + r) > 0) return (float)w / (float)(w + r);
    return -1.0f;
}

std::vector<PromoteTarget> decidePromotions(const std::vector<ClusterLoad>& clusters,
                                         const std::vector<ThreadCand>& cands,
                                         float primeUtil, int mode, int stableCycles,
                                         bool singleThreadLatency = false) {
    std::vector<PromoteTarget> out;
    if (clusters.size() < 2) return out;
    const int TOP = (int)clusters.back().tier;
    float secondUtil = clusters[clusters.size() - 2].util;
    float lowestUtil = clusters.front().util;
    float pressureThresh = pressureThresholdForMode(mode);
    float bigThresh = utilThresholdForMode(mode);
    if (stableCycles < spillover::kStableCycles) return out;
    std::vector<ThreadCand> pool = cands;
    std::sort(pool.begin(), pool.end(),
              [](const ThreadCand& a, const ThreadCand& b){ return a.weight > b.weight; });
    pool.erase(std::remove_if(pool.begin(), pool.end(),
              [](const ThreadCand& c){ return c.curTier == (int)CpuTier::UNKNOWN; }),
               pool.end());
    bool sameCap = false;
    {
        std::lock_guard<std::mutex> tlk(g_topologyRwMutex);
        if (g_clusters.size() >= 2) {
            float capTop  = (float)g_clusters.back().capacity;
            float capNext = (float)g_clusters[g_clusters.size() - 2].capacity;
            sameCap = sameCapacityClusters(capTop, capNext);
        }
    }
    const int BIG_TARGET = TOP - 1;
    bool littlePressure = (lowestUtil >= pressureThresh);
    bool bigHeadroom    = (secondUtil < bigThresh);
    bool step1Trigger = littlePressure || singleThreadLatency;
    if (step1Trigger && bigHeadroom && !sameCap) {
        int n = topNForMode(mode);
        for (const auto& c : pool) {
            if ((int)out.size() >= n) break;
            if (!c.critical) continue;
            if (c.curTier >= BIG_TARGET) continue;
            out.push_back({c.tid, BIG_TARGET});
        }
    }
    bool bigSaturated = (secondUtil >= bigThresh);
    if (bigSaturated && !sameCap && primeUtil <= spillover::kPrimeHeadroom) {
        for (const auto& c : pool) {
            if (!c.critical) continue;
            if (c.curTier >= TOP) continue;
            out.push_back({c.tid, TOP});
            break;
        }
    }
    return out;
}

namespace jankdiag {
enum class Cause { NONE, THERMAL_THROTTLE, GPU_BOUND, MEM_RECLAIM, IO_STALL,
                   CPU_SINGLE_THREAD, BG_INTERFERENCE };
struct JankInput {
    bool  jank = false;
    float p95Ms = 0.0f;
    bool  cpuThrottled = false;
    bool  gpuThrottled = false;
    int   gpuUtilPct = -1;
    float psiIoSome = 0.0f;
    float psiMemSome = 0.0f;
    float psiCpuSome = 0.0f;
    float clusterUtil = 0.0f;
    float critWaitRatio = 0.0f;
};
struct Verdict { Cause cause; int confidence; std::string evidence; };
inline const char* causeName(Cause c) {
    switch (c) {
        case Cause::THERMAL_THROTTLE: return "THERMAL_THROTTLE";
        case Cause::GPU_BOUND:        return "GPU_BOUND";
        case Cause::MEM_RECLAIM:      return "MEM_RECLAIM";
        case Cause::IO_STALL:         return "IO_STALL";
        case Cause::CPU_SINGLE_THREAD:return "CPU_SINGLE_THREAD";
        case Cause::BG_INTERFERENCE:  return "BG_INTERFERENCE";
        default:                      return "NONE";
    }
}
inline Verdict classify(const JankInput& in) {
    if (!in.jank) return {Cause::NONE, 0, "jank=0"};
    if (in.cpuThrottled || in.gpuThrottled)
        return {Cause::THERMAL_THROTTLE, 90, "throttle=1"};
    if (in.gpuUtilPct >= 85 && in.clusterUtil < 60.0f)
        return {Cause::GPU_BOUND, 80, "gpu=" + std::to_string(in.gpuUtilPct)};
    if (in.psiMemSome > 20.0f)
        return {Cause::MEM_RECLAIM, 75, "psiMem=" + std::to_string((int)in.psiMemSome)};
    if (in.psiIoSome > 20.0f)
        return {Cause::IO_STALL, 75, "psiIo=" + std::to_string((int)in.psiIoSome)};
    if (in.critWaitRatio > 0.30f)
        return {Cause::CPU_SINGLE_THREAD, 80,
                "waitRatio=" + std::to_string((int)(in.critWaitRatio*100))};
    if (in.psiCpuSome > 15.0f && in.critWaitRatio <= 0.30f)
        return {Cause::BG_INTERFERENCE, 60, "psiCpu=" + std::to_string((int)in.psiCpuSome)};
    return {Cause::NONE, 10, "insufficient"};
}
}

struct TidPromoState {
    int  satisfyCycles = 0;
    bool promoted       = false;
    long long promotedMs = 0;
    bool      maskApplied = false;
    cpu_set_t savedMask;
    int       mechanism = 0;
    int       targetTier = 0;
    std::string savedCpuset;
    long long starttime = 0;
    std::string origAffinity;
    int  mechRestoreRetries = 0;
    bool rtBoosted = false;
};
std::mutex g_spillStateMutex;
std::atomic<int> g_fgDownpinBlocked{0};
std::atomic<int> g_downpinDiagCount{0};
std::atomic<int> g_fgSwitchRelease{0};
static std::unordered_map<int, TidPromoState> g_spillState;
std::atomic<long long> g_spilloverPromotions{0};
std::atomic<int> g_spilloverActive{0};

static std::atomic<int> g_fgEdgeReapply{0};
static std::mutex g_fgEdgeMtx;
static std::string g_fgEdgeReapplyPkg;
static std::atomic<int> g_cgroupDriftHealed{0};

static bool reexertMembershipLocked(int tid, TidPromoState& st) {
    using BM = aatpe_mech::BoostMechanism;
    BM mech = (BM)st.mechanism;
    if (mech != BM::STUNE_CPUSET && mech != BM::CGROUP_UCLAMP) return false;
    std::string live = aatpe_mech::readThreadCpusetGroup(tid);
    if (live.empty()) return false;
    if (live == "top-app") return false;
    bool ok = false;
    if (mech == BM::STUNE_CPUSET) {
        ok = aatpe_mech::moveTidToCpusetGroup(tid, "top-app");
    } else {
        ok = (caps::writeTidToCpuCgroupGroup(tid, "top-app") == 0);
    }
    if (ok) {
        g_cgroupDriftHealed.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("spillover: healed cgroup drift tid=%d (%s -> top-app, via=%s)",
                 tid, live.c_str(), aatpe_mech::mechanismName(mech));
    } else {
        LOG_WARN("spillover: cgroup drift rewrite FAILED tid=%d (live=%s)", tid, live.c_str());
    }
    return ok;
}

static void applyLastResortUclamp(int tid, TidPromoState& st, unsigned int umin) {
    if (aatpe_mech::aatpeUclampFloorSet(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER, umin) == 0) {
        st.mechanism = (int)aatpe_mech::BoostMechanism::UCLAMP;
    }
}

static long long spillNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static bool spillReadThreadStarttime(int tid, unsigned long long* out) {
    if (!out) return false;
    std::string path = "/proc/" + std::to_string(tid) + "/stat";
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    size_t rp = line.rfind(") ");
    if (rp == std::string::npos || rp + 1 >= line.size()) return false;
    std::istringstream ss(line.substr(rp + 2));
    char state;
    if (!(ss >> state)) return false;
    unsigned long long v;
    for (int i = 1; i <= 19; ++i) {
        if (!(ss >> v)) return false;
        if (i == 19) { *out = v; return true; }
    }
    return false;
}

static bool spillDemoteIdentityMatch(int tid, long long recordedStarttime) {
    unsigned long long liveSt = 0;
    if (!spillReadThreadStarttime(tid, &liveSt)) return false;
    return liveSt == (unsigned long long)recordedStarttime;
}

static bool efficiencySoftDepreferActive() {
    static std::atomic<long long> s_lastReadMs{0};
    static std::atomic<int> s_depref{0};
    const long long nowMs = spillNowMs();
    long long last = s_lastReadMs.load(std::memory_order_relaxed);
    if (last != 0 && (nowMs - last) < 500) {
        return s_depref.load(std::memory_order_relaxed) != 0;
    }
    std::string s = readFile((std::string(STATE_DIR) + "/arbitrator_efficiency_hint").c_str());
    size_t b = s.find_first_not_of(" \t\r\n");
    float v = 0.0f;
    if (b != std::string::npos) v = strtof(s.c_str() + b, nullptr);
    bool depref = std::isfinite(v) && v >= 0.5f;
    s_depref.store(depref ? 1 : 0, std::memory_order_relaxed);
    s_lastReadMs.store(nowMs, std::memory_order_relaxed);
    return depref;
}
static int modeFromControlMode(const std::string& m) {
    if (m == "passive") return 0;
    if (m == "active")  return 2;
    return 1;
}

static std::vector<std::string> g_vmContainerPrefixes;
static std::mutex g_vmContainerMtx;
static bool g_vmContainerListLoaded = false;

static void loadVmContainerPrefixesLocked() {
    g_vmContainerPrefixes.clear();
    std::string path = std::string(STATE_DIR) + "/vm_containers.list";
    std::ifstream f(path.c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t b = line.find_first_not_of(" \t\r");
        size_t e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == '#') continue;
        g_vmContainerPrefixes.push_back(line);
    }
}

#ifdef AATPE_HOST_TEST
void setVmContainerPrefixesForTest(const std::vector<std::string>& pkgs) {
    std::lock_guard<std::mutex> lk(g_vmContainerMtx);
    g_vmContainerPrefixes = pkgs;
    g_vmContainerListLoaded = true;
}
#endif

bool isVmContainerPkg(const std::string& pkg) {
    if (pkg.empty()) return false;
    std::lock_guard<std::mutex> lk(g_vmContainerMtx);
    if (!g_vmContainerListLoaded) {
        g_vmContainerListLoaded = true;
        loadVmContainerPrefixesLocked();
    }
    for (const std::string& p : g_vmContainerPrefixes) {
        if (p.empty()) continue;
        if (pkg == p) return true;
        if (pkg.rfind(p + ".", 0) == 0) return true;
    }
    return false;
}

static long readPidUidForVm(int pid) {
    if (pid <= 0) return -1;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    if (!f) return -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            std::istringstream ss(line);
            std::string tag; long realUid = -1;
            ss >> tag >> realUid;
            return realUid;
        }
    }
    return -1;
}

namespace aatpe_vm {
static std::set<std::string> g_forcedGuestPnames;
}
#ifdef AATPE_HOST_TEST
void setVmGuestPnamesForTest(const std::set<std::string>& pkgs) {
    aatpe_vm::g_forcedGuestPnames = pkgs;
}
#endif

static std::set<std::string> g_vmGuestCache;
static long long g_vmGuestCacheMs = 0;
static int g_vmContainerPidCached = -1;
static std::mutex g_vmGuestCacheMtx;

std::set<std::string> intersectVmGuestOverrides(const std::string& overridesText,
                                                 const std::string& containerPkg,
                                                 long cuid,
                                                 const std::map<std::string,long>& runningUidByPkg) {
    std::set<std::string> out;
    if (overridesText.empty() || cuid <= 0) return out;
    std::istringstream iss(overridesText);
    std::string innerPkg;
    while (std::getline(iss, innerPkg)) {
        while (!innerPkg.empty() && (innerPkg.back()=='\r'||innerPkg.back()==' ')) innerPkg.pop_back();
        while (!innerPkg.empty() && (innerPkg.front()==' '||innerPkg.front()=='\t')) innerPkg.erase(innerPkg.begin());
        if (innerPkg.empty() || innerPkg[0]=='#') continue;
        if (innerPkg == containerPkg) continue;
        auto it = runningUidByPkg.find(innerPkg);
        if (it == runningUidByPkg.end()) continue;
        if (it->second != cuid) continue;
        out.insert(innerPkg);
    }
    return out;
}

std::set<std::string> resolveVmGuestPnames(const std::string& containerPkg) {
    std::lock_guard<std::mutex> lk(g_vmGuestCacheMtx);
    if (!aatpe_vm::g_forcedGuestPnames.empty()) return aatpe_vm::g_forcedGuestPnames;

    static const long kVmCacheTtlMs = 5000;
    long long now = spillNowMs();
    int cpid = findPidByPackage(containerPkg);
    if (cpid <= 0) {
        g_vmGuestCache.clear(); g_vmContainerPidCached = -1; g_vmGuestCacheMs = now;
        return g_vmGuestCache;
    }
    if (!g_vmGuestCache.empty() && cpid == g_vmContainerPidCached &&
        (now - g_vmGuestCacheMs) < kVmCacheTtlMs) {
        return g_vmGuestCache;
    }
    long cuid = readPidUidForVm(cpid);
    g_vmGuestCache.clear();
    g_vmContainerPidCached = cpid;
    g_vmGuestCacheMs = now;
    if (cuid <= 0) return g_vmGuestCache;

    std::string overrides = gamelist::currentManualGameList();
    if (overrides.empty()) return g_vmGuestCache;

    std::set<std::string> interesting;
    {
        std::istringstream oiss(overrides);
        std::string inner;
        while (std::getline(oiss, inner)) {
            while (!inner.empty() && (inner.back()=='\r'||inner.back()==' ')) inner.pop_back();
            while (!inner.empty() && (inner.front()==' '||inner.front()=='\t')) inner.erase(inner.begin());
            if (inner.empty() || inner[0]=='#') continue;
            if (inner == containerPkg) continue;
            interesting.insert(inner);
        }
    }
    if (interesting.empty()) return g_vmGuestCache;

    std::map<std::string,long> running;
    DIR* d = opendir("/proc");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            char* endp = nullptr;
            long pidl = strtol(e->d_name, &endp, 10);
            if (endp == e->d_name || *endp != '\0' || pidl <= 0 || pidl > INT_MAX) continue;
            int pid = (int)pidl;
            std::ifstream cf("/proc/" + std::to_string(pid) + "/cmdline");
            std::string comm;
            if (cf) std::getline(cf, comm, '\0');
            if (comm.empty()) continue;
            if (interesting.find(comm) == interesting.end()) continue;
            long uid = readPidUidForVm(pid);
            if (uid > 0) running[comm] = uid;
        }
        closedir(d);
    }
    g_vmGuestCache = intersectVmGuestOverrides(overrides, containerPkg, cuid, running);
    return g_vmGuestCache;
}

std::vector<ClusterLoad> snapshotClusterLoads() {
    std::vector<ClusterLoad> out;
    std::vector<std::pair<int, std::vector<int>>> snap;
    {
        std::lock_guard<std::mutex> tlk(g_topologyRwMutex);
        for (const auto& cl : g_clusters) snap.push_back({(int)cl.tier, cl.cpus});
    }
    std::lock_guard<std::mutex> mlk(g_cpuMetricsMutex);
    for (auto& [tier, cpus] : snap) {
        float sum = 0; int n = 0;
        for (int cpu : cpus) {
            auto it = g_cpuMetrics.find(cpu);
            if (it != g_cpuMetrics.end()) { sum += (float)it->second.util; n++; }
        }
        out.push_back({tier, n ? sum / n : 0.0f});
    }
    return out;
}

[[maybe_unused]] static bool applyPrimeAffinityFallback(int tid, cpu_set_t* prevMaskOut) {
    std::vector<int> prime = primePool();
    if (prime.empty()) return false;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int c : prime) CPU_SET(c, &mask);
    if (sched_getaffinity(tid, sizeof(cpu_set_t), prevMaskOut) != 0) return false;
    if (sched_setaffinity(tid, sizeof(cpu_set_t), &mask) != 0) {
        int e = errno; char ebuf[128];
        LOG_WARN("spillover: tid=%d prime-affinity fallback sched_setaffinity failed errno=%d (%s)",
                 tid, e, aatpeStrError(e, ebuf, sizeof(ebuf)));
        return false;
    }
    return true;
}
static bool applyTierAffinityFallback(int tid, int targetTier, cpu_set_t* prevMaskOut) {
    std::vector<int> pool;
    {
        std::lock_guard<std::mutex> tlk(g_topologyRwMutex);
        pool = (targetTier >= (int)CpuTier::PRIME) ? primePoolLocked() : g_bigPool;
    }
    if (pool.empty()) return false;
    cpu_set_t mask; CPU_ZERO(&mask);
    for (int c : pool) CPU_SET(c, &mask);
    cpu_set_t allowed = getThreadAllowedCpus(tid);
    cpu_set_t inter; CPU_ZERO(&inter);
    for (int c : pool) if (CPU_ISSET(c, &allowed)) CPU_SET(c, &inter);
    if (CPU_COUNT(&inter) == 0) return false;
    if (sched_getaffinity(tid, sizeof(cpu_set_t), prevMaskOut) != 0) return false;
    if (sched_setaffinity(tid, sizeof(cpu_set_t), &inter) != 0) {
        LOG_WARN("spillover: tid=%d tier=%d affinity failed errno=%d", tid, targetTier, errno);
        return false;
    }
    return true;
}

#ifdef AATPE_HOST_TEST
extern "C" void aatpePlacementRewriteBigPoolForTest(const int* cpus, int n) {
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    g_bigPool.assign(cpus, cpus + n);
}
extern "C" int aatpePlacementApplyTierFallbackForTest(int tid, int targetTier) {
    cpu_set_t prev;
    return applyTierAffinityFallback(tid, targetTier, &prev) ? 1 : 0;
}
#endif

static void restoreAffinity(int tid, const cpu_set_t& mask) {
    if (sched_setaffinity(tid, sizeof(cpu_set_t), &mask) != 0) {
        int e = errno; char ebuf[128];
        LOG_WARN("spillover: tid=%d restore affinity failed errno=%d (%s)",
                 tid, e, aatpeStrError(e, ebuf, sizeof(ebuf)));
    }
}

static void rewritePromotedTidsFile() {
    std::string out;
    for (auto& [tid, st] : g_spillState) {
        out += std::to_string(tid) + ":" + std::to_string(st.starttime) +
               ":" + st.origAffinity + ":" + std::to_string(st.mechanism) + "\n";
    }
    atomicWriteFile((std::string(STATE_DIR) + "/promoted_tids").c_str(), out);
}

namespace aatpe { namespace placement {
void releaseAllSpilloverLockedImpl() {
    std::vector<int> keys;
    for (auto& [tid, st] : g_spillState) keys.push_back(tid);
    for (int tid : keys) {
        auto it = g_spillState.find(tid);
        if (it == g_spillState.end()) continue;
        auto& st = it->second;
        if (st.maskApplied) { restoreAffinity(tid, st.savedMask); st.maskApplied = false; }
        if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::UCLAMP)
            aatpe_mech::aatpeUclampFloorClear(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER);
        if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::CGROUP_UCLAMP)
            aatpe_mech::restoreTidFromCgroupUclamp(tid, "top-app", st.savedCpuset);
        if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::STUNE_CPUSET) {
            if (!st.savedCpuset.empty())
                aatpe_mech::moveTidToCpusetGroup(tid, st.savedCpuset);
        }
        if (st.rtBoosted) {
            aatpe_priority::aatpeRestoreThreadPriority(tid);
            st.rtBoosted = false;
        }
        if (st.promoted) g_spilloverActive.fetch_sub(1, std::memory_order_relaxed);
    }
    g_spillState.clear();
    for (;;) {
        int cur = g_spilloverActive.load(std::memory_order_relaxed);
        if (cur <= 0) break;
        if (g_spilloverActive.compare_exchange_weak(cur, 0)) break;
    }
    rewritePromotedTidsFile();
}
}}

int placementSpilloverTick(const Config& cfgSnap) {
    static std::atomic<bool> s_tickInFlight{false};
    bool expected = false;
    if (!s_tickInFlight.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        return 0;
    }
    struct TickGuard { ~TickGuard() { s_tickInFlight.store(false, std::memory_order_release); } } tickGuard;

    aatpePruneDeadRtTids();
    bool emergency = (getSystemState() == SystemState::EMERGENCY);

    std::vector<ClusterLoad> loads = snapshotClusterLoads();
    if (loads.size() < 2) return 0;

    {
        std::string cp;
        float pthr = pressureThresholdForMode(modeFromControlMode(cfgSnap.controlMode));
        for (auto& cl : loads) {
            cp += "tier=" + std::to_string(cl.tier) +
                  " util=" + std::to_string((int)cl.util) +
                  " pressure=" + std::string(cl.util >= pthr ? "1" : "0") + "\n";
        }
        writeFile((std::string(STATE_DIR) + "/cluster_pressure").c_str(), cp);
    }

    int mode = modeFromControlMode(cfgSnap.controlMode);
    float primeUtil = loads.back().util;

    std::string fgPkg = getCachedForegroundApp();
    bool fgKnown = !fgPkg.empty() && fgPkg != "unknown";
    std::set<std::string> vmGuestPnames;
    if (fgKnown && isVmContainerPkg(fgPkg)) {
        vmGuestPnames = resolveVmGuestPnames(fgPkg);
    }
    std::set<std::string> critPriority;
    if (fgKnown) critPriority.insert(fgPkg);
    for (const auto& g : vmGuestPnames) critPriority.insert(g);
    std::vector<ThreadInfo> crit = collectCriticalThreads(8,
        critPriority.empty() ? nullptr : &critPriority);

    {
        int sfCount = 0;
        for (const auto& ti : crit) {
            if (aatpeIsSurfaceFlingerThread(ti.tname.c_str())) sfCount++;
        }
        writeFile((std::string(STATE_DIR) + "/sf_thread_count").c_str(),
                  std::to_string(sfCount));
    }
    ::aatpe::caps::runAutocalIfDue();

    bool vetoActive = false;
    static std::atomic<int> g_prevCause{-1};
    static std::atomic<int> g_debounce{0};
    {
        diag::SystemPressure sp = diag::getSystemPressureSnapshot();
        float maxCluster = 0; for (auto& l : loads) maxCluster = std::max(maxCluster, l.util);
        float waitRatio = 0.0f;
        if (!crit.empty()) {
            int64_t w = diag::getThreadRqWaitNs(crit[0].tid);
            int64_t r = diag::getThreadRunNs(crit[0].tid);
            if (w >= 0 && r >= 0 && (w + r) > 0) waitRatio = (float)w / (float)(w + r);
        }
        jankdiag::JankInput ji;
        ji.jank = frameaware::jankBoostVoteActive();
        ji.cpuThrottled = sp.cpuThrottled;
        ji.gpuThrottled = sp.gpuThrottled;
        ji.gpuUtilPct = sp.gpuUtilPct;
        ji.psiIoSome = sp.psiIoSomePct;
        ji.psiMemSome = sp.psiMemSomePct;
        ji.psiCpuSome = sp.psiCpuSomePct;
        ji.clusterUtil = maxCluster;
        ji.critWaitRatio = waitRatio;
        auto v = jankdiag::classify(ji);
        int cid = (int)v.cause;
        if (cid == g_prevCause.load()) g_debounce.fetch_add(1);
        else { g_debounce.store(0); g_prevCause.store(cid); }
        bool stable = g_debounce.load() >= 1;
        std::string line = std::string("cause=") + jankdiag::causeName(v.cause) +
                           " confidence=" + std::to_string(v.confidence) +
                           " evidence=" + v.evidence + "\n";
        writeFile((std::string(STATE_DIR) + "/jank_root_cause").c_str(), line);
        static const std::set<jankdiag::Cause> s_veto = {
            jankdiag::Cause::THERMAL_THROTTLE, jankdiag::Cause::GPU_BOUND,
            jankdiag::Cause::IO_STALL, jankdiag::Cause::MEM_RECLAIM };
        vetoActive = (stable && s_veto.count(v.cause));
    }

    diag::SystemPressure sp = diag::getSystemPressureSnapshot();
    float pfWaitRatio = -1.0f;
    for (const auto& ti : crit) {
        if (ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) {
            pfWaitRatio = placementThreadRqWaitRatio(ti.tid);
            break;
        }
    }
    bool pfThrottled = sp.cpuThrottled || sp.gpuThrottled || vetoActive ||
                       g_thermalEmergency.load(std::memory_order_relaxed);
    bool pfPsiArm   = sp.psiAvailable && sp.psiCpuSomePct >=
                      aatpe::placement::pressurefb::kPsiCpuArmPct;
    bool pfPsiClear = sp.psiAvailable && sp.psiCpuSomePct <
                      aatpe::placement::pressurefb::kPsiCpuClearPct;
    bool pfWaitArm   = pfWaitRatio >= aatpe::placement::pressurefb::kWaitArmRatio;
    bool pfWaitClear = pfWaitRatio <  aatpe::placement::pressurefb::kWaitClearRatio;
    static std::atomic<int> g_pfArmed{0};
    static std::atomic<int> g_pfArmStreak{0};
    static std::atomic<int> g_pfClearStreak{0};
    bool pfArmedPrev = g_pfArmed.load(std::memory_order_relaxed);
    bool pfArmedNow;
    if (pfThrottled) {
        g_pfArmStreak.store(0, std::memory_order_relaxed);
        pfArmedNow = false;
    } else if (pfArmedPrev) {
        if (pfPsiClear && pfWaitClear) {
            pfArmedNow = (g_pfClearStreak.fetch_add(1, std::memory_order_relaxed) + 1)
                         < aatpe::placement::pressurefb::kClearStreakReq;
            if (!pfArmedNow) g_pfArmStreak.store(0, std::memory_order_relaxed);
        } else {
            g_pfClearStreak.store(0, std::memory_order_relaxed);
            pfArmedNow = true;
        }
    } else {
        if (pfPsiArm || pfWaitArm) {
            pfArmedNow = (g_pfArmStreak.fetch_add(1, std::memory_order_relaxed) + 1)
                         >= aatpe::placement::pressurefb::kArmStreakReq;
            if (pfArmedNow) g_pfClearStreak.store(0, std::memory_order_relaxed);
        } else {
            g_pfArmStreak.store(0, std::memory_order_relaxed);
            pfArmedNow = false;
        }
    }
    g_pfArmed.store(pfArmedNow, std::memory_order_relaxed);

    std::vector<ThreadCand> cands;
    std::map<int, ThreadRole> tidRole;
    cands.reserve(crit.size());
    std::set<int> pfBoundedOnly;
    for (const auto& ti : crit) {
        bool allowed;
        if (fgKnown) {
            allowed = (ti.pname == fgPkg) ||
                      (!vmGuestPnames.empty() && vmGuestPnames.count(ti.pname));
        } else {
            bool latencyStarved =
                (ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) &&
                placementThreadRqWaitRatio(ti.tid) >=
                    aatpe::placement::pressurefb::kWaitArmRatio;
            bool manual = gamelist::isManualGame(ti.pname);
            allowed = manual || (pfArmedNow && latencyStarved);
            if (allowed && !manual && pfArmedNow) pfBoundedOnly.insert(ti.tid);
        }
        if (!allowed) continue;
        int curCpu = readThreadCurrentCpu(ti.pid, ti.tid);
        int tier = (curCpu >= 0) ? (int)tierOfCpu(curCpu) : (int)CpuTier::UNKNOWN;
        float weight = (ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) ? 90.0f : 60.0f;
        weight += std::min(1.0f, (float)ti.switchesPerSec / 30.0f) * 10.0f;
        cands.push_back({ti.tid, (int)ti.role, true, weight, tier});
        tidRole[ti.tid] = ti.role;
    }

    std::vector<PromoteTarget> targets = emergency ? std::vector<PromoteTarget>{} :
        decidePromotions(loads, cands, primeUtil, mode, spillover::kStableCycles,
                         pfArmedNow);
    int maxTier = 0;
    for (const auto& cl : clustersSnapshot()) maxTier = std::max(maxTier, (int)cl.tier);
    if (!emergency && (frameaware::jankBoostVoteActive() || pfArmedNow) && !cands.empty()) {
        auto heaviest = std::max_element(cands.begin(), cands.end(),
            [](const ThreadCand& a, const ThreadCand& b){ return a.weight < b.weight; });
        bool boundedOnly = pfBoundedOnly.count(heaviest->tid) > 0;
        if (heaviest->critical && heaviest->curTier < maxTier && !boundedOnly)
            targets.push_back({heaviest->tid, maxTier});
    }
    static std::atomic<bool> s_c3Suppressed{false};
    static std::atomic<int>  s_c3Streak{0};
    float c3Scale = g_thermalPlacementScale.load(std::memory_order_relaxed);
    bool c3Enabled = thermalSmoothEnabled.load(std::memory_order_relaxed);
    if (c3Enabled) {
        bool suppressed = s_c3Suppressed.load(std::memory_order_relaxed);
        int streak = s_c3Streak.load(std::memory_order_relaxed);
        if (!suppressed && c3Scale < 0.50f) {
            if (++streak >= 2) { s_c3Suppressed.store(true, std::memory_order_relaxed); s_c3Streak.store(0, std::memory_order_relaxed); }
            else s_c3Streak.store(streak, std::memory_order_relaxed);
        } else if (suppressed && c3Scale > 0.70f) {
            if (++streak >= 2) { s_c3Suppressed.store(false, std::memory_order_relaxed); s_c3Streak.store(0, std::memory_order_relaxed); }
            else s_c3Streak.store(streak, std::memory_order_relaxed);
        } else {
            s_c3Streak.store(0, std::memory_order_relaxed);
        }
    } else {
        s_c3Suppressed.store(false, std::memory_order_relaxed);
    }
    bool c3Suppress = s_c3Suppressed.load(std::memory_order_relaxed);
    if (!emergency && !cands.empty() && maxTier >= 2 &&
        !::aatpe::psi::bigcoreDeprefer() &&
        !efficiencySoftDepreferActive() &&
        !c3Suppress) {
        static constexpr int kMaxProactive = 2;
        const int bigTarget = maxTier - 1;
        std::set<int> already;
        for (const auto& t : targets) already.insert(t.tid);
        std::vector<const ThreadCand*> heavy;
        for (const auto& c : cands) {
            if (!c.critical) continue;
            if (c.role != ROLE_S2_RENDER && c.role != ROLE_S3_GAME) continue;
            if (c.curTier <= (int)CpuTier::UNKNOWN) continue;
            if (c.curTier >= bigTarget) continue;
            if (already.count(c.tid)) continue;
            heavy.push_back(&c);
        }
        std::stable_sort(heavy.begin(), heavy.end(),
            [](const ThreadCand* a, const ThreadCand* b){ return a->weight > b->weight; });
        int added = 0;
        for (const ThreadCand* c : heavy) {
            if (added >= kMaxProactive) break;
            targets.push_back({c->tid, bigTarget});
            added++;
        }
    }
    std::set<int> desiredSet;
    std::map<int,int> tidTier;
    int primeSlots = 1;
    {
        std::lock_guard<std::mutex> tlk(g_topologyRwMutex);
        int psz = (int)primePoolLocked().size();
        if (psz > 0) primeSlots = psz;
    }
    {
        int primeCount = 0;
        for (auto& t : targets) {
            if (t.targetTier >= maxTier) {
                if (primeCount >= primeSlots) t.targetTier = maxTier - 1;
                primeCount++;
            }
        }
    }
    for (auto& t : targets) { desiredSet.insert(t.tid); tidTier[t.tid] = t.targetTier; }

    long long nowMs = spillNowMs();
    long long dwell = (mode == 0) ? spillover::kDwellMsPassive
                    : (mode == 2) ? spillover::kDwellMsActive
                                  : spillover::kDwellMsAssist;
    {
        int hz = frameaware::getRefreshRate();
        if (hz > 0) dwell = dwell * 60 / hz;
    }
    aatpe_mech::SchedCaps caps = aatpe_mech::probeCapabilities();
    aatpe_mech::BoostMechanism mech = aatpe_mech::selectBoostMechanism(caps);
    int newly = 0;
    bool anyPromoted = false;
    std::map<int, std::string> preSavedCpuset;
    std::map<int, std::pair<std::string,std::string>> preCgroupPathHier;
    if (mech == aatpe_mech::BoostMechanism::CGROUP_UCLAMP ||
        mech == aatpe_mech::BoostMechanism::STUNE_CPUSET) {
        for (int tid : desiredSet) {
            preSavedCpuset[tid] = aatpe_mech::readThreadCpusetGroup(tid);
            std::string base, path, hier;
            if (aatpe_mech::readThreadCgroupInfo(tid, &base, &path, &hier)) {
                preCgroupPathHier[tid] = {path, hier};
            }
        }
    }
    {
        std::lock_guard<std::mutex> slk(g_spillStateMutex);
        if (vetoActive) {
            aatpe::placement::releaseAllSpilloverLockedImpl();
            aatpeBoostClear("placement");
            return 0;
        }
        for (int tid : desiredSet) {
            auto& st = g_spillState[tid];
            st.satisfyCycles++;
            if (!st.promoted && st.satisfyCycles >= spillover::kStableCycles &&
                !::aatpe::psi::noncriticalMigrationPaused()) {
                st.promoted = true;
                st.promotedMs = nowMs;
                st.targetTier = tidTier[tid];
                st.mechanism = (int)mech;
                std::string tname;
                {
                    std::lock_guard<std::mutex> tdbLock(g_threadsMutex);
                    auto thIt = g_threads.find(tid);
                    if (thIt != g_threads.end()) {
                        st.starttime = thIt->second.starttime;
                        tname = thIt->second.tname;
                    }
                }
                int umin = floorUclampForThreadLocked(tname.c_str(), st.targetTier,
                                                      ::aatpe::caps::autocalBigRatio(), mode);
                g_spilloverPromotions.fetch_add(1, std::memory_order_relaxed);
                g_spilloverActive.fetch_add(1, std::memory_order_relaxed);
                if (mech == aatpe_mech::BoostMechanism::UCLAMP) {
                    int rc = aatpe_mech::aatpeUclampFloorSet(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER, (unsigned int)umin);
                    if (rc != 0) {
                        st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                        st.mechanism = (int)aatpe_mech::BoostMechanism::AFFINITY;
                        if (!st.maskApplied) applyLastResortUclamp(tid, st, (unsigned int)umin);
                    }
                    st.rtBoosted = aatpe_priority::aatpeSetThreadPriority(tid, tidRole[tid],
                                                           cfgSnap.priorityBoostLevel);
                } else if (mech == aatpe_mech::BoostMechanism::CGROUP_UCLAMP) {
                    st.savedCpuset = preSavedCpuset[tid];
                    {
                        auto it = preCgroupPathHier.find(tid);
                        if (it != preCgroupPathHier.end()) {
                            aatpe_recovery::recoveryRecordCgroupPath(tid, it->second.first, it->second.second);
                        }
                    }
                    bool ok = aatpe_mech::writeCgroupUclampMin(tid,
                        (unsigned int)umin, "top-app");
                    if (!ok) ok = aatpe_mech::writeCgroupUclampMin(tid,
                        (unsigned int)umin, "top-app");
                    if (!ok) {
                        if (!st.savedCpuset.empty()) {
                            aatpe_mech::moveTidToCpusetGroup(tid, st.savedCpuset);
                        }
                        st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                        st.mechanism = (int)aatpe_mech::BoostMechanism::AFFINITY;
                        if (!st.maskApplied)
                            applyLastResortUclamp(tid, st, (unsigned int)umin);
                    }
                } else if (mech == aatpe_mech::BoostMechanism::STUNE_CPUSET) {
                    st.savedCpuset = preSavedCpuset[tid];
                    {
                        auto it = preCgroupPathHier.find(tid);
                        if (it != preCgroupPathHier.end()) {
                            aatpe_recovery::recoveryRecordCgroupPath(tid, it->second.first, it->second.second);
                        }
                    }
                    bool primeOk = (st.targetTier < (int)CpuTier::PRIME) ||
                                   (primePool().empty() ? false :
                                    aatpe_mech::topAppCpusContainsCpu(primePool()[0]));
                    bool joined = false;
                    if (primeOk) {
                        joined = aatpe_mech::moveTidToCpusetGroup(tid, "top-app");
                        if (!joined) joined = aatpe_mech::moveTidToCpusetGroup(tid, "top-app");
                    }
                    if (!joined) {
                        st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                        st.mechanism = (int)aatpe_mech::BoostMechanism::AFFINITY;
                        if (!st.maskApplied)
                            applyLastResortUclamp(tid, st, (unsigned int)umin);
                    }
                } else {
                    st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                    if (!st.maskApplied)
                        applyLastResortUclamp(tid, st, (unsigned int)umin);
                }
                if ((st.mechanism == (int)aatpe_mech::BoostMechanism::STUNE_CPUSET ||
                     st.mechanism == (int)aatpe_mech::BoostMechanism::CGROUP_UCLAMP) &&
                    !st.savedCpuset.empty()) {
                    st.origAffinity = "grp=" + st.savedCpuset;
                } else if (st.maskApplied) {
                    st.origAffinity = cpuSetToListStrLocal(st.savedMask);
                } else {
                    st.origAffinity = "-";
                }
                LOG_INFO("spillover: promote tid=%d to tier=%d (mode=%d via=%s)",
                         tid, st.targetTier, mode, aatpe_mech::mechanismName(
                             (aatpe_mech::BoostMechanism)st.mechanism));
                newly++;
            } else if (st.promoted) {
                reexertMembershipLocked(tid, st);
            }
        }
        if (g_fgEdgeReapply.exchange(0) != 0) {
            std::string edgePkg;
            { std::lock_guard<std::mutex> elk(g_fgEdgeMtx); edgePkg = g_fgEdgeReapplyPkg; }
            std::set<std::string> edgeVmGuests;
            if (isVmContainerPkg(edgePkg)) edgeVmGuests = resolveVmGuestPnames(edgePkg);
            for (auto& [tid, st] : g_spillState) {
                if (!st.promoted) continue;
                bool pkgMatch = true;
                {
                    std::lock_guard<std::mutex> tdbLock(g_threadsMutex);
                    auto thIt = g_threads.find(tid);
                    if (thIt != g_threads.end() && !edgePkg.empty() &&
                        thIt->second.pname != edgePkg &&
                        (edgeVmGuests.empty() || !edgeVmGuests.count(thIt->second.pname)))
                        pkgMatch = false;
                }
                if (!pkgMatch) continue;
                reexertMembershipLocked(tid, st);
            }
            LOG_INFO("spillover: fg-edge re-apply sweep done (pkg=%s)", edgePkg.c_str());
        }
        std::vector<int> keys;
        keys.reserve(g_spillState.size());
        for (auto& [tid, st] : g_spillState) keys.push_back(tid);
        for (int tid : keys) {
            auto it = g_spillState.find(tid);
            auto& st = it->second;
            if (!st.promoted) {
                if (!desiredSet.count(tid)) { st.satisfyCycles = 0; }
                continue;
            }
            if (desiredSet.count(tid)) continue;
            if (nowMs - st.promotedMs < dwell && !emergency) continue;
            {
                if (!spillDemoteIdentityMatch(tid, st.starttime)) {
                    LOG_INFO("spillover: demote tid=%d starttime mismatch "
                             "(recorded=%lld; thread gone or tid reused); "
                             "reclaiming stale record, not touching thread",
                             tid, (long long)st.starttime);
                    g_spilloverActive.fetch_sub(1, std::memory_order_relaxed);
                    g_spillState.erase(it);
                    continue;
                }
            }
            bool restoreOk = true;
            if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::UCLAMP)
                aatpe_mech::aatpeUclampFloorClear(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER);
            if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::CGROUP_UCLAMP)
                restoreOk = aatpe_mech::restoreTidFromCgroupUclamp(tid, "top-app", st.savedCpuset);
            if (st.maskApplied) { restoreAffinity(tid, st.savedMask); st.maskApplied = false; }
            if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::STUNE_CPUSET) {
                std::string live = aatpe_mech::readThreadCpusetGroup(tid);
                if (live.empty() || live == "top-app") {
                    std::string g = st.savedCpuset.empty() ? "foreground" : st.savedCpuset;
                    aatpe_mech::moveTidToCpusetGroup(tid, g);
                }
            }
            if (st.rtBoosted) {
                aatpe_priority::aatpeRestoreThreadPriority(tid);
                st.rtBoosted = false;
            }
            if (!restoreOk) {
                st.mechRestoreRetries++;
                LOG_WARN("spillover: demote tid=%d cgroup move-back failed (retry=%d); "
                         "keeping group floor up", tid, st.mechRestoreRetries);
            }
            g_spilloverActive.fetch_sub(1, std::memory_order_relaxed);
            LOG_INFO("spillover: demote tid=%d from PRIME (emergency=%d via=%s)", tid,
                     (int)emergency, aatpe_mech::mechanismName((aatpe_mech::BoostMechanism)st.mechanism));
            g_spillState.erase(it);
        }
        anyPromoted = !g_spillState.empty();
    }
    {
        bool degraded = (getSystemState() == SystemState::DEGRADED);
        int lvl = placementSchedtuneDesiredLevel(anyPromoted, emergency, degraded, mode);
        if (lvl < 0) aatpeBoostClear("placement");
        else         aatpeBoostSet("placement", lvl);
    }
    rewritePromotedTidsFile();
    return newly;
}
}

namespace aatpe { namespace placement {
void releaseAllSpilloverLocked() {
    std::lock_guard<std::mutex> lk(aatpe_spillover::g_spillStateMutex);
    aatpe_spillover::aatpe::placement::releaseAllSpilloverLockedImpl();
}
}}

namespace aatpe { namespace placement {
void releaseForegroundDownpins(const std::string& newFg) {
    if (newFg.empty() || newFg == "unknown") return;
    aatpe_mech::SchedCaps caps = aatpe_mech::probeCapabilities();
    std::vector<int> touched;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        for (auto& [tid, ti] : g_threads) {
            if (ti.pname != newFg) continue;
            bool criticalRole = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
            if (ti.affinityModified) {
                if (caps.cpusetTopApp) {
                    std::string live = aatpe_mech::readThreadCpusetGroup(tid);
                    if (!live.empty() && live != "top-app")
                        aatpe_mech::moveTidToCpusetGroup(tid, "top-app");
                }
                if (sched_setaffinity(tid, sizeof(cpu_set_t), &ti.originalAffinity) != 0) {
                    if (errno != ESRCH) LOG_WARN("releaseFG: tid=%d restore errno=%d", tid, errno);
                    continue;
                }
                ti.affinityModified = false;
                touched.push_back(tid);
            } else if (criticalRole) {
                if (caps.cpusetTopApp) {
                    std::string live = aatpe_mech::readThreadCpusetGroup(tid);
                    if (!live.empty() && live != "top-app")
                        aatpe_mech::moveTidToCpusetGroup(tid, "top-app");
                }
            }
        }
    }
    for (int tid : touched) { (void)tid; aatpe_spillover::g_fgSwitchRelease.fetch_add(1, std::memory_order_relaxed); }
}

static bool fgHealScreenOn() {
    FILE* f = fopen("/sys/class/graphics/fb0/blank", "r");
    if (!f) return true;
    int blank = 0;
    if (fscanf(f, "%d", &blank) != 1) { fclose(f); return true; }
    fclose(f);
    return (blank == 0);
}
static std::atomic<long long> g_lastFgCgroupHealMs{0};
void healForegroundCgroupMembership(const std::string& fgPkg) {
    if (fgPkg.empty() || fgPkg == "unknown") return;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = g_lastFgCgroupHealMs.load(std::memory_order_acquire);
    if (last != 0 && (nowMs - last) < 1000) return;
    g_lastFgCgroupHealMs.store(nowMs, std::memory_order_release);

    Config cfgSnap;
    snapshotConfig(cfgSnap);
    bool thermEmerg = g_thermalEmergency.load(std::memory_order_relaxed);
    int halLevel = aatpe::aatpeThermalHalLevel();
    bool screenOn = fgHealScreenOn();
    aatpe_mech::SchedCaps caps = aatpe_mech::probeCapabilities();

    std::string winMode = readFile((std::string(STATE_DIR) + "/frame_windowing_mode").c_str());
    bool windowingHealAllowed = (winMode == "fullscreen" || winMode == "unknown" || winMode.empty());
    if (!windowingHealAllowed) {
        return;
    }

    struct HealEntry {
        int tid;
        std::string pname;
        int role;
    };
    std::vector<HealEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        snapshot.reserve(g_threads.size());
        for (auto& [tid, ti] : g_threads) {
            snapshot.push_back({tid, ti.pname, (int)ti.role});
        }
    }
    for (auto& e : snapshot) {
        std::string live = aatpe_mech::readThreadCpusetGroup(e.tid);
        if (!healShouldLiftThread(e.pname, fgPkg, e.role, live,
                                  cfgSnap.controlMode, thermEmerg, halLevel,
                                  caps.cpusetTopApp, screenOn)) continue;
        if (aatpe_mech::moveTidToCpusetGroup(e.tid, "top-app")) {
            LOG_INFO("fg-heal: tid=%d pkg=%s lifted %s -> top-app (v4.4.1 P0)",
                     e.tid, fgPkg.c_str(), live.c_str());
        }
    }
}
}}

#include "placement.h"
namespace aatpe {
namespace placement {
std::uint64_t spilloverPromotionCount() noexcept {
    return static_cast<std::uint64_t>(
        aatpe_spillover::g_spilloverPromotions.load(std::memory_order_relaxed));
}
int spilloverActiveCount() noexcept {
    aatpe_mech::probeCapabilities();
    int v = aatpe_spillover::g_spilloverActive.load(std::memory_order_relaxed);
    return v > 0 ? v : 0;
}

void onInputEvent() noexcept {
    try {
        Config cfgSnap;
        snapshotConfig(cfgSnap);
        aatpe_spillover::placementSpilloverTick(cfgSnap);
    } catch (const std::exception& e) {
        LOG_WARN("onInputEvent: swallowed std::exception: %s", e.what());
    } catch (...) {
        LOG_WARN("onInputEvent: swallowed unknown exception");
    }
}

void onForegroundSwitchEdge(const std::string& newFg) noexcept {
    try {
        if (newFg.empty() || newFg == "unknown") return;
        {
            std::lock_guard<std::mutex> fgLock(g_foregroundMutex);
            g_cachedForegroundApp = newFg;
            g_foregroundCacheTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        {
            std::lock_guard<std::mutex> elk(aatpe_spillover::g_fgEdgeMtx);
            aatpe_spillover::g_fgEdgeReapplyPkg = newFg;
        }
        aatpe_spillover::g_fgEdgeReapply.store(1, std::memory_order_release);
    } catch (const std::exception& e) {
        LOG_WARN("onForegroundSwitchEdge: swallowed std::exception: %s", e.what());
    } catch (...) {
        LOG_WARN("onForegroundSwitchEdge: swallowed unknown exception");
    }
}
}
}

static void scoreOneCandidate(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                              const ScoringContext& scoreCtx,
                              bool mlActive, float mlAlpha,
                              bool adrcBoostActive, float adrcBoostUrgency,
                              bool adrcClampActive,
                              int cpuId,
                              int& outHeuristic, float& outFinalScore,
                              DecisionFusion& outFusion,
                              ThreadFeatures& outFeatures, bool& outFeaturesValid) {
    int heuristic = computeScore(cpuId, ti, liveCpu, scoreCtx);
    float finalScore = (float)heuristic;
    DecisionFusion fusion;
    ThreadFeatures features;
    bool featuresValid = false;
    if (mlActive || cfgSnap.mlHybridEnabled) {
        features = extractMlFeaturesCtx(ti, cpuId, scoreCtx);
        featuresValid = true;
    }

    int poolIdx = (int)getPoolForCpu(cpuId);
    bool poolColdStartBlocked = false;
    if (cfgSnap.mlColdStartProtection && poolIdx >= 0) {
        poolColdStartBlocked = !g_mlModel.isPoolReady(poolIdx);
    }

    fuseMlScore(cpuId, ti, heuristic, liveCpu, mlActive, mlAlpha, features,
                poolIdx, poolColdStartBlocked, finalScore, fusion);

    {
        int audioPen = getAudioThreadMigrationPenalty(ti.tname.c_str());
        if (audioPen > 0 && cpuId != g_audioIsolatedCpu.load(std::memory_order_relaxed)) {
            finalScore -= (float)audioPen * 0.1f;
        }
    }

    if (cfgSnap.mlHybridEnabled) {
        int hfPoolIdx = (int)getPoolForCpu(cpuId);
        if (hfPoolIdx >= 0) {
            if (!mlActive) {
                finalScore = normalizeHeuristicScore(heuristic);
            }
            float hf[15] = {0};
            hf[0] = 1.0f;
            unsigned long long avgRunUsProxy = (ti.nrSwitches > 0) ?
                (unsigned long long)((double)ti.runTime / (double)ti.nrSwitches / 1000.0) : 0;
            hf[1] = std::min(1.0f, (float)avgRunUsProxy / 50000.0f);
            hf[2] = std::min(1.0f, (float)ti.switchesPerSec / aatpe::hybrid_const::kSwitchesPerSecScale);
            hf[3] = std::min(1.0f, features.cpu_usage);
            hf[4] = std::min(1.0f, std::max(0.0f, features.priority));
            hf[5] = std::min(1.0f, std::max(0.0f, features.nice));
            hf[6] = features.is_game;
            hf[7] = std::min(1.0f, std::max(0.0f, features.thermal));
            int numPools = (int)clustersSnapshot().size();
            if (numPools < 1) numPools = 1;
            hf[8] = aatpe_ml_hybrid::poolFeatureForTier(hfPoolIdx, numPools);
            hf[9] = (liveCpu == cpuId) ? 1.0f : 0.0f;
            hf[10] = std::min(1.0f, std::max(0.0f, features.migration_count));
            hf[11] = hf[1] * hf[8];
            hf[12] = hf[1] * hf[3];
            hf[13] = hf[8] * hf[7];
            hf[14] = hf[1] * hf[7];
            float fineTune = aatpe_ml_hybrid::instance().getFineTuneAdjustment(
                hfPoolIdx, hf);
            finalScore += fineTune * 0.20f;
        }
    }

    if (getSystemState() != SystemState::EMERGENCY) {
        if (adrcBoostActive && getPoolForCpu(cpuId) == CpuPool::BIG) {
            finalScore += adrcBoostUrgency * 0.15f;
        }
        if (adrcClampActive && getPoolForCpu(cpuId) == CpuPool::BIG) {
            finalScore -= 0.05f;
        }
    }

    outHeuristic = heuristic;
    outFinalScore = finalScore;
    outFusion = fusion;
    outFeatures = features;
    outFeaturesValid = featuresValid;
}

void computeCandidateScores(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                            const cpu_set_t& allowedCpus, const std::vector<int>& allCpusCopy,
                            bool mlActive, float mlAlpha, const ScoringContext& scoreCtx,
                            bool adrcBoostActive, float adrcBoostUrgency, bool adrcClampActive,
                            int& bestCpu, float& bestFused, float& secondFused,
                            int& bestHeuristic, ThreadFeatures& bestFeatures,
                            DecisionFusion& bestFusion) {
    std::vector<int> candidates;
    candidates.reserve(allCpusCopy.size());
    for (int cpuId : allCpusCopy) {
        if (isCpuAllowed(cpuId, allowedCpus) && isCpuOnlineNow(cpuId)) candidates.push_back(cpuId);
    }
    for (int cpuId : candidates) {
        int heuristic; float finalScore; DecisionFusion fusion;
        ThreadFeatures features; bool featuresValid;
        scoreOneCandidate(ti, cfgSnap, liveCpu, scoreCtx, mlActive, mlAlpha,
                         adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                         cpuId, heuristic, finalScore, fusion, features, featuresValid);

        if (finalScore > bestFused && featuresValid) {
            bestFeatures = features;
        }

        selectOptimalPlacement(cpuId, finalScore, heuristic, fusion, featuresValid, features,
                               bestCpu, bestFused, secondFused, bestHeuristic,
                               bestFusion, bestFeatures);
    }
}

void applyColdStartExploration(float mlAlpha, bool mlActive, int liveCpu,
                               const cpu_set_t& allowedCpus,
                               const std::vector<int>& allCpusCopy,
                               int& bestCpu, Decision& d) {
    bool coldStart = (mlAlpha < 0.1f);
    if (!coldStart && mlActive) {
        std::lock_guard<std::mutex> mlLock2(g_mlMutex);
        coldStart = (g_mlModel.getTotalSamples() < 100);
    }
    if (coldStart && bestCpu >= 0 && allCpusCopy.size() > 1) {
        static std::atomic<unsigned int> exploreSeed{12345};
        unsigned int seed = exploreSeed.load(std::memory_order_relaxed);
        seed = seed * 1103515245u + 12345u;
        exploreSeed.store(seed, std::memory_order_relaxed);
        if ((seed % 100) < 10) {
            int randIdx = (seed >> 16) % allCpusCopy.size();
            int exploreCpu = allCpusCopy[randIdx];
            if (isCpuAllowed(exploreCpu, allowedCpus) && exploreCpu != liveCpu) {
                bestCpu = exploreCpu;
                d.reason = "COLD_EXPLORE";
            }
        }
    }
}

static void recomputeScoreForTarget(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                                    const ScoringContext& scoreCtx,
                                    bool mlActive, float mlAlpha,
                                    bool adrcBoostActive, float adrcBoostUrgency,
                                    bool adrcClampActive,
                                    int bestCpu,
                                    int& outBestHeuristic, float& outBestFused,
                                    float& outSecondFused,
                                    ThreadFeatures& outBestFeatures,
                                    DecisionFusion& outBestFusion) {
    if (bestCpu < 0) return;
    {
        int heuristic; float finalScore; DecisionFusion fusion;
        ThreadFeatures features; bool featuresValid;
        scoreOneCandidate(ti, cfgSnap, liveCpu, scoreCtx, mlActive, mlAlpha,
                         adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                         bestCpu, heuristic, finalScore, fusion, features, featuresValid);
        outBestHeuristic = heuristic;
        outBestFused = finalScore;
        outBestFusion = fusion;
        if (featuresValid) outBestFeatures = features;
    }
    {
        int curHeuristic; float curFinal; DecisionFusion curFusion;
        ThreadFeatures curFeatures; bool curValid;
        scoreOneCandidate(ti, cfgSnap, liveCpu, scoreCtx, mlActive, mlAlpha,
                         adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                         liveCpu, curHeuristic, curFinal, curFusion, curFeatures, curValid);
        outSecondFused = curFinal;
    }
}

bool thresholdGate(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                   int bestCpu, int bestHeuristic, float confidence,
                   const ScoringContext& scoreCtx, Decision& d) {
    int currentCpu = liveCpu;
    int thermalPressure = g_thermalMigrationPressure.load(std::memory_order_relaxed);
    int effectiveCostThreshold = cfgSnap.migrationCostThreshold;
    if (thermalPressure > 0) {
        effectiveCostThreshold += (cfgSnap.migrationCostThreshold * thermalPressure) / 100;
    }
    int currentScore = computeScore(currentCpu, ti, liveCpu, scoreCtx);
    int gain = bestHeuristic - currentScore;

    {
        float relGainPct = currentScore > 0
            ? (float)gain * 100.0f / (float)currentScore
            : 0.0f;
        int conf0_100 = (int)(confidence * 100.0f);
        if (conf0_100 < 0) conf0_100 = 0;
        if (conf0_100 > 100) conf0_100 = 100;
        placementAvgRecordGain(relGainPct, conf0_100);
    }

    {
        bool isLatencySensitive = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER ||
                                    ti.role == ROLE_S3_GAME);
        if (isLatencySensitive && cfgSnap.minUsefulCapacityRatio > 0.0f) {
            int bestCap = 0, curCap = 0, maxCap = 0;
            {
                std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
                auto bit = g_cpuMetrics.find(bestCpu);
                if (bit != g_cpuMetrics.end()) bestCap = bit->second.capacity;
                auto cit = g_cpuMetrics.find(currentCpu);
                if (cit != g_cpuMetrics.end()) curCap = cit->second.capacity;
                for (const auto& kv : g_cpuMetrics) {
                    if (kv.second.capacity > maxCap) maxCap = kv.second.capacity;
                }
            }
            if (maxCap <= 0) maxCap = aatpe::topology::kMaxCapacityRef;
            if (bestCap <= 0) bestCap = aatpe::topology::kDefaultCapacity;
            if (curCap <= 0) curCap = aatpe::topology::kDefaultCapacity;

            int minUsefulCap = (int)((float)maxCap * cfgSnap.minUsefulCapacityRatio);
            if (bestCap < minUsefulCap && curCap >= minUsefulCap) {
                bool threadActive = (ti.runTime > 0);
                if (threadActive) {
                    d.action = DecisionAction::KEEP;
                    d.reason = "INSUFFICIENT_CAP:" + std::to_string(bestCap) +
                               "<" + std::to_string(minUsefulCap);
                    g_keepLowConf++;
                    return true;
                }
            }
        }
    }

    {
        std::string fg = getCachedForegroundApp();
        bool fgKnown = !fg.empty() && fg != "unknown";
        int curPool = (int)getPoolForCpu(currentCpu);
        int tgtPool = (int)getPoolForCpu(bestCpu);
        bool downPin = (tgtPool < curPool);
        if (downPin) {
            uint64_t nowNs = (uint64_t)aatpe_spillover::spillNowMs() * 1000000ULL;
            bool quiet = (ti.lastSwitchStampNs > 0) &&
                         (nowNs - ti.lastSwitchStampNs >= 5000ULL*1000000ULL);
            bool targetIsFg = (fgKnown && ti.pname == fg);
            bool block = (targetIsFg || !fgKnown) &&
                         !(ti.role == ROLE_S5_BACKGROUND && quiet);
            if (block) {
                d.action = DecisionAction::KEEP;
                d.reason = "FG_NO_DOWNPIN";
                aatpe_spillover::g_fgDownpinBlocked.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (targetIsFg && (aatpe_spillover::g_downpinDiagCount++ & 63) == 0) {
                LOG_WARN("A6 downpin fg tid=%d comm=%s pkg=%s pool %d->%d role=%d quiet=%d",
                         ti.tid, ti.tname.c_str(), ti.pname.c_str(),
                         curPool, tgtPool, (int)ti.role, (int)quiet);
            }
        }
    }

    bool isHighPriority = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER ||
                           ti.role == ROLE_S3_GAME);
    CpuPool curPool = getPoolForCpu(currentCpu);
    CpuPool bestPool = getPoolForCpu(bestCpu);
    bool targetHigherPool = false;
    if (bestPool == CpuPool::BIG && (curPool == CpuPool::MID || curPool == CpuPool::LITTLE))
        targetHigherPool = true;
    else if (bestPool == CpuPool::MID && curPool == CpuPool::LITTLE)
        targetHigherPool = true;
    int perfGainForceThreshold = 15;
    if (targetHigherPool) {
        if (curPool == CpuPool::LITTLE && bestPool == CpuPool::MID)
            perfGainForceThreshold = 10;
        else if (curPool == CpuPool::MID && bestPool == CpuPool::BIG)
            perfGainForceThreshold = 15;
    }
    bool forceMigrate = (isHighPriority && targetHigherPool && gain >= perfGainForceThreshold);
    int hpEffectiveThreshold = effectiveCostThreshold;
    if (isHighPriority && targetHigherPool) {
        hpEffectiveThreshold = 1;
    }
    if (!forceMigrate && gain < hpEffectiveThreshold) {
        d.action = DecisionAction::KEEP; d.reason = "HIGH_COST:" + std::to_string(gain); g_keepHighCost++; return true;
    }
    bool mlMature = false;
    {
        std::lock_guard<std::mutex> mlLock(g_mlMutex);
        mlMature = g_mlModel.isMature();
    }
    if (mlMature) {
        float confidencePercent = confidence * 1000.0f;
        if (cfgSnap.confidenceThreshold > 0 && confidencePercent < (float)cfgSnap.confidenceThreshold) {
            d.action = DecisionAction::KEEP;
            d.reason = "LOW_CONF:" + std::to_string((int)confidencePercent);
            g_keepLowConf++;
            return true;
        }
    }
    return false;
}

std::mutex g_settleMutex;
std::unordered_map<int, ThreadSettleState> g_threadSettle;
std::atomic<long long> g_settleMigrations{0};
std::atomic<long long> g_settleHysteresisRejects{0};
std::atomic<long long> g_settleCooldownRejects{0};

static long long settleNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool placementHysteresisGate(int tid, int currentCpu, int bestCpu,
                             int currentHeuristic, int bestHeuristic,
                             const Config& cfg, Decision& d) {
    if (tid <= 0 || bestCpu < 0 || currentCpu < 0) return false;
    if (bestCpu == currentCpu) return false;
    const int pct = (cfg.migrationHysteresisPercent < 0) ? 0
                  : (cfg.migrationHysteresisPercent > 100 ? 100
                   : cfg.migrationHysteresisPercent);
    const long long cooldownMs = (cfg.migrationCooldownMs < 0) ? 0 : cfg.migrationCooldownMs;
    const long long nowMs = settleNowMs();

    std::lock_guard<std::mutex> lk(g_settleMutex);
    auto it = g_threadSettle.find(tid);
    if (it == g_threadSettle.end()) {
        ThreadSettleState st;
        st.settledCpu = currentCpu;
        st.settledScore = currentHeuristic;
        st.settledMs = nowMs;
        g_threadSettle[tid] = st;
        return false;
    }
    ThreadSettleState& st = it->second;

    if (st.settledCpu != currentCpu) {
        if (getPoolForCpu(st.settledCpu) != getPoolForCpu(currentCpu)) {
            st.settledCpu = currentCpu;
            st.settledScore = currentHeuristic;
            st.settledMs = nowMs;
        }
        return false;
    }

    if (cooldownMs > 0 && (nowMs - st.settledMs) < cooldownMs) {
        d.action = DecisionAction::KEEP;
        d.reason = "SETTLE_COOLDOWN:" + std::to_string(cooldownMs - (nowMs - st.settledMs)) + "ms";
        g_settleCooldownRejects.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (pct > 0) {
        long long required = (long long)st.settledScore +
                             ((long long)st.settledScore * pct) / 100;
        if (st.settledScore <= 0) required = st.settledScore + 2;
        if ((long long)bestHeuristic < required) {
            d.action = DecisionAction::KEEP;
            d.reason = "SETTLE_HYST:" + std::to_string(bestHeuristic) + "<" +
                       std::to_string(required) + "(settled=" +
                       std::to_string(st.settledScore) + ")";
            g_settleHysteresisRejects.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void placementRecordMigration(int tid, int cpu, int heuristicScore) {
    if (tid <= 0 || cpu < 0) return;
    std::lock_guard<std::mutex> lk(g_settleMutex);
    ThreadSettleState& st = g_threadSettle[tid];
    st.settledCpu = cpu;
    st.settledScore = heuristicScore;
    st.settledMs = settleNowMs();
    g_settleMigrations.fetch_add(1, std::memory_order_relaxed);
    if (g_threadSettle.size() > 1024) {
        auto oldest = g_threadSettle.begin();
        for (auto it = g_threadSettle.begin(); it != g_threadSettle.end(); ++it) {
            if (it->second.settledMs < oldest->second.settledMs) oldest = it;
        }
        g_threadSettle.erase(oldest);
    }
}

void placementInvalidateSettle(int tid) {
    if (tid <= 0) return;
    std::lock_guard<std::mutex> lk(g_settleMutex);
    g_threadSettle.erase(tid);
}

long long placementGetAvgSettleDwellMs() {
    std::lock_guard<std::mutex> lk(g_settleMutex);
    if (g_threadSettle.empty()) return 0;
    long long nowMs = settleNowMs();
    long long sum = 0;
    int n = 0;
    for (const auto& kv : g_threadSettle) {
        if (kv.second.settledMs > 0) {
            sum += (nowMs - kv.second.settledMs);
            n++;
        }
    }
    return (n > 0) ? (sum / n) : 0;
}

Decision decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency) {
    Decision d;
    d.targetCpu = -1;
    d.score = 0;
    d.confidence = 0;

    if (ti.protect || ti.role == ROLE_S0_SYSTEM) {
        d.action = DecisionAction::KEEP; d.reason = "PROTECTED"; g_keepProtected++; return d;
    }
    if (isKernelThread(ti.tname, ti.tid)) {
        d.isKernel = true;
        d.action = DecisionAction::KEEP; d.reason = "KERNEL_THREAD"; return d;
    }
    {
        std::lock_guard<std::mutex> lock(g_unmigratableMutex);
        if (g_unmigratableThreads.count(ti.tid)) {
            d.action = DecisionAction::KEEP; d.reason = "UNMIGRATABLE"; return d;
        }
    }

    if (!emergency) {
        if (g_thermalEmergency.load(std::memory_order_relaxed)) {
            d.action = DecisionAction::KEEP; d.reason = "THERMAL_EMERGENCY"; g_keepThermalEmergency++; return d;
        }
        if (g_safetyLatencyActive.load(std::memory_order_relaxed)) {
            d.action = DecisionAction::KEEP; d.reason = "SAFETY_LATENCY"; g_keepSafetyLatency++; return d;
        }
        if (aatpe::subsys::safemode::isActive()) {
            d.action = DecisionAction::KEEP; d.reason = "SAFE_MODE"; g_keepSafeMode++; return d;
        }
        if (cfg.controlMode == "passive") {
            d.action = DecisionAction::KEEP; d.reason = "PASSIVE"; g_keepPassive++; return d;
        }
    }

    float mlAlpha = 0.0f;
    bool mlActive = false;
    {
        std::lock_guard<std::mutex> lock(g_mlMutex);
        mlActive = g_mlModel.isEnabled() && cfg.mlLearningEnabled;
        if (mlActive) mlAlpha = g_mlModel.getAlpha();
    }
    if (mlActive && aatpe_ml::g_mlDriftDetected.load(std::memory_order_relaxed)) {
        mlAlpha = 0.0f;
    }

    int bestCpu = -1;
    float bestFused = -std::numeric_limits<float>::max();
    float secondFused = -std::numeric_limits<float>::max();
    int bestHeuristic = 0;
    ThreadFeatures bestFeatures{};
    DecisionFusion bestFusion;

    cpu_set_t allowedCpus = getThreadAllowedCpus(ti.tid);
    if (CPU_COUNT(&allowedCpus) == 0) CPU_ZERO(&allowedCpus);

    std::vector<int> allCpusCopy;
    {
        std::lock_guard<std::mutex> rlock(g_topologyRwMutex);
        allCpusCopy = g_allCpus;
    }

    int liveCpu = readThreadCurrentCpu(ti.pid, ti.tid);
    if (liveCpu < 0) liveCpu = ti.currentCpu;

    std::unique_lock<std::mutex> mlLoopLock;
    if (mlActive) {
        mlLoopLock = std::unique_lock<std::mutex>(g_mlMutex);
    }

    ScoringContext scoreCtx = buildScoringContext(cfg);

    AdrcOutput adrcOut{};
    bool adrcActive = false;
    bool adrcBoostActive = false;
    float adrcBoostUrgency = 0.0f;
    bool adrcClampActive = false;
    if (ti.role >= ROLE_S1_UI && ti.role <= ROLE_S3_GAME) {
        AdrcRole adrcRole = mapThreadRoleToAdrc(ti.role);
        adrcOut = getRoleOutput(adrcRole);
        adrcActive = (adrcOut.urgency > 0.001 || adrcOut.riskScore > 0.001);
        if (adrcActive && adrcOut.shouldBoost) {
            adrcBoostActive = true;
            adrcBoostUrgency = (float)adrcOut.urgency;
        }
    } else if (ti.role >= ROLE_S4_THROUGHPUT) {
        adrcClampActive = allCanClamp();
    }

    computeCandidateScores(ti, cfg, liveCpu, allowedCpus, allCpusCopy,
                          mlActive, mlAlpha, scoreCtx,
                          adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                          bestCpu, bestFused, secondFused, bestHeuristic,
                          bestFeatures, bestFusion);

    int loopBestCpu = bestCpu;

    if (mlLoopLock.owns_lock()) {
        mlLoopLock.unlock();
    }

    applyColdStartExploration(mlAlpha, mlActive, liveCpu, allowedCpus,
                              allCpusCopy, bestCpu, d);

    if (bestCpu < 0) {
        d.action = DecisionAction::KEEP; d.reason = "NO_CPU"; g_keepNoCpu++; return d;
    }

    bestCpu = applySmartPlacementFallback(bestCpu, ti, cfg, allowedCpus);

    if (bestCpu != loopBestCpu) {
        std::lock_guard<std::mutex> mlRecomputeLock(g_mlMutex);
        recomputeScoreForTarget(ti, cfg, liveCpu, scoreCtx,
                                mlActive, mlAlpha,
                                adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                                bestCpu,
                                bestHeuristic, bestFused, secondFused,
                                bestFeatures, bestFusion);
    }

    if (adrcActive && adrcOut.riskScore > 0.5) {
        secondFused -= (float)adrcOut.riskScore * 0.02f;
    }

    float confidence = bestFused - secondFused;

    int currentCpu = liveCpu;
    if (currentCpu == bestCpu) {
        d.action = DecisionAction::KEEP; d.reason = "ALREADY_OPTIMAL"; g_keepAlreadyOptimal++; return d;
    }

    if (!emergency) {
    if (thresholdGate(ti, cfg, liveCpu, bestCpu, bestHeuristic, confidence, scoreCtx, d)) {
        return d;
    }

    {
        int currentHeuristic = computeScore(liveCpu, ti, liveCpu, scoreCtx);
        if (placementHysteresisGate(ti.tid, liveCpu, bestCpu,
                                    currentHeuristic, bestHeuristic,
                                    cfg, d)) {
            return d;
        }
    }
    }

    if (mlActive && LearningModel::isAllZeroFeatures(bestFeatures)) {
        mlActive = false;
    }
    d.mlFeatures = bestFeatures;
    d.mlActive = mlActive;
    d.targetPool = getPoolForCpu(bestCpu);
    d.action = DecisionAction::MIGRATE;
    d.targetCpu = bestCpu;
    d.score = bestHeuristic;
    {
        int c = (int)(confidence * 1000);
        if (c < 0) c = 0;
        if (c > 1000) c = 1000;
        d.confidence = c;
    }
    d.reason = "Score:" + std::to_string(bestHeuristic) + " MLAlpha:" + std::to_string(mlAlpha)
             + " Pool:" + getPoolName(d.targetPool);
    if (adrcActive) {
        d.reason += " ADRC:u=" + std::to_string((int)(adrcOut.urgency * 100))
                  + "% r=" + std::to_string((int)(adrcOut.riskScore * 100))
                  + (adrcOut.shouldBoost ? " B" : "")
                  + (adrcOut.shouldClamp ? " C" : "");
    }
    d.fusion = bestFusion;
    g_fusionWeightAccum.record(bestFusion);
    g_migrateActual++;
    return d;
}

void protectCriticalThreads() {
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    if (cfgSnap.controlMode == "passive") return;
    if (g_thermalEmergency.load()) return;
    std::vector<int> allCpusSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        allCpusSnap = g_allCpus;
    }
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    for (auto&_sb_item : g_threads) {
        auto& tid = _sb_item.first; auto& ti = _sb_item.second;
        if (ti.role != ROLE_S0_SYSTEM) continue;
        if (isKthreaddChild(tid)) continue;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (int cpu : allCpusSnap) CPU_SET(cpu, &mask);
        int pret = sched_setaffinity(tid, sizeof(mask), &mask);
        if (pret != 0) {
            int e = errno; char ebuf[128];
            LOG_DEBUG("protectCriticalThreads: sched_setaffinity failed for tid=%d errno=%d (%s)",
                      tid, e, aatpeStrError(e, ebuf, sizeof(ebuf)));
            (void)e; (void)ebuf;
        }
    }
}

bool isKthreaddChild(int tid) {
    std::string statPath = "/proc/" + std::to_string(tid) + "/stat";
    std::ifstream f(statPath);
    if (!f) return false;
    std::string content;
    std::getline(f, content);
    size_t paren = content.rfind(')');
    if (paren == std::string::npos) return false;
    std::string rest = content.substr(paren + 1);
    std::stringstream ss(rest);
    std::string state, ppidStr;
    ss >> state >> ppidStr;
    try {
        int ppid = std::stoi(ppidStr);
        return (ppid == 2);
    } catch (...) {
        static int n = 0; if ((++n & 0x3F) == 1)
            LOG_DEBUG("isKernelThread: ppid field='%s' unparsable (throttled)", ppidStr.c_str());
        return false;
    }
}

bool isKernelThread(const std::string& tname, int tid) {
    static const char* kExactKernelThreads[] = {
        "kthreadd",
        "mm_percpu_wq",
        "kswapd0",
        "khugepaged",
        "oom_reaper",
        "writeback",
        "kdevtmpfs",
        "netns",
        "ipv6_addrconf",
        "zswap",
        "edac-poller"
    };
    static const char* kKernelThreadPrefixes[] = {
        "migration/",
        "rcu_",
        "ksoftirqd/",
        "kworker/",
        "watchdog/",
        "cpuhp/",
        "crypto/",
        "kintegrityd",
        "kblockd",
        "devfreq_",
        "perf/",
        "rcuop/",
        "rcub/",
        "charger_m",
        "rcuog/",
        "rcuos/",
        "irq/",
        "usb_",
        "phy-",
        "regulator/",
        "clocksou",
        "workqueue",
        "ksmd",
        "zram",
        "loop",
        "fsnotify",
        "kaudit",
        "inet_frag",
        "kmpath",
        "charge_",
        "ext4-",
        "f2fs-",
        "jbd2/",
        "md/",
        "dm-",
        "scsi_",
        "ubi/",
        "ubifs/",
        "yaffs-",
        "mtdblock",
        "nfsiod",
        "cifs/",
        "nfsd",
        "lockd/",
        "rpciod/",
        "sunrpc",
        "hci/",
        "btusb",
        "l2cap",
        "sco/",
        "rfcomm",
        "cfg80211",
        "mac80211",
        "wlan_",
        "hostap_",
        "wpa_s",
        "xt_"
    };

    if (!tname.empty()) {
        for (const char* name : kExactKernelThreads) {
            if (tname == name) return true;
        }
        for (const char* prefix : kKernelThreadPrefixes) {
            size_t plen = strlen(prefix);
            if (tname.size() >= plen && tname.compare(0, plen, prefix) == 0) return true;
        }
    }

    if (tid > 0) {
        std::string cmdlinePath = "/proc/" + std::to_string(tid) + "/cmdline";
        std::ifstream cf(cmdlinePath, std::ios::binary);
        if (cf) {
            char ch = 0;
            cf.get(ch);
            bool cmdlineEmpty = !cf.good();
            cf.close();
            if (cmdlineEmpty) return true;
        }
    }
    return false;
}
int findPidByPackage(const std::string& pkgName) {
    if (pkgName.empty() || pkgName == "unknown") return -1;
    DIR* procDir = opendir("/proc");
    if (!procDir) return -1;
    struct dirent* entry;
    int foundPid = -1;
    while ((entry = readdir(procDir)) != nullptr) {
        char* endptr = nullptr;
        long pidLong = strtol(entry->d_name, &endptr, 10);
        if (endptr == entry->d_name || *endptr != '\0' || pidLong <= 0 || pidLong > INT_MAX) continue;
        int pid = (int)pidLong;
        std::string cmdlinePath = "/proc/" + std::to_string(pid) + "/cmdline";
        std::ifstream cf(cmdlinePath);
        if (!cf) continue;
        std::string procName;
        std::getline(cf, procName, '\0');
        if (procName == pkgName) {
            foundPid = pid;
            break;
        }
    }
    closedir(procDir);
    return foundPid;
}

static std::mutex g_migLatchMutex;
struct MigLatchEntry { long long lastChangeMs; bool lastWasUphill; };
static std::unordered_map<int, MigLatchEntry> g_migLatch;
static constexpr long long kNormalCrossPathResidencyMs = 1000;
static constexpr long long kEmergencyResidencyMs      = 1500;
static constexpr long long kPostUphillResidencyMs      = 2000;
static std::atomic<long long> g_latchRejectNormal{0};
static std::atomic<long long> g_latchRejectEmergency{0};

static long long migLatchNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool migLatchQueryLocked(int tid, long long nowMs, bool emergency) {
    auto it = g_migLatch.find(tid);
    if (it == g_migLatch.end()) return true;
    long long elapsed = nowMs - it->second.lastChangeMs;
    if (it->second.lastWasUphill && !emergency && elapsed < kPostUphillResidencyMs) {
        return false;
    }
    long long minGap = emergency ? kEmergencyResidencyMs : kNormalCrossPathResidencyMs;
    return elapsed >= minGap;
}

static void migLatchStampLocked(int tid, long long nowMs, bool uphill) {
    g_migLatch[tid] = MigLatchEntry{nowMs, uphill};
    if (g_migLatch.size() > 1024) {
        auto oldest = g_migLatch.begin();
        for (auto it = g_migLatch.begin(); it != g_migLatch.end(); ++it) {
            if (it->second.lastChangeMs < oldest->second.lastChangeMs) oldest = it;
        }
        g_migLatch.erase(oldest);
    }
}

bool applyPlacement(const ThreadInfo& ti, int targetCpu, const std::string& reason, const Config& cfg, bool emergency) {
    if (!emergency && cfg.controlMode == "passive") { g_applyPassive++; return false; }
    if (!emergency && g_thermalEmergency.load(std::memory_order_relaxed)) { g_applyThermalEmergency++; return false; }
    if (!emergency && g_safetyLatencyActive.load(std::memory_order_relaxed)) { g_applySafetyLatency++; return false; }
    if (ti.role == ROLE_S0_SYSTEM) return false;
    if (targetCpu < 0) { return false; }
    if (g_topologyRebuilding.load(std::memory_order_acquire) && !isCpuOnlineNow(targetCpu)) {
        g_applyCpusetSkip++;
        LOG_DEBUG("[PLACE REBUILD_SKIP] tid=%d target=%d offline during topology rebuild",
                  ti.tid, targetCpu);
        return false;
    }
    const uint64_t genAtApplyStart = g_topologyGeneration.load(std::memory_order_acquire);
    if (isKernelThread(ti.tname, ti.tid)) { g_applyKernelSkip++; return false; }
    {
        std::lock_guard<std::mutex> lock(g_unmigratableMutex);
        if (g_unmigratableThreads.count(ti.tid)) { g_applyUnmigratableSkip++; return false; }
    }
    time_t now = time(nullptr);
    cpu_set_t allowedCpus = getThreadAllowedCpus(ti.tid);
    if (CPU_COUNT(&allowedCpus) == 0) CPU_ZERO(&allowedCpus);
    bool isFrameCritical = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
    RateLimitGuard rateGuard(isFrameCritical, emergency);
    if (!emergency && !rateGuard.acquired()) return false;

    {
        if (!isCpuAllowed(targetCpu, allowedCpus)) {
            g_applyCpusetSkip++;
            {
                std::lock_guard<std::mutex> lk(g_lastCpusetSkipMutex);
                g_lastCpusetSkip.tid = ti.tid;
                g_lastCpusetSkip.pid = ti.pid;
                g_lastCpusetSkip.targetCpu = targetCpu;
                g_lastCpusetSkip.maskCount = CPU_COUNT(&allowedCpus);
                snprintf(g_lastCpusetSkip.pname, sizeof(g_lastCpusetSkip.pname),
                         "%s", ti.pname.c_str());
            }
            LOG_DEBUG("[PLACE CPUSET_SKIP] %d/%d target CPU%d not in allowed cpuset (mask count=%d)",
                     ti.pid, ti.tid, targetCpu, CPU_COUNT(&allowedCpus));
            {
                static std::atomic<long long> s_lastSkipLogMs{0};
                static std::atomic<uint64_t> s_skipCount{0};
                s_skipCount.fetch_add(1, std::memory_order_relaxed);
                long long nowSkipMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                long long lastSkip = s_lastSkipLogMs.load(std::memory_order_relaxed);
                if (nowSkipMs - lastSkip >= 60000) {
                    if (s_lastSkipLogMs.compare_exchange_strong(lastSkip, nowSkipMs,
                                                                std::memory_order_relaxed)) {
                        LOG_INFO("[PLACE CPUSET_SKIP] throttled: %llu skips in last 60s "
                                 "(last: tid=%d targetCPU=%d maskCount=%d)",
                                 (unsigned long long)s_skipCount.exchange(0, std::memory_order_relaxed),
                                 ti.tid, targetCpu, CPU_COUNT(&allowedCpus));
                    }
                }
            }
            return false;
        }
    }

    if (!emergency) {
        std::lock_guard<std::mutex> lock(g_lastMigMutex);
        auto it = g_lastMigrationTime.find(ti.tid);
        if (it != g_lastMigrationTime.end() && (now - it->second) < cfg.migrationDwellSec) {
            g_applyIntervalSkip++;
            return false;
        }
    }
    {
        long long nowLatchMs = migLatchNowMs();
        std::lock_guard<std::mutex> lk(g_migLatchMutex);
        if (!migLatchQueryLocked(ti.tid, nowLatchMs, emergency)) {
            if (emergency) g_latchRejectEmergency.fetch_add(1, std::memory_order_relaxed);
            else           g_latchRejectNormal.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_migFailMutex);
        if (now - g_cooldownFloorWindowStart >= 1) {
            g_cooldownFloorWindowStart = now;
            g_cooldownBypassCount.store(0, std::memory_order_relaxed);
        }
        bool bypassByFloor = (g_cooldownBypassCount.load(std::memory_order_relaxed) < kMigCooldownFloorPerSec);

        long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            static std::atomic<unsigned> s_backoffSweepCounter{0};
            unsigned prevSweep = s_backoffSweepCounter.fetch_add(1, std::memory_order_relaxed);
            if (prevSweep + 1u >= 64u) {
                s_backoffSweepCounter.store(0, std::memory_order_relaxed);
                for (auto cit = g_migFailBackoffUntilMs.begin(); cit != g_migFailBackoffUntilMs.end(); ) {
                    if (nowMs >= cit->second) {
                        cit = g_migFailBackoffUntilMs.erase(cit);
                    } else {
                        ++cit;
                    }
                }
            }
            const size_t kMigBackoffMax = 1024;
            while (g_migFailBackoffUntilMs.size() > kMigBackoffMax) {
                auto victim = g_migFailBackoffUntilMs.begin();
                long long oldest = victim->second;
                for (auto it2 = g_migFailBackoffUntilMs.begin();
                     it2 != g_migFailBackoffUntilMs.end(); ++it2) {
                    if (it2->second < oldest) {
                        oldest = it2->second;
                        victim = it2;
                    }
                }
                g_migFailBackoffUntilMs.erase(victim);
            }
        }

        auto it = g_migFailBackoffUntilMs.find(ti.tid);
        if (it != g_migFailBackoffUntilMs.end()) {
            bool backoffActive = (nowMs < it->second);
            bool cpuChanged = false;
            auto cpuIt = g_migFailCpu.find(ti.tid);
            if (cpuIt != g_migFailCpu.end() && cpuIt->second != ti.currentCpu) {
                cpuChanged = true;
            }
            if (backoffActive && !cpuChanged && !bypassByFloor) {
                g_applyCooldownSkip++;
                return false;
            }
            g_migFailBackoffUntilMs.erase(it);
            if (cpuChanged) {
                g_migFailCount.erase(ti.tid);
                g_migFailCpu.erase(ti.tid);
            }
        }
        if (bypassByFloor) {
            g_cooldownBypassCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::vector<int> snapLittle, snapMid, snapBig, snapAll;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        snapLittle = g_littlePool;
        snapMid    = g_midPool;
        snapBig    = g_bigPool;
        snapAll    = g_allCpus;
    }
    cpu_set_t littleMask, midMask, bigMask, allMask;
    CPU_ZERO(&littleMask); CPU_ZERO(&midMask); CPU_ZERO(&bigMask); CPU_ZERO(&allMask);
    for (int c : snapLittle) CPU_SET(c, &littleMask);
    for (int c : snapMid)    CPU_SET(c, &midMask);
    for (int c : snapBig)    CPU_SET(c, &bigMask);
    for (int c : snapAll)    CPU_SET(c, &allMask);

    cpu_set_t poolMask;
    CPU_ZERO(&poolMask);
    bool poolFound = false;
    if (CPU_ISSET(targetCpu, &littleMask))      { poolMask = littleMask; poolFound = true; }
    else if (CPU_ISSET(targetCpu, &midMask))    { poolMask = midMask;    poolFound = true; }
    else if (CPU_ISSET(targetCpu, &bigMask))   { poolMask = bigMask;    poolFound = true; }
    if (!poolFound) poolMask = allMask;

    cpu_set_t cpusetIntersected;
    CPU_ZERO(&cpusetIntersected);
    CPU_AND(&cpusetIntersected, &poolMask, &allowedCpus);

    std::vector<int> onlineCpus = getOnlineCpus();
    cpu_set_t onlineMask;
    CPU_ZERO(&onlineMask);
    for (int cpu : onlineCpus) CPU_SET(cpu, &onlineMask);
    cpu_set_t finalMask;
    CPU_ZERO(&finalMask);
    CPU_AND(&finalMask, &cpusetIntersected, &onlineMask);

    if (CPU_COUNT(&finalMask) == 0) {
        g_applyCpusetSkip++;
        LOG_WARN("[PLACE SAFE_ABANDON] %d/%d target CPU%d: triple intersection (pool∩allowed∩online) is empty. Skipping migration (no fallback expansion).",
                 ti.pid, ti.tid, targetCpu);
        return false;
    }

    cpu_set_t savedOrigMask;
    CPU_ZERO(&savedOrigMask);
    bool needOrigSnapshot = !ti.affinityModified;
    int savedErrno = 0;
    if (needOrigSnapshot) {
        if (sched_getaffinity(ti.tid, sizeof(savedOrigMask), &savedOrigMask) != 0) {
            savedErrno = errno;
            if (savedErrno != ESRCH) g_applyFailed++;
            switch (savedErrno) {
                case EINVAL: g_applyFailedEinval++; break;
                case ESRCH:  g_applyFailedEsrch++;  break;
                case EPERM:  g_applyFailedEperm++;  break;
                default:     g_applyFailedOther++;  break;
            }
            if (savedErrno != ESRCH) {
                std::lock_guard<std::mutex> lock(g_migFailMutex);
                int& fails = g_migFailCount[ti.tid];
                fails++;
                g_migFailCpu[ti.tid] = ti.currentCpu;
                long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                g_migFailBackoffUntilMs[ti.tid] = nowMs + migFailBackoffMsForFail(fails);
            }
            return false;
        }
    }

    int ret = sched_setaffinity(ti.tid, sizeof(finalMask), &finalMask);
    savedErrno = errno;
    if (ret == 0) {
        const bool generationInvalidated =
            (g_topologyGeneration.load(std::memory_order_acquire) != genAtApplyStart);
        if (needOrigSnapshot && !generationInvalidated) {
            std::lock_guard<std::mutex> tdbLock(g_threadsMutex);
            auto thIt = g_threads.find(ti.tid);
            if (thIt != g_threads.end()) {
                thIt->second.originalAffinity = savedOrigMask;
                thIt->second.affinityModified = true;
            }
        }
        if (needOrigSnapshot) {
            std::string origList = cpuSetToListStrLocal(savedOrigMask);
            std::string appliedList = cpuSetToListStrLocal(finalMask);
            unsigned long long st = 0;
            {
                std::lock_guard<std::mutex> tdbLock(g_threadsMutex);
                auto thIt = g_threads.find(ti.tid);
                if (thIt != g_threads.end()) st = thIt->second.starttime;
            }
            aatpe_recovery::recoveryRecordAffinity(ti.pid, ti.tid, st, origList, appliedList);
        } else if (!generationInvalidated) {
            std::string appliedList = cpuSetToListStrLocal(finalMask);
            if (!aatpe_recovery::recoveryRefreshAppliedAffinity(ti.tid, appliedList)) {
                LOG_WARN("recoveryRefreshAppliedAffinity failed tid=%d", ti.tid);
            }
        }
        rateGuard.consume();
        g_totalMigrations++;
        g_applySuccess++;
        {
            std::lock_guard<std::mutex> lock(g_migFailMutex);
            g_migFailCount.erase(ti.tid);
            g_migFailCooldown.erase(ti.tid);
            g_migFailCpu.erase(ti.tid);
            g_migFailBackoffUntilMs.erase(ti.tid);
        }
        {
            std::lock_guard<std::mutex> lock(g_lastMigMutex);
            g_lastMigrationTime[ti.tid] = now;
        }
        {
            int latchFromPool = (int)getPoolForCpu(ti.currentCpu);
            int latchToPool   = (int)getPoolForCpu(targetCpu);
            bool uphill = (latchToPool > latchFromPool);
            std::lock_guard<std::mutex> lk(g_migLatchMutex);
            migLatchStampLocked(ti.tid, migLatchNowMs(), uphill);
        }
        {
            ScoringContext scoreCtx = buildScoringContext(cfg);
            placementRecordMigration(ti.tid, targetCpu, computeScore(targetCpu, ti, targetCpu, scoreCtx));
        }
        LOG_DEBUG("[PLACE] %d/%d -> CPU%d [%d] %s", ti.pid, ti.tid, targetCpu, (int)ti.role, reason.c_str());
        bool lowRoleConf = (ti.roleConfidence < cfg.threadRoleConfidenceThreshold);
        if (cfg.priorityBoostEnabled && !lowRoleConf &&
            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME)) {
            aatpe_priority::aatpeSetThreadPriority(ti.tid, ti.role, cfg.priorityBoostLevel);
        }
        if (cfg.logDecisions) {
            static std::atomic<unsigned long long> s_decisionSeq{0};
            long long decNowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            unsigned long long seq = s_decisionSeq.fetch_add(1, std::memory_order_relaxed);
            std::string decPath = std::string(DECISION_DIR) + "/" + std::to_string(ti.pid) + "_" + std::to_string(ti.tid) + "_" + std::to_string(decNowMs) + "_" + std::to_string(seq);
            std::string decContent = "pid=" + std::to_string(ti.pid) + "\ntid=" + std::to_string(ti.tid) + "\ntarget_cpu=" + std::to_string(targetCpu) + "\nrole=" + std::to_string((int)ti.role) + "\nreason=" + reason + "\ntimestamp=" + std::to_string(now) + "\n";
            writeFile(decPath.c_str(), decContent);
        }
        return true;
    } else {
        if (savedErrno != ESRCH) g_applyFailed++;
        LOG_DEBUG("[PLACE] applyPlacement failed errno=%d on tid %d; kernel preserved prior affinity mask (no rollback to originalAffinity).",
                  savedErrno, ti.tid);
        switch (savedErrno) {
            case EINVAL: g_applyFailedEinval++; break;
            case ESRCH:  g_applyFailedEsrch++;  break;
            case EPERM:  g_applyFailedEperm++;  break;
            default:     g_applyFailedOther++;  break;
        }
        bool markUnmigratable = false;
        {
            std::lock_guard<std::mutex> lock(g_migFailMutex);
            int& fails = g_migFailCount[ti.tid];
            fails++;
            g_migFailCpu[ti.tid] = ti.currentCpu;
            if (savedErrno == EINVAL) {
                if (fails >= kMigFailTriggerThreshold) {
                    markUnmigratable = true;
                }
            }
            if (savedErrno != ESRCH) {
                long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                g_migFailBackoffUntilMs[ti.tid] = nowMs + migFailBackoffMsForFail(fails);
            }
        }
        if (markUnmigratable) {
            std::lock_guard<std::mutex> ulock(g_unmigratableMutex);
            g_unmigratableThreads.insert(ti.tid);
        }
        if (savedErrno == ESRCH) {
            static std::atomic<int> s_esrchLogCounter{0};
            int c = s_esrchLogCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            if (c >= 100) {
                LOG_INFO("[PLACE ESRCH] %d/%d -> CPU%d (thread exited mid-scan, normal; "
                         "counted %d since start)", ti.pid, ti.tid, targetCpu, c);
                s_esrchLogCounter.store(0, std::memory_order_relaxed);
            }
        } else {
            char ebuf[128];
            LOG_WARN("[PLACE FAIL] %d/%d -> CPU%d errno=%d (%s)", ti.pid, ti.tid, targetCpu, savedErrno,
                     aatpeStrError(savedErrno, ebuf, sizeof(ebuf)));
        }
        return false;
    }
}

void cleanupDecisionsDir() {
    DIR* dir = opendir(DECISION_DIR);
    if (!dir) return;

    struct dirent* entry;
    int fileCount = 0;
    std::vector<std::pair<long long, std::string>> files;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        fileCount++;
        std::string fname = entry->d_name;
        size_t lastUnderscore = fname.rfind('_');
        if (lastUnderscore != std::string::npos) {
            char* endptr = nullptr;
            long long seq = std::strtoll(fname.substr(lastUnderscore + 1).c_str(), &endptr, 10);
            if (endptr && *endptr == '\0') {
                files.push_back({seq, fname});
            }
        }
    }
    closedir(dir);

    const int kMaxDecisionFiles = 500;
    if (fileCount <= kMaxDecisionFiles) return;

    std::sort(files.begin(), files.end());
    int toDelete = files.size() / 2;
    int cleaned = 0;
    for (int i = 0; i < toDelete; i++) {
        std::string fpath = std::string(DECISION_DIR) + "/" + files[i].second;
        unlink(fpath.c_str());
        cleaned++;
    }
    if (cleaned > 0) LOG_DEBUG("fix96: Lazy cleanup deleted %d oldest decision files (total=%d)", cleaned, fileCount);
}

static std::unordered_map<int, CpuPool> g_poolCache;
static std::mutex g_poolCacheMutex;
static void invalidatePoolCache() {
    std::lock_guard<std::mutex> lock(g_poolCacheMutex);
    g_poolCache.clear();
}

CpuPool getPoolForCpu(int cpu) {
    if (cpu < 0) return CpuPool::UNKNOWN;
    std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
    {
        std::lock_guard<std::mutex> lock(g_poolCacheMutex);
        auto it = g_poolCache.find(cpu);
        if (it != g_poolCache.end()) return it->second;
    }
    CpuPool pool = CpuPool::UNKNOWN;
    if (std::find(g_littlePool.begin(), g_littlePool.end(), cpu) != g_littlePool.end())
        pool = CpuPool::LITTLE;
    else if (std::find(g_midPool.begin(), g_midPool.end(), cpu) != g_midPool.end())
        pool = CpuPool::MID;
    else if (std::find(g_bigPool.begin(), g_bigPool.end(), cpu) != g_bigPool.end())
        pool = CpuPool::BIG;
    else {
        for (const auto& cl : g_clusters) {
            if (cl.tier == CpuTier::PRIME &&
                std::find(cl.cpus.begin(), cl.cpus.end(), cpu) != cl.cpus.end()) {
                pool = CpuPool::BIG;
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_poolCacheMutex);
        g_poolCache[cpu] = pool;
    }
    return pool;
}

std::vector<int> getPoolCpus(CpuPool pool) {
    std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
    switch (pool) {
        case CpuPool::LITTLE: return g_littlePool;
        case CpuPool::MID:    return g_midPool;
        case CpuPool::BIG:    return g_bigPool;
        default:               return {};
    }
}

int computeCoreSuitability(int cpuId, ThreadRole role, int currentLoadPct,
                           float coreAffW, float loadW, float tempW) {
    if (cpuId < 0) return 0;

    CpuPool pool = getPoolForCpu(cpuId);
    int coreTypeScore = 50;
    if (role == ROLE_S5_BACKGROUND) {
        switch (pool) {
            case CpuPool::LITTLE: coreTypeScore = 100; break;
            case CpuPool::MID:    coreTypeScore = 60;  break;
            case CpuPool::BIG:    coreTypeScore = 20;  break;
            default:              coreTypeScore = 50;   break;
        }
    } else if (role == ROLE_S4_THROUGHPUT) {
        switch (pool) {
            case CpuPool::LITTLE: coreTypeScore = 40; break;
            case CpuPool::MID:    coreTypeScore = 70; break;
            case CpuPool::BIG:    coreTypeScore = 80; break;
            default:              coreTypeScore = 50; break;
        }
    } else {
        switch (pool) {
            case CpuPool::LITTLE: coreTypeScore = 20; break;
            case CpuPool::MID:    coreTypeScore = 60;  break;
            case CpuPool::BIG:    coreTypeScore = 100; break;
            default:              coreTypeScore = 50;  break;
        }
    }

    int load = currentLoadPct;
    if (load < 0) load = 0;
    if (load > 100) load = 100;
    int loadScore = 100 - load;

    int cpuTemp = 0;
    {
        std::lock_guard<std::mutex> ml(g_cpuMetricsMutex);
        auto it = g_cpuMetrics.find(cpuId);
        if (it != g_cpuMetrics.end()) cpuTemp = it->second.temp;
    }
    if (cpuTemp < 0) cpuTemp = 0;
    if (cpuTemp > 100) cpuTemp = 100;
    int tempScore = 100 - cpuTemp;

    float wsum = coreAffW + loadW + tempW;
    if (wsum <= 0.0f) wsum = 1.0f;
    float score = (coreTypeScore * coreAffW + loadScore * loadW + tempScore * tempW) / wsum;
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    return static_cast<int>(std::lround(score));
}

static int applySmartPlacementFallback(int bestCpu, const ThreadInfo& ti,
                                       const Config& cfgSnap,
                                       const cpu_set_t& allowedCpus) {
    if (!cfgSnap.smartPlacementEnabled) return bestCpu;
    if (bestCpu < 0) return bestCpu;
    if (ti.role == ROLE_S0_SYSTEM) return bestCpu;

    int bestLoad = 0, bestTemp = 0;
    {
        std::lock_guard<std::mutex> ml(g_cpuMetricsMutex);
        auto it = g_cpuMetrics.find(bestCpu);
        if (it != g_cpuMetrics.end()) {
            bestLoad = it->second.util;
            bestTemp = it->second.temp;
        }
    }

    CpuPool bestPool = getPoolForCpu(bestCpu);
    int criticalWall = cfgSnap.thermalCriticalThreshold > 0
                       ? cfgSnap.thermalCriticalThreshold
                       : cfgSnap.thermalLimitCpu;

    static std::mutex g_smartLatchMutex;
    struct SmartLatch { long long pulledBackMs; };
    static std::unordered_map<int, SmartLatch> g_smartPullbackLatch;
    const int kPullbackTriggerPct = 85;
    const int kPullbackReleasePct = 65;
    const long long kPullbackHoldMs = 3000;

    auto lightestAllowedMid = [&]() -> int {
        std::vector<int> midCpus = getPoolCpus(CpuPool::MID);
        int bestMid = -1, bestMidLoad = 101;
        for (int c : midCpus) {
            if (!isCpuAllowed(c, allowedCpus)) continue;
            int l = 0;
            {
                std::lock_guard<std::mutex> ml(g_cpuMetricsMutex);
                auto it = g_cpuMetrics.find(c);
                if (it != g_cpuMetrics.end()) l = it->second.util;
            }
            if (l < bestMidLoad) { bestMidLoad = l; bestMid = c; }
        }
        return bestMid;
    };

    bool isFgCritical = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
    if (isFgCritical && bestPool == CpuPool::BIG) {
        long long nowSmartMs = migLatchNowMs();
        bool latched = false;
        {
            std::lock_guard<std::mutex> lk(g_smartLatchMutex);
            auto it = g_smartPullbackLatch.find(ti.tid);
            if (it != g_smartPullbackLatch.end()) {
                long long since = nowSmartMs - it->second.pulledBackMs;
                if (since >= kPullbackHoldMs && bestLoad < kPullbackReleasePct) {
                    g_smartPullbackLatch.erase(it);
                } else {
                    latched = true;
                }
            }
        }
        bool tempWall = (criticalWall > 0 && bestTemp >= criticalWall);
        bool overloaded = (bestLoad > kPullbackTriggerPct) || tempWall;

        if (latched) {
            int mid = lightestAllowedMid();
            if (mid >= 0) return mid;
        } else if (overloaded) {
            int mid = lightestAllowedMid();
            if (mid >= 0) {
                int midLoad = 0;
                {
                    std::lock_guard<std::mutex> ml(g_cpuMetricsMutex);
                    auto it = g_cpuMetrics.find(mid);
                    if (it != g_cpuMetrics.end()) midLoad = it->second.util;
                }
                if (bestLoad - midLoad > 20) {
                    {
                        std::lock_guard<std::mutex> lk(g_smartLatchMutex);
                        g_smartPullbackLatch[ti.tid] = SmartLatch{nowSmartMs};
                        if (g_smartPullbackLatch.size() > 1024) {
                            auto oldest = g_smartPullbackLatch.begin();
                            for (auto e = g_smartPullbackLatch.begin(); e != g_smartPullbackLatch.end(); ++e)
                                if (e->second.pulledBackMs < oldest->second.pulledBackMs) oldest = e;
                            g_smartPullbackLatch.erase(oldest);
                        }
                    }
                    LOG_DEBUG("[SMART] tid=%d role=%d bestCpu=%d overloaded(load=%d,temp=%d) -> midCpu=%d (pullback latched 3s)",
                              ti.tid, (int)ti.role, bestCpu, bestLoad, bestTemp, mid);
                    return mid;
                }
            }
        }
    }

    if (ti.role == ROLE_S5_BACKGROUND && bestPool == CpuPool::BIG && bestLoad > 30) {
        std::vector<int> littleCpus = getPoolCpus(CpuPool::LITTLE);
        if (!littleCpus.empty()) {
            int bestLittle = -1;
            int bestLittleLoad = 101;
            for (int c : littleCpus) {
                if (!isCpuAllowed(c, allowedCpus)) continue;
                int l = 0;
                {
                    std::lock_guard<std::mutex> ml(g_cpuMetricsMutex);
                    auto it = g_cpuMetrics.find(c);
                    if (it != g_cpuMetrics.end()) l = it->second.util;
                }
                if (l < bestLittleLoad) { bestLittleLoad = l; bestLittle = c; }
            }
            if (bestLittle >= 0) {
                LOG_DEBUG("[SMART] tid=%d BACKGROUND bigCpu=%d load=%d -> littleCpu=%d(load=%d)",
                          ti.tid, bestCpu, bestLoad, bestLittle, bestLittleLoad);
                return bestLittle;
            }
        }
    }

    return bestCpu;
}

const char* getPoolName(CpuPool pool) {
    switch (pool) {
        case CpuPool::LITTLE: return "LITTLE";
        case CpuPool::MID:    return "MID";
        case CpuPool::BIG:    return "BIG";
        default:               return "UNKNOWN";
    }
}

static std::vector<int> g_onlineCpusCache;
static std::mutex g_onlineCpusCacheMutex;

#ifdef AATPE_HOST_TEST
void setTopologyDebounceMsForTest(int ms) { g_topologyDebounceMs.store(ms); }
#endif
int topologyRebuildCount() { return s_topologyRebuildCount.load(); }
bool isTopologyRebuilding() { return g_topologyRebuilding.load(); }

bool isCpuOnlineNow(int cpu) {
    std::lock_guard<std::mutex> lk(g_onlineCpusCacheMutex);
    for (int c : g_onlineCpusCache) if (c == cpu) return true;
    return false;
}

void setOnlineCpusCache(const std::vector<int>& cpus) {
    std::lock_guard<std::mutex> lk(g_onlineCpusCacheMutex);
    g_onlineCpusCache = cpus;
}

static std::vector<int> getOnlineCpusRaw() {
    std::vector<int> result;
    std::string onlineStr = readFirstLine("/sys/devices/system/cpu/online");
    if (!onlineStr.empty()) {
        cpu_set_t mask = parseCpusetCpuList(onlineStr);
        for (int c = 0; c < CPU_SETSIZE; c++) {
            if (CPU_ISSET(c, &mask)) result.push_back(c);
        }
        if (!result.empty()) return result;
    }
    std::string possibleStr = readFirstLine("/sys/devices/system/cpu/possible");
    if (!possibleStr.empty()) {
        cpu_set_t mask = parseCpusetCpuList(possibleStr);
        for (int c = 0; c < CPU_SETSIZE; c++) {
            if (CPU_ISSET(c, &mask)) result.push_back(c);
        }
        if (!result.empty()) return result;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = (g_cpuCount > 0) ? g_cpuCount : 8;
    for (int c = 0; c < n; c++) result.push_back(c);
    return result;
}

std::vector<int> getOnlineCpus() {
    {
        std::lock_guard<std::mutex> lk(g_onlineCpusCacheMutex);
        if (!g_onlineCpusCache.empty()) return g_onlineCpusCache;
    }
    return getOnlineCpusRaw();
}

void seedOnlineCacheFromSysfs() {
    std::vector<int> online = getOnlineCpusRaw();
    if (online.empty()) return;
    std::sort(online.begin(), online.end());
    setOnlineCpusCache(online);
}

void refreshCpuTopology() {
    std::vector<int> online = getOnlineCpusRaw();
    if (online.empty()) return;
    std::sort(online.begin(), online.end());
    setOnlineCpusCache(online);
    bool changed;
    int cpuCountAfter = 0;
    size_t littleSz = 0, midSz = 0, bigSz = 0;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        changed = (online.size() != g_allCpus.size());
        if (!changed) {
            for (size_t i = 0; i < online.size(); i++) {
                if (online[i] != g_allCpus[i]) { changed = true; break; }
            }
        }
    }
    if (changed) {
        long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long lastRebuild = s_lastTopologyRebuildMs.load(std::memory_order_relaxed);
        int debounceMs = g_topologyDebounceMs.load(std::memory_order_relaxed);
        if (debounceMs > 0 && (nowMs - lastRebuild) < debounceMs) {
            LOG_DEBUG("TOPOLOGY-CHANGE: debounced (last rebuild %lldms ago < %dms), skipping",
                      nowMs - lastRebuild, debounceMs);
            return;
        }
        struct RebuildGuard {
            ~RebuildGuard() { g_topologyRebuilding.store(false, std::memory_order_release); }
        } guard;
        g_topologyRebuilding.store(true, std::memory_order_release);
        detectCpuTopology();
        s_lastTopologyRebuildMs.store(nowMs, std::memory_order_relaxed);
        s_topologyRebuildCount.fetch_add(1, std::memory_order_relaxed);
        invalidatePoolCache();
        invalidateAllScores();
        {
            std::lock_guard<std::mutex> histLock(g_historyMutex);
            g_history.clear();
        }
        {
            std::lock_guard<std::mutex> settleLock(g_settleMutex);
            g_threadSettle.clear();
        }
        precomputeCpuBaseScores();
        g_topologyGeneration.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("TOPOLOGY-CHANGE: full cache recalculation done (scores+history+settle+base)");
        {
            std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
            cpuCountAfter = g_cpuCount;
            littleSz = g_littlePool.size();
            midSz = g_midPool.size();
            bigSz = g_bigPool.size();
        }
        LOG_INFO("CPU topology refreshed: %d CPUs online, pools rebuilt (L=%zu M=%zu B=%zu)",
                 cpuCountAfter, littleSz, midSz, bigSz);
        atomicWriteFile((std::string(STATE_DIR) + "/cpu_count").c_str(), std::to_string(cpuCountAfter));
        std::string cpuListStr;
        for (size_t i = 0; i < online.size(); i++) {
            if (i > 0) cpuListStr += ",";
            cpuListStr += std::to_string(online[i]);
        }
        atomicWriteFile((std::string(STATE_DIR) + "/online_cpus").c_str(), cpuListStr);
    }
}

std::atomic<long long> g_lastUnmigratableReprobe{0};

void reprobeUnmigratableThreads() {
    time_t now = time(nullptr);
    long long last = g_lastUnmigratableReprobe.load(std::memory_order_relaxed);
    if ((long long)now - last < kUnmigratableReprobeIntervalSec) return;
    g_lastUnmigratableReprobe.store((long long)now, std::memory_order_relaxed);
    std::vector<int> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_unmigratableMutex);
        if (g_unmigratableThreads.empty()) return;
        snapshot.assign(g_unmigratableThreads.begin(), g_unmigratableThreads.end());
    }
    int removed = 0;
    std::vector<int> toRemove;
    for (int tid : snapshot) {
        std::string commPath = "/proc/" + std::to_string(tid) + "/comm";
        struct stat st;
        if (stat(commPath.c_str(), &st) != 0) {
            toRemove.push_back(tid);
            removed++;
            continue;
        }
        std::string tname = readFirstLine(commPath.c_str());
        trim(tname);
        if (isKernelThread(tname, tid)) {
            continue;
        }
        cpu_set_t probeMask;
        CPU_ZERO(&probeMask);
        if (sched_getaffinity(tid, sizeof(probeMask), &probeMask) == 0) {
            if (CPU_COUNT(&probeMask) > 1) {
                std::vector<int> allCpusSnap;
                {
                    std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
                    allCpusSnap = g_allCpus;
                }
                bool hasPoolIntersection = false;
                for (int cpu : allCpusSnap) {
                    if (CPU_ISSET(cpu, &probeMask)) {
                        hasPoolIntersection = true;
                        break;
                    }
                }
                if (hasPoolIntersection) {
                    toRemove.push_back(tid);
                    removed++;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_unmigratableMutex);
        for (int tid : toRemove) {
            g_unmigratableThreads.erase(tid);
        }
    }
    for (int tid : toRemove) {
        std::lock_guard<std::mutex> fl(g_migFailMutex);
        g_migFailCount.erase(tid);
        g_migFailCooldown.erase(tid);
        g_migFailCpu.erase(tid);
        g_migFailBackoffUntilMs.erase(tid);
    }
    if (removed > 0) {
        LOG_INFO("Unmigratable re-probe: removed %d threads", removed);
    }
}

static std::atomic<long long> g_lastEmergencyMigrate{
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()};
static constexpr int kEmergencyCooldownMs = 2000;
static std::atomic<long long> g_emergencyMigrations{0};

static constexpr int kEmergencyPerMinute = 10;
static std::mutex g_emergWindowMutex;
static long long g_emergWindowStartMs = 0;
static int g_emergWindowCount = 0;

struct EmergencyRec { long long toMs; int fromPool; int toPool; };
static std::unordered_map<int, EmergencyRec> g_lastEmergencyRec;

static int rolePriority(ThreadRole r) {
    switch (r) {
        case ROLE_S1_UI: return 0;
        case ROLE_S2_RENDER: return 1;
        case ROLE_S3_GAME: return 2;
        default: return 99;
    }
}

static std::vector<ThreadInfo> collectCriticalThreads(int maxCount,
        const std::set<std::string>* priorityPnames) {
    struct Cand { int tid; ThreadRole role; bool protect; };
    std::vector<Cand> cands;
    cands.reserve(16);
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (const auto& [tid, ti] : g_threads) {
            if (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) {
                cands.push_back({tid, ti.role, ti.protect});
            }
        }
    }
    std::vector<int> survivors;
    survivors.reserve(cands.size());
    {
        std::lock_guard<std::mutex> ulock(g_unmigratableMutex);
        for (const auto& c : cands) {
            if (!c.protect && !g_unmigratableThreads.count(c.tid)) {
                survivors.push_back(c.tid);
            }
        }
    }
    std::vector<ThreadInfo> criticalThreads;
    criticalThreads.reserve(survivors.size());
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (int tid : survivors) {
            auto it = g_threads.find(tid);
            if (it != g_threads.end()) criticalThreads.push_back(it->second);
        }
    }
    auto isPrio = [&](const ThreadInfo& t) {
        return priorityPnames && priorityPnames->count(t.pname) > 0;
    };
    std::sort(criticalThreads.begin(), criticalThreads.end(),
        [&](const ThreadInfo& a, const ThreadInfo& b) {
            bool pa = isPrio(a), pb = isPrio(b);
            if (pa != pb) return pa;
            return rolePriority(a.role) < rolePriority(b.role);
        });
    if ((int)criticalThreads.size() > maxCount) {
        criticalThreads.resize(maxCount);
    }
    return criticalThreads;
}

int emergencyMigrateCriticalThreads() {
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long lastMs = g_lastEmergencyMigrate.load(std::memory_order_relaxed);
    long long elapsedMs = nowMs - lastMs;
    if (elapsedMs < kEmergencyCooldownMs) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> wlk(g_emergWindowMutex);
        if (g_emergWindowStartMs == 0 || nowMs - g_emergWindowStartMs >= 60000) {
            g_emergWindowStartMs = nowMs;
            g_emergWindowCount = 0;
        }
        if (g_emergWindowCount >= kEmergencyPerMinute) {
            return 0;
        }
    }

    g_lastEmergencyMigrate.store(nowMs, std::memory_order_relaxed);

    const float c3Scale = g_thermalPlacementScale.load(std::memory_order_relaxed);
    const bool thermalSuppressed = (c3Scale < 0.50f);

    int migrated = 0;
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    std::vector<ThreadInfo> criticalThreads = collectCriticalThreads(3);
    for (const auto& ti : criticalThreads) {
        Decision d = decidePlacement(ti, cfgSnap, true);
        if (d.action != DecisionAction::MIGRATE) continue;

        int liveCpu = readThreadCurrentCpu(ti.pid, ti.tid);
        if (liveCpu < 0) liveCpu = ti.currentCpu;
        int fromPoolE = (int)getPoolForCpu(liveCpu);
        int toPoolE   = (int)getPoolForCpu(d.targetCpu);
        if (thermalSuppressed && toPoolE > fromPoolE) {
            continue;
        }
        {
            auto eit = g_lastEmergencyRec.find(ti.tid);
            if (eit != g_lastEmergencyRec.end()) {
                long long since = nowMs - eit->second.toMs;
                if (since < 5000 && toPoolE == eit->second.fromPool) {
                    continue;
                }
            }
        }

        bool ok = applyPlacement(ti, d.targetCpu, "emergency_jank", cfgSnap, true);
        if (ok) {
            migrated++;
            std::lock_guard<std::mutex> wlk(g_emergWindowMutex);
            g_emergWindowCount++;
            g_lastEmergencyRec[ti.tid] = EmergencyRec{nowMs, fromPoolE, toPoolE};
            if (g_lastEmergencyRec.size() > 1024) {
                auto oldest = g_lastEmergencyRec.begin();
                for (auto e = g_lastEmergencyRec.begin(); e != g_lastEmergencyRec.end(); ++e)
                    if (e->second.toMs < oldest->second.toMs) oldest = e;
                g_lastEmergencyRec.erase(oldest);
            }
        }
    }
    if (migrated > 0) {
        g_emergencyMigrations.fetch_add(migrated, std::memory_order_relaxed);
        LOG_WARN("EMERGENCY MIGRATION (jank): migrated %d critical threads (total=%lld)",
                 migrated, (long long)g_emergencyMigrations.load());
    }
    writeFile((std::string(STATE_DIR) + "/emergency_migrations").c_str(),
              g_emergencyMigrations.load(std::memory_order_relaxed));
    return migrated;
}

int lightweightMigrateCriticalThreads() {
    static std::atomic<long long> s_lastCallMs{0};
    auto now = std::chrono::steady_clock::now();
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    if (nowMs - s_lastCallMs.load() < 8) return 0;
    s_lastCallMs.store(nowMs);

    Config cfgSnap;
    snapshotConfig(cfgSnap);
    if (cfgSnap.controlMode == "passive") return 0;

    int migrated = 0;
    std::vector<ThreadInfo> criticalThreads = collectCriticalThreads(1);

    for (const auto& ti : criticalThreads) {
        if (migrated >= 1) break;

        Decision d = decidePlacement(ti, cfgSnap);
        if (d.action == DecisionAction::MIGRATE && d.targetCpu >= 0) {
            if (applyPlacement(ti, d.targetCpu, d.reason, cfgSnap)) {
                migrated++;
            }
        }
    }

    return migrated;
}

int proactiveMigrationCheck() {
    static std::atomic<long long> s_lastCheckMs{0};
    static std::atomic<bool> s_inSpillover{false};
    bool expected = false;
    if (!s_inSpillover.compare_exchange_strong(expected, true)) return 0;
    struct Guard { ~Guard() { s_inSpillover.store(false); } } guard;
    auto now = std::chrono::steady_clock::now();
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    if (nowMs - s_lastCheckMs.load() < 200) return 0;
    s_lastCheckMs.store(nowMs);

    if (!aatpe::subsys::safemode::aggressiveMigrationAllowed()) return 0;

    Config cfgSnap;
    snapshotConfig(cfgSnap);
    aatpe_spillover::placementSpilloverTick(cfgSnap);
    if (cfgSnap.controlMode == "passive") return 0;

    int migrated = 0;
    std::vector<ThreadInfo> criticalThreads = collectCriticalThreads(3);

    std::vector<int> allCpusCopy;
    {
        std::lock_guard<std::mutex> rlock(g_topologyRwMutex);
        allCpusCopy = g_allCpus;
    }

    for (const auto& ti : criticalThreads) {
        if (migrated >= 3) break;

        int currentCpu = readThreadCurrentCpu(ti.pid, ti.tid);
        if (currentCpu < 0) currentCpu = ti.currentCpu;
        if (currentCpu < 0) continue;

        float predictedCurrentLoad = predictCpuLoadMultiStep(currentCpu, 3);
        float confidence = getPredictionConfidence(currentCpu);

        if (predictedCurrentLoad < 85.0f || confidence < 0.5f) continue;

        cpu_set_t allowedCpus = getThreadAllowedCpus(ti.tid);

        int bestTarget = -1;
        float bestTargetLoad = 100.0f;
        for (int cpuId : allCpusCopy) {
            if (cpuId == currentCpu) continue;
            if (!isCpuAllowed(cpuId, allowedCpus)) continue;
            float predictedTargetLoad = predictCpuLoadMultiStep(cpuId, 3);
            float targetConfidence = getPredictionConfidence(cpuId);
            if (predictedTargetLoad < 40.0f && targetConfidence > 0.3f) {
                if (predictedTargetLoad < bestTargetLoad) {
                    bestTargetLoad = predictedTargetLoad;
                    bestTarget = cpuId;
                }
            }
        }

        if (bestTarget < 0) continue;

        ThreadFeatures features = extractMlFeatures(ti, bestTarget);
        int fromPool = (int)getPoolForCpu(currentCpu);
        int toPool = (int)getPoolForCpu(bestTarget);
        float mlGain = 0.0f;
        if (fromPool >= 0 && toPool >= 0 && fromPool != toPool) {
            std::lock_guard<std::mutex> mlLock(g_mlMutex);
            mlGain = g_mlModel.predictMigrationGain(fromPool, toPool, features);
        }

        if (mlGain < 0.0f) continue;

        Decision d;
        d.action = DecisionAction::MIGRATE;
        d.targetCpu = bestTarget;
        d.reason = "PROACTIVE: predicted overload " + std::to_string((int)predictedCurrentLoad)
                 + "% on CPU" + std::to_string(currentCpu)
                 + " -> CPU" + std::to_string(bestTarget)
                 + " (conf=" + std::to_string(confidence).substr(0,3) + ")";
        if (applyPlacement(ti, bestTarget, d.reason, cfgSnap)) {
            migrated++;
            LOG_DEBUG("PROACTIVE MIGRATION: tid=%d CPU%d->CPU%d predicted_load=%.0f%% confidence=%.2f",
                     ti.tid, currentCpu, bestTarget, predictedCurrentLoad, confidence);
        }
    }

    if (migrated > 0) {
        static std::atomic<long long> s_proactiveMigrationTotal{0};
        s_proactiveMigrationTotal.fetch_add(migrated);
        writeFile((std::string(STATE_DIR) + "/prediction_proactive_migrations").c_str(),
                  (long long)s_proactiveMigrationTotal.load());
    }

    return migrated;
}

#ifdef AATPE_HOST_TEST
namespace aatpe { namespace placement {
int spilloverCountForTest() {
    std::lock_guard<std::mutex> lk(aatpe_spillover::g_spillStateMutex);
    return (int)aatpe_spillover::g_spillState.size();
}
void spilloverDebugAddForTest(int tid, int tier) {
    std::lock_guard<std::mutex> lk(aatpe_spillover::g_spillStateMutex);
    auto& st = aatpe_spillover::g_spillState[tid];
    st.targetTier = tier;
}
void spilloverReleaseAllForTest() {
    releaseAllSpilloverLocked();
}
long long spilloverCgroupDriftHealedForTest() {
    return aatpe_spillover::g_cgroupDriftHealed.load(std::memory_order_relaxed);
}
bool spillDemoteIdentityMatchForTest(int tid, long long recordedStarttime) {
    return aatpe_spillover::spillDemoteIdentityMatch(tid, recordedStarttime);
}
unsigned long long spillReadThreadStarttimeForTest(int tid) {
    unsigned long long v = 0;
    (void)aatpe_spillover::spillReadThreadStarttime(tid, &v);
    return v;
}

bool placementLatchQueryForTest(int tid, long long nowMs, bool emergency) {
    std::lock_guard<std::mutex> lk(g_migLatchMutex);
    return migLatchQueryLocked(tid, nowMs, emergency);
}
void placementLatchStampForTest(int tid, long long nowMs, bool uphill) {
    std::lock_guard<std::mutex> lk(g_migLatchMutex);
    migLatchStampLocked(tid, nowMs, uphill);
}
void placementLatchResetForTest() {
    std::lock_guard<std::mutex> lk(g_migLatchMutex);
    g_migLatch.clear();
}
long long placementLatchRejectCountForTest(bool emergency) {
    return emergency ? g_latchRejectEmergency.load(std::memory_order_relaxed)
                     : g_latchRejectNormal.load(std::memory_order_relaxed);
}
void placementLatchConstantsForTest(long long* normalMs, long long* emergencyMs, long long* postUphillMs) {
    if (normalMs)    *normalMs = kNormalCrossPathResidencyMs;
    if (emergencyMs) *emergencyMs = kEmergencyResidencyMs;
    if (postUphillMs)*postUphillMs = kPostUphillResidencyMs;
}
int placementEmergencyCooldownMsForTest() { return kEmergencyCooldownMs; }
}}
#endif

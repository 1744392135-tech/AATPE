// AATPE - Placement Decision & Execution (split from aatped.cpp)
// Contains: decidePlacement, protectCriticalThreads, applyPlacement, cleanupDecisionsDir.
#include "aatped.h"
#include "scoring_interface.h"
#include "aatpe_ml_hybrid.h"  // hybrid learning fine-tuning
#include <array>  // P1-2: ring buffer
#include "adrc.h"              // ADRC feedforward prediction + ML fusion
#include "aatped_priority.h"   // RT 优先级 + uclamp 算力优先分配
#include "aatpe_recovery.h"    // unified persistent recovery records
#include "aatpe_placement_mech.h"  // ROUND-005 unified boost mechanism selection
#include "aatpe_caps.h"           // ROUND-015: writeTidToCpuCgroupGroup (cgroup.threads) + v4.2 autocalBigRatio()
#include "aatped_threadutil.h"    // v4.2 A1: aatpeIsSurfaceFlingerThread
#include "gpu_boost.h"            // v4.2 A2: "placement" producer via schedtune single-writer bridge
#include "aatpe_diag.h"            // ROUND-009 holistic pressure snapshot
#include "aatped_psi.h"            // v4.3.6: PSI overload/mem-pressure seams
#include "aatped_utils.h"         // aatpeDetectForegroundApp
#include "aatpe_state_ttl.h"   // #35: placementAvgRecordGain + TTL sweep
#include "aatpe_subsystem.h"    // safe-mode gates
#include <deque>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include <fstream>
extern float g_systemLoad; // system load for adaptive fusion temperature
// v4.4.1 P0: thermal HAL level getter (real in thermal.cpp, namespace aatpe).
namespace aatpe { int aatpeThermalHalLevel(); }
// Defined later in this file; used by aatpeIsSystemCriticalThread() above the
// per-candidate fusion loop.
bool isKernelThread(const std::string& tname, int tid);
// .cpp files may use using namespace (not included elsewhere)
using namespace aatpe;
using namespace aatpe_ml;
using namespace aatpe_ml_hybrid;
using namespace aatpe::adrc;
extern "C" void aatpePruneDeadRtTids();   // ROUND-014 P3

// C1/C3 exploration: thermal placement scale (default-off; default 1.0 = no-op).
extern std::atomic<float> g_thermalPlacementScale;
extern std::atomic<bool> thermalSmoothEnabled;

// ============================================================================
// precise-uninstall recovery record for affinity.
// Written once per thread on first migration: (pid, tid, orig_list, new_list).
// uninstall.sh restores orig_list ONLY when the live Cpus_allowed_list still
// equals new_list (i.e. no other component took over). Keeps these helpers
// file-static so the unit-test link graph (which links placement.cpp against a
// stub writeFile()) stays self-contained.
// ============================================================================
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

// 智能绑核：过载/过温降级微调（定义在 getPoolForCpu 之后，此处前置声明）。
// 新增 allowedCpus 参数 — fallback 选核时必须排除不在线程
// cpuset 允许列表里的 CPU，避免选到 CPU3 之类被 /background cpuset 排除的核心。
static int applySmartPlacementFallback(int bestCpu, const ThreadInfo& ti,
                                       const Config& cfgSnap,
                                       const cpu_set_t& allowedCpus);

// P2-4: std::mutex for g_allCpus / pool vectors. refreshCpuTopology() writes
// (lock), decidePlacement()/proactiveMigrationCheck() snapshot a copy (lock).
// (An earlier draft used a shared_mutex, but the read side always copies the
// whole vector under the lock anyway, so shared_lock granularity buys nothing;
// a plain std::mutex is what is actually used here.)
// g_allCpus and pool vectors are declared in aatped.h (Group1) and cannot be
// made atomic; this mutex protects concurrent access within placement.cpp.
std::mutex g_topologyRwMutex;  // merge: extern for Phase3

// H13: hotplug rebuild debounce + rebuild-window guard (defined here, used both
// by refreshCpuTopology() and applyPlacement() below).
std::atomic<bool> g_topologyRebuilding{false};
static std::atomic<long long> s_lastTopologyRebuildMs{0};
static std::atomic<int> s_topologyRebuildCount{0};
static std::atomic<int> g_topologyDebounceMs{500};

// P2-1: g_migrationCount / g_migrationWindowStart are declared as plain int/time_t
// in aatped.h (Group1, cannot change to std::atomic). This mutex protects the
// read-modify-write sequence in applyPlacement() within placement.cpp.
// Declared early so RateLimitGuard (defined below) can reference it.
static std::mutex g_migRateMutex;

// ============================================================================
// Unified Decision Fusion: four-way fusion (heuristic + ML + hybrid + history)
// with state-dependent weights, ML cold-start handling, and divide-by-zero
// protection (all weights zero → fallback to heuristic).
// ============================================================================
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

// Write fusion average weights to state file as JSON. Called periodically
// from daemonLoop so WebUI /api/fusion and /state/all can expose the data.
void writeFusionWeightsState() {
    float wh, wm, why, whs;
    long long cnt = g_fusionWeightAccum.getAverageWeights(wh, wm, why, whs);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"heuristic_weight\":%.4f,\"ml_weight\":%.4f,\"hybrid_weight\":%.4f,"
        "\"history_weight\":%.4f,\"total_decisions\":%lld}",
        (double)wh, (double)wm, (double)why, (double)whs, cnt);
    writeFile((std::string(STATE_DIR) + "/fusion_weights").c_str(), buf);
    // #34: expose hysteresis counters + avg dwell for WebUI diagnostics.
    writeFile((std::string(STATE_DIR) + "/placement_settle_migrations").c_str(),
              (int)g_settleMigrations.load(std::memory_order_relaxed));
    writeFile((std::string(STATE_DIR) + "/placement_settle_hysteresis_rejects").c_str(),
              (int)g_settleHysteresisRejects.load(std::memory_order_relaxed));
    writeFile((std::string(STATE_DIR) + "/placement_settle_cooldown_rejects").c_str(),
              (int)g_settleCooldownRejects.load(std::memory_order_relaxed));
    writeFile((std::string(STATE_DIR) + "/placement_settle_avg_dwell_ms").c_str(),
              (int)placementGetAvgSettleDwellMs());
}

// Write frame-time outcome counters + current snapshot to state files.
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

// ============================================================================
// RateLimitGuard: RAII wrapper for per-second migration rate limiting.
// Construct checks budget under g_migRateMutex (does not increment).
// Call consume() after successful sched_setaffinity to count the migration.
// Destructor is a no-op. This replaces ad-hoc lock_guard blocks in
// applyPlacement with a clear RAII boundary.
// ============================================================================
RateLimitGuard::RateLimitGuard(bool frameCritical, bool emergency) : acquired_(false), consumed_(false) {
    // Emergency migrations (thermal spike / jank / FPS drop) bypass the rate
    // limiter entirely AND must NOT reset the ordinary migration window — they
    // are rare event-driven moves and should not penalise passive migrations.
    if (emergency) {
        acquired_ = true;
        return;
    }
    // snapshot config FIRST (g_cfgMutex is the highest-level
    // lock) BEFORE taking g_migRateMutex, to preserve lock order. Previously
    // g_cfg.maxMigrationsPerSec was read under g_migRateMutex without the cfg
    // lock (racy) AND taking g_migRateMutex then g_cfgMutex would reverse the
    // documented order.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    // P2: clamp to >=0 so a misconfigured negative value doesn't underflow
    // and silently block all migrations (negative * 2 wraps to a huge unsigned).
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
RateLimitGuard::~RateLimitGuard() { /* no-op: budget is consumed via consume() */ }

DecisionFusion fuseDecisions(float heuristicScore, float mlScore,
                              float hybridScore, float historyScore,
                              SystemState state, bool mlColdStart,
                              float mlAlphaScale) {
    DecisionFusion f;
    // P2: the ternary clamps below do NOT catch NaN — (NaN<0) and (NaN>1) are
    // both false, so a NaN score falls through unchanged. Zero each up front.
    if (std::isnan(heuristicScore)) heuristicScore = 0.0f;
    if (std::isnan(mlScore))        mlScore        = 0.0f;
    if (std::isnan(hybridScore))    hybridScore    = 0.0f;
    if (std::isnan(historyScore))   historyScore   = 0.0f;
    f.heuristic_score = ((heuristicScore) < (0.0f) ? (0.0f) : ((heuristicScore) > (1.0f) ? (1.0f) : (heuristicScore)));
    f.ml_score        = ((mlScore) < (0.0f) ? (0.0f) : ((mlScore) > (1.0f) ? (1.0f) : (mlScore)));
    f.hybrid_score    = ((hybridScore) < (0.0f) ? (0.0f) : ((hybridScore) > (1.0f) ? (1.0f) : (hybridScore)));
    f.history_score   = ((historyScore) < (0.0f) ? (0.0f) : ((historyScore) > (1.0f) ? (1.0f) : (historyScore)));
    // P2: lock + snapshot the bool under g_dynamicWeightsMutex to avoid a torn
    // read racing the ML thread's write to g_dynamicScoreWeights.
    {
        std::lock_guard<std::mutex> dwlk(g_dynamicWeightsMutex);
        f.dynamic_weight_adjustment = g_dynamicScoreWeights.valid ? 1.0f : 0.0f;
    }

    // Clamp the continuous maturity scale to [0,1].
    if (mlAlphaScale < 0.0f) mlAlphaScale = 0.0f;
    if (mlAlphaScale > 1.0f) mlAlphaScale = 1.0f;

    // State-dependent base weights
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

    // ML cold start: zero ML weight, renormalize remaining weights
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
        // alpha is a CONTINUOUS fusion weight, not an on/off switch.
        // Previously poolAlpha<0.05 zeroed ML (cold start) and poolAlpha>=0.05
        // immediately jumped to the FULL fixed ML weight (wm), a discontinuity.
        // Now:  wm_eff = wm_base * mlAlphaScale (ramp 0 -> full as the model
        // matures), and the non-ML sources are renormalized proportionally so
        // the total weight (wh+wm+why+whs) is preserved. At scale==1.0 this is
        // the old fixed-weight behavior; at scale==0 it collapses to the
        // cold-start renormalization (smooth, no jump).
        float total = wh + wm + why + whs;
        float wmBase = wm;
        wm = wmBase * mlAlphaScale;
        float nonMlBase = wh + why + whs;       // weight before scaling
        float nonMlNew  = (total - wm);         // weight to reallocate to non-ML
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

    // Divide-by-zero protection: if all weights are zero, fall back to heuristic
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

// In-memory decision history ring buffer
// Emergency migration limit: migrated >= 10 per trigger triggers cooldown.
// emergency_migrations state file tracks count for watchdog. (replaces frequent file writes)
struct DecisionRecord {
    int pid; int tid; int targetCpu; int role;
    std::string reason; long long timestamp;
};
// P1-2: Ring buffer (std::array + head/tail indices) instead of std::deque.
// Eliminates heap allocations per push/pop; fixed 100-entry circular buffer.
// P1-3: g_scoreRing is written by pushDecisionRecord() (placement hot path) and
// read by getDecisionBuffer() (WebUI HTTP thread) — both take g_scoreRingMutex
// so a consumer never sees a half-updated head/count pair. Iteration math:
//   oldest entry = (head - count + size) % size; walking (start + i) % size for
//   i in [0,count) yields oldest→newest. Verified by hand for empty/full/wrap.
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

// ============================================================================
// Mechanical split of decidePlacement into file-scope helpers:
//   The original decidePlacement() was a single ~493-line function mixing
//   per-CPU scoring, ML fusion, cold-start exploration, threshold gating and
//   selection. It is now decomposed into file-scope helpers below. This is a
//   PURE MECHANICAL move: every statement, condition, lock acquisition/release
//   and log line is copied verbatim from the original function body. No logic,
//   no variable name/type, no ordering change.
//
//   Lock-spanning notes (preserved exactly):
//     - snapshotConfig() is still taken at decidePlacement() entry.
//     - g_mlMutex is still hoisted OUT of the per-CPU loop via mlLoopLock and
//       released right after the scoring loop, BEFORE the cold-start block
//       re-acquires it itself (std::mutex is non-recursive).
//     - g_topologyRwMutex still only guards the allCpus copy.
//     - g_cpuMetricsMutex is still taken inside the tie-break comparison.
// ============================================================================

// fuseMlScore(): per-candidate ML score fusion (was the L360-444 if/else-if/else
// chain inside the scoring loop). Operates on one cpuId; mutates finalScore
// (in/out) and fusion (out). g_mlMutex is held by the caller (hoisted loop
// lock); only the model-internal BatchLock is taken here.
//
// ML prediction cache. g_mlModel.predict() runs per (thread x
// candidate CPU) every decision cycle. Critical threads (S0_SYSTEM/S1_UI/
// S2_RENDER/S3_GAME) ALWAYS predict fresh (responsiveness preserved exactly).
// Non-critical threads (S4_THROUGHPUT/S5_BACKGROUND) reuse their last cached
// modelScore for kMlPredictTtlMs — the heuristic/hybrid/history/migration-gain
// terms are still recomputed fresh, so the decision keeps adapting; only the MLP
// output is reused for a few cycles. Guarded by g_mlMutex (fuseMlScore runs under
// it), so no extra lock is needed.
namespace {
struct MlPredictCacheEntry {
    float modelScore = 0.0f;
    uint64_t expireMs = 0;
};
std::unordered_map<uint64_t, MlPredictCacheEntry> s_mlPredictCache;
uint64_t s_mlPredictCacheSweepMs = 0;
constexpr uint64_t kMlPredictTtlMs = 800;      // ~4 decision cycles @200ms
constexpr uint64_t kMlPredictSweepIntervalMs = 5000;
inline uint64_t mlPredictNowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace
// #31/#37: System-critical thread whitelist.
// These threads must NEVER be moved or re-prioritized by the ML learner (and the
// placement loop treats them as untouchable):
//   - ROLE_S0_SYSTEM threads (system class)
//   - kernel housekeeping threads (kswapd/migration/rcu_/irq_/kworker/watchdog...)
//     via isKernelThread()
//   - the core system servers system_server and surfaceflinger (by process name)
// Vendor HAL / HwBinder worker threads (role=5) are intentionally NOT in this
// list so legitimate background optimization still applies to them.
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
        // P3: cacheKey below packs the tid into the high 32 bits; a negative or
        // zero tid would cast to a huge unsigned and collide across invalid
        // entries. Clamp to a well-defined key component (0) for bad tids.
        const unsigned cacheTid = (ti.tid > 0) ? (unsigned)ti.tid : 0u;
        // #37: ML must never shift system-critical threads. They are always placed
        // by pure heuristic (equivalent to ML-disabled path), regardless of model
        // alpha / readiness.
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
            // P0-1 (问题77): MUST use Pool ID, not CPU ID, for Per-Pool ML query.
            // Using CPU ID directly causes CPU3+ to return 0.0f in 3-Pool model.
            // poolIdx now computed once before this block (cold start gate reuse).
            bool mlValidForCpu = (poolIdx >= 0 && poolIdx < g_mlModel.getPoolCount());
            // P2-fix: Use BatchLock to hold g_mlModel's internal mutex across
            // predict() + getAlphaForPool() + predictMigrationGain() calls,
            // avoiding repeated lock/unlock cycles in the per-CPU placement loop.
            // g_mlMutex is held externally via mlLoopLock (hoisted
            // out of this loop). Only the model-internal BatchLock is taken here.
            if (mlValidForCpu) {
                // cache lookup. Critical threads (role <= S3_GAME)
                // always re-predict; non-critical reuse within the TTL.
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
                    // Anomaly detection: if model score is NaN or out of range, fall back to pure heuristic
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
                // Unified four-way decision fusion.
                float normHeuristic = normalizeHeuristicScore(heuristic);
                // fix55 (问题28/29): Use per-pool alpha for this candidate's pool.
                float poolAlpha = mlAlpha;
                int pIdx = (int)getPoolForCpu(cpuId);
                if (pIdx >= 0 && pIdx < g_mlModel.getPoolCount()) {
                    poolAlpha = g_mlModel.getAlphaForPool(pIdx);
                }
                // Real hybrid learning score (was hardcoded 0.0f stub)
                float hybridScoreVal = hybridScore(cpuId, ti);
                float historyScoreVal = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
                if (historyScoreVal < 0.0f) historyScoreVal = 0.0f;
                if (historyScoreVal > 1.0f) historyScoreVal = 1.0f;
                SystemState sysState = getSystemState();
                // alpha is a CONTINUOUS fusion weight. Normalize the
                // per-pool alpha (which ramps 0 -> max_alpha) to a maturity
                // fraction in [0,1] and pass it as the live ML weight scale. The
                // only true cold-start gate left is the external readiness block
                // (poolColdStartBlocked); the hard 0.05 on/off threshold is gone.
                float maxAlpha = g_mlModel.getMaxAlpha();
                // P2: maxAlpha==0 would make poolAlpha/maxAlpha = NaN/Inf; the
                // clamps below do not catch NaN. Guard the division and add an
                // explicit NaN fallback after clamping.
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
                // ML migration gain prediction (batched under same BatchLock)
                // P2 Problem6: use liveCpu (read at decision entry) as fromCpu,
                // not the possibly-stale ti.currentCpu snapshot.
                int fromPool = (int)getPoolForCpu(liveCpu);
                int toPool = poolIdx;
                if (fromPool >= 0 && toPool >= 0 && fromPool != toPool) {
                    float migrationGain = g_mlModel.predictMigrationGain(fromPool, toPool, features);
                    // P3: a NaN/Inf gain would poison finalScore (NaN propagates
                    // through the whole argmax selection). Clamp to 0 on invalid.
                    if (std::isnan(migrationGain) || std::isinf(migrationGain)) migrationGain = 0.0f;
                    finalScore += migrationGain * 0.5f;
                }
            } else {
                // CPU not in any known pool (e.g. hotplugged unknown core):
                // fall back to heuristic-only for this candidate.
                modelScore = normalizeHeuristicScore(heuristic);
                float normHeuristic = normalizeHeuristicScore(heuristic);
                // Real hybrid learning score in fallback path too
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
            // P1-3: Cold start (mlAlpha==0) — use normalized heuristic for
            // consistent 0-1 dimensionality. Previously raw heuristic (could be
            // negative or hundreds) was used, making confidence (bestFused-secondFused)
            // inconsistent with the warm-start fused-score path.
            finalScore = normalizeHeuristicScore(heuristic);
            // P2-datafix: Always record fusion weights even in cold start.
            // Without this, bestFusion stays default-all-zero and the accumulator
            // reports heuristic_weight=ml_weight=...=0 despite total_decisions>0.
            float normHeuristicCs = normalizeHeuristicScore(heuristic);
            float historyCs = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
            if (historyCs < 0.0f) historyCs = 0.0f;
            if (historyCs > 1.0f) historyCs = 1.0f;
            fusion = fuseDecisions(normHeuristicCs, 0.0f, 0.0f, historyCs,
                                   getSystemState(), true);
        } else {
            // ML fully disabled — heuristic-only decision. Still produce a valid
            // DecisionFusion so the weight accumulator records real values.
            float normHeuristicOff = normalizeHeuristicScore(heuristic);
            float historyOff = (float)placementSuccessScore(cpuId, ti.tid) / 100.0f;
            if (historyOff < 0.0f) historyOff = 0.0f;
            if (historyOff > 1.0f) historyOff = 1.0f;
            fusion = fuseDecisions(normHeuristicOff, 0.0f, 0.0f, historyOff,
                                   getSystemState(), true);
        }
}

// selectOptimalPlacement(): per-candidate best/second tie-break (was the
// L524-552 block inside the scoring loop). Mutates the loop-carried best state.
// g_cpuMetricsMutex is still taken ONLY for the tie-break utilization read.
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
            bestFusion = fusion;  // track best fusion
        } else if (std::fabs(finalScore - bestFused) < 1e-6f && bestCpu >= 0) {
            // 平局时选择负载更低的核心
            int curUtil = 0, bestUtil = 0;
            {
                std::lock_guard<std::mutex> metricsLock(g_cpuMetricsMutex);
                auto curIt = g_cpuMetrics.find(cpuId);
                auto bestIt = g_cpuMetrics.find(bestCpu);
                if (curIt != g_cpuMetrics.end()) curUtil = curIt->second.util;
                if (bestIt != g_cpuMetrics.end()) bestUtil = bestIt->second.util;
            }
            if (curUtil < bestUtil) {
                // 当前CPU负载更低, 替换为最佳
                secondFused = bestFused;
                bestCpu = cpuId;
                bestHeuristic = heuristic;
                bestFusion = fusion;
                if (featuresValid) bestFeatures = features;
            } else if (curUtil == bestUtil && cpuId < bestCpu) {
                // 利用率也相同时, 保持低CPU ID(先到先得), 不替换
            }
        } else if (finalScore > secondFused) {
            secondFused = finalScore;
        }
}

// B3 task2: PRIME spillover — PURE decision logic (host-testable seam).
// Promotes the heaviest critical threads to the highest (PRIME) cluster when the
// second-highest cluster is saturated AND PRIME has headroom. Tier-count generic
// (works for 2/3/4 clusters; returns empty when no PRIME exists).
// forward decl: defined later in this TU (global scope).
static std::vector<ThreadInfo> collectCriticalThreads(int maxCount,
        const std::set<std::string>* priorityPnames = nullptr);
namespace aatpe_spillover {
struct ClusterLoad { int tier; float util; };   // weak -> strong
struct ThreadCand  { int tid; int role; bool critical; float weight; int curTier; };
struct PromoteTarget { int tid; int targetTier; };

inline float utilThresholdForMode(int mode) {
    using namespace aatpe::spillover;
    if (mode == 0) return kUtilThresholdPassive;
    if (mode == 2) return kUtilThresholdActive;
    return kUtilThresholdAssist;
}
// ROUND-008b: cluster pressure threshold per mode (active/assist/passive).
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
// WS-C: uclamp.min target per target tier (R2 §6). PRIME=1024 full; BIG scaled by
// cluster capacity ratio (D1100 BIG A78@2.0 ≈ 896/1024). Host-testable seam.
// PLACE-01 fix: snapshot the small set of g_cfg fields this function reads under
// g_cfgMutex, so a concurrent config reload cannot deliver torn values. The old
// code read g_cfg.uclampSfPct / uclampCriticalMinPct / uclampPassivePct /
// uclampAssistMinPct lock-free while spillover may be running on another thread.
// Lock order: g_spillStateMutex (held by caller) -> g_cfgMutex. No call site
// holds g_cfgMutex while taking g_spillStateMutex, so this cannot deadlock.
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
    // v4.2 A1: mode=3 = SurfaceFlinger/compositor tier. Fixed low floor
    // (UCLAMP_SF_PCT=20%). Evaluated BEFORE the PRIME early-return below so an SF
    // thread the decider routed to PRIME is still held at the SF floor (~178)
    // instead of being yanked to 1024. Signature is UNCHANGED (mode is the existing
    // 3rd param, so the asm-mangled test symbol ...Eifi is untouched); modes 0/1/2
    // fall through to the legacy path -> v4.1.1 bit-identical. The caller helper
    // floorUclampForThreadLocked() selects mode=3 only when the thread comm matches
    // aatpeIsSurfaceFlingerThread(); otherwise it passes the original mode.
    if (mode == 3) {
        int sf = (int)(sfPct * 1024 / 100);
        int v = (int)(sf * bigCapacityRatio);
        if (v < 64) v = 64;
        if (v > 1024) v = 1024;
        return v;
    }
    // ROUND-014 P3: mode-scaled critical min (G12 calibration).
    int crit = critPct * 1024 / 100;
    if (targetTier >= (int)CpuTier::PRIME) return 1024;
    int v = (int)(crit * bigCapacityRatio);
    if (v < 64) v = 64;
    if (v > 1024) v = 1024;
    return v;
}
}}  // namespace aatpe::placement

// v4.2 A1: caller-level uclamp.floor selector. Centralises the SF/compositor
// whitelist check + PRIME-bypass so the promote block computes ONE `umin` and
// reuses it across all boost-mechanism branches (was 6 duplicate
// uclampMinForTier(...,0.87f,mode) calls). For any NON-SF thread it delegates
// straight to uclampMinForTier(targetTier, ratio, mode) -> byte-identical to
// v4.1.1 (this is the default-off bit-identical contract: the only threads whose
// value changes are SF-named compositor threads, which is the whole feature).
// `ratio` is the v4.2 A3 autocal-big ratio (defaults to 0.87f).
inline int floorUclampForThreadLocked(const char* tname, int targetTier,
                                      float bigCapacityRatio, int mode) {
    if (aatpeIsSurfaceFlingerThread(tname)) {
        return aatpe::placement::uclampMinForTier(targetTier, bigCapacityRatio, /*mode=*/3);
    }
    return aatpe::placement::uclampMinForTier(targetTier, bigCapacityRatio, mode);
}

// v4.2 A2: schedtune.boost level the placement "producer" should request this tick,
// or -1 to release the producer. Returns -1 (Clear) whenever there is NO active
// promotion window (empty spill set / emergency / thermal-degraded) so that
// max(touch,gpu,placement) collapses back to max(touch,gpu) — v4.1.1 bit-identical
// outside a spillover window. Mode maps: passive->schedtunePassiveBoost,
// assist->schedtuneAssistBoost, active->schedtuneCriticalBoost.
// PLACE-01 fix: snapshot the three schedtune boost levels under g_cfgMutex
// instead of reading g_cfg bare. Same lock-order argument as uclampMinForTier.
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

// Return true if capacity diff below ~10% — skip cross-cluster promotion between them.
inline bool sameCapacityClusters(float utilA, float utilB) {
    float hi = std::max(utilA, utilB), lo = std::min(utilA, utilB);
    if (hi < 1.0f) return true;
    return ((hi - lo) / hi) < 0.10f;
}

// ---- Frame-independent scheduling-pressure fallback (C1/C3/C4) -------------
// The only historical trigger for "stutter -> boost freq / up-move" was the pure
// frame-derived vote (frameaware::jankBoostVoteActive). When the OEM ROM frame
// source dies, that vote never arms and there is no independent "scheduling is
// laggy -> lift the foreground renderer" backstop. PSI / eBPF sched-wait were
// previously consumed ONLY for root-cause classification + veto (jankdiag), never
// to *produce* a promotion.
//
// This arm decision is deliberately OR-combined with the frame-derived vote and
// is gated by the SAME safety rails (thermal emergency, cpu/gpu throttle,
// thermal-veto set) — it never disables a thermal protection and never vetoes the
// frame vote. Hysteresis (separate arm/release thresholds + consecutive-tick
// debounce) prevents flapping. No package name is hardcoded: the candidates are
// the foreground render critical threads collected by collectCriticalThreads().
namespace aatpe { namespace placement { namespace pressurefb {
    // PSI cpu "some" stall (avg10, %). Real device reading ~22% under load.
    constexpr float kPsiCpuArmPct   = 20.0f;   // at/above -> candidate to ARM
    constexpr float kPsiCpuClearPct = 10.0f;   // below    -> candidate to RELEASE
    // foreground render thread runqueue-wait fraction w/(w+r).
    constexpr float kWaitArmRatio   = 0.25f;   // at/above -> scheduler-starved
    constexpr float kWaitClearRatio = 0.12f;   // below    -> no longer starved
    constexpr int   kArmStreakReq   = 2;       // consecutive pressure ticks to ARM
    constexpr int   kClearStreakReq = 3;       // consecutive quiet ticks to RELEASE
}}}
// Per-thread runqueue-wait fraction w/(w+r) from the eBPF/diag sampler. Returns
// <0 when the sampler has no data for this tid (host stub / node absent) — callers
// treat <0 as "unknown", never as "zero pressure".
static inline float placementThreadRqWaitRatio(int tid) {
    int64_t w = diag::getThreadRqWaitNs(tid);
    int64_t r = diag::getThreadRunNs(tid);
    if (w >= 0 && r >= 0 && (w + r) > 0) return (float)w / (float)(w + r);
    return -1.0f;
}

// clusters: weak->strong. Returns (tid, targetTier) — NEVER jumps straight to
// PRIME when BIG has headroom (research: gradual climb saves power).
std::vector<PromoteTarget> decidePromotions(const std::vector<ClusterLoad>& clusters,
                                         const std::vector<ThreadCand>& cands,
                                         float primeUtil, int mode, int stableCycles,
                                         bool singleThreadLatency = false) {
    std::vector<PromoteTarget> out;
    if (clusters.size() < 2) return out;
    // WS-C: top cluster = clusters.back() by capacity order, not the PRIME label.
    // Same-capacity adjacent clusters (9300 two big) give no cross-cluster gain.
    const int TOP = (int)clusters.back().tier;
    float secondUtil = clusters[clusters.size() - 2].util;
    float lowestUtil = clusters.front().util;
    float pressureThresh = pressureThresholdForMode(mode);
    float bigThresh = utilThresholdForMode(mode);
    if (stableCycles < spillover::kStableCycles) return out;     // not debounced
    // PLACE-06: stableCycles is intentionally a TEST SEAM. The production caller
    // (placementSpilloverTick:1807) already gates promotion per-thread on
    // st.satisfyCycles >= kStableCycles, so passing kStableCycles here is always
    // true by construction. Unit tests pass smaller values to exercise the
    // no-promote path directly. We retain the parameter (rather than deleting it)
    // because test_placement.cpp pins the debounce contract; the check itself is
    // the documented, intentional debounce gate.
    std::vector<ThreadCand> pool = cands;
    std::sort(pool.begin(), pool.end(),
              [](const ThreadCand& a, const ThreadCand& b){ return a.weight > b.weight; });
    // PLACE-02 fix: a thread whose current tier we cannot determine
    // (readThreadCurrentCpu failed -> CpuTier::UNKNOWN == -1) must NOT be
    // promoted off a guess. We don't know whether it is already on BIG/PRIME,
    // so any promotion would be at best a no-op and at worst a wrong move.
    // Filter UNKNOWN out of the candidate pool entirely.
    pool.erase(std::remove_if(pool.begin(), pool.end(),
              [](const ThreadCand& c){ return c.curTier == (int)CpuTier::UNKNOWN; }),
               pool.end());
    // Step 1: weakest cluster saturated but the next cluster has headroom ->
    // promote one step up. WS-C D: targets are RELATIVE to the top cluster (not
    // hardcoded tier 2), so 2-cluster / all-big devices climb correctly; and
    // equal-capacity adjacent clusters (9300 two big) give no cross-cluster gain.
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
    // C4: single-thread slow render. The classic spillover only fires once the
    // LITTLE cluster is actually saturated (littlePressure). On a single-threaded
    // renderer the weak cluster often has one busy core + three idle ones, so its
    // aggregate util never crosses the saturation threshold even though the render
    // thread is scheduler-latency starved. When the caller signals
    // singleThreadLatency (PSI-cpu stall OR the render thread's own rq-wait high),
    // allow the SAME bounded one-step-up to BIG_TARGET — it still requires BIG
    // headroom, still respects the same-capacity guard and kStableCycles debounce,
    // and never jumps to TOP (Step 2 is untouched). Default off -> bit-identical.
    bool step1Trigger = littlePressure || singleThreadLatency;
    if (step1Trigger && bigHeadroom && !sameCap) {
        int n = topNForMode(mode);
        for (const auto& c : pool) {
            if ((int)out.size() >= n) break;
            if (!c.critical) continue;
            if (c.curTier >= BIG_TARGET) continue;  // already on the step target or above
            out.push_back({c.tid, BIG_TARGET});
        }
    }
    // Step 2: second cluster also saturated AND top has headroom -> Top-1 to TOP.
    bool bigSaturated = (secondUtil >= bigThresh);
    if (bigSaturated && !sameCap && primeUtil <= spillover::kPrimeHeadroom) {
        for (const auto& c : pool) {
            if (!c.critical) continue;
            if (c.curTier >= TOP) continue;
            out.push_back({c.tid, TOP});
            break;  // Top-1 only for the top spill
        }
    }
    return out;
}

// ---- ROUND-009b: holistic jank root-cause classifier (pure, host-testable) --
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
}  // namespace jankdiag

// ---- B3 production wiring: periodic PRIME spillover sweep ------------------
// The pure decidePromotions() above is stateless; the per-cycle hysteresis
// state (satisfy debounce + promote timestamp + min-dwell demotion) lives here.
struct TidPromoState {
    int  satisfyCycles = 0;
    bool promoted       = false;
    long long promotedMs = 0;
    bool      maskApplied = false;      // affinity fallback is in use
    cpu_set_t savedMask;               // pre-promotion affinity, restored on demote
    int       mechanism = 0;            // BoostMechanism actually applied
    int       targetTier = 0;           // CpuTier the thread was promoted to
    std::string savedCpuset;            // pre-promotion cpuset group, restored on demote
    long long starttime = 0;            // /proc/<tid>/stat field 22 (TID reuse guard)
    std::string origAffinity;           // pre-promotion cpulist / grp=<name>
    int  mechRestoreRetries = 0;        // #4: demote move-back-to-saved-group retries
    // B-001: RT (sched_setattr SCHED_FIFO/RR) elevation is a SEPARATE action from the
    // uclamp/affinity/cpuset boost mechanism. It is applied on the UCLAMP promote
    // branch (aatpeSetThreadPriority). Track whether we actually elevated this tid so
    // the demote loop AND releaseAllSpilloverLockedImpl can symmetrically call the
    // ungated restore channel (aatpeRestoreThreadPriority). Without this the RT
    // priority leaked until thread death -> priority inversion.
    bool rtBoosted = false;
};
std::mutex g_spillStateMutex;
std::atomic<int> g_fgDownpinBlocked{0};   // ROUND-012: blocked front-app downpins
std::atomic<int> g_downpinDiagCount{0};   // A6: 1/64 rate-limit for fg downpin diagnostic
// PLACE-12: per-thread restore counter (NOT per fg-switch edge). The test suite
// pins "2 same-package threads restored -> +2", so we keep the counter name but
// document the real semantic: it counts affinity restores on fg-switch edges,
// not edges themselves.
std::atomic<int> g_fgSwitchRelease{0};
static std::unordered_map<int, TidPromoState> g_spillState;
std::atomic<long long> g_spilloverPromotions{0}; // total promotions (observability)
std::atomic<int> g_spilloverActive{0};           // currently hosted on PRIME

// ROUND-015: fg->bg->fg stuck-bug repair. The spillover "promoted" belief is
// daemon-side state; AMS moves the whole thread group out of the boosted cgroup
// at process granularity (v2 cgroup.procs) while an app is briefly backgrounded.
// On the return-to-foreground edge the old promoted==true short-circuit then
// skipped the re-pin, leaving the threads physically in the background cgroup.
// g_fgEdgeReapply arms a one-shot, debounce-free re-pin sweep consumed by the
// next spillover tick; g_cgHealEveryN drives the periodic kernel-membership
// readback so the cgroup/cpuset channel self-corrects like the RT channel does
// via schedGetAttr.
static std::atomic<int> g_fgEdgeReapply{0};
static std::mutex g_fgEdgeMtx;
static std::string g_fgEdgeReapplyPkg;
static std::atomic<int> g_cgroupDriftHealed{0};   // observability

// Read the tid's REAL cgroup/cpuset membership from /proc and, if it has drifted
// away from the boost group ("top-app") for a membership-based mechanism, rewrite
// it back. Local-scheduler dimensions (uclamp/affinity) are not moved by AMS's
// process-level cgroup.procs migration, so no readback is needed for them.
// Caller must hold g_spillStateMutex. Returns true iff a rewrite was issued.
static bool reexertMembershipLocked(int tid, TidPromoState& st) {
    using BM = aatpe_mech::BoostMechanism;
    BM mech = (BM)st.mechanism;
    if (mech != BM::STUNE_CPUSET && mech != BM::CGROUP_UCLAMP) return false;
    std::string live = aatpe_mech::readThreadCpusetGroup(tid);
    if (live.empty()) return false;          // can't tell: don't thrash
    if (live == "top-app") return false;      // already where we want it
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

// #3: last-resort per-task uclamp after the affinity fallback also failed. Records
// mechanism=UCLAMP ONLY when the floor actually landed, so the demote release path
// (which dispatches on st.mechanism) knows it must reset uclamp.min. Previously the
// return value was dropped: a transiently-successful last resort left mechanism=
// AFFINITY/maskApplied=false, so all four release branches missed and the uclamp
// floor leaked until thread death.
static void applyLastResortUclamp(int tid, TidPromoState& st, unsigned int umin) {
    // audit4 F4: route through the source-tagged floor ledger (SPILLOVER source) so
    // the effective floor is max(touch, spillover, priority) and a later touch
    // RELEASE can no longer stomp this floor to 0.
    if (aatpe_mech::aatpeUclampFloorSet(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER, umin) == 0) {
        st.mechanism = (int)aatpe_mech::BoostMechanism::UCLAMP;
    }
}

static long long spillNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// audit5 N3 (U-02): read-only live starttime (/proc/<tid>/stat field 22) used as a
// TID-reuse identity guard in the spillover demote loop. Equivalent to the
// file-local readThreadStarttime() in aatped_priority.cpp (same rfind(") ") parsing,
// same "read+discard state char then 19 numerics" field walk) — replicated here
// because that helper is static to aatped_priority.cpp and not exported. Pure read:
// takes no lock of its own; the caller (demote loop) already holds g_spillStateMutex,
// exactly as the rest of the loop's /proc+/sys reads do.
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
    if (!(ss >> state)) return false;   // discard single-char R/S/D/Z/T
    unsigned long long v;
    for (int i = 1; i <= 19; ++i) {
        if (!(ss >> v)) return false;
        if (i == 19) { *out = v; return true; }   // field 22 = starttime
    }
    return false;
}

// audit5 N3 (U-02): TID-reuse identity predicate for demotion. True iff the LIVE
// thread currently sitting on `tid` is still the SAME instance we promoted (its
// /proc/<tid>/stat field-22 starttime equals the value recorded at promote time).
// False when the thread is gone (unreadable) OR the tid has been recycled by a new
// thread (starttime mismatch). The demote loop uses this to decide whether it may
// touch the tid's scheduling attributes at all.
static bool spillDemoteIdentityMatch(int tid, long long recordedStarttime) {
    unsigned long long liveSt = 0;
    if (!spillReadThreadStarttime(tid, &liveSt)) return false;
    return liveSt == (unsigned long long)recordedStarttime;
}

// audit4 efficiency: the real consumer of the arbitrator's efficiency hint
// (STATE_DIR/arbitrator_efficiency_hint, a float 0..1 written by the arbitrator
// tick). A HIGH hint means boosting / big-core push is inefficient this window ->
// SOFT-deprefer the proactive big-core push. Read-only by design: a missing, empty,
// unreadable or non-finite file defaults to 0 (no deprefer). Throttled to 500ms so
// the hot placement loop does not stat() per cycle. Never touches the emergency /
// thermal hard walls (the caller still gates on !emergency).
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
    return 1;  // assist / emergency
}

// ============================================================================
// v4.2.7: VM (VMOS) container guest-game resolution.
//
// When the foreground app is a VM container, the real game runs inside a
// virtualized guest process that shares the container's UID but has a different
// package name. The stock foreground gate (ti.pname == fgPkg) then excludes the
// guest game threads from promotion. We resolve the guest pnames self-containedly
// by matching /proc/<pid>/status Uid.
//
// B-003 (zero-package-name hardcoding): the set of container/dual-space packages
// is NO LONGER a built-in literal table. It is a runtime-loaded, externally
// configurable manifest (one package-prefix per line, <state_dir>/vm_containers.list)
// plus a test seam. Adding a new VM/dual-app container is a config edit, NOT a code
// change — no package names appear in production source.
// ============================================================================
static std::vector<std::string> g_vmContainerPrefixes;
static std::mutex g_vmContainerMtx;
static bool g_vmContainerListLoaded = false;

// Read the externally-configured container prefix list. File format: one package
// prefix per line ('#' comments / blank lines ignored). Called lazily once on the
// first isVmContainerPkg() use (and again after the test seam injects a list).
static void loadVmContainerPrefixesLocked() {
    g_vmContainerPrefixes.clear();
    std::string path = std::string(STATE_DIR) + "/vm_containers.list";
    std::ifstream f(path.c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        // trim
        size_t b = line.find_first_not_of(" \t\r");
        size_t e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, e - b + 1);
        if (line.empty() || line[0] == '#') continue;
        g_vmContainerPrefixes.push_back(line);
    }
}

// Test seam / runtime API: override the container prefix list (host tests inject
// the known family; operators can also push a list programmatically).
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
        if (pkg.rfind(p + ".", 0) == 0) return true;   // dotted-prefix variant
    }
    return false;
}

// Read the real (first) UID from /proc/<pid>/status. Returns -1 on failure.
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
// Test seam: when non-empty, resolveVmGuestPnames() returns this directly
// (host hermetic tests inject the guest set instead of walking /proc).
static std::set<std::string> g_forcedGuestPnames;
}
#ifdef AATPE_HOST_TEST
void setVmGuestPnamesForTest(const std::set<std::string>& pkgs) {
    aatpe_vm::g_forcedGuestPnames = pkgs;
}
#endif

// Cached guest pnames for the current container. Re-scanned at most every 2s.
// v4.4.9 R1-1: dedicated mutex protects the cache against concurrent access from
// the input-monitor epoll thread (placementSpilloverTick every 40ms) and the
// main loop (proactiveMigrationCheck every ~200ms). Previously a data race on
// std::set (clear/rebuild vs copy) caused UB / potential heap corruption.
static std::set<std::string> g_vmGuestCache;
static long long g_vmGuestCacheMs = 0;
static int g_vmContainerPidCached = -1;
static std::mutex g_vmGuestCacheMtx;

// Pure intersection: which manual-game inner packages actually run inside THIS
// container (cuid). runningUidByPkg maps a package name to its host-process uid
// (empty/absent = not running). Extracted so host tests can exercise the
// game_overrides+uid gate without /proc.
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
        if (it == runningUidByPkg.end()) continue;   // not running
        if (it->second != cuid) continue;             // running, but in another container/profile
        out.insert(innerPkg);
    }
    return out;
}

// v4.4.9 R1-1: returns a VALUE (snapshot) instead of a const-ref to the internal
// cache. The internal cache is now protected by g_vmGuestCacheMtx against
// concurrent access from the input-monitor epoll thread and the main loop.
// Callers get a consistent copy; no UB from concurrent clear/rebuild vs read.
std::set<std::string> resolveVmGuestPnames(const std::string& containerPkg) {
    std::lock_guard<std::mutex> lk(g_vmGuestCacheMtx);
    // Test seam bypasses /proc entirely.
    if (!aatpe_vm::g_forcedGuestPnames.empty()) return aatpe_vm::g_forcedGuestPnames;

    static const long kVmCacheTtlMs = 5000;  // PLACE-05: was 2000; raised to cut syscall cadence
    long long now = spillNowMs();
    int cpid = findPidByPackage(containerPkg);
    if (cpid <= 0) {
        g_vmGuestCache.clear(); g_vmContainerPidCached = -1; g_vmGuestCacheMs = now;
        return g_vmGuestCache;
    }
    // Valid cache: same container pid and not stale.
    if (!g_vmGuestCache.empty() && cpid == g_vmContainerPidCached &&
        (now - g_vmGuestCacheMs) < kVmCacheTtlMs) {
        return g_vmGuestCache;
    }
    long cuid = readPidUidForVm(cpid);
    g_vmGuestCache.clear();
    g_vmContainerPidCached = cpid;
    g_vmGuestCacheMs = now;
    if (cuid <= 0) return g_vmGuestCache;

    // v4.2.7 P2: STRICT intersection with the user's manual game_overrides list.
    std::string overrides = gamelist::currentManualGameList();
    if (overrides.empty()) return g_vmGuestCache;

    // PLACE-05 fix: pre-parse the overrides list into a set of inner package
    // names we actually care about. The /proc scan below then only calls
    // readPidUidForVm (which opens /proc/<pid>/status — an extra syscall per pid)
    // for pids whose comm matches one of those names. On a 300-process device this
    // cuts the per-scan syscalls from ~600 to ~300 + N (N = # guest games). The
    // cache TTL is also raised from 2s to 5s: container game launches are not
    // sub-second events, and the old 2s cadence was the dominant cost.
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

    // Single /proc scan: comm -> uid (last write wins; same container shares uid).
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
            // PLACE-05: only open /proc/<pid>/status when this comm is one of the
            // inner packages we are looking for.
            if (interesting.find(comm) == interesting.end()) continue;
            long uid = readPidUidForVm(pid);
            if (uid > 0) running[comm] = uid;
        }
        closedir(d);
    }
    g_vmGuestCache = intersectVmGuestOverrides(overrides, containerPkg, cuid, running);
    return g_vmGuestCache;
}


// Snapshot per-cluster average util from g_cpuMetrics. No nested locks:
// cluster members are copied under the topology lock first, then metrics read.
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

// Apply a PRIME-cluster affinity fallback (used when uclamp is unavailable, e.g.
// MTK 4.14 D1100 latches E2BIG). Allows ANY core in the PRIME cluster (pool mask,
// not a single-CPU pin). Saves the previous mask for restore on demote.
// Returns true if the mask was applied.
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
// Apply a cluster-pool affinity mask for the given target tier. BIG target uses
// bigPool (cpu4-6); PRIME target uses primePool (cpu7). Saves previous mask.
static bool applyTierAffinityFallback(int tid, int targetTier, cpu_set_t* prevMaskOut) {
    // v4.4.8 MAJOR-3/RCV M2: snapshot BOTH the PRIME and BIG pools under
    // g_topologyRwMutex. The PRIME branch already called primePool() (which locks
    // internally), but the BIG branch previously copied g_bigPool with NO lock while
    // detectCpuTopology() rewrites it under the same mutex on the hotplug/thermal
    // thread — a data race (torn vector read -> segfault / EINVAL). Take the lock
    // ONCE here and use primePoolLocked() (NOT primePool()) so we don't re-lock the
    // non-recursive mutex and self-deadlock (h6 D1 lesson, cf. line ~1675).
    std::vector<int> pool;
    {
        std::lock_guard<std::mutex> tlk(g_topologyRwMutex);
        pool = (targetTier >= (int)CpuTier::PRIME) ? primePoolLocked() : g_bigPool;
    }
    if (pool.empty()) return false;
    cpu_set_t mask; CPU_ZERO(&mask);
    for (int c : pool) CPU_SET(c, &mask);
    // ROUND-014 P3 #35: intersect with the thread's cpuset-allowed CPUs, else
    // sched_setaffinity returns EINVAL for a thread still in /background cpus.
    cpu_set_t allowed = getThreadAllowedCpus(tid);
    cpu_set_t inter; CPU_ZERO(&inter);
    for (int c : pool) if (CPU_ISSET(c, &allowed)) CPU_SET(c, &inter);
    if (CPU_COUNT(&inter) == 0) return false;   // empty -> caller degrades to uclamp
    if (sched_getaffinity(tid, sizeof(cpu_set_t), prevMaskOut) != 0) return false;
    if (sched_setaffinity(tid, sizeof(cpu_set_t), &inter) != 0) {
        LOG_WARN("spillover: tid=%d tier=%d affinity failed errno=%d", tid, targetTier, errno);
        return false;
    }
    return true;
}

#ifdef AATPE_HOST_TEST
// v4.4.8 MAJOR-3/RCV M2 host seams for the g_bigPool data-race regression.
//
// Writer: mimic detectCpuTopology() hotplug/thermal rewrite of g_bigPool UNDER
// g_topologyRwMutex (the production write side, aatped_topology.cpp:1095). The
// reader below runs applyTierAffinityFallback() from another thread; under TSan
// the OLD unlocked `pool = g_bigPool` copy races against this writer and is
// reported. The fix snapshots g_bigPool under the same mutex, so TSan is clean.
extern "C" void aatpePlacementRewriteBigPoolForTest(const int* cpus, int n) {
    std::lock_guard<std::mutex> lk(g_topologyRwMutex);
    g_bigPool.assign(cpus, cpus + n);
}
// Reader: drive the real applyTierAffinityFallback() on tid=0 (self; the host
// allows sched_setaffinity on the calling thread). Returns 1 if it applied.
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

// WS-C (G9 boot-sweep contract): format `tid:starttime:orig_affinity:mechanism`.
static void rewritePromotedTidsFile() {
    std::string out;
    for (auto& [tid, st] : g_spillState) {
        out += std::to_string(tid) + ":" + std::to_string(st.starttime) +
               ":" + st.origAffinity + ":" + std::to_string(st.mechanism) + "\n";
    }
    atomicWriteFile((std::string(STATE_DIR) + "/promoted_tids").c_str(), out);
}

namespace aatpe { namespace placement {
// ROUND-014 P3: symmetric release of ALL spillover promotions. Extracted from
// the veto block so shutdown / emergency edge can reuse it. Caller must hold
// g_spillStateMutex.
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
        // B-001: symmetric RT restore on the full-release path (shutdown / veto /
        // emergency). Mirrors the demote loop: drop the tid back to SCHED_OTHER /
        // recorded nice / recorded uclamp via the ungated channel, only when the
        // promote branch actually elevated it.
        if (st.rtBoosted) {
            aatpe_priority::aatpeRestoreThreadPriority(tid);
            st.rtBoosted = false;
        }
        // P1-2: only balance the counter for entries that actually completed a
        // promotion. In-flight entries (st.promoted==false, still accumulating
        // satisfyCycles) never had g_spilloverActive incremented, so sub'ing them
        // here underflows the count (observed as -8 on a busy VM session).
        if (st.promoted) g_spilloverActive.fetch_sub(1, std::memory_order_relaxed);
    }
    g_spillState.clear();
    // P1-2: hard clamp so a prior bad balance can never surface as negative.
    for (;;) {
        int cur = g_spilloverActive.load(std::memory_order_relaxed);
        if (cur <= 0) break;
        if (g_spilloverActive.compare_exchange_weak(cur, 0)) break;
    }
    rewritePromotedTidsFile();
}
}}  // namespace

// Production periodic sweep. Called from proactiveMigrationCheck() each cycle.
// No-op (zero action / zero noise) when the device has no PRIME cluster.
int placementSpilloverTick(const Config& cfgSnap) {
    // PLACE-03 fix: non-blocking re-entrancy guard. The main loop calls this
    // every ~200ms; the input epoll thread calls it via onInputEvent() on touch.
    // Previously both paths serialised on g_spillStateMutex, so a touch arriving
    // while the main loop was mid-tick (cgroup writes under the spill lock)
    // blocked the input thread for the duration of that tick, eating into the
    // 16ms touch budget. With this CAS, the second caller simply returns 0;
    // the next periodic main-loop tick (≤200ms away) will pick up the new
    // state. Emergency release is still handled because whichever tick holds
    // the guard processes the current emergency/thermal state.
    static std::atomic<bool> s_tickInFlight{false};
    bool expected = false;
    if (!s_tickInFlight.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        return 0;   // another tick is in progress; skip this one
    }
    struct TickGuard { ~TickGuard() { s_tickInFlight.store(false, std::memory_order_release); } } tickGuard;

    aatpePruneDeadRtTids();   // ROUND-014 P3: free dead RT slots
    // Thermal emergency: never promote; release any promoted threads back to the
    // general mask (fire-fighting: don't pile load on PRIME while throttling).
    bool emergency = (getSystemState() == SystemState::EMERGENCY);

    std::vector<ClusterLoad> loads = snapshotClusterLoads();
    // WS-C: dynamic top cluster by capacity order, NOT the PRIME label (all-big
    // devices have no PRIME tag). Require >=2 clusters; top = clusters.back().
    if (loads.size() < 2) return 0;

    // ROUND-008b: cluster_pressure state file — one line per cluster.
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

    // A3: resolve fg + VM guest pnames BEFORE collecting critical threads, so the
    // collector can reserve truncation slots for the fg/guest package's threads.
    std::string fgPkg = getCachedForegroundApp();
    bool fgKnown = !fgPkg.empty() && fgPkg != "unknown";
    std::set<std::string> vmGuestPnames;
    if (fgKnown && isVmContainerPkg(fgPkg)) {
        vmGuestPnames = resolveVmGuestPnames(fgPkg);
    }
    // Priority set for the collector: fg pkg + resolved VM guest pnames.
    std::set<std::string> critPriority;
    if (fgKnown) critPriority.insert(fgPkg);
    for (const auto& g : vmGuestPnames) critPriority.insert(g);
    std::vector<ThreadInfo> crit = collectCriticalThreads(8,
        critPriority.empty() ? nullptr : &critPriority);

    // v4.2 A1: SF/compositor thread observability — count how many of the current
    // critical set hit the SF whitelist. Written as a uint state key for Batch C
    // (doctor/WebUI). Read-only; does not affect promotion decisions.
    {
        int sfCount = 0;
        for (const auto& ti : crit) {
            if (aatpeIsSurfaceFlingerThread(ti.tname.c_str())) sfCount++;
        }
        writeFile((std::string(STATE_DIR) + "/sf_thread_count").c_str(),
                  std::to_string(sfCount));
    }
    // v4.2 A3: capacity self-calibration. No-op unless charging + screen-off + idle
    // AND no valid calibration on disk; never blocks the tick.
    ::aatpe::caps::runAutocalIfDue();

    bool vetoActive = false;
    // ROUND-009c: holistic jank root-cause arbitration.
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
        // debounce: same cause for 2 consecutive cycles before acting.
        if (cid == g_prevCause.load()) g_debounce.fetch_add(1);
        else { g_debounce.store(0); g_prevCause.store(cid); }
        bool stable = g_debounce.load() >= 1;
        // write state file every tick (NONE when idle).
        std::string line = std::string("cause=") + jankdiag::causeName(v.cause) +
                           " confidence=" + std::to_string(v.confidence) +
                           " evidence=" + v.evidence + "\n";
        writeFile((std::string(STATE_DIR) + "/jank_root_cause").c_str(), line);
        // Arbitration: THERMAL/GPU_BOUND/IO/MEM veto CPU promotion.
        static const std::set<jankdiag::Cause> s_veto = {
            jankdiag::Cause::THERMAL_THROTTLE, jankdiag::Cause::GPU_BOUND,
            jankdiag::Cause::IO_STALL, jankdiag::Cause::MEM_RECLAIM };
        vetoActive = (stable && s_veto.count(v.cause));
    }

    // ---- Frame-independent scheduling-pressure fallback arm (C1/C3/C4) ----
    // Re-snapshot the holistic pressure here (independent of the veto block above,
    // whose own `sp` is scoped inside it). Cheap; keeps this backstop self-contained.
    diag::SystemPressure sp = diag::getSystemPressureSnapshot();
    // Sample the heaviest foreground render/game critical thread's runqueue-wait.
    float pfWaitRatio = -1.0f;
    for (const auto& ti : crit) {
        if (ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME) {
            pfWaitRatio = placementThreadRqWaitRatio(ti.tid);
            break;
        }
    }
    // Never arm while throttling: the jank classifier above only vetoes when a
    // frame vote is present, so on a frame-dead device we must guard thermal/GPU
    // pressure ourselves here. We do NOT relax any thermal protection.
    bool pfThrottled = sp.cpuThrottled || sp.gpuThrottled || vetoActive ||
                       g_thermalEmergency.load(std::memory_order_relaxed);
    bool pfPsiArm   = sp.psiAvailable && sp.psiCpuSomePct >=
                      aatpe::placement::pressurefb::kPsiCpuArmPct;
    bool pfPsiClear = sp.psiAvailable && sp.psiCpuSomePct <
                      aatpe::placement::pressurefb::kPsiCpuClearPct;
    bool pfWaitArm   = pfWaitRatio >= aatpe::placement::pressurefb::kWaitArmRatio;
    bool pfWaitClear = pfWaitRatio <  aatpe::placement::pressurefb::kWaitClearRatio;
    // Hysteresis persisted across ticks (atomics, TSan-clean). Arm on sustained
    // PSI stall OR render-thread starvation; release only after several quiet ticks.
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
    // L-PLACE-1: comm (RenderThread/GAME/UI) is spoofable via prctl(PR_SET_NAME).
    // Gate promotion on the process being the real foreground app (same truth source
    // as thread_scan's fgPid). A background/backdated process cannot fake its way
    // onto PRIME by renaming a thread.
    // fgPkg/fgKnown/vmGuestPnames were resolved above (A3) and used to reserve
    // critical-thread slots; reuse them here for the pname admission gate.
    // C3 provenance: tids admitted ONLY via the unknown-fg pressure window (not the
    // manual list) are recorded here so the PRIME jump below can keep them bounded to
    // one step up — an unknown-package renderer is starved enough to lift, but not
    // trusted for the full PRIME slot.
    std::set<int> pfBoundedOnly;
    for (const auto& ti : crit) {
        // W2-X F3: admission gate. When foreground attribution is known, admit the
        // foreground package (and VM guests). When it is UNKNOWN, do NOT silently
        // drop every thread (the old `if(!fgKnown) continue;` made user-added apps
        // like 快手 get ZERO promotion on devices where fg detection fails). Instead
        // open a window ONLY for packages the user explicitly added to the manual
        // game list — this keeps the spoof guard (a random background app cannot
        // rename a RenderThread onto PRIME) while guaranteeing a manual-list app
        // still gets promoted. Manual-list read goes through the existing
        // gamelist::isManualGame (mtime-cached hot reload); no new list plumbing.
        bool allowed;
        if (fgKnown) {
            allowed = (ti.pname == fgPkg) ||
                      (!vmGuestPnames.empty() && vmGuestPnames.count(ti.pname));
        } else {
            // C3: foreground attribution unknown. The default keeps the spoof guard
            // (only user-manual packages). But when the independent pressure fallback
            // is armed, the actual foreground RENDER/GAME critical threads collected
            // via collectCriticalThreads()/eBPF must still be eligible — otherwise the
            // vote arms yet lands nowhere ("票白 arm"). Evidence the thread is the
            // starved foreground renderer: its OWN measured runqueue-wait is high. The
            // resulting promotion stays bounded to one step up (see the proactive block
            // below; it never jumps straight to PRIME), so any spoof blast radius is
            // limited. When no pressure fallback is armed this branch is unreachable
            // and behaviour is byte-identical to the manual-list-only gate.
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
        cands.push_back({ti.tid, (int)ti.role, /*critical=*/true, weight, tier});
        tidRole[ti.tid] = ti.role;
    }

    // Emergency overrides the desired set: promote nothing, release everything.
    std::vector<PromoteTarget> targets = emergency ? std::vector<PromoteTarget>{} :
        decidePromotions(loads, cands, primeUtil, mode, spillover::kStableCycles,
                         pfArmedNow);   // C4: single-thread latency/PSI bounded spillover
    // ROUND-014 P3 C32: relative top tier (works for 1/2/3/4 clusters).
    // B-002: lock-protected snapshot (this path does NOT hold g_topologyRwMutex).
    int maxTier = 0;
    for (const auto& cl : clustersSnapshot()) maxTier = std::max(maxTier, (int)cl.tier);
    // ROUND-007c #6: sustained-jank boost vote.
    // C1: OR with the frame-independent scheduling-pressure fallback. The frame
    // vote (OEM SurfaceFlinger/jank source) and the PSI/eBPF-sched-wait fallback are
    // independent producers of the SAME heaviest-thread promotion; either one arms
    // it and neither can veto the other. The fallback is already hysteresis-gated and
    // thermal/throttle-gated above, and the !cands.empty() guard is unchanged.
    if (!emergency && (frameaware::jankBoostVoteActive() || pfArmedNow) && !cands.empty()) {
        auto heaviest = std::max_element(cands.begin(), cands.end(),
            [](const ThreadCand& a, const ThreadCand& b){ return a.weight < b.weight; });
        // C3: a thread admitted only via the unknown-fg pressure window is starved
        // enough to lift one step up, but is NOT trusted for the full PRIME slot —
        // skip the PRIME jump for it; the proactive/reactive spillover below gives it
        // the bounded BIG placement instead. fg-known / manual cands are unaffected.
        bool boundedOnly = pfBoundedOnly.count(heaviest->tid) > 0;
        if (heaviest->critical && heaviest->curTier < maxTier && !boundedOnly)
            targets.push_back({heaviest->tid, maxTier});
    }
    // W2-X F3: proactive critical-thread placement. The reactive spillover only
    // moves a thread up once the LITTLE cluster is already saturated — on a
    // foreground (or user-manual) game whose render thread is stuck on a LITTLE
    // core (single-threaded game render, or load accounting that never saturates
    // the whole little cluster), the module did nothing. Proactively WANT the
    // heaviest RENDER/GAME critical threads that are still on LITTLE/MID to be
    // hosted on the BIG tier (one step up). This only adds them to the desired
    // set; the existing kStableCycles debounce, min-dwell demotion and the
    // prime-top-1 balancing below still gate the actual promotion, and threads
    // already on BIG/PRIME are skipped (no double-promotion, no grabbing the
    // single PRIME core). cands are already filtered to the foreground package
    // (or manual-list packages when fg is unknown). No model/package hardcoding.
    // v4.3.6 PSI seam: under memory-reclaim pressure, big-core migration is
    // expensive (reclaim + cache miss) -> de-weight this proactive BIG push.
    // audit4 efficiency: also soft-defer when the arbitrator reports the boost is
    // inefficient this window (efficiency_hint >= 0.5). Read-only; missing file = 0.
    // C3 exploration: thermal placement scale with hysteresis (avoid oscillation).
    // Suppress proactive BIG push when scale < 0.50 (arm), release only when > 0.70.
    // Only tighten, never loosen; reactive/emergency/spillover are unaffected.
    // Atomic: placementSpilloverTick may be called from main loop AND onInputEvent().
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
        const int bigTarget = maxTier - 1;   // one step off LITTLE/MID onto BIG
        // Dedup: do not re-target a tid the reactive spillover already placed (it
        // may be headed to PRIME; a duplicate lower-tier entry would win the
        // last-write tidTier map and override that promotion).
        std::set<int> already;
        for (const auto& t : targets) already.insert(t.tid);
        std::vector<const ThreadCand*> heavy;
        for (const auto& c : cands) {
            if (!c.critical) continue;
            if (c.role != ROLE_S2_RENDER && c.role != ROLE_S3_GAME) continue;
            if (c.curTier <= (int)CpuTier::UNKNOWN) continue;  // can't confirm tier
            if (c.curTier >= bigTarget) continue;   // already on/above BIG
            if (already.count(c.tid)) continue;      // reactive already placed it
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
    // ROUND-014 P3: PRIME Top-1 — a single prime core can only host one heavy
    // thread; demote the rest to the next tier to avoid core thrash.
    // W2-X F1: the slot count scales with the detected PRIME pool size. D1100 has
    // 1 prime core -> 1 slot (unchanged); a 2-core PRIME pool (8-Elite 2+6 /
    // Dim900 2+6 under the ratio-lead detector) hosts up to 2 heavy threads
    // instead of demoting the 2nd render thread to LITTLE. Read under the topology
    // lock; fall back to 1 if the pool is unknown.
    int primeSlots = 1;
    {
        std::lock_guard<std::mutex> tlk(g_topologyRwMutex);
        // h6 D1 self-deadlock: we already hold g_topologyRwMutex here; primePool()
        // would re-lock the same non-recursive std::mutex -> deadlock (device
        // froze at beat 33). Use the caller-holds-the-lock variant.
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
    // ROUND-014 P3: dwell scales continuously with refresh rate (frame-budget).
    {
        int hz = frameaware::getRefreshRate();
        if (hz > 0) dwell = dwell * 60 / hz;
    }
    // ROUND-005: unified mechanism selection — ONE primary boost per thread,
    // no uclamp+affinity stacking. D1100 (MTK 4.14, no uclamp) picks STUNE_CPUSET.
    aatpe_mech::SchedCaps caps = aatpe_mech::probeCapabilities();
    aatpe_mech::BoostMechanism mech = aatpe_mech::selectBoostMechanism(caps);
    int newly = 0;
    // v4.2 A2: captured inside the spill lock, consumed outside it (the schedtune
    // bridge has its own mutex; we must not call it while holding g_spillStateMutex).
    bool anyPromoted = false;
    // PLACE-04 fix: pre-read the per-tid cgroup state (readThreadCpusetGroup +
    // readThreadCgroupInfo) OUTSIDE g_spillStateMutex. These are /proc/<tid>/cgroup
    // reads (one syscall each) used by the CGROUP_UCLAMP / CGROUP_CPUSET branches.
    // Doing them under the spill lock was adding ~2 syscalls per promoted thread to
    // the critical section, which the /state/all snapshot and the input-epoll path
    // had to wait on. We cache the results here and replay them inside the lock,
    // keeping the lock hold time dominated only by the cgroup *writes* (which must
    // be ordered against the demote loop).
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
        // WS-C T1: veto release INSIDE the mutex, symmetric by mechanism.
        if (vetoActive) {
            aatpe::placement::releaseAllSpilloverLockedImpl();
            // v4.2 A2: veto clears the "placement" schedtune producer so the bridge
            // collapses back to max(touch,gpu) = v4.1.1 behaviour.
            aatpeBoostClear("placement");
            return 0;
        }
        for (int tid : desiredSet) {
            auto& st = g_spillState[tid];
            st.satisfyCycles++;
            // v4.3.6 PSI seam: under SEVERE runqueue stall the closed-loop drives
            // cheap group-uclamp floors directly; pause NEW thread migration churn
            // (affinity/cgroup moves) so we do not worsen the stall. satisfyCycles
            // keeps accruing, so threads promote immediately once the pause lifts.
            if (!st.promoted && st.satisfyCycles >= spillover::kStableCycles &&
                !::aatpe::psi::noncriticalMigrationPaused()) {
                st.promoted = true;
                st.promotedMs = nowMs;
                st.targetTier = tidTier[tid];
                st.mechanism = (int)mech;   // default; downgrade paths below may rewrite
                // WS-C C: real /proc/<tid>/stat field-22 starttime (TID reuse guard).
                // v4.2 A1: also grab the thread comm here so the SF/compositor
                // whitelist check (floorUclampForThreadLocked) runs once, under the
                // existing g_threadsMutex lock — no extra lock / syscall.
                std::string tname;
                {
                    std::lock_guard<std::mutex> tdbLock(g_threadsMutex);
                    auto thIt = g_threads.find(tid);
                    if (thIt != g_threads.end()) {
                        st.starttime = thIt->second.starttime;
                        tname = thIt->second.tname;
                    }
                }
                // v4.2 A1/A3: compute the uclamp.floor ONCE for this thread. The
                // helper applies the SF/compositor tier (mode=3, uclampSfPct) BEFORE
                // the PRIME early-return, and uses the autocal-big ratio (defaults
                // 0.87f). All boost-mechanism branches below reuse this single umin.
                int umin = floorUclampForThreadLocked(tname.c_str(), st.targetTier,
                                                      ::aatpe::caps::autocalBigRatio(), mode);
                g_spilloverPromotions.fetch_add(1, std::memory_order_relaxed);
                g_spilloverActive.fetch_add(1, std::memory_order_relaxed);
                if (mech == aatpe_mech::BoostMechanism::UCLAMP) {
                    // WS-C: real per-task uclamp.min by target tier (R2 §6).
                    // audit4 F4: source-tagged ledger (SPILLOVER) so touch/priority
                    // floors are merged by max() and never stomped on release.
                    int rc = aatpe_mech::aatpeUclampFloorSet(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER, (unsigned int)umin);
                    if (rc != 0) {
                        // EPERM/EOPNOTSUPP: uclamp unavailable -> fall to affinity.
                        st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                        st.mechanism = (int)aatpe_mech::BoostMechanism::AFFINITY;
                        // #3: affinity empty -> uclamp-only last resort; record mechanism on success.
                        if (!st.maskApplied) applyLastResortUclamp(tid, st, (unsigned int)umin);
                    }
                    // B-001: record whether RT elevation actually took, so the symmetric
                    // restore (demote loop + releaseAllSpilloverLockedImpl) fires only
                    // when we truly changed the thread's scheduling class. set returns
                    // true for SCHED_FIFO/RR OR the nice-degrade fallback; both mutate
                    // state and must be undone on release. false (SYSTEM role / safe
                    // mode / all syscalls failed) => nothing to restore.
                    st.rtBoosted = aatpe_priority::aatpeSetThreadPriority(tid, tidRole[tid],
                                                           cfgSnap.priorityBoostLevel);
                } else if (mech == aatpe_mech::BoostMechanism::CGROUP_UCLAMP) {
                    // WS-C: cgroup-level uclamp.min (v2 percent / v1 raw). Failure
                    // (group vanished / unwritable) retries once then degrades.
                    // PLACE-04: use the pre-read cpuset/cgroup info captured OUTSIDE
                    // the spill lock instead of re-reading /proc here.
                    st.savedCpuset = preSavedCpuset[tid];
                    // H14: also capture full path + hierarchy for .rec persistence.
                    {
                        auto it = preCgroupPathHier.find(tid);
                        if (it != preCgroupPathHier.end()) {
                            aatpe_recovery::recoveryRecordCgroupPath(tid, it->second.first, it->second.second);
                        }
                    }
                    bool ok = aatpe_mech::writeCgroupUclampMin(tid,
                        (unsigned int)umin, "top-app");
                    // PLACE-08: single immediate retry is intentional — cgroup fs
                    // writes can transiently fail when the cgroup is being created/
                    // migrated by AMS. No backoff (we are inside spill lock); the
                    // fallback below handles persistent failure.
                    if (!ok) ok = aatpe_mech::writeCgroupUclampMin(tid,
                        (unsigned int)umin, "top-app");
                    if (!ok) {
                        // P1 round7: tid was already migrated into the top-app cgroup,
                        // but the uclamp floor write failed. Roll the tid back to its
                        // original cpuset group so it does NOT get stranded as a
                        // permanent top-app boosted thread.
                        if (!st.savedCpuset.empty()) {
                            aatpe_mech::moveTidToCpusetGroup(tid, st.savedCpuset);
                        }
                        st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                        st.mechanism = (int)aatpe_mech::BoostMechanism::AFFINITY;
                        if (!st.maskApplied)
                            applyLastResortUclamp(tid, st, (unsigned int)umin);
                    }
                } else if (mech == aatpe_mech::BoostMechanism::STUNE_CPUSET) {
                    // PLACE-04: use pre-read cpuset/cgroup info.
                    st.savedCpuset = preSavedCpuset[tid];
                    {
                        auto it = preCgroupPathHier.find(tid);
                        if (it != preCgroupPathHier.end()) {
                            aatpe_recovery::recoveryRecordCgroupPath(tid, it->second.first, it->second.second);
                        }
                    }
                    // PRIME target: only route via top-app if its cpus cover PRIME;
                    // otherwise fall to tier affinity mask.
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
                        // #3: last-resort uclamp records mechanism=UCLAMP on success.
                        if (!st.maskApplied)
                            applyLastResortUclamp(tid, st, (unsigned int)umin);
                    }
                } else {
                    st.maskApplied = applyTierAffinityFallback(tid, st.targetTier, &st.savedMask);
                    // #3: last-resort uclamp records mechanism=UCLAMP on success.
                    if (!st.maskApplied)
                        applyLastResortUclamp(tid, st, (unsigned int)umin);
                }
                // WS-C C: boot-sweep record field 3 = real restore target.
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
                // ROUND-015/#A.2: periodic kernel-membership readback. The promoted
                // belief is daemon-side; AMS may have moved the thread out of the
                // boosted cgroup at process granularity. Self-heal exactly like the
                // RT channel does via schedGetAttr. No-op unless live membership
                // actually drifted from "top-app".
                reexertMembershipLocked(tid, st);
            }
        }
        // ROUND-015/#A.1: fg->bg->fg edge sweep. The edge (armed by
        // onForegroundSwitchEdge) demands an immediate, debounce-free re-pin of the
        // returned-to-foreground app's promoted threads EVEN IF they are not in the
        // current top-8 critical set (cold metrics) or already carry promoted==true
        // (which would otherwise short-circuit the promote block above).
        if (g_fgEdgeReapply.exchange(0) != 0) {
            std::string edgePkg;
            { std::lock_guard<std::mutex> elk(g_fgEdgeMtx); edgePkg = g_fgEdgeReapplyPkg; }
            // v4.2.7: VM container — also re-pin guest game threads on fg-edge.
            std::set<std::string> edgeVmGuests;
            if (isVmContainerPkg(edgePkg)) edgeVmGuests = resolveVmGuestPnames(edgePkg);
            for (auto& [tid, st] : g_spillState) {
                if (!st.promoted) continue;
                // Match on the thread's package; unknown (already scanned out) still
                // gets re-pinned — the edge already gated on the package.
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
        // Demote / release. Iterate over a copy of keys because we erase.
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
            if (desiredSet.count(tid)) continue;        // still wanted
            if (nowMs - st.promotedMs < dwell && !emergency) continue;  // hold within dwell
            // audit5 N3 (U-02): TID-reuse identity guard. Before we touch this tid's
            // scheduling attributes on demote, confirm the LIVE thread is still the
            // same instance we promoted. Read /proc/<tid>/stat field-22 starttime; if
            // it cannot be read (thread exited) OR no longer equals the starttime we
            // recorded at promote time, the tid has been recycled by a new thread (or
            // the old one is gone). The spillover record belonged to the old instance,
            // so reclaim it and SKIP every restore / uclamp write — never demote an
            // unrelated RT/normal thread that merely inherited this tid. (The promote
            // side already records st.starttime = thIt->second.starttime; this closes
            // the loop on the demote side.)
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
            // demote: restore affinity and/or cpuset/uclamp symmetrically.
            bool restoreOk = true;
            if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::UCLAMP)
                aatpe_mech::aatpeUclampFloorClear(tid, aatpe_mech::AATPE_UCLAMP_SPILLOVER);   // drop spillover lift
            if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::CGROUP_UCLAMP)
                restoreOk = aatpe_mech::restoreTidFromCgroupUclamp(tid, "top-app", st.savedCpuset);
            if (st.maskApplied) { restoreAffinity(tid, st.savedMask); st.maskApplied = false; }
            if ((aatpe_mech::BoostMechanism)st.mechanism == aatpe_mech::BoostMechanism::STUNE_CPUSET) {
                // A.5: only pull the thread back to savedCpuset while we still own its
                // membership (it is still in top-app). If AMS already process-level-moved
                // the thread to background while it left the foreground, do NOT yank it
                // back into top-app — leave it where the kernel/AMS put it.
                std::string live = aatpe_mech::readThreadCpusetGroup(tid);
                if (live.empty() || live == "top-app") {
                    std::string g = st.savedCpuset.empty() ? "foreground" : st.savedCpuset;
                    aatpe_mech::moveTidToCpusetGroup(tid, g);
                }
            }
            // B-001: symmetric RT restore. The UCLAMP promote branch elevated this tid
            // to SCHED_FIFO/RR (+uclamp); undo it here on demote via the ungated restore
            // channel. Guarded by rtBoosted so non-UCLAMP / failed-promote tids are not
            // touched. Best-effort, never throws.
            if (st.rtBoosted) {
                aatpe_priority::aatpeRestoreThreadPriority(tid);
                st.rtBoosted = false;
            }
            // #4: cgroup-uclamp move-back FAILED -> restoreTidFromCgroupUclamp already
            // decided NOT to decrement the group floor (the thread may still be
            // physically in top-app and relying on it; tearing the floor down would
            // be the original #4 bug). We still reclaim the per-tid record here
            // (preserving the promoted_tids boot-sweep contract); the elevated floor
            // is conservative and bounded — it is released by restoreAllCgroupUclamp
            // on shutdown/emergency and re-balanced by the next promotion cycle.
            if (!restoreOk) {
                st.mechRestoreRetries++;
                LOG_WARN("spillover: demote tid=%d cgroup move-back failed (retry=%d); "
                         "keeping group floor up", tid, st.mechRestoreRetries);
            }
            g_spilloverActive.fetch_sub(1, std::memory_order_relaxed);
            LOG_INFO("spillover: demote tid=%d from PRIME (emergency=%d via=%s)", tid,
                     (int)emergency, aatpe_mech::mechanismName((aatpe_mech::BoostMechanism)st.mechanism));
            g_spillState.erase(it);                    // reclaim tid entry (no leak)
        }
        // v4.2 A2: snapshot whether any promotion is live, under the spill lock.
        anyPromoted = !g_spillState.empty();
    }
    // v4.2 A2: drive the "placement" schedtune.boost producer via the single-writer
    // bridge (max-aggregated with touch/gpu). Called OUTSIDE g_spillStateMutex (the
    // bridge takes its own s_boostMutex). When there is no active promotion window
    // (empty set / emergency / thermal-degraded) the desired level is -1 -> Clear,
    // so the bridge value collapses to max(touch,gpu) = v4.1.1 bit-identical
    // default. Event-aligned (Set during the spillover window, Clear on
    // demote/release/thermal), NOT a resident every-tick write.
    {
        bool degraded = (getSystemState() == SystemState::DEGRADED);
        int lvl = placementSchedtuneDesiredLevel(anyPromoted, emergency, degraded, mode);
        if (lvl < 0) aatpeBoostClear("placement");
        else         aatpeBoostSet("placement", lvl);
    }
    rewritePromotedTidsFile();   // sync boot-sweep contract
    return newly;
}
} // namespace aatpe_spillover

// ROUND-014 P3: exported entry for shutdown/emergency. Takes the spill lock
// then forwards to the Locked impl (which lives inside aatpe_spillover).
namespace aatpe { namespace placement {
void releaseAllSpilloverLocked() {
    std::lock_guard<std::mutex> lk(aatpe_spillover::g_spillStateMutex);
    aatpe_spillover::aatpe::placement::releaseAllSpilloverLockedImpl();
}
}}

// ROUND-012: on foreground switch edge, restore original affinity for the new
// foreground app's threads that we previously pinned to LITTLE.
namespace aatpe { namespace placement {
void releaseForegroundDownpins(const std::string& newFg) {
    if (newFg.empty() || newFg == "unknown") return;
    // v4.4.1 P0: probe once whether we can actually rewrite cpuset membership.
    aatpe_mech::SchedCaps caps = aatpe_mech::probeCapabilities();
    std::vector<int> touched;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        for (auto& [tid, ti] : g_threads) {
            if (ti.pname != newFg) continue;
            // v6.0.0 P0 (realdevice "locked little core"): edge-side cgroup lift must
            // NOT be gated on affinityModified. AMS parks the whole task group in the
            // background cpuset (0-3) on PiP/freeform/split/short-focus-loss; threads
            // AATPE never migrated have affinityModified==false, so the old loop skipped
            // them entirely and they stayed pinned to LITTLE after returning to the
            // foreground. For the new fg package's critical roles (UI/Render/Game) we
            // now read the live cgroup and lift to top-app REGARDLESS of affinityModified
            // — but only lift the cgroup (membership), never rewrite the affinity mask;
            // the normal placement loop owns masks for threads AATPE never pinned.
            bool criticalRole = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
            if (ti.affinityModified) {
                // v4.4.1 P0 (RC-1c): BEFORE restoring the affinity mask, first lift the
                // thread out of whatever cpuset the system parked it in during the
                // background window (background cpus=0-3). The captured originalAffinity
                // may itself have been clipped to 0-3 while the thread was confined
                // there; restoring it would leave it stuck on LITTLE forever. Moving
                // it back into top-app frees the allowed mask, then sched_setaffinity
                // is no longer clipped. Honest no-op when cpuset is not writable.
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
                // Never pinned by AATPE: only lift cgroup to top-app if the system
                // parked it elsewhere; do NOT touch the affinity mask.
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

// v4.4.1 P0 (RC-1a/RC-1b): periodic, NON-edge cgroup readback heal for the CURRENT
// foreground package's critical threads.
//
// WHY: AMS migrates the whole task group out of top-app (into the background
// cpuset, whose cpus are 0-3) at process granularity whenever an app is briefly
// backgrounded OR put into PiP / freeform. The module's old recovery was entirely
// package-NAME-edge triggered (loopForegroundTick only runs on curFg != prevFg).
// Entering/exiting a small window keeps the SAME package resumed, so the edge
// never fires and the heal never runs. Meanwhile every big-core sched_setaffinity
// for those threads is rejected by the cpuset pre-check (target cpu not in the
// thread's allowed mask -> EINVAL), so they stay stuck on LITTLE.
//
// This sweep closes that hole WITHOUT an edge: every ~1s, for the current fgPkg's
// critical threads (UI/Render/Game only), read the live cgroup membership; if it
// is not "top-app", rewrite the tid back into top-app. The normal placement loop
// then free-places them onto the big cluster.
//
// SAFE BY CONSTRUCTION:
//  - only pname == fgPkg threads are touched (never a background app);
//  - only critical roles S1_UI/S2_RENDER/S3_GAME (never S5 background threads);
//  - SCREEN MUST BE ON: with the screen off/standby the "fg" package is really the
//    last-used app being parked by the system — lifting it to top-app all night
//    would fight system suspend and drain battery (red-team MAJOR-1). No-op off.
//  - passive mode: no proactive migration (upgrade-only, respects user choice);
//  - v6.0.0: only REAL CPU thermalEmergency (sysfs wall) blocks the lift; skin
//    throttle (thermalHalLevel>=3 with !thermalEmergency) no longer walls it;
//  - cpusetTopApp capability must be writable, else honest no-op;
//  - no hardcoded path / SoC — works on 1+3+4, 2+6, all-big, MTK/Qualcomm/Tensor.
//
// v4.4.1 red-team: the per-thread DECISION is a pure function (healShouldLiftThread)
// so it is host-testable; this function only does IO (enumerate threads, read live
// cgroup, write top-app). The pure decision lives in its own zero-dependency TU
// (placement_heal_decision.cpp) so the host test can link it without the whole
// placement graph; it is declared in aatped.h.
// local screen-on probe (fb0/blank, same single source as frame.cpp; missing -> on).
static bool fgHealScreenOn() {
    FILE* f = fopen("/sys/class/graphics/fb0/blank", "r");
    if (!f) return true;  // unreadable -> assume on (fail-open for recovery)
    int blank = 0;
    if (fscanf(f, "%d", &blank) != 1) { fclose(f); return true; }
    fclose(f);
    return (blank == 0);
}
static std::atomic<long long> g_lastFgCgroupHealMs{0};
void healForegroundCgroupMembership(const std::string& fgPkg) {
    if (fgPkg.empty() || fgPkg == "unknown") return;
    // ~1s throttle: reading /proc/<tid>/cgroup for every tracked thread each tick
    // is wasteful; membership changes only at app/PiP switches.
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

    // Windowing check: only heal when the foreground app is fullscreen.
    // Skip PiP/pinned(2), freeform(5), split-screen(3) — the system intentionally
    // moved those threads out of top-app; fighting it wastes battery and confuses
    // the window manager. Missing/unreadable = unknown = default to heal (safe).
    std::string winMode = readFile((std::string(STATE_DIR) + "/frame_windowing_mode").c_str());
    bool windowingHealAllowed = (winMode == "fullscreen" || winMode == "unknown" || winMode.empty());
    if (!windowingHealAllowed) {
        return;  // PiP / freeform / split-screen: leave cgroup as-is
    }

    // Snapshot thread metadata under the lock, then release it before doing
    // /proc cgroup I/O (which can block). This avoids holding g_threadsMutex
    // during slow reads, which would stall the eBPF sched_switch updater.
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
    // Lock released; do cgroup I/O outside the critical section.
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
}}  // namespace

// B3收尾: public, thread-safe spillover observability accessors (called by G1
// from the state-export path for WebUI / on-device verification). Atomic reads.
#include "placement.h"
namespace aatpe {
namespace placement {
std::uint64_t spilloverPromotionCount() noexcept {
    return static_cast<std::uint64_t>(
        aatpe_spillover::g_spilloverPromotions.load(std::memory_order_relaxed));
}
int spilloverActiveCount() noexcept {
    // ROUND-007 P0-1: probe capabilities unconditionally on the state-export
    // path. ...
    aatpe_mech::probeCapabilities();
    // P1-2: never surface a negative count (defensive against any historical
    // imbalance). The authoritative balance is maintained at add/sub sites;
    // this clamp is the final read-side guard.
    int v = aatpe_spillover::g_spilloverActive.load(std::memory_order_relaxed);
    return v > 0 ? v : 0;
}

// ROUND-007b: event-driven immediate boost. Called by the input_monitor /
// cold-start event path the moment a touch/launch event fires — WITHOUT waiting
// for the next 200ms periodic tick (touch budget is 16ms; PELT half-decay 32ms).
// It runs the same spillover promotion evaluation immediately. Boost is
// short-lived: the promoted threads auto-release after the new short dwell
// (60-150ms) and kInputBoostWindowMs. Thread-level only (uclamp/top-app cpuset/
// affinity); never raises a global min_freq (research: power-costly, EAS back-
// pressured). No-op when EMERGENCY or no PRIME cluster.
void onInputEvent() noexcept {
    // Fix-D: noexcept callback — swallow any exception (bad_alloc / unexpected
    // std::string / map rehash) so the root daemon never std::terminate()s from
    // an epoll/input event path. Happy-path side effects are unchanged.
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

// ROUND-015: foreground-switch edge. loopForegroundTick already detected curFg
// freshly; we (1) publish it into the placement-side fg cache so the immediately
// following onInputEvent()->tick does not act on a stale 3s-TTL package, and
// (2) arm the debounce-free re-pin sweep consumed by that tick.
void onForegroundSwitchEdge(const std::string& newFg) noexcept {
    // Fix-D: same noexcept guard as onInputEvent(). The early empty/unknown bail
    // is a pure comparison and cannot throw, but keep it inside the try so any
    // future edit to the guard stays covered.
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
}  // namespace placement
}  // namespace aatpe

// scoreOneCandidate(): the SINGLE-CPU scoring pipeline, factored out of the
// original per-candidate loop body. It performs the EXACT same sequence the loop
// used to run inline:
//   computeScore -> extractMlFeaturesCtx -> pool cold-start gate -> fuseMlScore
//   -> audio migration penalty -> hybrid fine-tune -> ADRC boost/clamp.
// It performs NO best/second selection; it only returns the raw per-candidate
// (heuristic, finalScore, fusion, features). This is shared by:
//   (a) computeCandidateScores()          — the main ranking loop, and
//   (b) recomputeScoreForTarget()         — re-deriving metadata after
//       applyColdStartExploration() / applySmartPlacementFallback() move the
//       target (P1-2). Sharing one implementation guarantees the re-derived
//       score uses the IDENTICAL heuristic/ML/hybrid/history weights — no
//       divergence between the ranking loop and the post-fallback recompute.
// g_mlMutex is held by the caller (it touches g_mlModel + s_mlPredictCache).
static void scoreOneCandidate(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                              const ScoringContext& scoreCtx,
                              bool mlActive, float mlAlpha,
                              bool adrcBoostActive, float adrcBoostUrgency,
                              bool adrcClampActive,
                              int cpuId,
                              int& outHeuristic, float& outFinalScore,
                              DecisionFusion& outFusion,
                              ThreadFeatures& outFeatures, bool& outFeaturesValid) {
    // P2 Problem6: pass liveCpu so the migration-cost term inside computeScore
    // is scored against the same fresh fromCpu used for fromPool/hf[9] below.
    int heuristic = computeScore(cpuId, ti, liveCpu, scoreCtx);
    float finalScore = (float)heuristic;
    DecisionFusion fusion;  // declared at scope for outFusion tracking
    // P0-1: Extract ML features whenever mlActive, regardless of alpha.
    ThreadFeatures features;
    bool featuresValid = false;
    if (mlActive || cfgSnap.mlHybridEnabled) {
        features = extractMlFeaturesCtx(ti, cpuId, scoreCtx);
        featuresValid = true;
    }

    // ML cold start protection — per-pool readiness gate.
    int poolIdx = (int)getPoolForCpu(cpuId);
    bool poolColdStartBlocked = false;
    if (cfgSnap.mlColdStartProtection && poolIdx >= 0) {
        poolColdStartBlocked = !g_mlModel.isPoolReady(poolIdx);
    }

    fuseMlScore(cpuId, ti, heuristic, liveCpu, mlActive, mlAlpha, features,
                poolIdx, poolColdStartBlocked, finalScore, fusion);

    // P1-1 batch 3: audio thread migration penalty.
    {
        int audioPen = getAudioThreadMigrationPenalty(ti.tname.c_str());
        if (audioPen > 0 && cpuId != g_audioIsolatedCpu.load(std::memory_order_relaxed)) {
            finalScore -= (float)audioPen * 0.1f;
        }
    }

    // Hybrid learning fine-tuning (experimental, 20% weight)
    if (cfgSnap.mlHybridEnabled) {
        int hfPoolIdx = (int)getPoolForCpu(cpuId);
        if (hfPoolIdx >= 0) {
            if (!mlActive) {
                finalScore = normalizeHeuristicScore(heuristic);
            }
            // P1-4: proxy features matching hybrid tick() training.
            float hf[15] = {0};
            hf[0] = 1.0f;  // bias
            unsigned long long avgRunUsProxy = (ti.nrSwitches > 0) ?
                (unsigned long long)((double)ti.runTime / (double)ti.nrSwitches / 1000.0) : 0;
            hf[1] = std::min(1.0f, (float)avgRunUsProxy / 50000.0f);
            // R005c: f[2] = switches RATE/s (thread_scan derives ti.switchesPerSec
            // from nrSwitches_delta/elapsed), normalized by kSwitchesPerSecScale.
            // Was cumulative nrSwitches/1000 which saturated to 1 for old threads.
            hf[2] = std::min(1.0f, (float)ti.switchesPerSec / aatpe::hybrid_const::kSwitchesPerSecScale);
            hf[3] = std::min(1.0f, features.cpu_usage);
            // B3 task5: wire REAL training-contract features instead of hardcoded
            // 0.5/0.0. ThreadFeatures already carries priority/nice/thermal/
            // migration_count (extractMlFeaturesCtx). Training side consumes the
            // same raw values, so inference/training stay aligned (no new skew).
            hf[4] = std::min(1.0f, std::max(0.0f, features.priority));
            hf[5] = std::min(1.0f, std::max(0.0f, features.nice));
            hf[6] = features.is_game;
            hf[7] = std::min(1.0f, std::max(0.0f, features.thermal));
            // #F3-alignment: use the shared tier->feature mapping so inference
            // matches training. v4.1.2-hotfix: branch on the DISTINCT CLUSTER count
            // (compressed), NOT maxTier+1 — a 3-cluster sparse-tier box (D1100:
            // tiers 0/2/3) must stay on the historical 0.3/0.6/1.0 branch instead of
            // silently taking the 4-way linear branch.
            int numPools = (int)clustersSnapshot().size();   // B-002: lock-protected snapshot
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

    // P2-1: ADRC adjustment integrated PER-CANDIDATE so it actually re-ranks.
    // boost: latency-critical threads (UI/RENDER/GAME) get a bonus on BIG-core
    // candidates — a big core that was #2 in the raw fused ranking can now be
    // lifted to #1 (previously the boost was added only to the already-chosen
    // bestFused after the loop, so it could never flip the ranking).
    // clamp: non-critical threads (THROUGHPUT/BACKGROUND) get a mild penalty on
    // BIG-core candidates when the system has ADRC headroom, favoring little
    // cores (power saving). Previously this branch was dead code because
    // adrcActive was only set for S1-S3 while the clamp required role >= S4.
    //
    // P3-49: during SystemState::EMERGENCY (thermal emergency latched) ADRC is a
    // LOWER-policy feedforward and must NOT override the safety placement. In
    // particular the BIG-core boost would pull hot critical threads back onto the
    // very big cores we are trying to cool. Suppress both adjustments in emergency
    // so the pure temperature-aware heuristic drives the safety move. This only
    // changes EMERGENCY-state scoring; NORMAL/DEGRADED weights are untouched (no
    // scheduling-core-weight regression).
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

// computeCandidateScores(): the per-CPU scoring loop (was L326-553). Iterates
// the allowed CPU copy and, per candidate, runs scoreOneCandidate() then
// selectOptimalPlacement(). Loop-carried best state is passed by reference.
// g_mlMutex is held by the caller across this whole loop (mlLoopLock).
//
// P3-48: cpuset restriction is applied as a CANDIDATE-GENERATION step, not a
// post-hoc skip inside scoring. We first build the explicit list of CPUs the
// thread is actually allowed to use (allowedCpus ∩ allCpusCopy), then score
// ONLY that list. Previously the loop iterated allCpusCopy and called
// isCpuAllowed() per-iteration, so scoreOneCandidate() was still entered for
// disallowed CPUs only to be discarded — wasted ML/ADRC/heuristic work on CPUs
// that could never be chosen. The ordering is now strictly:
//   allowedCpus -> candidate generation -> scoring.
void computeCandidateScores(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                            const cpu_set_t& allowedCpus, const std::vector<int>& allCpusCopy,
                            bool mlActive, float mlAlpha, const ScoringContext& scoreCtx,
                            bool adrcBoostActive, float adrcBoostUrgency, bool adrcClampActive,
                            int& bestCpu, float& bestFused, float& secondFused,
                            int& bestHeuristic, ThreadFeatures& bestFeatures,
                            DecisionFusion& bestFusion) {
    // P3-48: generate the candidate set from the thread's allowed cpuset FIRST.
    // H13(c): also hard-filter by online cache — an offline CPU must never be
    // scored as a candidate, even if it is still in allCpusCopy (which is
    // rebuilt under the topology lock but may lag a 200ms hotplug window).
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

        // P0-1: Store features for best CPU whenever mlActive (not just alpha>0).
        if (finalScore > bestFused && featuresValid) {
            bestFeatures = features;
        }

        // 同评分平局打破机制 — least-utilized
        selectOptimalPlacement(cpuId, finalScore, heuristic, fusion, featuresValid, features,
                               bestCpu, bestFused, secondFused, bestHeuristic,
                               bestFusion, bestFeatures);
    }
}

// applyColdStartExploration(): post-loop exploration-exploitation (was L562-585).
// coldStart flag recomputed under g_mlMutex here (the hoisted loop lock has
// already been released). May override bestCpu and set d.reason="COLD_EXPLORE".
// The atomic exploreSeed is a function-local static (single instance), exactly
// as in the original.
void applyColdStartExploration(float mlAlpha, bool mlActive, int liveCpu,
                               const cpu_set_t& allowedCpus,
                               const std::vector<int>& allCpusCopy,
                               int& bestCpu, Decision& d) {
    // Cold-start exploration-exploitation balance.
    // During cold start (alpha<0.1 or total samples<100), 10% of decisions
    // randomly pick a target CPU to ensure sample diversity for ML learning.
    bool coldStart = (mlAlpha < 0.1f);
    if (!coldStart && mlActive) {
        std::lock_guard<std::mutex> mlLock2(g_mlMutex);
        coldStart = (g_mlModel.getTotalSamples() < 100);
    }
    if (coldStart && bestCpu >= 0 && allCpusCopy.size() > 1) {
        // P2-6: Use std::atomic<unsigned int> instead of plain static unsigned int
        // to avoid data race when decidePlacement is called from multiple threads.
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

// recomputeScoreForTarget(): P1-2 fix. After applyColdStartExploration() /
// applySmartPlacementFallback() may REPLACE bestCpu, the loop-carried scoring
// metadata (bestHeuristic / bestFeatures / bestFusion / bestFused) still
// describes the ORIGINAL bestCpu. That left Decision.targetCpu pointing at one
// CPU while Decision.score / Decision.mlFeatures argued for a different one.
//
// This helper re-runs scoreOneCandidate() for the NEW bestCpu so all metadata
// corresponds to the actual migration target. It also re-derives secondFused as
// the live CPU's fused score, so confidence = (newTarget - current) measures the
// real improvement margin instead of a stale runner-up from the original
// ranking. Because thresholdGate() and placementHysteresisGate() consume
// bestHeuristic / confidence AFTER this point, they automatically re-evaluate
// gain and the hysteresis band against the new target — no duplicate gate calls
// needed.
//
// g_mlMutex is held by the caller (scoreOneCandidate touches g_mlModel + the
// s_mlPredictCache which is guarded by g_mlMutex).
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
    // Re-derive metadata for the (possibly new) target.
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
    // Re-derive the runner-up against the live (current) CPU so the confidence
    // gap reflects "how much better is the new target than where the thread is".
    {
        int curHeuristic; float curFinal; DecisionFusion curFusion;
        ThreadFeatures curFeatures; bool curValid;
        scoreOneCandidate(ti, cfgSnap, liveCpu, scoreCtx, mlActive, mlAlpha,
                         adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                         liveCpu, curHeuristic, curFinal, curFusion, curFeatures, curValid);
        outSecondFused = curFinal;
    }
}

// thresholdGate(): migration cost / force-migrate / ML-mature confidence gate
// (was L626-700). Returns true when a KEEP decision was made (caller returns d);
// returns false to proceed to MIGRATE. currentCpu is liveCpu (preserved name).
//
// #35 : three-value scoring output. In addition to the legacy
// absolute score, we now track:
//   - relative gain = (bestHeuristic - currentScore) / max(currentScore, 1) * 100 (%)
//   - confidence    = fused gap (bestFused - secondFused), already in `confidence`
// Stats are accumulated via placementAvgRecordGain() and exposed as
// placement_avg_gain / placement_avg_confidence state files.
//
// #33 : minimum useful capacity. When a latency-sensitive thread
// (UI/RENDER/GAME) has high utilization, it must NOT be migrated to a pool whose
// capacity is below minUsefulCapacityRatio * maxCapacity. This prevents a
// 30%-utilization UI thread from being pinned to a little core (capacity ~240)
// when it needs ~307+ throughput to meet its frame deadline.
bool thresholdGate(const ThreadInfo& ti, const Config& cfgSnap, int liveCpu,
                   int bestCpu, int bestHeuristic, float confidence,
                   const ScoringContext& scoreCtx, Decision& d) {
    // Migration cost check
    int currentCpu = liveCpu;
    int thermalPressure = g_thermalMigrationPressure.load(std::memory_order_relaxed);
    int effectiveCostThreshold = cfgSnap.migrationCostThreshold;
    if (thermalPressure > 0) {
        effectiveCostThreshold += (cfgSnap.migrationCostThreshold * thermalPressure) / 100;
    }
    // P0-4: use the context-aware overload — reads pools/metrics/config from the
    // pre-snapshotted scoreCtx instead of re-locking g_topologyRwMutex /
    // g_cpuMetricsMutex / g_cfgMutex per candidate (was the legacy 3-arg
    // computeScore(currentCpu, ti, liveCpu) that re-took every lock).
    int currentScore = computeScore(currentCpu, ti, liveCpu, scoreCtx);
    // P0: Unified gain using RAW heuristic delta (bestHeuristic - currentScore),
    // producing gain=1~50 (normalized 0~1 delta *1000), while cold start used raw
    // heuristic delta=20~40. Threshold=6 was calibrated for raw deltas, so warm-start
    // gain was systematically too low, blocking ~40% of valid migrations.
    // Fused score is used for RANKING (selecting bestCpu), but gain (is it worth
    // migrating?) must use raw heuristic delta for consistent dimensionality.
    int gain = bestHeuristic - currentScore;

    // #35: record three-value stats for diagnostics (absolute, relative gain, confidence)
    {
        float relGainPct = currentScore > 0
            ? (float)gain * 100.0f / (float)currentScore
            : 0.0f;
        int conf0_100 = (int)(confidence * 100.0f);  // scale fused gap to 0-100
        if (conf0_100 < 0) conf0_100 = 0;
        if (conf0_100 > 100) conf0_100 = 100;
        placementAvgRecordGain(relGainPct, conf0_100);
    }

    // #33: minimum useful capacity guard.
    // For latency-sensitive threads (UI/RENDER/GAME), if the current CPU is already
    // on a pool with adequate capacity and the target pool's capacity is below
    // minUsefulCapacityRatio * maxCapacity, block the migration (do not downgrade
    // capacity for a thread that needs throughput).
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
            // Fallback to topology defaults if metrics not populated yet
            if (maxCap <= 0) maxCap = aatpe::topology::kMaxCapacityRef;
            if (bestCap <= 0) bestCap = aatpe::topology::kDefaultCapacity;
            if (curCap <= 0) curCap = aatpe::topology::kDefaultCapacity;

            int minUsefulCap = (int)((float)maxCap * cfgSnap.minUsefulCapacityRatio);
            // Only block if we are moving DOWN in capacity to an insufficient pool.
            // (Moving up in capacity is always allowed.)
            if (bestCap < minUsefulCap && curCap >= minUsefulCap) {
                // Estimate thread utilization from runTime / swicthes (rough proxy).
                // If the thread is actively running (runTime > 0), it likely needs
                // the capacity — block the downgrade.
                bool threadActive = (ti.runTime > 0);
                if (threadActive) {
                    d.action = DecisionAction::KEEP;
                    d.reason = "INSUFFICIENT_CAP:" + std::to_string(bestCap) +
                               "<" + std::to_string(minUsefulCap);
                    g_keepLowConf++;  // reuse existing counter bucket (diagnostics)
                    return true;
                }
            }
        }
    }

    // ROUND-012: front-app no-downpin gate. Uses real per-thread lastSwitchStampNs
    // (not cumulative nrSwitches, which misclassifies young active threads).
    {
        std::string fg = getCachedForegroundApp();
        bool fgKnown = !fg.empty() && fg != "unknown";
        int curPool = (int)getPoolForCpu(currentCpu);
        int tgtPool = (int)getPoolForCpu(bestCpu);
        bool downPin = (tgtPool < curPool);
        if (downPin) {
            // quiet = no sched_switch for >=5s. If timestamp unavailable, treat ACTIVE.
            uint64_t nowNs = (uint64_t)aatpe_spillover::spillNowMs() * 1000000ULL;
            bool quiet = (ti.lastSwitchStampNs > 0) &&
                         (nowNs - ti.lastSwitchStampNs >= 5000ULL*1000000ULL);
            bool targetIsFg = (fgKnown && ti.pname == fg);
            // fgKnown: block all non-S5 downpins. fgUnknown: block active non-S5.
            // PLACE-09: the two branches had identical RHS; collapse to one expression.
            // fgKnown && !targetIsFg (a background app) -> block stays false (downpin allowed).
            bool block = (targetIsFg || !fgKnown) &&
                         !(ti.role == ROLE_S5_BACKGROUND && quiet);
            if (block) {
                d.action = DecisionAction::KEEP;
                d.reason = "FG_NO_DOWNPIN";
                aatpe_spillover::g_fgDownpinBlocked.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            // A6: the fg/container thread IS being down-pinned (only allowed when
            // classified S5_BACKGROUND && quiet>=5s). Diagnostic — never changes
            // control flow. On a VMOS device the foreground is com.vmos.pro; if its
            // RenderThread/compositor is ever misclassified as S5 and pushed to
            // LITTLE (0-3) this log names tid/comm/pkg/from->to pool so we can
            // attribute the W1-EVID-09 sample (container at 0-3). 1/64-rate-limited.
            if (targetIsFg && (aatpe_spillover::g_downpinDiagCount++ & 63) == 0) {
                LOG_WARN("A6 downpin fg tid=%d comm=%s pkg=%s pool %d->%d role=%d quiet=%d",
                         ti.tid, ti.tname.c_str(), ti.pname.c_str(),
                         curPool, tgtPool, (int)ti.role, (int)quiet);
            }
        }
    }

    // 高优先级线程跨池迁移粘性修复
    // 问题: 已在小核的高优先级线程因cache(+15)+迁移惩罚(+30)=+45粘性,
    //       不会主动迁移到大核, 导致游戏时大核闲置。
    // 修复: 对UI/Render/Game高优先级线程, 当目标池性能高于当前池时:
    //   1) 降低迁移成本阈值(从默认3降到1), 减少跨池摩擦
    //   2) 添加性能增益强制迁移阈值: 当gain>=阈值时无视成本阈值强制迁移
    //   这样大核评分优势明显时高优先级线程能主动迁移, 同时保留小增益时的稳定性。
    // 池对感知的强制迁移阈值 — 原固定 15 未区分池对间的实际性能差距。
    //   天玑1100: LITTLE(A55)→MID(A78) 性能差距 58%, 应使用较低阈值(10)确保迁移;
    //   MID(A78)→BIG(A78) 同架构差距 ~0%, 使用更低阈值(8);
    //   LITTLE→BIG 保持 15 (最大跨度, 最高置信度)。
    bool isHighPriority = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER ||
                           ti.role == ROLE_S3_GAME);
    CpuPool curPool = getPoolForCpu(currentCpu);
    CpuPool bestPool = getPoolForCpu(bestCpu);
    bool targetHigherPool = false;
    if (bestPool == CpuPool::BIG && (curPool == CpuPool::MID || curPool == CpuPool::LITTLE))
        targetHigherPool = true;
    else if (bestPool == CpuPool::MID && curPool == CpuPool::LITTLE)
        targetHigherPool = true;
    // 池对感知的强制迁移阈值
    int perfGainForceThreshold = 15;  // default: LITTLE→BIG
    if (targetHigherPool) {
        if (curPool == CpuPool::LITTLE && bestPool == CpuPool::MID)
            perfGainForceThreshold = 10;  // A55→A78, 大跨度, 较低阈值确保迁移
        else if (curPool == CpuPool::MID && bestPool == CpuPool::BIG)
            // P-02: was 8. On Dimensity 1100 the MID and BIG clusters are the
            // SAME A78 microarchitecture, so an 8-point heuristic delta is well
            // within load-scoring noise and was force-migrating threads back and
            // forth between two near-identical cores. Raise to 15 to match the
            // LITTLE→BIG bar: only a genuinely large gain overrides the band.
            perfGainForceThreshold = 15;
        // LITTLE→BIG remains 15
    }
    bool forceMigrate = (isHighPriority && targetHigherPool && gain >= perfGainForceThreshold);
    // 高优先级线程跨池时降低迁移成本阈值(从默认3降到1)
    int hpEffectiveThreshold = effectiveCostThreshold;
    if (isHighPriority && targetHigherPool) {
        hpEffectiveThreshold = 1;  // 降低跨池摩擦
    }
    if (!forceMigrate && gain < hpEffectiveThreshold) {
        d.action = DecisionAction::KEEP; d.reason = "HIGH_COST:" + std::to_string(gain); g_keepHighCost++; return true;
    }
    // ML maturity-adaptive confidence gate.
    // Only enabled when ML is mature (alpha>0.1, total samples>100,
    // every pool >20 samples, V4+CRC32 model loaded).
    // During cold start / early warm start (immature), gate is SKIPPED to
    // allow migrations and ML sample accumulation.
    // Dimensionality fix: confidencePercent = confidence * 1000 (not *100).
    // Under normalized 0~1 fused scores, confidence=best-second is 0.001~0.05,
    // so confidencePercent=1~50. Threshold=8 only blocks "negligible difference"
    // migrations, not all migrations (which was the fix57/fix58 root cause).
    // P2-3: isMature() reads g_mlModel internal state, must hold g_mlMutex.
    bool mlMature = false;
    {
        std::lock_guard<std::mutex> mlLock(g_mlMutex);
        mlMature = g_mlModel.isMature();
    }
    if (mlMature) {
        float confidencePercent = confidence * 1000.0f;  // *1000 dimensionality
        if (cfgSnap.confidenceThreshold > 0 && confidencePercent < (float)cfgSnap.confidenceThreshold) {
            d.action = DecisionAction::KEEP;
            d.reason = "LOW_CONF:" + std::to_string((int)confidencePercent);
            g_keepLowConf++;
            return true;
        }
    }
    return false;
}

// ============================================================================
// #34 placement hysteresis + per-thread cooldown 
// ----------------------------------------------------------------------------
// WHY: On Dimensity 1100 the two A78 mid cores (cpu4, cpu5) have near-identical
// capacity and their fused scores fluctuate by 1-3% with load. Without a band,
// a game worker ping-ponged cpu4 <-> cpu5 every 200ms decision cycle. Each hop
// pays a real cache-cold + runqueue-shift penalty and produces visible jitter.
//
// Model: once a thread is migrated (or first observed) onto a CPU we anchor a
// "settled point": (settledCpu, settledScore, settledMs). A NEW candidate only
// wins when BOTH hold:
//   (a) min-dwell: now - settledMs >= MIGRATION_COOLDOWN_MS (default 500ms);
//   (b) band:      bestHeuristic >= settledScore * (1 + HYSTERESIS_PERCENT/100)
// The band is relative to the settled score, not the live current score, so a
// temporary dip on the settled CPU does not by itself pull the thread away.
// Force-migrate paths (high-priority targetHigherPool, thermal emergency) bypass
// this gate — those are deliberate corrections, not noise.
// ============================================================================
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
    if (bestCpu == currentCpu) return false;  // caller's ALREADY_OPTIMAL path
    const int pct = (cfg.migrationHysteresisPercent < 0) ? 0
                  : (cfg.migrationHysteresisPercent > 100 ? 100
                   : cfg.migrationHysteresisPercent);
    const long long cooldownMs = (cfg.migrationCooldownMs < 0) ? 0 : cfg.migrationCooldownMs;
    const long long nowMs = settleNowMs();

    std::lock_guard<std::mutex> lk(g_settleMutex);
    auto it = g_threadSettle.find(tid);
    if (it == g_threadSettle.end()) {
        // First observation: anchor to where the thread currently is. Do NOT
        // reject — let the first legitimate migration happen; from then on the
        // band applies.
        ThreadSettleState st;
        st.settledCpu = currentCpu;
        st.settledScore = currentHeuristic;
        st.settledMs = nowMs;
        g_threadSettle[tid] = st;
        return false;
    }
    ThreadSettleState& st = it->second;

    // If the thread is NOT on its settled CPU anymore (the kernel moved it, or
    // an external component repinned it), re-anchor to the live CPU rather than
    // acting on stale state.
    if (st.settledCpu != currentCpu) {
        // R3-007: applyPlacement pins a whole POOL mask, not a single CPU — the
        // kernel is free to run the thread on any CPU inside that pool. A live
        // currentCpu that merely differs from the anchored CPU but stays INSIDE the
        // same pool must NOT count as a re-anchor (that previously wiped the
        // hysteresis band on every in-pool scheduler choice). Only re-anchor when
        // the thread actually crossed into a DIFFERENT pool.
        if (getPoolForCpu(st.settledCpu) != getPoolForCpu(currentCpu)) {
            st.settledCpu = currentCpu;
            st.settledScore = currentHeuristic;
            st.settledMs = nowMs;
        }
        return false;
    }

    // (a) min-dwell cooldown.
    if (cooldownMs > 0 && (nowMs - st.settledMs) < cooldownMs) {
        d.action = DecisionAction::KEEP;
        d.reason = "SETTLE_COOLDOWN:" + std::to_string(cooldownMs - (nowMs - st.settledMs)) + "ms";
        g_settleCooldownRejects.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // (b) hysteresis band. settledScore is the raw heuristic score of the
    // settled CPU at anchor time. bestHeuristic must beat it by pct%.
    if (pct > 0) {
        long long required = (long long)st.settledScore +
                             ((long long)st.settledScore * pct) / 100;
        // Guard against negative/zero settledScore (uninformative anchor):
        // fall back to requiring a fixed 2-point absolute delta so the band
        // does not become "anything beats 0".
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
    // Opportunistic size cap: g_threadSettle is keyed by tid and grows only when
    // a new thread appears; bound it so long uptimes cannot grow it unbounded.
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
    // P0-5 fix: caller passes a pre-snapshotted Config& (e.g. loopCfg taken once
    // per main-loop iteration). This eliminates the per-call snapshotConfig()
    // (g_cfgMutex + deep-copy of 5 std::string members) that fired 3090 times/sec
    // on the hot decision path. Callers outside the main loop (emergencyMigrate,
    // lightweightMigrate, proactiveMigration) snapshot Config once at function
    // entry and pass it in.

    // Hard safety gates ALWAYS apply, even for emergency migration: we must never
    // pin protected/system threads or touch kernel threads.
    if (ti.protect || ti.role == ROLE_S0_SYSTEM) {
        d.action = DecisionAction::KEEP; d.reason = "PROTECTED"; g_keepProtected++; return d;
    }
    // defense-in-depth — skip kernel threads even if they slip past scan
    if (isKernelThread(ti.tname, ti.tid)) {
        d.isKernel = true;  // P2: record verdict so applyPlacement/WebUI skip re-probe
        d.action = DecisionAction::KEEP; d.reason = "KERNEL_THREAD"; return d;
    }
    // skip permanently unmigratable threads (errno=22 or 2+ failures)
    {
        std::lock_guard<std::mutex> lock(g_unmigratableMutex);
        if (g_unmigratableThreads.count(ti.tid)) {
            d.action = DecisionAction::KEEP; d.reason = "UNMIGRATABLE"; return d;
        }
    }

    // Policy gates. Ordinary periodic migrations honor them; EVENT-TRIGGERED
    // emergency migrations (THERMAL_SPIKE/JANK/FPS_DROP) BYPASS them. Rationale:
    // when the system is in thermal emergency / safe mode we STILL need to be
    // able to move hot critical threads to cooler (little) cores — otherwise the
    // very state that should cool the system freezes all migration.
    //
    // P3-49 — explicit priority ladder (highest first); a lower policy must never
    // mask a higher safety state:
    //   Emergency thermal > Safety > cpuset > control mode > foreground policy
    //   > ADRC > ML > heuristic > exploration.
    // The checks below are therefore ordered thermal/safety FIRST and control mode
    // (passive) LAST, so a latched thermal emergency is never reported/short-
    // circuited as a mere "passive" or "safe mode" decision.
    if (!emergency) {
        // (1) Emergency thermal — highest. Hard-stop ordinary ML/heuristic migration.
        if (g_thermalEmergency.load(std::memory_order_relaxed)) {
            d.action = DecisionAction::KEEP; d.reason = "THERMAL_EMERGENCY"; g_keepThermalEmergency++; return d;
        }
        // (2) Safety-latency (multi-ms frame stall).
        if (g_safetyLatencyActive.load(std::memory_order_relaxed)) {
            d.action = DecisionAction::KEEP; d.reason = "SAFETY_LATENCY"; g_keepSafetyLatency++; return d;
        }
        // (3) Safe Mode — suspend ML/aggressive periodic migration.
        if (aatpe::subsys::safemode::isActive()) {
            d.action = DecisionAction::KEEP; d.reason = "SAFE_MODE"; g_keepSafeMode++; return d;
        }
        // (4) Control mode (passive) — lowest of the latched gates.
        if (cfg.controlMode == "passive") {
            d.action = DecisionAction::KEEP; d.reason = "PASSIVE"; g_keepPassive++; return d;
        }
    }

    // ML fusion - get alpha (model influence weight)
    // Cold start: alpha=0 when samples < 100, so behavior matches fix20 exactly
    float mlAlpha = 0.0f;
    bool mlActive = false;
    {
        std::lock_guard<std::mutex> lock(g_mlMutex);
        mlActive = g_mlModel.isEnabled() && cfg.mlLearningEnabled;
        if (mlActive) mlAlpha = g_mlModel.getAlpha();
    }
    // #38: ML anomaly drift — if the closed loop detected sustained degradation
    // despite ML-recommended placements, enter safe mode: drop ML influence to
    // zero and let pure heuristic scheduling take over. The DriftDetector
    // self-heals (a genuine improvement resets the latch), so this is not
    // permanent.
    if (mlActive && aatpe_ml::g_mlDriftDetected.load(std::memory_order_relaxed)) {
        mlAlpha = 0.0f;
    }

    int bestCpu = -1;
    float bestFused = -std::numeric_limits<float>::max();
    float secondFused = -std::numeric_limits<float>::max();
    int bestHeuristic = 0;
    // H5: value-initialize instead of memset. ThreadFeatures is a pure aggregate
    // of floats (aatpe_ml.h:75) — ={} zero-initializes identically, no ctor to skip.
    ThreadFeatures bestFeatures{};
    DecisionFusion bestFusion;  // track best fusion for explainability + accumulator

    // fix37/47: cpuset-aware scheduling — respect system task profile/cpuset as hard constraint
    cpu_set_t allowedCpus = getThreadAllowedCpus(ti.tid);
    // P2: if the lookup failed/returned an empty mask, explicitly zero it so
    // downstream isCpuAllowed() sees a deterministic empty set rather than
    // relying on whatever the by-value return left on the stack.
    if (CPU_COUNT(&allowedCpus) == 0) CPU_ZERO(&allowedCpus);

    // P2-4: Copy g_allCpus under shared_lock to avoid concurrent modification
    // by refreshCpuTopology() during the scoring loop. Iterating a copy is
    // safe and avoids holding the lock during expensive computeScore calls.
    std::vector<int> allCpusCopy;
    {
        std::lock_guard<std::mutex> rlock(g_topologyRwMutex);
        allCpusCopy = g_allCpus;
    }

    // P2 Problem6 (sched_switch thread-identity pollution): read the LIVE
    // current CPU once at decision entry. `ti.currentCpu` is a snapshot copied
    // from g_threads by collectCriticalThreads(); although onSwitch now keeps
    // g_threads[nextTid].currentCpu fresh on every sched_switch (v94 breakpoint
    // 4), the snapshot can still race with an in-flight switch between the copy
    // and this scoring loop. Previously the loop used the snapshot (ti.currentCpu)
    // for migration-gain `fromPool`, the current_cpu_match feature hf[9], and
    // the cold-start explore exclusion, while the ALREADY_OPTIMAL check below
    // re-read the live CPU — so a thread that had just moved could be scored as
    // "migrating from" a CPU it already left. Reading once here and reusing the
    // same value for fromPool / hf[9] / explore / ALREADY_OPTIMAL keeps the whole
    // decision consistent against a single, real-time observation of fromCpu.
    int liveCpu = readThreadCurrentCpu(ti.pid, ti.tid);
    if (liveCpu < 0) liveCpu = ti.currentCpu;  // fallback to fresh g_threads snapshot

    // Hoist g_mlMutex acquisition OUT of the per-CPU loop.
    // Previously the lock was taken/released for EVERY cpuId candidate (8-16
    // lock/unlock pairs per decision). Now we take it once before the loop (when
    // mlActive) and release once after. This preserves the existing BatchLock
    // semantics and the lock order (g_cfg -> g_ml -> topology -> cpuMetrics).
    std::unique_lock<std::mutex> mlLoopLock;
    if (mlActive) {
        mlLoopLock = std::unique_lock<std::mutex>(g_mlMutex);
    }

    // P0-opt1: Build ScoringContext ONCE — snapshot pools + cpuMetrics under their
    // respective locks, then release. The per-CPU scoring loop reads from this
    // snapshot instead of re-locking g_topologyRwMutex / g_cpuMetricsMutex per
    // candidate (was 3+2 = 5 lock/unlock pairs per CPU × 8 CPUs = 40 pairs).
    ScoringContext scoreCtx = buildScoringContext(cfg);

    // ========================================================================
    // ADRC Integration: feedforward prediction.
    // P2-1: boost/clamp signals are now resolved BEFORE the candidate loop and
    // applied PER-CANDIDATE inside scoreOneCandidate(), so they genuinely re-rank
    // the CPUs (previously the boost was added only to the already-chosen
    // bestFused after the loop and could never lift a #2 big core to #1).
    //   boost -> UI/RENDER/GAME (S1-S3): +urgency*0.15 on BIG-core candidates.
    //   clamp -> THROUGHPUT/BACKGROUND (S4/S5): -0.05 on BIG-core candidates
    //            when the system has ADRC headroom (allCanClamp()). This was
    //            previously dead code because adrcActive was only set for S1-S3
    //            while the clamp required role >= S4.
    // riskScore is still applied post-selection as a confidence-gap tweak below.
    // ========================================================================
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
        // Non-critical threads: clamp toward little cores only when the system
        // has genuine ADRC headroom (power saving).
        adrcClampActive = allCanClamp();
    }

    computeCandidateScores(ti, cfg, liveCpu, allowedCpus, allCpusCopy,
                          mlActive, mlAlpha, scoreCtx,
                          adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                          bestCpu, bestFused, secondFused, bestHeuristic,
                          bestFeatures, bestFusion);

    // Snapshot the loop-chosen target so we can detect whether cold-start
    // exploration / smart fallback actually moved it (P1-2).
    int loopBestCpu = bestCpu;

    // release the hoisted g_mlMutex. The post-loop cold-start
    // block below re-acquires g_mlMutex itself; std::mutex is non-recursive, so
    // we must release here to avoid self-deadlock.
    if (mlLoopLock.owns_lock()) {
        mlLoopLock.unlock();
    }

    applyColdStartExploration(mlAlpha, mlActive, liveCpu, allowedCpus,
                              allCpusCopy, bestCpu, d);

    if (bestCpu < 0) {
        d.action = DecisionAction::KEEP; d.reason = "NO_CPU"; g_keepNoCpu++; return d;
    }

    // 智能绑核：过载/过温降级微调。
    // 大核过载(load>80%)或过温(temp>=CRITICAL墙)时，UI/RENDER/GAME 退中核；
    // BACKGROUND 在大核不空闲时退小核。bestCpu<0 / SYSTEM 线程已在函数内处理。
    // pass allowedCpus so fallback picks only cpuset-allowed CPUs.
    bestCpu = applySmartPlacementFallback(bestCpu, ti, cfg, allowedCpus);

    // P1-2: if exploration/fallback moved the target, re-derive the scoring
    // metadata (heuristic / features / fusion / fused score / secondFused) for
    // the NEW target so Decision.score and Decision.mlFeatures describe the CPU
    // we actually migrate to. Held under g_mlMutex because scoreOneCandidate
    // touches g_mlModel + s_mlPredictCache. When the target did NOT move, the
    // loop's metadata is kept untouched to preserve existing behavior.
    if (bestCpu != loopBestCpu) {
        std::lock_guard<std::mutex> mlRecomputeLock(g_mlMutex);
        recomputeScoreForTarget(ti, cfg, liveCpu, scoreCtx,
                                mlActive, mlAlpha,
                                adrcBoostActive, adrcBoostUrgency, adrcClampActive,
                                bestCpu,
                                bestHeuristic, bestFused, secondFused,
                                bestFeatures, bestFusion);
    }

    // ADRC risk score as an additional decision dimension (confidence tweak).
    // High risk threads get a confidence boost (more willing to migrate).
    // Applied after the P1-2 recompute so it adjusts the final secondFused.
    if (adrcActive && adrcOut.riskScore > 0.5) {
        secondFused -= (float)adrcOut.riskScore * 0.02f;
    }

    float confidence = bestFused - secondFused;

    // Read current CPU and check ALREADY_OPTIMAL BEFORE confidence.
    // ROOT CAUSE (fix35): confidence = bestFused - secondFused (gap between
    // #1 and #2 CPU). Under extreme load (system_load=493%) all CPUs score
    // nearly identically, so best-second < confidenceThreshold(5) for 99.8%
    // of decisions. LOW_CONF then blocked migration EVEN WHEN bestCpu was
    // a different (better) CPU than currentCpu.
    // Correct semantics:
    //   - current == best  -> already optimal, KEEP
    //   - current != best  -> confidence (best-vs-second) is irrelevant;
    //                        best is at least not worse than current, so
    //                        proceed to migration-cost check. The gain
    //                        (best - current) filter already catches
    //                        negligible improvements.
    // P2 Problem6: liveCpu was already read at decision entry above. Reuse it
    // here so the ALREADY_OPTIMAL comparison, migration-gain fromPool, and hf[9]
    // all agree on the same fromCpu observation — no second /proc/<tid>/stat read.
    int currentCpu = liveCpu;
    if (currentCpu == bestCpu) {
        d.action = DecisionAction::KEEP; d.reason = "ALREADY_OPTIMAL"; g_keepAlreadyOptimal++; return d;
    }

    // confidence gate removed for best!=current path.
    // confidence is still computed and logged in the MIGRATE decision for
    // diagnostics, but no longer blocks moving away from a suboptimal CPU.
    // P0: PERMANENTLY remove confidence hard-gate. fix55 re-introduced
    // it (confidencePercent = confidence*100, threshold=8), but under
    // normalized 0~1 fused scores, confidence=best-second is typically
    // 0.001~0.05 -> confidencePercent=0.1~5, ALWAYS < 8. This blocked ~95%
    // of warm-start migrations, causing migration to stop shortly after cold
    // start exited. Gain check (best-current >= cost_threshold) already filters
    // negligible improvements; confidence (best-vs-second) is irrelevant when
    // current != best.
    // Ordinary migrations must clear the gain/cost gate AND the settle band.
    // EVENT-TRIGGERED emergency migrations bypass both: they are deliberate
    // corrections for thermal/jank events and must not be throttled by the
    // noise-suppression band.
    if (!emergency) {
    if (thresholdGate(ti, cfg, liveCpu, bestCpu, bestHeuristic, confidence, scoreCtx, d)) {
        return d;
    }

    // #34 placement hysteresis + min-dwell cooldown.
    // Runs AFTER the basic gain/cost gate so only migrations that already clear
    // the cost threshold are considered. Deliberate corrections (high-priority
    // UI/RENDER/GAME threads climbing to a higher pool — e.g. A55->A78) bypass
    // the band: those are not noise, they are exactly the migrations we want fast.
    // #30: a LOW-confidence thread (classifier uncertain of its role, e.g. a
    // vendor binder that landed in a game bucket) must NOT get that bypass — we
    // do not aggressively move a thread whose role we are not sure about.
    // P-02 symmetric damping (根因分析 P-02): previously, UI/RENDER/GAME threads
    // climbing to a higher pool SKIPPED the hysteresis band + 500ms cooldown
    // entirely (the `if (!(hp && toHigherPool) || lowConf)` below), while the
    // downhill direction kept the full band. That asymmetric loop gain let a
    // thread trivially climb onto BIG (gain>=1), BIG immediately overloaded, the
    // overload fallback pulled it back down, and the next cycle climbed again — a
    // classic bang-bang limit cycle that kept the big core permanently in a
    // cold-cache transition state. Now UPHILL goes through the SAME 5% band +
    // 500ms cooldown as downhill. The forceMigrate path in thresholdGate still
    // lets a genuinely large gain through quickly; this gate only removes the
    // noise-driven micro-fluctuation climbs. (The post-uphill 2s no-re-placement
    // window in the unified residency latch adds the second half of the cure.)
    {
        int currentHeuristic = computeScore(liveCpu, ti, liveCpu, scoreCtx);
        if (placementHysteresisGate(ti.tid, liveCpu, bestCpu,
                                    currentHeuristic, bestHeuristic,
                                    cfg, d)) {
            return d;
        }
    }
    }  // !emergency

    // Record pending decision for online learning feedback loop
    // fix45 Problem 2 & 10: pass before_freq and before_timestamp for V2 outcome
    // P0-1: DO NOT record pending here. recordPendingDecision is now called
    // ONLY after applyPlacement returns true (actual sched_setaffinity success).
    // This breaks the closed-loop pollution: previously pending was recorded before
    // applyPlacement, and 8 skip/failure paths left stale pending that polluted ML.
    // We carry mlFeatures and mlActive in the Decision so the caller can record
    // pending only after confirmed success.
    // P0-D: Cold-start all-zero feature filtering.
    // During cold start (before CPU metrics populated), bestFeatures may be all-zero.
    // Recording these as pending would pollute ML training with invalid samples.
    // Set mlActive=false to skip pending recording for this decision.
    if (mlActive && LearningModel::isAllZeroFeatures(bestFeatures)) {
        mlActive = false;
    }
    d.mlFeatures = bestFeatures;
    d.mlActive = mlActive;
    d.targetPool = getPoolForCpu(bestCpu);
    d.action = DecisionAction::MIGRATE;
    d.targetCpu = bestCpu;
    d.score = bestHeuristic;
    // PLACE-11: clamp confidence to [0,1000]. ADRC risk-score adjustment can push
    // the fused delta below zero; a negative confidence would surface in WebUI
    // and is meaningless as a display value.
    {
        int c = (int)(confidence * 1000);
        if (c < 0) c = 0;
        if (c > 1000) c = 1000;
        d.confidence = c;
    }
    d.reason = "Score:" + std::to_string(bestHeuristic) + " MLAlpha:" + std::to_string(mlAlpha)
             + " Pool:" + getPoolName(d.targetPool);
    // ADRC: append feedforward signals to decision reason for debugging
    if (adrcActive) {
        d.reason += " ADRC:u=" + std::to_string((int)(adrcOut.urgency * 100))
                  + "% r=" + std::to_string((int)(adrcOut.riskScore * 100))
                  + (adrcOut.shouldBoost ? " B" : "")
                  + (adrcOut.shouldClamp ? " C" : "");
    }
    // carry fusion result for explainability + accumulator
    d.fusion = bestFusion;
    // P2: record() is called HERE, at decision time, BEFORE applyPlacement()
    // runs. So the accumulator averages DECISION-TIME weights (what the fusion
    // chose for this candidate), not post-hoc SUCCESS weights (a migration may
    // later be rate-limited, rejected by cpuset, or fail sched_setaffinity).
    // This is intentional: it measures how the fusion weights are distributed
    // across decisions, not how often migrations actually succeed.
    g_fusionWeightAccum.record(bestFusion);
    g_migrateActual++;
    return d;
}

// ============================================================================

void protectCriticalThreads() {
    // P2: Safety gate — skip if control mode is passive (daemon should not
    // modify any thread affinity in passive mode). Also skip if thermal
    // emergency is active (let thermal governor handle critical threads).
    // snapshot config for the passive gate.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    if (cfgSnap.controlMode == "passive") return;
    if (g_thermalEmergency.load()) return;
    // P3: snapshot g_allCpus under topology lock — iterating it lock-free races
    // a concurrent hotplug rebuild.
    std::vector<int> allCpusSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        allCpusSnap = g_allCpus;
    }
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    for (auto&_sb_item : g_threads) {
        auto& tid = _sb_item.first; auto& ti = _sb_item.second;
        if (ti.role != ROLE_S0_SYSTEM) continue;
        // P2: Safety gate — never touch kernel threads (kthreadd children)
        if (isKthreaddChild(tid)) continue;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (int cpu : allCpusSnap) CPU_SET(cpu, &mask);
        // D-17: Check return value, log failures at DEBUG level
        int pret = sched_setaffinity(tid, sizeof(mask), &mask);
        if (pret != 0) {
            int e = errno; char ebuf[128];
            LOG_DEBUG("protectCriticalThreads: sched_setaffinity failed for tid=%d errno=%d (%s)",
                      tid, e, aatpeStrError(e, ebuf, sizeof(ebuf)));
            (void)e; (void)ebuf;  // LOG_DEBUG is a no-op in release
        }
    }
}

// P2: Kernel thread PPID detection (real original)
// Reads /proc/<tid>/stat to get PPID (field 4). Returns true if PPID == 2
// (kthreadd), meaning this is a kernel thread spawned by kthreadd.
// More reliable than name matching for kernel threads with unusual names.
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
        return (ppid == 2); // kthreadd PID
    } catch (...) {
        static int n = 0; if ((++n & 0x3F) == 1)
            LOG_DEBUG("isKernelThread: ppid field='%s' unparsable (throttled)", ppidStr.c_str());
        return false;
    }
}

// ============================================================================
// THREAD SCANNER (fix: priority from rules, ignore system threads config)
// ============================================================================
// P0-1: Find the PID of a process by its package/cmdline name.
// Kernel thread detection.
// Kernel threads (migration/, rcu_*, ksoftirqd/, kworker/, kthreadd, init, etc.)
// cannot be migrated via sched_setaffinity — the call either fails (EPERM) or
// is immediately reverted by the kernel. Attempting to migrate them wastes CPU
// and inflates apply_failed counter (46.7% failure rate in fix34 diagnostics).
// Only explicit name-pattern matching is used to avoid false-positives on
// user-space threads that may have similar names or low TIDs.
// Enhanced kernel thread detection — triple detection.
// fix35 original: only name-prefix matching. TID 51-72 kernel threads slipped past.
// Detection layers (any one match → kernel thread):
//   1. Thread name prefix/exact match (existing, comprehensive list)
//   2. TID < 1000 AND /proc/<tid>/cmdline is empty (low-TID kernel threads)
//   3. /proc/<tid>/cmdline is empty (most reliable — kernel threads have no cmdline)
// Note: init (TID=1) is user-space and has non-empty cmdline, so it won't be
// falsely matched by layers 2/3. User-space threads always have ≥1 byte in cmdline.
bool isKernelThread(const std::string& tname, int tid) {
    // P3-5: Refactored from 60+ if/else to static arrays + loop matching.
    // Logic is 100% identical: exact-match list + prefix-match list + cmdline fallback.
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

    // --- Layer 1: Name-based matching (fast, no I/O) ---
    if (!tname.empty()) {
        for (const char* name : kExactKernelThreads) {
            if (tname == name) return true;
        }
        for (const char* prefix : kKernelThreadPrefixes) {
            size_t plen = strlen(prefix);
            if (tname.size() >= plen && tname.compare(0, plen, prefix) == 0) return true;
        }
    }

    // --- Layer 2 & 3: cmdline-based detection (requires procfs read) ---
    // Kernel threads have an empty /proc/<tid>/cmdline (0 bytes).
    // User-space threads always have at least the process name (≥1 byte).
    // init (TID=1) has "/init" in cmdline, so it is NOT falsely matched.
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
// Used to prioritize foreground app threads in scanThreads().
// Searches /proc/<pid>/cmdline for an exact match with the package name.
int findPidByPackage(const std::string& pkgName) {
    if (pkgName.empty() || pkgName == "unknown") return -1;
    DIR* procDir = opendir("/proc");
    if (!procDir) return -1;
    struct dirent* entry;
    int foundPid = -1;
    while ((entry = readdir(procDir)) != nullptr) {
        // P3-1: strtol with validation instead of atoi
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
// P0-1: Scan all threads of a specific PID into localThreads.
// Returns the number of threads added. Used to prioritize foreground app.

// ============================================================================
// P-01/P-03: unified per-tid affinity-change residency latch.
//
// ROOT CAUSE (根因分析_调度稳定性.md P-01/P-03): four independent migration
// paths — the 200ms main loop, the 16ms frame-sync lightweight migrator, the
// 200ms proactive predictor, and the event-triggered emergency migrator — could
// move the SAME tid at millisecond spacing. They only shared the coarse,
// seconds-granularity g_lastMigrationTime (5s dwell), AND the emergency path
// bypassed it (plus the rate guard and the hysteresis gate) entirely. That is the
// "migrate -> cache cold -> jank -> emergency re-migrate -> cache cold" 1-2s
// self-sustaining ping-pong, and the reason the module ran hotter / less stably
// than baseline in the A/B test.
//
// FIX: every sched_setaffinity funnels through applyPlacement(), so a single
// ms-granularity latch enforced there covers ALL four paths. No path can move a
// tid that was already moved within:
//   - kNormalCrossPathResidencyMs  (normal paths, 1000ms)
//   - kEmergencyResidencyMs         (emergency path, 1500ms — still allowed to
//                                   react fast, but cannot re-migrate the same
//                                   tid on a ~1s loop)
// Additionally, an UPHILL promotion (thread moved to a higher pool) triggers a
// kPostUphillResidencyMs (2000ms) no-re-placement window for NORMAL paths — this
// is the P-02 antidote to the big-core bang-bang (climb BIG -> overload -> pulled
// down -> climb again). Emergency thermal moves are exempt from the post-uphill
// hold (a genuinely hot thread must still be pullable to a cooler core), but they
// remain bound by the kEmergencyResidencyMs residency above.
// ============================================================================
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

// Pure predicate: does the residency latch PERMIT a migration on tid right now?
// Shared by applyPlacement() and the host regression seam. Caller must hold
// g_migLatchMutex. Returns true (allowed) when there is no prior migration, or
// when the elapsed time since the last affinity write clears the relevant window.
static bool migLatchQueryLocked(int tid, long long nowMs, bool emergency) {
    auto it = g_migLatch.find(tid);
    if (it == g_migLatch.end()) return true;  // first time: nothing to hold
    long long elapsed = nowMs - it->second.lastChangeMs;
    // Post-uphill hold blocks NORMAL re-placement for kPostUphillResidencyMs.
    // Emergency (thermal pull-down) is exempt from this specific hold.
    if (it->second.lastWasUphill && !emergency && elapsed < kPostUphillResidencyMs) {
        return false;
    }
    long long minGap = emergency ? kEmergencyResidencyMs : kNormalCrossPathResidencyMs;
    return elapsed >= minGap;
}

// Stamp the latch after a successful affinity write. `uphill` marks a promotion
// to a higher pool so the post-uphill no-re-placement window engages.
static void migLatchStampLocked(int tid, long long nowMs, bool uphill) {
    g_migLatch[tid] = MigLatchEntry{nowMs, uphill};
    // Opportunistic size bound (long uptime): evict the oldest entry.
    if (g_migLatch.size() > 1024) {
        auto oldest = g_migLatch.begin();
        for (auto it = g_migLatch.begin(); it != g_migLatch.end(); ++it) {
            if (it->second.lastChangeMs < oldest->second.lastChangeMs) oldest = it;
        }
        g_migLatch.erase(oldest);
    }
}

// returns true if sched_setaffinity succeeded (only then should pending be recorded)
// `emergency=true` for event-triggered migrations (THERMAL_SPIKE /
// JANK / FPS_DROP). Such migrations BYPASS the passive / thermal-emergency /
// safety-latches — when the system is overheating or janking we MUST still
// be able to move critical threads to cooler/faster cores. They now also honor
// the unified per-tid residency latch (P-01/P-03) on top of the hard safety checks.
bool applyPlacement(const ThreadInfo& ti, int targetCpu, const std::string& reason, const Config& cfg, bool emergency) {
    // P0-1: caller passes a pre-snapshotted Config& (loopCfg in the main loop;
    // cfgSnap once per emergency/lightweight/proactive call). The per-migration
    // snapshotConfig() deep-copy (g_cfgMutex + 5 std::string members) that fired
    // here on every migration is gone. Use `cfg` directly below.
    if (!emergency && cfg.controlMode == "passive") { g_applyPassive++; return false; }
    // 问题6 fix (方案B): thermal emergency gate at applyPlacement entry.
    // Defense-in-depth alongside the decidePlacement gate above.
    if (!emergency && g_thermalEmergency.load(std::memory_order_relaxed)) { g_applyThermalEmergency++; return false; }
    // safety-latency gate at applyPlacement entry (defense-in-depth
    // alongside the decidePlacement gate above).
    if (!emergency && g_safetyLatencyActive.load(std::memory_order_relaxed)) { g_applySafetyLatency++; return false; }
    if (ti.role == ROLE_S0_SYSTEM) return false;
    // R3-018: targetCpu<0 is a sentinel (no placement decided), not a real CPU.
    // Bail before any CPU_ISSET(targetCpu) / CPU_SET(targetCpu) — with a
    // cpu_set_t sized for MAX_NR_CPUS, targetCpu=-1 is an out-of-bounds index
    // that would corrupt neighboring mask words / the stack canary.
    if (targetCpu < 0) { return false; }
    // H13(b): if a topology rebuild is in flight and the target CPU is not
    // currently online, refuse the migration. The triple-intersection below
    // already filters offline CPUs from the final mask, but this early-out saves
    // the rate-limit / backoff bookkeeping on a thread that is about to be
    // re-decided anyway next tick.
    if (g_topologyRebuilding.load(std::memory_order_acquire) && !isCpuOnlineNow(targetCpu)) {
        g_applyCpusetSkip++;
        LOG_DEBUG("[PLACE REBUILD_SKIP] tid=%d target=%d offline during topology rebuild",
                  ti.tid, targetCpu);
        return false;
    }
    // H13(b): capture topology generation at apply start. If it changes while we
    // are in flight (hotplug mid-migration), the sched_setaffinity result is
    // discarded — the next tick will re-decide against the new topology.
    const uint64_t genAtApplyStart = g_topologyGeneration.load(std::memory_order_acquire);
    // skip kernel threads — sched_setaffinity will fail or be reverted
    // enhanced triple-detection (name + TID/cmdline + cmdline-empty)
    if (isKernelThread(ti.tname, ti.tid)) { g_applyKernelSkip++; return false; }
    // skip permanently unmigratable threads (errno=22 or 2+ failures)
    {
        std::lock_guard<std::mutex> lock(g_unmigratableMutex);
        if (g_unmigratableThreads.count(ti.tid)) { g_applyUnmigratableSkip++; return false; }
    }
    // D-16: Removed access() TOCTOU check - sched_setaffinity returns ESRCH if thread gone
    time_t now = time(nullptr);
    // P0-3: read the thread's allowed cpuset ONCE at entry and reuse it for BOTH
    // the cpuset pre-check below AND the triple-intersection mask build later.
    // (Previously getThreadAllowedCpus(ti.tid) was called twice per migration —
    // two sched_getaffinity syscalls.)
    cpu_set_t allowedCpus = getThreadAllowedCpus(ti.tid);
    // P2: same defensive zero as in decidePlacement — an empty/garbage mask must
    // be deterministic before isCpuAllowed() consumes it.
    if (CPU_COUNT(&allowedCpus) == 0) CPU_ZERO(&allowedCpus);
    // P2-fix: Use RateLimitGuard RAII for per-second migration rate limiting.
    // Replaces ad-hoc lock_guard block with a clear RAII boundary that
    // documents intent and prevents double-counting.
    bool isFrameCritical = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
    // Pass emergency so the guard bypasses the rate window entirely (and does
    // NOT reset the ordinary migration window that passive migrations share).
    RateLimitGuard rateGuard(isFrameCritical, emergency);
    // emergency migrations (thermal/jank) are safety-critical and
    // must NOT be blocked by the per-second rate limit — otherwise under a storm
    // the regular budget is exhausted and applyPlacement returns false here, so
    // the emergency migration neither moves the thread nor increments
    // migration_count. Emergency bypasses the rate guard (it still records its own
    // count via g_emergencyMigrations).
    if (!emergency && !rateGuard.acquired()) return false;

    // cpuset pre-check — verify target CPU is within thread's allowed cpuset.
    // This prevents errno=22 (EINVAL) from sched_setaffinity when a thread in
    // /background (cpus=1-3) is targeted at CPU7.
    // downgrade from LOG_WARN to throttled LOG_DEBUG.
    // The WARN was firing multiple times per second (same thread, same bad target
    // from applySmartPlacementFallback), burning measurable CPU in the log path.
    // Primary fix is in applySmartPlacementFallback (now filters by allowedCpus);
    // this pre-check is a safety net. Throttle to 1 INFO per 60s for diagnostics.
    {
        // P0-3: reuse the allowedCpus hoisted at function entry (no second sched_getaffinity).
        if (!isCpuAllowed(targetCpu, allowedCpus)) {
            g_applyCpusetSkip++;
            // v6.0.0 P1: snapshot the most recent refused sample for the panel.
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
            // Throttled summary: at most one INFO per 60 seconds.
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

    // per-thread minimum migration dwell — don't re-migrate the same
    // thread within migrationDwellSec seconds (configurable via
    // MIGRATION_DWELL_SEC, default 5s). Reduces migration churn.
    // Distinct from decision cooldown (how often we RE-EVALUATE) and
    // emergency override (which bypasses this gate when emergency=true).
    //
    // P3-51: the emergency override is now actually enforced. Previously the
    // comment promised it but the gate below was unconditional, so a thermal-
    // spike safety migration fired within migrationDwellSec (default 5s) of a
    // prior ordinary migration was silently dropped — exactly the window in which
    // the 5s hard dwell could obstruct real thermal protection. Ordinary
    // migrations still respect the dwell; event-triggered emergency migrations
    // (THERMAL_SPIKE/JANK/FPS_DROP) may move the thread immediately.
    if (!emergency) {
        std::lock_guard<std::mutex> lock(g_lastMigMutex);
        auto it = g_lastMigrationTime.find(ti.tid);
        if (it != g_lastMigrationTime.end() && (now - it->second) < cfg.migrationDwellSec) {
            g_applyIntervalSkip++;  // count decisions skipped due to min dwell interval
            return false;  // silently skip — too soon since last migration
        }
    }
    // P-01/P-03 unified ms-granularity cross-path residency latch.
    // Unlike the 5s dwell above (non-emergency only), THIS gate applies to every
    // path including emergency: after any affinity write on a tid, no other path
    // (main loop / frame-sync / proactive / emergency) may write that tid again
    // until the residency window elapses. This is the single choke point that
    // stops the four decision-makers from overturning each other at millisecond
    // spacing. Emergency keeps its right to MOVE a hot thread quickly; it loses
    // the right to re-migrate the SAME tid on a ~1s loop.
    {
        long long nowLatchMs = migLatchNowMs();
        std::lock_guard<std::mutex> lk(g_migLatchMutex);
        if (!migLatchQueryLocked(ti.tid, nowLatchMs, emergency)) {
            if (emergency) g_latchRejectEmergency.fetch_add(1, std::memory_order_relaxed);
            else           g_latchRejectNormal.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    // migration failure cooldown — revised from fix35.
    // - No permanent blocking (removed in fix36).
    // - Cooldown only after >=2 consecutive failures.
    // - Early release if thread's current CPU changed since last failure.
    // - Floor guarantee: at least kMigCooldownFloorPerSec migrations/sec bypass cooldown.
    {
        std::lock_guard<std::mutex> lock(g_migFailMutex);
        // floor-bypass window management
        if (now - g_cooldownFloorWindowStart >= 1) {
            g_cooldownFloorWindowStart = now;
            g_cooldownBypassCount.store(0, std::memory_order_relaxed);
        }
        bool bypassByFloor = (g_cooldownBypassCount.load(std::memory_order_relaxed) < kMigCooldownFloorPerSec);

        // P3-52: exponential backoff expiry (steady_clock ms). This is the
        // authoritative per-tid retry throttle (replaces the flat 10s lockout).
        // Sweep expired entries opportunistically (throttled to every 64th
        // placement) and enforce a 1024-entry ceiling by evicting the oldest.
        long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            // P1: non-atomic unsigned counter on a hot path reached from multiple
            // applyPlacement call sites would lose increments. Atomic, relaxed
            // order (only used for a throttled sweep cadence).
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
            // P1-2: unordered_map::erase(begin()) removes an ARBITRARY bucket
            // element, not the oldest backoff. When we need to evict due to
            // the cap, find the entry with the smallest backoffUntilMs (i.e.
            // the one that will expire soonest) and erase that.
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
            // early release if thread CPU changed since failure (state changed)
            bool cpuChanged = false;
            auto cpuIt = g_migFailCpu.find(ti.tid);
            if (cpuIt != g_migFailCpu.end() && cpuIt->second != ti.currentCpu) {
                cpuChanged = true;
            }
            if (backoffActive && !cpuChanged && !bypassByFloor) {
                g_applyCooldownSkip++;
                return false;
            }
            // expired, cpu-changed, or floor bypass: clear backoff and allow
            g_migFailBackoffUntilMs.erase(it);
            if (cpuChanged) {
                // state changed: also reset failure count so next failure starts fresh
                g_migFailCount.erase(ti.tid);
                g_migFailCpu.erase(ti.tid);
            }
        }
        if (bypassByFloor) {
            g_cooldownBypassCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Pool-level affinity instead of single-CPU lock.
    // P0-7 + new-B: Final mask = pool_mask ∩ thread_allowed_cpuset ∩ online_cpus.
    // Previously only pool mask was set without intersection, which could include
    // CPUs not allowed by the thread's cpuset or offline CPUs (causing EINVAL
    // that triggered permanent unmigratable marking).
    // P1-1 fix: g_littlePool/g_midPool/g_bigPool (and g_allCpus) are rewritten
    // by detectCpuTopology() under g_topologyRwMutex. Iterating them lock-free
    // here races a concurrent hotplug rebuild (iterating a vector being
    // assign()'d is UB). Snapshot the pools under the lock, then build the mask
    // from the local copies.
    std::vector<int> snapLittle, snapMid, snapBig, snapAll;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        snapLittle = g_littlePool;
        snapMid    = g_midPool;
        snapBig    = g_bigPool;
        snapAll    = g_allCpus;
    }
    // P1-2: build cpu_set_t masks once, then use CPU_ISSET (O(1) bit test) to
    // find which pool targetCpu belongs to — replaces the previous 3× std::find
    // over the pool vectors (each O(pool_size) linear scan).
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

    // Intersect with thread's allowed cpuset — P0-3: reuse the hoisted
    // allowedCpus (no second sched_getaffinity per migration).
    cpu_set_t cpusetIntersected;
    CPU_ZERO(&cpusetIntersected);
    CPU_AND(&cpusetIntersected, &poolMask, &allowedCpus);

    // fix52 new-B: Intersect with online CPUs (responds to hotplug, avoids EINVAL).
    // P0-2: getOnlineCpus() now returns the topology-maintained cache (no sysfs
    // read on the migration hot path). P2: result is built once here and reused
    // for the final mask below.
    std::vector<int> onlineCpus = getOnlineCpus();
    cpu_set_t onlineMask;
    CPU_ZERO(&onlineMask);
    for (int cpu : onlineCpus) CPU_SET(cpu, &onlineMask);
    cpu_set_t finalMask;
    CPU_ZERO(&finalMask);
    CPU_AND(&finalMask, &cpusetIntersected, &onlineMask);

    // P0-2 (问题78): Strict triple-intersection only.
    // When pool ∩ allowed_cpus ∩ online_cpus is empty, SAFELY ABANDON migration.
    // DO NOT fall back to pool∩online (removes thread cpuset restriction),
    // online-only (removes both pool and cpuset restrictions), or pool mask
    // (removes online restriction). Any fallback breaks the thread's original
    // allowed CPU boundary and is a security/safety violation.
    if (CPU_COUNT(&finalMask) == 0) {
        g_applyCpusetSkip++;
        LOG_WARN("[PLACE SAFE_ABANDON] %d/%d target CPU%d: triple intersection (pool∩allowed∩online) is empty. Skipping migration (no fallback expansion).",
                 ti.pid, ti.tid, targetCpu);
        return false;
    }

    // P0-3 : read the original affinity mask BEFORE the syscall so
    // we have it for the recovery record, but do NOT set affinityModified=true or
    // store originalAffinity until sched_setaffinity() SUCCEEDS. Previously the
    // flag was set before the syscall; if the first attempt failed, the thread was
    // marked modified anyway, so on the later successful retry origCaptured was
    // false and the recovery record was never written (uninstall could not
    // restore affinity). Correct order: read orig -> sched_setaffinity -> on
    // success mark modified + record recovery.
    cpu_set_t savedOrigMask;
    CPU_ZERO(&savedOrigMask);
    bool needOrigSnapshot = !ti.affinityModified;
    int savedErrno = 0;
    if (needOrigSnapshot) {
        if (sched_getaffinity(ti.tid, sizeof(savedOrigMask), &savedOrigMask) != 0) {
            // Cannot read current affinity; abandon rather than record a bogus orig.
            savedErrno = errno;
            // ESRCH (thread already exited) is not a real
            // placement failure — only count it in g_applyFailedEsrch.
            if (savedErrno != ESRCH) g_applyFailed++;
            switch (savedErrno) {
                case EINVAL: g_applyFailedEinval++; break;
                case ESRCH:  g_applyFailedEsrch++;  break;
                case EPERM:  g_applyFailedEperm++;  break;
                default:     g_applyFailedOther++;  break;
            }
            // R3-008: the very first getaffinity snapshot used to return false
            // immediately with NO backoff bookkeeping, so the main loop retried
            // this (still-present) thread every tick and spun on a failing
            // syscall. Treat it like the setaffinity failure path: accumulate the
            // exponential backoff (ESRCH excluded — the thread is leaving).
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
    // P3-5: Save errno immediately after syscall. Subsequent lock_guard/map
    // operations can overwrite errno, causing the EINVAL check below to use
    // a stale/wrong value.
    savedErrno = errno;
    if (ret == 0) {
        // H13(b): if a hotplug bumped g_topologyGeneration between our candidate
        // selection and now, the targetCpu may be offline or the pool mapping may
        // have changed. The sched_setaffinity already succeeded (kernel moved the
        // thread), but we must NOT stamp a recovery record against the old
        // topology — the next tick will re-decide and re-record. Count the
        // migration but skip recovery settlement.
        const bool generationInvalidated =
            (g_topologyGeneration.load(std::memory_order_acquire) != genAtApplyStart);
        // P0-3: NOW mark modified + persist original. Only a successful migration
        // counts as having mutated the thread.
        if (needOrigSnapshot && !generationInvalidated) {
            std::lock_guard<std::mutex> tdbLock(g_threadsMutex);
            auto thIt = g_threads.find(ti.tid);
            if (thIt != g_threads.end()) {
                thIt->second.originalAffinity = savedOrigMask;
                thIt->second.affinityModified = true;
            }
        }
        // unified recovery record. recoveryRecordAffinity
        // returns bool and only marks recorded after the atomic write succeeds;
        // a failed write leaves the tid unrecorded for the next scan.
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
            // ROUND-013: subsequent successful affinity change -> refresh applied.
            // H13(b): if topology changed mid-flight, skip the applied-refresh —
            // the next tick will re-decide against the new topology.
            std::string appliedList = cpuSetToListStrLocal(finalMask);
            if (!aatpe_recovery::recoveryRefreshAppliedAffinity(ti.tid, appliedList)) {
                LOG_WARN("recoveryRefreshAppliedAffinity failed tid=%d", ti.tid);
            }
        }
        // P2-fix: Use RateLimitGuard::consume() to count successful migration
        rateGuard.consume();
        g_totalMigrations++;
        g_applySuccess++;
        // reset all failure tracking on successful migration (P3-52: backoff resets to 0)
        {
            std::lock_guard<std::mutex> lock(g_migFailMutex);
            g_migFailCount.erase(ti.tid);
            g_migFailCooldown.erase(ti.tid);
            g_migFailCpu.erase(ti.tid);
            g_migFailBackoffUntilMs.erase(ti.tid);
        }
        // record last migration time for per-thread min interval
        {
            std::lock_guard<std::mutex> lock(g_lastMigMutex);
            g_lastMigrationTime[ti.tid] = now;
        }
        // P-01/P-03: stamp the unified ms residency latch. Determine whether this
        // write was an UPHILL promotion (to a higher pool) so the post-uphill
        // no-re-placement window engages for normal paths.
        {
            int latchFromPool = (int)getPoolForCpu(ti.currentCpu);
            int latchToPool   = (int)getPoolForCpu(targetCpu);
            bool uphill = (latchToPool > latchFromPool);
            std::lock_guard<std::mutex> lk(g_migLatchMutex);
            migLatchStampLocked(ti.tid, migLatchNowMs(), uphill);
        }
        // #34: anchor the hysteresis settled point to this successful migration.
        // R3-009: use the scoreCtx overload — buildScoringContext(cfg) snapshots
        // pools/cpuMetrics/config once, instead of the 3-arg computeScore that
        // re-locks topology/cpumetrics/config on this hot success path.
        {
            ScoringContext scoreCtx = buildScoringContext(cfg);
            placementRecordMigration(ti.tid, targetCpu, computeScore(targetCpu, ti, targetCpu, scoreCtx));
        }
        LOG_DEBUG("[PLACE] %d/%d -> CPU%d [%d] %s", ti.pid, ti.tid, targetCpu, (int)ti.role, reason.c_str());
        // 算力优先分配：placement 成功后，对关键线程(UI/RENDER/GAME)
        // 通过 sched_setattr() 设 SCHED_FIFO/RR + uclamp。best-effort：
        // 失败只 LOG_WARN，不影响 applyPlacement 的成功语义。
        // #30: do NOT RT-boost a low-confidence thread — the classifier may have
        // guessed its role, and promoting a guessed-SYSTEM/background vendor
        // binder to SCHED_FIFO would destabilize system services.
        // P1-11: confidence==0 (UNKNOWN) is treated as low confidence and is
        // REFUSED RT boost. Only roleConfidence >= threshold earns RT.
        bool lowRoleConf = (ti.roleConfidence < cfg.threadRoleConfidenceThreshold);
        if (cfg.priorityBoostEnabled && !lowRoleConf &&
            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME)) {
            aatpe_priority::aatpeSetThreadPriority(ti.tid, ti.role, cfg.priorityBoostLevel);
        }
        // fix: honor LOG_DECISIONS config
        if (cfg.logDecisions) {
            // R3-013: the old filename used time_t seconds, so two migrations of
            // the SAME pid/tid within one second overwrote each other's decision
            // file. Use wall-clock milliseconds plus a process-global monotonic
            // sequence so every successful migration gets a unique file.
            static std::atomic<unsigned long long> s_decisionSeq{0};
            long long decNowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            unsigned long long seq = s_decisionSeq.fetch_add(1, std::memory_order_relaxed);
            std::string decPath = std::string(DECISION_DIR) + "/" + std::to_string(ti.pid) + "_" + std::to_string(ti.tid) + "_" + std::to_string(decNowMs) + "_" + std::to_string(seq);
            std::string decContent = "pid=" + std::to_string(ti.pid) + "\ntid=" + std::to_string(ti.tid) + "\ntarget_cpu=" + std::to_string(targetCpu) + "\nrole=" + std::to_string((int)ti.role) + "\nreason=" + reason + "\ntimestamp=" + std::to_string(now) + "\n";
            writeFile(decPath.c_str(), decContent);
        }
        return true;  // only successful sched_setaffinity returns true
    } else {
        // fix: log sched_setaffinity failures
        // ESRCH (thread already exited) is not a real placement
        // failure — it only increments g_applyFailedEsrch below.
        if (savedErrno != ESRCH) g_applyFailed++;
        // R3-001: sched_setaffinity() is atomic — when it fails the kernel leaves
        // the thread's affinity mask UNCHANGED. On a *subsequent* migration (the
        // thread was already successfully placed once) the mask therefore already
        // equals the last successfully applied placement, i.e. exactly the desired
        // "pre-this-migration" state. The old code unconditionally rolled back to
        // originalAffinity (the very first pre-migration mask), which on the second
        // migration threw away the first successful placement and oscillated the
        // thread between two pools forever. On the *first* migration the kernel
        // likewise kept savedOrigMask, so no corrective syscall is needed either.
        // We deliberately do NOT issue a rollback sched_setaffinity here.
        LOG_DEBUG("[PLACE] applyPlacement failed errno=%d on tid %d; kernel preserved prior affinity mask (no rollback to originalAffinity).",
                  savedErrno, ti.tid);
        // P1-18: Classify failure by errno (real original)
        // failure-reason taxonomy. These counters are already
        // surfaced to state JSON (apply_failed_{einval,esrch,eperm,other}) and
        // the CLI:
        //   EINVAL = 22 : invalid mask / kernel thread / cpuset-restricted target
        //                 (kernel rejects the requested affinity)
        //   ESRCH  = 3  : thread already exited between scan and setaffinity
        //   EPERM  = 1  : no CAP_SYS_NICE / affinity permission denied
        //   other       : transient (EBUSY, EAGAIN...) or unknown kernel rejection
        // Pre-migration skips are counted separately: apply_cpuset_skip (cpuset
        // restriction), apply_kernel_skip, apply_unmigratable_skip,
        // apply_interval_skip, apply_cooldown_skip, apply_passive.
        switch (savedErrno) {
            case EINVAL: g_applyFailedEinval++; break;
            case ESRCH:  g_applyFailedEsrch++;  break;
            case EPERM:  g_applyFailedEperm++;  break;
            default:     g_applyFailedOther++;  break;
        }
        // record failure — cooldown only after >=2 consecutive failures, no permanent block
        // P2-20: EINVAL no longer permanently marks unmigratable on the FIRST
        // failure. A single EINVAL can be transient (cpuset race, hotplug
        // mid-syscall). Now: first EINVAL → count + short cooldown; only after
        // kMigFailTriggerThreshold (2) consecutive EINVAL failures do we mark
        // the thread permanently unmigratable. This prevents a single
        // spurious EINVAL from permanently banning a thread.
        bool markUnmigratable = false;
        {
            std::lock_guard<std::mutex> lock(g_migFailMutex);
            int& fails = g_migFailCount[ti.tid];
            fails++;
            g_migFailCpu[ti.tid] = ti.currentCpu;
            if (savedErrno == EINVAL) {
                // EINVAL from sched_setaffinity: the affinity mask was rejected.
                // This can be transient (cpuset/hotplug race). Only mark
                // permanently after kMigFailTriggerThreshold consecutive failures.
                if (fails >= kMigFailTriggerThreshold) {
                    markUnmigratable = true;
                }
            }
            // P3-52: exponential backoff ramp. The retry delay grows with the
            // consecutive failure count (100 -> 200 -> 500 -> 1000 -> 2000 ms,
            // capped) and is reset to 0 on the next successful migration. ESRCH
            // (thread already exited) is NOT a real placement failure, so we do
            // not accumulate a throttle for a thread that is leaving anyway.
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
        // ESRCH (errno=3) means the thread exited between our
        // scan and sched_setaffinity(). This is completely normal on Android
        // where threads are created/destroyed at high frequency — it is NOT a
        // placement failure. Downgrade to LOG_DEBUG to stop logcat flooding
        // (which itself consumed measurable CPU at 116% baseline). Other errnos
        // (EINVAL/EPERM/EBUSY) stay at WARN because they indicate real kernel
        // rejection. A periodic INFO summary every 100 ESRCHs keeps the
        // phenomenon observable without spamming.
        if (savedErrno == ESRCH) {
            // P1-1: non-atomic int RMW from multiple worker threads is a data
            // race. Use atomic with relaxed ordering — this is just a log-throttle
            // counter, no cross-thread ordering needed.
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
        return false;  // failed placement returns false
    }
}

// ============================================================================
// DECISIONS DIRECTORY CLEANUP (fix: prevent infinite growth)
// ============================================================================
void cleanupDecisionsDir() {
    // True lazy deletion - only scan when file count > 500,
    // then batch-delete oldest 50% to avoid I/O storm.
    DIR* dir = opendir(DECISION_DIR);
    if (!dir) return;

    // First pass: count files and collect (seq, filename).
    // PLACE-10: decision filenames are `pid_tid_ms_seq` (see logDecisionsDirWrite).
    // rfind('_') lands on the underscore BEFORE the trailing seq, so the numeric
    // key we parse is the monotonic seq counter, not the wall-clock ms. Because
    // g_decisionSeq is incremented once per successful decision, seq is
    // monotonically increasing with creation time — so sorting by seq ascending
    // correctly yields oldest-first. The variable is renamed accordingly.
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

    // Only clean when file count exceeds threshold
    const int kMaxDecisionFiles = 500;
    if (fileCount <= kMaxDecisionFiles) return;

    // Sort by seq (oldest first) and delete oldest 50%
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

// ============================================================================
// Pool helper functions (per-pool ML and placement decisions)
// ============================================================================
// Pool cache: CPU → CpuPool lookup cache, invalidated on topology rebuild.
// Lookup happens under the topology read lock to prevent rebuild races.
static std::unordered_map<int, CpuPool> g_poolCache;
static std::mutex g_poolCacheMutex;
static void invalidatePoolCache() {
    std::lock_guard<std::mutex> lock(g_poolCacheMutex);
    g_poolCache.clear();
}

CpuPool getPoolForCpu(int cpu) {
    if (cpu < 0) return CpuPool::UNKNOWN;
    // Hold topology read lock for entire lookup/cache operation
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
        // B3 task1: PRIME cpus are NOT in any legacy little/mid/big pool (G6
        // keeps them as the separate top cluster). Treat them as CpuPool::BIG
        // for legacy callers (prime is a strict superset of big capability),
        // so a prime cpu never returns UNKNOWN. New spillover code uses
        // tierOfCpu()/primePool() directly to keep the distinction.
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

// ============================================================================
// 智能绑核：computeCoreSuitability(cpuId, role, currentLoadPct)
//
// 综合评分（0~100，越高越适合放置该角色线程）：
//   1) 核心类型分（core affinity，权重 CORE_AFFINITY_WEIGHT）
//        UI/RENDER/GAME: BIG=100 MID=60 LITTLE=20  —— 偏好大核/超大核
//        BACKGROUND:     LITTLE=100 MID=60 BIG=20  —— 偏好小核，大核空闲也可
//        THROUGHPUT:     BIG=80 MID=70 LITTLE=40  —— 偏吞吐，大中核皆可
//   2) 负载分（load balance，权重 LOAD_BALANCE_WEIGHT）
//        (100 - currentLoadPct)，当前 util% 越高分越低
//   3) 温度分（temperature，权重 TEMPERATURE_WEIGHT）
//        (100 - cpuTemp)，该核温度越高分越低（clamp 0~100）
//
// 三项按权重加权后归一到 0~100。函数只读 g_cpuMetrics（在调用方持锁下），
// 不做任何迁移；是否真正替换 bestCpu 由 decidePlacement 的过载降级逻辑决定。
// ============================================================================
int computeCoreSuitability(int cpuId, ThreadRole role, int currentLoadPct,
                           float coreAffW, float loadW, float tempW) {
    if (cpuId < 0) return 0;

    // ---- 1. 核心类型分（角色相关）----
    CpuPool pool = getPoolForCpu(cpuId);
    int coreTypeScore = 50;  // UNKNOWN fallback
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
        // UI / RENDER / GAME：偏好大核/超大核
        switch (pool) {
            case CpuPool::LITTLE: coreTypeScore = 20; break;
            case CpuPool::MID:    coreTypeScore = 60;  break;
            case CpuPool::BIG:    coreTypeScore = 100; break;
            default:              coreTypeScore = 50;  break;
        }
    }

    // ---- 2. 负载分 ----
    int load = currentLoadPct;
    if (load < 0) load = 0;
    if (load > 100) load = 100;
    int loadScore = 100 - load;

    // ---- 3. 温度分 ----
    int cpuTemp = 0;
    {
        std::lock_guard<std::mutex> ml(g_cpuMetricsMutex);
        auto it = g_cpuMetrics.find(cpuId);
        if (it != g_cpuMetrics.end()) cpuTemp = it->second.temp;
    }
    if (cpuTemp < 0) cpuTemp = 0;
    if (cpuTemp > 100) cpuTemp = 100;
    int tempScore = 100 - cpuTemp;

    // ---- 加权求和（权重和归一化，防止配置和!=1时越界）----
    float wsum = coreAffW + loadW + tempW;
    if (wsum <= 0.0f) wsum = 1.0f;
    float score = (coreTypeScore * coreAffW + loadScore * loadW + tempScore * tempW) / wsum;
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    // H6: use std::lround instead of hand-rolled (int)(x+0.5f). Input score is
    // clamped to [0,100] so behavior is unchanged on this path; lround is correct
    // for negatives / exact .5 cases and silences clang-tidy. <cmath> already
    // included at line 22.
    return static_cast<int>(std::lround(score));
}

// ============================================================================
// 智能绑核：过载/过温降级微调。
// 在 computeCandidateScores 选出 bestCpu 后调用：
//   - 若 bestCpu 是大核且当前负载>80% 或温度>CRITICAL 墙，则尝试在 MID pool
//     中找负载最低的核替换 bestCpu（UI/RENDER 大核过载时退中核）。
//   - GAME 线程：优先大核，但若大核温度过高，也退中核（缓存亲和性由
//     computeCandidateScores 已有逻辑保证——同游戏线程相邻核心）。
//   - BACKGROUND 线程：若 bestCpu 是大核且大核空闲(load<30%)则保留，
//     否则退小核。
// 返回可能替换后的 cpuId（不修改 Decision 其他字段）。
// ============================================================================
static int applySmartPlacementFallback(int bestCpu, const ThreadInfo& ti,
                                       const Config& cfgSnap,
                                       const cpu_set_t& allowedCpus) {
    if (!cfgSnap.smartPlacementEnabled) return bestCpu;
    if (bestCpu < 0) return bestCpu;
    if (ti.role == ROLE_S0_SYSTEM) return bestCpu;  // SYSTEM 保护，不动

    // 读 bestCpu 的当前负载与温度
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
    // CRITICAL 墙：与 thermal.cpp 一致（新字段优先，旧字段 fallback）
    int criticalWall = cfgSnap.thermalCriticalThreshold > 0
                       ? cfgSnap.thermalCriticalThreshold
                       : cfgSnap.thermalLimitCpu;

    // ---- P-02: latched overload pullback (20% deadband + 3s no-reclimb) ----
    // Previously the pullback triggered at bestLoad>80% with NO release threshold:
    // as soon as load dipped, the next cycle re-scored BIG as best and force-
    // migrated the thread back up, only to pull it down again on the next overload.
    // That is the bang-bang. Now:
    //   * trigger pullback at load > 85% (or thermal wall);
    //   * once pulled back, hold the thread on MID for >= kPullbackHoldMs (3s);
    //   * release the hold only after the big core has settled below
    //     kPullbackReleasePct (65%) AND the 3s hold elapsed -> 20% deadband.
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
                // Release only after the hold AND the big core dropped into the
                // deadband. Otherwise keep holding.
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
            // Hold-off: scoring prefers BIG again, but we keep this thread on MID
            // for the 3s window so it cannot immediately re-climb into overload.
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
                // Only switch when MID is genuinely quieter (load gap > 20%).
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

    // ---- BACKGROUND：大核不空闲时退小核 ----
    if (ti.role == ROLE_S5_BACKGROUND && bestPool == CpuPool::BIG && bestLoad > 30) {
        std::vector<int> littleCpus = getPoolCpus(CpuPool::LITTLE);
        if (!littleCpus.empty()) {
            // 仅在 allowedCpus 中选小核，避免选到 CPU3
            // 之类被线程 cpuset 排除的核心（之前每次都选 CPU3 导致 CPUSET_SKIP
            // 每秒刷屏 WARN 日志）。
            int bestLittle = -1;
            int bestLittleLoad = 101;
            for (int c : littleCpus) {
                if (!isCpuAllowed(c, allowedCpus)) continue;  // skip cpuset-excluded
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
            // 没有 allowed 的小核，保持原 bestCpu 不强制迁移。
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

// ============================================================================
// fix52 new-C: Dynamic online CPU detection (responds to CPU hotplug)
// P0-2: the hot path (applyPlacement) previously parsed
// /sys/devices/system/cpu/online on EVERY migration. refreshCpuTopology() runs
// once per main-loop iteration and is the only caller that should hit sysfs;
// it populates g_onlineCpusCache via setOnlineCpusCache(). getOnlineCpus() then
// returns the cached vector (cheap copy of ~8 ints) instead of opening +
// reading + parsing the sysfs node on every migration.
// ============================================================================
static std::vector<int> g_onlineCpusCache;
static std::mutex g_onlineCpusCacheMutex;

// H13 test seams (the atomics themselves are declared near g_topologyRwMutex).
#ifdef AATPE_HOST_TEST
void setTopologyDebounceMsForTest(int ms) { g_topologyDebounceMs.store(ms); }
#endif
int topologyRebuildCount() { return s_topologyRebuildCount.load(); }
bool isTopologyRebuilding() { return g_topologyRebuilding.load(); }

// Hard offline filter: is cpu in the current online cache? Safe to call on the
// hot path (mutex-protected read of a small vector).
bool isCpuOnlineNow(int cpu) {
    std::lock_guard<std::mutex> lk(g_onlineCpusCacheMutex);
    for (int c : g_onlineCpusCache) if (c == cpu) return true;
    return false;
}

void setOnlineCpusCache(const std::vector<int>& cpus) {
    std::lock_guard<std::mutex> lk(g_onlineCpusCacheMutex);
    g_onlineCpusCache = cpus;
}

// Raw sysfs read — ONLY called from refreshCpuTopology() (periodic, not per-migration).
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
    // Fast path: return the topology-maintained cache.
    {
        std::lock_guard<std::mutex> lk(g_onlineCpusCacheMutex);
        if (!g_onlineCpusCache.empty()) return g_onlineCpusCache;
    }
    // Cold start (refreshCpuTopology has not run yet): fall back to sysfs.
    return getOnlineCpusRaw();
}

// P3-a: seed the online cache from raw sysfs at the end of detectCpuTopology().
// Previously the cache was only populated by refreshCpuTopology() in the main loop,
// leaving a <1s cold-start window where g_onlineCpusCache was empty and
// isCpuOnlineNow() filtered every candidate. Reads raw sysfs; on failure leaves
// the existing cache untouched (safe). Called by detectCpuTopology() after the
// pool rebuild completes.
void seedOnlineCacheFromSysfs() {
    std::vector<int> online = getOnlineCpusRaw();
    if (online.empty()) return;  // safe fallback: keep whatever was there
    std::sort(online.begin(), online.end());
    setOnlineCpusCache(online);
}

void refreshCpuTopology() {
    // P0-2: read sysfs directly (not the cache — we ARE the cache updater),
    // then publish the fresh result so applyPlacement() stops hitting sysfs.
    std::vector<int> online = getOnlineCpusRaw();
    if (online.empty()) return;
    // R3-015: the element-wise comparison below assumes both lists are in the
    // same (ascending) order. Sort `online` defensively so a non-ascending parse
    // of /sys/.../online can never report a spurious topology change (or miss a
    // real one) purely due to ordering.
    std::sort(online.begin(), online.end());
    setOnlineCpusCache(online);
    // P1-2: read g_allCpus under g_topologyRwMutex — it is written by
    // detectCpuTopology() under the same lock. Previously this comparison ran
    // lock-free, racing a concurrent hotplug rebuild.
    bool changed;
    // PLACE-07: declared at function scope so the post-rebuild block below can use
    // them; removed the pre-rebuild dead reads that populated them on the way in.
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
        // H13(a): debounce. If we rebuilt within the last kDebounceMs, skip the
        // expensive detectCpuTopology() + cache invalidation this tick. The
        // online cache above was already refreshed, so applyPlacement still sees
        // the correct online mask; the next tick (200ms later) will re-evaluate
        // and rebuild if the topology is still different. This coalesces a burst
        // of online/offline uevents (thermal big-core toggling) into one rebuild.
        long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long lastRebuild = s_lastTopologyRebuildMs.load(std::memory_order_relaxed);
        int debounceMs = g_topologyDebounceMs.load(std::memory_order_relaxed);
        if (debounceMs > 0 && (nowMs - lastRebuild) < debounceMs) {
            LOG_DEBUG("TOPOLOGY-CHANGE: debounced (last rebuild %lldms ago < %dms), skipping",
                      nowMs - lastRebuild, debounceMs);
            return;
        }
        // H13(b): mark rebuilding so applyPlacement() refuses to pin a thread to
        // a CPU that may be offline mid-rebuild. detectCpuTopology() owns the
        // topology write lock for the complete rebuild, so readers can only
        // observe a coherent topology snapshot.
        // P3-b: RAII guard sets g_topologyRebuilding=true on construction and
        // clears it on scope exit (including any future early-return added between
        // now and the end of this block). -fno-exceptions means no unwinding today,
        // but the guard is free and prevents a future bug leaving the flag stuck.
        struct RebuildGuard {
            ~RebuildGuard() { g_topologyRebuilding.store(false, std::memory_order_release); }
        } guard;
        g_topologyRebuilding.store(true, std::memory_order_release);
        detectCpuTopology();
        s_lastTopologyRebuildMs.store(nowMs, std::memory_order_relaxed);
        s_topologyRebuildCount.fetch_add(1, std::memory_order_relaxed);
        // Invalidate pool cache after topology/pool rebuild
        invalidatePoolCache();
        // ====================================================================
        // #36 topology change FULL recalculation:
        // After hotplug/topology change, we must clear ALL caches that were
        // computed against the old CPU layout:
        //   1. Score cache (g_scoreCache) — cached scores are stale.
        //   2. Placement history (g_history) — old CPU IDs may be gone.
        //   3. Settled placement state (g_threadSettle) — old settledCpu invalid.
        //   4. Precomputed base scores — capacity/contention baseline changed.
        //   5. ML assignment cache — model was trained against old pool mapping.
        // ====================================================================
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
        // Bump topology generation so state consumers know the layout changed.
        g_topologyGeneration.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("TOPOLOGY-CHANGE: full cache recalculation done (scores+history+settle+base)");
        // P1-2: re-read the post-rebuild sizes under the lock — the previous
        // snapshot was from BEFORE the rebuild.
        {
            std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
            cpuCountAfter = g_cpuCount;
            littleSz = g_littlePool.size();
            midSz = g_midPool.size();
            bigSz = g_bigPool.size();
        }
        LOG_INFO("CPU topology refreshed: %d CPUs online, pools rebuilt (L=%zu M=%zu B=%zu)",
                 cpuCountAfter, littleSz, midSz, bigSz);
        // GPUTOP-03: use atomicWriteFile (tmp+rename) to match the topology layer.
        // A concurrent WebUI/APK/watchdog reader must never see a half-written file.
        atomicWriteFile((std::string(STATE_DIR) + "/cpu_count").c_str(), std::to_string(cpuCountAfter));
        // Build CPU list string (e.g. "0-3,6-7")
        std::string cpuListStr;
        for (size_t i = 0; i < online.size(); i++) {
            if (i > 0) cpuListStr += ",";
            cpuListStr += std::to_string(online[i]);
        }
        atomicWriteFile((std::string(STATE_DIR) + "/online_cpus").c_str(), cpuListStr);
    }
}

// ============================================================================
// P1-9: Unmigratable thread re-probe (auto-unlock)
// ============================================================================
// P1-6: atomic to avoid RMW data race (reprobe runs on the timer thread,
// but decidePlacement also touches unmigratable set). time_t is long on
// Android arm64.
std::atomic<long long> g_lastUnmigratableReprobe{0};

void reprobeUnmigratableThreads() {
    time_t now = time(nullptr);
    long long last = g_lastUnmigratableReprobe.load(std::memory_order_relaxed);
    if ((long long)now - last < kUnmigratableReprobeIntervalSec) return;
    g_lastUnmigratableReprobe.store((long long)now, std::memory_order_relaxed);
    // P2: snapshot the unmigratable set under the lock, then release it BEFORE
    // doing stat()/readFirstLine()/sched_getaffinity() per tid. Holding
    // g_unmigratableMutex across those syscalls blocked decidePlacement()'s
    // fast-path count(tid) check on every decision cycle.
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
        // (问题27): Read thread name for kernel thread detection.
        // Kernel threads are permanently unmigratable — don't remove them.
        std::string tname = readFirstLine(commPath.c_str());
        trim(tname);
        if (isKernelThread(tname, tid)) {
            continue;  // kernel thread: permanently unmigratable, keep in list
        }
        cpu_set_t probeMask;
        CPU_ZERO(&probeMask);
        if (sched_getaffinity(tid, sizeof(probeMask), &probeMask) == 0) {
            if (CPU_COUNT(&probeMask) > 1) {
                // fix55 (问题27/28): Verify thread's allowed CPUs intersect with
                // at least one online pool CPU. If no intersection, thread is
                // still effectively unmigratable (cgroup restriction).
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
    // Re-take the lock only for the bulk erase.
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

// ============================================================================
// WEBUI JSON DATA OUTPUT
// Writes structured JSON files to /dev/aatpe/webui/ for the WebUI frontend.
// 5 JSON files: threads.json, decisions.json, explain.json, model.json, system.json
// Core scheduling logic is NOT modified - this is read-only data export.
// ============================================================================

// ============================================================================
// Lightweight Event-Driven Emergency Migration
// Triggered when jank_detected transitions from false→true.
// Only migrates critical threads (UI/RENDER/GAME), skips dumpsys/ML learning.
// Cooldown: 1000ms between emergency migrations (was 100ms — too short,
//            combined with THERMAL_SPIKE每秒 event it caused continuous migration
//            CPU burn; raised to 1s per CPU-fix requirements).
// ============================================================================
// P1-5: stored as atomic<long long> ms-since-epoch to avoid a data race on
// the steady_clock::time_point (which is not trivially atomic in C++17).
static std::atomic<long long> g_lastEmergencyMigrate{
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()};
// P-01: was 1000ms. A 1s cooldown was too short: a migration at t=0 caused a
// cache-cold jank at ~t=50ms, the rising edge re-fired at t=1000ms, and the
// loop's period matched the cooldown itself — the system could not escape the
// self-sustaining ping-pong. Raise to 2000ms so the window between emergency
// moves exceeds the residency latch and lets the cache warm / frames recover.
static constexpr int kEmergencyCooldownMs = 2000;
// cumulative emergency migration counter (was writing the
// per-call value, overwriting the file -> always read 0).
static std::atomic<long long> g_emergencyMigrations{0};

// P-01: per-minute ceiling on emergency migrations. The 2s cooldown still allows
// up to ~30 calls/min, each moving up to 3 threads — far more than a healthy
// system ever needs. Cap successful emergency migrations to kEmergencyPerMinute
// inside any rolling 60s window; this is the outer bound that guarantees the
// emergency path cannot become a migration storm even if jank stays elevated.
static constexpr int kEmergencyPerMinute = 10;
static std::mutex g_emergWindowMutex;
static long long g_emergWindowStartMs = 0;
static int g_emergWindowCount = 0;

// P-01: emergency reversal guard. Record the pool an emergency move left and
// arrived at; within kEmergencyReverseMs a second emergency may NOT move the
// same tid back to the pool it just left (A->B->A). Combined with the 1500ms
// residency latch this closes the 1-2s A<->B loop.
struct EmergencyRec { long long toMs; int fromPool; int toPool; };
static std::unordered_map<int, EmergencyRec> g_lastEmergencyRec;

// P3-7: Helper to collect critical threads (UI/RENDER/GAME), shared by all migration functions
static int rolePriority(ThreadRole r) {
    // UI threads have highest priority for frame-sync migration,
    // then RENDER, then GAME. Ensures most critical thread migrated first
    // when migration count is limited to 1.
    switch (r) {
        case ROLE_S1_UI: return 0;
        case ROLE_S2_RENDER: return 1;
        case ROLE_S3_GAME: return 2;
        default: return 99;
    }
}

static std::vector<ThreadInfo> collectCriticalThreads(int maxCount,
        const std::set<std::string>* priorityPnames) {
    // P0-5: do NOT hold g_threadsMutex across the g_unmigratableMutex probe.
    // Previously the whole table was scanned with g_threadsMutex held AND a
    // second lock (g_unmigratableMutex) taken per-thread — serializing the
    // eBPF sched_switch updater against emergency/lightweight migration.
    // Two-pass:
    //   1) under g_threadsMutex, collect (tid, role, protect) candidates only.
    //   2) release g_threadsMutex; take g_unmigratableMutex once to filter.
    //   3) re-take g_threadsMutex to copy the full ThreadInfo for survivors.
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
    // Bulk unmigratable filter under a single g_unmigratableMutex hold.
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
    // Second pass: copy full ThreadInfo for survivors under g_threadsMutex.
    std::vector<ThreadInfo> criticalThreads;
    criticalThreads.reserve(survivors.size());
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (int tid : survivors) {
            auto it = g_threads.find(tid);
            if (it != g_threads.end()) criticalThreads.push_back(it->second);
        }
    }
    // sort by role priority (UI > RENDER > GAME) so most critical
    // threads are first when migration count is limited.
    // A3: when priorityPnames (current fg pkg + resolved VM guest pnames) is
    // given, threads belonging to those packages sort FIRST — reserving slots
    // before the maxCount truncation. This stops the VM container's own many
    // S2/S3 threads from crowding out the guest game's S3_GAME thread from the
    // candidate set. Within the priority group, role priority still applies.
    auto isPrio = [&](const ThreadInfo& t) {
        return priorityPnames && priorityPnames->count(t.pname) > 0;
    };
    std::sort(criticalThreads.begin(), criticalThreads.end(),
        [&](const ThreadInfo& a, const ThreadInfo& b) {
            bool pa = isPrio(a), pb = isPrio(b);
            if (pa != pb) return pa;        // priority group first
            return rolePriority(a.role) < rolePriority(b.role);
        });
    if ((int)criticalThreads.size() > maxCount) {
        criticalThreads.resize(maxCount);
    }
    return criticalThreads;
}

int emergencyMigrateCriticalThreads() {
    // Cooldown check
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long lastMs = g_lastEmergencyMigrate.load(std::memory_order_relaxed);
    // P3: keep the difference in long long; casting to int could overflow/wrap
    // on a huge delta (e.g. after a long sleep or clock jump).
    long long elapsedMs = nowMs - lastMs;
    if (elapsedMs < kEmergencyCooldownMs) {
        return 0;  // In cooldown, skip
    }

    // P-01: per-minute ceiling. If we already moved kEmergencyPerMinute threads
    // in the last 60s, do not start another emergency sweep this time. This is the
    // outer bound that keeps the emergency path from becoming a migration storm
    // even when jank/thermal pressure stays elevated.
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

    // P-09: read the thermal placement scale once for this sweep. When it is
    // suppressed (<0.50, i.e. the system is hot), emergency moves must only pull
    // threads DOWN to cooler pools — never push them UP onto a hotter big core.
    // Without this, "hot -> emergency pushes thread to BIG -> hotter -> more jank"
    // kept the thermal loop oscillating.
    const float c3Scale = g_thermalPlacementScale.load(std::memory_order_relaxed);
    const bool thermalSuppressed = (c3Scale < 0.50f);

    int migrated = 0;
    // P0-5 fix: snapshot config ONCE per emergency call and pass to decidePlacement,
    // instead of decidePlacement re-snapshotting for every critical thread.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    // P2-3: use shared collectCriticalThreads helper (was inline loop)
    // limit max 3 threads per emergency call (was 10).
    std::vector<ThreadInfo> criticalThreads = collectCriticalThreads(3);
    for (const auto& ti : criticalThreads) {
        // emergency=true bypasses Safe Mode / thermal-emergency /
        // safety-latency / passive latches AND the gain-confidence / hysteresis
        // gates. It is now ALSO subject to the unified per-tid residency latch
        // (applyPlacement) and to the per-thread thermal-direction + reversal
        // guards below — so it can move a hot thread quickly, but cannot re-move
        // the SAME tid on a ~1s loop or push it back up while thermally suppressed.
        Decision d = decidePlacement(ti, cfgSnap, /*emergency=*/true);
        if (d.action != DecisionAction::MIGRATE) continue;

        // P-09 thermal-direction gate: only allow DOWNWARD/cooling moves while
        // thermally suppressed.
        int liveCpu = readThreadCurrentCpu(ti.pid, ti.tid);
        if (liveCpu < 0) liveCpu = ti.currentCpu;
        int fromPoolE = (int)getPoolForCpu(liveCpu);
        int toPoolE   = (int)getPoolForCpu(d.targetCpu);
        if (thermalSuppressed && toPoolE > fromPoolE) {
            // Would climb to a hotter pool while thermally suppressed: block.
            continue;
        }
        // P-01 reversal guard: within 5s, do not move the tid back to the pool
        // it was emergency-migrated AWAY from (A->B->A).
        {
            auto eit = g_lastEmergencyRec.find(ti.tid);
            if (eit != g_lastEmergencyRec.end()) {
                long long since = nowMs - eit->second.toMs;
                if (since < 5000 && toPoolE == eit->second.fromPool) {
                    continue;
                }
            }
        }

        // P0-1: pass the per-call cfgSnap (no internal re-snapshot).
        bool ok = applyPlacement(ti, d.targetCpu, "emergency_jank", cfgSnap, /*emergency=*/true);
        if (ok) {
            migrated++;
            std::lock_guard<std::mutex> wlk(g_emergWindowMutex);
            g_emergWindowCount++;
            // Record the emergency move for the reversal guard.
            g_lastEmergencyRec[ti.tid] = EmergencyRec{nowMs, fromPoolE, toPoolE};
            if (g_lastEmergencyRec.size() > 1024) {
                auto oldest = g_lastEmergencyRec.begin();
                for (auto e = g_lastEmergencyRec.begin(); e != g_lastEmergencyRec.end(); ++e)
                    if (e->second.toMs < oldest->second.toMs) oldest = e;
                g_lastEmergencyRec.erase(oldest);
            }
        }
    }
    // emergency_migrations is a CUMULATIVE counter. Previously this
    // wrote the per-call `migrated` value (overwriting the file), so a call that
    // migrated 0 threads wiped the running total -> the on-disk file always read
    // 0 even though the log reported successful emergency migrations. Now we
    // accumulate into an atomic total and persist that.
    if (migrated > 0) {
        g_emergencyMigrations.fetch_add(migrated, std::memory_order_relaxed);
        LOG_WARN("EMERGENCY MIGRATION (jank): migrated %d critical threads (total=%lld)",
                 migrated, (long long)g_emergencyMigrations.load());
    }
    writeFile((std::string(STATE_DIR) + "/emergency_migrations").c_str(),
              g_emergencyMigrations.load(std::memory_order_relaxed));
    return migrated;
}

// ============================================================================
// fix84 Feature 2: Frame-Synchronized Lightweight Migration
// Only migrates critical threads (UI/RENDER/GAME), max 1 per trigger (was 5, reduced to prevent overruns).
// Skips ML learning, dumpsys, thermal detection for minimal latency.
// ============================================================================
int lightweightMigrateCriticalThreads() {
    static std::atomic<long long> s_lastCallMs{0};
    auto now = std::chrono::steady_clock::now();
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Cooldown: min 8ms between calls (avoid redundant migrations)
    if (nowMs - s_lastCallMs.load() < 8) return 0;
    s_lastCallMs.store(nowMs);

    // snapshot config for the passive gate.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    if (cfgSnap.controlMode == "passive") return 0;

    int migrated = 0;
    // P3-7: use shared collectCriticalThreads helper
    std::vector<ThreadInfo> criticalThreads = collectCriticalThreads(1); // limit to 1 thread per frame-sync decision to prevent overruns

    // Process critical threads (order as scanned, no util field in ThreadInfo)
    for (const auto& ti : criticalThreads) {
        if (migrated >= 1) break;  // Max 1 per frame-sync decision (was 5, caused overruns)

        Decision d = decidePlacement(ti, cfgSnap);
        if (d.action == DecisionAction::MIGRATE && d.targetCpu >= 0) {
            // P0-1: pass the per-call cfgSnap.
            if (applyPlacement(ti, d.targetCpu, d.reason, cfgSnap)) {
                migrated++;
            }
        }
    }

    return migrated;
}

// ============================================================================
// fix84 Feature 3: Proactive Migration Check
// Predicts future CPU load and proactively migrates critical threads before
// their current CPU becomes overloaded. Requires high prediction confidence.
// ============================================================================
int proactiveMigrationCheck() {
    static std::atomic<long long> s_lastCheckMs{0};
    // D1 deadlock fix (v4.3.1h5): process-level re-entrancy guard. If a nested
    // RT-boost / foreground path on the SAME decision thread calls back into the
    // spillover entry while we are already inside it, do NOT re-enter (the entry
    // locks g_rtMutex/g_foregroundMutex non-recursively -> self-deadlock).
    static std::atomic<bool> s_inSpillover{false};
    bool expected = false;
    if (!s_inSpillover.compare_exchange_strong(expected, true)) return 0;
    struct Guard { ~Guard() { s_inSpillover.store(false); } } guard;
    auto now = std::chrono::steady_clock::now();
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Cooldown: 200ms between proactive migrations
    if (nowMs - s_lastCheckMs.load() < 200) return 0;
    s_lastCheckMs.store(nowMs);

    // Safe Mode suspends aggressive proactive migration.
    if (!aatpe::subsys::safemode::aggressiveMigrationAllowed()) return 0;

    // snapshot config for the passive gate.
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    // B3: PRIME spillover sweep runs in EVERY mode (passive uses its own
    // conservative matrix). No-op on devices without a PRIME cluster. This is
    // the production call site of aatpe_spillover::decidePromotions().
    aatpe_spillover::placementSpilloverTick(cfgSnap);
    if (cfgSnap.controlMode == "passive") return 0;

    int migrated = 0;
    // P3-7: use shared collectCriticalThreads helper
    std::vector<ThreadInfo> criticalThreads = collectCriticalThreads(3);

    // P2-4: Copy g_allCpus under shared_lock for concurrent-safe iteration
    std::vector<int> allCpusCopy;
    {
        std::lock_guard<std::mutex> rlock(g_topologyRwMutex);
        allCpusCopy = g_allCpus;
    }

    for (const auto& ti : criticalThreads) {
        if (migrated >= 3) break;  // Max 3 proactive migrations per check

        // R3-006: ti.currentCpu is a scan snapshot that can be seconds stale —
        // the overload decision must use the live CPU the thread is actually on.
        int currentCpu = readThreadCurrentCpu(ti.pid, ti.tid);
        if (currentCpu < 0) currentCpu = ti.currentCpu;  // fall back to snapshot
        if (currentCpu < 0) continue;

        // Predict future load of current CPU (multi-step, conservative max)
        float predictedCurrentLoad = predictCpuLoadMultiStep(currentCpu, 3);
        float confidence = getPredictionConfidence(currentCpu);

        // Only proactively migrate if:
        // 1. Current CPU predicted to exceed 85% in next 150ms
        // 2. Prediction confidence > 0.5
        if (predictedCurrentLoad < 85.0f || confidence < 0.5f) continue;

        // R3-006: a thread pinned by cpuset may not be allowed on every online
        // CPU. Snapshot its allowed mask once and skip disallowed targets (the
        // target loop below previously scored cpuset-excluded CPUs).
        cpu_set_t allowedCpus = getThreadAllowedCpus(ti.tid);

        // Find a target CPU with predicted low load
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

        // P2-14: Use extractMlFeatures to extract real features instead of
        // memset-to-zero. All-zero features produce meaningless migration gain
        // predictions, effectively disabling the ML gain check.
        ThreadFeatures features = extractMlFeatures(ti, bestTarget);
        int fromPool = (int)getPoolForCpu(currentCpu);
        int toPool = (int)getPoolForCpu(bestTarget);
        float mlGain = 0.0f;
        if (fromPool >= 0 && toPool >= 0 && fromPool != toPool) {
            std::lock_guard<std::mutex> mlLock(g_mlMutex);
            mlGain = g_mlModel.predictMigrationGain(fromPool, toPool, features);
        }

        // Only migrate if ML predicts positive gain (or model not trained yet)
        if (mlGain < 0.0f) continue;

        // Execute migration
        Decision d;
        d.action = DecisionAction::MIGRATE;
        d.targetCpu = bestTarget;
        d.reason = "PROACTIVE: predicted overload " + std::to_string((int)predictedCurrentLoad)
                 + "% on CPU" + std::to_string(currentCpu)
                 + " -> CPU" + std::to_string(bestTarget)
                 + " (conf=" + std::to_string(confidence).substr(0,3) + ")";
        // P0-1: pass the per-call cfgSnap.
        if (applyPlacement(ti, bestTarget, d.reason, cfgSnap)) {
            migrated++;
            // per-migration log downgraded INFO->DEBUG.
            // Proactive migrations can fire dozens/sec under game load; logging
            // each one at INFO would itself become a perf/log-volume problem.
            // The aggregate count is already surfaced as the
            // prediction_proactive_migrations state file below.
            LOG_DEBUG("PROACTIVE MIGRATION: tid=%d CPU%d->CPU%d predicted_load=%.0f%% confidence=%.2f",
                     ti.tid, currentCpu, bestTarget, predictedCurrentLoad, confidence);
        }
    }

    if (migrated > 0) {
        // P2-4: use cumulative counter instead of per-check count
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
// audit5 N3: expose the TID-reuse identity predicate (the exact condition the
// demote loop gates on) so host regression can drive the real function.
bool spillDemoteIdentityMatchForTest(int tid, long long recordedStarttime) {
    return aatpe_spillover::spillDemoteIdentityMatch(tid, recordedStarttime);
}
unsigned long long spillReadThreadStarttimeForTest(int tid) {
    unsigned long long v = 0;
    (void)aatpe_spillover::spillReadThreadStarttime(tid, &v);
    return v;
}

// ============================================================================
// Scheduling-stability regression seams (P-01/P-02/P-03).
// Expose the unified per-tid residency latch so host tests can drive it directly
// without a real sched_setaffinity:
//   - placementLatchQueryForTest: does the latch PERMIT a migration on tid now?
//   - placementLatchStampForTest: plant a latch entry at an arbitrary (uphill) time.
//   - placementLatchResetForTest: clear all latch entries between tests.
//   - placementLatchRejectCountForTest: how many migrations the latch blocked.
//   - placementLatchConstantsForTest: read the tunable windows the tests assert.
// ============================================================================
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

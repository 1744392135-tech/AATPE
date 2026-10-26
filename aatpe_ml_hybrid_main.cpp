// AATPE Hybrid Learning Module Implementation (core lifecycle)
// Phase-2 refactor: split from aatpe_ml_hybrid.cpp
#include "aatpe_ml_hybrid.h"
#include "aatped.h"  // LOG_INFO/LOG_DEBUG/LOG_WARN unified logging
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <unistd.h>
namespace aatpe_ml_hybrid {

HybridLearner::HybridLearner() {}
HybridLearner::~HybridLearner() {}

void HybridLearner::init(int numPools, int featureNumPools) {
    std::lock_guard<std::mutex> lock(mutex_);
    numPools_ = numPools;
    // Back-compat: if the caller did not supply the distinct-cluster count, fall
    // back to the slot count (previous behaviour). The daemon always supplies it.
    featureNumPools_ = (featureNumPools > 0) ? featureNumPools : numPools;
    poolDeltas_.clear(); poolDeltas_.resize(numPools);
    prevCpuRuntimeNs_.clear(); prevCpuRuntimeNs_.resize(0);  // P3-9: dynamic
    prevSnapshots_.clear();  // P2-11
    prevTickTimestampNs_ = 0;
    alpha_ = 0.0f;  // (问题20): start un-engaged, ramp via recomputeAlphaLocked()
    initialized_ = true;
    LOG_INFO("[ML-HYBRID] initialized with %d pools (feature-normalizer=%d)",
             numPools, featureNumPools_);
}

void HybridLearner::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled != enabled_) {
        enabled_ = enabled;
        LOG_INFO("[ML-HYBRID] %s", enabled ? "ENABLED" : "DISABLED");
        if (!enabled) prevSnapshots_.clear();
    }
}

// thread scheduling attributes (priority/nice/role) and the
// per-CPU thermal vector are now read from the real runtime state in aatped.cpp
// and carried into tick(); the previously hard-coded neutral constants
// (priority=0.5, nice=0.5, is_game=0.0, thermal=0.5, migration_count=0.0) are
// gone. migration_count is derived inside tick() by diffing the CPU a thread
// ran on between consecutive snapshots.
void HybridLearner::tick(const std::vector<ThreadCpuSnapshot>& threadSnapshots,
                          const std::vector<uint64_t>& cpuRuntimeNs, int cpuCount,
                          const std::vector<float>& cpuThermalNorm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !initialized_ || poolDeltas_.empty()) return;
    // F-002 (v4.3.4): mirror the main ML thermal pause gate. aatped_main latches
    // g_mlTrainingPausedByThermal under SEVERE thermal throttling (loopThermalTick);
    // LearningModel::update() then skips SGD (aatpe_ml.cpp SKIP_THERMAL). Previously
    // this hybrid tick() had no such gate, so it kept training on the thermally-
    // distorted runRatio signal (CPU busy-ness is inflated by throttle). Pause here
    // too; on resume the normal path resumes from existing prevSnapshots_ (one
    // averaged sample over the pause window, self-correcting next tick).
    if (aatpe_ml::g_mlTrainingPausedByThermal.load(std::memory_order_relaxed)) {
        return;
    }
    tickCount_++;

    struct ThreadDelta {
        int tid, cpu, pool;
        int priority, nice, role;
        bool migrated;
        uint64_t deltaRunNs,deltaTimeNs; float runRatio;
        uint64_t deltaSwitches;  // R005c: nrSwitches increment this interval (tid-reuse -> 0)
    };
    std::vector<ThreadDelta> deltas;
    deltas.reserve(threadSnapshots.size());
    uint64_t nowNs = 0;
    for (const auto& snap : threadSnapshots) {
        if (snap.timestamp > nowNs) nowNs = snap.timestamp;
        auto it = prevSnapshots_.find(snap.tid);
        if (it != prevSnapshots_.end()) {
            const auto& prev = it->second;
            uint64_t dr = (snap.runTimeNs >= prev.runTimeNs) ? (snap.runTimeNs - prev.runTimeNs) : 0;
            uint64_t dt = (snap.timestamp > prev.timestamp) ? (snap.timestamp - prev.timestamp) : 1;
            if (dr > 0 && dt > 0) {
                ThreadDelta td; td.tid=snap.tid; td.cpu=snap.cpu; td.pool=snap.pool;
                td.priority = snap.priority;
                td.nice     = snap.nice;
                td.role     = snap.role;
                // migration detection = CPU changed since last snapshot.
                td.migrated = (prev.cpu >= 0 && snap.cpu >= 0 && prev.cpu != snap.cpu);
                td.deltaRunNs=dr; td.deltaTimeNs=dt;
                td.runRatio=std::min(1.0f,(float)dr/(float)dt);
                td.deltaSwitches = (snap.nrSwitches >= prev.nrSwitches)
                                 ? (snap.nrSwitches - prev.nrSwitches) : 0ULL;
                deltas.push_back(td);
            }
        }
        prevSnapshots_[snap.tid] = snap;
    }

    // evict oldest by timestamp
    // LED-062 (R5-006): was O(n^2) — for each eviction it re-scanned the whole map
    // to find the minimum timestamp. Collect the (timestamp,tid) pairs once, sort
    // them, then erase the `toEvict` oldest entries: O(n log n) per eviction cycle.
    if (prevSnapshots_.size() > (size_t)kMaxTrackedThreads) {
        size_t toEvict = prevSnapshots_.size() - (size_t)kMaxTrackedThreads/2;
        std::vector<std::pair<uint64_t,int>> ordered;
        ordered.reserve(prevSnapshots_.size());
        for (const auto& kv : prevSnapshots_) ordered.push_back({kv.second.timestamp, kv.first});
        std::sort(ordered.begin(), ordered.end(),
                  [](const std::pair<uint64_t,int>& a, const std::pair<uint64_t,int>& b){ return a.first < b.first; });
        for (size_t e = 0; e < toEvict && e < ordered.size(); e++) {
            prevSnapshots_.erase(ordered[e].second);
        }
    }

    // actual timestamp delta; P3-9: dynamic resize
    if ((int)prevCpuRuntimeNs_.size() < cpuCount) prevCpuRuntimeNs_.resize(cpuCount, 0);
    uint64_t wallNs = 500000000ULL;
    if (prevTickTimestampNs_ > 0 && nowNs > prevTickTimestampNs_) wallNs = nowNs - prevTickTimestampNs_;
    prevTickTimestampNs_ = nowNs;
    std::vector<float> cpuUtilDelta(cpuCount, 0.0f);
    for (int cpu=0; cpu<cpuCount && cpu<(int)cpuRuntimeNs.size(); cpu++) {
        if (cpu < (int)prevCpuRuntimeNs_.size()) {
            uint64_t prev=prevCpuRuntimeNs_[cpu], cur=cpuRuntimeNs[cpu];
            if (cur>=prev && wallNs>0) cpuUtilDelta[cpu]=std::min(1.0f,(float)(cur-prev)/(float)wallNs);
            prevCpuRuntimeNs_[cpu]=cur;
        }
    }

    int updatesThisTick = 0;
    for (const auto& td : deltas) {
        if (td.cpu<0||td.cpu>=cpuCount) continue;
        // the pool is resolved UPSTREAM by aatped.cpp via
        // getPoolForCpu(), which uses the real g_littlePool/g_midPool/g_bigPool
        // topology (auto-detected or config-overridden). The old hard-coded
        // `cpu>=7 -> pool2, cpu>=4 -> pool1` fallback was wrong on every
        // non-8-core device; it is replaced by a safe default (pool 0) only
        // when the upstream resolver returned UNKNOWN (-1).
        int pool = td.pool;
        if (pool<0||pool>=numPools_) pool=0;
        if (pool>=numPools_) pool=numPools_-1;

        float features[15]={0};
        features[0]=1.0f;
        features[1]=std::min(1.0f,(float)td.deltaRunNs/1000.0f/50000.0f);
        // R005c: f[2] = switches RATE/s = deltaSwitches / (deltaTimeNs in sec),
        // normalized by kSwitchesPerSecScale. Matches inference placement.cpp
        // hf[2]=ti.switchesPerSec/kSwitchesPerSecScale (both derive a rate, not a
        // cumulative counter). A long-lived but quiet thread -> ~0; a chattery one -> 1.
        float swRate = (td.deltaTimeNs > 0)
            ? (float)td.deltaSwitches / ((float)td.deltaTimeNs / 1.0e9f) : 0.0f;
        features[2] = std::min(1.0f, std::max(0.0f, swRate / aatpe::hybrid_const::kSwitchesPerSecScale));
        features[3]=cpuUtilDelta[td.cpu];
        // G7 R004-B2 round2: G5 now wires REAL values at inference (placement.cpp
        // hf[4/5/7/10] built from ThreadFeatures). Training MUST reproduce the EXACT
        // normalization LearningModel::extractFeatures uses, otherwise train/serve
        // distributions diverge again. Constants mirrored from aatpe_ml.cpp
        // (kMaxPriority=99, kNiceMin=-20, kNiceRange=39, kMaxThermal=120,
        // kMaxMigrationCount=1000) — keep in sync with extractFeatures():
        //   f.priority      = 1 - priority/99
        //   f.nice         = 1 - (nice+20)/39
        //   f.thermal      = temp/120          (cpuThermalNorm is pre-normalized)
        //   f.migration    = migCount/1000     (training only knows bool migrated)
        features[4]=std::min(1.0f,std::max(0.0f, 1.0f - (float)td.priority/99.0f));
        features[5]=std::min(1.0f,std::max(0.0f, 1.0f - ((float)td.nice + 20.0f)/39.0f));
        // features[6] is_game    : ROLE_S3_GAME=3 or ROLE_S2_RENDER=2 -> 1.0
        features[6]=(td.role==3||td.role==2)?1.0f:0.0f;
        float therm=0.5f;
        if (td.cpu>=0 && td.cpu<(int)cpuThermalNorm.size()) therm=cpuThermalNorm[td.cpu];
        features[7]=std::min(1.0f,std::max(0.0f, therm));
        features[8] = poolFeatureForTier(pool, featureNumPools_);  // #12: tier-aware normalization
        // ML-03: train/serve skew on current_cpu_match. At inference
        // placement.cpp sets hf[9] = (liveCpu == candidateCpu) ? 1.0 : 0.0, so the
        // feature is 0 whenever we score a candidate the thread does NOT currently
        // run on — the common migration case. Training snapshots, however, are all
        // collected from the thread's ACTUAL cpu (pool = td.pool), so features[9]
        // was hard-wired to 1.0 and w9 never saw the 0 branch: inference on hf[9]=0
        // was pure extrapolation. Drop it to 0 on a deterministic (~1/4, tid+tick
        // mixed so a given thread sees both values over time) fraction of training
        // samples. No RNG needed (reproducible); weight_clamp=10 and the alpha ramp
        // bound any one-sided w9.
        features[9] = (((td.tid + tickCount_) & 3) == 0) ? 0.0f : 1.0f;
        features[10]=td.migrated?1.0f:0.0f;
        features[11]=features[1]*features[8];
        features[12]=features[1]*features[3];
        features[13]=features[8]*features[7];
        features[14]=features[1]*features[7];
        // target is now GOOD-NESS, not busy-ness.
        // low runRatio (lightly loaded) = positive sample. Previously target was
        // `td.runRatio`, so the model learned to predict "how busy the thread is"
        // while the fusion expected a "how good the placement is" signal.
        // G7 R004-B2 (L2): label = 1 - runRatio is a *current-busyness* proxy, not a
        // counter-factual placement-quality (jank/frame-time) label. The main ML model
        // already trains on measureOutcomeV2 (real p95 frame-time improvement); the
        // hybrid here keeps the proxy but its alpha is capped small so a weak label
        // cannot dominate. Real frame-time labels require the closed loop (真机).
        sgdUpdate(pool, features, 1.0f - td.runRatio);
        updatesThisTick++;
    }
    totalUpdates_ += updatesThisTick;
    recomputeAlphaLocked();  // (问题20): progress the alpha ramp
    if (tickCount_%20==0 && updatesThisTick>0)
        LOG_DEBUG("[ML-HYBRID] tick #%d: %d threads, %d updates, total=%d alpha=%.4f",
                  tickCount_, (int)deltas.size(), updatesThisTick, totalUpdates_, (double)alpha_);
}

void HybridLearner::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& d:poolDeltas_) { memset(d.weights,0,sizeof(d.weights)); d.updateCount=0; }
    totalUpdates_=0; tickCount_=0; prevSnapshots_.clear();
    alpha_=0.0f;  // (问题20): cold restart alpha
    LOG_INFO("[ML-HYBRID] reset all delta weights");
}

HybridPoolDelta HybridLearner::getPoolDelta(int pool) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool>=0&&pool<(int)poolDeltas_.size()) return poolDeltas_[pool];
    return HybridPoolDelta();
}

std::string HybridLearner::getStatusString() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char buf[256];
    snprintf(buf,sizeof(buf),"hybrid=%s pools=%d ticks=%d updates=%d alpha=%.4f",
             enabled_?"on":"off",(int)poolDeltas_.size(),tickCount_,totalUpdates_,(double)alpha_);
    return std::string(buf);
}

HybridLearner& instance() { static HybridLearner inst; return inst; }
} // namespace aatpe_ml_hybrid

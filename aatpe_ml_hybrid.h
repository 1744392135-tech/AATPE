// AATPE - Hybrid Learning Module
#ifndef AATPE_ML_HYBRID_H
#define AATPE_ML_HYBRID_H
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>
namespace aatpe_ml_hybrid {

// ThreadCpuSnapshot now carries the per-thread scheduling
// attributes that tick() needs to build a TRAINING feature vector that matches
// the serving-time vector built by aatped.cpp::hybridScore(). Previously these
// were hard-coded to neutral constants inside tick(), so the SGD weights for
// features[4..7,10] never learned any real signal.
//
// priority / nice / role are copied verbatim from g_threads[tid] at snapshot
// time (under g_threadsMutex in aatped.cpp). They are normalized inside tick().
// CPU temperature is per-CPU (not per-thread), so it is delivered separately
// as the cpuThermalNorm[] vector argument of tick().
struct ThreadCpuSnapshot {
    int tid = 0;
    int cpu = -1;
    int pool = -1;
    uint64_t runTimeNs = 0;
    uint64_t timestamp = 0;
    int priority = 0;     // raw ThreadInfo.priority (kDefaultPriority=50)
    int nice = 0;         // raw ThreadInfo.nice (range -20..19)
    int role = 0;        // raw ThreadRole enum (0=S0_SYS ... 3=S3_GAME, 2=S2_RENDER)
    // R005c: raw ThreadInfo.nrSwitches, copied verbatim from g_threads so the
    // training feature f[2] uses the EXACT same number placement.cpp feeds at
    // inference (hf[2]=min(1, nrSwitches/1000)). Previously training used
    // runRatio here -> train/serve skew on feature index 2.
    uint64_t nrSwitches = 0;
};

// Per-pool delta weights. updateCount doubles as the sample-count gate used by
// the cold-start protection (问题19) and the alpha ramp (问题20).
struct HybridPoolDelta {
    float weights[15] = {0};
    int updateCount = 0;
};

// #12: pool feature (features[8]) normalization by tier count.
//
// Legacy 3-tier devices (LITTLE/MID/BIG, numPools<=3) use the historical
// hard-coded mapping 0.3 / 0.6 / 1.0 for pool 0/1/2. These values MUST be
// preserved exactly so already-trained models on shipped devices keep their
// learned weights — the inference side (placement.cpp / aatped_main.cpp) still
// uses the old formula on 3-tier hardware, and changing the training distribution
// would cause a silent train/serve drift.
//
// For 4+ tier devices (LITTLE/MID/BIG/PRIME, numPools>=4) the PRIME pool (index
// numPools-1) must map to 1.0 (strongest core) and the LITTLE pool (index 0) to
// 0.0, with linear spacing. Previously pool=3 fell through to the else branch and
// got 0.3 — the same as the weakest core, inverting the feature.
//
// Choose pool/(tiers-1) over capacity/maxCapacity because:
//   1) it is deterministic and does not depend on cpu_capacity sysfs timing;
//   2) the existing hybrid delta model is linear in the feature, so linear
//      spacing matches the weight semantics;
//   3) it matches the "tier ordinal" semantics the rest of the code uses.
inline float poolFeatureForTier(int pool, int numPools) {
    if (numPools <= 3) {
        // Preserve historical values byte-for-byte.
        return (pool == 2) ? 1.0f : (pool == 1) ? 0.6f : 0.3f;
    }
    if (numPools <= 1) return 0.0f;
    // Clamp pool to valid range before division.
    if (pool < 0) pool = 0;
    if (pool >= numPools) pool = numPools - 1;
    return (float)pool / (float)(numPools - 1);
}

class HybridLearner {
public:
    HybridLearner();
    ~HybridLearner();
    void init(int numPools, int featureNumPools = -1);
    void setEnabled(bool enabled);
    bool isEnabled() const { std::lock_guard<std::mutex> lk(mutex_); return enabled_; }
    // cpuThermalNorm is indexed by cpu id and holds the
    // per-CPU temperature normalized to [0,1] (temp/120). It defaults to an
    // empty vector, in which case tick() uses a neutral 0.5f thermal feature
    // (keeps legacy host-side unit-test call sites compiling).
    void tick(const std::vector<ThreadCpuSnapshot>& threadSnapshots,
              const std::vector<uint64_t>& cpuRuntimeNs, int cpuCount,
              const std::vector<float>& cpuThermalNorm = std::vector<float>());
    float getFineTuneAdjustment(int pool, const float features[15]) const;
    float getFineTuneAdjustment(int pool, float avg_run_us, float nr_switches,
        float cpu_usage, float priority, float nice, float is_game, float thermal,
        float capacity, float current_cpu_match, float migration_count,
        float cross_run_cap, float cross_run_usage, float cross_cap_thermal,
        float cross_run_thermal) const;
    // HybridModel::predict() — returns normalized [0,1] score for decision fusion.
    // Wraps getFineTuneAdjustment ([-1,1]) and maps to [0,1] via (x+1)/2.
    // Returns 0.0f when disabled, uninitialized, cold-start, or alpha==0.
    //
    // the training target was changed from `runRatio`
    // (busy-ness) to `1.0f - runRatio` (good-ness: low busy-ness = good).
    // Therefore predict() now outputs a "goodness" score: high = the sampled
    // placement tends to be lightly loaded / healthy, low = heavily loaded.
    float predict(int pool, const float features[15]) const;
    float predict(int cpuId, int pool, float avg_run_us, float nr_switches,
        float cpu_usage, float priority, float nice, float is_game, float thermal,
        float capacity, float current_cpu_match, float migration_count,
        float cross_run_cap, float cross_run_usage, float cross_cap_thermal,
        float cross_run_thermal) const;
    // per-pool cold-start gate. A pool is "ready" only after it
    // has accumulated at least kHybridColdStartSamples SGD updates; until then
    // getFineTuneAdjustment()/predict() return 0 so the immature deltas cannot
    // perturb four-way decision fusion.
    bool isPoolReady(int pool) const;
    // progressive fusion weight. Starts at 0.0 and ramps
    // linearly to kHybridMaxAlpha once totalUpdates_ passes the cold-start
    // threshold (mirrors LearningModel::recomputeAllAlphas()). The returned
    // value is multiplied onto every fine-tune adjustment output.
    float getAlpha() const;
    bool save(const char* path) const;
    bool load(const char* path);
    void reset();
    bool getLoadedEnabled() const { std::lock_guard<std::mutex> lk(mutex_); return enabled_; }
    int getTotalUpdates() const { std::lock_guard<std::mutex> lk(mutex_); return totalUpdates_; }
    int getPoolCount() const { std::lock_guard<std::mutex> lk(mutex_); return (int)poolDeltas_.size(); }
    std::string getStatusString() const;
    HybridPoolDelta getPoolDelta(int pool) const;
private:
    mutable std::mutex mutex_;
    bool enabled_ = false;
    bool initialized_ = false;
    int numPools_ = 0;
    // v4.1.2-hotfix: distinct-physical-cluster count used ONLY for the hf[8]
    // feature normalizer. numPools_ stays = maxTier+1 (model slots); this separates
    // the sparse-tier D1100 case where slots=4 but real clusters=3.
    int featureNumPools_ = 0;
    std::vector<HybridPoolDelta> poolDeltas_;
    std::unordered_map<int, ThreadCpuSnapshot> prevSnapshots_;
    std::vector<uint64_t> prevCpuRuntimeNs_;
    uint64_t prevTickTimestampNs_ = 0;
    int totalUpdates_ = 0;
    int tickCount_ = 0;
    float alpha_ = 0.0f;          // (问题20): progressive fusion weight
    static constexpr float kLearningRate = 0.001f;
    static constexpr float kWeightClamp = 0.5f;
    static constexpr int kMaxTrackedThreads = 512;
    // cold-start gate + alpha ramp endpoints.
    static constexpr int kHybridColdStartSamples = 50;
    static constexpr float kHybridMaxAlpha = 0.1f;
    float dotProduct(const HybridPoolDelta& delta, const float features[15]) const;
    void sgdUpdate(int pool, const float features[15], float target);
    float clampWeight(float w) const;
    // recompute alpha_ from totalUpdates_ (caller holds mutex_).
    void recomputeAlphaLocked();
};

HybridLearner& instance();
} // namespace aatpe_ml_hybrid
#endif

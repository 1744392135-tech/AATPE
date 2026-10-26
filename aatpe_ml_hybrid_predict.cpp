// AATPE Hybrid Learning Module Implementation (prediction logic)
// Phase-2 refactor: split from aatpe_ml_hybrid.cpp
#include "aatpe_ml_hybrid.h"
#include <cmath>
#include <algorithm>
namespace aatpe_ml_hybrid {

float HybridLearner::getFineTuneAdjustment(int pool, const float features[15]) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_||!initialized_||pool<0||pool>=(int)poolDeltas_.size()) return 0.0f;
    // cold-start gate. An immature pool has not seen enough
    // samples for its delta weights to be trustworthy; force neutral output so
    // it cannot perturb four-way decision fusion.
    if (poolDeltas_[pool].updateCount < kHybridColdStartSamples) return 0.0f;
    float dot = dotProduct(poolDeltas_[pool], features);
    // P3-29: guard against NaN/Inf dot product feeding exp() -> UB.
    if (!std::isfinite(dot)) return 0.0f;
    // Clamp dot to avoid exp overflow for large |dot|.
    if (dot > 20.0f) dot = 20.0f;
    if (dot < -20.0f) dot = -20.0f;
    float sig = 1.0f/(1.0f+std::exp(-dot));
    float adj = (sig - 0.5f) * 2.0f;
    // progressive alpha fusion. Starts at 0 (cold) and ramps
    // linearly to kHybridMaxAlpha so a newly-learned delta cannot shock the
    // scheduler. The multiplication lives here so predict()/hybridScore() and
    // the placement.cpp getFineTuneAdjustment() call site both see the same
    // alpha-weighted value.
    return adj * alpha_;
}

float HybridLearner::getFineTuneAdjustment(int pool, float avg_run_us, float nr_switches,
    float cpu_usage, float priority, float nice, float is_game, float thermal,
    float capacity, float current_cpu_match, float migration_count,
    float cross_run_cap, float cross_run_usage, float cross_cap_thermal,
    float cross_run_thermal) const {
    float f[15];
    f[0]=1.0f; f[1]=avg_run_us; f[2]=nr_switches; f[3]=cpu_usage; f[4]=priority;
    f[5]=nice; f[6]=is_game; f[7]=thermal; f[8]=capacity; f[9]=current_cpu_match;
    f[10]=migration_count; f[11]=cross_run_cap; f[12]=cross_run_usage;
    f[13]=cross_cap_thermal; f[14]=cross_run_thermal;
    return getFineTuneAdjustment(pool, f);
}

// HybridModel::predict() — normalized [0,1] score for four-way decision fusion.
// output semantics are GOOD-NESS (high = lightly loaded / good
// placement). When cold-started or alpha==0 this returns the neutral 0.5 after
// the (adj==0) -> (0+1)/2 mapping; see getFineTuneAdjustment().
float HybridLearner::predict(int pool, const float features[15]) const {
    float adj = getFineTuneAdjustment(pool, features);  // [-1, 1]
    // Map [-1,1] -> [0,1]: 0.0 means strong negative, 1.0 means strong positive
    return (adj + 1.0f) * 0.5f;
}

float HybridLearner::predict(int cpuId, int pool, float avg_run_us, float nr_switches,
    float cpu_usage, float priority, float nice, float is_game, float thermal,
    float capacity, float current_cpu_match, float migration_count,
    float cross_run_cap, float cross_run_usage, float cross_cap_thermal,
    float cross_run_thermal) const {
    (void)cpuId;
    float f[15];
    f[0]=1.0f; f[1]=avg_run_us; f[2]=nr_switches; f[3]=cpu_usage; f[4]=priority;
    f[5]=nice; f[6]=is_game; f[7]=thermal; f[8]=capacity; f[9]=current_cpu_match;
    f[10]=migration_count; f[11]=cross_run_cap; f[12]=cross_run_usage;
    f[13]=cross_cap_thermal; f[14]=cross_run_thermal;
    return predict(pool, f);
}

// per-pool cold-start readiness.
bool HybridLearner::isPoolReady(int pool) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool<0||pool>=(int)poolDeltas_.size()) return false;
    return poolDeltas_[pool].updateCount >= kHybridColdStartSamples;
}

} // namespace aatpe_ml_hybrid

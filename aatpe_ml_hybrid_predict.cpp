#include "aatpe_ml_hybrid.h"
#include <cmath>
#include <algorithm>
namespace aatpe_ml_hybrid {

float HybridLearner::getFineTuneAdjustment(int pool, const float features[15]) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_||!initialized_||pool<0||pool>=(int)poolDeltas_.size()) return 0.0f;
    if (poolDeltas_[pool].updateCount < kHybridColdStartSamples) return 0.0f;
    float dot = dotProduct(poolDeltas_[pool], features);
    if (!std::isfinite(dot)) return 0.0f;
    if (dot > 20.0f) dot = 20.0f;
    if (dot < -20.0f) dot = -20.0f;
    float sig = 1.0f/(1.0f+std::exp(-dot));
    float adj = (sig - 0.5f) * 2.0f;
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

float HybridLearner::predict(int pool, const float features[15]) const {
    float adj = getFineTuneAdjustment(pool, features);
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

bool HybridLearner::isPoolReady(int pool) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pool<0||pool>=(int)poolDeltas_.size()) return false;
    return poolDeltas_[pool].updateCount >= kHybridColdStartSamples;
}

}

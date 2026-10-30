#ifndef AATPE_ML_HYBRID_H
#define AATPE_ML_HYBRID_H
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>
namespace aatpe_ml_hybrid {

struct ThreadCpuSnapshot {
    int tid = 0;
    int cpu = -1;
    int pool = -1;
    uint64_t runTimeNs = 0;
    uint64_t timestamp = 0;
    int priority = 0;
    int nice = 0;
    int role = 0;
    uint64_t nrSwitches = 0;
};

struct HybridPoolDelta {
    float weights[15] = {0};
    int updateCount = 0;
};

inline float poolFeatureForTier(int pool, int numPools) {
    if (numPools <= 3) {
        return (pool == 2) ? 1.0f : (pool == 1) ? 0.6f : 0.3f;
    }
    if (numPools <= 1) return 0.0f;
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
    void tick(const std::vector<ThreadCpuSnapshot>& threadSnapshots,
              const std::vector<uint64_t>& cpuRuntimeNs, int cpuCount,
              const std::vector<float>& cpuThermalNorm = std::vector<float>());
    float getFineTuneAdjustment(int pool, const float features[15]) const;
    float getFineTuneAdjustment(int pool, float avg_run_us, float nr_switches,
        float cpu_usage, float priority, float nice, float is_game, float thermal,
        float capacity, float current_cpu_match, float migration_count,
        float cross_run_cap, float cross_run_usage, float cross_cap_thermal,
        float cross_run_thermal) const;
    float predict(int pool, const float features[15]) const;
    float predict(int cpuId, int pool, float avg_run_us, float nr_switches,
        float cpu_usage, float priority, float nice, float is_game, float thermal,
        float capacity, float current_cpu_match, float migration_count,
        float cross_run_cap, float cross_run_usage, float cross_cap_thermal,
        float cross_run_thermal) const;
    bool isPoolReady(int pool) const;
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
    int featureNumPools_ = 0;
    std::vector<HybridPoolDelta> poolDeltas_;
    std::unordered_map<int, ThreadCpuSnapshot> prevSnapshots_;
    std::vector<uint64_t> prevCpuRuntimeNs_;
    uint64_t prevTickTimestampNs_ = 0;
    int totalUpdates_ = 0;
    int tickCount_ = 0;
    float alpha_ = 0.0f;
    static constexpr float kLearningRate = 0.001f;
    static constexpr float kWeightClamp = 0.5f;
    static constexpr int kMaxTrackedThreads = 512;
    static constexpr int kHybridColdStartSamples = 50;
    static constexpr float kHybridMaxAlpha = 0.1f;
    float dotProduct(const HybridPoolDelta& delta, const float features[15]) const;
    void sgdUpdate(int pool, const float features[15], float target);
    float clampWeight(float w) const;
    void recomputeAlphaLocked();
};

HybridLearner& instance();
}
#endif

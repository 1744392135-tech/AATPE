#ifndef AATPE_ML_H
#define AATPE_ML_H
#include <vector>
#include <string>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <atomic>
#include <array>
#include <ctime>
namespace aatpe_ml {

static constexpr int ML_MODEL_VERSION = 6;
static constexpr int ML_FEATURE_COUNT = 10;
static constexpr int ML_CROSS_FEATURE_COUNT = 4;
static constexpr int ML_WEIGHT_COUNT = 1 + ML_FEATURE_COUNT + ML_CROSS_FEATURE_COUNT;
static constexpr int MIN_MEASURE_INTERVAL_MS = 500;
extern std::atomic<long long> g_pendingSkippedExisting;
extern std::atomic<long long> g_invalidOutcomeSkipped;
extern std::atomic<long long> g_droppedInvalidTargetPool;
extern std::atomic<long long> g_loadedWeightDegraded;
enum class UpdateResult : int {
    TRAINED = 0,
    SKIP_THERMAL = 1,
    SKIP_DISABLED_ANOMALY = 2,
    SKIP_POOL = 3,
    SKIP_INVALID_OUTCOME = 4,
};
extern std::atomic<long long> g_mlUpdateSkipThermal;
extern std::atomic<long long> g_mlUpdateSkipDisabled;

extern std::atomic<bool> g_mlTrainingPausedByThermal;
extern std::atomic<bool> g_mlFrameOutcomeSuppressedByThermal;
extern std::atomic<long long> g_mlExploreSkippedTotal;

extern std::atomic<long long> g_mlShadowSamplesCollected;
extern std::atomic<long long> g_mlShadowSamplesDropped;

struct ThreadFeatures {
    float avg_run_us = 0.0f;
    float nr_switches = 0.0f;
    float cpu_usage = 0.0f;
    float priority = 0.0f;
    float nice = 0.0f;
    float is_game = 0.0f;
    float thermal = 0.0f;
    float capacity = 0.0f;
    float current_cpu_match = 0.0f;
    float migration_count = 0.0f;
    float cross_run_cap = 0.0f;
    float cross_run_usage = 0.0f;
    float cross_cap_thermal = 0.0f;
    float cross_run_thermal = 0.0f;
};

struct PoolModel {
    float weights[ML_WEIGHT_COUNT];
    float learning_rate;
    int sample_count;
    float alpha = 0.0f;
};
using CpuModel = PoolModel;

struct PendingDecision {
    int tid;
    int target_cpu;
    int target_pool;
    ThreadFeatures features;
    unsigned long long before_run_time;
    unsigned long long before_nr_switches;
    int before_cpu_usage;
    int before_freq;
    time_t before_timestamp;
    unsigned long long before_mono_ns;
    time_t timestamp;
    bool active;
    unsigned long long starttime;
    unsigned long long create_time_ms;
    float frame_time_before_p95 = 0.0f;
    float frame_time_before_avg = 0.0f;
    int   frame_time_before_samples = 0;
    unsigned long long last_attempt_ms = 0;
    int consecutive_sleeps = 0;
    bool counterfactualControl = false;
    bool shadow = false;
};

struct BaselineEntry {
    float run_ratio;
    time_t timestamp;
};

class LearningModel {
public:
    LearningModel();
    ~LearningModel();
    void init(int num_pools);
    float predict(int pool, const ThreadFeatures& features) const;
    float predictMigrationGain(int fromPool, int toPool, const ThreadFeatures& features) const;
    UpdateResult update(int pool, const ThreadFeatures& features, float outcome);
    bool save(const char* path) const;
    bool load(const char* path);
    bool validateParameters() const;
    float getAlpha() const;
    bool isMature() const;
    void decayLearningRate();
    void reset();
    void migratePoolCount(int new_count);
    int getTotalSamples() const;
    std::vector<float> getWeights() const;
    void setWeights(const std::vector<float>& w);
    void setWeights(int poolIdx, const std::vector<float>& w);
    int getPoolCount() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return (int)models.size(); }
    class BatchLock {
    public:
        explicit BatchLock(LearningModel& m) : model_(m) { model_.mutex_.lock(); }
        ~BatchLock() { model_.mutex_.unlock(); }
        BatchLock(const BatchLock&) = delete;
        BatchLock& operator=(const BatchLock&) = delete;
    private:
        LearningModel& model_;
    };
    float predictUnlocked(int pool, const ThreadFeatures& features) const;
    int getPoolSampleCount(int poolIdx) const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (poolIdx < 0 || poolIdx >= (int)models.size()) return 0;
        return models[poolIdx].sample_count;
    }
    PoolModel getPoolModel(int pool) const;
    CpuModel getCpuModel(int cpu) const;
#ifdef AATPE_HOST_TEST
    void aatpeTestCorruptPoolWeights(int pool);
#endif
    bool isValidPrediction(float score) const;
    static bool isAllZeroFeatures(const ThreadFeatures& f);
    void setMaxAlpha(float v) { std::lock_guard<std::recursive_mutex> lk(mutex_); max_alpha = v; }
    void setInitialLR(float v) { std::lock_guard<std::recursive_mutex> lk(mutex_); initial_lr = v; }
    void setDecay(float v) { std::lock_guard<std::recursive_mutex> lk(mutex_); lr_decay = v; }
    void setWeightClamp(float v) { std::lock_guard<std::recursive_mutex> lk(mutex_); weight_clamp = v; }
    void setL2Lambda(float v) { std::lock_guard<std::recursive_mutex> lk(mutex_); l2_lambda = v; }
    void setColdStartSamples(int v) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        cold_start_samples = (v >= 0 && v <= 100000) ? v : 50;
    }
    bool isPoolReady(int poolIdx) const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (poolIdx < 0 || poolIdx >= (int)models.size()) return false;
        return models[poolIdx].sample_count >= min_samples_threshold;
    }
    bool isPoolPresent(int poolIdx) const;
    int getMinSamples() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return min_samples_threshold; }
    void setMinSamples(int n) { std::lock_guard<std::recursive_mutex> lk(mutex_); min_samples_threshold = (n > 0) ? n : 1; }
    void setEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); enabled = v; }
    bool isEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return enabled; }

    void setMadFilterEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); madFilterEnabled_ = v; }
    bool isMadFilterEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return madFilterEnabled_; }
    float computeOutlierWeightForPool(int pool, float outcome);

    void setDriftRelearnEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); driftRelearnEnabled_ = v; }
    bool isDriftRelearnEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return driftRelearnEnabled_; }
    void nudgeTowardNeutral(int pool);
    int getDriftRingSize(int pool) const;

    void setEpsilonGreedy(double v) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (v < 0.0) v = 0.0;
        epsilonGreedy_ = v;
    }
    double getEpsilonGreedy() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return epsilonGreedy_; }
    void setExploreJankBlocked(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); exploreJankBlocked_ = v; }
    bool maybeExplore(int tid, float predicted);
    static long long getExploreSkippedTotal() { return g_mlExploreSkippedTotal.load(std::memory_order_relaxed); }
    static float counterfactualControlLabel(float outcome) {
        if (outcome < 0.0f) return outcome;
        return 1.0f - outcome;
    }
    void setCpuPoolAssignment(const std::vector<int>& a) { std::lock_guard<std::recursive_mutex> lk(mutex_); m_cpuPoolAssignment = a; }
    std::vector<int> getCpuPoolAssignment() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_cpuPoolAssignment; }
    void setTopologyFingerprint(const std::string& fp) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (!m_topologyFingerprint.empty() && m_topologyFingerprint != fp) m_modelStale = true;
        m_topologyFingerprint = fp;
    }
    std::string getTopologyFingerprint() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_topologyFingerprint; }
    bool isModelStale() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_modelStale; }
    void markModelStale() { std::lock_guard<std::recursive_mutex> lk(mutex_); m_modelStale = true; }
    uint32_t getGeneration() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_generation; }
    bool isAnomalyLatched() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_anomalyLatched; }
    float getAlphaForPool(int pool) const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (!enabled || pool < 0 || pool >= (int)models.size()) return 0.0f;
        return models[pool].alpha;
    }
    float getMaxAlpha() const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        return max_alpha > 0.0f ? max_alpha : 1.0f;
    }
    void recomputeAllAlphas();
    void recordPendingDecision(int tid, int target_cpu, int target_pool,
                                const ThreadFeatures& features,
                                unsigned long long before_run_time,
                                unsigned long long before_nr_switches,
                                int before_cpu_usage,
                                int before_freq = 0,
                                time_t before_timestamp = 0,
                                unsigned long long starttime = 0,
                                float frame_time_before_p95 = 0.0f,
                                float frame_time_before_avg = 0.0f,
                                int frame_time_before_samples = 0);
    void recordPendingDecision(int tid, int target_cpu,
                                const ThreadFeatures& features,
                                unsigned long long before_run_time,
                                unsigned long long before_nr_switches,
                                int before_cpu_usage,
                                int before_freq = 0,
                                time_t before_timestamp = 0,
                                unsigned long long starttime = 0,
                                float frame_time_before_p95 = 0.0f,
                                float frame_time_before_avg = 0.0f,
                                int frame_time_before_samples = 0);
    int findPendingDecision(int tid);
    bool getPendingDecision(int idx, PendingDecision& out) const;
    std::vector<PendingDecision> getAllPendingDecisions() const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        std::vector<PendingDecision> out;
        out.reserve(pending_decisions.size());
        for (const auto& pd : pending_decisions) {
            if (pd.active) out.push_back(pd);
        }
        return out;
    }
    void clearPendingDecision(int tid);
    void markPendingCounterfactual(int tid);
    void markPendingShadow(int tid);
    void setShadowSamplingEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); shadowSamplingEnabled_ = v; }
    bool isShadowSamplingEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return shadowSamplingEnabled_; }
    void recordShadowOutcome(int pool, const ThreadFeatures& features, float outcome);
    long long getShadowSamplesCollected() const { return g_mlShadowSamplesCollected.load(std::memory_order_relaxed); }
    long long getShadowSamplesDropped() const { return g_mlShadowSamplesDropped.load(std::memory_order_relaxed); }
    size_t getShadowRingSize() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return shadowRing_.size(); }
    struct ShadowSampleView {
        int pool = 0;
        ThreadFeatures features;
        float outcome = 0.0f;
    };
    std::vector<ShadowSampleView> drainShadowSamples(size_t maxN, bool clear = true);
    int clearExpiredPendingDecisions(int max_age_sec);
    void setPendingSleepBackoff(int tid, int consecutive_sleeps, unsigned long long last_attempt_ms);
    void recordBaseline(int tid, float run_ratio);
    float getBaselineRunRatio(int tid) const;
    void cleanExpiredBaselines(int max_age_sec);
    float recordBaselineDuty(int tid, unsigned long long runTime,
                             unsigned long long nowNs, int curFreq, int maxFreq);
    static float measureOutcomeV2(unsigned long long before_run_time,
        unsigned long long after_run_time, time_t before_ts, time_t after_ts,
        int before_freq, int after_freq, int base_freq, int loadInt, float baseline_run_ratio,
        float frame_time_before_p95 = 0.0f, float frame_time_after_p95 = 0.0f,
        int frame_time_sample_count = 0, float frame_time_weight = 0.0f,
        bool is_ui_render_thread = false,
        unsigned long long before_monotonic_ns = 0,
        unsigned long long after_monotonic_ns = 0);
    static float measureOutcome(unsigned long long before_run_time,
        unsigned long long after_run_time, unsigned long long before_switches,
        unsigned long long after_switches, int before_cpu_usage, int after_cpu_usage);
    static ThreadFeatures extractFeatures(unsigned long long avg_run_us_raw,
        unsigned long long nr_switches_raw, int cpu_usage_pct, int priority_raw,
        int nice_raw, bool is_game, int thermal_celsius, int target_cpu_capacity,
        bool current_cpu_match, unsigned long long migration_count_raw);
    std::string getStatusString() const;
    static std::atomic<long long> g_saturatedSamplesRetained;

private:
    mutable std::recursive_mutex mutex_;
    std::vector<PoolModel> models;
    int total_samples;
    float alpha;
    float max_alpha;
    float initial_lr;
    float lr_decay;
    float weight_clamp;
    float l2_lambda;
    int cold_start_samples;
    int min_samples_threshold;
    mutable bool enabled;
    mutable uint32_t m_generation = 0;
    mutable bool m_anomalyLatched = false;
    std::vector<int> m_cpuPoolAssignment;
    bool poolHasCpusUnlocked(int poolIdx) const;
    std::string m_topologyFingerprint;
    bool m_modelStale = false;
    bool m_modelLoaded = false;
    bool madFilterEnabled_ = false;
    bool driftRelearnEnabled_ = false;
    bool exploreJankBlocked_ = false;
    double epsilonGreedy_ = 0.0;
    std::vector<std::deque<float>> outcomeMadWindow_;
    std::vector<std::deque<float>> outcomeRing_;
    std::vector<float> stOutcomeEma_;
    uint64_t exploreTick_ = 0;
    static constexpr int   kMadWindow      = 100;
    static constexpr int   kMadMinSamples  = 10;
    static constexpr float  kMadK           = 3.0f;
    static constexpr float  kMadDownweight  = 0.2f;
    static constexpr int   kDriftRingW     = 200;
    static constexpr float  kNudgeAlpha     = 0.5f;
    static constexpr float  kExploreMargin  = 0.15f;
    struct ShadowSample { int pool; ThreadFeatures features; float outcome; };
    std::deque<ShadowSample> shadowRing_;
    bool shadowSamplingEnabled_ = true;
    static constexpr size_t kMaxShadowSamples = 512;
    void syncRuntimePoolStateLocked();
    float applyMadFilterLocked(int pool, float outcome);
    std::vector<PendingDecision> pending_decisions;
    std::map<int, std::deque<BaselineEntry>> baseline_windows;
    struct BaselineRef {
        unsigned long long runTime = 0;
        unsigned long long ns = 0;
        int freq = 0;
    };
    std::map<int, BaselineRef> baseline_refs_;
    static constexpr int BASELINE_WINDOW_SIZE = 3;
    static constexpr int MAX_PENDING_DECISIONS = 1024;
    static constexpr int MAX_BASELINE_THREADS = 512;
    float clampWeight(float w) const;
    float dotProduct(const PoolModel& model, const ThreadFeatures& features) const;
    static float sigmoid(float x);
};

struct FrameTimeOutcome {
    float frame_time_avg = 0.0f;
    float frame_time_p95 = 0.0f;
    float frame_time_p99 = 0.0f;
    int   jank_count = 0;
    int   sample_count = 0;
    bool  valid = false;
    time_t timestamp = 0;
};

typedef int (*SFLatencyCallback)(int refreshRate, int& outMaxLat, bool& outValid);
typedef std::string (*ForegroundAppCallback)();
extern SFLatencyCallback g_sfLatencyCallback;
extern ForegroundAppCallback g_foregroundAppCallback;

class GfxInfoFrameTimeProvider {
public:
    GfxInfoFrameTimeProvider();
    ~GfxInfoFrameTimeProvider();
    FrameTimeOutcome getSnapshot();
    FrameTimeOutcome refresh();
    bool isAvailable() const { return available_; }
    void setCacheTtlSec(int sec) { cache_ttl_sec_ = (sec > 0) ? sec : 2; }
#ifdef AATPE_HOST_TEST
    FrameTimeOutcome parseGfxinfoForTest(const std::string& out) { return parseGfxinfo(out); }
#endif
private:
    FrameTimeOutcome parseGfxinfo(const std::string& output);
    std::string executeCommand(const std::string& cmd);
    FrameTimeOutcome cached_;
    time_t cache_time_;
    int cache_ttl_sec_;
    bool available_;
};

class SurfaceFlingerLatencyProvider {
public:
    SurfaceFlingerLatencyProvider();
    ~SurfaceFlingerLatencyProvider();
    int getLatencyUs(int refreshRate, int& outMaxLat, bool& outValid);
    bool isAvailable() const { return available_; }
private:
    int parseSFLatency(const std::string& output, int& outMaxLat);
    std::string executeCommand(const std::string& cmd);
    bool available_;
};

extern std::atomic<long long> g_ftOutcomeSamples;
extern std::atomic<long long> g_ftOutcomeFallback;
extern std::atomic<long long> g_ftOutcomeImproved;
extern std::atomic<long long> g_ftOutcomeDegraded;
extern std::atomic<long long> g_ftGfxinfoCalls;
extern std::atomic<long long> g_ftGfxinfoCacheHits;

class DriftDetector {
public:
    DriftDetector();
    void recordOutcome(float outcome, int recommendedPool);
    bool isDriftDetected() const { return driftDetected_.load(std::memory_order_relaxed); }
    void reset();
    void setWindow(int n) { window_ = (n >= 3) ? n : 3; }
    long long getSamples() const { return samples_.load(std::memory_order_relaxed); }
    long long getOutOfRangePoolSamples() const {
        return outOfRangePoolSamples_.load(std::memory_order_relaxed);
    }
    long long getConsecutiveDegrades() const {
        long long mx = 0;
        for (const auto& c : consecDegrades_) {
            long long v = c.load(std::memory_order_relaxed);
            if (v > mx) mx = v;
        }
        return mx;
    }
private:
    static constexpr int kMaxPools = 8;
    std::atomic<bool> driftDetected_{false};
    int  window_ = 10;
    std::array<std::atomic<long long>, kMaxPools> consecDegrades_{};
    std::atomic<long long> samples_{0};
    std::atomic<long long> outOfRangePoolSamples_{0};
};
extern std::atomic<bool> g_mlDriftDetected;

std::string computeDeviceTopologyHash();
bool loadTopologySidecar(const char* modelPath, const std::string& currentHash);
bool saveTopologySidecar(const char* modelPath, const std::string& currentHash);

float rolePriorShrink(int ownSamples);

void boostRoiRecord(bool charging, float jankDelta, int dischargeMwDelta);
float boostRoiRecentNetMean();

}
#endif

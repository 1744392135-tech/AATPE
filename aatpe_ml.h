// AATPE - Adaptive Android Task Placement Engine
// Machine Learning Online Learning Closed-Loop System
// Per-Pool Logistic Regression + SGD Online Learning (changed from per-CPU)
// Version: (see ./VERSION)
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

// v6 (P3-43/P3-44): model file now self-describes its provenance. After the
// per-pool weight block and BEFORE the trailing CRC it additionally carries:
//   uint32 generation      — monotonically bumped each save (protects against
//                            a stale/older process clobbering a newer on-disk model)
//   uint64 saveTimestamp   — wall-clock seconds at save time
//   uint64 topologyHash   — FNV-1a of computeDeviceTopologyHash(); on load a
//                            mismatch means the weights were trained on a
//                            different CPU topology (core count / little-mid-big
//                            reshuffle / ROM) and MUST NOT be reused -> cold start.
// The trailing CRC32 covers all of the above, so a flipped generation/hash byte
// is rejected as a CRC error (P3-45). v1..v5 files still load for back-compat;
// only v6 files carry the embedded provenance block.
static constexpr int ML_MODEL_VERSION = 6;
static constexpr int ML_FEATURE_COUNT = 10;
static constexpr int ML_CROSS_FEATURE_COUNT = 4;
static constexpr int ML_WEIGHT_COUNT = 1 + ML_FEATURE_COUNT + ML_CROSS_FEATURE_COUNT;
// Minimum measurement interval. Skip outcome measurement until 500ms after migration.
static constexpr int MIN_MEASURE_INTERVAL_MS = 500;
// Diagnostic counter for pending overwrite protection.
extern std::atomic<long long> g_pendingSkippedExisting;
// M-23: counts outcome measurements returned as -1 (invalid: bad timestamps,
// non-monotonic run-time, zero delta) that update() now REJECTS instead of
// silently clamping to 0. Surfaced on the WebUI so callers that forget to
// filter negative outcomes are observable.
extern std::atomic<long long> g_invalidOutcomeSkipped;
// ROUND-014 P3: pending sample whose target_pool vanished after a shrink.
extern std::atomic<long long> g_droppedInvalidTargetPool;
// F-6: loaded weights reset (tampered/NaN file).
extern std::atomic<long long> g_loadedWeightDegraded;
// v4.2.1 T1-fix: update() now returns WHY it did/did not train, so the caller
// can keep the funnel honest: "outcome_valid" must mean "TRAINED", not
// "outcome was finite but rejected by an internal gate".
enum class UpdateResult : int {
    TRAINED = 0,            // SGD step executed, sample_count/total_samples incremented
    SKIP_THERMAL = 1,       // g_mlTrainingPausedByThermal latched
    SKIP_DISABLED_ANOMALY = 2, // !enabled (setEnabled(false) or NaN/anomaly latch)
    SKIP_POOL = 3,          // pool out of range (already counted by g_droppedInvalidTargetPool)
    SKIP_INVALID_OUTCOME = 4, // outcome < 0 sentinel (already counted by g_invalidOutcomeSkipped)
};
// New funnel counters for the two internal gates that previously returned
// silently (thermal pause / disabled-anomaly). Pool and negative-outcome skips
// already have dedicated counters (g_droppedInvalidTargetPool /
// g_invalidOutcomeSkipped); we alias them here for a uniform funnel readout.
extern std::atomic<long long> g_mlUpdateSkipThermal;
extern std::atomic<long long> g_mlUpdateSkipDisabled;

// F-10: set true by main during thermal throttling -> update() skips learning.
extern std::atomic<bool> g_mlTrainingPausedByThermal;
// v4.1.2-hotfix: true only under MODERATE(2) throttling -> suppress the
// frame-time outcome fusion while still training the schedstat classic path.
extern std::atomic<bool> g_mlFrameOutcomeSuppressedByThermal;
// v4.2 Batch B3 (F-1 epsilon-greedy exploration): cumulative count of explore
// attempts that were SUPPRESSED by a hard gate (thermal >= MODERATE, or a jank
// active window). Written (uint text) to STATE_DIR/ml_explore_skipped_total so
// the WebUI/doctor can see the exploration safety budget being spent.
extern std::atomic<long long> g_mlExploreSkippedTotal;

// v4.5.0 shadow sampling (observe-only / passive mode). Shadow samples are
// (features -> measured outcome) pairs recorded WITHOUT applying any scheduling
// action and WITHOUT training the live model. They live in a separate bounded
// ring so offline effect-validation can run while the daemon stays read-only.
//   g_mlShadowSamplesCollected: cumulative accepted shadow samples (ever).
//   g_mlShadowSamplesDropped:   shadow outcomes rejected (invalid sentinel /
//                               shadow switch off). Surfaced to state/log.
extern std::atomic<long long> g_mlShadowSamplesCollected;
extern std::atomic<long long> g_mlShadowSamplesDropped;

struct ThreadFeatures {
    // v4.3.4 P1: default-initialize all 14 floats to 0.0f. placement.cpp KEEP /
    // PROTECTED / KERNEL_THREAD early-return paths produce a Decision whose
    // mlFeatures is value-initialized; without these defaults the floats were
    // indeterminate (UB if ever read). Real feature extraction overwrites every
    // field, so learning values are unchanged.
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
    unsigned long long before_mono_ns;  // P1 round7: monotonic ns start for delta
    time_t timestamp;
    bool active;
    unsigned long long starttime;
    unsigned long long create_time_ms;  // steady_clock ms at pending creation
    // frame-time snapshot captured at migration time (before).
    // Used to compute frame-time delta in outcome measurement. 0 = no data.
    float frame_time_before_p95 = 0.0f;
    float frame_time_before_avg = 0.0f;
    int   frame_time_before_samples = 0;
    // Sleep-pending exponential backoff. Without backoff, a sleeping
    // thread is re-measured every 50ms cycle, producing 99.5% skipped samples.
    // consecutive_sleeps tracks how many times this pending was skipped due to
    // sleep; last_attempt_ms records the last measurement attempt time.
    // Backoff = min(100 * 2^min(n,6), 5000) ms. Reset on successful outcome.
    unsigned long long last_attempt_ms = 0;
    int consecutive_sleeps = 0;
    // v4.2 Batch B3: set when this pending was created by an epsilon-greedy
    // control tick (boost deliberately suppressed). The outcome loop then folds
    // the measured outcome through the paired-difference (sign-flipped) label.
    bool counterfactualControl = false;
    // v4.5.0: set when this pending was created in observe-only / auxiliary
    // (passive) mode. No migration was applied (control semantics unchanged); the
    // outcome loop measures the thread's actual run-ratio as usual but routes the
    // (features -> outcome) pair into the separate SHADOW buffer instead of
    // training the live model. Shadow samples never move weights / sample_count /
    // alpha and never feed the drift latch.
    bool shadow = false;
};

struct BaselineEntry {
    float run_ratio;
    time_t timestamp;
};

class LearningModel {
public:
    // Constructor auto-initializes with 3 pools (little/mid/big).
    // The object is immediately usable after construction; no separate init() call required.
    // Call init(num_pools) to reconfigure pool count (idempotent, safe to call repeatedly).
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
    // ROUND-010 D4-07: dimension drift hotplug handler. Unlike init(), this
    // PRESERVES the learned weights/sample counts of existing pools and only
    // appends zero-initialized pools on growth, or drops trailing pools on shrink.
    // Used when PRIME (slot 3) comes online after boot (3->4) so accumulated
    // knowledge on pools 0..2 is not discarded. pending/baseline state untouched.
    void migratePoolCount(int new_count);
    int getTotalSamples() const;
    std::vector<float> getWeights() const;
    void setWeights(const std::vector<float>& w);
    // per-pool weight setter (thread-safe, recursive_mutex)
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
    // LED-059 (R5-002): was an unclamped setter. A runtime setColdStartSamples(-5)
    // bypassed validateParameters() and made the cold-start poolProgress denominator
    // negative -> negative alpha polluting fusion. Mirror setMinSamples' clamping:
    // reject out-of-range values and fall back to the 50-sample default.
    void setColdStartSamples(int v) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        cold_start_samples = (v >= 0 && v <= 100000) ? v : 50;
    }
    // ML cold start protection — per-pool readiness gate.
    // Returns true if the pool has accumulated at least min_samples_threshold samples.
    // When cold start protection is enabled, pools returning false are excluded from
    // ML score fusion (heuristic-only scheduling) until they reach the threshold.
    bool isPoolReady(int poolIdx) const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (poolIdx < 0 || poolIdx >= (int)models.size()) return false;
        return models[poolIdx].sample_count >= min_samples_threshold;
    }
    // #97 (rel15): does this pool slot correspond to a REAL CPU cluster on this
    // device? Presence is derived RUNTIME from the live cpu->pool assignment
    // (setCpuPoolAssignment), never hardcoded: a tier slot that the topology left
    // with zero CPUs (e.g. a 1+3+4 design that has no MID tier, or a future device
    // with a tier the model still reserves a slot for) reports false.
    //
    // Semantics:
    //   * No assignment has been synced yet (boot, host tests) -> treat every pool
    //     as present (preserves the historical "all slots are clusters" behavior).
    //   * Assignment synced and NO cpu maps to poolIdx -> ABSENT cluster.
    //
    // An absent pool:
    //   - is excluded from isMature()'s per-pool maturity denominator (an empty
    //     tier can never reach min samples, so it must not drag readiness to 0%);
    //   - is surfaced to the UI as "no such tier / not applicable" instead of
    //     "still learning".
    bool isPoolPresent(int poolIdx) const;
    int getMinSamples() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return min_samples_threshold; }
    void setMinSamples(int n) { std::lock_guard<std::recursive_mutex> lk(mutex_); min_samples_threshold = (n > 0) ? n : 1; }
    void setEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); enabled = v; }
    bool isEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return enabled; }

    // ========================================================================
    // v4.2 Batch B — ML coldstart deferred features.
    // ALL three default OFF. When off the update()/predict() paths are
    // byte-for-byte identical to v4.1.1 (see the implementation notes).
    // The daemon pushes g_cfg.mlOutcomeMadFilter / mlDriftRelearn /
    // mlEpsilonGreedy into these setters; aatpe_ml.cpp itself never reads g_cfg
    // (it is linked into the lean aatpe-cli which does not link aatped_config).
    // ========================================================================
    // B1 (P1): outcome MAD k=3 robust clipping. When on, an outcome whose
    // |outcome - rollingMedian| > 3*MAD for its pool is down-weighted to 0.2
    // (never dropped). Per-pool rolling median/MAD only, KB-scale, no per-app map.
    void setMadFilterEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); madFilterEnabled_ = v; }
    bool isMadFilterEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return madFilterEnabled_; }
    // Exposed for host tests: returns the sample weight in [0.2,1.0] that the MAD
    // filter WOULD apply to `outcome` for `pool`, updating the rolling window.
    // Only meaningful when madFilterEnabled_ is on; returns 1.0 otherwise.
    float computeOutlierWeightForPool(int pool, float outcome);

    // B2 (P2): drift relearn. When on, per-pool W=200 outcome ring + short/long
    // alpha-fused EMA are maintained (runtime only, never persisted). On a drift
    // latch the caller invokes nudgeTowardNeutral(pool), which pulls that pool's
    // weights HALF WAY toward the zero vector (= sigmoid(0) = neutral 0.5), never
    // a full reset.
    void setDriftRelearnEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); driftRelearnEnabled_ = v; }
    bool isDriftRelearnEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return driftRelearnEnabled_; }
    void nudgeTowardNeutral(int pool);
    int getDriftRingSize(int pool) const;

    // B3 (P2): epsilon-greedy counterfactual exploration. 0.0 (default) =>
    // maybeExplore() ALWAYS returns false (zero branch). The daemon's g_cfg parse
    // already clamps the value to [0,0.05]; this setter keeps only the lower bound
    // so host tests can drive an ε=1.0 "always explore" scenario deterministically.
    void setEpsilonGreedy(double v) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (v < 0.0) v = 0.0;
        epsilonGreedy_ = v;
    }
    double getEpsilonGreedy() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return epsilonGreedy_; }
    void setExploreJankBlocked(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); exploreJankBlocked_ = v; }
    // Returns true when THIS tick's boost should be SUPPRESSED (control sample).
    // Deterministic PRNG: splitmix64(tid ^ tick), no global rand() state.
    bool maybeExplore(int tid, float predicted);
    static long long getExploreSkippedTotal() { return g_mlExploreSkippedTotal.load(std::memory_order_relaxed); }
    // Paired-difference label for a control (boost-suppressed) sample. A control
    // outcome that looks GOOD (thread fine without a boost) must teach the model
    // that this placement does NOT need a boost -> label is mirrored about 0.5.
    static float counterfactualControlLabel(float outcome) {
        if (outcome < 0.0f) return outcome;          // invalid sentinel, pass through
        return 1.0f - outcome;
    }
    void setCpuPoolAssignment(const std::vector<int>& a) { std::lock_guard<std::recursive_mutex> lk(mutex_); m_cpuPoolAssignment = a; }
    // P2-14: return by value (copy) instead of const&. The internal vector can
    // be reallocated on another thread while the caller holds the reference.
    std::vector<int> getCpuPoolAssignment() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_cpuPoolAssignment; }
    void setTopologyFingerprint(const std::string& fp) {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (!m_topologyFingerprint.empty() && m_topologyFingerprint != fp) m_modelStale = true;
        m_topologyFingerprint = fp;
    }
    // P3-26: return by value (copy) instead of const& to avoid dangling reference.
    std::string getTopologyFingerprint() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_topologyFingerprint; }
    bool isModelStale() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_modelStale; }
    void markModelStale() { std::lock_guard<std::recursive_mutex> lk(mutex_); m_modelStale = true; }
    // P3-43: on-disk provenance generation (bumped each save). 0 until a v6 model
    // is saved/loaded.
    uint32_t getGeneration() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_generation; }
    // P3-45: runtime anomaly latch. True once predict() has observed a NaN/Inf or
    // out-of-range output; the model is then one-way disabled (heuristic fallback)
    // until the next reload/re-init.
    bool isAnomalyLatched() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return m_anomalyLatched; }
    float getAlphaForPool(int pool) const {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (!enabled || pool < 0 || pool >= (int)models.size()) return 0.0f;
        return models[pool].alpha;
    }
    // upper bound of the alpha ramp. pool alpha ramps linearly to
    // max_alpha (see recomputeAllAlphas), so maturity fraction = poolAlpha /
    // max_alpha in [0,1]. fuseDecisions() uses that fraction as the CONTINUOUS ML
    // weight scale instead of a cold-start on/off threshold.
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
    // P0-2: Return copies of all active pending decisions (includes exited threads).
    // Caller must hold g_mlMutex. Iterating this instead of g_threads ensures
    // pending decisions for threads that have exited are still measured/expired.
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
    // v4.2 Batch B3: tag the pending decision for `tid` as a counterfactual
    // control sample (boost deliberately suppressed). The outcome loop then folds
    // the measured outcome through the paired-difference label. No-op if no
    // pending exists for tid.
    void markPendingCounterfactual(int tid);
    // v4.5.0: tag the pending decision for `tid` as a SHADOW pending (created in
    // observe-only / passive mode; no migration applied). The outcome loop routes
    // the measured outcome into the shadow buffer, never into update(). No-op if
    // no active pending exists for tid.
    void markPendingShadow(int tid);
    // ---- v4.5.0 shadow sampling (observe-only) ----
    // When on (default), observe-only mode still records feature->outcome pairs
    // into the bounded shadow ring. When off, recordShadowOutcome() is a no-op
    // (zero collection). This is independent of the live-model `enabled` gate:
    // shadow recording is pure telemetry and must not train/latch anything.
    void setShadowSamplingEnabled(bool v) { std::lock_guard<std::recursive_mutex> lk(mutex_); shadowSamplingEnabled_ = v; }
    bool isShadowSamplingEnabled() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return shadowSamplingEnabled_; }
    // Record a shadow outcome. NEVER trains: weights/sample_count/alpha/MAD/drift
    // ring are untouched. outcome < 0 (measureOutcomeV2 invalid sentinel) is
    // rejected and counted via g_mlShadowSamplesDropped. The ring is capped at
    // kMaxShadowSamples (oldest evicted) so memory stays bounded.
    void recordShadowOutcome(int pool, const ThreadFeatures& features, float outcome);
    long long getShadowSamplesCollected() const { return g_mlShadowSamplesCollected.load(std::memory_order_relaxed); }
    long long getShadowSamplesDropped() const { return g_mlShadowSamplesDropped.load(std::memory_order_relaxed); }
    size_t getShadowRingSize() const { std::lock_guard<std::recursive_mutex> lk(mutex_); return shadowRing_.size(); }
    // v6.0.0: bounded, drainable snapshot of the shadow ring for offline
    // effect-validation export. Returns up to maxN OLDEST samples (clamped to
    // kMaxShadowSamples = 512 so the allocation is always bounded); when
    // clear=true the returned prefix is REMOVED from the ring (drain), when
    // false it is a pure snapshot. Lock held for the whole copy so the CLI/
    // control path cannot block the main loop on unbounded work and the
    // exported view is internally consistent. Never trains / mutates the live
    // model beyond removing the drained prefix.
    struct ShadowSampleView {
        int pool = 0;
        ThreadFeatures features;
        float outcome = 0.0f;
    };
    std::vector<ShadowSampleView> drainShadowSamples(size_t maxN, bool clear = true);
    int clearExpiredPendingDecisions(int max_age_sec);
    // Update sleep-backoff state for a pending decision.
    // Used by outcome measurement loop to implement exponential backoff
    // for sleeping threads. consecutive_sleeps=0 resets backoff.
    void setPendingSleepBackoff(int tid, int consecutive_sleeps, unsigned long long last_attempt_ms);
    void recordBaseline(int tid, float run_ratio);
    float getBaselineRunRatio(int tid) const;
    void cleanExpiredBaselines(int max_age_sec);
    // F1.1 (v4.3.8): record a baseline from a TRUE normalized duty cycle, in the
    // SAME dimension as measureOutcomeV2's nrr (delta_run/delta_time, normalized
    // by freq_factor), instead of the old avgRunNs/10ms timeslice proxy which is
    // a different physical quantity.
    // Two-period tracking: the FIRST call for a tid only latches the reference
    // sample (runTime/nowNs/freq) and records NOTHING -> the outcome side then
    // falls back to the self-consistent 1.0-nrr score. Subsequent calls compute
    // duty = (runTime-prevRunTime)/(nowNs-prevNs) over [prev, now], normalize it
    // by avgFreq/maxFreq exactly like measureOutcomeV2, and push the resulting
    // baseline_nrr into the sliding baseline window. nowNs MUST be the monotonic
    // timestamp sampled at the SAME instant as runTime (a fresh schedstat read).
    // Returns the baseline_nrr that was recorded (0.0f when no sample produced,
    // e.g. first period, idle window, or counter reset).
    float recordBaselineDuty(int tid, unsigned long long runTime,
                             unsigned long long nowNs, int curFreq, int maxFreq);
    static float measureOutcomeV2(unsigned long long before_run_time,
        unsigned long long after_run_time, time_t before_ts, time_t after_ts,
        int before_freq, int after_freq, int base_freq, int loadInt, float baseline_run_ratio,
        float frame_time_before_p95 = 0.0f, float frame_time_after_p95 = 0.0f,
        int frame_time_sample_count = 0, float frame_time_weight = 0.0f,
        bool is_ui_render_thread = false,
        unsigned long long before_monotonic_ns = 0,  // P1 round7: ns delta clock
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
    int min_samples_threshold;  // per-pool sample threshold for cold start protection
    // P3-45: `enabled` is mutable so predictUnlocked() (const, running under the
    // model's own recursive_mutex via BatchLock) can one-way latch it OFF when a
    // runtime NaN/Inf prediction is observed. The mutex still serializes access;
    // mutability only relaxes the const-correctness on this single safety path.
    mutable bool enabled;
    // P3-43: on-disk provenance generation (bumped each save; read back on load).
    // `mutable` because save() is const but bumps this internal provenance
    // counter; the recursive_mutex still serializes all access.
    mutable uint32_t m_generation = 0;
    // P3-45: runtime anomaly latch (set under mutex; cleared by init()/reset()).
    mutable bool m_anomalyLatched = false;
    std::vector<int> m_cpuPoolAssignment;
    // #97: callers must already hold mutex_. Returns true if at least one real CPU
    // is mapped to poolIdx in the live cpu->pool assignment. When no assignment has
    // been synced yet, conservatively reports true for every pool (historical
    // behavior); once synced, a pool slot with zero mapped CPUs is an absent tier.
    bool poolHasCpusUnlocked(int poolIdx) const;
    std::string m_topologyFingerprint;
    bool m_modelStale = false;
    bool m_modelLoaded = false;
    // ---- v4.2 Batch B runtime state (all gated; no effect when off) ----
    bool madFilterEnabled_ = false;
    bool driftRelearnEnabled_ = false;
    bool exploreJankBlocked_ = false;
    double epsilonGreedy_ = 0.0;
    // B1: per-pool rolling outcome window for robust median/MAD. KB-scale, no
    // per-app map. Sized lazily to models.size() in update().
    std::vector<std::deque<float>> outcomeMadWindow_;
    // B2: per-pool W=200 outcome ring + short-term outcome EMA (runtime only).
    std::vector<std::deque<float>> outcomeRing_;
    std::vector<float> stOutcomeEma_;
    // B3: deterministic explore tick counter for the PRNG.
    uint64_t exploreTick_ = 0;
    // Batch B tuning constants.
    static constexpr int   kMadWindow      = 100;
    static constexpr int   kMadMinSamples  = 10;
    static constexpr float  kMadK           = 3.0f;
    static constexpr float  kMadDownweight  = 0.2f;
    static constexpr int   kDriftRingW     = 200;
    static constexpr float  kNudgeAlpha     = 0.5f;   // half step toward neutral
    static constexpr float  kExploreMargin  = 0.15f;  // edge band |pred-0.5|
    // ---- v4.5.0 shadow sampling state (observe-only; runtime only) ----
    // Bounded ring of (pool, features, outcome) recorded in passive mode. Never
    // persisted, never feeds predict()/SGD. Capped to keep memory bounded
    // regardless of how long the daemon runs in observe-only mode.
    struct ShadowSample { int pool; ThreadFeatures features; float outcome; };
    std::deque<ShadowSample> shadowRing_;
    bool shadowSamplingEnabled_ = true;
    static constexpr size_t kMaxShadowSamples = 512;
    void syncRuntimePoolStateLocked();
    float applyMadFilterLocked(int pool, float outcome);
    std::vector<PendingDecision> pending_decisions;
    std::map<int, std::deque<BaselineEntry>> baseline_windows;
    // F1.1: two-period baseline duty reference. Holds the previous schedstat
    // runTime paired with the monotonic ns timestamp and CPU freq sampled at the
    // SAME instant, so recordBaselineDuty can compute a true duty over [prev,now].
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

// ============================================================================
// Frame-Time Outcome Measurement (IMPLEMENTED)
// FrameTimeOutcome holds real frame timing data parsed from dumpsys gfxinfo
// and SurfaceFlinger latency. Fusion: outcome = ft_weight * frame_time_improvement
// + (1-ft_weight) * run_ratio_improvement.
// ============================================================================
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

// GfxInfoFrameTimeProvider — parses dumpsys gfxinfo <pkg> for percentile frame times.
// Cached with TTL (default 2s) to avoid repeated expensive dumpsys Binder calls.
class GfxInfoFrameTimeProvider {
public:
    GfxInfoFrameTimeProvider();
    ~GfxInfoFrameTimeProvider();
    FrameTimeOutcome getSnapshot();
    FrameTimeOutcome refresh();
    bool isAvailable() const { return available_; }
    void setCacheTtlSec(int sec) { cache_ttl_sec_ = (sec > 0) ? sec : 2; }
#ifdef AATPE_HOST_TEST
    // v4.4.9 R4-F3: hermetic seam to drive parseGfxinfo with fake dumpsys output
    // (no shell / no callback). Compiled out of release (no symbol in nm).
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

// SurfaceFlingerLatencyProvider — reuses frame.cpp's SF latency detection via callback.
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

// Global frame-time outcome counters for WebUI diagnostics.
extern std::atomic<long long> g_ftOutcomeSamples;
extern std::atomic<long long> g_ftOutcomeFallback;
extern std::atomic<long long> g_ftOutcomeImproved;
extern std::atomic<long long> g_ftOutcomeDegraded;
extern std::atomic<long long> g_ftGfxinfoCalls;
extern std::atomic<long long> g_ftGfxinfoCacheHits;

// ============================================================================
// #38 ML anomaly drift detector 
// ----------------------------------------------------------------------------
// Watches the closed loop: when the ML model keeps voting one pool as best but
// the MEASURED outcome (frame time / run-ratio improvement) keeps degrading for
// N consecutive windows, the model has drifted off (e.g. workload changed, ROM
// update retuned sched). We latch a drift flag and let the caller fall back to
// heuristic scheduling + shrink alpha. Single-threaded: recordOutcome() is
// called from the daemon outcome-measurement loop, same thread that queries.
// ============================================================================
class DriftDetector {
public:
    DriftDetector();
    // outcome: measureOutcomeV2 FUSED SCORE in [0,1]; 0.5 = neutral, <0.5 =
    // regression/degrade, >0.5 = improvement (v4.4.9 R4-F1 contract fix; the old
    // "signed improvement, neutral==0" contract never latched in production
    // because measureOutcomeV2 returns a 0.5-centered score).
    // recommendedPool: the pool ML voted best for this migration.
    void recordOutcome(float outcome, int recommendedPool);
    // True when ANY pool's consecutive-degrade streak has hit the latch.
    bool isDriftDetected() const { return driftDetected_.load(std::memory_order_relaxed); }
    void reset();
    void setWindow(int n) { window_ = (n >= 3) ? n : 3; }
    // P2-13: atomic counters so cross-thread reads/writes are well-defined.
    long long getSamples() const { return samples_.load(std::memory_order_relaxed); }
    // ML-08: outcomes whose recommendedPool was outside [0,kMaxPools) used to be
    // silently clamped to pool 0, polluting pool 0's degrade streak (a garbage
    // pool id could falsely latch global drift). They are now dropped and
    // counted here instead.
    long long getOutOfRangePoolSamples() const {
        return outOfRangePoolSamples_.load(std::memory_order_relaxed);
    }
    // G7 R004-B2 (L1): degrades are tracked PER POOL; this returns the max
    // streak across pools so a single bad pool does not fake a global drift.
    long long getConsecutiveDegrades() const {
        long long mx = 0;
        for (const auto& c : consecDegrades_) {
            long long v = c.load(std::memory_order_relaxed);
            if (v > mx) mx = v;
        }
        return mx;
    }
private:
    static constexpr int kMaxPools = 8;  // up to 4 tiers + headroom
    std::atomic<bool> driftDetected_{false};
    int  window_ = 10;
    // G7 R004-B2 (L1): per-pool consecutive-degrade counters (was a single
    // global counter that ignored recommendedPool, so one workload's bad streak
    // on one pool latched the GLOBAL fallback for every pool).
    std::array<std::atomic<long long>, kMaxPools> consecDegrades_{};
    std::atomic<long long> samples_{0};
    std::atomic<long long> outOfRangePoolSamples_{0};  // ML-08
};
extern std::atomic<bool> g_mlDriftDetected;

// #39 topology binding helpers. Compute a stable hash string from the live CPU
// topology (online cpu count, per-pool core counts, capacity signature). Returns
// e.g. "n8:L4:M3:B1:cap<...>". On a different device/ROM this string differs.
std::string computeDeviceTopologyHash();
// Sidecar persistence next to model.dat. Returns true when the stored hash
// matches currentHash (fresh model for this device). Mismatch/missing -> false
// (caller must cold-start). Writes currentHash on save.
bool loadTopologySidecar(const char* modelPath, const std::string& currentHash);
bool saveTopologySidecar(const char* modelPath, const std::string& currentHash);

// ROUND-014 P3: per-role prior migration strength. shrink=max(0.3,1-ownSamples/50):
// full trust at >=50 own samples, floor 0.3.
float rolePriorShrink(int ownSamples);

// ROUND-014 P3: boost ROI loop. boostRoiRecord skips charging sessions. recentNetMean
// over last up-to-32 sessions; negative = boost cost more than it helped.
void boostRoiRecord(bool charging, float jankDelta, int dischargeMwDelta);
float boostRoiRecentNetMean();

} // namespace aatpe_ml
#endif // AATPE_ML_H

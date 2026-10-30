#include "aatpe_ml.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <algorithm>
#include <limits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <map>
#include <deque>
#include <mutex>
#include <chrono>
#include <sstream>
using namespace aatpe_ml;

std::string aatpeExec(const std::string& cmd, const char* debugTag);

namespace aatpe_ml {

std::atomic<long long> g_pendingSkippedExisting{0};
std::atomic<long long> g_invalidOutcomeSkipped{0};
std::atomic<long long> g_droppedInvalidTargetPool{0};
std::atomic<long long> g_loadedWeightDegraded{0};
std::atomic<bool> g_mlTrainingPausedByThermal{false};
std::atomic<bool> g_mlFrameOutcomeSuppressedByThermal{false};
std::atomic<long long> g_mlExploreSkippedTotal{0};
std::atomic<long long> g_mlUpdateSkipThermal{0};
std::atomic<long long> g_mlUpdateSkipDisabled{0};
std::atomic<long long> g_mlShadowSamplesCollected{0};
std::atomic<long long> g_mlShadowSamplesDropped{0};

#include "aatpe-constants.h"
namespace {
void writeUintStateFileAtomic(const char* name, unsigned long long value) {
    char finalPath[256], tmpPath[300];
    snprintf(finalPath, sizeof(finalPath), "%s/%s", aatpe::paths::kStateDir, name);
    snprintf(tmpPath, sizeof(tmpPath), "%s/%s.tmp.%d", aatpe::paths::kStateDir, name, getpid());
    FILE* fp = fopen(tmpPath, "w");
    if (!fp) return;
    fprintf(fp, "%llu\n", value);
    fflush(fp);
    fclose(fp);
    rename(tmpPath, finalPath);
}
}

static void writeUintStateFile(const char* name, unsigned long long value) {
    writeUintStateFileAtomic(name, value);
}

SFLatencyCallback g_sfLatencyCallback = nullptr;
ForegroundAppCallback g_foregroundAppCallback = nullptr;
std::atomic<long long> g_ftOutcomeSamples{0};
std::atomic<long long> g_ftOutcomeFallback{0};
std::atomic<long long> g_ftOutcomeImproved{0};
std::atomic<long long> g_ftOutcomeDegraded{0};
std::atomic<long long> g_ftGfxinfoCalls{0};
std::atomic<long long> g_ftGfxinfoCacheHits{0};
std::atomic<long long> LearningModel::g_saturatedSamplesRetained{0};

static constexpr float kMaxAvgRunUs = 500000.0f;
static constexpr float kMaxNrSwitches [[maybe_unused]] = 10000.0f;
static constexpr float kMaxMigrationCount = 1000.0f;
static constexpr int kMaxPriority = 99;
static constexpr int kNiceRange = 39;
static constexpr int kNiceMin = -20;
static constexpr int kMaxThermal = 120;
static constexpr int kMaxCapacity = 1024;
static constexpr float kDefaultL2Lambda = 0.001f;

LearningModel::LearningModel()
    : total_samples(0), alpha(0.0f), max_alpha(0.3f), initial_lr(0.01f),
      lr_decay(0.999f), weight_clamp(10.0f), l2_lambda(kDefaultL2Lambda),
      cold_start_samples(50), min_samples_threshold(2000), enabled(true) {
    init(3);
}
LearningModel::~LearningModel() {}

void LearningModel::init(int num_pools) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    models.clear();
    if (num_pools <= 0) num_pools = 3;
    if (num_pools > 16) num_pools = 16;
    models.resize(num_pools);
    for (auto& m : models) {
        for (int i = 0; i < ML_WEIGHT_COUNT; i++) m.weights[i] = 0.0f;
        m.learning_rate = initial_lr;
        m.sample_count = 0;
        m.alpha = 0.0f;
    }
    total_samples = 0; alpha = 0.0f;
    pending_decisions.clear();
    baseline_windows.clear();
    m_anomalyLatched = false;
}

float LearningModel::sigmoid(float x) {
    if (x > 20.0f) return 1.0f;
    if (x < -20.0f) return 0.0f;
    return 1.0f / (1.0f + std::exp(-x));
}

float LearningModel::dotProduct(const PoolModel& model, const ThreadFeatures& f) const {
    float sum = model.weights[0];
    sum += model.weights[1]*f.avg_run_us + model.weights[2]*f.nr_switches
         + model.weights[3]*f.cpu_usage + model.weights[4]*f.priority
         + model.weights[5]*f.nice + model.weights[6]*f.is_game
         + model.weights[7]*f.thermal + model.weights[8]*f.capacity
         + model.weights[9]*f.current_cpu_match + model.weights[10]*f.migration_count
         + model.weights[11]*f.cross_run_cap + model.weights[12]*f.cross_run_usage
         + model.weights[13]*f.cross_cap_thermal + model.weights[14]*f.cross_run_thermal;
    return sum;
}

float LearningModel::predict(int pool, const ThreadFeatures& features) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return predictUnlocked(pool, features);
}

float LearningModel::predictUnlocked(int pool, const ThreadFeatures& features) const {
    if (!enabled) return 0.0f;
    if (pool < 0 || pool >= (int)models.size()) return 0.0f;
    float raw = sigmoid(dotProduct(models[pool], features));
    if (!std::isfinite(raw) || raw < 0.0f || raw > 1.0f) {
        enabled = false;
        m_anomalyLatched = true;
        return 0.0f;
    }
    return raw;
}

float LearningModel::predictMigrationGain(int fromPool, int toPool, const ThreadFeatures& features) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled) return 0.0f;
    if (fromPool < 0 || fromPool >= (int)models.size()) return 0.0f;
    if (toPool < 0 || toPool >= (int)models.size()) return 0.0f;
    if (fromPool == toPool) return 0.0f;
    if (models[fromPool].sample_count < 20 || models[toPool].sample_count < 20) return 0.0f;
    return predict(toPool, features) - predict(fromPool, features);
}

float LearningModel::clampWeight(float w) const {
    if (w > weight_clamp) return weight_clamp;
    if (w < -weight_clamp) return -weight_clamp;
    return w;
}

UpdateResult LearningModel::update(int pool, const ThreadFeatures& features, float outcome) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled) { g_mlUpdateSkipDisabled.fetch_add(1, std::memory_order_relaxed); return UpdateResult::SKIP_DISABLED_ANOMALY; }
    if (g_mlTrainingPausedByThermal.load(std::memory_order_relaxed)) {
        g_mlUpdateSkipThermal.fetch_add(1, std::memory_order_relaxed);
        return UpdateResult::SKIP_THERMAL;
    }
    if (pool < 0 || pool >= (int)models.size()) {
        g_droppedInvalidTargetPool.fetch_add(1, std::memory_order_relaxed);
        return UpdateResult::SKIP_POOL;
    }
    if (outcome < 0.0f) {
        g_invalidOutcomeSkipped.fetch_add(1, std::memory_order_relaxed);
        return UpdateResult::SKIP_INVALID_OUTCOME;
    }
    if (outcome > 1.0f) outcome = 1.0f;
    PoolModel& model = models[pool];
    float sampleWeight = 1.0f;
    if (madFilterEnabled_) {
        sampleWeight = applyMadFilterLocked(pool, outcome);
    }
    if (driftRelearnEnabled_) {
        syncRuntimePoolStateLocked();
        auto& ring = outcomeRing_[pool];
        ring.push_back(outcome);
        while ((int)ring.size() > kDriftRingW) ring.pop_front();
        float& st = stOutcomeEma_[pool];
        st = (st == 0.0f) ? outcome : (0.1f * outcome + 0.9f * st);
    }
    float prediction = sigmoid(dotProduct(model, features));
    float error = prediction - outcome;
    float feature_vals[ML_FEATURE_COUNT + ML_CROSS_FEATURE_COUNT] = {
        features.avg_run_us, features.nr_switches, features.cpu_usage,
        features.priority, features.nice, features.is_game, features.thermal,
        features.capacity, features.current_cpu_match, features.migration_count,
        features.cross_run_cap, features.cross_run_usage,
        features.cross_cap_thermal, features.cross_run_thermal
    };
    const float effLr = model.learning_rate * sampleWeight;
    model.weights[0] = clampWeight(model.weights[0] - effLr * error);
    for (int i = 0; i < ML_FEATURE_COUNT + ML_CROSS_FEATURE_COUNT; i++) {
        float grad = error * feature_vals[i] + l2_lambda * model.weights[i + 1];
        model.weights[i + 1] = clampWeight(model.weights[i + 1] - effLr * grad);
    }
    model.sample_count++;
    total_samples++;
    model.learning_rate *= lr_decay;
    if (model.learning_rate < 1e-6f) model.learning_rate = 1e-6f;
    if (model.sample_count >= cold_start_samples && cold_start_samples > 0) {
        float poolProgress = (float)(model.sample_count - cold_start_samples + 1) /
                             (float)(cold_start_samples * 10);
        if (poolProgress < 0.0f) poolProgress = 0.0f;
        if (poolProgress > 1.0f) poolProgress = 1.0f;
        model.alpha = max_alpha * poolProgress;
    } else { model.alpha = 0.0f; }
    if (total_samples >= cold_start_samples && cold_start_samples > 0) {
        float progress = (float)(total_samples - cold_start_samples + 1) /
                         (float)(cold_start_samples * 10);
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        alpha = max_alpha * progress;
    } else { alpha = 0.0f; }
    return UpdateResult::TRAINED;
}

void LearningModel::syncRuntimePoolStateLocked() {
    const int n = (int)models.size();
    if ((int)outcomeMadWindow_.size() < n) outcomeMadWindow_.resize(n);
    if ((int)outcomeRing_.size() < n) outcomeRing_.resize(n);
    if ((int)stOutcomeEma_.size() < n) stOutcomeEma_.resize(n, 0.0f);
}

float LearningModel::applyMadFilterLocked(int pool, float outcome) {
    syncRuntimePoolStateLocked();
    auto& win = outcomeMadWindow_[pool];
    float w = 1.0f;
    const size_t n = win.size();
    if (n >= (size_t)kMadMinSamples) {
        std::vector<float> sorted(win.begin(), win.end());
        std::sort(sorted.begin(), sorted.end());
        float med = (sorted.size() & 1)
                  ? sorted[sorted.size() / 2]
                  : 0.5f * (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]);
        std::vector<float> absdev;
        absdev.reserve(sorted.size());
        for (float v : sorted) absdev.push_back(std::fabs(v - med));
        std::sort(absdev.begin(), absdev.end());
        float mad = (absdev.size() & 1)
                  ? absdev[absdev.size() / 2]
                  : 0.5f * (absdev[absdev.size() / 2 - 1] + absdev[absdev.size() / 2]);
        float threshold;
        if (mad > 1e-6f) {
            threshold = kMadK * mad;
        } else {
            double mean = 0.0; for (float v : win) mean += v; mean /= (double)n;
            double var = 0.0; for (float v : win) { double d = v - mean; var += d * d; }
            var /= (double)n;
            float sigma = (float)std::sqrt(var);
            threshold = kMadK * sigma;
            if (threshold < 1e-4f) threshold = 0.1f;
        }
        if (std::fabs(outcome - med) > threshold) w = kMadDownweight;
    }
    win.push_back(outcome);
    while ((int)win.size() > kMadWindow) win.pop_front();
    return w;
}

float LearningModel::computeOutlierWeightForPool(int pool, float outcome) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (outcome > 1.0f) outcome = 1.0f;
    if (!madFilterEnabled_) return 1.0f;
    if (pool < 0 || pool >= (int)models.size()) return 1.0f;
    return applyMadFilterLocked(pool, outcome);
}

void LearningModel::nudgeTowardNeutral(int pool) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (!driftRelearnEnabled_) return;
    if (pool < 0 || pool >= (int)models.size()) return;
    for (int i = 0; i < ML_WEIGHT_COUNT; i++)
        models[pool].weights[i] *= kNudgeAlpha;
}

int LearningModel::getDriftRingSize(int pool) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (pool < 0 || pool >= (int)outcomeRing_.size()) return 0;
    return (int)outcomeRing_[pool].size();
}

static uint64_t mlSplitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

bool LearningModel::maybeExplore(int tid, float predicted) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (epsilonGreedy_ <= 0.0) return false;
    if (g_mlTrainingPausedByThermal.load(std::memory_order_relaxed) ||
        g_mlFrameOutcomeSuppressedByThermal.load(std::memory_order_relaxed)) {
        g_mlExploreSkippedTotal.fetch_add(1, std::memory_order_relaxed);
        writeUintStateFile("ml_explore_skipped_total",
                           (unsigned long long)g_mlExploreSkippedTotal.load(std::memory_order_relaxed));
        return false;
    }
    if (exploreJankBlocked_) {
        g_mlExploreSkippedTotal.fetch_add(1, std::memory_order_relaxed);
        writeUintStateFile("ml_explore_skipped_total",
                           (unsigned long long)g_mlExploreSkippedTotal.load(std::memory_order_relaxed));
        return false;
    }
    if (std::fabs(predicted - 0.5f) >= kExploreMargin) return false;
    exploreTick_++;
    uint64_t s = (uint64_t)(uint32_t)tid + exploreTick_ * 0xD1B54A32D192ED03ULL;
    uint64_t h = mlSplitmix64(s);
    double r = (double)(h >> 11) / (double)(1ULL << 53);
    return (r < epsilonGreedy_);
}

static std::vector<PoolModel> aggregateCpuModelsToPools(
    const std::vector<PoolModel>& cpuModels,
    const std::vector<int>& cpuPoolAssignment, float initial_lr) {
    int targetPools = 1;
    for (int a : cpuPoolAssignment) if (a >= targetPools - 1) targetPools = a + 1;
    if (targetPools < 1) targetPools = 1;
    if (targetPools > 16) targetPools = 16;
    std::vector<PoolModel> pools(targetPools);
    for (int p = 0; p < targetPools; p++) {
        for (int w = 0; w < ML_WEIGHT_COUNT; w++) pools[p].weights[w] = 0.0f;
        pools[p].learning_rate = initial_lr;
        pools[p].sample_count = 0;
    }
    std::vector<double> weightSums(size_t(targetPools) * ML_WEIGHT_COUNT, 0.0);
    std::vector<long long> sampleSums(targetPools, 0);
    std::vector<double> lrSums(targetPools, 0.0);
    int numCpus = (int)cpuModels.size();
    for (int cpu = 0; cpu < numCpus; cpu++) {
        int pool = (cpu < (int)cpuPoolAssignment.size()) ? cpuPoolAssignment[cpu] : -1;
        if (pool < 0 || pool >= targetPools) continue;
        int sc = cpuModels[cpu].sample_count;
        if (sc < 0) sc = 0;
        double weight = (double)sc;
        if (weight <= 0.0) weight = 1.0;
        for (int w = 0; w < ML_WEIGHT_COUNT; w++)
            weightSums[pool * ML_WEIGHT_COUNT + w] += (double)cpuModels[cpu].weights[w] * weight;
        sampleSums[pool] += sc;
        lrSums[pool] += (double)cpuModels[cpu].learning_rate * weight;
    }
    for (int p = 0; p < targetPools; p++) {
        long long totalSc = sampleSums[p];
        if (totalSc > 0) {
            double norm = 1.0 / (double)totalSc;
            for (int w = 0; w < ML_WEIGHT_COUNT; w++)
                pools[p].weights[w] = (float)(weightSums[p * ML_WEIGHT_COUNT + w] * norm);
            pools[p].learning_rate = (float)(lrSums[p] * norm);
            pools[p].sample_count = (int)totalSc;
        }
    }
    return pools;
}

static std::vector<int> defaultCpuPoolAssignment(int numCpus) {
    std::vector<int> assignment(numCpus, -1);
    if (numCpus <= 0) return assignment;
    if (numCpus == 1) { assignment[0] = 0; }
    else if (numCpus == 2) { assignment[0] = 0; assignment[1] = 2; }
    else if (numCpus == 3) { assignment[0] = 0; assignment[1] = 1; assignment[2] = 2; }
    else if (numCpus <= 4) { for (int i=0;i<2;i++) assignment[i]=0; for (int i=2;i<4;i++) assignment[i]=2; }
    else if (numCpus <= 6) { int lc=numCpus-2; for(int i=0;i<lc;i++) assignment[i]=0; for(int i=lc;i<numCpus;i++) assignment[i]=2; }
    else {
        int bigCount=2, midCount=std::max(1,numCpus-6), littleCount=numCpus-midCount-bigCount;
        if (littleCount<1) littleCount=1;
        for (int i=0;i<littleCount&&i<numCpus;i++) assignment[i]=0;
        for (int i=littleCount;i<littleCount+midCount&&i<numCpus;i++) assignment[i]=1;
        for (int i=littleCount+midCount;i<numCpus;i++) assignment[i]=2;
    }
    return assignment;
}

static uint32_t aatpe_crc32(const void* data, size_t len) {
    static const uint32_t table[256] = {
        0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
        0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,
        0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
        0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63084CD9,0xFA0F3D63,0x8D080DF5,
        0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
        0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
        0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
        0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
        0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
        0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
        0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
        0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
        0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
        0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
        0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
        0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
        0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
        0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
        0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
        0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
        0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
        0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
        0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
        0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
        0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
        0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
        0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
        0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
        0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
        0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
        0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
        0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
    };
    const uint8_t* buf = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint64_t aatpe_topo_hash64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t poolRecordSize(int num_pools) {
    return (size_t)num_pools * (sizeof(float) * ML_WEIGHT_COUNT + sizeof(float) + sizeof(int));
}

static constexpr size_t kV6HeaderBytes =
    8 + sizeof(int32_t) + sizeof(int32_t) + sizeof(int)
    + sizeof(float) * 6 + sizeof(int) + sizeof(int);

static constexpr size_t kV6ProvenanceBytes =
    sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t);

static size_t computeModelFileSize(int num_pools) {
    return kV6HeaderBytes + poolRecordSize(num_pools) + kV6ProvenanceBytes + sizeof(uint32_t);
}

bool LearningModel::save(const char* path) const {
    uint64_t topoHash = aatpe_topo_hash64(computeDeviceTopologyHash());
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!path || path[0] == '\0') return false;
    char tmp_path[1024];
    int sn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    if (sn < 0 || (size_t)sn >= sizeof(tmp_path)) return false;
    int tmpFd = ::open(tmp_path, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (tmpFd < 0) return false;
    FILE* fp = fdopen(tmpFd, "w+b");
    if (!fp) {
        close(tmpFd);
        unlink(tmp_path);
        return false;
    }
    bool ok = true;
    size_t bytes_written = 0;
    #define ML_FWRITE(ptr, size, count) do { size_t _n = fwrite((ptr),(size),(count),fp); bytes_written += _n*(size); if (_n != (count)) ok = false; } while(0)
    const char magic[8] = {'A','A','T','P','E','M','L','6'};
    ML_FWRITE(magic, 1, 8);
    int32_t version = ML_MODEL_VERSION;
    ML_FWRITE(&version, sizeof(int32_t), 1);
    int32_t num_pools = (int32_t)models.size();
    ML_FWRITE(&num_pools, sizeof(int32_t), 1);
    ML_FWRITE(&total_samples, sizeof(int), 1);
    ML_FWRITE(&alpha, sizeof(float), 1);
    ML_FWRITE(&max_alpha, sizeof(float), 1);
    ML_FWRITE(&initial_lr, sizeof(float), 1);
    ML_FWRITE(&lr_decay, sizeof(float), 1);
    ML_FWRITE(&weight_clamp, sizeof(float), 1);
    ML_FWRITE(&l2_lambda, sizeof(float), 1);
    ML_FWRITE(&cold_start_samples, sizeof(int), 1);
    int32_t enabledInt = enabled ? 1 : 0;
    ML_FWRITE(&enabledInt, sizeof(int32_t), 1);
    for (const auto& m : models) {
        ML_FWRITE(m.weights, sizeof(float), ML_WEIGHT_COUNT);
        ML_FWRITE(&m.learning_rate, sizeof(float), 1);
        ML_FWRITE(&m.sample_count, sizeof(int), 1);
    }
    m_generation += 1;
    uint32_t gen = m_generation;
    ML_FWRITE(&gen, sizeof(uint32_t), 1);
    uint64_t saveTs = (uint64_t)time(nullptr);
    ML_FWRITE(&saveTs, sizeof(uint64_t), 1);
    ML_FWRITE(&topoHash, sizeof(uint64_t), 1);
    #undef ML_FWRITE
    if (ok) {
        long data_len = ftell(fp);
        if (data_len <= 0 || data_len > 65536) ok = false;
        if (ok && fseek(fp, 0, SEEK_SET) == 0) {
            unsigned char buf[65536];
            if (fread(buf, 1, (size_t)data_len, fp) == (size_t)data_len &&
                fseek(fp, data_len, SEEK_SET) == 0) {
                uint32_t crc = aatpe_crc32(buf, (size_t)data_len);
                if (fwrite(&crc, sizeof(uint32_t), 1, fp) == 1) bytes_written += sizeof(uint32_t);
                else ok = false;
            } else ok = false;
        }
    }
    if (ok && fflush(fp) != 0) ok = false;
    if (ok && fsync(fileno(fp)) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    size_t expected = computeModelFileSize((int)models.size());
    if (ok && bytes_written != expected) ok = false;
    if (!ok) {
        if (unlink(tmp_path) != 0) fprintf(stderr, "[ML] unlink tmp failed: %s\n", tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        if (unlink(tmp_path) != 0) fprintf(stderr, "[ML] unlink tmp after rename fail: %s\n", tmp_path);
        return false;
    }
    return true;
}

bool LearningModel::load(const char* path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!path) return false;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    char magic[8];
    if (fread(magic, 1, 8, fp) != 8) { fclose(fp); return false; }
    const char ev6[8]={'A','A','T','P','E','M','L','6'};
    const char ev5[8]={'A','A','T','P','E','M','L','5'};
    const char ev4[8]={'A','A','T','P','E','M','L','4'};
    const char ev3[8]={'A','A','T','P','E','M','L','3'};
    const char ev2[8]={'A','A','T','P','E','M','L','2'};
    const char ev1[8]={'A','A','T','P','E','M','L','1'};
    bool is_v6=(memcmp(magic,ev6,8)==0), is_v5=(memcmp(magic,ev5,8)==0), is_v4=(memcmp(magic,ev4,8)==0);
    bool is_v3=(memcmp(magic,ev3,8)==0), is_v2=(memcmp(magic,ev2,8)==0);
    bool is_v1=(memcmp(magic,ev1,8)==0);
    if (!is_v1&&!is_v2&&!is_v3&&!is_v4&&!is_v5&&!is_v6) { fclose(fp); return false; }
    auto readConfig = [&]() -> bool {
        return fread(&total_samples,sizeof(int),1,fp)==1 && fread(&alpha,sizeof(float),1,fp)==1
            && fread(&max_alpha,sizeof(float),1,fp)==1 && fread(&initial_lr,sizeof(float),1,fp)==1
            && fread(&lr_decay,sizeof(float),1,fp)==1 && fread(&weight_clamp,sizeof(float),1,fp)==1
            && fread(&l2_lambda,sizeof(float),1,fp)==1 && fread(&cold_start_samples,sizeof(int),1,fp)==1;
    };
    uint32_t loadedGen = 0; uint64_t loadedSaveTs = 0; uint64_t loadedTopoHash = 0;
    if (is_v3 || is_v4 || is_v5 || is_v6) {
        int32_t version = 0;
        if (fread(&version,sizeof(int32_t),1,fp)!=1) { fclose(fp); return false; }
        if (version!=3 && version!=4 && version!=5 && version!=6) { fclose(fp); return false; }
        int32_t num_pools = 0;
        if (fread(&num_pools,sizeof(int32_t),1,fp)!=1) { fclose(fp); return false; }
        if (num_pools<=0 || num_pools>16) { fclose(fp); return false; }
        models.resize(num_pools);
        if (!readConfig()) { fclose(fp); return false; }
        if (version >= 5) {
            int32_t le = 1;
            if (fread(&le,sizeof(int32_t),1,fp)!=1) { fclose(fp); return false; }
            enabled = (le != 0);
        } else enabled = true;
        for (auto& m : models) {
            if (fread(m.weights,sizeof(float),ML_WEIGHT_COUNT,fp)!=(size_t)ML_WEIGHT_COUNT) { fclose(fp); return false; }
            for (auto& w : m.weights) {
                if (!std::isfinite(w)) { w = 0.0f; g_loadedWeightDegraded.fetch_add(1, std::memory_order_relaxed); }
                else {
                    float c = clampWeight(w);
                    if (c != w) g_loadedWeightDegraded.fetch_add(1, std::memory_order_relaxed);
                    w = c;
                }
            }
            if (fread(&m.learning_rate,sizeof(float),1,fp)!=1) { fclose(fp); return false; }
            if (fread(&m.sample_count,sizeof(int),1,fp)!=1) { fclose(fp); return false; }
        }
        if (version >= 6) {
            if (fread(&loadedGen,sizeof(uint32_t),1,fp)!=1) { fclose(fp); return false; }
            if (fread(&loadedSaveTs,sizeof(uint64_t),1,fp)!=1) { fclose(fp); return false; }
            if (fread(&loadedTopoHash,sizeof(uint64_t),1,fp)!=1) { fclose(fp); return false; }
        }
        if (version >= 4) {
            long data_end = ftell(fp);
            if (data_end <= 0 || data_end > 65536) { fclose(fp); return false; }
            uint32_t stored_crc = 0;
            if (fread(&stored_crc,sizeof(uint32_t),1,fp)==1) {
                std::vector<unsigned char> buf((size_t)data_end);
                if (fseek(fp,0,SEEK_SET)==0 && fread(buf.data(),1,(size_t)data_end,fp)==(size_t)data_end) {
                    uint32_t calc_crc = aatpe_crc32(buf.data(),(size_t)data_end);
                    if (calc_crc != stored_crc) {
                        fprintf(stderr,
                                "[ML] CRC mismatch (file=%s stored=%08x calc=%08x); rejecting model\n",
                                path, stored_crc, calc_crc);
                        fclose(fp); return false;
                    }
                } else { fclose(fp); return false; }
            } else { fclose(fp); return false; }
        }
        if (version >= 6) {
            uint64_t curTopo = aatpe_topo_hash64(computeDeviceTopologyHash());
            if (loadedTopoHash != curTopo) {
                fprintf(stderr,
                        "[ML] embedded topology hash mismatch (file=%016llx cur=%016llx); rejecting model\n",
                        (unsigned long long)loadedTopoHash, (unsigned long long)curTopo);
                fclose(fp);
                for (auto& m : models) {
                    for (int i = 0; i < ML_WEIGHT_COUNT; i++) m.weights[i] = 0.0f;
                    m.learning_rate = initial_lr;
                    m.sample_count = 0;
                    m.alpha = 0.0f;
                }
                total_samples = 0; alpha = 0.0f;
                m_anomalyLatched = false;
                return false;
            }
            m_generation = loadedGen;
        }
    } else if (is_v2) {
        int32_t version = 0;
        if (fread(&version,sizeof(int32_t),1,fp)!=1) { fclose(fp); return false; }
        if (version != 2) { fclose(fp); return false; }
        int32_t num_cpus = 0;
        if (fread(&num_cpus,sizeof(int32_t),1,fp)!=1) { fclose(fp); return false; }
        if (num_cpus<=0||num_cpus>256) { fclose(fp); return false; }
        std::vector<PoolModel> cpuModels(num_cpus);
        if (!readConfig()) { fclose(fp); return false; }
        for (int i=0;i<num_cpus;i++) {
            if (fread(cpuModels[i].weights,sizeof(float),ML_WEIGHT_COUNT,fp)!=(size_t)ML_WEIGHT_COUNT) { fclose(fp); return false; }
            if (fread(&cpuModels[i].learning_rate,sizeof(float),1,fp)!=1) { fclose(fp); return false; }
            if (fread(&cpuModels[i].sample_count,sizeof(int),1,fp)!=1) { fclose(fp); return false; }
        }
        std::vector<int> pa = m_cpuPoolAssignment;
        if (pa.empty()) pa = defaultCpuPoolAssignment(num_cpus);
        models = aggregateCpuModelsToPools(cpuModels, pa, initial_lr);
        enabled = true;
        m_anomalyLatched = false;
    } else {
        int32_t num_cpus = 0;
        if (fread(&num_cpus,sizeof(int32_t),1,fp)!=1) { fclose(fp); return false; }
        if (num_cpus<=0||num_cpus>256) { fclose(fp); return false; }
        std::vector<PoolModel> cpuModels(num_cpus);
        if (fread(&total_samples,sizeof(int),1,fp)!=1||fread(&alpha,sizeof(float),1,fp)!=1
         ||fread(&max_alpha,sizeof(float),1,fp)!=1||fread(&initial_lr,sizeof(float),1,fp)!=1
         ||fread(&lr_decay,sizeof(float),1,fp)!=1||fread(&weight_clamp,sizeof(float),1,fp)!=1
         ||fread(&cold_start_samples,sizeof(int),1,fp)!=1) { fclose(fp); return false; }
        l2_lambda = kDefaultL2Lambda;
        for (int i=0;i<num_cpus;i++) {
            float oldw[11];
            if (fread(oldw,sizeof(float),11,fp)!=11) { fclose(fp); return false; }
            for (int w=0;w<11;w++) cpuModels[i].weights[w]=oldw[w];
            for (int w=11;w<ML_WEIGHT_COUNT;w++) cpuModels[i].weights[w]=0.0f;
            if (fread(&cpuModels[i].learning_rate,sizeof(float),1,fp)!=1) { fclose(fp); return false; }
            if (fread(&cpuModels[i].sample_count,sizeof(int),1,fp)!=1) { fclose(fp); return false; }
        }
        std::vector<int> pa = m_cpuPoolAssignment;
        if (pa.empty()) pa = defaultCpuPoolAssignment(num_cpus);
        models = aggregateCpuModelsToPools(cpuModels, pa, initial_lr);
        enabled = true;
        m_anomalyLatched = false;
    }
    fclose(fp);
    if (!validateParameters()) {
        int pc = (int)models.size();
        init(pc > 0 ? pc : 3);
        enabled = false;
        m_anomalyLatched = true;
        fprintf(stderr, "[ML] validateParameters failed on load; model disabled (heuristic fallback)\n");
        return false;
    }
    {
        struct stat mst;
        if (stat(path, &mst) == 0) {
            time_t ageSec = time(nullptr) - mst.st_mtime;
            const time_t kStaleAgeSec = time_t(7) * 24 * 3600;
            if (ageSec > kStaleAgeSec) {
                if (total_samples > cold_start_samples) {
                    total_samples = cold_start_samples +
                        (total_samples - cold_start_samples) / 2;
                }
                for (auto& m : models) {
                    if (m.sample_count > cold_start_samples) {
                        m.sample_count = cold_start_samples +
                            (m.sample_count - cold_start_samples) / 2;
                    }
                }
            }
        }
    }
    recomputeAllAlphas();
    m_modelLoaded = true;
    writeUintStateFile("ml_weight_degraded_count",
                       (unsigned long long)g_loadedWeightDegraded.load(std::memory_order_relaxed));
    writeUintStateFile("ml_loaded_degraded",
                       (unsigned long long)(g_loadedWeightDegraded.load() > 0 ? 1 : 0));
    return true;
}

float LearningModel::getAlpha() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled) return 0.0f;
    return alpha;
}

bool LearningModel::isMature() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled) return false;
    if (alpha <= 0.05f) return false;
    int total = 0;
    for (const auto& m : models) total += m.sample_count;
    if (total <= 100) return false;
    for (int p = 0; p < (int)models.size(); p++) {
        if (!poolHasCpusUnlocked(p)) continue;
        if (models[p].sample_count <= 20) return false;
    }
    if (!m_modelLoaded) return false;
    return true;
}

bool LearningModel::poolHasCpusUnlocked(int poolIdx) const {
    if (poolIdx < 0 || poolIdx >= (int)models.size()) return false;
    if (m_cpuPoolAssignment.empty()) return true;
    for (int cpuPool : m_cpuPoolAssignment) {
        if (cpuPool == poolIdx) return true;
    }
    return false;
}

bool LearningModel::isPoolPresent(int poolIdx) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return poolHasCpusUnlocked(poolIdx);
}

void LearningModel::recomputeAllAlphas() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& m : models) {
        if (m.sample_count >= cold_start_samples) {
            float p = (float)(m.sample_count - cold_start_samples + 1) / (float)(cold_start_samples * 10);
            if (p > 1.0f) p = 1.0f;
            m.alpha = max_alpha * p;
        } else m.alpha = 0.0f;
    }
    if (total_samples >= cold_start_samples) {
        float gp = (float)(total_samples - cold_start_samples + 1) / (float)(cold_start_samples * 10);
        if (gp > 1.0f) gp = 1.0f;
        alpha = max_alpha * gp;
    } else alpha = 0.0f;
}

[[deprecated("LR decay is now per-sample in update(); this function is a no-op")]]
void LearningModel::decayLearningRate() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        fprintf(stderr, "[ML] decayLearningRate() is a no-op (LR decay is per-sample in update()); ignoring call\n");
    }
}

void LearningModel::reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& m : models) {
        for (int i=0;i<ML_WEIGHT_COUNT;i++) m.weights[i]=0.0f;
        m.learning_rate=initial_lr; m.sample_count=0; m.alpha=0.0f;
    }
    total_samples=0; alpha=0.0f;
    pending_decisions.clear(); baseline_windows.clear();
    m_modelLoaded = false;
    m_anomalyLatched = false;
}

void LearningModel::migratePoolCount(int new_count) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (new_count <= 0) new_count = 1;
    if (new_count > 16) new_count = 16;
    int old = (int)models.size();
    if (new_count == old) return;
    if (new_count > old) {
        models.resize(new_count);
        for (int p = old; p < new_count; p++) {
            for (int i = 0; i < ML_WEIGHT_COUNT; i++) models[p].weights[i] = 0.0f;
            models[p].learning_rate = initial_lr;
            models[p].sample_count = 0;
            models[p].alpha = 0.0f;
        }
    } else {
        while ((int)models.size() > new_count) {
            int victim = (int)models.size() - 1;
            int target = new_count - 1;
            PoolModel& v = models[victim];
            PoolModel& t = models[target];
            long long sv = v.sample_count, st = t.sample_count;
            long long tot = st + sv;
            if (tot > 0) {
                for (int i = 0; i < ML_WEIGHT_COUNT; i++) {
                    float wv = (sv > 0) ? v.weights[i] : t.weights[i];
                    float wt = t.weights[i];
                    float merged = (sv > 0 && st > 0)
                        ? (wt * (float)st + wv * (float)sv) / (float)tot
                        : wv;
                    t.weights[i] = clampWeight(merged);
                }
                t.sample_count += v.sample_count;
            }
            models.pop_back();
        }
    }
    recomputeAllAlphas();
}

int LearningModel::getTotalSamples() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return total_samples;
}

std::vector<float> LearningModel::getWeights() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<float> all;
    for (const auto& pm : models) for (float w : pm.weights) all.push_back(w);
    return all;
}

void LearningModel::setWeights(const std::vector<float>& w) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t idx = 0;
    for (auto& pm : models) {
        for (int i=0;i<ML_WEIGHT_COUNT;i++) {
            if (idx < w.size()) {
                float val = w[idx];
                if (!std::isnan(val) && !std::isinf(val)) pm.weights[i] = clampWeight(val);
                idx++;
            } else pm.weights[i] = 0.0f;
        }
    }
}

void LearningModel::setWeights(int poolIdx, const std::vector<float>& w) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (poolIdx < 0 || poolIdx >= (int)models.size()) return;
    PoolModel& pm = models[poolIdx];
    for (int i = 0; i < ML_WEIGHT_COUNT; i++) {
        if (i < (int)w.size()) {
            float val = w[i];
            if (!std::isnan(val) && !std::isinf(val)) pm.weights[i] = clampWeight(val);
        } else pm.weights[i] = 0.0f;
    }
}

PoolModel LearningModel::getPoolModel(int pool) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PoolModel empty;
    for (int i=0;i<ML_WEIGHT_COUNT;i++) empty.weights[i]=0.0f;
    empty.learning_rate=0.0f; empty.sample_count=0;
    if (pool<0||pool>=(int)models.size()) return empty;
    return models[pool];
}

CpuModel LearningModel::getCpuModel(int cpu) const { return getPoolModel(cpu); }

#ifdef AATPE_HOST_TEST
void LearningModel::aatpeTestCorruptPoolWeights(int pool) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (pool<0||pool>=(int)models.size()) return;
    models[pool].weights[0] = std::numeric_limits<float>::quiet_NaN();
    models[pool].weights[1] = 1e30f;
}
#endif

bool LearningModel::isValidPrediction(float score) const {
    return !std::isnan(score) && !std::isinf(score) && score >= 0.0f && score <= 1.0f;
}

bool LearningModel::isAllZeroFeatures(const ThreadFeatures& f) {
    return f.avg_run_us == 0.0f && f.nr_switches == 0.0f &&
           f.cpu_usage == 0.0f && f.priority == 0.0f &&
           f.nice == 0.0f && f.is_game == 0.0f &&
           f.thermal == 0.0f && f.capacity == 0.0f &&
           f.current_cpu_match == 0.0f && f.migration_count == 0.0f &&
           f.cross_run_cap == 0.0f && f.cross_run_usage == 0.0f &&
           f.cross_cap_thermal == 0.0f && f.cross_run_thermal == 0.0f;
}

bool LearningModel::validateParameters() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (std::isnan(max_alpha)||std::isinf(max_alpha)||max_alpha<0||max_alpha>1) return false;
    if (std::isnan(initial_lr)||std::isinf(initial_lr)||initial_lr<=0||initial_lr>1) return false;
    if (std::isnan(lr_decay)||std::isinf(lr_decay)||lr_decay<=0||lr_decay>1) return false;
    if (std::isnan(weight_clamp)||std::isinf(weight_clamp)||weight_clamp<=0) return false;
    if (std::isnan(l2_lambda)||std::isinf(l2_lambda)||l2_lambda<0||l2_lambda>1) return false;
    if (cold_start_samples<0||cold_start_samples>100000) return false;
    for (const auto& m : models) {
        for (int i=0;i<ML_WEIGHT_COUNT;i++) {
            float w=m.weights[i];
            if (std::isnan(w)||std::isinf(w)) return false;
            if (w<-weight_clamp*10||w>weight_clamp*10) return false;
        }
        if (std::isnan(m.learning_rate)||std::isinf(m.learning_rate)) return false;
        if (m.learning_rate<0||m.learning_rate>1) return false;
        if (m.sample_count<0) return false;
    }
    if (std::isnan(alpha)||std::isinf(alpha)||alpha<0||alpha>1) return false;
    if (total_samples<0) return false;
    long long sum=0; for (const auto& m:models) sum+=m.sample_count;
    if (sum>0&&total_samples>0) {
        long long mx=std::max(sum,(long long)total_samples), mn=std::min(sum,(long long)total_samples);
        if (mx>mn*10) return false;
    }
    return true;
}

void LearningModel::recordPendingDecision(int tid, int target_cpu, int target_pool,
    const ThreadFeatures& features, unsigned long long before_run_time,
    unsigned long long before_nr_switches, int before_cpu_usage,
    int before_freq, time_t before_timestamp, unsigned long long starttime,
    float frame_time_before_p95, float frame_time_before_avg,
    int frame_time_before_samples) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    bool skipDueToExisting = false;
    for (const auto& existing : pending_decisions) {
        if (existing.tid == tid && existing.active && existing.create_time_ms > 0) {
            unsigned long long nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs >= existing.create_time_ms &&
                (nowMs - existing.create_time_ms) < MIN_MEASURE_INTERVAL_MS) {
                skipDueToExisting = true;
            }
            break;
        }
    }
    if (skipDueToExisting) {
        g_pendingSkippedExisting++;
        return;
    }
    for (auto it=pending_decisions.begin();it!=pending_decisions.end();++it)
        if (it->tid==tid) { pending_decisions.erase(it); break; }
    while ((int)pending_decisions.size() >= MAX_PENDING_DECISIONS)
        pending_decisions.erase(pending_decisions.begin());
    PendingDecision pd;
    pd.tid=tid; pd.target_cpu=target_cpu; pd.target_pool=target_pool;
    pd.features=features; pd.before_run_time=before_run_time;
    pd.before_nr_switches=before_nr_switches; pd.before_cpu_usage=before_cpu_usage;
    pd.before_freq=before_freq; pd.before_timestamp=before_timestamp?before_timestamp:time(nullptr);
    pd.before_mono_ns = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    pd.timestamp=time(nullptr); pd.active=true; pd.starttime=starttime;
    pd.create_time_ms = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    pd.frame_time_before_p95 = frame_time_before_p95;
    pd.frame_time_before_avg = frame_time_before_avg;
    pd.frame_time_before_samples = frame_time_before_samples;
    pending_decisions.push_back(pd);
}

void LearningModel::recordPendingDecision(int tid, int target_cpu,
    const ThreadFeatures& features, unsigned long long before_run_time,
    unsigned long long before_nr_switches, int before_cpu_usage,
    int before_freq, time_t before_timestamp, unsigned long long starttime,
    float frame_time_before_p95, float frame_time_before_avg,
    int frame_time_before_samples) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    recordPendingDecision(tid,target_cpu,-1,features,before_run_time,before_nr_switches,
                          before_cpu_usage,before_freq,before_timestamp,starttime,
                          frame_time_before_p95,frame_time_before_avg,frame_time_before_samples);
}

int LearningModel::findPendingDecision(int tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (size_t i=0;i<pending_decisions.size();i++)
        if (pending_decisions[i].tid==tid && pending_decisions[i].active) return (int)i;
    return -1;
}

bool LearningModel::getPendingDecision(int idx, PendingDecision& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (idx<0||idx>=(int)pending_decisions.size()) return false;
    out = pending_decisions[idx];
    return true;
}

void LearningModel::clearPendingDecision(int tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto it=pending_decisions.begin();it!=pending_decisions.end();++it)
        if (it->tid==tid) { pending_decisions.erase(it); return; }
}

void LearningModel::markPendingCounterfactual(int tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int idx = findPendingDecision(tid);
    if (idx >= 0) pending_decisions[idx].counterfactualControl = true;
}

void LearningModel::markPendingShadow(int tid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int idx = findPendingDecision(tid);
    if (idx >= 0) pending_decisions[idx].shadow = true;
}

void LearningModel::recordShadowOutcome(int pool, const ThreadFeatures& features, float outcome) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!shadowSamplingEnabled_) {
        g_mlShadowSamplesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (outcome < 0.0f) {
        g_mlShadowSamplesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (pool < 0 || pool >= (int)models.size()) {
        g_mlShadowSamplesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (outcome > 1.0f) outcome = 1.0f;
    ShadowSample s;
    s.pool = pool;
    s.features = features;
    s.outcome = outcome;
    shadowRing_.push_back(s);
    while (shadowRing_.size() > kMaxShadowSamples) shadowRing_.pop_front();
    g_mlShadowSamplesCollected.fetch_add(1, std::memory_order_relaxed);
}

std::vector<LearningModel::ShadowSampleView> LearningModel::drainShadowSamples(size_t maxN, bool clear) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t n = shadowRing_.size();
    if (maxN < n) n = maxN;
    if (n > kMaxShadowSamples) n = kMaxShadowSamples;
    std::vector<ShadowSampleView> out;
    out.reserve(n);
    auto it = shadowRing_.begin();
    for (size_t i = 0; i < n; ++i, ++it) {
        ShadowSampleView v;
        v.pool = it->pool;
        v.features = it->features;
        v.outcome = it->outcome;
        out.push_back(std::move(v));
    }
    if (clear && n > 0) {
        shadowRing_.erase(shadowRing_.begin(), shadowRing_.begin() + (std::ptrdiff_t)n);
    }
    return out;
}

int LearningModel::clearExpiredPendingDecisions(int max_age_sec) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    time_t now=time(nullptr);
    int expired = 0;
    for (auto it=pending_decisions.begin();it!=pending_decisions.end();)
        if (now-it->timestamp>max_age_sec) { it=pending_decisions.erase(it); expired++; } else ++it;
    return expired;
}

void LearningModel::setPendingSleepBackoff(int tid, int consecutive_sleeps, unsigned long long last_attempt_ms) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& pd : pending_decisions) {
        if (pd.tid == tid && pd.active) {
            pd.consecutive_sleeps = consecutive_sleeps;
            pd.last_attempt_ms = last_attempt_ms;
            return;
        }
    }
}

void LearningModel::recordBaseline(int tid, float run_ratio) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (run_ratio<0||run_ratio>1) return;
    if ((int)baseline_windows.size()>=MAX_BASELINE_THREADS &&
        baseline_windows.find(tid)==baseline_windows.end()) {
        int ot=-1; time_t ots=time(nullptr);
        for (const auto& kv:baseline_windows)
            if (!kv.second.empty()&&kv.second.front().timestamp<ots) { ots=kv.second.front().timestamp; ot=kv.first; }
        if (ot>=0) baseline_windows.erase(ot);
    }
    auto& dq=baseline_windows[tid];
    BaselineEntry e; e.run_ratio=run_ratio; e.timestamp=time(nullptr);
    dq.push_back(e);
    while ((int)dq.size()>BASELINE_WINDOW_SIZE) dq.pop_front();
}

float LearningModel::getBaselineRunRatio(int tid) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it=baseline_windows.find(tid);
    if (it==baseline_windows.end()||it->second.empty()) return 0.0f;
    float sum=0; for (const auto& e:it->second) sum+=e.run_ratio;
    return sum/(float)it->second.size();
}

float LearningModel::recordBaselineDuty(int tid, unsigned long long runTime,
                                        unsigned long long nowNs, int curFreq, int maxFreq) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = baseline_refs_.find(tid);
    if (it == baseline_refs_.end()) {
        BaselineRef r; r.runTime = runTime; r.ns = nowNs; r.freq = curFreq;
        baseline_refs_[tid] = r;
        return 0.0f;
    }
    BaselineRef prev = it->second;
    it->second = BaselineRef{runTime, nowNs, curFreq};

    if (nowNs <= prev.ns) return 0.0f;
    if (runTime < prev.runTime) return 0.0f;
    unsigned long long drun = runTime - prev.runTime;
    unsigned long long dns  = nowNs - prev.ns;
    if (drun == 0) return 0.0f;

    float duty = (float)drun / (float)dns;
    if (duty > 1.0f) duty = 1.0f;

    float freq_factor = 1.0f;
    if (maxFreq > 0 && curFreq > 0) {
        float avgFreq = (prev.freq > 0) ? (float)(prev.freq + curFreq) * 0.5f
                                        : (float)curFreq;
        freq_factor = avgFreq / (float)maxFreq;
        if (freq_factor < 0.1f) freq_factor = 0.1f;
        if (freq_factor > 5.0f) freq_factor = 5.0f;
    }
    float baseline_nrr = duty / freq_factor;
    if (baseline_nrr > 1.0f) baseline_nrr = 1.0f;

    recordBaseline(tid, baseline_nrr);
    return baseline_nrr;
}

void LearningModel::cleanExpiredBaselines(int max_age_sec) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    time_t now=time(nullptr);
    for (auto it=baseline_windows.begin();it!=baseline_windows.end();) {
        auto& dq=it->second;
        while (!dq.empty()&&(now-dq.front().timestamp)>max_age_sec) dq.pop_front();
        if (dq.empty()) it=baseline_windows.erase(it); else ++it;
    }
}

float LearningModel::measureOutcomeV2(unsigned long long before_run_time,
    unsigned long long after_run_time, time_t before_ts, time_t after_ts,
    int before_freq, int after_freq, int base_freq, int loadInt, float baseline_run_ratio,
    float frame_time_before_p95, float frame_time_after_p95,
    int frame_time_sample_count, float frame_time_weight,
    bool is_ui_render_thread,
    unsigned long long before_mono_ns, unsigned long long after_mono_ns) {
    if (after_run_time<before_run_time) return -1.0f;
    unsigned long long delta_run=after_run_time-before_run_time;
    unsigned long long delta_time_ns = 0;
    if (after_mono_ns > before_mono_ns && before_mono_ns > 0) {
        delta_time_ns = after_mono_ns - before_mono_ns;
    } else {
        if (after_ts<=before_ts||before_ts<=0) return -1.0f;
        delta_time_ns=(unsigned long long)(after_ts-before_ts)*1000000000ULL;
    }
    if (delta_run==0||delta_time_ns==0) return -1.0f;
    float run_ratio=(float)delta_run/(float)delta_time_ns;
    if (run_ratio>1.0f) run_ratio=1.0f;
    bool saturated = (run_ratio > 0.99f);
    if (saturated) {
        g_saturatedSamplesRetained.fetch_add(1, std::memory_order_relaxed);
    }
    float freq_factor=1.0f;
    if (base_freq>0&&after_freq>0) {
        float avg_freq=(before_freq>0)?((float)before_freq+(float)after_freq)*0.5f:(float)after_freq;
        freq_factor=avg_freq/(float)base_freq;
        if (freq_factor<0.1f) freq_factor=0.1f;
        if (freq_factor>5.0f) freq_factor=5.0f;
    }
    float nrr=run_ratio/freq_factor; if (nrr>1.0f) nrr=1.0f;
    float rr_score;
    if (baseline_run_ratio>0.001f) {
        float imp=(baseline_run_ratio-nrr)/baseline_run_ratio;
        rr_score=0.5f+imp*0.5f;
    } else rr_score=1.0f-nrr;
    float lw=1.0f;
    if (loadInt>200) lw=0.3f; else if (loadInt>100) lw=0.7f;
    rr_score=rr_score*lw+0.5f*(1.0f-lw);
    if (saturated) {
        float satBias = (freq_factor >= 1.0f) ? 0.2f : 0.6f;
        rr_score = rr_score * (1.0f - satBias) + 0.5f * satBias;
    }
    if (rr_score > 1.0f) rr_score = 1.0f;
    if (rr_score < 0.0f) rr_score = 0.0f;

    if (is_ui_render_thread) {
        g_ftOutcomeSamples.fetch_add(1, std::memory_order_relaxed);
    }
    bool useFrameTime = is_ui_render_thread && frame_time_sample_count > 0
                      && frame_time_before_p95 > 0.0f && frame_time_after_p95 > 0.0f
                      && frame_time_weight > 0.0f && frame_time_weight <= 1.0f;
    if (!useFrameTime) {
        if (is_ui_render_thread) {
            g_ftOutcomeFallback.fetch_add(1, std::memory_order_relaxed);
        }
        return rr_score;
    }

    float ft_improvement = (frame_time_before_p95 - frame_time_after_p95) / frame_time_before_p95;
    if (ft_improvement > 1.0f) ft_improvement = 1.0f;
    if (ft_improvement < -1.0f) ft_improvement = -1.0f;
    float ft_score = 0.5f + ft_improvement * 0.5f;
    if (ft_score > 1.0f) ft_score = 1.0f;
    if (ft_score < 0.0f) ft_score = 0.0f;

    if (ft_improvement > 0.001f) g_ftOutcomeImproved.fetch_add(1, std::memory_order_relaxed);
    else if (ft_improvement < -0.001f) g_ftOutcomeDegraded.fetch_add(1, std::memory_order_relaxed);

    float ft_w = frame_time_weight;
    if (ft_w > 1.0f) ft_w = 1.0f;
    if (ft_w < 0.0f) ft_w = 0.0f;
    float fused = ft_w * ft_score + (1.0f - ft_w) * rr_score;
    if (fused > 1.0f) fused = 1.0f;
    if (fused < 0.0f) fused = 0.0f;
    return fused;
}

float LearningModel::measureOutcome(unsigned long long before_run_time,
    unsigned long long after_run_time, unsigned long long before_switches,
    unsigned long long after_switches, int before_cpu_usage, int after_cpu_usage) {
    if (before_run_time==0) before_run_time=1;
    if (before_switches==0) before_switches=1;
    float rr=(float)after_run_time/(float)before_run_time;
    float rs=1.0f-(rr-0.5f)*0.5f; if(rs>1)rs=1; if(rs<0)rs=0;
    float sr=(float)after_switches/(float)before_switches;
    float ss=1.0f;
    if (sr>1.5f) { ss=1.0f-(sr-1.0f)*0.5f; if(ss<0)ss=0; }
    float ud=(float)(after_cpu_usage-before_cpu_usage);
    float us=1.0f;
    if (ud>20.0f) { us=1.0f-(ud-20.0f)/80.0f; if(us<0)us=0; }
    float o=rs*0.5f+ss*0.25f+us*0.25f;
    if (o > 1) o = 1;
    if (o < 0) o = 0;
    return o;
}

ThreadFeatures LearningModel::extractFeatures(unsigned long long avg_run_us_raw,
    unsigned long long nr_switches_raw, int cpu_usage_pct, int priority_raw,
    int nice_raw, bool is_game, int thermal_celsius, int target_cpu_capacity,
    bool current_cpu_match, unsigned long long migration_count_raw) {
    ThreadFeatures f;
    float ar;
    if (avg_run_us_raw>0) {
        double lr=std::log(1.0+(double)avg_run_us_raw), lm=std::log(1.0+(double)kMaxAvgRunUs);
        if (lm>0&&std::isfinite(lr)) ar=(float)(lr/lm); else ar=(float)avg_run_us_raw/kMaxAvgRunUs;
    } else ar=0;
    if (ar > 1) ar = 1;
    if (ar < 0) ar = 0;
    f.avg_run_us = ar;
    constexpr float kMainSwitchesPerSecScale = 300.0f;
    float sw=(float)nr_switches_raw/kMainSwitchesPerSecScale; if(sw>1)sw=1; if(sw<0)sw=0; f.nr_switches=sw;
    float us=(float)cpu_usage_pct/100.0f; if(us>1)us=1; if(us<0)us=0; f.cpu_usage=us;
    float pr=1.0f-(float)priority_raw/(float)kMaxPriority; if(pr>1)pr=1; if(pr<0)pr=0; f.priority=pr;
    float nv=1.0f-((float)(nice_raw-kNiceMin)/(float)kNiceRange); if(nv>1)nv=1; if(nv<0)nv=0; f.nice=nv;
    f.is_game=is_game?1.0f:0.0f;
    float th=(float)thermal_celsius/(float)kMaxThermal; if(th>1)th=1; if(th<0)th=0; f.thermal=th;
    float cp=(float)target_cpu_capacity/(float)kMaxCapacity; if(cp>1)cp=1; if(cp<0)cp=0; f.capacity=cp;
    f.current_cpu_match=current_cpu_match?1.0f:0.0f;
    float mg=(float)migration_count_raw/kMaxMigrationCount; if(mg>1)mg=1; if(mg<0)mg=0; f.migration_count=mg;
    f.cross_run_cap=f.avg_run_us*f.capacity;
    f.cross_run_usage=f.avg_run_us*f.cpu_usage;
    f.cross_cap_thermal=f.capacity*f.thermal;
    f.cross_run_thermal=f.avg_run_us*f.thermal;
    return f;
}

std::string LearningModel::getStatusString() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string result;
    char buf[512];
    snprintf(buf,sizeof(buf),
        "=== AATPE ML Model Status (V5, Per-Pool, 15 weights) ===\n"
        "Enabled: %s  Pools: %d  Samples: %d  Alpha: %.4f (max=%.4f)\n"
        "ColdStart: %s (threshold=%d)  LR: %.6f  Decay: %.4f  Clamp: %.2f  L2: %.6f\n\n",
        enabled?"YES":"NO", (int)models.size(), total_samples, alpha, max_alpha,
        (total_samples<cold_start_samples)?"ACTIVE":"COMPLETE", cold_start_samples,
        initial_lr, lr_decay, weight_clamp, l2_lambda);
    result+=buf;
    for (size_t i=0;i<models.size();i++) {
        const auto& m=models[i];
        snprintf(buf,sizeof(buf),
            "Pool %zu: samples=%d lr=%.6f alpha=%.4f bias=%+.3f w1..w14=[%+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f %+.2f]\n",
            i,m.sample_count,m.learning_rate,m.alpha,m.weights[0],
            m.weights[1],m.weights[2],m.weights[3],m.weights[4],m.weights[5],
            m.weights[6],m.weights[7],m.weights[8],m.weights[9],m.weights[10],
            m.weights[11],m.weights[12],m.weights[13],m.weights[14]);
        result+=buf;
    }
    return result;
}

GfxInfoFrameTimeProvider::GfxInfoFrameTimeProvider()
    : cache_time_(0), cache_ttl_sec_(5), available_(false) {
    cached_.sample_count = 0;
    cached_.valid = false;
}

GfxInfoFrameTimeProvider::~GfxInfoFrameTimeProvider() {}

std::string GfxInfoFrameTimeProvider::executeCommand(const std::string& cmd) {
    return aatpeExec(cmd, "GFXINFO");
}

static float aatpe_ml_parse_first_float(const std::string& s) {
    float val = 0.0f;
    bool seenDigit = false;
    bool negative = false;
    bool decimal = false;
    float decimalDiv = 10.0f;
    for (char c : s) {
        if (c == '-' && !seenDigit) { negative = true; continue; }
        if (c >= '0' && c <= '9') {
            seenDigit = true;
            if (!decimal) { val = val * 10.0f + (float)(c - '0'); }
            else {
                if (decimalDiv > 1e7f) break;
                val += (float)(c - '0') / decimalDiv; decimalDiv *= 10.0f;
            }
        } else if (c == '.' && seenDigit) {
            decimal = true;
        } else if (seenDigit) {
            break;
        }
    }
    return negative ? -val : val;
}

FrameTimeOutcome GfxInfoFrameTimeProvider::parseGfxinfo(const std::string& output) {
    FrameTimeOutcome result;
    result.sample_count = 0;
    result.valid = false;
    if (output.empty()) return result;

    float p50 = 0.0f, p95 = 0.0f, p99 = 0.0f;
    int totalFrames = 0;
    int jankyFrames = 0;

    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);

        bool isP50 = (trimmed.find("50th") != std::string::npos) ||
                     (trimmed.find("p50") != std::string::npos) ||
                     (trimmed.find("P50") != std::string::npos);
        bool isP95 = (trimmed.find("95th") != std::string::npos) ||
                     (trimmed.find("p95") != std::string::npos) ||
                     (trimmed.find("P95") != std::string::npos);
        bool isP99 = (trimmed.find("99th") != std::string::npos) ||
                     (trimmed.find("p99") != std::string::npos) ||
                     (trimmed.find("P99") != std::string::npos);
        if (!isP50 && trimmed.rfind("50%", 0) == 0) isP50 = true;
        if (!isP95 && trimmed.rfind("95%", 0) == 0) isP95 = true;
        if (!isP99 && trimmed.rfind("99%", 0) == 0) isP99 = true;

        if ((isP50 || isP95 || isP99) &&
            (trimmed.find("percentile") != std::string::npos ||
             trimmed.find(':') != std::string::npos ||
             trimmed.find('=') != std::string::npos)) {
            size_t sepPos = trimmed.find_last_of(":=");
            if (sepPos != std::string::npos && sepPos + 1 < trimmed.size()) {
                std::string numPart = trimmed.substr(sepPos + 1);
                float valMs = aatpe_ml_parse_first_float(numPart);
                if (valMs > 0.0f) {
                    if (isP50) p50 = valMs;
                    else if (isP95) p95 = valMs;
                    else if (isP99) p99 = valMs;
                }
            }
        }
        if (trimmed.find("Total frames rendered:") != std::string::npos) {
            size_t colonPos = trimmed.find(':');
            if (colonPos != std::string::npos) {
                totalFrames = (int)aatpe_ml_parse_first_float(trimmed.substr(colonPos + 1));
            }
        }
        if (trimmed.find("Janky frames:") != std::string::npos) {
            size_t colonPos = trimmed.find(':');
            if (colonPos != std::string::npos) {
                jankyFrames = (int)aatpe_ml_parse_first_float(trimmed.substr(colonPos + 1));
            }
        }
    }

    if (p50 > 0.0f) {
        result.frame_time_avg = p50;
        result.frame_time_p95 = (p95 > 0.0f) ? p95 : p50 * 1.5f;
        result.frame_time_p99 = (p99 > 0.0f) ? p99 : result.frame_time_p95 * 1.2f;
        result.sample_count = (totalFrames > 0) ? totalFrames : 1;
        result.jank_count = jankyFrames;
        result.valid = true;
        result.timestamp = time(nullptr);
        available_ = true;
    }
    return result;
}

FrameTimeOutcome GfxInfoFrameTimeProvider::getSnapshot() {
    time_t now = time(nullptr);
    if (cache_time_ > 0 && (now - cache_time_) < cache_ttl_sec_) {
        g_ftGfxinfoCacheHits.fetch_add(1, std::memory_order_relaxed);
        return cached_;
    }
    return refresh();
}

FrameTimeOutcome GfxInfoFrameTimeProvider::refresh() {
    std::string pkg;
    if (g_foregroundAppCallback != nullptr) {
        pkg = g_foregroundAppCallback();
    }
    if (pkg.empty() || pkg == "unknown") {
        cached_.sample_count = 0;
        cached_.valid = false;
        cache_time_ = time(nullptr);
        return cached_;
    }

    auto validPkgName = [](const std::string& p) -> bool {
        if (p.empty() || p.size() > 255) return false;
        if (!isalpha((unsigned char)p[0])) return false;
        bool sawDot = false;
        for (char ch : p) {
            if (ch == '.') { sawDot = true; continue; }
            if (!(isalnum((unsigned char)ch) || ch == '_')) return false;
        }
        return sawDot;
    };
    if (!validPkgName(pkg)) {
        cached_.sample_count = 0;
        cached_.valid = false;
        cache_time_ = time(nullptr);
        return cached_;
    }

    g_ftGfxinfoCalls.fetch_add(1, std::memory_order_relaxed);
    std::string cmd = "dumpsys gfxinfo " + pkg + " 2>/dev/null";
    std::string output = executeCommand(cmd);
    FrameTimeOutcome result = parseGfxinfo(output);
    cached_ = result;
    cache_time_ = time(nullptr);
    return cached_;
}

SurfaceFlingerLatencyProvider::SurfaceFlingerLatencyProvider()
    : available_(false) {}

SurfaceFlingerLatencyProvider::~SurfaceFlingerLatencyProvider() {}

std::string SurfaceFlingerLatencyProvider::executeCommand(const std::string& cmd) {
    return aatpeExec(cmd, "SF_LAT_PROV");
}

int SurfaceFlingerLatencyProvider::parseSFLatency(const std::string& output, int& outMaxLat) {
    outMaxLat = 0;
    if (output.empty()) return 0;
    int avgLat = 0;
    long long sumLat = 0;
    int count = 0;
    long long maxLat = 0;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        long long v0 = 0, v1 = 0, v2 = 0;
        int n = sscanf(trimmed.c_str(), "%lld %lld %lld", &v0, &v1, &v2);
        if (n == 3) {
            const long long kMaxGfxTsNs = (long long)1e18;
            if (v0 >= 0 && v0 <= kMaxGfxTsNs && v1 >= 0 && v1 <= kMaxGfxTsNs) {
                long long latNs = v1 - v0;
                if (latNs >= 0 && latNs <= 60000000000LL) {
                    long long latUs = latNs / 1000;
                    if (latUs >= 1000 && latUs <= 100000) {
                        sumLat += latUs;
                        count++;
                        if (latUs > maxLat) maxLat = latUs;
                    }
                }
            }
        }
    }
    if (count > 0) {
        avgLat = (int)(sumLat / count);
        outMaxLat = (int)maxLat;
        available_ = true;
    }
    return avgLat;
}

int SurfaceFlingerLatencyProvider::getLatencyUs(int refreshRate, int& outMaxLat, bool& outValid) {
    outMaxLat = 0;
    outValid = false;
    if (g_sfLatencyCallback != nullptr) {
        int maxLat = 0;
        bool valid = false;
        int avgLat = g_sfLatencyCallback(refreshRate, maxLat, valid);
        outMaxLat = maxLat;
        outValid = valid;
        if (valid) available_ = true;
        return avgLat;
    }
    std::string output = executeCommand("dumpsys SurfaceFlinger --latency 2>/dev/null");
    int maxLat = 0;
    int avgLat = parseSFLatency(output, maxLat);
    outMaxLat = maxLat;
    outValid = (avgLat > 0);
    return avgLat;
}

std::atomic<bool> g_mlDriftDetected{false};

DriftDetector::DriftDetector() {}

void DriftDetector::recordOutcome(float outcome, int recommendedPool) {
    samples_.fetch_add(1, std::memory_order_relaxed);
    if (recommendedPool < 0 || recommendedPool >= kMaxPools) {
        outOfRangePoolSamples_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    int p = recommendedPool;
    constexpr float kNeutral = 0.5f;
    if (outcome < kNeutral) {
        long long cd = consecDegrades_[p].fetch_add(1, std::memory_order_relaxed) + 1;
        if (cd >= window_ && !driftDetected_.load()) {
            driftDetected_.store(true, std::memory_order_relaxed);
            g_mlDriftDetected.store(true, std::memory_order_relaxed);
        }
    } else if (outcome > kNeutral) {
        consecDegrades_[p].store(0, std::memory_order_relaxed);
        if (driftDetected_.load()) {
            bool anyBad = false;
            for (const auto& c : consecDegrades_) {
                if (c.load(std::memory_order_relaxed) >= window_) { anyBad = true; break; }
            }
            if (!anyBad) {
                driftDetected_.store(false, std::memory_order_relaxed);
                g_mlDriftDetected.store(false, std::memory_order_relaxed);
            }
        }
    }
}

void DriftDetector::reset() {
    for (auto& c : consecDegrades_) c.store(0, std::memory_order_relaxed);
    samples_.store(0, std::memory_order_relaxed);
    outOfRangePoolSamples_.store(0, std::memory_order_relaxed);
    driftDetected_.store(false, std::memory_order_relaxed);
    g_mlDriftDetected.store(false, std::memory_order_relaxed);
}

std::string computeDeviceTopologyHash() {
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu <= 0) ncpu = 1;
    std::string capSig;
    for (long c = 0; c < ncpu && c < 16; c++) {
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpu_capacity";
        std::ifstream f(p);
        int cap = 0;
        if (f) f >> cap;
        if (cap <= 0) cap = 380;
        if (c) capSig += ",";
        capSig += std::to_string(cap);
    }
    return "n" + std::to_string(ncpu) + ":cap<" + capSig + ">";
}

bool loadTopologySidecar(const char* modelPath, const std::string& currentHash) {
    if (!modelPath || !*modelPath) return false;
    std::string sidecar = std::string(modelPath) + ".topo";
    std::ifstream f(sidecar);
    if (!f) return false;
    std::string stored;
    std::getline(f, stored);
    while (!stored.empty() && (stored.back()=='\n'||stored.back()=='\r'||stored.back()==' '))
        stored.pop_back();
    return stored == currentHash;
}

bool saveTopologySidecar(const char* modelPath, const std::string& currentHash) {
    if (!modelPath || !*modelPath) return false;
    std::string sidecar = std::string(modelPath) + ".topo";
    std::string tmp = sidecar + ".tmp." + std::to_string(getpid());
    int tmpFd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (tmpFd < 0) return false;
    FILE* fp = ::fdopen(tmpFd, "w");
    if (!fp) { ::close(tmpFd); return false; }
    bool ok = (fwrite(currentHash.c_str(), 1, currentHash.size(), fp) == currentHash.size());
    if (fflush(fp) != 0) ok = false;
    if (ok && fsync(fileno(fp)) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    if (!ok) {
        if (unlink(tmp.c_str()) != 0) fprintf(stderr, "[ML] unlink topo tmp failed\n");
        return false;
    }
    if (rename(tmp.c_str(), sidecar.c_str()) != 0) {
        if (unlink(tmp.c_str()) != 0) fprintf(stderr, "[ML] unlink topo tmp after rename fail\n");
        return false;
    }
    return true;
}

float rolePriorShrink(int ownSamples) {
    if (ownSamples < 0) ownSamples = 0;
    float s = 1.0f - (float)ownSamples / 50.0f;
    if (s < 0.3f) s = 0.3f;
    return s;
}

static std::mutex s_roiMutex;
static std::deque<float> s_roiRing;
static constexpr int kRoiRingMax = 32;
static constexpr int kRoiNoiseMw = 200;

void boostRoiRecord(bool charging, float jankDelta, int dischargeMwDelta) {
    if (charging) return;
    if (std::abs(dischargeMwDelta) < kRoiNoiseMw) return;
    float net = jankDelta * 1.0f - (float)dischargeMwDelta / 1000.0f;
    std::lock_guard<std::mutex> lk(s_roiMutex);
    s_roiRing.push_back(net);
    while ((int)s_roiRing.size() > kRoiRingMax) s_roiRing.pop_front();
}

float boostRoiRecentNetMean() {
    std::lock_guard<std::mutex> lk(s_roiMutex);
    if (s_roiRing.empty()) return 0.0f;
    double sum = 0; for (float v : s_roiRing) sum += v;
    return (float)(sum / (double)s_roiRing.size());
}

}

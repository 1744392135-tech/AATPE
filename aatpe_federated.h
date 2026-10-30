#ifndef AATPE_FEDERATED_H
#define AATPE_FEDERATED_H
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
namespace aatpe_federated {

struct ModelMetadata {
    uint32_t generation = 0;
    int64_t  timestamp  = 0;
    uint32_t modelVersion = 0;
    uint32_t topologyHash = 0;
};

struct ModelGradient {
    std::vector<float> weights;
    float alpha = 0.0f;
    int sampleCount = 0;
};

struct FederatedStats {
    int rounds = 0;
    int exports = 0;
    int imports = 0;
    int trustedImports = 0;
    int aggregatedModels = 0;
    int topologyMatchCount = 0;
    int localExcludedImmature = 0;
};

std::vector<float> aggregateModels(const std::vector<std::vector<float>>& models,
                                    const std::vector<int>& sampleCounts);

class FederatedLearner {
public:
    FederatedLearner();
    ~FederatedLearner();
    void init(const std::string& exchangeDir);
    void setLocalModel(const std::vector<float>& weights, float alpha, int sampleCount);
    void setEnabled(bool e);
    bool isEnabled() const;
    void setAllowUnverifiedCrc(bool allow) {
        std::lock_guard<std::mutex> lk(mutex_);
        allowUnverifiedCrc_ = allow;
    }
    bool getAllowUnverifiedCrc() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return allowUnverifiedCrc_;
    }
    void setLocalTopologyFingerprint(const std::string& fp) {
        std::lock_guard<std::mutex> lk(mutex_);
        localTopoFp_ = fp;
    }
    void exportLocalModel(void);
    void checkAndImportModels(void);
    void runAggregation(void);
    void loadReplayState(void) {
        std::lock_guard<std::mutex> lk(mutex_);
        loadReplayStateLocked();
    }
    void saveReplayState(void) {
        std::lock_guard<std::mutex> lk(mutex_);
        saveReplayStateLocked();
    }
    FederatedStats getStats(void) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return stats_;
    }
    std::vector<float> getAggregatedModel(void) const {
        std::lock_guard<std::mutex> lk(mutex_);
        return aggregatedModel_;
    }
private:
    mutable std::mutex mutex_;
    std::string exchangeDir_;
    bool enabled_ = false;
    bool allowUnverifiedCrc_ = false;
    std::vector<float> localWeights_;
    float localAlpha_ = 0.0f;
    int localSampleCount_ = 0;
    std::string localTopoFp_;
    std::vector<float> aggregatedModel_;
    FederatedStats stats_;
    long long lastExportTime_ = 0;
    uint32_t localGeneration_ = 1;
    uint32_t lastAcceptedGeneration_ = 0;
    std::map<std::string, uint32_t> lastAcceptedBySource_;
    static constexpr int64_t kMaxTimestampDriftSec = 86400;
    static constexpr int kFederatedLocalMinSamples = 1000;
    struct ImportedModelEntry {
        std::vector<float> weights;
        int sampleCount = 0;
        bool trusted = false;
        std::string topologyFingerprint;
        uint32_t generation = 0;
    };
    static constexpr int MAX_IMPORTED_MODELS = 64;
    std::vector<ImportedModelEntry> importedModels_;
    void loadReplayStateLocked(void);
    void saveReplayStateLocked(void);
    void ensureDirsLocked(void);
};
}
#endif

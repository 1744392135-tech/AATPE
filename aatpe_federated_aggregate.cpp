#include "aatpe_federated.h"
#include <algorithm>
#include <cmath>
namespace aatpe_federated {

std::vector<float> aggregateModels(const std::vector<std::vector<float>>& models,
                                    const std::vector<int>& sampleCounts) {
    if (models.empty()) return {};
    std::vector<int> counts = sampleCounts;
    while ((int)counts.size()<(int)models.size()) counts.push_back(1);
    constexpr int kMaxContributorWeight = 100000;
    for (auto& c : counts) {
        if (c < 1) c = 1;
        if (c > kMaxContributorWeight) c = kMaxContributorWeight;
    }
    size_t maxLen=0;
    for (const auto& m:models) maxLen=std::max(maxLen,m.size());
    std::vector<float> result(maxLen,0.0f);
    for (size_t i=0;i<maxLen;i++) {
        std::vector<float> finiteVals;
        for (const auto& m:models) if (i<m.size() && std::isfinite(m[i])) finiteVals.push_back(m[i]);
        if (finiteVals.empty()) { result[i]=0.0f; continue; }
        std::sort(finiteVals.begin(), finiteVals.end());
        float med = finiteVals[finiteVals.size()/2];
        std::vector<float> dev;
        for (float v:finiteVals) dev.push_back(std::fabs(v-med));
        std::sort(dev.begin(), dev.end());
        float mad = dev.empty()?0.0f:dev[dev.size()/2];
        float lo, hi;
        if (mad > 1e-6f) { lo = med - 3.0f*mad; hi = med + 3.0f*mad; }
        else { lo = -100.0f; hi = 100.0f; }
        double ws=0; long long tw=0;
        for (size_t m=0;m<models.size();m++) {
            if (i<models[m].size() && std::isfinite(models[m][i])) {
                float v = models[m][i];
                if (v<lo) v=lo; else if (v>hi) v=hi;
                ws+=(double)v*(double)counts[m]; tw+=counts[m];
            }
        }
        if (tw>0) result[i]=(float)(ws/(double)tw);
    }
    return result;
}

void FederatedLearner::runAggregation(void) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!enabled_) return;
    if (importedModels_.empty()) return;
    std::vector<std::vector<float>> models;
    std::vector<int> counts;
    bool localMature = (localSampleCount_ >= kFederatedLocalMinSamples);
    if (localMature && !localWeights_.empty()) {
        models.push_back(localWeights_);
        counts.push_back(localSampleCount_);
    } else {
        stats_.localExcludedImmature++;
    }
    int trustedCount = 0;
    int topoMatches = 0;
    for (const auto& m:importedModels_) {
        if (m.trusted) {
            models.push_back(m.weights);
            counts.push_back(m.sampleCount);
            trustedCount++;
            if (!localTopoFp_.empty() && !m.topologyFingerprint.empty() &&
                m.topologyFingerprint == localTopoFp_) {
                topoMatches++;
            }
        }
    }
    stats_.topologyMatchCount = topoMatches;
    if (trustedCount==0) {
        aggregatedModel_ = localWeights_;
        stats_.rounds++;
        importedModels_.clear();
        return;
    }
    aggregatedModel_ = aggregateModels(models, counts);
    stats_.aggregatedModels += trustedCount;
    stats_.rounds++;
    importedModels_.clear();
}

}

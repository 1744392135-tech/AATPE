// Federated Learning implementation (model aggregation)
// Phase-2 refactor: split from aatpe_federated.cpp
#include "aatpe_federated.h"
#include <algorithm>
#include <cmath>
namespace aatpe_federated {

// dimension mismatch handled per-dimension; sampleCounts padded
std::vector<float> aggregateModels(const std::vector<std::vector<float>>& models,
                                    const std::vector<int>& sampleCounts) {
    if (models.empty()) return {};
    std::vector<int> counts = sampleCounts;
    while ((int)counts.size()<(int)models.size()) counts.push_back(1);
    // G7-004: minimal robust aggregation — cap each contributor's sample-count
    // weight so a single trusted peer with a huge (up to 1e6) sampleCount cannot
    // dominate the average when the local model has ~1e3 samples. The real trust
    // boundary is the PSK-HMAC; this is a defensive floor, not a full robust mean.
    constexpr int kMaxContributorWeight = 100000;
    for (auto& c : counts) {
        if (c < 1) c = 1;                 // never zero/negative weight
        if (c > kMaxContributorWeight) c = kMaxContributorWeight;
    }
    size_t maxLen=0;
    for (const auto& m:models) maxLen=std::max(maxLen,m.size());
    std::vector<float> result(maxLen,0.0f);
    // ROUND-014 P3: per-dimension robust aggregation.
    for (size_t i=0;i<maxLen;i++) {
        // collect finite values per dimension; skip NaN/Inf per-dim (do not drop whole peer).
        std::vector<float> finiteVals;
        for (const auto& m:models) if (i<m.size() && std::isfinite(m[i])) finiteVals.push_back(m[i]);
        if (finiteVals.empty()) { result[i]=0.0f; continue; }
        // MAD outlier clipping: median +/- 3*MAD; MAD==0 -> fallback +/-100 (=10*clamp10).
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
                if (v<lo) v=lo; else if (v>hi) v=hi;   // winsorize, not drop
                ws+=(double)v*(double)counts[m]; tw+=counts[m];
            }
        }
        if (tw>0) result[i]=(float)(ws/(double)tw);
    }
    return result;
}

void FederatedLearner::runAggregation(void) {
    std::lock_guard<std::mutex> lk(mutex_);
    // G7-001: opt-in gate. When disabled, do not aggregate imported inputs.
    if (!enabled_) return;
    if (importedModels_.empty()) return;
    std::vector<std::vector<float>> models;
    std::vector<int> counts;
    // do NOT let an immature local model poison the average.
    // A local model with very few samples has not converged; folding it in
    // biases the aggregated weights toward noise. Gate it on sample count.
    bool localMature = (localSampleCount_ >= kFederatedLocalMinSamples);
    if (localMature && !localWeights_.empty()) {
        models.push_back(localWeights_);
        counts.push_back(localSampleCount_);
    } else {
        stats_.localExcludedImmature++;
    }
    // count topology matches. The v1 wire format does not yet
    // carry a fingerprint, so ImportedModelEntry::topologyFingerprint is usually
    // empty (unknown). When both sides carry a non-empty fingerprint, count a
    // match; mismatches are still aggregated (no hard exclusion) but are logged
    // via stats for diagnosis.
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
        // No trusted remote models: keep local as the aggregated model (or empty).
        aggregatedModel_ = localWeights_;
        stats_.rounds++;
        importedModels_.clear();
        return;
    }
    // device-heterogeneity caveat — aggregateModels() does a
    // plain sample-count-weighted average across all trusted models. When the
    // local device and the imported devices differ in CPU topology (cluster
    // count / core mix), a naive average can distort per-pool weights. The
    // topology match accounting above exposes how homogeneous the mix is;
    // future work can down-weight mismatched fingerprints.
    aggregatedModel_ = aggregateModels(models, counts);
    stats_.aggregatedModels += trustedCount;
    stats_.rounds++;
    importedModels_.clear();
}

} // namespace aatpe_federated

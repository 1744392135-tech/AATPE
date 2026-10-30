#include "aatpe_ml_hybrid.h"
namespace aatpe_ml_hybrid {

float HybridLearner::getAlpha() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alpha_;
}

void HybridLearner::recomputeAlphaLocked() {
    if (!enabled_) { alpha_ = 0.0f; return; }
    if (totalUpdates_ >= kHybridColdStartSamples) {
        float p = (float)(totalUpdates_ - kHybridColdStartSamples + 1)
                / (float)(kHybridColdStartSamples * 10);
        if (p > 1.0f) p = 1.0f;
        alpha_ = kHybridMaxAlpha * p;
    } else {
        alpha_ = 0.0f;
    }
}

}

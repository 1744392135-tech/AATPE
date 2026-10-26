// AATPE Hybrid Learning Module Implementation (alpha scheduling)
// Phase-2 refactor: split from aatpe_ml_hybrid.cpp
#include "aatpe_ml_hybrid.h"
namespace aatpe_ml_hybrid {

// current progressive fusion weight.
float HybridLearner::getAlpha() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alpha_;
}

// alpha ramp, mirroring LearningModel::recomputeAllAlphas().
// alpha_ is 0 until totalUpdates_ reaches the cold-start gate, then ramps
// linearly to kHybridMaxAlpha over the next kHybridColdStartSamples*10 updates.
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

} // namespace aatpe_ml_hybrid

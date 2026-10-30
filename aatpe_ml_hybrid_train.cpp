#include "aatpe_ml_hybrid.h"
#include <cmath>
#include <algorithm>
namespace aatpe_ml_hybrid {

void HybridLearner::sgdUpdate(int pool, const float features[15], float target) {
    if (pool<0||pool>=(int)poolDeltas_.size()) return;
    if (!std::isfinite(target)) return;
    for (int i=0;i<15;i++) if (!std::isfinite(features[i])) return;
    HybridPoolDelta& delta = poolDeltas_[pool];
    float pred = dotProduct(delta, features);
    float predSig = 1.0f/(1.0f+std::exp(-pred));
    float error = target - predSig;
    for (int i=0;i<15;i++) {
        delta.weights[i] += kLearningRate * error * features[i];
        delta.weights[i] = clampWeight(delta.weights[i]);
    }
    delta.updateCount++;
}

float HybridLearner::dotProduct(const HybridPoolDelta& delta, const float features[15]) const {
    float sum=0; for (int i=0;i<15;i++) sum+=delta.weights[i]*features[i]; return sum;
}

float HybridLearner::clampWeight(float w) const {
    if (w>kWeightClamp) return kWeightClamp;
    if (w<-kWeightClamp) return -kWeightClamp;
    return w;
}

}

#ifndef AATPE_C2_ENVELOPE_H
#define AATPE_C2_ENVELOPE_H

#include "scene_infer.h"

namespace aatpe {
namespace c2env {

enum class C2State : int {
    IDLE = 0,
    LOADING_BOOST,
    COMBAT_BOOST,
};

constexpr long long kLoadingHoldMaxMs   = 20000;
constexpr int       kLoadingExitStreak   = 3;
constexpr float     kLoadingExitFpsRatio = 0.5f;
constexpr int       kMaxArmsPerMinute    = 3;
constexpr long long kArmWindowMs         = 60000;
constexpr float     kThermalHeadroomCutoff = 0.15f;

bool c2Tick(long long nowMs, const scenefuse::SceneDecision& d,
            float headroom01, int halLevel, bool emergency);

#ifdef AATPE_HOST_TEST
C2State currentStateForTest();
const char* currentSourceForTest();
int currentLevelForTest();
void resetC2ForTest();
#endif

}
}

#endif

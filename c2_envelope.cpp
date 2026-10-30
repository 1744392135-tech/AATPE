#include "c2_envelope.h"
#include <cstring>
#include "gpu_boost.h"

namespace aatpe {
namespace c2env {

static C2State s_state = C2State::IDLE;
static long long s_enterMs = 0;
static int s_loadingExitStreak = 0;
static int s_armCountInWindow = 0;
static long long s_windowStartMs = 0;
static const char* s_currentSource = "";
static int s_currentLevel = 0;
static bool s_timeboxLocked = false;

static void clearLoadingBoost() {
    if (s_state == C2State::LOADING_BOOST) {
        aatpeBoostClear("scene_loading");
        s_state = C2State::IDLE;
        s_currentSource = "";
        s_currentLevel = 0;
        s_loadingExitStreak = 0;
    }
}

bool c2Tick(long long nowMs, const scenefuse::SceneDecision& d,
            float headroom01, int halLevel, bool emergency) {
    if (emergency || halLevel >= 1 || headroom01 < kThermalHeadroomCutoff) {
        if (s_state != C2State::IDLE) {
            clearLoadingBoost();
        }
        return false;
    }

    if (nowMs - s_windowStartMs >= kArmWindowMs) {
        s_windowStartMs = nowMs;
        s_armCountInWindow = 0;
    }

    scenefuse::Scene sc = d.scene;

    if (sc == scenefuse::Scene::LOADING) {
        if (s_state != C2State::LOADING_BOOST) {
            if (s_timeboxLocked) {
                return false;
            }
            if (s_armCountInWindow >= kMaxArmsPerMinute) {
                return false;
            }
            clearLoadingBoost();
            aatpeBoostSet("scene_loading", 80);
            s_state = C2State::LOADING_BOOST;
            s_enterMs = nowMs;
            s_currentSource = "scene_loading";
            s_currentLevel = 80;
            s_armCountInWindow++;
        }
        if (d.fpsRatio01 >= kLoadingExitFpsRatio) {
            s_loadingExitStreak++;
            if (s_loadingExitStreak >= kLoadingExitStreak) {
                clearLoadingBoost();
            }
        } else {
            s_loadingExitStreak = 0;
        }
        if (nowMs - s_enterMs >= kLoadingHoldMaxMs) {
            clearLoadingBoost();
            s_timeboxLocked = true;
        }
    } else if (sc == scenefuse::Scene::COMBAT || sc == scenefuse::Scene::TEAMFIGHT) {
        s_timeboxLocked = false;
        if (s_state == C2State::LOADING_BOOST) {
            clearLoadingBoost();
        }
        s_state = C2State::COMBAT_BOOST;
        s_currentSource = "preboost";
        s_currentLevel = 0;
    } else {
        s_timeboxLocked = false;
        clearLoadingBoost();
        s_state = C2State::IDLE;
    }

    return (s_state != C2State::IDLE);
}

#ifdef AATPE_HOST_TEST
C2State currentStateForTest() { return s_state; }
const char* currentSourceForTest() { return s_currentSource; }
int currentLevelForTest() { return s_currentLevel; }
void resetC2ForTest() {
    s_state = C2State::IDLE;
    s_enterMs = 0;
    s_loadingExitStreak = 0;
    s_armCountInWindow = 0;
    s_windowStartMs = 0;
    s_currentSource = "";
    s_currentLevel = 0;
    s_timeboxLocked = false;
}
#endif

}
}

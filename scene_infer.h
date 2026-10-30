#ifndef AATPE_SCENE_INFER_H
#define AATPE_SCENE_INFER_H

#include <cstdint>

namespace aatpe {
namespace scenefuse {

enum class Scene : int {
    UNKNOWN = 0,
    MENU,
    LOADING,
    COMBAT,
    TEAMFIGHT,
    BACKGROUND,
};

struct SceneFeatures {
    float fpsNow        = 0.0f;
    float fpsTarget     = 60.0f;
    float missRate      = 0.0f;
    float sfLatBudgetR = 0.0f;

    float cpuUtilFg     = 0.0f;
    float runDelayPerMs = 0.0f;
    float dRunDelay_dt  = 0.0f;
    float iowaitPct     = 0.0f;
    float cgThrottleDlt = 0.0f;
    float psiCpuSome    = -1.0f;

    float gpuBusyPct    = 0.0f;
    float dGpuBusy_dt   = 0.0f;

    float tapsPerSec    = 0.0f;
    float dTaps_dt      = 0.0f;

    float netRxPktsPs   = 0.0f;
    float dNetRx_dt     = 0.0f;

    float tempSlopeCps  = 0.0f;
    float headroom01    = 1.0f;

    uint32_t capsMask   = 0;
    const char* gpuTag  = "?";
    int  lowFpsStreak   = 0;
    bool fgIsGame       = false;
};

struct ActiveSignal {
    const char* name;
    float strength01;
};

struct SceneDecision {
    Scene       scene        = Scene::UNKNOWN;
    float       confidence01 = 0.0f;
    float       fpsRatio01  = 0.0f;
    ActiveSignal active[6];
    int         activeCount  = 0;
    bool        degraded     = false;
    const char* reason       = "init";
};

struct SceneState {
    Scene  committedScene   = Scene::UNKNOWN;
    Scene  prevCommittedScene = Scene::UNKNOWN;
    int    stableStreak     = 0;
    int    lowFpsStreak     = 0;
    float  tapsBaseline    = 0.0f;
    float  netBaseline      = 0.0f;
    long long enterMs        = 0;
    long long sceneEnterMs   = 0;
    bool   gameStarted      = false;
    int    teamfightExitStreak = 0;
    int    sceneChangeCount = 0;
    long long lastChangeMs   = 0;
    int    changesInWindow = 0;
    int    loadingConvergeStreak = 0;
};

constexpr uint32_t kCapPSI       = 1u << 0;
constexpr uint32_t kCapGPU       = 1u << 1;
constexpr uint32_t kCapSchedstat = 1u << 2;

constexpr float kTapsSlopeArm    = 0.5f;
constexpr float kRunDelaySlopeArm = 0.02f;
constexpr float kGpuSlopeArm      = 1.0f;
constexpr float kMinConfidence   = 0.35f;
constexpr int   kLoadingMinStreak = 2;
constexpr int   kTeamfightExitStreak = 4;
constexpr int   kLoadingExitStreak    = 3;
constexpr float kLoadingExitFpsRatio  = 0.5f;
constexpr float kPingPongWindowMs     = 5000.0f;
constexpr int   kPingPongMaxFlips    = 2;

SceneDecision inferScene(const SceneFeatures& f, SceneState& s, long long nowMs);

#ifdef AATPE_HOST_TEST
void resetSceneStateForTest(SceneState& s);
#endif

}
}

#endif

#ifndef AATPE_SCENE_WIRING_H
#define AATPE_SCENE_WIRING_H

#include <cstdint>
#include <string>
#include <vector>
#include "scene_infer.h"
#include "aatped_gamescene.h"

namespace aatpe {
namespace scenewire {

enum class SceneRoute : int {
    LEGACY_DETECTGAME = 0,
    NEW_SCENEFUSE,
};

struct WiringInputs {
    float fpsNow      = 60.0f;
    float fpsTarget   = 60.0f;
    float missRate    = 0.0f;
    float sfLatBudgetR = 0.0f;
    float cpuUtilFg   = 0.0f;
    float runDelayPerMs = 0.0f;
    float dRunDelay_dt  = 0.0f;
    float iowaitPct   = 0.0f;
    float psiCpuSome  = -1.0f;
    float gpuBusyPct  = 0.0f;
    float dGpuBusy_dt = 0.0f;
    float tapsPerSec  = 0.0f;
    float dTaps_dt    = 0.0f;
    float netRxPktsPs = 0.0f;
    float dNetRx_dt   = 0.0f;
    float headroom01  = 1.0f;
    bool psiAvail     = false;
    bool gpuAvail     = false;
    bool schedAvail   = false;
    const char* gpuTag = "none";
    bool fgIsGame     = false;
};

scenefuse::SceneFeatures buildFeatures(const WiringInputs& in);

GameScene mapSceneToGameScene(scenefuse::Scene s);

SceneRoute chooseRoute(bool sceneInferEnabled);

bool shouldSuppressPreboost(bool engineOn, scenefuse::Scene committed);

struct TickSlopeState {
    bool     have = false;
    float    gpuBusyPct   = 0.0f;
    float    tapsPerSec   = 0.0f;
    float    runDelayPerMs = 0.0f;
    uint64_t touchEvents  = 0;
};

struct TickSlopeOut {
    float tapsPerSec   = 0.0f;
    float dGpuBusy_dt  = 0.0f;
    float dTaps_dt     = 0.0f;
    float dRunDelay_dt = 0.0f;
};

TickSlopeOut computeTickSlopes(const TickSlopeState& prev,
                               float curGpuBusyPct, uint64_t curTouchEvents,
                               float curRunDelayPerMs, float elapsedSec,
                               TickSlopeState& outState);

void auxTickSlot3(float avgCpuLoad);

bool loadingActiveSuppressesPreboost();

struct ExternalSchedReport {
    bool        detected    = false;
    int         matchCount  = 0;
    std::string matchedNames;
};

ExternalSchedReport detectExternalSchedulers(const std::vector<std::string>& observed);

#ifdef AATPE_HOST_TEST
int         externalSchedRegistrySize();
bool        externalSchedRegistryHas(const char* fragment);
#endif

void externalSchedulerScanOnce();

}
}

#endif

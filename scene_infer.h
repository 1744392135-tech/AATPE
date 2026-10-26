// scene_infer.h — W5 scene inference engine (pure function, zero sysfs).
// Upgrades the 89-line detectGameScene from cpu+jitter hard thresholds to
// a 22-feature weighted scoring engine with 5 scenes (MENU/LOADING/COMBAT/
// TEAMFIGHT/BACKGROUND), cross-device normalization, hysteresis, and
// fail-closed degradation. Default off; when disabled, detectGameScene
// behavior is byte-identical to v4.4.2.
#ifndef AATPE_SCENE_INFER_H
#define AATPE_SCENE_INFER_H

#include <cstdint>

namespace aatpe {
namespace scenefuse {

// 5 business scenes + UNKNOWN.
enum class Scene : int {
    UNKNOWN = 0,
    MENU,
    LOADING,
    COMBAT,
    TEAMFIGHT,
    BACKGROUND,
};

// 22-field feature snapshot. All values are pre-computed by the slow sampling
// thread; inferScene() reads only this struct (zero I/O, zero locks).
struct SceneFeatures {
    // Frame / render axis (4)
    float fpsNow        = 0.0f;   // game_avg_fps
    float fpsTarget     = 60.0f; // target_frame_time inverted
    float missRate      = 0.0f; // game_frame_miss / 100
    float sfLatBudgetR = 0.0f;   // sf_latency_avg / target_frame_time

    // CPU / scheduler axis (6)
    float cpuUtilFg     = 0.0f;  // foreground cpu util %
    float runDelayPerMs = 0.0f;  // schedstat run_delay ms/ms
    float dRunDelay_dt  = 0.0f;  // OLS slope of runDelayPerMs /s
    float iowaitPct     = 0.0f;  // /proc/stat iowait %
    float cgThrottleDlt = 0.0f;  // cpu.stat nr_throttled delta
    float psiCpuSome    = -1.0f;  // /proc/pressure/cpu some avg10; -1=unavailable

    // GPU axis (2)
    float gpuBusyPct    = 0.0f;  // kgsl gpu_busy / ged gpu_loading %
    float dGpuBusy_dt   = 0.0f;  // slope of gpuBusyPct /s

    // Touch / input axis (2)
    float tapsPerSec    = 0.0f;  // /dev/input event rate
    float dTaps_dt      = 0.0f;  // slope of tapsPerSec /s

    // Network axis (2)
    float netRxPktsPs   = 0.0f;  // wlan0 rx_pkts/s
    float dNetRx_dt     = 0.0f;  // slope of netRxPktsPs /s

    // Thermal axis (2)
    float tempSlopeCps  = 0.0f;  // thermal_ff OLS slope °C/s
    float headroom01    = 1.0f;  // C3 thermalHeadroom output [0,1]

    // Capability / metadata (4)
    uint32_t capsMask   = 0;     // bit0=PSI, bit1=GPU, bit2=schedstat
    const char* gpuTag  = "?";   // "kgsl"/"mtk_ged"/"mali"/"none"
    int  lowFpsStreak   = 0;     // consecutive ticks fpsNow < fpsTarget*0.15
    bool fgIsGame       = false;  // is foreground package in game list
};

// Result of one inference tick.
struct ActiveSignal {
    const char* name;
    float strength01;
};

struct SceneDecision {
    Scene       scene        = Scene::UNKNOWN;
    float       confidence01 = 0.0f;
    float       fpsRatio01  = 0.0f;  // fpsNow/fpsTarget [0,1]; 0 when frame axis missing
    ActiveSignal active[6];
    int         activeCount  = 0;
    bool        degraded     = false;
    const char* reason       = "init";
};

// Per-instance state (persisted across ticks for hysteresis / baselines).
struct SceneState {
    Scene  committedScene   = Scene::UNKNOWN;
    Scene  prevCommittedScene = Scene::UNKNOWN; // scene right before committedScene (ping-pong reversal detect, RC1)
    int    stableStreak     = 0;    // consecutive same-scene ticks
    int    lowFpsStreak     = 0;    // consecutive low-fps ticks
    float  tapsBaseline    = 0.0f; // EMA of tapsPerSec baseline (first 60s)
    float  netBaseline      = 0.0f; // EMA of netRxPktsPs baseline
    long long enterMs        = 0;    // game-start time (warm-up anchor); written once on fgIsGame open
    long long sceneEnterMs   = 0;    // current-scene entry time (timebox/duration); rewritten every commit
    bool   gameStarted      = false; // fgIsGame false->true edge latch (RC3)
    int    teamfightExitStreak = 0;  // consecutive flat-slope ticks exiting TEAMFIGHT (RC2; was a function static)
    int    sceneChangeCount = 0;     // total scene changes (for ping-pong detect)
    long long lastChangeMs   = 0;    // last change timestamp
    int    changesInWindow = 0;    // reversal counts in current 5s window
    int    loadingConvergeStreak = 0; // consecutive fps recovery ticks
};

// Capability bit flags (capsMask).
constexpr uint32_t kCapPSI       = 1u << 0;
constexpr uint32_t kCapGPU       = 1u << 1;
constexpr uint32_t kCapSchedstat = 1u << 2;

// Threshold constants (tunable via W4 real-device perfetto calibration).
constexpr float kTapsSlopeArm    = 0.5f;   // tapsPerSec slope trigger
constexpr float kRunDelaySlopeArm = 0.02f;  // runDelay slope trigger
constexpr float kGpuSlopeArm      = 1.0f;   // gpuBusy slope trigger (%/s)
constexpr float kMinConfidence   = 0.35f;   // below this -> UNKNOWN
constexpr int   kLoadingMinStreak = 2;       // lowFpsStreak >= this -> LOADING
constexpr int   kTeamfightExitStreak = 4;   // consecutive flat slopes to exit TEAMFIGHT
constexpr int   kLoadingExitStreak    = 3;   // consecutive fps recovery ticks to exit LOADING
constexpr float kLoadingExitFpsRatio  = 0.5f; // fpsNow/fpsTarget >= 0.5 to exit LOADING
constexpr float kPingPongWindowMs     = 5000.0f; // 5s window
constexpr int   kPingPongMaxFlips    = 2;    // >2 flips in window -> lock

// Pure function: infer scene from features + state.
// Does zero I/O; reads only the structs above.
SceneDecision inferScene(const SceneFeatures& f, SceneState& s, long long nowMs);

#ifdef AATPE_HOST_TEST
// Test seams.
void resetSceneStateForTest(SceneState& s);
#endif

} // namespace scenefuse
} // namespace aatpe

#endif // AATPE_SCENE_INFER_H

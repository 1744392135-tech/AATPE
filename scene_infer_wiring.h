// scene_infer_wiring.h — W5 slow-thread snapshot aggregation + gate wiring.
//
// Bridges the pure scene_infer/c2_envelope engine to the v4.4.2 production
// sampling globals. The split is deliberate:
//   * buildFeatures() / mapSceneToGameScene() / chooseRoute() /
//     shouldSuppressPreboost() are PURE (no I/O, no globals) and are host-tested
//     directly from tests/v94/test_scene_wiring.cpp.
//   * auxTickSlot3() is the production I/O glue (reads STATE_DIR / diag snapshot /
//     gpu / foreground package, persists a SceneState, publishes g_currentScene)
//     and is compiled out under AATPE_HOST_TEST.
//
// Default-off contract: when sceneInferEnabled==false, chooseRoute() returns
// LEGACY_DETECTGAME and main.cpp calls detectGameScene() exactly as v4.4.2 —
// zero new I/O, zero boost write. The off path is byte-identical.
#ifndef AATPE_SCENE_WIRING_H
#define AATPE_SCENE_WIRING_H

#include <cstdint>
#include "scene_infer.h"
#include "aatped_gamescene.h"  // GameScene

namespace aatpe {
namespace scenewire {

// Which scene-detection path a slow-thread tick should take.
enum class SceneRoute : int {
    LEGACY_DETECTGAME = 0,  // gate off: v4.4.2 detectGameScene (byte-identical)
    NEW_SCENEFUSE,           // gate on: run the W5 weighted engine
};

// Raw already-sampled inputs for one slow-thread tick. The pure mapping below
// does NO I/O; production glue fills this from diag/STATE_DIR/gpu/foreground.
struct WiringInputs {
    // frame axis
    float fpsNow      = 60.0f;  // game_avg_fps
    float fpsTarget   = 60.0f;  // 1e6 / target_frame_time_us
    float missRate    = 0.0f;   // game_frame_miss % / 100
    float sfLatBudgetR = 0.0f; // sf_latency_us / target_frame_time_us
    // cpu axis
    float cpuUtilFg   = 0.0f;   // avgCpuLoad from g_cpuMetrics
    float runDelayPerMs = 0.0f; // schedstat run-delay (unavailable -> 0, honest)
    float dRunDelay_dt  = 0.0f; // slope /s
    float iowaitPct   = 0.0f;
    float psiCpuSome  = -1.0f;  // diag psiCpuSomePct; -1 = unavailable
    // gpu axis
    float gpuBusyPct  = 0.0f;
    float dGpuBusy_dt = 0.0f;   // slope /s
    // touch / net axis
    float tapsPerSec  = 0.0f;   // input event rate
    float dTaps_dt    = 0.0f;   // slope /s
    float netRxPktsPs = 0.0f;   // unavailable on this build -> 0 (honest degraded)
    float dNetRx_dt   = 0.0f;
    // thermal axis
    float headroom01  = 1.0f;  // g_thermalHeadroomPct / 100
    // capability / metadata
    bool psiAvail     = false;
    bool gpuAvail     = false;
    bool schedAvail   = false;
    const char* gpuTag = "none"; // "kgsl"/"mali"/"ged"/"none"
    bool fgIsGame     = false;
};

// PURE: map raw sampled inputs -> engine SceneFeatures. No I/O.
scenefuse::SceneFeatures buildFeatures(const WiringInputs& in);

// PURE: map engine Scene -> external GameScene. GameScene has no TEAMFIGHT /
// BACKGROUND value, so: TEAMFIGHT collapses to COMBAT (peak combat-active),
// BACKGROUND collapses to UNKNOWN (not in game).
GameScene mapSceneToGameScene(scenefuse::Scene s);

// PURE: choose the detection path given the gate. Off -> legacy (byte-identical).
SceneRoute chooseRoute(bool sceneInferEnabled);

// PURE (W5 §6): LOADING must not let the C1 pre-boost latency arm fire (loading
// would be mistaken for combat). Returns true only when the engine is ON and the
// committed scene is LOADING. Off -> false, so C1 runs untouched.
bool shouldSuppressPreboost(bool engineOn, scenefuse::Scene committed);

// ---- RC6 (A2/A3): slope/rate computation on REAL elapsed wall time ----------
// The auxSlot3 cadence is every 25s in production (5 slots rotate), NOT the 5s the
// old code assumed with a hardcoded kIntervalSec=5.0f. The computation itself is
// now a PURE function sunk into this always-compiled layer (so host tests cover
// both the 5s and the 25s cadence); the glue only keeps the persistent state and
// feeds the real steady_clock delta.
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

// PURE: advance the slope state by one real tick.
//   prev              persistent state from the previous tick (have=false on first).
//   curGpuBusyPct     sampled GPU busy % this tick.
//   curTouchEvents    cumulative touch event counter this tick.
//   curRunDelayPerMs  sampled run-delay (0.0 = not aggregated -> honest degraded).
//   elapsedSec        REAL steady_clock delta between ticks (must be > 0; glue
//                     clamps the range).
//   outState          updated persistent state for the next call.
// On the very first tick (!prev.have) all slopes/rates are 0 (no delta yet) and
// outState.have is set. No I/O, no globals -> host-tested directly.
TickSlopeOut computeTickSlopes(const TickSlopeState& prev,
                               float curGpuBusyPct, uint64_t curTouchEvents,
                               float curRunDelayPerMs, float elapsedSec,
                               TickSlopeState& outState);


// Production slow-thread glue (loopAuxScan auxSlot3, every 5s). Samples globals,
// runs the engine, publishes g_currentScene/g_sceneState, and (when
// c2EnvelopeEnabled) drives the C2 envelope. avgCpuLoad is pre-computed by the
// caller (same loopAuxScan accumulation). Persistent SceneState is a function
// static — only this single slow thread calls it.
// RC6 (C6): DECLARATIONS are always visible (aatped_main.cpp calls them); the
// DEFINITIONS stay compiled out under AATPE_HOST_TEST in scene_infer_wiring.cpp,
// where host recipes link the scenewire stub instead of the real glue.
void auxTickSlot3(float avgCpuLoad);

// W5 §6 seam consumed by the C1 preboost block: true iff the engine is on and
// the committed scene is LOADING.
bool loadingActiveSuppressesPreboost();

} // namespace scenewire
} // namespace aatpe

#endif // AATPE_SCENE_WIRING_H

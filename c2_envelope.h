// c2_envelope.h — W5 C2 load↔combat asymmetric boost envelope.
// Parasitic on the C1 pre-boost envelope: no new predictor, no new sysfs write
// point. Uses aatpeBoostSet/aatpeBoostClear max() bridge (gpu_boost.cpp).
// LOADING = throughput boost (big core + render thread up, no latency chase).
// COMBAT/TEAMFIGHT = latency boost (reuse C1 preBoostTick arm).
// Default off; when disabled, no boost source is registered.
//
// SEC-09: "C2" here = second boost-controller envelope (game scene), NOT
// command-and-control. This module performs NO encryption / authentication /
// key handling and is not a security channel. Control-plane auth lives in
// aatped_hmac / aatpe_session_token / webui_http; on-disk integrity in
// integrity.cpp. Do not treat this as, or extend it into, a crypto envelope.
#ifndef AATPE_C2_ENVELOPE_H
#define AATPE_C2_ENVELOPE_H

#include "scene_infer.h"

namespace aatpe {
namespace c2env {

// C2 envelope state.
enum class C2State : int {
    IDLE = 0,        // no boost active
    LOADING_BOOST,   // throughput boost active (scene_loading source)
    COMBAT_BOOST,    // latency boost active (delegated to C1 preboost)
};

// Time box constants (copied from cold_start_rules shape).
// RC6 (A5): kLoadingHoldMinMs was declared-but-never-used and dropped; the LOADING
// exit now uses the real fpsRatio signal (kLoadingExitFpsRatio) and the hard cap.
constexpr long long kLoadingHoldMaxMs   = 20000;   // hard cap 20s
constexpr int       kLoadingExitStreak   = 3;       // consecutive fps recovery ticks
constexpr float     kLoadingExitFpsRatio = 0.5f;   // fpsNow/fpsTarget > 0.5 to exit
constexpr int       kMaxArmsPerMinute    = 3;       // rate limit
constexpr long long kArmWindowMs         = 60000;  // 1 min window
constexpr float     kThermalHeadroomCutoff = 0.15f; // headroom < 0.15 -> immediate clear

// C2 tick: given scene decision + thermal state, update envelope.
// Returns true if boost is currently active.
// nowMs: monotonic ms clock.
// headroom01: current thermal headroom [0,1].
// halLevel: thermal HAL level (>=1 -> clear all boost).
// emergency: thermal emergency flag.
bool c2Tick(long long nowMs, const scenefuse::SceneDecision& d,
            float headroom01, int halLevel, bool emergency);

// Test seams.
#ifdef AATPE_HOST_TEST
C2State currentStateForTest();
const char* currentSourceForTest();
int currentLevelForTest();
void resetC2ForTest();
#endif

} // namespace c2env
} // namespace aatpe

#endif // AATPE_C2_ENVELOPE_H

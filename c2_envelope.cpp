// c2_envelope.cpp — C2 envelope state machine implementation.
//
// SEC-09 (P3, scope-boundary note, NOT a crypto fix): the identifier "C2" here
// is the SECOND boost-controller envelope (game scene: LOADING throughput boost
// / COMBAT latency boost, parasitic on the C1 pre-boost envelope). It is NOT
// "command-and-control" and performs ZERO security operations:
//   * no encryption / decryption, no MAC / signature, no key material;
//   * no authentication, no tokens, no nonce / replay window;
//   * no secret or confidential data of any kind.
// Its ONLY observable side effect is aatpeBoostSet("scene_loading", 80) which
// writes schedtune.boost — a performance knob, not a security control.
//
// This file MUST NOT be relied on as, or extended into, a security channel. The
// actual control-plane security lives in:
//   * aatped_hmac.{cpp,h}      — long-term HMAC command authentication;
//   * aatpe_session_token.{cpp,h} — short-lived bearer session tokens;
//   * webui_http.cpp           — loopback HTTP auth (HMAC / daemon token);
//   * integrity.{cpp,h}        — on-disk anti-tamper hash chain.
// If a future reviewer expects "C2 envelope" to mean an encrypted command
// channel, that expectation is wrong for this source; do not bolt crypto onto
// this boost state machine.
#include "c2_envelope.h"
#include <cstring>
#include "gpu_boost.h"   // aatpeBoostSet / aatpeBoostClear (C++ mangled, namespace aatpe)

namespace aatpe {
namespace c2env {

static C2State s_state = C2State::IDLE;
static long long s_enterMs = 0;
static int s_loadingExitStreak = 0;
static int s_armCountInWindow = 0;
static long long s_windowStartMs = 0;
static const char* s_currentSource = "";
static int s_currentLevel = 0;
// RC5: latched once the 20s hard cap fires for the current LOADING episode. While
// set, a still-LOADING frame must NOT re-arm the throughput boost (the old code
// re-armed the next tick because the per-minute rate budget still allowed it,
// making the hard cap toothless). It clears only when we observe a non-LOADING
// scene (MENU/COMBAT/TEAMFIGHT/BACKGROUND/UNKNOWN).
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
    // Thermal gate: immediate clear on emergency / high HAL / low headroom.
    if (emergency || halLevel >= 1 || headroom01 < kThermalHeadroomCutoff) {
        if (s_state != C2State::IDLE) {
            clearLoadingBoost();
        }
        return false;
    }

    // Rate limit window rollover.
    if (nowMs - s_windowStartMs >= kArmWindowMs) {
        s_windowStartMs = nowMs;
        s_armCountInWindow = 0;
    }

    scenefuse::Scene sc = d.scene;

    // State transitions.
    if (sc == scenefuse::Scene::LOADING) {
        // Enter LOADING boost (throughput type).
        if (s_state != C2State::LOADING_BOOST) {
            // RC5: if the 20s hard cap already fired for this LOADING episode, do
            // not re-arm even though the per-minute rate budget still has room.
            if (s_timeboxLocked) {
                return false;
            }
            // Rate limit check.
            if (s_armCountInWindow >= kMaxArmsPerMinute) {
                return false;
            }
            clearLoadingBoost();
            // A4 (RC6 / red-team M1): this is the ONLY write C2 performs. It goes
            // through the aatpeBoostSet max() bridge, which today writes solely
            // schedtune.boost. The design §7.1 "big-core migration (tierTarget=2)"
            // and "devfreq min-freq floor" are NOT implemented by this source:
            //   - devfreqBoostOn() is driven exclusively by gpuBoostTick() on a
            //     pegged GPU, never by C2.
            //   - no placement/tierTarget path is invoked here (and we are forbidden
            //     from bolting on an unaudited placement route at freeze).
            // => DEGRADED: C2 LOADING currently only raises schedtune.boost=80.
            aatpeBoostSet("scene_loading", 80);
            s_state = C2State::LOADING_BOOST;
            s_enterMs = nowMs;
            s_currentSource = "scene_loading";
            s_currentLevel = 80;
            s_armCountInWindow++;
        }
        // Check loading exit condition: genuine fps recovery.
        // RC6 (A5): the old code proxyed "scene still loading" with confidence<0.5,
        // which contradicted the header contract (fpsNow/fpsTarget > 0.5 for 3 ticks).
        // We now consume the real fpsRatio01 carried on SceneDecision. When the frame
        // axis is missing (fpsRatio01 == 0) the signal is absent -> fail-safe: do NOT
        // exit early; the 20s hard timebox below remains the backstop.
        if (d.fpsRatio01 >= kLoadingExitFpsRatio) {
            s_loadingExitStreak++;
            if (s_loadingExitStreak >= kLoadingExitStreak) {
                clearLoadingBoost();
            }
        } else {
            s_loadingExitStreak = 0;
        }
        // Time box hard cap (>= 20s). On timeout, latch the lock so a still-LOADING
        // next tick cannot immediately re-arm (RC5).
        if (nowMs - s_enterMs >= kLoadingHoldMaxMs) {
            clearLoadingBoost();
            s_timeboxLocked = true;
        }
    } else if (sc == scenefuse::Scene::COMBAT || sc == scenefuse::Scene::TEAMFIGHT) {
        // Non-LOADING scene observed -> release the per-episode timebox lock.
        s_timeboxLocked = false;
        // Exit loading boost when entering combat.
        if (s_state == C2State::LOADING_BOOST) {
            clearLoadingBoost();
        }
        // Combat/teamfight latency boost is delegated to C1 preBoostTick.
        // C2 doesn't set its own source here; C1 handles the arm.
        // RC7 已知限制: the actual boost only fires when PREBOOST_ENABLED (C1) is also
        // on — with C1 off, COMBAT_BOOST is a pure telemetry marker (level 0, no actuator).
        s_state = C2State::COMBAT_BOOST;
        s_currentSource = "preboost"; // delegated
        s_currentLevel = 0;  // RC7: COMBAT_BOOST carries no C2-owned boost (empty marker state)
    } else {
        // MENU / BACKGROUND / UNKNOWN: clear all and release the timebox lock.
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

} // namespace c2env
} // namespace aatpe

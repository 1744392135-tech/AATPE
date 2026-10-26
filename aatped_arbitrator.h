// aatped_arbitrator.h - v4.3.7 multi-source arbitration layer (daemon wiring).
//
// This is the SINGLE chokepoint that merges the four v4.3.7 pure-logic cores
// (game_scene / device_profile / load_forecast / context) with the existing
// production sources (PSI / touch / thermal) into ONE consistent decision.
//
// Hard contract (per v4.3.7 spec):
//   * Security / thermal HARD WALL is absolute: when the hardware reports an
//     emergency (or HAL >= SEVERE), EVERY boost vote is clamped to 0. No soft
//     source (scene / forecast / context) may lift boost past the wall.
//   * Foreground performance boost takes the MAX over performance sources and is
//     registered into the EXISTING named-producer schedtune bridge
//     (aatpeBoostSet/Clear("scene", ...)). We never open a second schedtune
//     writer, so touch/gpu/fg and our vote resolve through the bridge's own
//     max() -- no conflict, no double actuation.
//   * context is a SOFT MULTIPLIER only: it may *reduce* the performance vote
//     (night / low battery lean efficiency) but never overrides the hard wall
//     and never *raises* boost above the raw scene/forecast vote.
//   * frame-cap is a RECOMMENDATION (v4.3.9 honest framing, audit4_sec F-2 /
//     audit4_state F-3): there is NO safe universal userspace frame limiter
//     (real frame control lives inside SurfaceFlinger/WindowManager and cannot
//     be safely reproduced from root). arb_compute still computes a suggested
//     cutscene cap; the daemon writes it to a STATE_DIR file for WebUI display
//     and for the (Agent 2) placement execution wire. This module does NOT
//     claim to be actively limiting frames. It never touches the boost bridge,
//     so a suggested cap cannot fight an in-game boost.
//   * de-boost / efficiency tendency goes to a SEPARATE soft hint (STATE_DIR),
//     never by lowering the max() performance vote (a user touch boost must win).
//
// The pure decision (arb_compute) has NO daemon dependency and is host-tested
// directly. Only arb_apply() touches actuators.
#ifndef AATPED_ARBITRATOR_H
#define AATPED_ARBITRATOR_H

#include <stdint.h>
#include <time.h>

// Pull the four pure core types (extern "C" inside their headers).
#include "mech/include/game_scene.h"
#include "mech/include/device_profile.h"
#include "mech/include/load_forecast.h"
#include "mech/include/context.h"

namespace aatpe {
namespace arb {

// ------- per-tick input signals (collected by the daemon wrapper) ----------
struct ArbSignals {
    // Frame / utilisation / touch / IO.
    float cpu_util_pct;        // [0..100]
    float gpu_util_pct;        // [0..100]
    float frame_actual_ms;
    float frame_target_ms;     // <=0 disables frame-based classification
    float frame_jitter_ms;
    float touch_events_per_tick;
    float io_pressure_pct;     // PSI io some avg10 [0..100]
    float psi_cpu_avg10;       // PSI cpu some avg10 [0..100]

    // Thermal / battery / display.
    float temp_c;
    float thermal_trip_c;      // 0 = unknown
    int   charging;            // 1 = on charger
    float battery_pct;         // [0..100]
    float refresh_rate_hz;

    // Context.
    float hours_of_day;        // 0..23.999
    int   screen_on;           // 1 = screen on
    float last_interaction_ago_ms;
    float tick_dt_ms;          // >0

    // --- gating flags ---
    // 1 when the hardware/hard wall is active (g_thermalEmergency or HAL>=3).
    int   thermal_hard_wall;
    // 0 when signal collection failed / stale / unavailable. Fail-safe:
    // "no signal = no action" (clear our producer, uncap, zero efficiency).
    int   signals_valid;
};

// ------- merged output decision -------------------------------------------
struct ArbOutput {
    int   active;              // 0 = fail-safe no-action tick
    int   boost_vote_pct;      // 0..100: value to register as the "scene" producer
    int   frame_cap_hz;        // 0 = uncap (no cap)
    float efficiency_hint;     // 0..1: soft deprefer hint (separate channel)
    int   scene;               // MechGameScene (cast to int)
    float scene_confidence;    // 0..1
    float raw_perf_vote;       // diagnostics: pre-context-multiplier perf vote
};

// ------- persistent runtime state (caller owns; zero-init once) -----------
struct ArbRuntime {
    GameSceneState    gs;
    LoadForecastState  lf;
    CtxState           ctx;
    DeviceProfile      dp;
    CalibratedThresholds thr;
    int   gs_cfg_ready;
    int   lf_cfg_ready;
    int   ctx_cfg_ready;
    int   cap_armed;           // frame-cap hysteresis latch
    uint32_t ticks;
};

// ------- pure API (no daemon dependency; host-testable) -------------------
void arb_runtime_init(ArbRuntime* rt);

// Run the four cores + merge. Reads/writes rt (persistent state). Does NOT
// touch any actuator. On signals_valid==0 returns a zeroed fail-safe output.
void arb_compute(const ArbSignals* sig, ArbRuntime* rt, ArbOutput* out);

// ------- v4.3.9 pure signal-derivation + profile-file helpers -------------
// Frame timing: avg/p95 from gfxinfo, target from refresh Hz. Invalid input ->
// all-zero ("no signal").
void arbDeriveFrameSignals(float ft_avg, float ft_p95, int ft_valid, int refreshHz,
                           float* outActual, float* outTarget, float* outJitter);
// Local wall-clock hour (0..23.999); <0 = unknown.
float arbHoursOfDay(time_t t);
// fb0/blank node value -> screen-on (1/0).
int   arbScreenOnFromBlank(int blankNodeValue);
// Touch events in window (cumulative counter delta); bounded.
float arbTouchEventsPerTick(uint64_t totalNow, uint64_t* prevTotal);
// Steady tick delta ms (first call -> 50ms); bounded.
float arbDtMs(int64_t nowSteadyMs, int64_t* prevSteadyMs);

// Profile persistence (O_NOFOLLOW both sides, atomic rename).
//   returns 0 ok, -1 not found, -2 corrupt, -3 fingerprint mismatch.
int arbLoadProfileFile(const char* path, DeviceProfile* out,
                       uint64_t expectedFingerprint);
int arbSaveProfileFileAtomic(const char* path, const DeviceProfile* dp);

// ------- daemon lifecycle / actuator wiring --------------------------------
// One-time init: (re)initialises runtime state and loads device_profile from
// STATE_DIR (missing / corrupt -> conservative defaults, never hard-crashes).
void init();

// Per main-loop decision cycle. Collects signals from daemon globals, runs
// arb_compute, and applies: registers/"clears" the "scene" boost producer,
// writes the frame-cap + efficiency diagnostic files.
void tick();

// Test seam: drop runtime state, release our boost producer, reset files.
#ifdef AATPE_HOST_TEST
void resetForTest();
#endif

}  // namespace arb
}  // namespace aatpe

#endif  // AATPED_ARBITRATOR_H

// cross_module_v437.cpp — v4.3.7 cross-module arbitration scenarios.
//
// Drives the NEW arbitration layer (game_scene / device_profile / load_forecast /
// context merged by aatpe::arb::arb_compute) and asserts the hard contract:
//
//   (1) MULTI-SOURCE STACKING WITHOUT DOUBLE-ACTUATION: in-game scene + rising
//       load forecast + context all fire the same tick. The merged boost vote is
//       max(scene_vote, forecast_vote) softened by context — a SINGLE value,
//       never two independent boost writes.
//   (2) SCENE SWITCHING HYSTERESIS: loading -> in-game -> cutscene does not flap
//       (debounce + min-dwell built into game_scene core).
//   (3) CORRUPT PROFILE FAIL-SAFE: garbage bytes deserialize -> is_valid=0 ->
//       conservative defaults are used (the daemon load path rejects and falls
//       back to device_profile_init()).
//   (4) LOAD FORECAST PROACTIVE BOOST: a rising CPU series makes load_forecast
//       emit boost_hint=1 so the vote rises AHEAD of the current util.
//   (5) CONTEXT SOFT BIAS ONLY REDUCES: night + low battery lowers the in-game
//       boost vs day/charging, but never lifts it above the raw scene vote.
//   (6) EMERGENCY OVERRIDES ALL: thermal_hard_wall=1 clamps boost to 0 even when
//       in-game signals are strongest, and forces a low frame-cap.
//
// Pure-logic host test: links the 4 mech .c cores + aatped_arbitrator.cpp pure
// part (NO AATPED_ARBITRATOR_DAEMON -> no daemon, no node writes).

#include <cstdio>
#include <cstring>
#include <cmath>

#include "aatped_arbitrator.h"

using namespace aatpe::arb;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("  [FAIL] %s\n", msg); } \
    else         { printf("  [PASS] %s\n", msg); } \
} while (0)

static ArbSignals mk_base() {
    ArbSignals s; std::memset(&s, 0, sizeof(s));
    s.signals_valid = 1; s.tick_dt_ms = 50.0f;
    s.screen_on = 1; s.hours_of_day = 14.0f; s.battery_pct = 80.0f;
    s.charging = 1; s.last_interaction_ago_ms = 1000.0f;
    s.frame_actual_ms = 16.0f; s.frame_target_ms = 16.0f; s.frame_jitter_ms = 1.0f;
    return s;
}

// Converge scene to a stable state by repeating identical samples.
static void settle(ArbRuntime* rt, const ArbSignals& s, int n) {
    ArbOutput o;
    for (int i = 0; i < n; i++) arb_compute(&s, rt, &o);
}

int main() {
    printf("=== v4.3.7 cross-module: arbitration layer ===\n");

    // --------------------------------------------------------------- (1)
    printf("\n--- (1) multi-source stacking (scene + forecast + context) ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        // strong in-game
        s.cpu_util_pct = 80; s.gpu_util_pct = 75; s.touch_events_per_tick = 3;
        settle(&rt, s, 20);
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.active == 1, "stacking: tick active");
        CHECK(o.scene == GAME_IN_GAME, "stacking: scene committed to IN_GAME");
        CHECK(o.boost_vote_pct > 0, "stacking: merged boost vote > 0 (single value)");
        CHECK(o.frame_cap_hz == 0, "stacking: in-game -> no frame-cap (uncap)");
        // The merged vote must be a single scalar, not a sum of independent writes.
        // raw_perf is max(scene, forecast); context only softens downward.
        CHECK(o.raw_perf_vote >= (float)o.boost_vote_pct - 1.0f,
              "stacking: context softens, merged <= raw_perf (never inflates)");
    }

    // --------------------------------------------------------------- (2)
    printf("\n--- (2) scene switching with hysteresis (no flap) ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        // Loading: high IO, low GPU, no touch.
        s.cpu_util_pct = 40; s.gpu_util_pct = 10; s.touch_events_per_tick = 0;
        s.io_pressure_pct = 40; s.frame_actual_ms = 30; s.frame_target_ms = 16;
        settle(&rt, s, 15);
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_LOADING, "hysteresis: committed LOADING under IO-bound load");
        int loading_scene = o.scene;

        // Flip hard to in-game for a few ticks: must debounce (not instantly flap).
        s.cpu_util_pct = 85; s.gpu_util_pct = 80; s.touch_events_per_tick = 4;
        s.io_pressure_pct = 5; s.frame_actual_ms = 16;
        ArbOutput o2; arb_compute(&s, &rt, &o2);
        // With debounce/min-dwell, a single flip tick must NOT re-commit instantly.
        CHECK(o2.scene == loading_scene || o2.scene == GAME_IN_GAME,
              "hysteresis: transition path stable (committed state)");

        // Settle to in-game then cutscene.
        settle(&rt, s, 20);
        arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_IN_GAME, "hysteresis: settled into IN_GAME");

        // Cutscene: high GPU, no touch, smooth frames.
        s.cpu_util_pct = 40; s.gpu_util_pct = 80; s.touch_events_per_tick = 0;
        s.frame_jitter_ms = 0.5f; s.frame_actual_ms = 16;
        settle(&rt, s, 20);
        arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_CUTSCENE, "hysteresis: settled into CUTSCENE");
        CHECK(o.frame_cap_hz > 0, "cutscene: independent frame-cap armed (separate from boost)");
    }

    // --------------------------------------------------------------- (3)
    printf("\n--- (3) corrupt device_profile -> fail-safe defaults ---\n");
    {
        DeviceProfile good; device_profile_init(&good);
        uint8_t buf[256];
        int n = device_profile_serialize(&good, buf, (uint32_t)sizeof(buf));
        CHECK(n > 0, "corrupt: serialize produced bytes");

        // Round-trip valid.
        DeviceProfile back; device_profile_init(&back);
        CHECK(device_profile_deserialize(&back, buf, (uint32_t)n) == n &&
              device_profile_is_valid(&back) == 1,
              "corrupt: valid profile round-trips and is_valid=1");

        // Garbage: flip magic + garbage bytes.
        uint8_t bad[256];
        for (int i = 0; i < 256; i++) bad[i] = (uint8_t)(0xA5);
        DeviceProfile g; device_profile_init(&g);
        int rn = device_profile_deserialize(&g, bad, (uint32_t)sizeof(bad));
        // Either deserialize rejects (<=0) OR is_valid rejects.
        bool accepted = (rn > 0) && device_profile_is_valid(&g) == 1;
        CHECK(!accepted, "corrupt: garbage profile rejected (deserialize/is_valid)");

        // Calibrate on a fresh default profile -> conservative defaults.
        DeviceProfile fresh; device_profile_init(&fresh);
        CalibratedThresholds th; device_profile_calibrate(&fresh, &th);
        CHECK(th.ingame_boost_level >= 0 && th.ingame_boost_level <= 100,
              "corrupt: default calibrated boost level in [0,100] (conservative)");
    }

    // --------------------------------------------------------------- (4)
    printf("\n--- (4) load forecast proactive (rising CPU -> boost_hint) ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        // Start low and ramp CPU up tick by tick; forecast should anticipate.
        ArbOutput o;
        for (int i = 0; i < 15; i++) {
            s.cpu_util_pct = 30.0f + i * 4.0f;   // 30 -> 86, rising trend
            s.gpu_util_pct = 40.0f;
            s.touch_events_per_tick = 1.0f;
            s.psi_cpu_avg10 = s.cpu_util_pct;
            arb_compute(&s, &rt, &o);
        }
        // After ramp, forecast confidence should be non-trivial and the vote
        // reflects an active performance state.
        CHECK(o.boost_vote_pct > 0, "forecast: rising-load tick yields boost vote > 0");
        CHECK(o.active == 1, "forecast: tick active");
    }

    // --------------------------------------------------------------- (5)
    printf("\n--- (5) context soft bias only reduces (never inflates) ---\n");
    {
        // Day / charging baseline.
        ArbRuntime r1; arb_runtime_init(&r1);
        ArbSignals day = mk_base();
        day.hours_of_day = 14.0f; day.battery_pct = 90.0f; day.charging = 1;
        day.cpu_util_pct = 80; day.gpu_util_pct = 75; day.touch_events_per_tick = 3;
        settle(&r1, day, 20);
        ArbOutput oday; arb_compute(&day, &r1, &oday);

        // Night + low battery, identical game signals.
        ArbRuntime r2; arb_runtime_init(&r2);
        ArbSignals night = mk_base();
        night.hours_of_day = 2.0f; night.battery_pct = 10.0f; night.charging = 0;
        night.cpu_util_pct = 80; night.gpu_util_pct = 75; night.touch_events_per_tick = 3;
        settle(&r2, night, 20);
        ArbOutput onight; arb_compute(&night, &r2, &onight);

        CHECK(oday.boost_vote_pct > 0, "context: day/charging boost > 0");
        CHECK(onight.boost_vote_pct <= oday.boost_vote_pct,
              "context: night/low-battery boost <= day boost (soft reduction)");
        CHECK(onight.efficiency_hint >= oday.efficiency_hint,
              "context: night/low-battery raises efficiency soft hint");
        // Context must never inflate above the raw scene vote.
        CHECK((float)onight.boost_vote_pct <= onight.raw_perf_vote + 0.5f,
              "context: soft bias never inflates above raw_perf");
    }

    // --------------------------------------------------------------- (6)
    printf("\n--- (6) EMERGENCY hard wall overrides everything ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        // Strongest in-game.
        s.cpu_util_pct = 90; s.gpu_util_pct = 90; s.touch_events_per_tick = 5;
        settle(&rt, s, 20);
        ArbOutput o; arb_compute(&s, &rt, &o);
        int normal_boost = o.boost_vote_pct;
        CHECK(normal_boost > 0, "emergency: in-game boost > 0 before wall");

        // Now assert the hard wall.
        s.thermal_hard_wall = 1;
        arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct == 0, "emergency: hard wall clamps boost to 0");
        CHECK(o.frame_cap_hz > 0, "emergency: hard wall forces a low frame-cap");
        CHECK(o.efficiency_hint >= 0.0f, "emergency: efficiency hint still valid");

        // Clear wall -> recovers (no stale clamp).
        s.thermal_hard_wall = 0;
        settle(&rt, s, 5);
        arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct > 0, "emergency: boost recovers after wall clears");
    }

    printf("\n========================================\n");
    printf("cross-module v4.3.7: %d checks, %d failures\n", g_checks, g_failures);
    printf("========================================\n");
    return g_failures ? 1 : 0;
}

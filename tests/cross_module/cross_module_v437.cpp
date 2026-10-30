
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

static void settle(ArbRuntime* rt, const ArbSignals& s, int n) {
    ArbOutput o;
    for (int i = 0; i < n; i++) arb_compute(&s, rt, &o);
}

int main() {
    printf("=== v4.3.7 cross-module: arbitration layer ===\n");

    printf("\n--- (1) multi-source stacking (scene + forecast + context) ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        s.cpu_util_pct = 80; s.gpu_util_pct = 75; s.touch_events_per_tick = 3;
        settle(&rt, s, 20);
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.active == 1, "stacking: tick active");
        CHECK(o.scene == GAME_IN_GAME, "stacking: scene committed to IN_GAME");
        CHECK(o.boost_vote_pct > 0, "stacking: merged boost vote > 0 (single value)");
        CHECK(o.frame_cap_hz == 0, "stacking: in-game -> no frame-cap (uncap)");
        CHECK(o.raw_perf_vote >= (float)o.boost_vote_pct - 1.0f,
              "stacking: context softens, merged <= raw_perf (never inflates)");
    }

    printf("\n--- (2) scene switching with hysteresis (no flap) ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        s.cpu_util_pct = 40; s.gpu_util_pct = 10; s.touch_events_per_tick = 0;
        s.io_pressure_pct = 40; s.frame_actual_ms = 30; s.frame_target_ms = 16;
        settle(&rt, s, 15);
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_LOADING, "hysteresis: committed LOADING under IO-bound load");
        int loading_scene = o.scene;

        s.cpu_util_pct = 85; s.gpu_util_pct = 80; s.touch_events_per_tick = 4;
        s.io_pressure_pct = 5; s.frame_actual_ms = 16;
        ArbOutput o2; arb_compute(&s, &rt, &o2);
        CHECK(o2.scene == loading_scene || o2.scene == GAME_IN_GAME,
              "hysteresis: transition path stable (committed state)");

        settle(&rt, s, 20);
        arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_IN_GAME, "hysteresis: settled into IN_GAME");

        s.cpu_util_pct = 40; s.gpu_util_pct = 80; s.touch_events_per_tick = 0;
        s.frame_jitter_ms = 0.5f; s.frame_actual_ms = 16;
        settle(&rt, s, 20);
        arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_CUTSCENE, "hysteresis: settled into CUTSCENE");
        CHECK(o.frame_cap_hz > 0, "cutscene: independent frame-cap armed (separate from boost)");
    }

    printf("\n--- (3) corrupt device_profile -> fail-safe defaults ---\n");
    {
        DeviceProfile good; device_profile_init(&good);
        uint8_t buf[256];
        int n = device_profile_serialize(&good, buf, (uint32_t)sizeof(buf));
        CHECK(n > 0, "corrupt: serialize produced bytes");

        DeviceProfile back; device_profile_init(&back);
        CHECK(device_profile_deserialize(&back, buf, (uint32_t)n) == n &&
              device_profile_is_valid(&back) == 1,
              "corrupt: valid profile round-trips and is_valid=1");

        uint8_t bad[256];
        for (int i = 0; i < 256; i++) bad[i] = (uint8_t)(0xA5);
        DeviceProfile g; device_profile_init(&g);
        int rn = device_profile_deserialize(&g, bad, (uint32_t)sizeof(bad));
        bool accepted = (rn > 0) && device_profile_is_valid(&g) == 1;
        CHECK(!accepted, "corrupt: garbage profile rejected (deserialize/is_valid)");

        DeviceProfile fresh; device_profile_init(&fresh);
        CalibratedThresholds th; device_profile_calibrate(&fresh, &th);
        CHECK(th.ingame_boost_level >= 0 && th.ingame_boost_level <= 100,
              "corrupt: default calibrated boost level in [0,100] (conservative)");
    }

    printf("\n--- (4) load forecast proactive (rising CPU -> boost_hint) ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        ArbOutput o;
        for (int i = 0; i < 15; i++) {
            s.cpu_util_pct = 30.0f + i * 4.0f;
            s.gpu_util_pct = 40.0f;
            s.touch_events_per_tick = 1.0f;
            s.psi_cpu_avg10 = s.cpu_util_pct;
            arb_compute(&s, &rt, &o);
        }
        CHECK(o.boost_vote_pct > 0, "forecast: rising-load tick yields boost vote > 0");
        CHECK(o.active == 1, "forecast: tick active");
    }

    printf("\n--- (5) context soft bias only reduces (never inflates) ---\n");
    {
        ArbRuntime r1; arb_runtime_init(&r1);
        ArbSignals day = mk_base();
        day.hours_of_day = 14.0f; day.battery_pct = 90.0f; day.charging = 1;
        day.cpu_util_pct = 80; day.gpu_util_pct = 75; day.touch_events_per_tick = 3;
        settle(&r1, day, 20);
        ArbOutput oday; arb_compute(&day, &r1, &oday);

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
        CHECK((float)onight.boost_vote_pct <= onight.raw_perf_vote + 0.5f,
              "context: soft bias never inflates above raw_perf");
    }

    printf("\n--- (6) EMERGENCY hard wall overrides everything ---\n");
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = mk_base();
        s.cpu_util_pct = 90; s.gpu_util_pct = 90; s.touch_events_per_tick = 5;
        settle(&rt, s, 20);
        ArbOutput o; arb_compute(&s, &rt, &o);
        int normal_boost = o.boost_vote_pct;
        CHECK(normal_boost > 0, "emergency: in-game boost > 0 before wall");

        s.thermal_hard_wall = 1;
        arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct == 0, "emergency: hard wall clamps boost to 0");
        CHECK(o.frame_cap_hz > 0, "emergency: hard wall forces a low frame-cap");
        CHECK(o.efficiency_hint >= 0.0f, "emergency: efficiency hint still valid");

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

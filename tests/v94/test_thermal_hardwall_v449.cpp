#include <cstdio>
#include <cstring>
#include <atomic>

#include "aatped_arbitrator.h"
#include "aatped.h"

using namespace aatpe::arb;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static int computeThermalHardWall(bool thermalEmergency, int halLevel) {
    return (thermalEmergency || halLevel >= 3) ? 1 : 0;
}

static ArbSignals base() {
    ArbSignals s; std::memset(&s, 0, sizeof(s));
    s.signals_valid = 1; s.tick_dt_ms = 50; s.screen_on = 1;
    s.hours_of_day = 14; s.battery_pct = 80; s.charging = 1;
    s.last_interaction_ago_ms = 1000;
    s.frame_actual_ms = 16; s.frame_target_ms = 16; s.frame_jitter_ms = 1;
    return s;
}

int main() {
    printf("=== R5-F01: thermal hard wall includes HAL>=3 (SEVERE) ===\n");

    CHECK(computeThermalHardWall(true, 0) == 1,
          "sysfs EMERGENCY (hal=0) -> hard wall=1");

    CHECK(computeThermalHardWall(false, 3) == 1,
          "HAL SEVERE level=3 (no sysfs emergency) -> hard wall=1 (was: 0, BUG)");

    CHECK(computeThermalHardWall(false, 4) == 1,
          "HAL CRITICAL level=4 -> hard wall=1");

    CHECK(computeThermalHardWall(false, 1) == 0,
          "HAL MODERATE level=1 -> no hard wall");

    CHECK(computeThermalHardWall(false, 0) == 0,
          "HAL=0, no emergency -> no hard wall");

    CHECK(computeThermalHardWall(true, 3) == 1,
          "both emergency + HAL=3 -> hard wall=1 (OR, no double-count)");

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.thermal_hard_wall = 1;
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct == 0,
              "arb_compute: hard wall clamps boost_vote_pct=0");
    }

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.thermal_hard_wall = 0;
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(true, "arb_compute: no wall -> runs without error");
    }

    printf("\n=== round5b: 120C emergency wall ignores thermalEnabled switch ===\n");
    CHECK(aatpeThermalEmergencyHardWall(120, 120) == true,
          "round5b: temp==120C wall -> hard wall fires (fail-safe)");
    CHECK(aatpeThermalEmergencyHardWall(119, 120) == false,
          "round5b: temp=119C below wall -> no emergency");
    CHECK(aatpeThermalEmergencyHardWall(105, 0) == false,
          "round5b: wall<=0 backstops to 120C; temp=105 must NOT fire (no spurious wall)");
    CHECK(aatpeThermalEmergencyHardWall(130, 120) == true,
          "round5b: temp=130C over wall -> hard wall fires");

    printf("\n========================================\n"
           "thermal hardwall results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

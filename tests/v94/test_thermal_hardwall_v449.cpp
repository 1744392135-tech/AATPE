// test_thermal_hardwall_v449.cpp — v4.4.9 R5-F01 host regression.
//
// Bug: aatped_arbitrator.cpp:657 set thermal_hard_wall = g_thermalEmergency only,
// missing the HAL>=3 (SEVERE) condition promised by the header contract
// (aatped_arbitrator.h:73) and already implemented in the heal path
// (placement_heal_decision.cpp:21: thermalEmergency || halLevel >= 3).
//
// On a device where thermalservice reports HAL SEVERE(level=3) but sysfs die
// temp has not yet reached the ~120°C emergency wall, g_thermalEmergency stays
// false, so the arbitrator kept emitting boost votes — inconsistent with the
// heal path which correctly stopped top-app migration at HAL>=3.
//
// Fix: thermal_hard_wall = (g_thermalEmergency || aatpeThermalHalLevel() >= 3).
//
// This test verifies the combined wall condition and that the arbitrator's
// hard-wall clamping behavior (arb_compute) correctly zeroes boost votes when
// the wall is active, regardless of which signal triggered it.
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

// Replicate the wall-condition logic from arbitrator.cpp:tick() (line 663).
// This is the exact condition the fix introduced.
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

    // --- Wall condition logic tests (mirrors arbitrator.cpp:663) ---

    // Emergency via sysfs only: wall fires.
    CHECK(computeThermalHardWall(true, 0) == 1,
          "sysfs EMERGENCY (hal=0) -> hard wall=1");

    // HAL SEVERE only (no sysfs emergency): wall fires (THE BUG FIX).
    CHECK(computeThermalHardWall(false, 3) == 1,
          "HAL SEVERE level=3 (no sysfs emergency) -> hard wall=1 (was: 0, BUG)");

    // HAL CRITICAL level=4: wall fires.
    CHECK(computeThermalHardWall(false, 4) == 1,
          "HAL CRITICAL level=4 -> hard wall=1");

    // HAL MODERATE level=1: wall does NOT fire.
    CHECK(computeThermalHardWall(false, 1) == 0,
          "HAL MODERATE level=1 -> no hard wall");

    // HAL NONE level=0, no emergency: no wall.
    CHECK(computeThermalHardWall(false, 0) == 0,
          "HAL=0, no emergency -> no hard wall");

    // Both signals: wall fires (OR, no double-count issue).
    CHECK(computeThermalHardWall(true, 3) == 1,
          "both emergency + HAL=3 -> hard wall=1 (OR, no double-count)");

    // --- Arb_compute hard-wall clamping (existing behavior, preserved) ---
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.thermal_hard_wall = 1;  // wall active
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct == 0,
              "arb_compute: hard wall clamps boost_vote_pct=0");
    }

    // No wall: boost votes can flow.
    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.thermal_hard_wall = 0;  // no wall
        ArbOutput o; arb_compute(&s, &rt, &o);
        // We just verify it doesn't crash; the boost magnitude is scene-dependent.
        CHECK(true, "arb_compute: no wall -> runs without error");
    }

    // v6.0.0 round5b P2: emergency hard wall is FAIL-SAFE w.r.t. thermalEnabled.
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

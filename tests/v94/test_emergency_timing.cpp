// test_emergency_timing.cpp - rel15 thermal oscillation fix.
//
// Root cause under test:
//   The EMERGENCY recovery debounce (30s + temperature drop) used a private
//   g_emergencyStartTime that was reset ONLY on the thermal self-detect entry
//   path. When the watchdog forced EMERGENCY via set_control_mode=emergency ->
//   setRuntimeControlMode -> transitionToState(EMERGENCY), the private clock was
//   left stale, so `(now - g_emergencyStartTime) >= 30s` was already true and
//   thermal.cpp bailed out of EMERGENCY ~6s later. The watchdog re-forced it:
//   a 40-50s NORMAL->EMERGENCY->DEGRADED->NORMAL oscillation.
//
// This test asserts the two invariants the fix upholds:
//   (A) thermalEmergencyShouldExit() PURE predicate: recovery requires BOTH
//       temperature below the exit wall AND >= 30s continuous cooling. At 6s it
//       must NOT exit (the old 6s jitter), even though the temperature has dropped.
//   (B) The EMERGENCY entry-edge observation resets the start timestamp on the
//       FIRST scan that sees EMERGENCY -- regardless of which entry path flipped
//       the state (thermal self-detect OR external watchdog trigger).
//
// Links the REAL thermal.o (same recipe as emergency_recovery) so the pure
// predicate and the edge seam are the production code, not a reimplementation.
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include "aatped.h"
#include "aatped_state.h"

// Production symbols (thermal.cpp). Not declared in aatped_state.h, so the test
// forward-declares them: thermalEmergencyShouldExit is production logic; the
// *ForTest seams are gated behind AATPE_HOST_TEST (which TFLAGS defines).
extern bool thermalEmergencyShouldExit(SystemState cur, int emergencyTempC,
                                       int emergencyWallC, long secSinceEmergencyEntry);
#ifdef AATPE_HOST_TEST
extern time_t thermalEmergencyStartTimeForTest();
extern void thermalSetEmergencyStartTimeForTest(time_t t);
extern bool thermalObserveEmergencyEdgeForTest(SystemState cur);
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    mkdir(STATE_DIR, 0755);

    // Wall = 120C (the daemon default). Exit wall = 120 - kEmergencyHysteresis(10)
    // = 110C. Recovery gate = kEmergencyRecoverySec(30s).
    const int wall = 120;

    printf("=== (A) thermalEmergencyShouldExit: 30s + temp-drop gate ===\n");
    // Not in EMERGENCY at all -> never exits.
    CHECK(thermalEmergencyShouldExit(SystemState::NORMAL, 100, wall, 999) == false,
          "NORMAL -> never exit");
    // In EMERGENCY but still HOT (115 >= 110 exit wall) -> hold, even at 60s.
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 115, wall, 60) == false,
          "EMERGENCY 115C (not cooled) + 60s -> still hold");
    // THE BUG: cooled below exit wall but only 6s elapsed (watchdog-forced entry
    // with a stale clock made the old code believe 30s had passed). Must HOLD.
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 100, wall, 6) == false,
          "EMERGENCY 100C cooled but 6s only -> HOLD (no 6s jitter)");
    // Cooled AND >=30s -> may recover.
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 100, wall, 30) == true,
          "EMERGENCY 100C + exactly 30s -> recover");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 100, wall, 31) == true,
          "EMERGENCY 100C + 31s -> recover");
    // Just inside the hysteresis band (109 < 110) AND 30s -> recover.
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 109, wall, 30) == true,
          "EMERGENCY 109C (inside band) + 30s -> recover");
    // Exactly at the exit wall (110, not strictly below) -> hold even at 60s.
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 110, wall, 60) == false,
          "EMERGENCY 110C (== exit wall, not below) -> hold");

#ifdef AATPE_HOST_TEST
    printf("=== (B) entry-edge observation resets the clock on ANY entry path ===\n");
    // Start from a clean slate.
    thermal_resetRuntimeStateForTest();

    // Simulate the WATCHDOG path: a PREVIOUS emergency left a STALE start time.
    thermalSetEmergencyStartTimeForTest(100000);

    // We were in NORMAL; watchdog externally flips us to EMERGENCY (no thermal
    // self-detect happened). thermal.cpp's first scan after that observes the
    // rising edge and MUST reset the clock -- otherwise recovery fires at 6s.
    bool edge1 = thermalObserveEmergencyEdgeForTest(SystemState::EMERGENCY);
    CHECK(edge1 == true, "first scan seeing EMERGENCY after NORMAL = rising edge");
    time_t s1 = thermalEmergencyStartTimeForTest();
    CHECK(s1 > 100000, "start timestamp refreshed from stale 100000 to ~now");
    CHECK(s1 >= (time_t)1700000000, "start timestamp is a real 2026+ epoch (not stale)");

    // Stay in EMERGENCY another scan: no new edge, clock must NOT move (it keeps
    // measuring THIS continuous episode).
    time_t after1 = thermalEmergencyStartTimeForTest();
    bool edge2 = thermalObserveEmergencyEdgeForTest(SystemState::EMERGENCY);
    CHECK(edge2 == false, "second scan still in EMERGENCY = no edge");
    CHECK(thermalEmergencyStartTimeForTest() == after1, "clock unchanged while held in EMERGENCY");

    // Cool down to DEGRADED: the edge tracker clears.
    bool edge3 = thermalObserveEmergencyEdgeForTest(SystemState::DEGRADED);
    CHECK(edge3 == false, "scan seeing DEGRADED = no edge");

    // Re-enter EMERGENCY (next oscillation / real new overheat): fresh edge again.
    thermalSetEmergencyStartTimeForTest(100000);  // simulate stale again
    bool edge4 = thermalObserveEmergencyEdgeForTest(SystemState::EMERGENCY);
    CHECK(edge4 == true, "re-entry after DEGRADED = rising edge again");
    CHECK(thermalEmergencyStartTimeForTest() > 100000,
          "re-entry resets stale clock again");

    thermal_resetRuntimeStateForTest();
#endif

    printf("----\nresult: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

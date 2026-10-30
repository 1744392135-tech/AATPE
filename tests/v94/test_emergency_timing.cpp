#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include "aatped.h"
#include "aatped_state.h"

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

    const int wall = 120;

    printf("=== (A) thermalEmergencyShouldExit: 30s + temp-drop gate ===\n");
    CHECK(thermalEmergencyShouldExit(SystemState::NORMAL, 100, wall, 999) == false,
          "NORMAL -> never exit");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 115, wall, 60) == false,
          "EMERGENCY 115C (not cooled) + 60s -> still hold");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 100, wall, 6) == false,
          "EMERGENCY 100C cooled but 6s only -> HOLD (no 6s jitter)");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 100, wall, 30) == true,
          "EMERGENCY 100C + exactly 30s -> recover");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 100, wall, 31) == true,
          "EMERGENCY 100C + 31s -> recover");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 109, wall, 30) == true,
          "EMERGENCY 109C (inside band) + 30s -> recover");
    CHECK(thermalEmergencyShouldExit(SystemState::EMERGENCY, 110, wall, 60) == false,
          "EMERGENCY 110C (== exit wall, not below) -> hold");

#ifdef AATPE_HOST_TEST
    printf("=== (B) entry-edge observation resets the clock on ANY entry path ===\n");
    thermal_resetRuntimeStateForTest();

    thermalSetEmergencyStartTimeForTest(100000);

    bool edge1 = thermalObserveEmergencyEdgeForTest(SystemState::EMERGENCY);
    CHECK(edge1 == true, "first scan seeing EMERGENCY after NORMAL = rising edge");
    time_t s1 = thermalEmergencyStartTimeForTest();
    CHECK(s1 > 100000, "start timestamp refreshed from stale 100000 to ~now");
    CHECK(s1 >= (time_t)1700000000, "start timestamp is a real 2026+ epoch (not stale)");

    time_t after1 = thermalEmergencyStartTimeForTest();
    bool edge2 = thermalObserveEmergencyEdgeForTest(SystemState::EMERGENCY);
    CHECK(edge2 == false, "second scan still in EMERGENCY = no edge");
    CHECK(thermalEmergencyStartTimeForTest() == after1, "clock unchanged while held in EMERGENCY");

    bool edge3 = thermalObserveEmergencyEdgeForTest(SystemState::DEGRADED);
    CHECK(edge3 == false, "scan seeing DEGRADED = no edge");

    thermalSetEmergencyStartTimeForTest(100000);
    bool edge4 = thermalObserveEmergencyEdgeForTest(SystemState::EMERGENCY);
    CHECK(edge4 == true, "re-entry after DEGRADED = rising edge again");
    CHECK(thermalEmergencyStartTimeForTest() > 100000,
          "re-entry resets stale clock again");

    thermal_resetRuntimeStateForTest();
#endif

    printf("----\nresult: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

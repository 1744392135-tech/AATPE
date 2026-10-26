// test_watchdog_hotfix.cpp - Hotfix v4.1.1 watchdog host tests.
//
// Covers the four acceptance scenarios for the boot-grace / degraded-self-heal /
// adoption fixes. Following the existing tests/v94/test_watchdog.cpp pattern we
// #include the production .cpp with main() renamed, so the test drives the REAL
// file-static helpers (wdSteadyTransition, wdControlModeFor) and the REAL pure
// boundary decisions in watchdog:: from aatpe-constants.h — no re-implementation.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -DAATPE_HOST_TEST -I$SRC
//           test_watchdog_hotfix.cpp -o test_watchdog_hotfix -pthread
#include <cstdio>
#include <cstdlib>
#include <string>
#define main watchdog_main_unused
#include "aatpe-watchdog.cpp"
#undef main

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        if (c) { g_pass++; printf("  [PASS] %s\n", m); }                       \
        else { g_fail++; printf("  [FAIL] %s\n", m); }                          \
    } while (0)

int main() {
    const int grace = watchdog::kBootGraceSec;             // 120
    const int mult  = watchdog::kBootGraceHeartbeatMultiplier; // 3
    const int recStable = watchdog::kRecoveryStableSec;   // 30
    const int maxRapid  = watchdog::kMaxRapidCrashesBeforeDegrade; // 2
    const int baseHb    = watchdog::kHeartbeatTimeoutSec;  // 30

    // =====================================================================
    printf("=== (a) BOOT GRACE: two sub-10s deaths inside grace do NOT escalate ===\n");
    // bootNow inside grace
    CHECK(watchdog::inBootGrace(30, grace) == true,  "bootNow=30 < 120 -> inside grace");
    CHECK(watchdog::inBootGrace(119, grace) == true,"bootNow=119 < 120 -> inside grace");
    // heartbeat timeout widened 3x during grace
    CHECK(watchdog::effectiveHeartbeatTimeout(baseHb, true)  == baseHb * mult,
          "grace: heartbeat timeout 30s -> 90s (3x)");
    CHECK(watchdog::effectiveHeartbeatTimeout(baseHb, false) == baseHb,
          "post-grace: heartbeat timeout stays 30s");

    // Simulate the device loop's rapid-count decision over two rapid deaths that
    // both happen at bootNow=30 (inside grace, e.g. the service.sh/watchdog
    // double-start flock race).
    {
        int rapidCount = 0;
        bool degraded = false;
        for (int death = 0; death < 2; ++death) {           // two rapid deaths
            bool ing = watchdog::inBootGrace(30, grace);     // bootNow=30
            if (watchdog::countRapidCrash(ing)) rapidCount++;
            if (watchdog::shouldEscalateDegraded(ing, rapidCount, maxRapid, degraded)) {
                degraded = true;
            }
        }
        CHECK(rapidCount == 0, "grace: two rapid deaths NOT counted (rapidCount stays 0)");
        CHECK(degraded == false, "grace: two rapid deaths do NOT rise to DEGRADED");
        printf("  [INFO] rapidCount=%d degraded=%d (expected 0/0)\n", rapidCount, degraded);
    }

    // =====================================================================
    printf("=== (b) POST-GRACE regression: same behaviour still escalates ===\n");
    CHECK(watchdog::inBootGrace(150, grace) == false, "bootNow=150 >= 120 -> past grace");
    {
        int rapidCount = 0;
        bool degraded = false;
        for (int death = 0; death < 2; ++death) {           // two rapid deaths
            bool ing = watchdog::inBootGrace(150, grace);    // bootNow=150
            if (watchdog::countRapidCrash(ing)) rapidCount++;
            if (watchdog::shouldEscalateDegraded(ing, rapidCount, maxRapid, degraded)) {
                degraded = true;
            }
        }
        CHECK(rapidCount == 2, "post-grace: two rapid deaths ARE counted (rapidCount=2)");
        CHECK(degraded == true, "post-grace: reaching threshold STILL rises to DEGRADED (regression intact)");
        printf("  [INFO] rapidCount=%d degraded=%d (expected 2/1)\n", rapidCount, degraded);
    }
    // also: first rapid death alone must NOT escalate (need 2)
    CHECK(watchdog::shouldEscalateDegraded(false, 1, maxRapid, false) == false,
          "post-grace: single rapid death (<2) does not escalate");

    // =====================================================================
    printf("=== (c) DEGRADED self-heal: live+fresh for recStable -> RECOVERY -> NORMAL ===\n");
    // streak shorter than the stable window -> keep waiting
    CHECK(watchdog::degradedSelfHeal(true, 20, recStable) == false,
          "DEGRADED: fresh 20s < 30s -> keep latched (no kill, no release yet)");
    // daemon not actually healthy this sample -> never self-heal
    CHECK(watchdog::degradedSelfHeal(false, 60, recStable) == false,
          "DEGRADED: daemon dead/stale this sample -> no self-heal");
    // fresh continuously for the full window -> may clear the latch
    CHECK(watchdog::degradedSelfHeal(true, 30, recStable) == true,
          "DEGRADED: fresh 30s >= 30s -> clear latch WITHOUT killing");
    CHECK(watchdog::degradedSelfHeal(true, 90, recStable) == true,
          "DEGRADED: fresh 90s -> still self-heal");

    // After self-heal the loop calls wdTransition(RECOVERY). RECOVERY maps to
    // control_mode=assist, then the EXISTING RECOVERY->NORMAL hysteresis promotes us
    // once health stays clean. Confirm the control-mode latch release rides the
    // existing path (no second notification path invented).
    CHECK(std::string(wdControlModeFor(WdRuntimeState::DEGRADED)) == "assist",
          "DEGRADED latches control_mode=assist (the stuck state)");
    CHECK(std::string(wdControlModeFor(WdRuntimeState::RECOVERY)) == "assist",
          "RECOVERY still assist during the stable window");
    CHECK(std::string(wdControlModeFor(WdRuntimeState::NORMAL)) == "active",
          "NORMAL releases latch -> control_mode=active");

    // Drive the EXISTING table-driven transition: RECOVERY clean window elapsed -> NORMAL.
    {
        WdEvidence ev;
        ev.daemonAlive = true; ev.thermalEmergency = false;
        ev.migOver = false; ev.loadOver = false; ev.explicitStop = false;
        // just entered RECOVERY (recoverySince==now) -> stays RECOVERY
        CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1000, 0, 1000, recStable, 30)
              == WdRuntimeState::RECOVERY,
              "RECOVERY: window just started -> stays RECOVERY");
        // clean for 40s >= 30s -> NORMAL (this is the wdTransition(NORMAL) notification
        // that sends set_control_mode=active, releasing the assist latch)
        CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1040, 0, 1000, recStable, 30)
              == WdRuntimeState::NORMAL,
              "RECOVERY: clean 40s -> NORMAL (assist latch released via existing path)");
    }

    // =====================================================================
    printf("=== (d) ADOPTION: pidfile live+fresh aatped we did not spawn -> adopt, not spawn ===\n");
    // live + fresh + we did NOT start it (daemonStartedAt==0) -> adopt
    CHECK(watchdog::shouldAdoptExistingDaemon(true, true, false) == true,
          "live+fresh+not-ours -> adopt (refresh daemonStartedAt/dpid, do NOT spawn)");
    // live + fresh but WE started it (daemonStartedAt>0) -> no re-adopt
    CHECK(watchdog::shouldAdoptExistingDaemon(true, true, true) == false,
          "live+fresh but already ours -> keep current (no double adopt)");
    // not heartbeat-fresh yet (hb==0 early boot) -> do NOT adopt yet, wait
    CHECK(watchdog::shouldAdoptExistingDaemon(true, false, false) == false,
          "live but heartbeat not fresh yet -> defer adoption (wait for first hb)");
    // dead pidfile -> no adoption (normal restart path runs instead)
    CHECK(watchdog::shouldAdoptExistingDaemon(false, true, false) == false,
          "pidfile points at dead process -> not adopted (restart path)");

    printf("\n========================================\n");
    printf("Hotfix watchdog results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

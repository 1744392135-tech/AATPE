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
    const int grace = watchdog::kBootGraceSec;
    const int mult  = watchdog::kBootGraceHeartbeatMultiplier;
    const int recStable = watchdog::kRecoveryStableSec;
    const int maxRapid  = watchdog::kMaxRapidCrashesBeforeDegrade;
    const int baseHb    = watchdog::kHeartbeatTimeoutSec;

    printf("=== (a) BOOT GRACE: two sub-10s deaths inside grace do NOT escalate ===\n");
    CHECK(watchdog::inBootGrace(30, grace) == true,  "bootNow=30 < 120 -> inside grace");
    CHECK(watchdog::inBootGrace(119, grace) == true,"bootNow=119 < 120 -> inside grace");
    CHECK(watchdog::effectiveHeartbeatTimeout(baseHb, true)  == baseHb * mult,
          "grace: heartbeat timeout 30s -> 90s (3x)");
    CHECK(watchdog::effectiveHeartbeatTimeout(baseHb, false) == baseHb,
          "post-grace: heartbeat timeout stays 30s");

    {
        int rapidCount = 0;
        bool degraded = false;
        for (int death = 0; death < 2; ++death) {
            bool ing = watchdog::inBootGrace(30, grace);
            if (watchdog::countRapidCrash(ing)) rapidCount++;
            if (watchdog::shouldEscalateDegraded(ing, rapidCount, maxRapid, degraded)) {
                degraded = true;
            }
        }
        CHECK(rapidCount == 0, "grace: two rapid deaths NOT counted (rapidCount stays 0)");
        CHECK(degraded == false, "grace: two rapid deaths do NOT rise to DEGRADED");
        printf("  [INFO] rapidCount=%d degraded=%d (expected 0/0)\n", rapidCount, degraded);
    }

    printf("=== (b) POST-GRACE regression: same behaviour still escalates ===\n");
    CHECK(watchdog::inBootGrace(150, grace) == false, "bootNow=150 >= 120 -> past grace");
    {
        int rapidCount = 0;
        bool degraded = false;
        for (int death = 0; death < 2; ++death) {
            bool ing = watchdog::inBootGrace(150, grace);
            if (watchdog::countRapidCrash(ing)) rapidCount++;
            if (watchdog::shouldEscalateDegraded(ing, rapidCount, maxRapid, degraded)) {
                degraded = true;
            }
        }
        CHECK(rapidCount == 2, "post-grace: two rapid deaths ARE counted (rapidCount=2)");
        CHECK(degraded == true, "post-grace: reaching threshold STILL rises to DEGRADED (regression intact)");
        printf("  [INFO] rapidCount=%d degraded=%d (expected 2/1)\n", rapidCount, degraded);
    }
    CHECK(watchdog::shouldEscalateDegraded(false, 1, maxRapid, false) == false,
          "post-grace: single rapid death (<2) does not escalate");

    printf("=== (c) DEGRADED self-heal: live+fresh for recStable -> RECOVERY -> NORMAL ===\n");
    CHECK(watchdog::degradedSelfHeal(true, 20, recStable) == false,
          "DEGRADED: fresh 20s < 30s -> keep latched (no kill, no release yet)");
    CHECK(watchdog::degradedSelfHeal(false, 60, recStable) == false,
          "DEGRADED: daemon dead/stale this sample -> no self-heal");
    CHECK(watchdog::degradedSelfHeal(true, 30, recStable) == true,
          "DEGRADED: fresh 30s >= 30s -> clear latch WITHOUT killing");
    CHECK(watchdog::degradedSelfHeal(true, 90, recStable) == true,
          "DEGRADED: fresh 90s -> still self-heal");

    CHECK(std::string(wdControlModeFor(WdRuntimeState::DEGRADED)) == "assist",
          "DEGRADED latches control_mode=assist (the stuck state)");
    CHECK(std::string(wdControlModeFor(WdRuntimeState::RECOVERY)) == "assist",
          "RECOVERY still assist during the stable window");
    CHECK(std::string(wdControlModeFor(WdRuntimeState::NORMAL)) == "active",
          "NORMAL releases latch -> control_mode=active");

    {
        WdEvidence ev;
        ev.daemonAlive = true; ev.thermalEmergency = false;
        ev.migOver = false; ev.loadOver = false; ev.explicitStop = false;
        CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1000, 0, 1000, recStable, 30)
              == WdRuntimeState::RECOVERY,
              "RECOVERY: window just started -> stays RECOVERY");
        CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1040, 0, 1000, recStable, 30)
              == WdRuntimeState::NORMAL,
              "RECOVERY: clean 40s -> NORMAL (assist latch released via existing path)");
    }

    printf("=== (d) ADOPTION: pidfile live+fresh aatped we did not spawn -> adopt, not spawn ===\n");
    CHECK(watchdog::shouldAdoptExistingDaemon(true, true, false) == true,
          "live+fresh+not-ours -> adopt (refresh daemonStartedAt/dpid, do NOT spawn)");
    CHECK(watchdog::shouldAdoptExistingDaemon(true, true, true) == false,
          "live+fresh but already ours -> keep current (no double adopt)");
    CHECK(watchdog::shouldAdoptExistingDaemon(true, false, false) == false,
          "live but heartbeat not fresh yet -> defer adoption (wait for first hb)");
    CHECK(watchdog::shouldAdoptExistingDaemon(false, true, false) == false,
          "pidfile points at dead process -> not adopted (restart path)");

    printf("\n========================================\n");
    printf("Hotfix watchdog results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

#include <cstdio>
#include <string>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static std::string curMode() {
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    return g_cfg.controlMode;
}

int main() {
    printf("=== ActiveLock: user picks 激进(active) ===\n");
    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "assist"; }
    ControlResult r0 = executeControlLine("set_mode=active");
    CHECK(r0.success == true, "set_mode=active accepted");
    CHECK(curMode() == "active", "g_cfg.controlMode==active after user picks 激进");
    CHECK(g_userModeLock.load() == true, "g_userModeLock==true (manual lock set)");
    CHECK(getSystemState() == SystemState::NORMAL, "system_state==NORMAL");

    printf("=== ActiveLock: watchdog 3s tick wants passive (migration_rate) ===\n");
    ControlResult r1 = executeControlLine("set_control_mode=passive&reason=migration_rate");
    CHECK(curMode() == "active",
          "watchdog passive downgrade does NOT drag active back to passive (the bounce)");
    CHECK(g_userModeLock.load() == true, "g_userModeLock still true after ignored downgrade");
    CHECK(g_watchdogPassiveLocked.load() == false,
          "watchdog passive lock NOT latched while user holds the lock");

    printf("=== ActiveLock: thermal emergency MUST still override the lock ===\n");
    ControlResult r2 = executeControlLine("set_control_mode=emergency&reason=thermal_overheat");
    CHECK(r2.success == true, "set_control_mode=emergency accepted");
    CHECK(curMode() == "emergency",
          "thermal emergency overrides user-locked active (safety path intact)");
    CHECK(getSystemState() == SystemState::EMERGENCY, "system_state==EMERGENCY");

    printf("=== ActiveLock: user picks 被动(passive) -> lock released, safety works ===\n");
    ControlResult r3 = executeControlLine("set_mode=passive");
    CHECK(r3.success == true, "set_mode=passive accepted");
    CHECK(curMode() == "passive", "g_cfg.controlMode==passive after user picks 被动");
    CHECK(g_userModeLock.load() == false, "g_userModeLock==false after user picks passive");

    ControlResult r4 = executeControlLine("set_control_mode=passive&reason=migration_rate");
    CHECK(r4.success == true, "watchdog passive applies once user lock is released");
    CHECK(curMode() == "passive", "g_cfg.controlMode stays passive (safety path works)");

    printf("\n========================================\n"
           "ActiveLock results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

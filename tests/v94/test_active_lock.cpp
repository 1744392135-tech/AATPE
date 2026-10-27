// test_active_lock.cpp - v94 hermetic regression test for the WebUI
// "激进(active) -> 被动(passive) bounce within ~3s" bug.
//
// Root cause proven here: the watchdog issues `set_control_mode=passive&reason=...`
// on its 3s tick (watchdog::kDefaultIntervalSec=3) for mig-rate / load / memory /
// crash-loop latching. That command routes through executeControlLine() ->
// setRuntimeControlMode("passive", ...). Before the fix setRuntimeControlMode()
// did NOT consult g_userModeLock, so it unconditionally overwrote the user's
// explicitly-chosen "active": g_cfg.controlMode -> "passive", g_watchdogPassiveLocked
// latched, system_state -> PASSIVE, STATE_DIR/control_mode rewritten. The WebUI
// button reads that file and bounces 激进 -> 被动 within one 3s watchdog interval.
//
// The fix gates a NON-emergency passive downgrade on g_userModeLock: when the user
// manually picked active/assist, the safety path may not drag it back to passive.
// Real thermal emergency MUST still override (it is the one safety event that wins),
// and a user who picks passive clears the lock, so the safety path keeps working.
//
// Links aatped_control.cpp (the real executeControlLine + setRuntimeControlMode)
// with v94_daemon_deps + control_deps stubs, same link set as the `control` recipe.
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
    // Simulate the WebUI control page: user clicks 激进.
    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "assist"; }
    ControlResult r0 = executeControlLine("set_mode=active");
    CHECK(r0.success == true, "set_mode=active accepted");
    CHECK(curMode() == "active", "g_cfg.controlMode==active after user picks 激进");
    CHECK(g_userModeLock.load() == true, "g_userModeLock==true (manual lock set)");
    CHECK(getSystemState() == SystemState::NORMAL, "system_state==NORMAL");

    printf("=== ActiveLock: watchdog 3s tick wants passive (migration_rate) ===\n");
    // This is the bounce path: the watchdog writes set_control_mode=passive on its
    // 3s cadence. On a healthy 40C idle device this used to clobber the user's
    // active within ~3s. After the fix it must be IGNORED while the lock is held.
    ControlResult r1 = executeControlLine("set_control_mode=passive&reason=migration_rate");
    CHECK(curMode() == "active",
          "watchdog passive downgrade does NOT drag active back to passive (the bounce)");
    CHECK(g_userModeLock.load() == true, "g_userModeLock still true after ignored downgrade");
    CHECK(g_watchdogPassiveLocked.load() == false,
          "watchdog passive lock NOT latched while user holds the lock");

    printf("=== ActiveLock: thermal emergency MUST still override the lock ===\n");
    // Safety: a real thermal emergency is the one event that always wins, even
    // when the user manually locked active. It must NOT be blocked.
    ControlResult r2 = executeControlLine("set_control_mode=emergency&reason=thermal_overheat");
    CHECK(r2.success == true, "set_control_mode=emergency accepted");
    CHECK(curMode() == "emergency",
          "thermal emergency overrides user-locked active (safety path intact)");
    CHECK(getSystemState() == SystemState::EMERGENCY, "system_state==EMERGENCY");

    printf("=== ActiveLock: user picks 被动(passive) -> lock released, safety works ===\n");
    // A user who explicitly chooses passive releases the manual lock, so the
    // watchdog's passive downgrade must apply normally (no over-protection).
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

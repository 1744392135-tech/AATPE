// test_emergency_recovery.cpp - P3-1: thermal emergency recovery must honor a
// user's explicit mode change made DURING the EMERGENCY latch.
//
// Semantics under test (lesson #20: user explicit switch overrides the latch):
//   * No in-latch override  -> recovery restores g_preEmergencyCfg.controlMode.
//   * User picked a mode while latched (g_emergencyUserOverride=true,
//     g_cfg.controlMode already = that choice) -> recovery KEEPS the user's
//     choice instead of silently restoring the stale pre-emergency snapshot.
//
// This drives the REAL extracted production function thermalApplyEmergencyModeRecovery()
// (not a reimplementation). thermal_deps provides a stateful stub of the state
// machine + real tmp+rename writeFileAtomic so we can read back control_mode.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include "aatped.h"
#include "aatped_state.h"

// g_emergencyUserOverride is defined by thermal_deps (this recipe's stub).
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include "aatped.h"
#include "aatped_state.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Read back STATE_DIR/control_mode written by the recovery function.
static std::string readControlModeFile() {
    std::string p = std::string(STATE_DIR) + "/control_mode";
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return "<missing>";
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return "<empty>"; }
    fclose(f);
    // trim trailing whitespace/newline
    std::string s(buf);
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
    return s;
}

int main() {
    mkdir(STATE_DIR, 0755);

    printf("=== P3-1: thermal emergency recovery honors in-latch user override ===\n");

    // ---- Baseline: user was on "assist" before emergency.
    g_cfg.controlMode = "assist";
    g_preEmergencyCfg.controlMode = "assist";   // what transitionToState(EMERGENCY) snapshots
    g_preEmergencyAutoProfile = false;
    // Recovery runs AFTER transitionToState(DEGRADED) -> state is DEGRADED.
    g_systemState.store(static_cast<int>(SystemState::DEGRADED));

    // ---- Case A: NO in-latch override -> restore the pre-emergency snapshot.
    g_emergencyUserOverride.store(false);
    // Simulate the latch having forced controlMode to "passive" while latched.
    g_cfg.controlMode = "passive";
    std::string rA = thermalApplyEmergencyModeRecovery();
    CHECK(rA == "assist", "A: no-override recovery restores snapshot 'assist'");
    CHECK(g_cfg.controlMode == "assist", "A: g_cfg.controlMode restored to 'assist'");
    CHECK(g_emergencyUserOverride.load() == false, "A: override flag stays false");
    CHECK(readControlModeFile() == "assist", "A: control_mode file written 'assist'");

    // ---- Case B: user explicitly switched to "active" WHILE latched.
    // This mirrors the set_mode= production path: setUserControlMode +
    // g_cfg.controlMode = mappedMode + g_emergencyUserOverride=true, all while
    // getSystemState()==EMERGENCY.
    g_preEmergencyCfg.controlMode = "assist";   // stale snapshot from emergency entry
    g_emergencyUserOverride.store(false);
    g_cfg.controlMode = "assist";
    g_systemState.store(static_cast<int>(SystemState::EMERGENCY));  // still latched
    // user picks active mid-latch (exactly what set_mode= does):
    g_cfg.controlMode = "active";
    g_emergencyUserOverride.store(true);
    // temperature recovers -> production calls transitionToState(DEGRADED) then
    // thermalApplyEmergencyModeRecovery().
    g_systemState.store(static_cast<int>(SystemState::DEGRADED));
    std::string rB = thermalApplyEmergencyModeRecovery();
    CHECK(rB == "active", "B: in-latch override keeps user's 'active'");
    CHECK(g_cfg.controlMode == "active", "B: g_cfg.controlMode NOT clobbered to 'assist'");
    CHECK(g_emergencyUserOverride.load() == false, "B: override flag consumed after recovery");
    CHECK(readControlModeFile() == "active", "B: control_mode file written 'active' (not 'assist')");

    printf("----\nresult: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

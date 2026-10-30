#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include "aatped.h"
#include "aatped_state.h"

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

static std::string readControlModeFile() {
    std::string p = std::string(STATE_DIR) + "/control_mode";
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return "<missing>";
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return "<empty>"; }
    fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
    return s;
}

int main() {
    mkdir(STATE_DIR, 0755);

    printf("=== P3-1: thermal emergency recovery honors in-latch user override ===\n");

    g_cfg.controlMode = "assist";
    g_preEmergencyCfg.controlMode = "assist";
    g_preEmergencyAutoProfile = false;
    g_systemState.store(static_cast<int>(SystemState::DEGRADED));

    g_emergencyUserOverride.store(false);
    g_cfg.controlMode = "passive";
    std::string rA = thermalApplyEmergencyModeRecovery();
    CHECK(rA == "assist", "A: no-override recovery restores snapshot 'assist'");
    CHECK(g_cfg.controlMode == "assist", "A: g_cfg.controlMode restored to 'assist'");
    CHECK(g_emergencyUserOverride.load() == false, "A: override flag stays false");
    CHECK(readControlModeFile() == "assist", "A: control_mode file written 'assist'");

    g_preEmergencyCfg.controlMode = "assist";
    g_emergencyUserOverride.store(false);
    g_cfg.controlMode = "assist";
    g_systemState.store(static_cast<int>(SystemState::EMERGENCY));
    g_cfg.controlMode = "active";
    g_emergencyUserOverride.store(true);
    g_systemState.store(static_cast<int>(SystemState::DEGRADED));
    std::string rB = thermalApplyEmergencyModeRecovery();
    CHECK(rB == "active", "B: in-latch override keeps user's 'active'");
    CHECK(g_cfg.controlMode == "active", "B: g_cfg.controlMode NOT clobbered to 'assist'");
    CHECK(g_emergencyUserOverride.load() == false, "B: override flag consumed after recovery");
    CHECK(readControlModeFile() == "active", "B: control_mode file written 'active' (not 'assist')");

    printf("----\nresult: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

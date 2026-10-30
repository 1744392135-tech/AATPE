#include <cstdio>
#include <cstring>
#include "aatped.h"
#include "aatped_state.h"

extern "C" void aatpe_state_jsonEscapeForTest(const char* in, char* out, int outCap);

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    aatpe_state_setDwellMsForTest(0);

    printf("=== State: systemStateToString ===\n");
    CHECK(std::string(systemStateToString(SystemState::NORMAL)) == "NORMAL", "NORMAL string");
    CHECK(std::string(systemStateToString(SystemState::DEGRADED)) == "DEGRADED", "DEGRADED string");
    CHECK(std::string(systemStateToString(SystemState::PASSIVE)) == "PASSIVE", "PASSIVE string");
    CHECK(std::string(systemStateToString(SystemState::EMERGENCY)) == "EMERGENCY", "EMERGENCY string");

    printf("=== State: systemStatePriority monotonic ===\n");
    CHECK(systemStatePriority(SystemState::EMERGENCY) > systemStatePriority(SystemState::PASSIVE),
          "EMERGENCY > PASSIVE");
    CHECK(systemStatePriority(SystemState::PASSIVE) > systemStatePriority(SystemState::DEGRADED),
          "PASSIVE > DEGRADED");
    CHECK(systemStatePriority(SystemState::DEGRADED) > systemStatePriority(SystemState::NORMAL),
          "DEGRADED > NORMAL");

    printf("=== State: transitionToState escalation ===\n");
    g_systemState.store((int)SystemState::NORMAL);
    CHECK(transitionToState(SystemState::DEGRADED, "high load") == true, "NORMAL->DEGRADED ok");
    CHECK(getSystemState() == SystemState::DEGRADED, "now DEGRADED");
    CHECK(transitionToState(SystemState::EMERGENCY, "thermal") == true, "DEGRADED->EMERGENCY ok");
    CHECK(getSystemState() == SystemState::EMERGENCY, "now EMERGENCY");
    CHECK(g_thermalEmergency.load() == true, "EMERGENCY sets thermalEmergency");

    printf("=== State: priority enforcement blocks bad downgrade ===\n");
    CHECK(transitionToState(SystemState::NORMAL, "skip") == false, "EMERGENCY->NORMAL blocked");
    CHECK(getSystemState() == SystemState::EMERGENCY, "still EMERGENCY");

    printf("=== State: EMERGENCY->DEGRADED recovery allowed ===\n");
    CHECK(transitionToState(SystemState::DEGRADED, "recovered") == true, "EMERGENCY->DEGRADED ok");
    CHECK(transitionToState(SystemState::NORMAL, "recovered") == true, "DEGRADED->NORMAL ok");
    CHECK(getSystemState() == SystemState::NORMAL, "back to NORMAL");

    printf("=== State: no-op transition ===\n");
    CHECK(transitionToState(SystemState::NORMAL, "same") == false, "same state -> false");

    printf("=== State: history recorded ===\n");
    auto hist = getStateHistory();
    CHECK(hist.size() >= 3, "history has >=3 entries");
    CHECK(std::string(getLastTransitionReason()) == "recovered", "last reason recorded");

    printf("=== State: v3.4.2-fix dwell debounce ===\n");
    aatpe_state_setDwellMsForTest((long long)kStateDwellMs);
    g_systemState.store((int)SystemState::NORMAL);
    CHECK(transitionToState(SystemState::EMERGENCY, "thermal") == true,
          "dwell: escalation NORMAL->EMERGENCY bypasses dwell");
    CHECK(getSystemState() == SystemState::EMERGENCY, "dwell: now EMERGENCY");
    CHECK(transitionToState(SystemState::DEGRADED, "high_cpu_usage") == false,
          "dwell: rapid EMERGENCY->DEGRADED deferred (<5s)");
    CHECK(getSystemState() == SystemState::EMERGENCY, "dwell: still EMERGENCY after deferred downgrade");
    aatpe_state_setDwellMsForTest(0);
    CHECK(transitionToState(SystemState::DEGRADED, "thermal_recovery") == true,
          "dwell: EMERGENCY->DEGRADED allowed after dwell released");
    CHECK(getSystemState() == SystemState::DEGRADED, "dwell: now DEGRADED");

    printf("=== State: v4.2 Batch C writeStateJson graceful degradation ===\n");
    {
        extern const char* STATE_DIR;
        static char sTmp[] = "/tmp/aatpe_state_v42_XXXXXX";
        char* sd = mkdtemp(sTmp);
        if (sd) STATE_DIR = sd;
        writeStateJson();
        CHECK(true, "writeStateJson() with absent A/B/C state files does not crash");
    }

    printf("=== State: STATE-02 JSON escaping for embedded strings ===\n");
    {
        char buf[256];
        aatpe_state_jsonEscapeForTest("uclamp=\"yes\"\nstune=yes\\x", buf, sizeof(buf));
        std::string e(buf);
        CHECK(e.find("\\\"") != std::string::npos, "jsonEscape escapes double-quote");
        CHECK(e.find("\\n") != std::string::npos, "jsonEscape escapes newline");
        CHECK(e.find("\\\\") != std::string::npos, "jsonEscape escapes backslash");
        bool ok = true;
        for (size_t i = 0; i < e.size(); ++i) {
            if (e[i] == '"' && (i == 0 || e[i-1] != '\\')) { ok = false; break; }
        }
        CHECK(ok, "jsonEscape: every literal quote is backslash-escaped");
        aatpe_state_jsonEscapeForTest("1.23", buf, sizeof(buf));
        CHECK(std::string(buf) == "1.23", "jsonEscape leaves plain numeric loadavg_raw intact");
    }

    printf("\n========================================\n"
           "State results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

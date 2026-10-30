#include <cstdio>
#include "aatpe-constants.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    using namespace aatpe::watchdog;
    printf("=== Task3: high-load DEGRADED gating (anti-flap) ===\n");
    const long long dwellThresh = 10000;

    CHECK(highLoadShouldDegrade(false, 0,     dwellThresh) == false, "high util, no jank, 0ms  -> hold NORMAL");
    CHECK(highLoadShouldDegrade(false, 5000,  dwellThresh) == false, "high util, no jank, 5s   -> hold NORMAL (no flap)");
    CHECK(highLoadShouldDegrade(false, 10000, dwellThresh) == true,  "high util, no jank, 10s  -> degrade (long dwell)");
    CHECK(highLoadShouldDegrade(false, 30000, dwellThresh) == true,  "high util, no jank, 30s  -> degrade");
    CHECK(highLoadShouldDegrade(true,  0,     dwellThresh) == true,  "high util + jank, 0ms    -> degrade now");
    CHECK(highLoadShouldDegrade(true,  1000,  dwellThresh) == true,  "high util + jank, 1s     -> degrade now");

    printf("=== watchdog: rapid_crash_loop latch self-heal ===\n");
    const int stable = kRecoveryStableSec;
    CHECK(degradedSelfHeal(true, stable,    stable) == true,  "healthy 30s -> release latch");
    CHECK(degradedSelfHeal(true, stable+30, stable) == true, "healthy 60s -> release latch");
    CHECK(degradedSelfHeal(true, stable-1,  stable) == false, "healthy 29s -> still latched");
    CHECK(degradedSelfHeal(true, 0,         stable) == false, "healthy 0s  -> still latched");
    CHECK(degradedSelfHeal(false, 60,      stable) == false,  "unhealthy 60s -> stay latched (no release)");
    CHECK(degradedSelfHeal(false, 0,        stable) == false,  "unhealthy 0s  -> stay latched");

    printf("\n========================================\n"
           "degraded gate: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

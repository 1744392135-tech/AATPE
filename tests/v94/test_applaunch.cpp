#include <cstdio>
#include "aatped.h"
#include "aatped_applaunch.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== AppLaunch: recordAppLaunch stores a record ===\n");
    recordAppLaunch("com.game.alpha", 1001, 1002);
    CHECK(g_appLaunchHistory.size() == 1, "history has 1 entry after record");
    int cpu = getPreloadedPreferredCpu("com.game.alpha");
    CHECK(cpu >= 0, "known package returns a preferred cpu");
    CHECK(cpu < g_cpuCount, "preferred cpu within range");

    printf("=== AppLaunch: getPreloadedPreferredCpu edge cases ===\n");
    CHECK(getPreloadedPreferredCpu("com.unknown.app") == -1, "unknown package -> -1");
    CHECK(getPreloadedPreferredCpu(nullptr) == -1, "nullptr package -> -1");

    printf("=== AppLaunch: second record preserves historical preferred cpu ===\n");
    int first = getPreloadedPreferredCpu("com.game.alpha");
    recordAppLaunch("com.game.alpha", 2001, 2002);
    CHECK(getPreloadedPreferredCpu("com.game.alpha") == first,
          "re-launch keeps historically chosen preferred cpu");

    printf("=== AppLaunch: checkAndPreloadNewApps graceful on host ===\n");
    size_t histBefore = g_appLaunchHistory.size();
    checkAndPreloadNewApps();
    CHECK(g_appLaunchHistory.size() >= histBefore,
          "checkAndPreloadNewApps preserves existing launch history");

    printf("\n========================================\n"
           "AppLaunch results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

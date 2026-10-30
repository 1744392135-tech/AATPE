#include <cstdio>
#include "aatped.h"
#include "aatped_uevent.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== uevent: checkUeventFallback arms start time, stays inactive ===\n");
    g_ueventFallbackActive.store(false);
    g_ueventEventCount.store(0);
    checkUeventFallback();
    CHECK(g_ueventFallbackActive.load() == false, "1st call arms timer, not active");
    for (int i = 0; i < 5; i++) checkUeventFallback();
    CHECK(g_ueventFallbackActive.load() == false, "rapid re-checks within window stay inactive");

    printf("=== uevent: pollSysfsEvents no-op while fallback inactive ===\n");
    g_ueventEventCount.store(0);
    pollSysfsEvents();
    pollSysfsEvents();
    CHECK(g_ueventEventCount.load() == 0,
          "inactive pollSysfsEvents produces zero events");

    printf("=== uevent: event counter atomic ===\n");
    g_ueventEventCount.store(7);
    CHECK(g_ueventEventCount.load() == 7, "event counter read/write round-trip");
    g_ueventEventCount.store(0);

    printf("=== uevent: manual activation then poll no crash ===\n");
    g_ueventFallbackActive.store(true);
    pollSysfsEvents();
    pollSysfsEvents();
    CHECK(g_ueventEventCount.load() == 0,
          "active pollSysfsEvents on host produces zero fabricated events");
    g_ueventFallbackActive.store(false);

    printf("\n========================================\n"
           "Uevent-fallback results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

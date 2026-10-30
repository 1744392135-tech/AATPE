#include <cstdio>
#include <vector>
#include "aatpe_ml.h"
#include "aatped_topology.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

extern void setOnlineCpusCache(const std::vector<int>& cpus);

int main() {
    printf("==> H13 hotplug topology hardening (host) ==\n");

    setOnlineCpusCache({0, 1, 2, 3, 6, 7});
    CHECK(isCpuOnlineNow(0), "H13: cpu0 online after seed");
    CHECK(isCpuOnlineNow(3), "H13: cpu3 online");
    CHECK(isCpuOnlineNow(6), "H13: cpu6 online");
    CHECK(isCpuOnlineNow(7), "H13: cpu7 online");
    CHECK(!isCpuOnlineNow(4), "H13: cpu4 offline (not in cache)");
    CHECK(!isCpuOnlineNow(5), "H13: cpu5 offline");
    CHECK(!isCpuOnlineNow(99), "H13: cpu99 offline (out of range)");

    setOnlineCpusCache({0, 1, 2, 3, 6});
    CHECK(isCpuOnlineNow(6), "H13: cpu6 still online");
    CHECK(!isCpuOnlineNow(7), "H13: cpu7 now offline (shrink)");

    setOnlineCpusCache({0, 1, 2, 3, 6, 7});
    CHECK(isCpuOnlineNow(7), "H13: cpu7 back online (grow)");

    setTopologyDebounceMsForTest(0);
    setTopologyDebounceMsForTest(500);
    CHECK(true, "H13: setTopologyDebounceMsForTest(0/500) no crash");

    CHECK(!isTopologyRebuilding(), "H13: not rebuilding at rest");
    int n0 = topologyRebuildCount();
    CHECK(n0 >= 0, "H13: rebuild count non-negative");

    int before = topologyRebuildCount();
    for (int i = 0; i < 10; i++) {
        refreshCpuTopology();
    }
    int after = topologyRebuildCount();
    CHECK((after - before) <= 2,
          "H13: 10 rapid refreshCpuTopology() on stable mask triggers <=2 rebuilds (debounce works)");

    CHECK(!isTopologyRebuilding(), "H13: not rebuilding after burst");

    setOnlineCpusCache({0, 1, 2, 3, 6, 7});
    seedOnlineCacheFromSysfs();
    CHECK(!isCpuOnlineNow(99), "H13 P3-a: cpu99 offline after seed (no crash)");

    CHECK(!isTopologyRebuilding(), "H13 P3-b: rebuilding flag cleared after refresh returns");

    printf(failures ? "=== H13 FAIL (%d) ===\n" : "=== H13 PASS ===\n", failures);
    return failures ? 1 : 0;
}

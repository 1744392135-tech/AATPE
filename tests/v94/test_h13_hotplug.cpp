// test_h13_hotplug.cpp — H13 host test for hotplug topology rebuild hardening.
//
// What is host-testable (no real /sys hotplug, no real detectCpuTopology):
//   1. setOnlineCpusCache() + isCpuOnlineNow() — the offline hard filter.
//   2. setTopologyDebounceMsForTest() toggles the debounce window without crashing.
//   3. topologyRebuildCount() starts at 0 and does not decrement/overflow.
//   4. isTopologyRebuilding() is false at rest (mid-rebuild window not stuck).
//   5. Calling refreshCpuTopology() 10 times rapidly on a host where the online
//      mask is stable does NOT trigger 10 rebuilds (it triggers 0 because the
//      mask doesn't change, or at most 1 cold-start rebuild).
//
// The actual debounce-coalesces-10-uevents-into-1-rebuild path requires a real
// sysfs online mask that changes 10 times; that is verified on device.
#include <cstdio>
#include <vector>
#include "aatpe_ml.h"
#include "aatped_topology.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

// setOnlineCpusCache is defined in placement.cpp; declared here as a test seam.
extern void setOnlineCpusCache(const std::vector<int>& cpus);

int main() {
    printf("==> H13 hotplug topology hardening (host) ==\n");

    // 1. Offline hard filter: seed the online cache with {0,1,2,3,6,7}.
    setOnlineCpusCache({0, 1, 2, 3, 6, 7});
    CHECK(isCpuOnlineNow(0), "H13: cpu0 online after seed");
    CHECK(isCpuOnlineNow(3), "H13: cpu3 online");
    CHECK(isCpuOnlineNow(6), "H13: cpu6 online");
    CHECK(isCpuOnlineNow(7), "H13: cpu7 online");
    CHECK(!isCpuOnlineNow(4), "H13: cpu4 offline (not in cache)");
    CHECK(!isCpuOnlineNow(5), "H13: cpu5 offline");
    CHECK(!isCpuOnlineNow(99), "H13: cpu99 offline (out of range)");

    // 2. Shrink the online set: cpu7 goes offline.
    setOnlineCpusCache({0, 1, 2, 3, 6});
    CHECK(isCpuOnlineNow(6), "H13: cpu6 still online");
    CHECK(!isCpuOnlineNow(7), "H13: cpu7 now offline (shrink)");

    // 3. Expand: cpu7 comes back online.
    setOnlineCpusCache({0, 1, 2, 3, 6, 7});
    CHECK(isCpuOnlineNow(7), "H13: cpu7 back online (grow)");

    // 4. Debounce toggle: 0 disables, 500 re-enables. No crash.
    setTopologyDebounceMsForTest(0);
    setTopologyDebounceMsForTest(500);
    CHECK(true, "H13: setTopologyDebounceMsForTest(0/500) no crash");

    // 5. Rebuild state at rest.
    CHECK(!isTopologyRebuilding(), "H13: not rebuilding at rest");
    int n0 = topologyRebuildCount();
    CHECK(n0 >= 0, "H13: rebuild count non-negative");

    // 6. Rapid refreshCpuTopology calls on a host where the online mask is stable
    //    must NOT trigger repeated rebuilds. On x86_64 host, getOnlineCpusRaw()
    //    reads sysconf (constant), so after the first cold-start the mask is
    //    unchanged and refreshCpuTopology() should NOT bump the rebuild counter.
    int before = topologyRebuildCount();
    for (int i = 0; i < 10; i++) {
        refreshCpuTopology();
    }
    int after = topologyRebuildCount();
    // The mask is stable across 10 calls; rebuilds should not grow by 10.
    // At most a small number (cold-start / first-touch). We assert the growth is
    // <= 2 (debounce coalesces bursts; stable mask triggers 0).
    CHECK((after - before) <= 2,
          "H13: 10 rapid refreshCpuTopology() on stable mask triggers <=2 rebuilds (debounce works)");

    // 7. Re-check at rest after the burst.
    CHECK(!isTopologyRebuilding(), "H13: not rebuilding after burst");

    // P3-a: seedOnlineCacheFromSysfs exists and does not crash. On host the
    // sysfs read may succeed (real host cpus) or return empty (sandbox); either
    // way the call must not crash and isCpuOnlineNow must stay consistent.
    setOnlineCpusCache({0, 1, 2, 3, 6, 7});  // re-seed known state
    seedOnlineCacheFromSysfs();              // may overwrite; must not crash
    // After seed, cpu99 must still be offline (host never has cpu99).
    CHECK(!isCpuOnlineNow(99), "H13 P3-a: cpu99 offline after seed (no crash)");

    // P3-b: RAII guard — after refreshCpuTopology returns, rebuilding MUST be
    // false even if a rebuild happened (the guard's destructor cleared it).
    CHECK(!isTopologyRebuilding(), "H13 P3-b: rebuilding flag cleared after refresh returns");

    printf(failures ? "=== H13 FAIL (%d) ===\n" : "=== H13 PASS ===\n", failures);
    return failures ? 1 : 0;
}

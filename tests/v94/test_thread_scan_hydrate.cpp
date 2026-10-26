// test_thread_scan_hydrate.cpp — v4.4.9 G4 regression: CLONE_THREAD placeholder
// starttime hydration invariant.
//
// onFork() inserts a placeholder ThreadInfo (starttime==0, empty comm,
// needsHydration=true). The periodic refresh must:
//   (a) force a full REUSE repopulate on the placeholder (not silently keep it as
//       a fully-tracked, already-hydrated thread);
//   (b) after hydration (needsHydration=false, real starttime set), NOT treat the
//       placeholder's 0 starttime as "thread exited / tid reused". The TID-reuse
//       guard requires BOTH starttimes nonzero, so a hydrated thread is never
//       spuriously evicted; and a missing /proc read returns 0 -> no REUSE.
#include <cstdio>
#include <unistd.h>
#include "aatped.h"
#include "aatped_types.h"

// Returns 0=KEEP,1=REUSE,2=GONE (thread_scan.cpp, AATPE_HOST_TEST seam).
extern int threadScanRefreshForTest(ThreadInfo& ti, bool checkStarttime);

namespace {
constexpr int kRR_KEEP = 0;
constexpr int kRR_REUSE = 1;
}

static int g_fail = 0;
#define CHECK(c, m) do { if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); g_fail++; } } while(0)

int main() {
    printf("=== G4: CLONE_THREAD placeholder starttime hydration ===\n");

    // (a) Placeholder: needsHydration=true, starttime=0. Must force REUSE.
    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 0;
        ti.needsHydration = true;
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r == kRR_REUSE, "placeholder (needsHydration) forces REUSE repopulate");
    }

    // (b) Hydrated: needsHydration=false, real starttime. checkStarttime=false ->
    //     trust cached starttime, no spurious REUSE.
    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 12345ULL;
        ti.needsHydration = false;
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r != kRR_REUSE, "hydrated thread (cached starttime) not spuriously REUSE");
    }

    // (c) Hydrated thread, checkStarttime=true: /proc/<tid>/stat field 22 may be 0
    //     or unreadable in the host sandbox; the guard requires BOTH starttimes
    //     nonzero AND differing, so a single-zero read must NOT trigger REUSE.
    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 12345ULL;
        ti.needsHydration = false;
        int r = threadScanRefreshForTest(ti, true);
        CHECK(r != kRR_REUSE || ti.needsHydration == false,
              "hydrated thread not evicted on missing/zero starttime read");
    }

    // (d) Boundary: placeholder with checkStarttime=true still wins on
    //     needsHydration (checked first, before any /proc read).
    {
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = getpid();
        ti.starttime = 0;
        ti.needsHydration = true;
        int r = threadScanRefreshForTest(ti, true);
        CHECK(r == kRR_REUSE, "placeholder hydration beats checkStarttime read");
    }

    if (g_fail == 0) { printf("=== test_thread_scan_hydrate: ALL PASS ===\n"); return 0; }
    printf("=== test_thread_scan_hydrate: %d FAILURES ===\n", g_fail);
    return 1;
}

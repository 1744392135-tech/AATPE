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

    // (e) rel15 大小核颠倒: lastSwitchMs-fresh freeze-break. A thread frozen by
    //     skipSchedstat (idleRefreshes>=6) whose lastSwitchMs is still recent
    //     (< kInactiveMs) -- i.e. markThreadScoreDirty just logged a real
    //     sched_switch for it -- must have its idle tier reset on the very next
    //     calibration scan, instead of waiting up to kLongIdleRescanPeriod cycles
    //     for the forced rescan. The /proc schedstat read for this fake tid fails
    //     (returns KEEP), but the freeze-break block runs BEFORE that read and
    //     resets idleRefreshes. (P1-1: keys off lastSwitchMs freshness, not the
    //     wakeHint flag, because shouldRescoreThread consumes wakeHint within one
    //     main-loop turn, long before the 60s scan.)
    {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = 777777;   // fake tid; schedstat open will fail
        ti.needsHydration = false;
        ti.starttime = 12345ULL;
        ti.idleRefreshes = 20;                // frozen by skipSchedstat
        ti.longIdleRescan = 0;
        ti.switchesPerSec = 0.0;
        ti.nrSwitches = 1000;
        // Plant a FRESH lastSwitchMs (100ms ago) for this tid in the score cache.
        {
            std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
            g_scoreCache[777777].lastSwitchMs = nowMs - 100;
        }
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r == kRR_KEEP, "frozen thread refresh returns KEEP (schedstat unreadable)");
        CHECK(ti.idleRefreshes == 0,
              "fresh lastSwitchMs breaks skipSchedstat freeze: idleRefreshes reset to 0");
    }

    // (f) Control: a frozen thread whose lastSwitchMs is STALE (older than
    //     kInactiveMs) stays frozen. This pins that the freeze-break is driven
    //     specifically by a RECENT switch, not by the refresh itself.
    {
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ThreadInfo ti;
        ti.pid = getpid(); ti.tid = 888888;   // fake tid
        ti.needsHydration = false;
        ti.starttime = 12345ULL;
        ti.idleRefreshes = 20;
        ti.longIdleRescan = 0;
        ti.switchesPerSec = 0.0;
        ti.nrSwitches = 1000;
        // Plant a STALE lastSwitchMs (60s ago, >> kInactiveMs=10s).
        {
            std::lock_guard<std::mutex> lk(g_scoreCacheMutex);
            g_scoreCache[888888].lastSwitchMs = nowMs - 60000;
        }
        int r = threadScanRefreshForTest(ti, false);
        CHECK(r == kRR_KEEP, "frozen thread with stale lastSwitchMs returns KEEP");
        // idleRefreshes stays >= threshold: lastSwitchMs is not fresh, so the
        // freeze-break block does NOT reset idleRefreshes; skipSchedstat short-
        // circuits before any schedstat read.
        CHECK(ti.idleRefreshes >= 6,
              "stale lastSwitchMs: freeze persists (idleRefreshes untouched)");
    }

    if (g_fail == 0) { printf("=== test_thread_scan_hydrate: ALL PASS ===\n"); return 0; }
    printf("=== test_thread_scan_hydrate: %d FAILURES ===\n", g_fail);
    return 1;
}

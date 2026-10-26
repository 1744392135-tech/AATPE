// test_scoring_active.cpp - v600-fix2 regression: scoring-loop active/rescore
// decision extracted into aatped_utils.h shouldRescoreThread().
//
// Root cause this pins down:
//   * invalidateAllScores() runs every kRebalanceMs=1s and set dirty=true on
//     EVERY cached entry.
//   * The OLD predicate `dirty || neverScored || (expired && active)` did NOT
//     gate dirty on active, so sleeping/background threads were rescored every
//     second -> daemon CPU 43-47%, ~475 decisions/s.
//   * When eBPF is unavailable (polling/degraded), onSwitch never runs, so
//     lastSwitchMs stays 0 until the first score stamps it, then freezes. The
//     OLD `active = (lastSwitchMs==0) || ...` therefore treated EVERY thread as
//     active until first scoring, and after first scoring the forced dirty bit
//     kept rescoring sleeping threads.
//
// The FIX: shouldRescoreThread() gates (dirty||expired) on `active`, where
// `active` is the UNION of (eBPF recency, only trusted when ebpfLive) and
// (/proc schedstat: switchesPerSec>0 || idleRefreshes<6). neverScored always
// passes. The dirty bit is consumed regardless.
//
// This test links NO production .cpp TUs -- shouldRescoreThread() /
// threadIsActiveBySchedstat() are header-only inline in aatped_utils.h.
#include <cstdio>
#include "aatped.h"   // umbrella header: pulls aatped_types.h -> aatped_utils.h in order

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s  (got %d)\n", m, (int)(c)); } \
} while(0)

static constexpr long long kNow      = 100000;
static constexpr long long kTtl      = 500;
static constexpr long long kInactive  = 10000;

// A thread that has been sleeping for many scan cycles (nrSwitches not growing).
static ThreadInfo makeSleeping() {
    ThreadInfo ti;
    ti.idleRefreshes   = 20;   // >> kSchedstatIdleRefreshesActive=6
    ti.switchesPerSec  = 0.0;
    ti.nrSwitches      = 1000;
    ti.runTime         = 100000;
    return ti;
}

// A thread thread_scan currently sees running (nrSwitches growing).
static ThreadInfo makeRunning() {
    ThreadInfo ti;
    ti.idleRefreshes   = 0;
    ti.switchesPerSec  = 50.0;
    ti.nrSwitches      = 5000;
    ti.runTime         = 200000;
    return ti;
}

int main() {
    printf("=== v600-fix2: shouldRescoreThread active/dirty gating ===\n");

    // ---- 1. neverScored always rescores, even if the thread is sleeping ----
    {
        ThreadScoreEntry e;  // cachedAtMs=0, dirty=false by default
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == true, "neverScored sleeping thread -> rescore (first pass)");
        CHECK(e.dirty == false, "neverScored consumes dirty bit");
    }

    // ---- 2. eBPF OFF + dirty + sleeping thread -> MUST NOT rescore ---------
    // This is the exact root-cause scenario: invalidateAllScores set dirty=true
    // on every thread, eBPF is off (lastSwitchMs=0), thread is sleeping.
    // OLD code: active=(lastSwitchMs==0)=true -> dirty=true -> rescore. BUG.
    // NEW code: activeByEBPF=false (ebpf off), activeBySchedstat=false
    //           (idleRefreshes>=6) -> skip.
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;   // fresh cache (not expired)
        e.lastSwitchMs = 0;            // eBPF never ran
        e.dirty        = true;         // invalidateAllScores forced it
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == false, "eBPF-off + dirty + sleeping -> SKIP (root cause fix)");
        CHECK(e.dirty == false, "inactive-dirty bit is still consumed (not leaked)");
    }

    // ---- 3. eBPF OFF + dirty + running thread -> rescores -------------------
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = 0;
        e.dirty        = true;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == true, "eBPF-off + dirty + running -> rescore");
        CHECK(e.dirty == false, "active-dirty bit consumed");
    }

    // ---- 4. eBPF OFF + expired + sleeping -> MUST NOT rescore ---------------
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - kTtl - 1000;  // expired
        e.lastSwitchMs = 0;
        e.dirty        = false;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == false, "eBPF-off + expired + sleeping -> SKIP");
    }

    // ---- 5. eBPF OFF + expired + running -> rescores ------------------------
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - kTtl - 1000;
        e.lastSwitchMs = 0;
        e.dirty        = false;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == true, "eBPF-off + expired + running -> rescore (periodic refresh)");
    }

    // ---- 6. fresh cache, not dirty, running -> does NOT rescore -------------
    // Cache still valid; thread running but no event touched it. No waste.
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 10;
        e.lastSwitchMs = kNow - 10;
        e.dirty        = false;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == false, "fresh cache + running + clean -> SKIP (no spurious rescore)");
    }

    // ---- 7. eBPF LIVE + recent lastSwitchMs + dirty + sleeping schedstat ---
    // eBPF just saw a switch; schedstat hasn't caught up yet. Union -> active.
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - 50;          // recent eBPF event
        e.dirty        = true;
        ThreadInfo ti = makeSleeping();       // schedstat hasn't refreshed yet
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r == true, "eBPF-live + recent switch + dirty -> rescore (event-driven)");
    }

    // ---- 8. eBPF LIVE + stale lastSwitchMs + sleeping schedstat -------------
    // Both signals agree: thread is inactive. Dirty (forced by rebalance) must
    // NOT trigger a rescore.
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;  // too old
        e.dirty        = true;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r == false, "eBPF-live + stale switch + sleeping + dirty -> SKIP");
    }

    // ---- 9. eBPF LIVE + stale lastSwitchMs but schedstat running ------------
    // eBPF may have missed recent switches (brief gap); schedstat says running.
    // Union must NOT drop a running thread.
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r == true, "eBPF-live stale + schedstat running + dirty -> rescore (union)");
    }

    // ---- 10. eBPF OFF + lastSwitchMs=0 (uninitialized) + running ----------
    // After first scoring lastSwitchMs gets stamped; if eBPF never came up it
    // freezes. The schedstat signal must keep running threads active.
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = 0;
        e.dirty        = true;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == true, "eBPF-off + lastSwitchMs==0 + running -> rescore via schedstat");
    }

    // ---- 11. Quantitative: 500 sleeping + 10 running, invalidateAllScores ---
    // Simulate the global rebalance backstick: dirty=true on every entry.
    // Assert only running threads (+ fresh neverScored) get rescored; sleeping
    // threads do NOT. This is the 320-decisions/s -> ~active-thread-count
    // regression.
    {
        const int kSleeping = 500;
        const int kRunning  = 10;
        int rescored = 0;
        int dirtyConsumed = 0;
        for (int i = 0; i < kSleeping; i++) {
            ThreadScoreEntry e;
            e.cachedAtMs   = kNow - 100;
            e.lastSwitchMs = 0;
            e.dirty        = true;
            ThreadInfo ti = makeSleeping();
            bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
            if (r) rescored++;
            if (!e.dirty) dirtyConsumed++;
        }
        for (int i = 0; i < kRunning; i++) {
            ThreadScoreEntry e;
            e.cachedAtMs   = kNow - 100;
            e.lastSwitchMs = 0;
            e.dirty        = true;
            ThreadInfo ti = makeRunning();
            bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
            if (r) rescored++;
            if (!e.dirty) dirtyConsumed++;
        }
        CHECK(rescored == kRunning,
              "quant: only running threads rescored after invalidateAllScores");
        CHECK(dirtyConsumed == kSleeping + kRunning,
              "quant: dirty bit consumed on every entry (no leak)");
        printf("  [INFO] rescored=%d (expected %d sleeping=%d running=%d)\n",
               rescored, kRunning, kSleeping, kRunning);
    }

    // ---- 12. Fresh/new thread (cachedAtMs=0) with sleeping schedstat --------
    // neverScored must pass even though schedstat says idle (thread just added).
    {
        ThreadScoreEntry e;   // cachedAtMs=0
        e.dirty = false;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/false);
        CHECK(r == true, "neverScored (cachedAtMs==0) always passes regardless of schedstat");
    }

    printf("=== v600-fix2: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

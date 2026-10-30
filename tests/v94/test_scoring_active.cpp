#include <cstdio>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s  (got %d)\n", m, (int)(c)); } \
} while(0)

static constexpr long long kNow      = 100000;
static constexpr long long kTtl      = 500;
static constexpr long long kInactive  = 10000;

static ThreadInfo makeSleeping() {
    ThreadInfo ti;
    ti.idleRefreshes   = 20;
    ti.switchesPerSec  = 0.0;
    ti.nrSwitches      = 1000;
    ti.runTime         = 100000;
    return ti;
}

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

    {
        ThreadScoreEntry e;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == true, "neverScored sleeping thread -> rescore (first pass)");
        CHECK(e.dirty == false, "neverScored consumes dirty bit");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = 0;
        e.dirty        = true;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == false, "eBPF-off + dirty + sleeping -> SKIP (root cause fix)");
        CHECK(e.dirty == false, "inactive-dirty bit is still consumed (not leaked)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = 0;
        e.dirty        = true;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == true, "eBPF-off + dirty + running -> rescore");
        CHECK(e.dirty == false, "active-dirty bit consumed");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - kTtl - 1000;
        e.lastSwitchMs = 0;
        e.dirty        = false;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == false, "eBPF-off + expired + sleeping -> SKIP");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - kTtl - 1000;
        e.lastSwitchMs = 0;
        e.dirty        = false;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == true, "eBPF-off + expired + running -> rescore (periodic refresh)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 10;
        e.lastSwitchMs = kNow - 10;
        e.dirty        = false;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == false, "fresh cache + running + clean -> SKIP (no spurious rescore)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - 50;
        e.dirty        = true;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == true, "eBPF-live + recent switch + dirty -> rescore (event-driven)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == false, "eBPF-live + stale switch + sleeping + dirty -> SKIP");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == true, "eBPF-live stale + schedstat running + dirty -> rescore (union)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = 0;
        e.dirty        = true;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == true, "eBPF-off + lastSwitchMs==0 + running -> rescore via schedstat");
    }

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
            bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
            if (r) rescored++;
            if (!e.dirty) dirtyConsumed++;
        }
        for (int i = 0; i < kRunning; i++) {
            ThreadScoreEntry e;
            e.cachedAtMs   = kNow - 100;
            e.lastSwitchMs = 0;
            e.dirty        = true;
            ThreadInfo ti = makeRunning();
            bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
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

    {
        ThreadScoreEntry e;
        e.dirty = false;
        ThreadInfo ti = makeSleeping();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false);
        CHECK(r == true, "neverScored (cachedAtMs==0) always passes regardless of schedstat");
    }

    printf("=== v600-fix2: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#include <cstdio>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static constexpr long long kNow     = 100000;
static constexpr long long kTtl     = 500;
static constexpr long long kInactive = 10000;

static ThreadInfo makeFrozen() {
    ThreadInfo ti;
    ti.idleRefreshes  = 20;
    ti.switchesPerSec = 0.0;
    ti.nrSwitches     = 1000;
    ti.runTime        = 100000;
    return ti;
}

static ThreadInfo makeRunning() {
    ThreadInfo ti;
    ti.idleRefreshes  = 0;
    ti.switchesPerSec = 50.0;
    ti.nrSwitches     = 5000;
    ti.runTime        = 200000;
    return ti;
}

int main() {
    printf("=== rel15 大小核颠倒: emergency-exit freeze-recovery ===\n");

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        ThreadInfo ti = makeFrozen();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == false, "BUG-PATH: frozen worker + stale eBPF + dirty -> SKIP (rel15 symptom)");
        CHECK(e.dirty == false, "dirty consumed on the skipped path");
        CHECK(e.forceRescore == false, "forceRescore NOT auto-set by the gate");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        e.forceRescore = true;
        ThreadInfo ti = makeFrozen();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == true, "FIX: forceRescore rescues frozen worker after EMERGENCY->NORMAL");
        CHECK(e.forceRescore == false, "forceRescore consumed (one-shot, not sticky)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = false;
        e.forceRescore = true;
        ThreadInfo ti = makeFrozen();
        bool r1 = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r1 == true, "one-shot: first loop rescored");
        e.dirty = true;
        bool r2 = shouldRescoreThread(e, ti, kNow + 1000, kTtl, kInactive, true);
        CHECK(r2 == false, "one-shot: second loop does NOT rescore frozen worker (no CPU regression)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        e.wakeHint     = true;
        ThreadInfo ti = makeFrozen();
        bool r1 = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r1 == true, "wakeHint: first call rescore (real sched_switch)");
        CHECK(e.wakeHint == false, "wakeHint consumed by first call (one rescore per wake)");
        e.dirty = false;
        bool r2 = shouldRescoreThread(e, ti, kNow + 200, kTtl, kInactive, true);
        CHECK(r2 == false, "wakeHint: second call SKIP (no repeated per-loop rescore)");
        e.wakeHint = true;
        bool r3 = shouldRescoreThread(e, ti, kNow + 400, kTtl, kInactive, true);
        CHECK(r3 == true, "wakeHint: subsequent fresh switch -> rescore again (edge-triggered)");
        CHECK(e.wakeHint == false, "second wakeHint also consumed");
    }

    {
        const int kSleeping = 500;
        const int kRunning  = 10;
        int rescored = 0;
        for (int i = 0; i < kSleeping; i++) {
            ThreadScoreEntry e;
            e.cachedAtMs   = kNow - 100;
            e.lastSwitchMs = 0;
            e.dirty        = true;
            ThreadInfo ti = makeFrozen();
            if (shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false)) rescored++;
        }
        for (int i = 0; i < kRunning; i++) {
            ThreadScoreEntry e;
            e.cachedAtMs   = kNow - 100;
            e.lastSwitchMs = 0;
            e.dirty        = true;
            ThreadInfo ti = makeRunning();
            if (shouldRescoreThread(e, ti, kNow, kTtl, kInactive, false)) rescored++;
        }
        CHECK(rescored == kRunning,
              "CPU-regression: only running threads rescored after invalidateAllScores (sleeping skipped)");
        printf("  [INFO] rescored=%d (expected %d sleeping=%d running=%d)\n",
               rescored, kRunning, kSleeping, kRunning);
    }

    {
        ThreadScoreEntry e;
        e.dirty = false;
        ThreadInfo ti = makeFrozen();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == true, "neverScored still bypasses active gate (first placement)");
    }

    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 10;
        e.lastSwitchMs = kNow - 10;
        e.dirty        = false;
        ThreadInfo ti = makeRunning();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == false, "running + fresh cache + clean -> SKIP (no waste)");
    }

    printf("=== rel15 freeze-recovery: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

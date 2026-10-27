// test_rel15_freeze.cpp - rel15 "大小核颠倒" regression test.
//
// Root cause (confirmed by host BED aatpe_bed.cpp and on-device audit):
//   * shouldRescoreThread() gates (dirty||expired) on `active`. A low-switch-rate
//     CPU-bound worker (logic/physics/JobSystem/GPU backend) that was migrated to
//     little cores during EMERGENCY is judged active=false after thermal recovers,
//     because:
//       - onSwitch() is globally throttled 1/10, so the worker's eBPF mark ages
//         past kInactiveMs=10s; and
//       - thread_scan's skipSchedstat freezes its schedstat view after 6 idle
//         refreshes (idleRefreshes>=6, switchesPerSec==0).
//     -> active=false -> rescore permanently skipped -> worker stays pinned on
//        little cores (rel14 promoted at cycle0; rel15 waited until cycle11).
//
// The FIX (direction given by task):
//   * Keep the active gate normally (daemon CPU must not regress to 43%).
//   * Add ONE-SHOT bypasses:
//       - forceRescore: main loop sets it on EMERGENCY->NORMAL (and other
//         high->NORMAL transitions) so affected threads are re-scored once.
//       - wakeHint: onSwitch sets it on a real sched_switch; shouldRescoreThread
//         rescores unconditionally (a real switch IS the active evidence), and
//         thread_scan consumes it to break the skipSchedstat freeze immediately.
//   * Both are edge-triggered and consumed (forceRescore immediately; wakeHint by
//     thread_scan) so there is NO per-second full rescore.
//
// This test links NO production .cpp TUs -- shouldRescoreThread() /
// threadIsActiveBySchedstat() are header-only inline in aatped_utils.h.
#include <cstdio>
#include "aatped.h"   // umbrella header: pulls aatped_types.h -> aatped_utils.h in order

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static constexpr long long kNow     = 100000;
static constexpr long long kTtl     = 500;
static constexpr long long kInactive = 10000;

// A thread thread_scan has FROZEN: it was idle long enough for skipSchedstat to
// kick in (idleRefreshes>=6) and switchesPerSec is frozen at 0. Mirrors the
// audit's mali-cmar-backe worker pinned on LITTLE at 52% CPU.
static ThreadInfo makeFrozen() {
    ThreadInfo ti;
    ti.idleRefreshes  = 20;   // >> kSchedstatIdleRefreshesActive=6
    ti.switchesPerSec = 0.0;  // frozen by skipSchedstat
    ti.nrSwitches     = 1000;
    ti.runTime        = 100000;
    return ti;
}

// A thread thread_scan currently sees running.
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

    // ---- 1. BUG REPRODUCTION: frozen worker + stale eBPF + dirty -> SKIP -----
    // This is the exact rel15 failure: invalidateAllScores set dirty=true, but
    // active=false (eBPF aged out + schedstat frozen). shouldRescoreThread must
    // SKIP (this is the bug we are fixing; we assert the bypass path rescored).
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;  // aged past kInactiveMs
        e.dirty        = true;
        ThreadInfo ti = makeFrozen();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r == false, "BUG-PATH: frozen worker + stale eBPF + dirty -> SKIP (rel15 symptom)");
        CHECK(e.dirty == false, "dirty consumed on the skipped path");
        CHECK(e.forceRescore == false, "forceRescore NOT auto-set by the gate");
    }

    // ---- 2. FIX: EMERGENCY->NORMAL forceRescore rescues the frozen worker ----
    // The main loop calls forceRescoreAllScores() on EMERGENCY->NORMAL. Here we
    // simulate that by setting forceRescore=true on the frozen entry. The SAME
    // thread that was skipped above MUST now be re-scored (promoted back to BIG).
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        e.forceRescore = true;   // <-- set by main loop on EMERGENCY->NORMAL
        ThreadInfo ti = makeFrozen();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r == true, "FIX: forceRescore rescues frozen worker after EMERGENCY->NORMAL");
        CHECK(e.forceRescore == false, "forceRescore consumed (one-shot, not sticky)");
    }

    // ---- 3. forceRescore is truly one-shot: second loop does NOT rescore ----
    // After the first rescue loop consumed forceRescore, the frozen worker must NOT
    // be re-scored every subsequent loop (that would re-introduce the 43% CPU).
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = false;
        e.forceRescore = true;
        ThreadInfo ti = makeFrozen();
        // first loop: rescue
        bool r1 = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r1 == true, "one-shot: first loop rescored");
        // second loop: forceRescore already consumed, dirty=false, active=false -> skip
        e.dirty = true;  // simulate next invalidateAllScores()
        bool r2 = shouldRescoreThread(e, ti, kNow + 1000, kTtl, kInactive, true);
        CHECK(r2 == false, "one-shot: second loop does NOT rescore frozen worker (no CPU regression)");
    }

    // ---- 4. wakeHint: one-shot rescore, then SKIP (P1-1 red-team fix) --------
    // onSwitch() -> markThreadScoreDirty() sets wakeHint=true when a real switch
    // happens. The first call must rescore (wake event). The second call, with
    // wakeHint already consumed and no new switch, must SKIP -- otherwise a thread
    // that switches once then sleeps gets re-scored every 200ms forever
    // (red-team 300/300 repro, daemon CPU back to ~15-20%).
    {
        ThreadScoreEntry e;
        e.cachedAtMs   = kNow - 100;
        e.lastSwitchMs = kNow - kInactive - 500;
        e.dirty        = true;
        e.wakeHint     = true;   // <-- set by onSwitch
        ThreadInfo ti = makeFrozen();
        bool r1 = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r1 == true, "wakeHint: first call rescore (real sched_switch)");
        CHECK(e.wakeHint == false, "wakeHint consumed by first call (one rescore per wake)");
        // second call 200ms later: wakeHint gone, dirty consumed, active=false -> SKIP
        e.dirty = false;
        bool r2 = shouldRescoreThread(e, ti, kNow + 200, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r2 == false, "wakeHint: second call SKIP (no repeated per-loop rescore)");
        // a THIRD call with a NEW wake event must rescore again (each switch = one rescore)
        e.wakeHint = true;
        bool r3 = shouldRescoreThread(e, ti, kNow + 400, kTtl, kInactive, /*ebpfLive=*/true);
        CHECK(r3 == true, "wakeHint: subsequent fresh switch -> rescore again (edge-triggered)");
        CHECK(e.wakeHint == false, "second wakeHint also consumed");
    }

    // ---- 5. NO CPU REGRESSION: 500 sleeping threads, invalidateAllScores ------
    // The normal active gate MUST keep skipping sleeping threads. Only running
    // threads (+ neverScored) get rescored. This pins the daemon-CPU fix.
    {
        const int kSleeping = 500;
        const int kRunning  = 10;
        int rescored = 0;
        for (int i = 0; i < kSleeping; i++) {
            ThreadScoreEntry e;
            e.cachedAtMs   = kNow - 100;
            e.lastSwitchMs = 0;
            e.dirty        = true;
            // no forceRescore, no wakeHint: these are sleeping background threads
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

    // ---- 6. neverScored still bypasses everything (first placement) ----------
    {
        ThreadScoreEntry e;   // cachedAtMs==0
        e.dirty = false;
        ThreadInfo ti = makeFrozen();
        bool r = shouldRescoreThread(e, ti, kNow, kTtl, kInactive, true);
        CHECK(r == true, "neverScored still bypasses active gate (first placement)");
    }

    // ---- 7. Running thread with fresh cache + no event -> no spurious rescore
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

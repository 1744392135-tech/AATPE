// test_ebpf_h01h02.cpp — host regression tests for the two High eBPF findings
// independently reviewed on 2026-10-29:
//
//   H01  Soft-trust delivery gating. On a locked ROM (tracefs unreadable) on a
//        known 4.14 layout family, postLoadOffsetDegradeDecision() decides to
//        trust the baked-in offsets and stay EVENT_DRIVEN (raising
//        offsetUnverifiedButTrusted_). BEFORE this fix the deliver*Event() gates
//        trusted ONLY VERIFIED||NO_CTX, so every fork/switch/wakeup event from a
//        UNKNOWN tracepoint was silently offsetFilteredCount_++ -- while the
//        WebUI still reported TRUSTED_BUILTIN. The soft-trust flag was never
//        consulted on the delivery path. After the fix, an UNKNOWN tracepoint is
//        delivered exactly when the load committed to soft-trust; DEGRADED and
//        UNKNOWN-without-soft-trust remain conservatively dropped.
//
//   H02  bpftool fallback attach table. sched_events.bpf.c is tracepoint-only and
//        ships six programs (fork/exit/switch/exec/wakeup/wakeup_new). BEFORE the
//        fix the bpftool fallback loop only opened+attached the first four, and
//        paired them with kprobe guesses that do not exist in this object; the
//        wakeup programs -- the only writers of the wakeup_cache map that runq
//        latency is derived from -- were never attached. After the fix the attach
//        table covers all six and attaches each straight to its tracepoint.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include "aatpe_ebpf.h"
using namespace aatpe_ebpf;

// Host integrity gate shim (identical to the other ebpf host tests): production
// aatpe_ebpf.cpp calls aatpe::integrity::privilegedOpsAllowed(); default = Ok.
namespace aatpe { namespace integrity {
static int g_testVerdict = 1;   // 1 == Ok
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// ---------------------------------------------------------------------------
// H01: soft-trust must let UNKNOWN ctx-reading events through; but DEGRADED and
// UNKNOWN-without-soft-trust must still be dropped.
// ---------------------------------------------------------------------------
static void test_h01_soft_trust_delivery(void) {
    printf("[H01] soft-trust UNKNOWN events are delivered, not offset-filtered\n");

    // --- Case A: known 4.14 family + tracefs unreadable (all UNKNOWN) -> soft
    // trust in force -> fork/switch/runq events must NOT be filtered.
    {
        EbpfScheduler s;
        s.g4TestEnterAttached();
        // sw=UNKNOWN + knownFamily=true -> postLoad raises soft-trust, stays
        // EVENT_DRIVEN (the production locked-ROM path).
        s.g4TestApplyOffsetStatus(TpOffsetStatus::UNKNOWN, /*knownFamily=*/true);
        s.hostTestSetOffsetStatus('f', TpOffsetStatus::UNKNOWN);  // fork also unreadable
        s.hostTestSetOffsetStatus('w', TpOffsetStatus::UNKNOWN);  // wakeup also unreadable
        CHECK(s.isOffsetUnverifiedButTrusted() == true,
              "A: soft-trust flag raised (known family + UNKNOWN)");
        CHECK(std::string(s.getDetailedStateString()) == "TRUSTED_BUILTIN",
              "A: WebUI state is TRUSTED_BUILTIN");

        uint64_t before = s.getOffsetFiltered();

        ForkEvent fe{}; fe.hdr.type = (uint32_t)EventType::FORK;
        s.hostTestDeliverFork(fe);
        SwitchEvent se{}; se.hdr.type = (uint32_t)EventType::SWITCH;
        s.hostTestDeliverSwitch(se);
        RunqEvent re{}; re.hdr.type = (uint32_t)EventType::RUNQ;
        s.hostTestDeliverRunq(re);

        uint64_t after = s.getOffsetFiltered();
        CHECK(after == before,
              "A: soft-trust UNKNOWN fork/switch/runq NOT offset-filtered (delivered)");
        CHECK(s.hostTestOffsetWarnFired('f') == false,
              "A: no offset-gate WARN for a delivered soft-trust fork event");
        s.unload();
    }

    // --- Case B: non-known family + tracefs unreadable (UNKNOWN) -> NO soft
    // trust -> fork must STILL be dropped (conservative).
    {
        EbpfScheduler s;
        s.g4TestEnterAttached();
        s.g4TestApplyOffsetStatus(TpOffsetStatus::UNKNOWN, /*knownFamily=*/false);
        s.hostTestSetOffsetStatus('f', TpOffsetStatus::UNKNOWN);
        CHECK(s.isOffsetUnverifiedButTrusted() == false,
              "B: unknown family + UNKNOWN => no soft-trust");
        uint64_t before = s.getOffsetFiltered();
        ForkEvent fe{}; fe.hdr.type = (uint32_t)EventType::FORK;
        s.hostTestDeliverFork(fe);
        CHECK(s.getOffsetFiltered() == before + 1,
              "B: UNKNOWN without soft-trust fork is still dropped");
        s.g4TestStopPollThread();
    }

    // --- Case C: known family but live format MISMATCH (DEGRADED) -> real
    // evidence of wrong offsets -> fork must STILL be dropped, even though the
    // kernel family is known. Soft-trust never applies to DEGRADED.
    {
        EbpfScheduler s;
        s.g4TestEnterAttached();
        s.g4TestApplyOffsetStatus(TpOffsetStatus::DEGRADED, /*knownFamily=*/true);
        s.hostTestSetOffsetStatus('f', TpOffsetStatus::DEGRADED);
        CHECK(s.isOffsetUnverifiedButTrusted() == false,
              "C: DEGRADED mismatch is never soft-trusted");
        uint64_t before = s.getOffsetFiltered();
        ForkEvent fe{}; fe.hdr.type = (uint32_t)EventType::FORK;
        s.hostTestDeliverFork(fe);
        CHECK(s.getOffsetFiltered() == before + 1,
              "C: DEGRADED fork still dropped (real mismatch evidence)");
        s.g4TestStopPollThread();
    }
}

// ---------------------------------------------------------------------------
// H02: the bpftool fallback attach table must cover all six tracepoint programs,
// including the two wakeup programs whose wakeup_cache writes feed runq latency.
// ---------------------------------------------------------------------------
static void test_h02_bpftool_attach_table(void) {
    printf("[H02] bpftool fallback attach table covers wakeup/wakeup_new\n");
    const auto& tbl = EbpfScheduler::hostTestBpftoolAttachTable();
    CHECK(tbl.size() == 6, "H02: attach table has exactly the 6 tracepoint programs");

    bool fork=false, exit_=false, sw=false, exec=false, wk=false, wkn=false;
    for (const auto& row : tbl) {
        if (row.second == "tracepoint/sched/sched_process_fork")     fork = true;
        if (row.second == "tracepoint/sched/sched_process_exit")     exit_ = true;
        if (row.second == "tracepoint/sched/sched_switch")           sw = true;
        if (row.second == "tracepoint/sched/sched_process_exec")     exec = true;
        if (row.second == "tracepoint/sched/sched_wakeup")          wk = true;
        if (row.second == "tracepoint/sched/sched_wakeup_new")       wkn = true;
    }
    CHECK(fork && exit_ && sw && exec, "H02: fork/exit/switch/exec attached");
    CHECK(wk,   "H02: sched_wakeup attached (writes wakeup_cache)");
    CHECK(wkn,  "H02: sched_wakeup_new attached (writes wakeup_cache)");

    // The wakeup programs pin under their ELF function names so loadall pins can
    // be opened by the loop.
    bool pinWakeup=false, pinWakeupNew=false;
    for (const auto& row : tbl) {
        if (row.first == "handle_wakeup")     pinWakeup = true;
        if (row.first == "handle_wakeup_new") pinWakeupNew = true;
    }
    CHECK(pinWakeup && pinWakeupNew,
          "H02: wakeup/wakeup_new pin names enumerated (were missing pre-fix)");
}

int main(void) {
    printf("=== eBPF H01/H02 independent-review host tests ===\n");
    test_h01_soft_trust_delivery();
    test_h02_bpftool_attach_table();
    printf("=== RESULT: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include "aatpe_ebpf.h"
using namespace aatpe_ebpf;

namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static void test_h01_soft_trust_delivery(void) {
    printf("[H01] soft-trust UNKNOWN events are delivered, not offset-filtered\n");

    {
        EbpfScheduler s;
        s.g4TestEnterAttached();
        s.g4TestApplyOffsetStatus(TpOffsetStatus::UNKNOWN, true);
        s.hostTestSetOffsetStatus('f', TpOffsetStatus::UNKNOWN);
        s.hostTestSetOffsetStatus('w', TpOffsetStatus::UNKNOWN);
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

    {
        EbpfScheduler s;
        s.g4TestEnterAttached();
        s.g4TestApplyOffsetStatus(TpOffsetStatus::UNKNOWN, false);
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

    {
        EbpfScheduler s;
        s.g4TestEnterAttached();
        s.g4TestApplyOffsetStatus(TpOffsetStatus::DEGRADED, true);
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

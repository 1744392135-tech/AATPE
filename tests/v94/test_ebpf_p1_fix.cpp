#include <cstdio>
#include <cstring>
#include <string>
#include "aatpe_ebpf.h"
#include "integrity.h"

namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

extern "C" void aatpe_host_test_set_bpf_inject(uint32_t flags);

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    using namespace aatpe_ebpf;
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    aatpe_host_test_set_bpf_inject(0);

    printf("=== EBPF P1 regression tests ===\n");

    printf("--- EBPF-05: isEventDrivenLive() requires running poll thread ---\n");
    {
        EbpfScheduler s;
        CHECK(s.isEventDrivenLive() == false, "fresh scheduler: not live");

        s.g4TestEnterAttached();
        CHECK(s.isEventDrivenLive() == false,
              "attached but poll thread NOT running: isEventDrivenLive()==false");

        s.g4TestEnterRingLossHybrid();
        CHECK(s.isEventDrivenLive() == false,
              "HYBRID (ring-loss, poll thread stopped): isEventDrivenLive()==false");

        s.g4TestEnterAttached();
        CHECK(s.isEventDrivenLive() == false,
              "EVENT_DRIVEN but poll thread not started: still false");
    }

    printf("--- EBPF-03: unload() drains SPSC queues and switchAgg_ ---\n");
    {
        EbpfScheduler s;
        s.g4TestPushFakeCriticalEvent();
        CHECK(s.g4TestCriticalQueueDepth() == 1, "fake event pushed (depth=1)");

        s.unload();
        CHECK(s.g4TestCriticalQueueDepth() == 0,
              "after unload(): criticalQueue_ empty (no stale FORK/EXIT)");
        CHECK(s.g4TestNormalQueueDepth() == 0,
              "after unload(): normalQueue_ empty");
        CHECK(s.g4TestSwitchAggSize() == 0,
              "after unload(): switchAgg_ cleared (no stale aggregates)");
    }

    printf("--- EBPF-04: poll thread start failure -> tear down, not silent HYBRID ---\n");
    {
        EbpfScheduler s;
        aatpe_host_test_set_bpf_inject(0x1u | 0x8u | 0x10u);
        bool r = s.load();
        (void)r;
        CHECK(s.isEventDrivenLive() == false,
              "with poll-thread-fail injection: not event-driven live");
        std::string mode = s.getModeString() ? s.getModeString() : "";
        CHECK(mode == "polling", "landed in polling (not HYBRID with unmanned rings)");
        CHECK(s.g4TestPollThreadRunning() == false,
              "poll thread is NOT running (no silent leak)");
        s.g4TestStopPollThread();
        aatpe_host_test_set_bpf_inject(0);
    }

    printf("--- EBPF-04b: normal host load (no caps) -> POLLING, no crash ---\n");
    {
        EbpfScheduler s;
        aatpe_host_test_set_bpf_inject(0);
        bool r = s.load();
        CHECK(r == true || r == false, "load() returns without crash");
        CHECK(s.isEventDrivenLive() == false, "host (no real BPF): not live");
        std::string mode = s.getModeString() ? s.getModeString() : "";
        CHECK(mode == "polling", "host default: polling fallback");
        s.g4TestStopPollThread();
    }

    printf("--- EBPF-01/02: kprobe ring path -> setupPerCpuRingBuffers(); "
           "RUNTIME COVERAGE IS DEVICE-ONLY (no host CHECK kept)\n");

    printf("\n========================================\n"
           "EBPF P1 fix results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

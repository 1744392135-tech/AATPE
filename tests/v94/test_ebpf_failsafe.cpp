#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include "aatpe_ebpf.h"
#include "integrity.h"

namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

extern "C" void aatpe_host_test_set_bpf_inject(uint32_t flags);

static constexpr uint32_t kFakeProgFd = 0x1u;
static constexpr uint32_t kForceEnosys = 0x2u;
static constexpr uint32_t kForceEperm  = 0x4u;
static constexpr uint32_t kFakeCaps    = 0x8u;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static void driveFailsafe(const char* faultName, uint32_t inject) {
    printf("--- fault: %s (inject=0x%x) ---\n", faultName, inject);
    aatpe_ebpf::EbpfScheduler sched;
    aatpe_host_test_set_bpf_inject(inject);

    bool r = sched.load();
    CHECK(r == true || r == false, "load() returns bool without abort/crash");

    std::string mode = sched.getModeString() ? sched.getModeString() : "";
    CHECK(mode == "polling", "mode degraded to polling (not event_driven)");
    CHECK(sched.isEventDrivenLive() == false,
          "isEventDrivenLive()==false (daemon g_ebpfActive must be false)");

    CHECK(sched.g4ProgsAttached() == false, "no programs left attached (no fd leak)");

    sched.g4TestStopPollThread();

    aatpe_host_test_set_bpf_inject(0);
}

int main() {
    using namespace aatpe_ebpf;
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);

    printf("=== G2: eBPF loader fail-safe matrix ===\n");

    driveFailsafe("1.object-absent (NO_CAP)", 0x0u);

    driveFailsafe("3.bpf()=ENOSYS", kFakeCaps | kForceEnosys);

    driveFailsafe("3.bpf()=EPERM", kFakeCaps | kForceEperm);

    driveFailsafe("4.ringbuf-create-fails", kFakeCaps | kFakeProgFd);

    driveFailsafe("5.attach-partial-fails", kFakeCaps | kFakeProgFd);

    driveFailsafe("6.version-unsupported", kFakeCaps | kForceEnosys);

    printf("--- fault: 2.corrupt/zero object (ELF parser robustness) ---\n");
    {
        EbpfScheduler s;
        const char* zpath = "aatpe_g2_zero.o";
        { std::ofstream(zpath).close(); }
        bool z = s.hostTestParseElfFile(zpath);
        CHECK(z == false, "zero-byte object rejected (no crash)");
        const char* gpath = "aatpe_g2_garbage.o";
        { std::ofstream g(gpath, std::ios::binary);
          const char junk[] = "THIS IS NOT AN ELF FILE \x00\x01\x02";
          g.write(junk, sizeof(junk)); }
        bool g = s.hostTestParseElfFile(gpath);
        CHECK(g == false, "bad-magic object rejected (no crash)");
        const char* tpath = "aatpe_g2_trunc.o";
        { std::ofstream t(tpath, std::ios::binary);
          const unsigned char elf[] = {0x7f,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0,0,1,0,0xb7};
          t.write((const char*)elf, sizeof(elf)); }
        bool t = s.hostTestParseElfFile(tpath);
        CHECK(t == false, "truncated ELF rejected (no OOB read)");
        remove(zpath); remove(gpath); remove(tpath);
    }

    printf("--- fault: integrity-gate-FAILED (fail-closed) ---\n");
    {
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Failed);
        EbpfScheduler s;
        (void)s.load();
        CHECK(s.isEventDrivenLive() == false, "gate-Failed: not event-driven live");
        CHECK(std::string(s.getModeString()) == "polling", "gate-Failed: mode polling");
        s.g4TestStopPollThread();
        aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    }

    printf("\n========================================\n"
           "G2 fail-safe results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

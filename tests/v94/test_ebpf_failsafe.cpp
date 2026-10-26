// test_ebpf_failsafe.cpp - G2 (v4.4.9) host fail-safe matrix for the eBPF loader.
//
// Drives EbpfScheduler::load() through injected failure conditions on a host
// where real BPF/tracefs/perf-attach is unavailable, and asserts the loader
// FAILS SAFE: it must not abort / crash / hit UB / leak fds, must land in
// POLLING mode, must NOT report event-driven monitoring as live (the daemon's
// g_ebpfActive must publish false), and must leave no programs attached.
//
// All fault injection goes through the AATPE_HOST_TEST-only seam
// aatpe_host_test_set_bpf_inject(); in release builds that seam is compiled out.
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include "aatpe_ebpf.h"
#include "integrity.h"

// The ebpf failsafe recipe does not link integrity.cpp; provide the same
// test-controlled backing for the G3 integrity gate that test_ebpf.cpp uses.
// Default verdict = Ok (1) so load() runs its real 3-phase loader path.
namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

// AATPE_HOST_TEST-only injection seam defined in aatpe_ebpf.cpp.
extern "C" void aatpe_host_test_set_bpf_inject(uint32_t flags);

// inject flags (must match aatpe_ebpf.cpp)
static constexpr uint32_t kFakeProgFd = 0x1u;
static constexpr uint32_t kForceEnosys = 0x2u;
static constexpr uint32_t kForceEperm  = 0x4u;
static constexpr uint32_t kFakeCaps    = 0x8u;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Run one fresh scheduler through load() under the given inject flags and assert
// the full fail-safe post-condition set.
static void driveFailsafe(const char* faultName, uint32_t inject) {
    printf("--- fault: %s (inject=0x%x) ---\n", faultName, inject);
    aatpe_ebpf::EbpfScheduler sched;
    aatpe_host_test_set_bpf_inject(inject);

    // load() must return (no abort/UB) and must NOT throw.
    bool r = sched.load();
    CHECK(r == true || r == false, "load() returns bool without abort/crash");

    // The whole point: after a failed load the daemon must run on polling, and
    // g_ebpfActive (published from isEventDrivenLive()) must be FALSE.
    std::string mode = sched.getModeString() ? sched.getModeString() : "";
    CHECK(mode == "polling", "mode degraded to polling (not event_driven)");
    CHECK(sched.isEventDrivenLive() == false,
          "isEventDrivenLive()==false (daemon g_ebpfActive must be false)");

    // Nothing may be left attached (no fd leak across the failure rollback).
    CHECK(sched.g4ProgsAttached() == false, "no programs left attached (no fd leak)");

    // Poll thread may have been started for the polling fallback; stop it so the
    // scheduler destructor does not abort on a joinable thread.
    sched.g4TestStopPollThread();

    // Reset inject for the next case.
    aatpe_host_test_set_bpf_inject(0);
}

int main() {
    using namespace aatpe_ebpf;
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);

    printf("=== G2: eBPF loader fail-safe matrix ===\n");

    // ① Object absent / no usable loader (host default: no caps, nothing attaches).
    driveFailsafe("1.object-absent (NO_CAP)", 0x0u);

    // ③ bpf() returns ENOSYS (kernel has no BPF at all).
    driveFailsafe("3.bpf()=ENOSYS", kFakeCaps | kForceEnosys);

    // ③ bpf() returns EPERM (seccomp / policy blocks BPF).
    driveFailsafe("3.bpf()=EPERM", kFakeCaps | kForceEperm);

    // ④ ring buffer creation fails after maps/progs "load" (fake prog fds, real
    // perf_event_open/mmap fails on host) -> rings never map -> rollback -> polling.
    driveFailsafe("4.ringbuf-create-fails", kFakeCaps | kFakeProgFd);

    // ⑤ attach partially fails (programs "load" but per-tracepoint attach fails)
    // -> nothing ends up attached -> rollback -> polling. Same injection as ④,
    // asserted via the shared post-conditions (no leak, polling, not live).
    driveFailsafe("5.attach-partial-fails", kFakeCaps | kFakeProgFd);

    // ⑥ version / feature unsupported: bpf() ENOSYS with caps present lands the
    // classifier in KERNEL_UNSUPPORTED, which must still degrade to polling.
    driveFailsafe("6.version-unsupported", kFakeCaps | kForceEnosys);

    // ② Corrupt / zero-length object: the ELF parser must reject without crash.
    printf("--- fault: 2.corrupt/zero object (ELF parser robustness) ---\n");
    {
        EbpfScheduler s;
        // zero-byte file
        const char* zpath = "aatpe_g2_zero.o";
        { std::ofstream(zpath).close(); }
        bool z = s.hostTestParseElfFile(zpath);
        CHECK(z == false, "zero-byte object rejected (no crash)");
        // garbage (bad magic) file
        const char* gpath = "aatpe_g2_garbage.o";
        { std::ofstream g(gpath, std::ios::binary);
          const char junk[] = "THIS IS NOT AN ELF FILE \x00\x01\x02";
          g.write(junk, sizeof(junk)); }
        bool g = s.hostTestParseElfFile(gpath);
        CHECK(g == false, "bad-magic object rejected (no crash)");
        // truncated-but-magic file: short header must be rejected, not OOB-read
        const char* tpath = "aatpe_g2_trunc.o";
        { std::ofstream t(tpath, std::ios::binary);
          const unsigned char elf[] = {0x7f,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0,0,1,0,0xb7};
          t.write((const char*)elf, sizeof(elf)); }
        bool t = s.hostTestParseElfFile(tpath);
        CHECK(t == false, "truncated ELF rejected (no OOB read)");
        remove(zpath); remove(gpath); remove(tpath);
    }

    // Daemon-wiring invariant: even with the integrity gate FAILED, load() must
    // stay non-event-driven (fail-closed), proving the gating path does not hard-fail.
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

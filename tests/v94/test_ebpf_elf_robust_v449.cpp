// test_ebpf_elf_robust_v449.cpp - v4.4.9 R3 red-team cluster FX4a regressions.
//
// Covers two eBPF-loader findings:
//   F-02 [P3] parseElfPrograms(): shstrtab/.strtab names were built with
//              std::string((const char*)) which walks past the declared string
//              table — and past the whole ELF `data` buffer — when the table
//              bytes are NOT NUL-terminated in bounds. A corrupt/tampered .o
//              therefore caused a heap OOB read. We now require a NUL within
//              the table and reject the object (return false) otherwise. This
//              test feeds a hand-built ELF64 whose section-name run is
//              unterminated and asserts it is rejected without crashing.
//              Compiled and run under ASan/UBSan in the gate (must be clean).
//   F-01 [P3] degrade(): dedup gate compared against the legacy
//              EbpfState::DEGRADED(6) which production never writes (it writes
//              DEGRADED_RING(9)); every main-loop iteration re-logged the WARN
//              and re-invoked stopPollThread(). We drive repeated degrade()
//              calls and assert the one-time WARN branch fires exactly once.
//
// All seams are AATPE_HOST_TEST-only; the release/NDK build compiles them out.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include "aatpe_ebpf.h"
#include "integrity.h"

// Same test-controlled integrity backing that test_ebpf_failsafe.cpp uses; we
// never actually load() here, but the TU links aatpe_ebpf.o which references
// privilegedOpsAllowed(). Default verdict = Ok so nothing refuses to link.
namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Write `bytes` to `path`; returns true on success.
static bool writeFile(const char* path, const uint8_t* bytes, size_t n) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.good()) return false;
    f.write(reinterpret_cast<const char*>(bytes), n);
    f.close();
    return f.good();
}

// Build a minimal ELF64 exercising the shstrtab name parser.
//
// Layout (little-endian ELF64):
//   [0..63)   ELF header
//   [64..127) section header [0]  (SHT_NULL, all zero)
//   [128..191) section header [1] = shstrtab. sh_name = shNameOff (controllable),
//              sh_offset = 192, sh_size = shstrSize (controllable).
//   [192..)   raw shstr bytes (length = shstrSize).
//
// shstr bytes are caller-supplied; by omitting the trailing NUL we force the
// parser to hit the in-bounds-NUL check (F-02).
static std::vector<uint8_t> buildMiniElf(uint32_t shNameOff,
                                         const std::vector<uint8_t>& shstr) {
    const size_t kHdr = 64, kShEnt = 64;
    const size_t shoff = kHdr;              // section header table right after ELF hdr
    const size_t shstrDataOff = shoff + 2 * kShEnt;  // = 192
    std::vector<uint8_t> b(shstrDataOff + shstr.size(), 0);

    // ELF header
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2;   // ELFCLASS64
    b[5] = 1;   // little-endian
    b[6] = 1;   // EV_CURRENT
    auto wr16 = [&](size_t o, uint16_t v){ memcpy(b.data()+o, &v, 2); };
    auto wr32 = [&](size_t o, uint32_t v){ memcpy(b.data()+o, &v, 4); };
    auto wr64 = [&](size_t o, uint64_t v){ memcpy(b.data()+o, &v, 8); };
    wr64(40, (uint64_t)shoff);   // e_shoff
    wr16(58, (uint16_t)kShEnt); // e_shentsize
    wr16(60, 2);                // e_shnum
    wr16(62, 1);                // e_shstrndx

    // Section header [1] = shstrtab.
    size_t s1 = shoff + kShEnt;
    wr32(s1 + 0, shNameOff);          // sh_name
    wr32(s1 + 4, 3);                  // sh_type = SHT_STRTAB
    wr64(s1 + 24, (uint64_t)shstrDataOff);  // sh_offset
    wr64(s1 + 32, (uint64_t)shstr.size()); // sh_size

    // Raw shstr bytes.
    memcpy(b.data() + shstrDataOff, shstr.data(), shstr.size());
    return b;
}

static void test_f02_unterminated_shstr_rejected(void) {
    printf("--- F-02: unterminated shstrtab name must be rejected (no OOB) ---\n");
    using namespace aatpe_ebpf;
    EbpfScheduler sched;

    // shstr = "\x00tracepo" : first byte NUL (offset 0 = empty name), then the
    // 7 bytes "tracepo" with NO trailing NUL. sh_name=1 points at "tracepo"
    // whose run hits the end of the 8-byte table with no NUL in bounds.
    std::vector<uint8_t> shstr = {0x00, 't','r','a','c','e','p','o'};
    auto elf = buildMiniElf(/*shNameOff=*/1, shstr);
    const char* path = "./v449_f02_unterminated.o";
    CHECK(writeFile(path, elf.data(), elf.size()), "wrote corrupt-ELF fixture");

    bool ok = sched.hostTestParseElfFile(path);
    CHECK(ok == false, "unterminated shstrtab name -> parseElfPrograms rejects (false)");

    // Positive control: same shape but WITH a trailing NUL. Must also return
    // false (no PROGBITS programs) but crucially must NOT OOB-read either — under
    // ASan this proves the bounded read stays inside the buffer.
    std::vector<uint8_t> shstrTerm = {0x00, 't','r','a','c','e','p','o', 0x00};
    auto elf2 = buildMiniElf(1, shstrTerm);
    const char* path2 = "./v449_f02_terminated.o";
    CHECK(writeFile(path2, elf2.data(), elf2.size()), "wrote terminated-ELF fixture");
    bool ok2 = sched.hostTestParseElfFile(path2);
    CHECK(ok2 == false, "terminated shstrtab parses without OOB (no programs -> false)");

    // Out-of-bounds sh_name (>= table size) must be tolerated (empty section
    // name), not crash.
    std::vector<uint8_t> shstr3 = {0x00, 'a', 0x00};
    auto elf3 = buildMiniElf(/*shNameOff=*/99, shstr3);  // 99 >= size 3
    const char* path3 = "./v449_f02_oobname.o";
    CHECK(writeFile(path3, elf3.data(), elf3.size()), "wrote oob-shname fixture");
    bool ok3 = sched.hostTestParseElfFile(path3);
    CHECK(ok3 == false, "sh_name>=table size tolerated (section skipped, no OOB)");

    sched.g4TestStopPollThread();
}

static void test_f01_degrade_dedup_single_warn(void) {
    printf("--- F-01: repeated degrade() -> exactly one WARN transition ---\n");
    using namespace aatpe_ebpf;
    EbpfScheduler sched;
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);

    // Start from an attached/event-driven state (production post-attach shape).
    sched.g4TestEnterAttached();
    CHECK(sched.g4StateIsActive() || sched.g4StateIsEventDriven(),
          "precondition: scheduler starts attached/active");

    // First degrade: must take the one-time WARN branch.
    sched.g4TestCallDegrade("ring-loss regression");
    CHECK(sched.g4TestDegradeWarnCount() == 1, "first degrade() fires WARN exactly once");
    CHECK(sched.g4StateIsDegradedRing() == true, "state written to DEGRADED_RING(9)");

    // Hammer it: shouldDegrade() stays true forever on cumulative counters, so
    // the main loop would call degrade() every iteration. The dedup gate must
    // keep the WARN branch at 1.
    for (int i = 0; i < 100; i++) {
        sched.g4TestCallDegrade("ring-loss regression");
    }
    CHECK(sched.g4TestDegradeWarnCount() == 1,
          "100 further degrade() calls do NOT re-log (dedup on DEGRADED_RING)");
    CHECK(sched.g4StateIsDegradedRing() == true, "state remains DEGRADED_RING (no state thrash)");

    sched.g4TestStopPollThread();
}

int main() {
    printf("=== v4.4.9 FX4a: eBPF ELF robustness + degrade dedup ===\n");
    test_f02_unterminated_shstr_rejected();
    test_f01_degrade_dedup_single_warn();
    printf("=== v449 ebpf_elf_robust: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

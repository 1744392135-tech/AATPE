#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include "aatpe_ebpf.h"
#include "integrity.h"

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

static bool writeFile(const char* path, const uint8_t* bytes, size_t n) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.good()) return false;
    f.write(reinterpret_cast<const char*>(bytes), n);
    f.close();
    return f.good();
}

static std::vector<uint8_t> buildMiniElf(uint32_t shNameOff,
                                         const std::vector<uint8_t>& shstr) {
    const size_t kHdr = 64, kShEnt = 64;
    const size_t shoff = kHdr;
    const size_t shstrDataOff = shoff + 2 * kShEnt;
    std::vector<uint8_t> b(shstrDataOff + shstr.size(), 0);

    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 2;
    b[5] = 1;
    b[6] = 1;
    auto wr16 = [&](size_t o, uint16_t v){ memcpy(b.data()+o, &v, 2); };
    auto wr32 = [&](size_t o, uint32_t v){ memcpy(b.data()+o, &v, 4); };
    auto wr64 = [&](size_t o, uint64_t v){ memcpy(b.data()+o, &v, 8); };
    wr64(40, (uint64_t)shoff);
    wr16(58, (uint16_t)kShEnt);
    wr16(60, 2);
    wr16(62, 1);

    size_t s1 = shoff + kShEnt;
    wr32(s1 + 0, shNameOff);
    wr32(s1 + 4, 3);
    wr64(s1 + 24, (uint64_t)shstrDataOff);
    wr64(s1 + 32, (uint64_t)shstr.size());

    memcpy(b.data() + shstrDataOff, shstr.data(), shstr.size());
    return b;
}

static void test_f02_unterminated_shstr_rejected(void) {
    printf("--- F-02: unterminated shstrtab name must be rejected (no OOB) ---\n");
    using namespace aatpe_ebpf;
    EbpfScheduler sched;

    std::vector<uint8_t> shstr = {0x00, 't','r','a','c','e','p','o'};
    auto elf = buildMiniElf( 1, shstr);
    const char* path = "./v449_f02_unterminated.o";
    CHECK(writeFile(path, elf.data(), elf.size()), "wrote corrupt-ELF fixture");

    bool ok = sched.hostTestParseElfFile(path);
    CHECK(ok == false, "unterminated shstrtab name -> parseElfPrograms rejects (false)");

    std::vector<uint8_t> shstrTerm = {0x00, 't','r','a','c','e','p','o', 0x00};
    auto elf2 = buildMiniElf(1, shstrTerm);
    const char* path2 = "./v449_f02_terminated.o";
    CHECK(writeFile(path2, elf2.data(), elf2.size()), "wrote terminated-ELF fixture");
    bool ok2 = sched.hostTestParseElfFile(path2);
    CHECK(ok2 == false, "terminated shstrtab parses without OOB (no programs -> false)");

    std::vector<uint8_t> shstr3 = {0x00, 'a', 0x00};
    auto elf3 = buildMiniElf( 99, shstr3);
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

    sched.g4TestEnterAttached();
    CHECK(sched.g4StateIsActive() || sched.g4StateIsEventDriven(),
          "precondition: scheduler starts attached/active");

    sched.g4TestCallDegrade("ring-loss regression");
    CHECK(sched.g4TestDegradeWarnCount() == 1, "first degrade() fires WARN exactly once");
    CHECK(sched.g4StateIsDegradedRing() == true, "state written to DEGRADED_RING(9)");

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

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
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
} while (0)

static void test_ebpf13_buffer_margin(void) {
    printf("[EBPF-13] PERCPU_ARRAY buffer slot margin\n");
    CHECK(EbpfScheduler::hostTestEmbeddedCpuBufferSlots(8) == 16,
          "slots(8) == 16 (old formula gave 8, margin claim was false)");
    CHECK(EbpfScheduler::hostTestEmbeddedCpuBufferSlots(9) == 24,
          "slots(9) == 24 (round-up 16 + explicit 8 margin)");
    CHECK(EbpfScheduler::hostTestEmbeddedCpuBufferSlots(1) == 16,
          "slots(1) == 16");
    CHECK(EbpfScheduler::hostTestEmbeddedCpuBufferSlots(0) >= 8,
          "slots(0) clamped to a sane minimum");
    bool invariantOk = true;
    for (long p = 1; p <= 128; p++) {
        int s = EbpfScheduler::hostTestEmbeddedCpuBufferSlots(p);
        if (s < p + 8) { invariantOk = false; break; }
    }
    CHECK(invariantOk, "invariant slots(p) >= p + 8 for p in 1..128");
}

static void test_ebpf16_warn_flag_reset(void) {
    printf("[EBPF-16] offset-filter WARN flags reset across unload()\n");
    EbpfScheduler s;
    ForkEvent ev{};
    ev.hdr.type = (uint32_t)EventType::FORK;

    s.hostTestSetOffsetDegraded('f');
    CHECK(s.hostTestOffsetWarnFired('f') == false, "precondition: flag starts clear");
    s.hostTestDeliverFork(ev);
    CHECK(s.hostTestOffsetWarnFired('f') == true,
          "first filtered fork event flips the one-shot WARN flag");
    bool wasSet = s.hostTestOffsetWarnFired('f');
    s.hostTestDeliverFork(ev);
    CHECK(s.hostTestOffsetWarnFired('f') == wasSet, "flag stays set after repeat drop");

    s.unload();
    CHECK(s.hostTestOffsetWarnFired('f') == false,
          "unload() resets the WARN flag for the next load epoch");

    s.hostTestSetOffsetDegraded('f');
    s.hostTestDeliverFork(ev);
    CHECK(s.hostTestOffsetWarnFired('f') == true,
          "WARN fires again after reload (was permanently silent before fix)");
    s.unload();
}

static bool writeSyntheticElf(const char* path, size_t progBytes,
                              bool withPseudoMapRefs) {
    std::vector<uint8_t> shstr;
    shstr.push_back(0);
    const char* secName = "tracepoint/sched/sched_process_fork";
    size_t nameOff = shstr.size();
    shstr.insert(shstr.end(), secName, secName + std::strlen(secName) + 1);
    size_t shstrtabNameOff = shstr.size();
    const char* shn = ".shstrtab";
    shstr.insert(shstr.end(), shn, shn + std::strlen(shn) + 1);

    const uint64_t shstrFileOff = 176;
    const uint64_t progFileOff = 128;
    const uint64_t shoff = 256;

    size_t imageSize = (size_t)std::max<uint64_t>(
        std::max(shoff + 3 * 64, shstrFileOff + shstr.size()),
        progFileOff + progBytes);
    std::vector<uint8_t> f(imageSize, 0);

    f[0] = 0x7f; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2;
    f[5] = 1;
    f[6] = 1;
    auto wr16 = [&](size_t off, uint16_t v) { std::memcpy(&f[off], &v, 2); };
    auto wr32 = [&](size_t off, uint32_t v) { std::memcpy(&f[off], &v, 4); };
    auto wr64 = [&](size_t off, uint64_t v) { std::memcpy(&f[off], &v, 8); };
    wr16(16, 1);
    wr16(18, 247);
    wr32(20, 1);
    wr64(40, shoff);
    wr16(52, 64);
    wr16(58, 64);
    wr16(60, 3);
    wr16(62, 2);

    if (progFileOff + progBytes > f.size()) return false;
    if (withPseudoMapRefs) {
        for (size_t off = 0; off + 16 <= progBytes; off += 16) {
            f[progFileOff + off] = 0x18;
            f[progFileOff + off + 1] = 0x10;
        }
    } else {
        for (size_t off = 0; off + 8 <= progBytes; off += 8) {
            f[progFileOff + off] = 0xb7;
            f[progFileOff + off + 1] = 0x00;
        }
    }

    std::memcpy(f.data() + shstrFileOff, shstr.data(), shstr.size());

    auto writeShdr = [&](int idx, uint32_t name, uint32_t type, uint64_t off,
                         uint64_t size) {
        size_t base = shoff + (size_t)idx * 64;
        wr32(base + 0, name);
        wr32(base + 4, type);
        wr64(base + 24, off);
        wr64(base + 32, size);
    };
    writeShdr(0, 0, 0, 0, 0);
    writeShdr(1, (uint32_t)nameOff, 1, progFileOff, progBytes);
    writeShdr(2, (uint32_t)shstrtabNameOff, 3, shstrFileOff, shstr.size());

    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t w = ::write(fd, f.data(), f.size());
    ::close(fd);
    return w == (ssize_t)f.size();
}

static void test_ebpf20_mapindices_fallback(void) {
    printf("[EBPF-20] parseElfPrograms no-.rel fallback sized by actual refs\n");
    EbpfScheduler s;
    const char* p3 = "/tmp/aatpe_test_ebpf20_3refs.o";
    const char* p0 = "/tmp/aatpe_test_ebpf20_0refs.o";

    CHECK(writeSyntheticElf(p3, 48, true), "fixture: write .o with 3 map refs");
    std::vector<std::vector<int>> mi;
    bool ok = s.hostTestParseElfMapIndices(p3, mi);
    CHECK(ok && mi.size() == 1, "parseElfPrograms accepted the synthetic .o");
    if (ok && !mi.empty()) {
        CHECK(mi[0].size() == 3,
              "fallback produced exactly 3 mapIndices (old hardcode always made 8)");
        bool seq = (mi[0].size() == 3 && mi[0][0] == 0 && mi[0][1] == 1 && mi[0][2] == 2);
        CHECK(seq, "fallback indices are sequential 0,1,2");
    }

    CHECK(writeSyntheticElf(p0, 16, false), "fixture: write .o with 0 map refs");
    mi.clear();
    ok = s.hostTestParseElfMapIndices(p0, mi);
    CHECK(ok && mi.size() == 1, "parseElfPrograms accepted the no-ref .o");
    if (ok && !mi.empty()) {
        CHECK(mi[0].empty(), "0 map refs -> empty mapIndices (old hardcode made 8)");
    }
    ::unlink(p3);
    ::unlink(p0);
}

int main(void) {
    printf("=== EBPF P3 fixes host tests (EBPF-13/16/20) ===\n");
    test_ebpf13_buffer_margin();
    test_ebpf16_warn_flag_reset();
    test_ebpf20_mapindices_fallback();
    printf("=== RESULT: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

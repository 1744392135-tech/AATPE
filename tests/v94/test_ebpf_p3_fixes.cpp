// test_ebpf_p3_fixes.cpp — host regression tests for the EBPF-13..EBPF-20 P3
// fixes in aatpe_ebpf.cpp / aatpe_ebpf.h (逐行审查 round, 2026-10).
//
// Covers what CAN be verified on the host without a real bpf/tracefs device:
//   EBPF-13  readEmbeddedCpuCounts PERCPU_ARRAY buffer slot sizing (real helper)
//   EBPF-16  one-shot offset-filter WARN flags are reset by unload() so the next
//            load epoch WARN fires again (real deliverForkEvent gate + unload)
//   EBPF-20  parseElfPrograms no-.rel fallback sizes mapIndices by the actual
//            PSEUDO_MAP_FD instruction count (real parseElfPrograms on synthetic
//            ELF .o files written to /tmp)
//
// EBPF-14 (STATE_DIR), EBPF-15 (dead stub removal), EBPF-17 (mutex kept with
// evidence), EBPF-18 (online-CPU kprobe loop) and EBPF-19 (sizeof-first attach
// strategy) are compile-verified here (they link the real production TU) and
// documented in the review report; EBPF-18's runtime needs a real bpftool +
// kprobe environment (DEGRADED on host).
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

// Host integrity gate shim (identical to test_ebpf_event_parse.cpp): production
// aatpe_ebpf.cpp calls aatpe::integrity::privilegedOpsAllowed(); default = Ok.
namespace aatpe { namespace integrity {
static int g_testVerdict = 1;   // 1 == Ok
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

// ---------------------------------------------------------------------------
// EBPF-13: buffer slot sizing must be >= possibleCpus + 8 (explicit margin).
// ---------------------------------------------------------------------------
static void test_ebpf13_buffer_margin(void) {
    printf("[EBPF-13] PERCPU_ARRAY buffer slot margin\n");
    // Old formula ((p+15)/8)*8 gave 8 for p=8 — NO margin at all.
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

// ---------------------------------------------------------------------------
// EBPF-16: firstDrop WARN flags reset by unload() (load-epoch lifecycle).
// ---------------------------------------------------------------------------
static void test_ebpf16_warn_flag_reset(void) {
    printf("[EBPF-16] offset-filter WARN flags reset across unload()\n");
    EbpfScheduler s;
    ForkEvent ev{};
    ev.hdr.type = (uint32_t)EventType::FORK;

    // Force fork offsets DEGRADED -> deliverForkEvent() filters and flips flag.
    s.hostTestSetOffsetDegraded('f');
    CHECK(s.hostTestOffsetWarnFired('f') == false, "precondition: flag starts clear");
    s.hostTestDeliverFork(ev);
    CHECK(s.hostTestOffsetWarnFired('f') == true,
          "first filtered fork event flips the one-shot WARN flag");
    // A second filtered delivery must NOT re-warn (flag stays set) — proves the
    // flag is actually one-shot, not per-call.
    bool wasSet = s.hostTestOffsetWarnFired('f');
    s.hostTestDeliverFork(ev);
    CHECK(s.hostTestOffsetWarnFired('f') == wasSet, "flag stays set after repeat drop");

    // unload() must reset it (hotplug rebuild / config reload epoch boundary).
    s.unload();
    CHECK(s.hostTestOffsetWarnFired('f') == false,
          "unload() resets the WARN flag for the next load epoch");

    // After reload-epoch simulation the WARN must fire again.
    s.hostTestSetOffsetDegraded('f');
    s.hostTestDeliverFork(ev);
    CHECK(s.hostTestOffsetWarnFired('f') == true,
          "WARN fires again after reload (was permanently silent before fix)");
    s.unload();  // clean teardown
}

// ---------------------------------------------------------------------------
// EBPF-20: build a synthetic ELF64 .o in /tmp and verify parseElfPrograms'
// no-.rel fallback sizes mapIndices to the ACTUAL pseudo-map-fd count.
//
// Layout (all little-endian):
//   [0,64)   ELF64 header, e_shoff=256, e_shnum=3, e_shentsize=64, e_shstrndx=2
//   [128,..) prog bytes: `progSize` bytes of BPF instructions
//   [176,..) shstrtab: "\0tracepoint/sched/sched_process_fork\0.shstrtab\0"
//   [256,..) 3 section headers (64B each): #0 NULL, #1 PROGBITS prog, #2 shstrtab
// ---------------------------------------------------------------------------
static bool writeSyntheticElf(const char* path, size_t progBytes,
                              bool withPseudoMapRefs) {
    std::vector<uint8_t> shstr;
    shstr.push_back(0);                                              // [0] ""
    const char* secName = "tracepoint/sched/sched_process_fork";     // [1]
    size_t nameOff = shstr.size();
    shstr.insert(shstr.end(), secName, secName + std::strlen(secName) + 1);
    size_t shstrtabNameOff = shstr.size();                           // ".shstrtab"
    const char* shn = ".shstrtab";
    shstr.insert(shstr.end(), shn, shn + std::strlen(shn) + 1);

    const uint64_t shstrFileOff = 176;
    const uint64_t progFileOff = 128;
    const uint64_t shoff = 256;

    // Final image spans all three extents; allocate the whole image once and
    // never shrink it (an earlier resize() call truncated the section headers).
    size_t imageSize = (size_t)std::max<uint64_t>(
        std::max(shoff + 3 * 64, shstrFileOff + shstr.size()),
        progFileOff + progBytes);
    std::vector<uint8_t> f(imageSize, 0);

    // --- ELF64 header ---
    f[0] = 0x7f; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2;  // ELFCLASS64
    f[5] = 1;  // 2's-complement, little endian
    f[6] = 1;  // EV_CURRENT
    auto wr16 = [&](size_t off, uint16_t v) { std::memcpy(&f[off], &v, 2); };
    auto wr32 = [&](size_t off, uint32_t v) { std::memcpy(&f[off], &v, 4); };
    auto wr64 = [&](size_t off, uint64_t v) { std::memcpy(&f[off], &v, 8); };
    wr16(16, 1);    // e_type = REL
    wr16(18, 247);  // e_machine = BPF
    wr32(20, 1);    // e_version
    wr64(40, shoff);
    wr16(52, 64);   // e_ehsize
    wr16(58, 64);   // e_shentsize
    wr16(60, 3);    // e_shnum
    wr16(62, 2);    // e_shstrndx

    // --- prog bytes ---
    if (progFileOff + progBytes > f.size()) return false;
    if (withPseudoMapRefs) {
        // Each PSEUDO_MAP_FD = 16 bytes: low insn opcode BPF_LD(0x18), src_reg=1.
        for (size_t off = 0; off + 16 <= progBytes; off += 16) {
            f[progFileOff + off] = 0x18;           // BPF_LD | BPF_IMM
            f[progFileOff + off + 1] = 0x10;       // src_reg==1, dst_reg==0
        }
    } else {
        // No map refs: BPF_ALU64_ADDimm (0xb7), src_reg==0.
        for (size_t off = 0; off + 8 <= progBytes; off += 8) {
            f[progFileOff + off] = 0xb7;
            f[progFileOff + off + 1] = 0x00;
        }
    }

    // --- shstrtab ---
    std::memcpy(f.data() + shstrFileOff, shstr.data(), shstr.size());

    // --- section headers ---
    auto writeShdr = [&](int idx, uint32_t name, uint32_t type, uint64_t off,
                         uint64_t size) {
        size_t base = shoff + (size_t)idx * 64;
        wr32(base + 0, name);
        wr32(base + 4, type);
        // sh_flags(8) sh_addr(8) left zero
        wr64(base + 24, off);
        wr64(base + 32, size);
        // sh_link(4) sh_info(4) sh_addralign(8) sh_entsize(8) left zero
    };
    writeShdr(0, 0, 0, 0, 0);                                    // SHN_UNDEF
    writeShdr(1, (uint32_t)nameOff, 1, progFileOff, progBytes);  // PROGBITS prog
    writeShdr(2, (uint32_t)shstrtabNameOff, 3, shstrFileOff, shstr.size()); // STRTAB

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

    // 3 pseudo-map-fd references -> 48 prog bytes.
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

    // 0 map references -> empty fallback list (old hardcode still made 8 bogus
    // entries, which could mask load errors).
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

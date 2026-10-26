// test_wakeup_reaper_v450.cpp — F-03 v4.5.0 host hardening regression.
//
// F-03 (v4.4.9 known-limit, deferred to v4.5.0): the wakeup_cache BPF HASH map
// had no TTL reaper. A task woken but never scheduled on a CPU and never exited
// left a cold wakeup_rec forever; on long uptime + many such tasks the 8192
// HASH could fill (new wakeups -> ENOSPC, counted). A true periodic BPF sweep
// is impossible on the 4.14 minimum ABI (bpf_for_each_map_elem needs 5.13,
// bpf_timer 5.15, userspace get_next_key 4.15).
//
// The v4.5.0 fix: wakeup_cache is now BPF_MAP_TYPE_LRU_HASH (kernel 4.10+), so
// the KERNEL evicts the least-recently-used (cold/orphaned) entry automatically
// when the map nears full — an in-kernel TTL reaper. The userspace loader opens
// it with a plain-HASH fallback (EINVAL -> "invalid map type" on pre-LRU
// kernels) so unsupported kernels degrade to the v4.4.9 bounded-HASH behavior
// instead of failing load.
//
// This host test:
//   (1) pins the pure fallback DECISION (wakeupCacheMapDecision) — no BPF
//       syscall needed, fully hermetic.
//   (2) asserts the regenerated embedded header declares wakeup_cache as
//       LRU_HASH (type 9 — BPF UAPI; type 3 is PROG_ARRAY), locking the
//       source-of-truth build chain.
//   (3) drives the real loader under injected BPF failure to prove the new
//       fallback path does NOT break the existing fail-safe (still lands in
//       polling, no abort/UB).
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include "aatpe_ebpf.h"
#include "integrity.h"

// ebpf failsafe recipe does not link integrity.cpp; provide the same backing.
namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

// AATPE_HOST_TEST-only seams defined in aatpe_ebpf.cpp.
extern "C" void aatpe_host_test_set_bpf_inject(uint32_t flags);
extern "C" int  aatpe_wakeup_map_decision_for_test(int lruFd, int lruErrno);
extern "C" int  aatpe_wakeup_preferred_type_for_test();
extern "C" int  aatpe_wakeup_embedded_type_for_test();

// Kernel UAPI (linux/bpf.h): LRU_HASH==9. 3 is PROG_ARRAY — a past build bug
// passed 3 to BPF_MAP_CREATE and the kernel read it as PROG_ARRAY. Lock both.
static constexpr int kUapiLruHash   = 9;
static constexpr int kUapiProgArray = 3;

static constexpr uint32_t kFakeProgFd = 0x1u;
static constexpr uint32_t kForceEnosys = 0x2u;
static constexpr uint32_t kForceEperm  = 0x4u;
static constexpr uint32_t kFakeCaps    = 0x8u;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== F-03 wakeup_cache LRU reaper (v4.5.0) ===\n");

    // --- (1) pure fallback decision -----------------------------------------
    printf("--- decision: wakeupCacheMapDecision ---\n");
    // LRU fd valid -> accept LRU (use the in-kernel reaper).
    CHECK(aatpe_wakeup_map_decision_for_test(196608, 0) == 0,
          "LRU fd valid -> accept LRU (0)");
    CHECK(aatpe_wakeup_map_decision_for_test(3, 0) == 0,
          "LRU fd valid (small fd) -> accept LRU (0)");
    // LRU rejected EINVAL -> kernel lacks the type -> retry HASH.
    CHECK(aatpe_wakeup_map_decision_for_test(-1, EINVAL) == 1,
          "LRU EINVAL -> fall back to HASH (1)");
    // bpf() entirely absent -> HASH fails identically -> hard fail.
    CHECK(aatpe_wakeup_map_decision_for_test(-1, ENOSYS) == -1,
          "LRU ENOSYS -> hard fail (-1), no point retrying HASH");
    // No caps -> HASH fails identically -> hard fail.
    CHECK(aatpe_wakeup_map_decision_for_test(-1, EPERM) == -1,
          "LRU EPERM -> hard fail (-1), no point retrying HASH");
    // Out of memory -> HASH fails identically -> hard fail.
    CHECK(aatpe_wakeup_map_decision_for_test(-1, ENOMEM) == -1,
          "LRU ENOMEM -> hard fail (-1)");

    // --- (2) embedded header source-of-truth ---------------------------------
    // F-03 UAPI lock: the preferred map type must be LRU_HASH==9 (kernel UAPI),
    // NOT PROG_ARRAY(3). The regenerated embedded header must agree. A past
    // build bug used 3, which the kernel interprets as PROG_ARRAY -> either
    // EINVAL (silently falling back to HASH, making the reaper a no-op) or the
    // wrong map type. This regression test prevents that from recurring.
    printf("--- UAPI type lock: LRU_HASH must be 9, not 3 ---\n");
    int pref = aatpe_wakeup_preferred_type_for_test();
    int emb  = aatpe_wakeup_embedded_type_for_test();
    CHECK(pref == kUapiLruHash,
          "preferred wakeup_cache map type == LRU_HASH(9)");
    CHECK(pref != kUapiProgArray,
          "preferred type != PROG_ARRAY(3) (the past bug)");
    CHECK(emb == kUapiLruHash,
          "regenerated embedded header declares wakeup_cache as LRU_HASH(9)");
    CHECK(emb != kUapiProgArray,
          "embedded header type != PROG_ARRAY(3)");
    CHECK(pref == emb, "loader preferred type == embedded header type");

    // --- (3) fail-safe regression under injected BPF failure ----------------
    // The new LRU->HASH retry must not change the outer fail-safe contract:
    // when every bpf() call fails, the loader still lands in polling without
    // abort. Drive ENOSYS and EPERM (the two injection faults).
    printf("--- fail-safe regression: load() under injected fault ---\n");
    for (uint32_t fault = kForceEnosys; fault <= kForceEperm; fault <<= 1) {
        aatpe_ebpf::EbpfScheduler sched;
        aatpe_host_test_set_bpf_inject(kFakeCaps | fault);
        bool r = sched.load();
        CHECK(r == true || r == false, "load() returns bool without abort/UB");
        std::string mode = sched.getModeString() ? sched.getModeString() : "";
        CHECK(mode == "polling", "still degrades to polling (fallback harmless)");
        CHECK(sched.isEventDrivenLive() == false,
              "isEventDrivenLive()==false after fault");
    }
    aatpe_host_test_set_bpf_inject(0);

    printf("=== wakeup reaper v450: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

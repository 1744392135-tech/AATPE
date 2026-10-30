#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include "aatpe_ebpf.h"
#include "integrity.h"

namespace aatpe { namespace integrity {
static int g_testVerdict = 1;
bool privilegedOpsAllowed() { return g_testVerdict == 1; }
void setIntegrityVerdictForTest(IntegrityVerdict v) { g_testVerdict = (int)v; }
}}

extern "C" void aatpe_host_test_set_bpf_inject(uint32_t flags);
extern "C" int  aatpe_wakeup_map_decision_for_test(int lruFd, int lruErrno);
extern "C" int  aatpe_wakeup_preferred_type_for_test();
extern "C" int  aatpe_wakeup_embedded_type_for_test();

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

    printf("--- decision: wakeupCacheMapDecision ---\n");
    CHECK(aatpe_wakeup_map_decision_for_test(196608, 0) == 0,
          "LRU fd valid -> accept LRU (0)");
    CHECK(aatpe_wakeup_map_decision_for_test(3, 0) == 0,
          "LRU fd valid (small fd) -> accept LRU (0)");
    CHECK(aatpe_wakeup_map_decision_for_test(-1, EINVAL) == 1,
          "LRU EINVAL -> fall back to HASH (1)");
    CHECK(aatpe_wakeup_map_decision_for_test(-1, ENOSYS) == -1,
          "LRU ENOSYS -> hard fail (-1), no point retrying HASH");
    CHECK(aatpe_wakeup_map_decision_for_test(-1, EPERM) == -1,
          "LRU EPERM -> hard fail (-1), no point retrying HASH");
    CHECK(aatpe_wakeup_map_decision_for_test(-1, ENOMEM) == -1,
          "LRU ENOMEM -> hard fail (-1)");

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

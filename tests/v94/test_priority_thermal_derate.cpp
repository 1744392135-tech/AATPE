#include <cstdio>
#include <cstdint>
#include <atomic>
#include <string>

#include "aatped_priority.cpp"

std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}

const char* STATE_DIR = "/tmp/aatpe_derate_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFileAtomic(const char*, const std::string&) { return true; }

std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};

namespace aatpe_mech {
static unsigned g_priorityFloor = 0;
int aatpeUclampFloorSet(int, int src, unsigned int utilMin) {
    if (src == 2 ) g_priorityFloor = utilMin;
    return 0;
}
void aatpeUclampFloorClear(int, int) {}
unsigned int aatpeUclampFloorGet(int, int src) {
    return src == 2 ? g_priorityFloor : 0u;
}
}

namespace aatpe_recovery {
bool recoveryGet(int, RecoveryRecord&) { return false; }
void recoveryErase(int) {}
bool recoveryRecordSched(int, int, unsigned long long, uint32_t, uint32_t, int,
                         uint32_t, uint32_t, uint32_t, uint32_t, int, uint32_t, uint32_t) {
    return true;
}
bool recoveryRefreshAppliedSched(int, uint32_t, uint32_t, int, uint32_t, uint32_t) {
    return true;
}
}

namespace aatpe { namespace subsys { namespace safemode {
bool rtBoostAllowed() { return true; }
}}}

static int g_pass = 0, g_fail = 0;
using namespace aatpe_priority;
#define CHECK_SKIP(cur, tgt, last, expectSkip, msg) do {                 \
    int got = aatpeRtUclampRewriteSkippableForTest((unsigned)(cur),        \
                                                   (unsigned)(tgt),       \
                                                   (unsigned)(last));     \
    if (got == (expectSkip)) { g_pass++; printf("  [PASS] %s\n", msg); }   \
    else { g_fail++; printf("  [FAIL] %s (got=%d want=%d)\n",             \
                            msg, got, (int)(expectSkip)); }               \
} while (0)

int main() {
    printf("=== MAJOR-2: thermal soft-derate uclamp idempotency ===\n");

    unsigned long derated = (unsigned long)((uint32_t)(1024u * 0.6f));
    printf("  (1024 * 0.6f) = %lu (expect 614)\n", derated);
    if (derated == 614) { g_pass++; } else { g_fail++; printf("  [FAIL] derated floor != 614\n"); }

    CHECK_SKIP(0, 1024, 0, 0, "first boost (cur=0,tgt=1024,last=0) -> MUST write up");

    CHECK_SKIP(1024, 1024, 1024, 1, "steady at 1024 (cur=tgt=last=1024) -> skip");

    CHECK_SKIP(1024, 614, 1024, 0,
               "thermal derate 1024->614 on boosted thread -> MUST write down (was: wrongly skipped)");

    CHECK_SKIP(614, 1024, 614, 0, "recovery 614->1024 -> MUST write up");

    CHECK_SKIP(1024, 1024, 1024, 1, "post-recovery steady 1024 -> skip");

    CHECK_SKIP(1024, 512, 512, 1,
               "spillover raised floor, priority target=512 -> skip (do NOT stomp spillover)");

    CHECK_SKIP(1024, 0, 0, 1, "target=0 (no role floor) -> skip");

    CHECK_SKIP(800, 1024, 0, 0, "cur=800 below target=1024, never boosted -> MUST write up");

    printf("\n========================================\n"
           "thermal derate results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

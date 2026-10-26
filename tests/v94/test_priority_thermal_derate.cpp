// test_priority_thermal_derate.cpp — v4.4.8 MAJOR-2 host regression.
//
// Bug (RCV M1, confirmed): when THERMAL_SMOOTH is active and g_thermalPlacementScale
// drops below 1.0 on an ALREADY-BOOSTED thread, roleToUclampMin() lowers the desired
// uclamp_min (e.g. RENDER 1024 -> (uint32_t)(1024*0.6f) = 614). But the old
// idempotency test was `cur.sched_util_min >= targetUclampMin`; the kernel still
// reads 1024 (written at boost time), 1024 >= 614 -> "satisfied" -> step2 skipped,
// so the thermal derate never landed on existing game threads (the exact population
// the feature exists to protect).
//
// Fix: uclampRewriteSkippable(curMin, targetMin, lastPriorityMin) now forces a
// write-down whenever priority's OWN desired floor dropped below what it last wrote
// (targetMin < lastPriorityMin), while preserving the steady-state "kernel already
// higher, don't stomp another source" behaviour.
//
// This test drives that pure decision directly (whole-TU include like rt_restore)
// and pins every transition in the RCV trace table. The end-to-end sched_setattr
// observation on a real tid is left for C2 (TSan/integration); the decision logic
// is what was wrong and is fully determined here.
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <string>

#include "aatped_priority.cpp"

// ---- stubs for the externals aatped_priority.cpp references (see rt_restore) --
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
// Minimal ledger: track the PRIORITY source floor so the production call to
// aatpeUclampFloorGet() has a real implementation; we do not invoke the full
// aatpeSetThreadPriority() here, but the TU must link.
static unsigned g_priorityFloor = 0;
int aatpeUclampFloorSet(int, int src, unsigned int utilMin) {
    if (src == 2 /*AATPE_UCLAMP_PRIORITY*/) g_priorityFloor = utilMin;
    return 0;
}
void aatpeUclampFloorClear(int, int) {}
unsigned int aatpeUclampFloorGet(int, int src) {
    return src == 2 ? g_priorityFloor : 0u;
}
}  // namespace aatpe_mech

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
}  // namespace aatpe_recovery

namespace aatpe { namespace subsys { namespace safemode {
bool rtBoostAllowed() { return true; }
}}}

static int g_pass = 0, g_fail = 0;
// skippable==1 means "no re-write needed"; skippable==0 means "MUST write".
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

    // The RCV number: RENDER base 1024 * scale 0.6 -> 614 (integer truncation).
    unsigned long derated = (unsigned long)((uint32_t)(1024u * 0.6f));
    printf("  (1024 * 0.6f) = %lu (expect 614)\n", derated);
    if (derated == 614) { g_pass++; } else { g_fail++; printf("  [FAIL] derated floor != 614\n"); }

    // A. First boost at scale=1.0: kernel cur=0, target=1024, we never wrote -> MUST write.
    CHECK_SKIP(0, 1024, 0, 0, "first boost (cur=0,tgt=1024,last=0) -> MUST write up");

    // B. Steady at target: kernel=1024, target=1024, last=1024 -> skip (no storm).
    CHECK_SKIP(1024, 1024, 1024, 1, "steady at 1024 (cur=tgt=last=1024) -> skip");

    // C. *** THE BUG *** thermal derate scale=0.6 AFTER a prior boost:
    //    kernel still reads 1024, target dropped to 614, we last wrote 1024.
    //    Old code: 1024>=614 -> skip (DERATE LOST). New code -> MUST write down to 614.
    CHECK_SKIP(1024, 614, 1024, 0,
               "thermal derate 1024->614 on boosted thread -> MUST write down (was: wrongly skipped)");

    // D. Thermal recovery: kernel=614, target back to 1024, last=614 -> MUST write up.
    CHECK_SKIP(614, 1024, 614, 0, "recovery 614->1024 -> MUST write up");

    // E. After recovery steady: kernel=1024, target=1024, last=1024 -> skip.
    CHECK_SKIP(1024, 1024, 1024, 1, "post-recovery steady 1024 -> skip");

    // F. Other source (spillover) raised the kernel floor to 1024 while this
    //    priority source only targets UI=512 and last wrote 512. We must NOT
    //    stomp spillover: last==target so iMustLower is false -> skip. This pins
    //    that the fix does not regress the "don't fight another source" case.
    CHECK_SKIP(1024, 512, 512, 1,
               "spillover raised floor, priority target=512 -> skip (do NOT stomp spillover)");

    // G. target=0 (role has no floor) -> always satisfied/skip.
    CHECK_SKIP(1024, 0, 0, 1, "target=0 (no role floor) -> skip");

    // H. Never boosted (last=0) but kernel reads 800 below target 1024 -> write up.
    CHECK_SKIP(800, 1024, 0, 0, "cur=800 below target=1024, never boosted -> MUST write up");

    printf("\n========================================\n"
           "thermal derate results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

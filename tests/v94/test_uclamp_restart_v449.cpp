// test_uclamp_restart_v449.cpp — v4.4.9 R1-2 host regression.
//
// Bug: after a daemon restart (no reboot), the per-tid uclamp ledger g_floors()
// is empty (lastPriorityMin=0) but the kernel still carries the old instance's
// boost on SCHED_FIFO threads. When thermal soft-derate activates (target=614)
// on a stale-boosted thread (kernel=1024), the old uclampRewriteSkippable
// computed kernelHighEnough=true && !iMustLower=true -> skipped the write-down,
// leaving the thread's floor stuck at 1024 forever. Thermal protection was
// silently bypassed for every thread boosted by the previous daemon instance.
//
// Fix: when lastPriorityMin==0 (no ledger entry) AND curMin > targetMin (kernel
// reads higher than our target), we MUST write down — we cannot prove we
// previously wrote the higher value, so we correct any stale boost.
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <string>

#include "aatped_priority.cpp"

// ---- stubs for externals (same pattern as test_priority_thermal_derate) ----
std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}

const char* STATE_DIR = "/tmp/aatpe_uclamp_restart_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFileAtomic(const char*, const std::string&) { return true; }

std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};

namespace aatpe_mech {
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
    printf("=== R1-2: uclamp derate after daemon restart (ledger empty) ===\n");

    // THE BUG: daemon restart (no reboot). Old daemon left kernel sched_util_min=1024
    // on a SCHED_FIFO game thread. New daemon's ledger is empty (last=0).
    // Thermal derate activates: target drops to 614.
    // Old code: skippable=true -> write-down silently skipped -> kernel stays 1024.
    // New code: skippable=false -> MUST write down to 614.
    CHECK_SKIP(1024, 614, 0, 0,
               "RESTART derate 1024->614, ledger empty (last=0) -> MUST write down (was: wrongly skipped)");

    // Steady state after restart: kernel=1024, target=1024, ledger empty.
    // No change needed -> skip (idempotent, no syscall storm).
    CHECK_SKIP(1024, 1024, 0, 1,
               "RESTART steady at 1024 (cur=tgt=1024, last=0) -> skip");

    // Boost-up after restart: kernel=0, target=1024, ledger empty.
    // Need to write up -> MUST write.
    CHECK_SKIP(0, 1024, 0, 0,
               "RESTART boost-up 0->1024, ledger empty -> MUST write up");

    // Partial boost after restart: kernel=512, target=1024, ledger empty.
    // Kernel below target -> MUST write up.
    CHECK_SKIP(512, 1024, 0, 0,
               "RESTART partial boost 512->1024 -> MUST write up");

    // Same target after restart: kernel=614, target=614, ledger empty.
    // Already at target -> skip.
    CHECK_SKIP(614, 614, 0, 1,
               "RESTART at target 614 (cur=tgt=614, last=0) -> skip");

    // --- Regression: same-session derate still works (M1 fix preserved) ---
    CHECK_SKIP(1024, 614, 1024, 0,
               "same-session derate 1024->614 (last=1024) -> MUST write down (M1 preserved)");

    // --- Regression: don't stomp another source's higher floor ---
    CHECK_SKIP(1024, 512, 512, 1,
               "spillover raised floor, priority target=512, last=512 -> skip (do NOT stomp)");

    // --- Regression: target=0 always skip ---
    CHECK_SKIP(1024, 0, 0, 1, "target=0 (no role floor) -> skip");

    printf("\n========================================\n"
           "uclamp restart results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

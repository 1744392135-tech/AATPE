// HOTFIX isolated test — #1/#2: ungated RT restore.
//
// Verifies aatpe_priority::aatpeRestoreThreadPriority() (a) actually reverts a boosted
// tid to its recorded SCHED_OTHER / nice / uclamp, and (b) is NOT blocked by the
// rtBoostAllowed() gate (safe mode active). The capture records the SAME parameters
// the production syscall path uses, so this pins "restore targets SCHED_OTHER" rather
// than only "a function ran".
#include <cstdio>
#include <string>
#include <atomic>
#include <vector>

// ---- production source (whole-TU include, like test_watchdog.cpp) -----------
#include "aatped_priority.cpp"

// ---- minimal stubs for the externals aatped_priority.cpp references --------
std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}

// writeFile: no-op (uclamp-support stamp)
const char* STATE_DIR = "/tmp/hotfix_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFileAtomic(const char*, const std::string&) { return true; }

// C3 exploration stubs (aatped_priority.cpp references these)
std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};

// v4.3.9: per-tid floor ledger (priority registers its PRIORITY floor; the
// whole-TU harness does not pull aatpe_placement_mech.cpp, so define them;
// signatures must match aatpe_placement_mech.h (FloorSet returns int).
namespace aatpe_mech {
int aatpeUclampFloorSet(int, int, unsigned int) { return 0; }
void aatpeUclampFloorClear(int, int) {}
// v4.4.8 MAJOR-2: priority.cpp now reads the PRIORITY floor back from the ledger
// (thermal-derate write-down detection). The restore path never derates, so a
// constant 0 (never-boosted) readback is the correct stub here.
unsigned int aatpeUclampFloorGet(int, int) { return 0; }
}

namespace aatpe_recovery {
// Seeded recovery record for the tid under test.
static RecoveryRecord s_seed;
static bool s_seedValid = false;
static int s_eraseCount = 0;
bool recoveryGet(int tid, RecoveryRecord& out) {
    (void)tid;
    if (!s_seedValid) return false;
    out = s_seed;
    return true;
}
void recoveryErase(int tid) { (void)tid; s_eraseCount++; }
bool recoveryRecordSched(int, int, unsigned long long, uint32_t, uint32_t, int,
                         uint32_t, uint32_t, uint32_t, uint32_t, int, uint32_t, uint32_t) {
    return true;
}
bool recoveryRefreshAppliedSched(int, uint32_t, uint32_t, int, uint32_t, uint32_t) { return true; }
}  // namespace aatpe_recovery

// rtBoostAllowed: the gate. We force it FALSE (safe mode latched) and COUNT calls.
// The restore function must NOT consult it — if it does, the count goes >0 and the
// test fails.
namespace aatpe { namespace subsys { namespace safemode {
static int s_rtGateCalls = 0;
bool rtBoostAllowed() { s_rtGateCalls++; return false; }
}}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

int main() {
    using namespace aatpe_priority;
    printf("=== HOTFIX #1/#2: ungated RT restore ===\n");

    // Seed a recovery record: thread was boosted from SCHED_OTHER/nice -5/uclamp 0.
    aatpe_recovery::s_seed = aatpe_recovery::RecoveryRecord{};
    aatpe_recovery::s_seed.tid = 4242;
    aatpe_recovery::s_seed.origPolicy = 0;       // SCHED_OTHER
    aatpe_recovery::s_seed.origPriority = 0;
    aatpe_recovery::s_seed.origNice = -5;
    aatpe_recovery::s_seed.haveOrigSched = true;
    aatpe_recovery::s_seed.origUclampMin = 0;
    aatpe_recovery::s_seed.origUclampMax = 1024;
    aatpe_recovery::s_seed.haveOrigUclamp = true;
    aatpe_recovery::s_seedValid = true;

    // Put the tid into the RT boosted set (as a boost would).
    ::aatpeRtDebugSeedForTest(4242, (unsigned long long)0x1234);
    CHECK(aatpeRtContainsForTest(4242) == 1, "pre: tid seeded into RT boost set");

    // Enable the syscall capture so restore records intended params instead of
    // issuing real syscalls (we have no real tid / caps on host).
    aatpe_priority::aatpeRtRestoreCaptureEnableForTest(true);
    aatpe_priority::aatpeRtRestoreCaptureResetForTest();

    // Call restore. rtBoostAllowed() is forced FALSE below; restore must still run.
    bool ok = aatpe_priority::aatpeRestoreThreadPriority(4242);
    CHECK(ok, "restore returned true");

    aatpe_priority::RestoreCapture cap = aatpe_priority::aatpeRtRestoreCaptureForTest();
    CHECK(cap.invoked, "restore actually issued a policy/nice restore");
    CHECK(cap.tid == 4242, "restore targeted the right tid");
    CHECK(cap.policy == 0 /*SCHED_OTHER*/, "restore reverted policy to SCHED_OTHER (not clamped to 1)");
    CHECK(cap.prio == 0, "restore reverted static priority to 0");
    CHECK(cap.nice == -5, "restore reverted nice to recorded -5");
    CHECK(cap.uclampMin == 0, "restore dropped uclamp_min back to recorded 0");

    CHECK(aatpeRtContainsForTest(4242) == 0, "tid removed from RT boost set");
    CHECK(aatpe_recovery::s_eraseCount == 1, "recovery record erased after live restore");

    // THE key assertion: restore must NOT go through the safe-mode gate.
    using namespace aatpe::subsys::safemode;
    CHECK(s_rtGateCalls == 0, "restore did NOT consult rtBoostAllowed() (safe mode cannot block release)");

    // Fall-back path: no recovery record -> restore to safe defaults SCHED_OTHER/nice 0.
    aatpe_recovery::s_seedValid = false;
    ::aatpeRtDebugSeedForTest(4343, (unsigned long long)0x9999);
    aatpe_priority::aatpeRtRestoreCaptureResetForTest();
    aatpe_priority::aatpeRestoreThreadPriority(4343);
    cap = aatpe_priority::aatpeRtRestoreCaptureForTest();
    CHECK(cap.invoked && cap.policy == 0 && cap.nice == 0,
          "no-record fallback: restore to SCHED_OTHER/nice 0 defaults");
    CHECK(aatpeRtContainsForTest(4343) == 0, "fallback tid removed from RT boost set");

    printf("\n========================================\n"
           "RT restore results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

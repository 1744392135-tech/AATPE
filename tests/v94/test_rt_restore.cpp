#include <cstdio>
#include <string>
#include <atomic>
#include <vector>

#include "aatped_priority.cpp"

std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}

const char* STATE_DIR = "/tmp/hotfix_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
bool writeFile(const char*, int) { return true; }
bool writeFileAtomic(const char*, const std::string&) { return true; }

std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> thermalSmoothEnabled{false};

namespace aatpe_mech {
int aatpeUclampFloorSet(int, int, unsigned int) { return 0; }
void aatpeUclampFloorClear(int, int) {}
unsigned int aatpeUclampFloorGet(int, int) { return 0; }
}

namespace aatpe_recovery {
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
}

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

    aatpe_recovery::s_seed = aatpe_recovery::RecoveryRecord{};
    aatpe_recovery::s_seed.tid = 4242;
    aatpe_recovery::s_seed.origPolicy = 0;
    aatpe_recovery::s_seed.origPriority = 0;
    aatpe_recovery::s_seed.origNice = -5;
    aatpe_recovery::s_seed.haveOrigSched = true;
    aatpe_recovery::s_seed.origUclampMin = 0;
    aatpe_recovery::s_seed.origUclampMax = 1024;
    aatpe_recovery::s_seed.haveOrigUclamp = true;
    aatpe_recovery::s_seedValid = true;

    ::aatpeRtDebugSeedForTest(4242, (unsigned long long)0x1234);
    CHECK(aatpeRtContainsForTest(4242) == 1, "pre: tid seeded into RT boost set");

    aatpe_priority::aatpeRtRestoreCaptureEnableForTest(true);
    aatpe_priority::aatpeRtRestoreCaptureResetForTest();

    bool ok = aatpe_priority::aatpeRestoreThreadPriority(4242);
    CHECK(ok, "restore returned true");

    aatpe_priority::RestoreCapture cap = aatpe_priority::aatpeRtRestoreCaptureForTest();
    CHECK(cap.invoked, "restore actually issued a policy/nice restore");
    CHECK(cap.tid == 4242, "restore targeted the right tid");
    CHECK(cap.policy == 0 , "restore reverted policy to SCHED_OTHER (not clamped to 1)");
    CHECK(cap.prio == 0, "restore reverted static priority to 0");
    CHECK(cap.nice == -5, "restore reverted nice to recorded -5");
    CHECK(cap.uclampMin == 0, "restore dropped uclamp_min back to recorded 0");

    CHECK(aatpeRtContainsForTest(4242) == 0, "tid removed from RT boost set");
    CHECK(aatpe_recovery::s_eraseCount == 1, "recovery record erased after live restore");

    using namespace aatpe::subsys::safemode;
    CHECK(s_rtGateCalls == 0, "restore did NOT consult rtBoostAllowed() (safe mode cannot block release)");

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

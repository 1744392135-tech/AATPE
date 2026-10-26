// test_spillover_rt.cpp — B-001 regression: spillover RT promote must be
// symmetrically restored on BOTH release paths (the periodic demote loop AND
// releaseAllSpilloverLockedImpl used by shutdown / veto / emergency).
//
// The production code elevated spillover threads to SCHED_FIFO/RR via
// aatpeSetThreadPriority() on the UCLAMP promote branch (placement.cpp:1624),
// but neither the demote loop nor releaseAllSpilloverLockedImpl called the
// ungated restore channel — the RT priority leaked until thread death (priority
// inversion). This test pins the fix: promote a thread under a FORCED-UCLAMP
// capability probe, then drive each release path and assert
// aatpeRestoreThreadPriority() was invoked for exactly the tid we boosted.
//
// Link note: this recipe deliberately OMITS aatped_priority.o and supplies
// recording stubs for the three symbols placement.cpp / aatpe_placement_mech.o
// reference, so we can OBSERVE which restore channel runs (host syscalls on fake
// tids would otherwise make the real priority object return false and never set
// rtBoosted). This mirrors test_coldstart_release.cpp's recording-stub pattern.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "aatped.h"
#include "aatpe_placement_mech.h"
#include "placement.h"
#include "aatped_priority.h"
#include "aatpe_caps.h"

using namespace aatpe;
using namespace aatpe_ml;

// ---- recording stubs for the priority channel (replaces aatped_priority.o) --
namespace aatpe_priority {
static std::vector<int> g_setCalls;     // tids we promoted (setter)
static std::vector<int> g_restoreCalls; // tids we restored
bool aatpeSetThreadPriority(int tid, ThreadRole, int) {
    g_setCalls.push_back(tid);
    return true;   // host: pretend the RT elevation took so rtBoosted latches
}
bool aatpeRestoreThreadPriority(int tid) {
    g_restoreCalls.push_back(tid);
    return true;
}
// aatpe_placement_mech.o:437 references uclampSupportState() in the non-testRoot
// path only; we set a test root, so this is never called at runtime, but it must
// link. Return 1 (supported) so a naive caller sees a sane verdict.
int uclampSupportState() { return 1; }
bool uclampAvailable()   { return true; }
}  // namespace aatpe_priority

// placementSpilloverTick calls this (extern "C", normally in aatped_priority.o).
extern "C" void aatpePruneDeadRtTids(void) {}

// releaseAllSpilloverLocked() is declared in aatped.h under aatpe::placement.

// ---- spillover seams (defined in placement.cpp) ------------------------------
namespace aatpe_spillover {
int placementSpilloverTick(const Config& cfgSnap);
}

// audit5 N3 (U-02): demote-loop TID-reuse identity seam. Exposed by placement.cpp
// (same pattern as the integration S28 scenario). The demote loop now reads the
// LIVE /proc/<tid>/stat starttime and restores ONLY when it still matches the
// value recorded at promote time. To exercise the restore path hermetically we
// promote the test's OWN (live) main thread, whose /proc/<tid>/stat always exists
// with a stable starttime, and seed that same starttime into its ThreadInfo.
namespace aatpe { namespace placement {
unsigned long long spillReadThreadStarttimeForTest(int tid);
}}

static int myTid() { return (int)syscall(SYS_gettid); }

// Strong override of the weak gamelist stub: fg pname already matches, so the
// admission gate does not need the manual-list path; leave it fail-closed=false.

// ---- test harness ------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

static bool calledFor(const std::vector<int>& v, int tid) {
    for (int t : v) if (t == tid) return true;
    return false;
}

// Seed one critical GAME thread currently on a LITTLE core, saturated BIG / idle
// PRIME, foreground known. Returns the seeded tid.
static void seedCandidate(int tid) {
    ThreadInfo ti; ti.tid = tid; ti.role = ROLE_S3_GAME; ti.nrSwitches = 500;
    ti.currentCpu = 0; ti.pname = "com.test.fg";
    g_threads[tid] = ti;
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 90;   // BIG saturated
    g_cpuMetrics[7].util = 10;                                 // PRIME idle
}

int main() {
    ::mkdir("/tmp/aatpe_v94_spillrt", 0755);

    // Force the UCLAMP boost branch: without this the host env reports uclamp
    // unavailable and promotion takes the affinity fallback (which never calls
    // aatpeSetThreadPriority). We want the real UCLAMP promote path that B-001
    // fixed.
    std::string root = "/tmp/aatpe_v94_spillrt";
    aatpe_mech::mechSetTestRoot(root);
    aatpe_mech::mechSetUclampVerdict(1);   // 1 => c.uclamp = true

    extern std::string g_cachedForegroundApp;
    g_cachedForegroundApp = "com.test.fg";
    Config cfg;                // controlMode defaults to "assist" (mode=1)

    printf("=== B-001: spillover RT promote -> symmetric restore ===\n");

    // ---- Phase 1: promote a LIVE thread, then drive the DEMOTE loop ----------
    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        // Promote our own (always-live) main thread so the N3 demote identity
        // guard sees a real /proc/<tid>/stat. Seed its ThreadInfo.starttime with
        // the LIVE starttime so recorded==live -> the demote loop may restore.
        int liveTid = myTid();
        unsigned long long liveSt = aatpe::placement::spillReadThreadStarttimeForTest(liveTid);
        CHECK(liveSt != 0, "seed: live main-thread starttime readable from /proc");
        seedCandidate(liveTid);
        g_threads[liveTid].starttime = liveSt;
        aatpe_priority::g_setCalls.clear();
        aatpe_priority::g_restoreCalls.clear();

        aatpe_spillover::placementSpilloverTick(cfg);   // cycle1: satisfy=1
        aatpe_spillover::placementSpilloverTick(cfg);   // cycle2: satisfy=2 -> promote
        CHECK(calledFor(aatpe_priority::g_setCalls, liveTid),
              "promote (UCLAMP branch) calls aatpeSetThreadPriority(liveTid)");
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "promote increments spillover counter");

        // Make the thread leave the desired set: drop it from the thread table and
        // relax BIG load, then wait out the assist dwell so the demote loop fires.
        g_threads.erase(liveTid);
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 30;
        }
        usleep(350 * 1000);   // > kDwellMsAssist (300ms)
        aatpe_spillover::placementSpilloverTick(cfg);

        CHECK(calledFor(aatpe_priority::g_restoreCalls, liveTid),
              "demote loop calls aatpeRestoreThreadPriority(liveTid) [identity matched]");
    }

    // ---- Phase 2: promote tid 701, then drive releaseAllSpilloverLocked() ----
    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        seedCandidate(701);
        aatpe_priority::g_setCalls.clear();
        aatpe_priority::g_restoreCalls.clear();

        aatpe_spillover::placementSpilloverTick(cfg);   // cycle1
        aatpe_spillover::placementSpilloverTick(cfg);   // cycle2 -> promote
        CHECK(calledFor(aatpe_priority::g_setCalls, 701),
              "release-phase promote calls aatpeSetThreadPriority(701)");
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "release-phase promotion increments counter");

        // Full release (shutdown / veto / emergency path).
        aatpe::placement::releaseAllSpilloverLocked();
        CHECK(calledFor(aatpe_priority::g_restoreCalls, 701),
              "releaseAllSpilloverLocked calls aatpeRestoreThreadPriority(701)");
        CHECK(aatpe::placement::spilloverActiveCount() == 0,
              "releaseAllSpilloverLocked drains active counter to 0");

        g_threads.erase(701);
    }

    printf("\n========================================\n"
           "Spillover RT restore results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

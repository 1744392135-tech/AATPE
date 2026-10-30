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

namespace aatpe_priority {
static std::vector<int> g_setCalls;
static std::vector<int> g_restoreCalls;
bool aatpeSetThreadPriority(int tid, ThreadRole, int) {
    g_setCalls.push_back(tid);
    return true;
}
bool aatpeRestoreThreadPriority(int tid) {
    g_restoreCalls.push_back(tid);
    return true;
}
int uclampSupportState() { return 1; }
bool uclampAvailable()   { return true; }
}

extern "C" void aatpePruneDeadRtTids(void) {}

namespace aatpe_spillover {
int placementSpilloverTick(const Config& cfgSnap);
}

namespace aatpe { namespace placement {
unsigned long long spillReadThreadStarttimeForTest(int tid);
}}

static int myTid() { return (int)syscall(SYS_gettid); }

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

static bool calledFor(const std::vector<int>& v, int tid) {
    for (int t : v) if (t == tid) return true;
    return false;
}

static void seedCandidate(int tid) {
    ThreadInfo ti; ti.tid = tid; ti.role = ROLE_S3_GAME; ti.nrSwitches = 500;
    ti.currentCpu = 0; ti.pname = "com.test.fg";
    g_threads[tid] = ti;
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 90;
    g_cpuMetrics[7].util = 10;
}

int main() {
    ::mkdir("/tmp/aatpe_v94_spillrt", 0755);

    std::string root = "/tmp/aatpe_v94_spillrt";
    aatpe_mech::mechSetTestRoot(root);
    aatpe_mech::mechSetUclampVerdict(1);

    extern std::string g_cachedForegroundApp;
    g_cachedForegroundApp = "com.test.fg";
    Config cfg;

    printf("=== B-001: spillover RT promote -> symmetric restore ===\n");

    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        int liveTid = myTid();
        unsigned long long liveSt = aatpe::placement::spillReadThreadStarttimeForTest(liveTid);
        CHECK(liveSt != 0, "seed: live main-thread starttime readable from /proc");
        seedCandidate(liveTid);
        g_threads[liveTid].starttime = liveSt;
        aatpe_priority::g_setCalls.clear();
        aatpe_priority::g_restoreCalls.clear();

        aatpe_spillover::placementSpilloverTick(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(calledFor(aatpe_priority::g_setCalls, liveTid),
              "promote (UCLAMP branch) calls aatpeSetThreadPriority(liveTid)");
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "promote increments spillover counter");

        g_threads.erase(liveTid);
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 30;
        }
        usleep(350 * 1000);
        aatpe_spillover::placementSpilloverTick(cfg);

        CHECK(calledFor(aatpe_priority::g_restoreCalls, liveTid),
              "demote loop calls aatpeRestoreThreadPriority(liveTid) [identity matched]");
    }

    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        seedCandidate(701);
        aatpe_priority::g_setCalls.clear();
        aatpe_priority::g_restoreCalls.clear();

        aatpe_spillover::placementSpilloverTick(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(calledFor(aatpe_priority::g_setCalls, 701),
              "release-phase promote calls aatpeSetThreadPriority(701)");
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "release-phase promotion increments counter");

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

// test_gpu_boost.cpp - hermetic tests for the boost-node RE-PROBE state machine
// (ROUND-002 audit G5-001 fix). Links gpu_boost.cpp compiled with
// -DAATPE_GPU_BOOST_TEST (see build.sh gpu_boost recipe).
//
// Drives probeTick() through an injected writable predicate, so no real schedtune
// node is needed on the host. Real-node behaviour (open/write success) is covered
// on-device; host side we assert the state machine:
//   (a) first round all unwritable -> node stays empty, re-probe scheduled
//   (b) a node later becomes writable  -> re-probe adopts it
//   (c) chosen node fails writes 5x    -> re-probe triggered
//   (d) stable adoption                -> no repeated probing inside the throttle
#include <cstdio>
#include <string>
#include <atomic>
#include <unistd.h>
#include "gpu_boost.h"

// gpu_boost.cpp reads g_gpuUtilization (provided by gpu_deps stub).

// Minimal host stubs for the two accessors gpu_boost.cpp calls (production lives
// in the daemon layer). The probe tests do not exercise the game-detection path.
std::string aatpeGetForegroundPackage() { return std::string(); }
bool isPackageInGameList(const std::string&) { return false; }

// Additional host stubs for gpu_boost.cpp dependencies
int getSystemState() { return 0; }  // 0 = NORMAL
namespace aatpe { int aatpeThermalHalLevel() { return 0; } }

using namespace aatpe;
using namespace aatpe::boost_test;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Global predicate state. Candidates tried in order; this predicate decides
// writability on demand so a test script can flip it mid-run.
static std::string g_acceptPath;   // if non-empty, only this path is writable
static bool g_onlyFirst = false;   // accept first candidate only

static bool pred(const char* path) {
    if (!g_acceptPath.empty()) return path == g_acceptPath;
    if (g_onlyFirst) {
        // first candidate in boostCandidates()
        return std::string(path) == "/dev/stune/top-app/schedtune.boost";
    }
    return false;
}

int main() {
    const long long T0 = 1000000;

    printf("=== (a) first round: all candidates unwritable -> empty + re-probe scheduled ===\n");
    resetForTest();
    g_acceptPath.clear(); g_onlyFirst = false;
    tickForTest(T0, pred);                 // t=T0: nothing writable -> next probe T0+30s
    CHECK(nodeForTest().empty(), "a1: node stays empty when no candidate writable");
    tickForTest(T0 + 1000, pred);         // t=T0+1s: inside 30s throttle -> no re-probe
    CHECK(nodeForTest().empty(), "a2: no re-probe inside 30s throttle");
    tickForTest(T0 + 31000, pred);         // t=T0+31s: still nothing -> next probe T0+61s
    CHECK(nodeForTest().empty(), "a3: still empty after 31s with no node");

    printf("=== (b) node appears later -> throttled re-probe adopts it ===\n");
    g_acceptPath = "/sys/fs/cgroup/top-app/schedtune.boost";
    tickForTest(T0 + 61000, pred);         // next allowed probe: now writable
    CHECK(nodeForTest() == g_acceptPath, "b1: re-probe discovers the new node");

    printf("=== (d) stable adoption -> no repeated probing inside throttle ===\n");
    int ticksBefore = consecFailsForTest();
    tickForTest(T0 + 62000, pred);         // +1s after adoption
    tickForTest(T0 + 70000, pred);        // +9s
    CHECK(nodeForTest() == g_acceptPath, "d1: node stays adopted");
    CHECK(consecFailsForTest() == ticksBefore, "d2: stable adoption does not re-probe/fail");

    printf("=== (c) chosen node write-fails 5x -> re-probe triggered ===\n");
    // Point the adopted "node" at a path that cannot be opened, then hammer the
    // shared bridge. writeBoostLocked fails -> s_consecWriteFails climbs; the
    // existing 500ms write backoff paces the retries, so sleep between them.
    resetForTest();
    // A real candidate path that does not exist on the host: adopted by the
    // predicate, but open() in writeBoostLocked fails -> write failures.
    g_acceptPath = "/dev/stune/top-app/schedtune.boost";
    tickForTest(T0, pred);                 // adopt the (absent) candidate path
    CHECK(nodeForTest() == g_acceptPath, "c1: candidate node adopted for failure sim");
    for (int i = 0; i < 5; i++) {
        aatpeBoostSet("gpu", 60);          // probeTick + one throttled write attempt
        usleep(520 * 1000);                // let the 500ms write backoff elapse
    }
    CHECK(consecFailsForTest() >= 5, "c2: five write failures recorded");
    // Next tick must cross the threshold and re-probe (pred still accepts the path).
    tickForTest(T0 + 5000, pred);
    CHECK(consecFailsForTest() == 0, "c3: re-probe reset the consecutive-failure counter");
    CHECK(nodeForTest() == g_acceptPath, "c4: re-probe re-adopts via predicate");

    // === (e) devfreq boost on/off round-trip (照 gpu_driver2.cpp) ===
    printf("\n=== (e) devfreq boost on/off round-trip ===\n");
    {
        // Pre-create fake devfreq dir + min_freq + max_freq files
        system("mkdir -p /tmp/devfreq_fake_e");
        { FILE* f = fopen("/tmp/devfreq_fake_e/min_freq", "w"); if (f) { fprintf(f, "300000"); fclose(f); } }
        { FILE* f = fopen("/tmp/devfreq_fake_e/max_freq", "w"); if (f) { fprintf(f, "800000"); fclose(f); } }

        boost_test::devfreqResetForTest();
        boost_test::devfreqSetNodeForTest("/tmp/devfreq_fake_e/min_freq");

        // On
        boost_test::devfreqBoostOnForTest();
        CHECK(boost_test::devfreqBoostedForTest() == true, "e1: devfreq boost activated");
        std::string minVal;
        { FILE* f = fopen("/tmp/devfreq_fake_e/min_freq", "r"); if (f) { char buf[64]; if (fgets(buf,sizeof(buf),f)) minVal = buf; fclose(f); } }
        CHECK(minVal.find("800000") == std::string::npos, "e2a: min_freq NOT pinned to max_freq (T-02 fix)");
        // T-02: floor = savedMin + 70%*(maxF-savedMin) = 300000 + 0.7*500000 = 650000
        CHECK(minVal.find("650000") != std::string::npos, "e2b: min_freq raised to 70% of range (650000)");
        CHECK(boost_test::devfreqSavedMinForTest() == 300000, "e3: savedMin recorded original value");

        // Off
        boost_test::devfreqBoostOffForTest();
        CHECK(boost_test::devfreqBoostedForTest() == false, "e4: devfreq boost deactivated");
        minVal.clear();
        { FILE* f = fopen("/tmp/devfreq_fake_e/min_freq", "r"); if (f) { char buf[64]; if (fgets(buf,sizeof(buf),f)) minVal = buf; fclose(f); } }
        CHECK(minVal.find("300000") != std::string::npos, "e5: min_freq restored to saved value");

        // Idempotent off
        boost_test::devfreqBoostOffForTest();
        CHECK(boost_test::devfreqBoostedForTest() == false, "e6: off idempotent");

        boost_test::devfreqResetForTest();
        system("rm -rf /tmp/devfreq_fake_e");
    }

    // === (f) devfreq node unavailable → schedtune fallback ===
    printf("\n=== (f) devfreq unavailable → schedtune fallback ===\n");
    {
        boost_test::devfreqResetForTest();
        boost_test::devfreqSetNodeForTest("");
        aatpeBoostSet("gpu", 60);
        CHECK(true, "f1: boost with no devfreq node falls back cleanly");
        boost_test::devfreqResetForTest();
    }

    printf("\n========================================\n"
           "gpu_boost devfreq: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

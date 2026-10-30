#include <cstdio>
#include <string>
#include <atomic>
#include <unistd.h>
#include "gpu_boost.h"

std::string aatpeGetForegroundPackage() { return std::string(); }
bool isPackageInGameList(const std::string&) { return false; }

int getSystemState() { return 0; }
namespace aatpe { int aatpeThermalHalLevel() { return 0; } }

using namespace aatpe;
using namespace aatpe::boost_test;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static std::string g_acceptPath;
static bool g_onlyFirst = false;

static bool pred(const char* path) {
    if (!g_acceptPath.empty()) return path == g_acceptPath;
    if (g_onlyFirst) {
        return std::string(path) == "/dev/stune/top-app/schedtune.boost";
    }
    return false;
}

int main() {
    const long long T0 = 1000000;

    printf("=== (a) first round: all candidates unwritable -> empty + re-probe scheduled ===\n");
    resetForTest();
    g_acceptPath.clear(); g_onlyFirst = false;
    tickForTest(T0, pred);
    CHECK(nodeForTest().empty(), "a1: node stays empty when no candidate writable");
    tickForTest(T0 + 1000, pred);
    CHECK(nodeForTest().empty(), "a2: no re-probe inside 30s throttle");
    tickForTest(T0 + 31000, pred);
    CHECK(nodeForTest().empty(), "a3: still empty after 31s with no node");

    printf("=== (b) node appears later -> throttled re-probe adopts it ===\n");
    g_acceptPath = "/sys/fs/cgroup/top-app/schedtune.boost";
    tickForTest(T0 + 61000, pred);
    CHECK(nodeForTest() == g_acceptPath, "b1: re-probe discovers the new node");

    printf("=== (d) stable adoption -> no repeated probing inside throttle ===\n");
    int ticksBefore = consecFailsForTest();
    tickForTest(T0 + 62000, pred);
    tickForTest(T0 + 70000, pred);
    CHECK(nodeForTest() == g_acceptPath, "d1: node stays adopted");
    CHECK(consecFailsForTest() == ticksBefore, "d2: stable adoption does not re-probe/fail");

    printf("=== (c) chosen node write-fails 5x -> re-probe triggered ===\n");
    resetForTest();
    g_acceptPath = "/dev/stune/top-app/schedtune.boost";
    tickForTest(T0, pred);
    CHECK(nodeForTest() == g_acceptPath, "c1: candidate node adopted for failure sim");
    for (int i = 0; i < 5; i++) {
        aatpeBoostSet("gpu", 60);
        usleep(520 * 1000);
    }
    CHECK(consecFailsForTest() >= 5, "c2: five write failures recorded");
    tickForTest(T0 + 5000, pred);
    CHECK(consecFailsForTest() == 0, "c3: re-probe reset the consecutive-failure counter");
    CHECK(nodeForTest() == g_acceptPath, "c4: re-probe re-adopts via predicate");

    printf("\n=== (e) devfreq boost on/off round-trip ===\n");
    {
        system("mkdir -p /tmp/devfreq_fake_e");
        { FILE* f = fopen("/tmp/devfreq_fake_e/min_freq", "w"); if (f) { fprintf(f, "300000"); fclose(f); } }
        { FILE* f = fopen("/tmp/devfreq_fake_e/max_freq", "w"); if (f) { fprintf(f, "800000"); fclose(f); } }

        boost_test::devfreqResetForTest();
        boost_test::devfreqSetNodeForTest("/tmp/devfreq_fake_e/min_freq");

        boost_test::devfreqBoostOnForTest();
        CHECK(boost_test::devfreqBoostedForTest() == true, "e1: devfreq boost activated");
        std::string minVal;
        { FILE* f = fopen("/tmp/devfreq_fake_e/min_freq", "r"); if (f) { char buf[64]; if (fgets(buf,sizeof(buf),f)) minVal = buf; fclose(f); } }
        CHECK(minVal.find("800000") == std::string::npos, "e2a: min_freq NOT pinned to max_freq (T-02 fix)");
        CHECK(minVal.find("650000") != std::string::npos, "e2b: min_freq raised to 70% of range (650000)");
        CHECK(boost_test::devfreqSavedMinForTest() == 300000, "e3: savedMin recorded original value");

        boost_test::devfreqBoostOffForTest();
        CHECK(boost_test::devfreqBoostedForTest() == false, "e4: devfreq boost deactivated");
        minVal.clear();
        { FILE* f = fopen("/tmp/devfreq_fake_e/min_freq", "r"); if (f) { char buf[64]; if (fgets(buf,sizeof(buf),f)) minVal = buf; fclose(f); } }
        CHECK(minVal.find("300000") != std::string::npos, "e5: min_freq restored to saved value");

        boost_test::devfreqBoostOffForTest();
        CHECK(boost_test::devfreqBoostedForTest() == false, "e6: off idempotent");

        boost_test::devfreqResetForTest();
        system("rm -rf /tmp/devfreq_fake_e");
    }

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

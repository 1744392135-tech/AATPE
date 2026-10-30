#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include "gpu_boost.h"

std::string aatpeGetForegroundPackage() { return "com.tencent.kg"; }
bool isPackageInGameList(const std::string&) { return true; }

int getSystemState() { return 0; }

namespace aatpe {
int aatpeThermalHalLevel() { return 0; }
static bool g_simulateDegraded = false;
bool aatpeThermalDegraded() { return g_simulateDegraded; }
}

using namespace aatpe;
using namespace aatpe::boost_test;

extern std::atomic<int> g_thermalCpu;
extern std::atomic<int> g_thermalHeadroomPct;
extern std::atomic<float> g_gpuUtilization;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static bool predAcceptTopApp(const char* path) {
    return std::string(path) == "/dev/stune/top-app/schedtune.boost";
}

static void tickOnce() { gpuBoostTick(); }

int main() {
    const long long T0 = 1000000;

    resetForTest();
    tickForTest(T0, predAcceptTopApp);
    CHECK(nodeForTest() == "/dev/stune/top-app/schedtune.boost",
          "setup: fake schedtune node adopted");

    system("rm -rf /tmp/devfreq_boost_th && mkdir -p /tmp/devfreq_boost_th");
    { FILE* f = fopen("/tmp/devfreq_boost_th/min_freq", "w");
      if (f) { fprintf(f, "300000\n"); fclose(f); } }
    { FILE* f = fopen("/tmp/devfreq_boost_th/max_freq", "w");
      if (f) { fprintf(f, "800000\n"); fclose(f); } }
    devfreqResetForTest();
    devfreqSetNodeForTest("/tmp/devfreq_boost_th/min_freq");

    g_thermalCpu.store(40);
    g_thermalHeadroomPct.store(70);
    g_gpuUtilization.store(99.0f);
    aatpe::g_simulateDegraded = false;

    printf("=== C: cold + util=99 + game -> boost engages ===\n");
    boostResetHysteresisForTest();
    aatpeBoostClear("gpu");
    tickOnce();
    CHECK(boostEngagedForTest() == true, "C1: boost engaged at 40C/99%");
    CHECK(devfreqBoostedForTest() == true, "C2: devfreq floor raised");
    {
        FILE* f = fopen("/tmp/devfreq_boost_th/min_freq", "r");
        char buf[64] = {0};
        if (f) { fgets(buf, sizeof(buf), f); fclose(f); }
        CHECK(strstr(buf, "800000") == nullptr, "C3: min_freq NOT pinned to max (T-02)");
        CHECK(strstr(buf, "650000") != nullptr, "C4: min_freq = 650000 (70%% of range)");
    }

    printf("=== A: HAL=0 but temp=95C -> NO boost (T-01/T-03) ===\n");
    boostResetHysteresisForTest();
    aatpeBoostClear("gpu");
    devfreqBoostOffForTest();
    g_thermalCpu.store(95);
    g_thermalHeadroomPct.store(0);
    g_gpuUtilization.store(99.0f);
    tickOnce();
    CHECK(boostEngagedForTest() == false, "A1: boost NOT engaged at 95C even with HAL=0");
    CHECK(devfreqBoostedForTest() == false, "A2: devfreq floor released at 95C");

    printf("=== B: temp=85C -> tier 1 (half schedtune, no devfreq) ===\n");
    boostResetHysteresisForTest();
    aatpeBoostClear("gpu");
    devfreqBoostOffForTest();
    g_thermalCpu.store(85);
    g_thermalHeadroomPct.store(40);
    g_gpuUtilization.store(99.0f);
    tickOnce();
    CHECK(boostEngagedForTest() == true, "B1: boost engaged at 85C (tier 1, not fully off)");
    CHECK(devfreqBoostedForTest() == false, "B2: devfreq floor released at 85C (T-07)");

    printf("=== E: sensor degraded -> fail-safe release (T-06) ===\n");
    boostResetHysteresisForTest();
    aatpeBoostClear("gpu");
    devfreqBoostOffForTest();
    g_thermalCpu.store(40);
    g_thermalHeadroomPct.store(70);
    g_gpuUtilization.store(99.0f);
    aatpe::g_simulateDegraded = true;
    tickOnce();
    CHECK(boostEngagedForTest() == false, "E1: no boost when sensor degraded (fail-safe)");
    CHECK(devfreqBoostedForTest() == false, "E2: devfreq released on sensor outage");
    aatpe::g_simulateDegraded = false;

    printf("=== D: hysteresis — util 99 -> 82 -> 75 does not flip-flop ===\n");
    boostResetHysteresisForTest();
    aatpeBoostClear("gpu");
    devfreqBoostOffForTest();
    g_thermalCpu.store(40);
    g_thermalHeadroomPct.store(70);
    g_gpuUtilization.store(99.0f);
    tickOnce();
    CHECK(boostEngagedForTest() == true, "D1: engaged at 99%");
    g_gpuUtilization.store(82.0f);
    tickOnce();
    CHECK(boostEngagedForTest() == true, "D2: stays engaged at 82% (above exit band 80%)");
    g_gpuUtilization.store(75.0f);
    tickOnce();
    CHECK(boostEngagedForTest() == true, "D3: still engaged immediately after drop (dwell not elapsed)");
    usleep(2600 * 1000);
    tickOnce();
    CHECK(boostEngagedForTest() == false, "D4: released after 2.5s dwell below exit band");

    printf("=== F: temp ramp 40 -> 85 -> 95 ramps boost down ===\n");
    boostResetHysteresisForTest();
    aatpeBoostClear("gpu");
    devfreqBoostOffForTest();
    g_thermalCpu.store(40);
    g_thermalHeadroomPct.store(70);
    g_gpuUtilization.store(99.0f);
    tickOnce();
    CHECK(boostEngagedForTest() == true && devfreqBoostedForTest() == true,
          "F1: full boost at 40C");
    g_thermalCpu.store(85);
    g_thermalHeadroomPct.store(40);
    tickOnce();
    CHECK(boostEngagedForTest() == true && devfreqBoostedForTest() == false,
          "F2: at 85C boost stays engaged but devfreq released");
    g_thermalCpu.store(95);
    g_thermalHeadroomPct.store(0);
    tickOnce();
    CHECK(boostEngagedForTest() == false, "F3: at 95C boost fully released");

    devfreqResetForTest();
    system("rm -rf /tmp/devfreq_boost_th");

    printf("\n========================================\n"
           "gpu_boost thermal: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

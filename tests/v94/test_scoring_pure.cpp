#include <cstdio>
#include <cmath>
#include <string>
#include "aatped.h"
#include "scoring_interface.h"
using namespace aatpe;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    initScoringArrays(4);

    printf("=== Scoring-pure: predictCpuLoadMultiStep ===\n");
    float bad = predictCpuLoadMultiStep(999, 3);
    CHECK(bad == 50.0f, "multiStep out-of-range cpu -> 50");
    float empty = predictCpuLoadMultiStep(2, 2);
    CHECK(empty == 50.0f, "multiStep empty history -> 50");
    recordCpuLoadSample(3, 40);
    float one = predictCpuLoadMultiStep(3, 1);
    CHECK(one == 40.0f, "multiStep one-sample history -> back value 40");
    for (int v = 20; v <= 29; v++) recordCpuLoadSample(0, v);
    float m1 = predictCpuLoadMultiStep(0, 0);
    float m5 = predictCpuLoadMultiStep(0, 99);
    float m3 = predictCpuLoadMultiStep(0, 3);
    CHECK(m1 >= 0.0f && m1 <= 100.0f, "multiStep steps=0 clamps to 1, in [0,100]");
    CHECK(m5 >= 0.0f && m5 <= 100.0f, "multiStep steps=99 clamps to 5, in [0,100]");
    CHECK(m5 >= m1 - 1.0f, "multiStep longer horizon >= near horizon for rising trend");
    (void)m3;

    printf("=== Scoring-pure: latencyScore role table ===\n");
    CHECK(latencyScore(0, ROLE_S0_SYSTEM)   == 0,        "latency S0 system -> 0");
    CHECK(latencyScore(0, ROLE_S1_UI)       == 200,      "latency S1 UI -> 200");
    CHECK(latencyScore(0, ROLE_S2_RENDER)   == 180,      "latency S2 render -> 180");
    CHECK(latencyScore(0, ROLE_S3_GAME)    == 150,      "latency S3 game -> 150");
    CHECK(latencyScore(0, ROLE_S4_THROUGHPUT) == 50,    "latency S4 throughput -> 50");
    CHECK(latencyScore(0, ROLE_S5_BACKGROUND)  == 0,     "latency S5 background -> 0");
    CHECK(latencyScore(0, (ThreadRole)999)  == 0,        "latency unknown role -> 0 default");

    printf("=== Scoring-pure: ScoringContext accessors ===\n");
    {
        std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
        g_cpuMetrics[1].capacity = 400;
        g_cpuMetrics[1].temp      = 35;
        g_cpuMetrics[1].util      = 55;
    }
    Config cfg;
    ScoringContext ctx = buildScoringContext(cfg);
    CHECK(ctx.getCapacity(1) == 400, "ctx.getCapacity hits seeded cpu");
    CHECK(ctx.getTemp(1)     == 35,  "ctx.getTemp hits seeded cpu");
    CHECK(ctx.getUtil(1)     == 55,  "ctx.getUtil hits seeded cpu");
    CHECK(ctx.getCapacity(77) == 0,  "ctx.getCapacity unknown cpu -> 0");
    CHECK(ctx.getTemp(77)     == 0,  "ctx.getTemp unknown cpu -> 0");
    CHECK(ctx.getUtil(77)     == 0, "ctx.getUtil unknown cpu -> 0");

    printf("\n========================================\nScoring-pure results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

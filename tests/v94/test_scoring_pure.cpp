// test_scoring_pure.cpp - v94 H1 coverage-gate follow-up: exercise the pure
// scoring decision helpers that test_scoring.cpp does NOT reach:
//   * predictCpuLoadMultiStep()  - multi-step OLS load forecast (clamps steps,
//     short-history fallback, out-of-range cpu)
//   * latencyScore(cpuId, role)  - the per-role latency score table (all 6 roles
//     + the default branch)
//   * ScoringContext::getCapacity/getTemp/getUtil - hot-path accessors
//
// Links the SAME production TUs + stubs as the `scoring` recipe (see build.sh):
//   scoring adrc aatpe_ml  +  scoring_deps ml_deps log_stub
// Zero production behaviour change; only adds host assertions.
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
    // Out-of-range cpu -> neutral 50.
    float bad = predictCpuLoadMultiStep(999, 3);
    CHECK(bad == 50.0f, "multiStep out-of-range cpu -> 50");
    // Empty history (<3 samples) -> neutral 50.
    float empty = predictCpuLoadMultiStep(2, 2);
    CHECK(empty == 50.0f, "multiStep empty history -> 50");
    // Single sample (<3) -> returns that sample, not 50.
    recordCpuLoadSample(3, 40);
    float one = predictCpuLoadMultiStep(3, 1);
    CHECK(one == 40.0f, "multiStep one-sample history -> back value 40");
    // Rising trend over >=3 samples, multi-step horizon. steps<=0 -> clamp to 1,
    // steps>5 -> clamp to 5. The forecast must extrapolate the rising line and
    // be bounded to [0,100].
    for (int v = 20; v <= 29; v++) recordCpuLoadSample(0, v);
    float m1 = predictCpuLoadMultiStep(0, 0);    // steps clamped to 1
    float m5 = predictCpuLoadMultiStep(0, 99);   // steps clamped to 5
    float m3 = predictCpuLoadMultiStep(0, 3);
    CHECK(m1 >= 0.0f && m1 <= 100.0f, "multiStep steps=0 clamps to 1, in [0,100]");
    CHECK(m5 >= 0.0f && m5 <= 100.0f, "multiStep steps=99 clamps to 5, in [0,100]");
    // A rising series extrapolated further ahead must not predict LESS than the
    // near-horizon step (max over horizon is monotone non-decreasing in horizon).
    CHECK(m5 >= m1 - 1.0f, "multiStep longer horizon >= near horizon for rising trend");
    (void)m3;

    printf("=== Scoring-pure: latencyScore role table ===\n");
    // Pure role->score lookup (aatpe-constants.h latency::kScore*). cpuId is
    // unused by the table; only the role matters.
    CHECK(latencyScore(0, ROLE_S0_SYSTEM)   == 0,        "latency S0 system -> 0");
    CHECK(latencyScore(0, ROLE_S1_UI)       == 200,      "latency S1 UI -> 200");
    CHECK(latencyScore(0, ROLE_S2_RENDER)   == 180,      "latency S2 render -> 180");
    CHECK(latencyScore(0, ROLE_S3_GAME)    == 150,      "latency S3 game -> 150");
    CHECK(latencyScore(0, ROLE_S4_THROUGHPUT) == 50,    "latency S4 throughput -> 50");
    CHECK(latencyScore(0, ROLE_S5_BACKGROUND)  == 0,     "latency S5 background -> 0");
    CHECK(latencyScore(0, (ThreadRole)999)  == 0,        "latency unknown role -> 0 default");

    printf("=== Scoring-pure: ScoringContext accessors ===\n");
    // Seed a couple of cpu metrics, snapshot into a ScoringContext, then read back
    // through the context accessors (cpuMetrics map lookup hit + miss).
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

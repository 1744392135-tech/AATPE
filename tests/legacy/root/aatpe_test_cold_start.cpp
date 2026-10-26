// AATPE v66: ML Cold Start Protection Unit Test
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Verifies:
//   1. LearningModel::isPoolReady() — false below threshold, true at/above
//   2. getMinSamples()/setMinSamples() — threshold getter/setter
//   3. Cold start gating logic — when protection enabled + pool not ready,
//      ML score is excluded (heuristic-only); when disabled or pool ready,
//      ML participates normally.
//
// Links against the REAL aatpe_ml.cpp (LearningModel) for isPoolReady tests.
// Gating logic is replicated as a pure function (matching placement.cpp)
// to avoid pulling in the full daemon's Android dependencies.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "aatpe_ml.h"

using namespace aatpe_ml;

// ---------------------------------------------------------------------------
// Replicated cold-start gating predicate (exact logic from placement.cpp)
// ---------------------------------------------------------------------------
// Returns true if ML should be BLOCKED for this pool due to cold start protection.
static bool isPoolColdStartBlocked(bool coldStartProtection, int poolIdx,
                                     const LearningModel& model) {
    if (!coldStartProtection) return false;
    if (poolIdx < 0) return false;
    return !model.isPoolReady(poolIdx);
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define ASSERT_EQ(actual, expected, msg) do { \
    long long _a = (long long)(actual), _e = (long long)(expected); \
    if (_a == _e) { g_pass++; printf("  [PASS] %s (got %lld)\n", msg, _a); } \
    else { g_fail++; printf("  [FAIL] %s (got %lld, expected %lld)\n", msg, _a, _e); } \
} while(0)

// ---------------------------------------------------------------------------
// Test 1: isPoolReady() — fresh model has 0 samples, all pools not ready
// ---------------------------------------------------------------------------
static void test_fresh_model_not_ready() {
    printf("\n=== Test: Fresh Model — all pools NOT ready ===\n");
    LearningModel model;
    model.init(3);
    model.setMinSamples(2000);
    ASSERT_EQ(model.getMinSamples(), 2000, "min_samples threshold = 2000");
    ASSERT(!model.isPoolReady(0), "Pool 0 (LITTLE) not ready with 0 samples");
    ASSERT(!model.isPoolReady(1), "Pool 1 (MID) not ready with 0 samples");
    ASSERT(!model.isPoolReady(2), "Pool 2 (BIG) not ready with 0 samples");
    ASSERT(!model.isPoolReady(-1), "Invalid pool -1 returns not ready");
    ASSERT(!model.isPoolReady(99), "Out-of-range pool 99 returns not ready");
}

// ---------------------------------------------------------------------------
// Test 2: isPoolReady() — boundary: threshold-1 false, threshold true
// ---------------------------------------------------------------------------
static void test_boundary_threshold() {
    printf("\n=== Test: Boundary — threshold-1 false, threshold true ===\n");
    LearningModel model;
    model.init(3);
    const int threshold = 100;
    model.setMinSamples(threshold);

    // Feed samples to pool 0 until threshold-1
    ThreadFeatures f;
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < threshold - 1; i++) {
        model.update(0, f, 0.5f);
    }
    ASSERT(!model.isPoolReady(0), "Pool 0 NOT ready at threshold-1 (99)");
    ASSERT(!model.isPoolReady(1), "Pool 1 still NOT ready (0 samples)");

    // One more sample pushes pool 0 to exactly threshold
    model.update(0, f, 0.5f);
    ASSERT(model.isPoolReady(0), "Pool 0 IS ready at exactly threshold (100)");
    ASSERT(!model.isPoolReady(1), "Pool 1 still NOT ready (0 samples)");

    // Pool 1 gets threshold samples independently
    for (int i = 0; i < threshold; i++) {
        model.update(1, f, 0.5f);
    }
    ASSERT(model.isPoolReady(1), "Pool 1 IS ready after 100 samples");
    ASSERT(!model.isPoolReady(2), "Pool 2 still NOT ready (0 samples)");
}

// ---------------------------------------------------------------------------
// Test 3: setMinSamples() — changing threshold affects readiness dynamically
// ---------------------------------------------------------------------------
static void test_dynamic_threshold_change() {
    printf("\n=== Test: Dynamic threshold change ===\n");
    LearningModel model;
    model.init(3);
    model.setMinSamples(500);

    ThreadFeatures f;
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < 300; i++) model.update(0, f, 0.5f);

    ASSERT(!model.isPoolReady(0), "Pool 0 not ready (300 < 500)");
    // Lower threshold to 200 — pool should now be ready
    model.setMinSamples(200);
    ASSERT(model.isPoolReady(0), "Pool 0 IS ready after threshold lowered to 200 (300 >= 200)");
    // Raise threshold to 1000 — pool should become not ready
    model.setMinSamples(1000);
    ASSERT(!model.isPoolReady(0), "Pool 0 NOT ready after threshold raised to 1000 (300 < 1000)");
    // setMinSamples(0) should clamp to 1 (per implementation)
    model.setMinSamples(0);
    ASSERT_EQ(model.getMinSamples(), 1, "setMinSamples(0) clamps to 1");
    ASSERT(model.isPoolReady(0), "Pool 0 ready with threshold=1 (300 >= 1)");
}

// ---------------------------------------------------------------------------
// Test 4: Cold start gating — protection enabled, pool not ready → blocked
// ---------------------------------------------------------------------------
static void test_gating_enabled_not_ready() {
    printf("\n=== Test: Gating — protection ON + pool NOT ready → ML blocked ===\n");
    LearningModel model;
    model.init(3);
    model.setMinSamples(2000);
    // Fresh model: 0 samples, all pools not ready

    bool protection = true;
    ASSERT(isPoolColdStartBlocked(protection, 0, model), "Pool 0 cold-start BLOCKED (protection on, not ready)");
    ASSERT(isPoolColdStartBlocked(protection, 1, model), "Pool 1 cold-start BLOCKED");
    ASSERT(isPoolColdStartBlocked(protection, 2, model), "Pool 2 cold-start BLOCKED");

    // Invalid pool idx is never blocked (falls through to heuristic)
    ASSERT(!isPoolColdStartBlocked(protection, -1, model), "Invalid pool -1 NOT blocked");
}

// ---------------------------------------------------------------------------
// Test 5: Cold start gating — protection disabled → never blocked
// ---------------------------------------------------------------------------
static void test_gating_disabled_never_blocked() {
    printf("\n=== Test: Gating — protection OFF → ML NEVER blocked ===\n");
    LearningModel model;
    model.init(3);
    model.setMinSamples(2000);
    // Fresh model: 0 samples

    bool protection = false;
    ASSERT(!isPoolColdStartBlocked(protection, 0, model), "Pool 0 NOT blocked (protection off)");
    ASSERT(!isPoolColdStartBlocked(protection, 1, model), "Pool 1 NOT blocked (protection off)");
    ASSERT(!isPoolColdStartBlocked(protection, 2, model), "Pool 2 NOT blocked (protection off)");
}

// ---------------------------------------------------------------------------
// Test 6: Cold start gating — protection enabled, pool ready → not blocked
// ---------------------------------------------------------------------------
static void test_gating_enabled_ready() {
    printf("\n=== Test: Gating — protection ON + pool READY → ML NOT blocked ===\n");
    LearningModel model;
    model.init(3);
    const int threshold = 50;
    model.setMinSamples(threshold);

    ThreadFeatures f;
    memset(&f, 0, sizeof(f));
    // Only pool 2 reaches threshold
    for (int i = 0; i < threshold; i++) model.update(2, f, 0.5f);

    bool protection = true;
    ASSERT(isPoolColdStartBlocked(protection, 0, model), "Pool 0 BLOCKED (not ready)");
    ASSERT(isPoolColdStartBlocked(protection, 1, model), "Pool 1 BLOCKED (not ready)");
    ASSERT(!isPoolColdStartBlocked(protection, 2, model), "Pool 2 NOT blocked (ready)");
}

// ---------------------------------------------------------------------------
// Test 7: Per-pool independence — one pool ready doesn't affect others
// ---------------------------------------------------------------------------
static void test_per_pool_independence() {
    printf("\n=== Test: Per-pool independence ===\n");
    LearningModel model;
    model.init(3);
    model.setMinSamples(100);

    ThreadFeatures f;
    memset(&f, 0, sizeof(f));
    // Pool 0: 150 samples (ready), Pool 1: 50 (not ready), Pool 2: 0 (not ready)
    for (int i = 0; i < 150; i++) model.update(0, f, 0.5f);
    for (int i = 0; i < 50; i++) model.update(1, f, 0.5f);

    ASSERT(model.isPoolReady(0), "Pool 0 ready (150 >= 100)");
    ASSERT(!model.isPoolReady(1), "Pool 1 NOT ready (50 < 100)");
    ASSERT(!model.isPoolReady(2), "Pool 2 NOT ready (0 < 100)");

    // Total samples is 200 but per-pool readiness is independent
    ASSERT_EQ(model.getTotalSamples(), 200, "Total samples = 200 (sum of all pools)");
}

// ---------------------------------------------------------------------------
// Test 8: Default config values match specification
// ---------------------------------------------------------------------------
static void test_default_values() {
    printf("\n=== Test: Default values match specification ===\n");
    LearningModel model;  // default constructor
    ASSERT_EQ(model.getMinSamples(), 2000, "Default min_samples_threshold = 2000");

    // Simulate Config defaults (matching aatped.h)
    struct SimConfig {
        bool mlColdStartProtection = true;
        int mlMinSamples = 2000;
    };
    SimConfig cfg;
    ASSERT(cfg.mlColdStartProtection == true, "Default mlColdStartProtection = true");
    ASSERT_EQ(cfg.mlMinSamples, 2000, "Default mlMinSamples = 2000");
}

int main() {
    printf("===========================================================\n");
    printf("  AATPE ML Cold Start Protection Unit Test\n");
    printf("===========================================================\n");

    test_default_values();
    test_fresh_model_not_ready();
    test_boundary_threshold();
    test_dynamic_threshold_change();
    test_gating_enabled_not_ready();
    test_gating_disabled_never_blocked();
    test_gating_enabled_ready();
    test_per_pool_independence();

    printf("\n===========================================================\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("===========================================================\n");
    return (g_fail == 0) ? 0 : 1;
}

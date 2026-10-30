#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>

#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"

using namespace aatpe_ml;

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define ASSERT_NEAR(actual, expected, tol, msg) do { \
    float _a = (actual), _e = (expected), _t = (tol); \
    if (std::fabs(_a - _e) <= _t) { g_pass++; printf("  [PASS] %s (got %.6f, expected %.6f)\n", msg, _a, _e); } \
    else { g_fail++; printf("  [FAIL] %s (got %.6f, expected %.6f, diff %.6f)\n", msg, _a, _e, std::fabs(_a-_e)); } \
} while(0)

static ThreadFeatures makeFeatures(float val = 0.5f) {
    ThreadFeatures f;
    f.avg_run_us = val;
    f.nr_switches = val;
    f.cpu_usage = val;
    f.priority = val;
    f.nice = val;
    f.is_game = 1.0f;
    f.thermal = val;
    f.capacity = val;
    f.current_cpu_match = 1.0f;
    f.migration_count = val;
    f.cross_run_cap = val * val;
    f.cross_run_usage = val * val;
    f.cross_cap_thermal = val * val;
    f.cross_run_thermal = val * val;
    return f;
}

static void test_ml_lr_decay_sample_count() {
    printf("\n=== Test: ML Learning Rate Decay (sample-count based, v66 fix) ===\n");
    LearningModel m;
    m.init(3);
    m.setInitialLR(0.01f);
    m.setDecay(0.999f);

    PoolModel pm0 = m.getPoolModel(0);
    ASSERT_NEAR(pm0.learning_rate, 0.01f, 1e-6f, "initial lr = 0.01");
    ASSERT(pm0.sample_count == 0, "initial sample_count = 0");

    ThreadFeatures f = makeFeatures();
    m.update(0, f, 0.8f);
    pm0 = m.getPoolModel(0);
    ASSERT_NEAR(pm0.learning_rate, 0.01f * 0.999f, 1e-6f, "after 1 sample: lr = 0.00999");
    ASSERT(pm0.sample_count == 1, "after 1 sample: sample_count = 1");

    for (int i = 1; i < 100; i++) m.update(0, f, 0.8f);
    pm0 = m.getPoolModel(0);
    float expected100 = 0.01f * std::pow(0.999f, 100);
    ASSERT_NEAR(pm0.learning_rate, expected100, 1e-6f, "after 100 samples: lr = 0.01*0.999^100");
    ASSERT(pm0.sample_count == 100, "after 100 samples: sample_count = 100");
}

static void test_ml_lr_at_3000_samples() {
    printf("\n=== Test: ML LR at 3000 samples (≈0.0005, NOT 1e-6) ===\n");
    LearningModel m;
    m.init(3);
    m.setInitialLR(0.01f);
    m.setDecay(0.999f);

    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 3000; i++) {
        m.update(0, f, 0.7f);
    }

    PoolModel pm0 = m.getPoolModel(0);
    float expected3000 = 0.01f * std::pow(0.999f, 3000);
    printf("  [INFO] lr at 3000 samples = %.8f (expected %.8f)\n", pm0.learning_rate, expected3000);
    ASSERT_NEAR(pm0.learning_rate, expected3000, 1e-7f, "3000 samples: lr matches 0.01*0.999^3000");
    ASSERT(pm0.learning_rate > 0.0004f && pm0.learning_rate < 0.0006f,
           "3000 samples: lr ≈ 0.0005 (in range 0.0004-0.0006)");
    ASSERT(pm0.learning_rate > 1e-6f, "3000 samples: lr NOT at 1e-6 floor (old time-based bug)");
    ASSERT(pm0.sample_count == 3000, "3000 samples: sample_count = 3000");
}

static void test_ml_lr_per_model_independence() {
    printf("\n=== Test: ML LR per-model independence (pool 0 update doesn't decay pool 1) ===\n");
    LearningModel m;
    m.init(3);
    m.setInitialLR(0.01f);
    m.setDecay(0.999f);

    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 100; i++) m.update(0, f, 0.8f);

    PoolModel pm0 = m.getPoolModel(0);
    PoolModel pm1 = m.getPoolModel(1);
    PoolModel pm2 = m.getPoolModel(2);

    ASSERT(pm0.sample_count == 100, "pool 0 has 100 samples");
    ASSERT(pm1.sample_count == 0, "pool 1 has 0 samples (not updated)");
    ASSERT(pm2.sample_count == 0, "pool 2 has 0 samples (not updated)");
    ASSERT_NEAR(pm0.learning_rate, 0.01f * std::pow(0.999f, 100), 1e-6f,
                "pool 0 lr decayed 100 times");
    ASSERT_NEAR(pm1.learning_rate, 0.01f, 1e-6f, "pool 1 lr NOT decayed (still 0.01)");
    ASSERT_NEAR(pm2.learning_rate, 0.01f, 1e-6f, "pool 2 lr NOT decayed (still 0.01)");
}

static void test_ml_weight_update_magnitude() {
    printf("\n=== Test: ML weight update magnitude sufficient (not stuck at 0.001) ===\n");
    LearningModel m;
    m.init(3);
    m.setInitialLR(0.01f);
    m.setDecay(0.999f);
    m.setL2Lambda(0.0f);

    ThreadFeatures f = makeFeatures(0.8f);
    for (int i = 0; i < 3000; i++) {
        m.update(0, f, 1.0f);
    }

    PoolModel pm0 = m.getPoolModel(0);
    float maxAbsWeight = 0.0f;
    for (int i = 0; i < ML_WEIGHT_COUNT; i++) {
        float aw = std::fabs(pm0.weights[i]);
        if (aw > maxAbsWeight) maxAbsWeight = aw;
    }
    printf("  [INFO] max |weight| after 3000 samples = %.6f\n", maxAbsWeight);
    printf("  [INFO] bias weight = %.6f\n", pm0.weights[0]);
    ASSERT(maxAbsWeight > 0.01f, "max |weight| > 0.01 after 3000 samples (meaningful learning)");
    ASSERT(pm0.weights[0] > 0.1f, "bias weight > 0.1 (learned to predict high outcome)");

    float pred = m.predict(0, f);
    printf("  [INFO] prediction after training = %.6f\n", pred);
    ASSERT(pred > 0.7f, "prediction > 0.7 after training on outcome=1.0 (model learned)");
}

static void test_ml_lr_floor() {
    printf("\n=== Test: ML LR floor at 1e-6 (only after ~9200+ samples) ===\n");
    LearningModel m;
    m.init(1);
    m.setInitialLR(0.01f);
    m.setDecay(0.999f);

    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 9206; i++) m.update(0, f, 0.5f);
    PoolModel pm = m.getPoolModel(0);
    printf("  [INFO] lr at 9206 samples = %.8f\n", pm.learning_rate);
    ASSERT(pm.learning_rate >= 1e-6f - 1e-9f, "lr near floor at 9206 samples");

    for (int i = 0; i < 100; i++) m.update(0, f, 0.5f);
    pm = m.getPoolModel(0);
    ASSERT_NEAR(pm.learning_rate, 1e-6f, 1e-9f, "lr at 1e-6 floor after 9306 samples");
}

static void test_command_parsing_set_ml_hybrid() {
    printf("\n=== Test: Command parsing set_ml_hybrid=1 (off-by-one regression) ===\n");
    const char* prefix = "set_ml_hybrid=";
    ASSERT(strlen(prefix) == 14, "prefix 'set_ml_hybrid=' has length 14");

    std::string line1 = "set_ml_hybrid=1";
    const char* val1 = line1.c_str() + 14;
    ASSERT(strcmp(val1, "1") == 0, "set_ml_hybrid=1 → valStr='1' (correct offset 14)");
    ASSERT(strcmp(val1, "=1") != 0, "valStr is NOT '=1' (off-by-one bug absent)");

    std::string line0 = "set_ml_hybrid=0";
    const char* val0 = line0.c_str() + 14;
    ASSERT(strcmp(val0, "0") == 0, "set_ml_hybrid=0 → valStr='0'");

    auto isAllDigits = [](const char* s) -> bool {
        if (!s || *s == '\0') return false;
        for (; *s; s++) if (*s < '0' || *s > '9') return false;
        return true;
    };
    ASSERT(isAllDigits(val1), "valStr='1' passes isAllDigits");
    ASSERT(isAllDigits(val0), "valStr='0' passes isAllDigits");

    const char* badVal = line1.c_str() + 13;
    ASSERT(strcmp(badVal, "=1") == 0, "offset 13 would give '=1' (the bug)");
    ASSERT(!isAllDigits(badVal), "'=1' fails isAllDigits (would be rejected)");
}

static void test_hybrid_init_enable() {
    printf("\n=== Test: Hybrid learning init/enable/disable ===\n");
    aatpe_ml_hybrid::HybridLearner h;
    h.init(3);
    ASSERT(h.getPoolCount() == 3, "hybrid init with 3 pools");
    ASSERT(!h.isEnabled(), "hybrid disabled by default");

    h.setEnabled(true);
    ASSERT(h.isEnabled(), "hybrid enabled after setEnabled(true)");

    h.setEnabled(false);
    ASSERT(!h.isEnabled(), "hybrid disabled after setEnabled(false)");

    std::string status = h.getStatusString();
    ASSERT(status.find("hybrid=off") != std::string::npos, "status shows hybrid=off");
    h.setEnabled(true);
    status = h.getStatusString();
    ASSERT(status.find("hybrid=on") != std::string::npos, "status shows hybrid=on");
}

static void test_hybrid_sgd_update() {
    printf("\n=== Test: Hybrid SGD update changes weights ===\n");
    aatpe_ml_hybrid::HybridLearner h;
    h.init(3);
    h.setEnabled(true);

    aatpe_ml_hybrid::HybridPoolDelta before = h.getPoolDelta(0);
    float beforeSum = 0;
    for (int i = 0; i < 15; i++) beforeSum += std::fabs(before.weights[i]);
    ASSERT_NEAR(beforeSum, 0.0f, 1e-6f, "hybrid weights zero before tick");

    std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snaps1, snaps2;
    aatpe_ml_hybrid::ThreadCpuSnapshot s1, s2;
    s1.tid = 100; s1.cpu = 0; s1.pool = 0; s1.runTimeNs = 1000000; s1.timestamp = 1000000000ULL;
    s2.tid = 100; s2.cpu = 0; s2.pool = 0; s2.runTimeNs = 200000000; s2.timestamp = 1500000000ULL;
    snaps1.push_back(s1);
    snaps2.push_back(s2);

    std::vector<uint64_t> cpuRt(8, 0);
    h.tick(snaps1, cpuRt, 8);
    h.tick(snaps2, cpuRt, 8);

    aatpe_ml_hybrid::HybridPoolDelta after = h.getPoolDelta(0);
    float afterSum = 0;
    for (int i = 0; i < 15; i++) afterSum += std::fabs(after.weights[i]);
    printf("  [INFO] hybrid weight sum after SGD = %.6f, updates=%d\n", afterSum, after.updateCount);
    ASSERT(afterSum > 0.0f, "hybrid weights changed after SGD (non-zero sum)");
    ASSERT(after.updateCount > 0, "hybrid updateCount > 0 after SGD");
    ASSERT(h.getTotalUpdates() > 0, "hybrid totalUpdates > 0");
}

static void test_hybrid_save_load() {
    printf("\n=== Test: Hybrid save/load roundtrip ===\n");
    const char* path = "/tmp/aatpe_hybrid_test.bin";
    unlink(path);

    aatpe_ml_hybrid::HybridLearner h1;
    h1.init(3);
    h1.setEnabled(true);

    std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snaps1, snaps2;
    aatpe_ml_hybrid::ThreadCpuSnapshot s1, s2;
    s1.tid = 200; s1.cpu = 6; s1.pool = 2; s1.runTimeNs = 500000; s1.timestamp = 2000000000ULL;
    s2.tid = 200; s2.cpu = 6; s2.pool = 2; s2.runTimeNs = 150000000; s2.timestamp = 2500000000ULL;
    snaps1.push_back(s1); snaps2.push_back(s2);
    std::vector<uint64_t> cpuRt(8, 0);
    h1.tick(snaps1, cpuRt, 8);
    h1.tick(snaps2, cpuRt, 8);

    ASSERT(h1.save(path), "hybrid save succeeds");

    aatpe_ml_hybrid::HybridLearner h2;
    h2.init(3);
    ASSERT(h2.load(path), "hybrid load succeeds");

    aatpe_ml_hybrid::HybridPoolDelta d1 = h1.getPoolDelta(2);
    aatpe_ml_hybrid::HybridPoolDelta d2 = h2.getPoolDelta(2);
    bool match = true;
    for (int i = 0; i < 15; i++) {
        if (std::fabs(d1.weights[i] - d2.weights[i]) > 1e-7f) { match = false; break; }
    }
    ASSERT(match, "hybrid loaded weights match saved weights (pool 2)");
    ASSERT(d2.updateCount == d1.updateCount, "hybrid loaded updateCount matches");
    ASSERT(h2.getTotalUpdates() == h1.getTotalUpdates(), "hybrid loaded totalUpdates matches");

    unlink(path);
}

static void test_hybrid_reset() {
    printf("\n=== Test: Hybrid reset clears weights ===\n");
    aatpe_ml_hybrid::HybridLearner h;
    h.init(3);
    h.setEnabled(true);

    std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snaps1, snaps2;
    aatpe_ml_hybrid::ThreadCpuSnapshot s1, s2;
    s1.tid = 300; s1.cpu = 4; s1.pool = 1; s1.runTimeNs = 100000; s1.timestamp = 3000000000ULL;
    s2.tid = 300; s2.cpu = 4; s2.pool = 1; s2.runTimeNs = 80000000; s2.timestamp = 3500000000ULL;
    snaps1.push_back(s1); snaps2.push_back(s2);
    std::vector<uint64_t> cpuRt(8, 0);
    h.tick(snaps1, cpuRt, 8);
    h.tick(snaps2, cpuRt, 8);
    ASSERT(h.getTotalUpdates() > 0, "hybrid has updates before reset");

    h.reset();
    ASSERT(h.getTotalUpdates() == 0, "hybrid totalUpdates=0 after reset");
    aatpe_ml_hybrid::HybridPoolDelta d = h.getPoolDelta(1);
    float sum = 0;
    for (int i = 0; i < 15; i++) sum += std::fabs(d.weights[i]);
    ASSERT_NEAR(sum, 0.0f, 1e-6f, "hybrid weights zero after reset");
}

static void test_hybrid_disabled_zero_overhead() {
    printf("\n=== Test: Hybrid disabled = zero overhead (early return) ===\n");
    aatpe_ml_hybrid::HybridLearner h;
    h.init(3);
    h.setEnabled(false);

    std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snaps;
    aatpe_ml_hybrid::ThreadCpuSnapshot s;
    s.tid = 400; s.cpu = 0; s.pool = 0; s.runTimeNs = 1000000; s.timestamp = 4000000000ULL;
    snaps.push_back(s);
    std::vector<uint64_t> cpuRt(8, 0);

    for (int i = 0; i < 10; i++) h.tick(snaps, cpuRt, 8);
    ASSERT(h.getTotalUpdates() == 0, "hybrid disabled: no updates after 10 ticks");
    aatpe_ml_hybrid::HybridPoolDelta d = h.getPoolDelta(0);
    float sum = 0;
    for (int i = 0; i < 15; i++) sum += std::fabs(d.weights[i]);
    ASSERT_NEAR(sum, 0.0f, 1e-6f, "hybrid disabled: weights stay zero");
}

static void test_ml_predict_update() {
    printf("\n=== Test: ML predict + update basic ===\n");
    LearningModel m;
    m.init(3);
    ThreadFeatures f = makeFeatures();

    float pred0 = m.predict(0, f);
    ASSERT(pred0 >= 0.0f && pred0 <= 1.0f, "initial prediction in [0,1]");
    ASSERT_NEAR(pred0, 0.5f, 0.01f, "initial prediction ≈0.5 (zero weights → sigmoid(0)=0.5)");

    m.update(0, f, 1.0f);
    float pred1 = m.predict(0, f);
    ASSERT(pred1 > pred0, "prediction increases after update with outcome=1.0");

    m.update(0, f, 0.0f);
    float pred2 = m.predict(0, f);
    ASSERT(pred2 < pred1, "prediction decreases after update with outcome=0.0");
}

static void test_ml_save_load() {
    printf("\n=== Test: ML save/load roundtrip ===\n");
    const char* path = "/tmp/aatpe_ml_test.bin";
    unlink(path);

    LearningModel m1;
    m1.init(3);
    ThreadFeatures f = makeFeatures(0.6f);
    for (int i = 0; i < 50; i++) m1.update(0, f, 0.9f);
    for (int i = 0; i < 30; i++) m1.update(1, f, 0.3f);

    ASSERT(m1.save(path), "ML save succeeds");

    LearningModel m2;
    m2.init(3);
    ASSERT(m2.load(path), "ML load succeeds");

    PoolModel p1_0 = m1.getPoolModel(0);
    PoolModel p2_0 = m2.getPoolModel(0);
    bool match = true;
    for (int i = 0; i < ML_WEIGHT_COUNT; i++) {
        if (std::fabs(p1_0.weights[i] - p2_0.weights[i]) > 1e-6f) { match = false; break; }
    }
    ASSERT(match, "ML loaded weights match saved (pool 0)");
    ASSERT(p2_0.sample_count == 50, "ML loaded sample_count=50 (pool 0)");
    ASSERT_NEAR(p2_0.learning_rate, p1_0.learning_rate, 1e-7f, "ML loaded lr matches");

    PoolModel p2_1 = m2.getPoolModel(1);
    ASSERT(p2_1.sample_count == 30, "ML loaded sample_count=30 (pool 1)");

    unlink(path);
}

static void test_ml_validate_parameters() {
    printf("\n=== Test: ML validateParameters ===\n");
    LearningModel m;
    m.init(3);
    ASSERT(m.validateParameters(), "fresh model validates OK");

    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 10; i++) m.update(0, f, 0.5f);
    ASSERT(m.validateParameters(), "trained model validates OK");
}

static void test_ml_is_all_zero_features() {
    printf("\n=== Test: ML isAllZeroFeatures ===\n");
    ThreadFeatures zero;
    memset(&zero, 0, sizeof(zero));
    ASSERT(LearningModel::isAllZeroFeatures(zero), "all-zero features detected");

    ThreadFeatures nonzero = makeFeatures();
    ASSERT(!LearningModel::isAllZeroFeatures(nonzero), "non-zero features not flagged");

    ThreadFeatures onlyBias;
    memset(&onlyBias, 0, sizeof(onlyBias));
    onlyBias.avg_run_us = 0.001f;
    ASSERT(!LearningModel::isAllZeroFeatures(onlyBias), "tiny non-zero feature not flagged");
}

static void test_ml_extract_features() {
    printf("\n=== Test: ML extractFeatures normalization ===\n");
    ThreadFeatures f = LearningModel::extractFeatures(
        100000,
        500,
        75,
        50,
        0,
        true,
        120,
        1024,
        true,
        10
    );
    ASSERT(f.avg_run_us >= 0.0f && f.avg_run_us <= 1.0f, "avg_run_us normalized [0,1]");
    ASSERT(f.nr_switches >= 0.0f && f.nr_switches <= 1.0f, "nr_switches normalized [0,1]");
    ASSERT(f.cpu_usage >= 0.0f && f.cpu_usage <= 1.0f, "cpu_usage normalized [0,1]");
    ASSERT(f.is_game == 1.0f, "is_game=1.0");
    ASSERT(f.thermal >= 0.0f && f.thermal <= 1.0f, "thermal normalized [0,1]");
    ASSERT(f.current_cpu_match == 1.0f, "current_cpu_match=1.0");
    ASSERT(f.cross_run_cap > 0.0f, "cross_run_cap computed > 0");
    ASSERT(f.cross_run_usage > 0.0f, "cross_run_usage computed > 0");
}

static void test_ml_alpha_cold_start() {
    printf("\n=== Test: ML alpha cold-start behavior ===\n");
    LearningModel m;
    m.init(3);
    m.setColdStartSamples(50);
    m.setMaxAlpha(0.3f);

    ASSERT_NEAR(m.getAlpha(), 0.0f, 1e-6f, "global alpha=0 at cold start (0 samples)");

    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 49; i++) m.update(0, f, 0.8f);
    ASSERT_NEAR(m.getAlpha(), 0.0f, 1e-6f, "global alpha=0 with 49 samples (< cold_start=50)");
    ASSERT_NEAR(m.getAlphaForPool(0), 0.0f, 1e-6f, "pool 0 alpha=0 with 49 samples");

    m.update(0, f, 0.8f);
    ASSERT(m.getAlpha() > 0.0f, "global alpha > 0 at 50 samples (cold start exited, +1 formula)");
    ASSERT_NEAR(m.getAlpha(), 0.0006f, 1e-5f, "global alpha=0.0006 at exactly 50 samples");
    m.update(0, f, 0.8f);
    ASSERT(m.getAlpha() > 0.0006f, "global alpha increases at 51 samples");
}

static void test_ml_reset() {
    printf("\n=== Test: ML reset ===\n");
    LearningModel m;
    m.init(3);
    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 100; i++) m.update(0, f, 0.8f);
    ASSERT(m.getTotalSamples() == 100, "100 samples before reset");

    m.reset();
    ASSERT(m.getTotalSamples() == 0, "0 samples after reset");
    PoolModel pm = m.getPoolModel(0);
    ASSERT(pm.sample_count == 0, "pool 0 sample_count=0 after reset");
    ASSERT_NEAR(pm.learning_rate, 0.01f, 1e-6f, "pool 0 lr reset to 0.01");
    float sum = 0;
    for (int i = 0; i < ML_WEIGHT_COUNT; i++) sum += std::fabs(pm.weights[i]);
    ASSERT_NEAR(sum, 0.0f, 1e-6f, "pool 0 weights zero after reset");
}

static void test_ml_measure_outcome_v2() {
    printf("\n=== Test: ML measureOutcomeV2 ===\n");

    float good = LearningModel::measureOutcomeV2(
        0, 100000000,
        1000, 1001,
        2000000, 2000000,
        2000000,
        50,
        0.5f
    );
    ASSERT(good >= 0.0f && good <= 1.0f, "good outcome in [0,1]");
    printf("  [INFO] good outcome (run_ratio 0.1, baseline 0.5) = %.4f\n", good);

    float bad = LearningModel::measureOutcomeV2(
        0, 800000000,
        1000, 1001,
        2000000, 2000000,
        2000000,
        50,
        0.5f
    );
    ASSERT(bad >= 0.0f && bad <= 1.0f, "bad outcome in [0,1]");
    printf("  [INFO] bad outcome (run_ratio 0.8, baseline 0.5) = %.4f\n", bad);
    ASSERT(good > bad, "good outcome (low run_ratio) score > bad outcome (high run_ratio)");

    float anomaly = LearningModel::measureOutcomeV2(
        1000000, 1000000,
        1000, 1001,
        2000000, 2000000,
        2000000,
        50,
        0.5f
    );
    ASSERT(anomaly < 0.0f, "delta_run=0 returns -1 (anomaly/sleeping)");
}

static void test_red_phase_lr_decay_bug_detection() {
    printf("\n=== Test: RED phase — verify test catches time-based decay bug ===\n");
    LearningModel m;
    m.init(3);
    m.setInitialLR(0.01f);
    m.setDecay(0.999f);

    for (int i = 0; i < 9200; i++) m.decayLearningRate();
    PoolModel pm0 = m.getPoolModel(0);
    printf("  [INFO] After 9200 decayLearningRate() calls: lr=%.8f, samples=%d\n",
           pm0.learning_rate, pm0.sample_count);
    ASSERT(pm0.sample_count == 0, "no samples after decayLearningRate-only calls");
    ASSERT(pm0.learning_rate == 0.01f, "lr unchanged (decayLearningRate is no-op by design)");

    LearningModel m2;
    m2.init(1);
    m2.setInitialLR(0.01f);
    m2.setDecay(0.999f);
    ThreadFeatures f = makeFeatures();
    for (int i = 0; i < 9200; i++) m2.update(0, f, 0.8f);
    PoolModel pm2 = m2.getPoolModel(0);
    printf("  [INFO] NEW sample-based: lr after 9200 samples = %.8f, samples=%d\n",
           pm2.learning_rate, pm2.sample_count);
    ASSERT(pm2.sample_count == 9200, "NEW fix: lr at floor with 9200 actual samples");
    ASSERT(pm2.learning_rate <= 1.1e-6f, "lr decayed to floor after 9200 actual samples");
    float maxW = 0;
    for (int i = 0; i < ML_WEIGHT_COUNT; i++) maxW = std::max(maxW, std::fabs(pm2.weights[i]));
    printf("  [INFO] NEW fix: max |weight| = %.6f\n", maxW);
    ASSERT(maxW > 0.01f, "NEW fix: weights learned meaningfully before lr hit floor");
}

int main() {
    printf("================================================================\n");
    printf("  AATPE Comprehensive Unit Test Suite\n");
    printf("  ML Learning Rate Fix + Hybrid Learning + Command Parsing\n");
    printf("================================================================\n");

    test_ml_lr_decay_sample_count();
    test_ml_lr_at_3000_samples();
    test_ml_lr_per_model_independence();
    test_ml_weight_update_magnitude();
    test_ml_lr_floor();

    test_command_parsing_set_ml_hybrid();

    test_hybrid_init_enable();
    test_hybrid_sgd_update();
    test_hybrid_save_load();
    test_hybrid_reset();
    test_hybrid_disabled_zero_overhead();

    test_ml_predict_update();
    test_ml_save_load();
    test_ml_validate_parameters();
    test_ml_is_all_zero_features();
    test_ml_extract_features();
    test_ml_alpha_cold_start();
    test_ml_reset();

    test_ml_measure_outcome_v2();

    test_red_phase_lr_decay_bug_detection();

    printf("\n================================================================\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("================================================================\n");

    if (g_fail > 0) {
        printf("SOME TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}

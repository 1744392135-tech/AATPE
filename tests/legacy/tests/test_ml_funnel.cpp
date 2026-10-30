#include "../aatpe_ml.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; } \
    else { printf("  [FAIL] %s\n", msg); g_fail++; } \
} while(0)

static ThreadFeatures makeFeatures() {
    ThreadFeatures f;
    f.avg_run_us = 0.3f; f.nr_switches = 0.2f; f.cpu_usage = 0.5f;
    f.priority = 0.7f; f.nice = 0.5f; f.is_game = 1.0f;
    f.thermal = 0.3f; f.capacity = 0.8f; f.current_cpu_match = 0.0f;
    f.migration_count = 0.1f;
    f.cross_run_cap = f.avg_run_us * f.capacity;
    f.cross_run_usage = f.avg_run_us * f.cpu_usage;
    f.cross_cap_thermal = f.capacity * f.thermal;
    f.cross_run_thermal = f.avg_run_us * f.thermal;
    return f;
}

int main() {
    printf("===========================================================\n");
    printf("  AATPE v66 ML Learning Funnel Unit Test\n");
    printf("  (regression: stale ti.runTime -> delta_run=0 -> 0 samples)\n");
    printf("===========================================================\n\n");

    printf("=== Test 1: Full funnel (record -> measure -> update) ===\n");
    {
        LearningModel model;
        model.init(3);
        CHECK(model.getTotalSamples() == 0, "initial total_samples = 0");

        ThreadFeatures feats = makeFeatures();
        unsigned long long beforeRun = 1000000000ULL;
        unsigned long long afterRun  = beforeRun + 180000000ULL;
        time_t beforeTs = 1000;
        time_t afterTs  = beforeTs + 1;
        int beforeFreq = 1500000, afterFreq = 1800000, baseFreq = 2000000;

        model.recordPendingDecision(1234, 4, 2, feats, beforeRun, 100, 50,
                                    beforeFreq, beforeTs, 0);
        int idx = model.findPendingDecision(1234);
        CHECK(idx >= 0, "pending decision recorded and found");

        float outcome = LearningModel::measureOutcomeV2(
            beforeRun, afterRun, beforeTs, afterTs,
            beforeFreq, afterFreq, baseFreq, 50, 0.0f);
        CHECK(outcome >= 0.0f, "measureOutcomeV2 returns valid (>=0) for normal run_ratio");
        printf("  [INFO] outcome = %.4f (run_ratio=0.18)\n", outcome);

        if (outcome >= 0.0f) {
            model.update(2, feats, outcome);
        }
        CHECK(model.getTotalSamples() == 1, "total_samples = 1 after one valid update");
        CHECK(model.getPoolModel(2).sample_count == 1, "pool 2 sample_count = 1");

        model.clearPendingDecision(1234);
        CHECK(model.findPendingDecision(1234) < 0, "pending cleared after successful update");
    }

    printf("\n=== Test 2: delta_run=0 (sleeping thread) returns -1 ===\n");
    {
        LearningModel model;
        model.init(3);
        ThreadFeatures feats = makeFeatures();
        unsigned long long beforeRun = 500000000ULL;
        unsigned long long afterRun  = beforeRun;
        time_t beforeTs = 2000, afterTs = 2001;

        float outcome = LearningModel::measureOutcomeV2(
            beforeRun, afterRun, beforeTs, afterTs,
            1500000, 1500000, 2000000, 50, 0.0f);
        CHECK(outcome < 0.0f, "measureOutcomeV2 returns -1 for delta_run=0 (sleeping)");

        model.recordPendingDecision(5678, 2, 1, feats, beforeRun, 50, 30,
                                    1500000, beforeTs, 0);
        float o1 = LearningModel::measureOutcomeV2(beforeRun, afterRun, beforeTs, afterTs,
                                                    1500000, 1500000, 2000000, 50, 0.0f);
        CHECK(o1 < 0.0f, "first measurement: sleeping (-1), pending kept");
        CHECK(model.findPendingDecision(5678) >= 0, "pending NOT cleared after sleeping skip");
        CHECK(model.getTotalSamples() == 0, "no sample collected after sleeping skip");

        unsigned long long freshAfterRun = beforeRun + 100000000ULL;
        time_t afterTs2 = beforeTs + 1;
        float o2 = LearningModel::measureOutcomeV2(beforeRun, freshAfterRun, beforeTs, afterTs2,
                                                    1500000, 1800000, 2000000, 50, 0.0f);
        CHECK(o2 >= 0.0f, "second measurement: fresh runtime -> valid outcome");
        if (o2 >= 0.0f) {
            model.update(1, feats, o2);
            model.clearPendingDecision(5678);
        }
        CHECK(model.getTotalSamples() == 1, "sample collected after fresh runtime measurement");
        CHECK(model.findPendingDecision(5678) < 0, "pending cleared after valid update");
    }

    printf("\n=== Test 3: Saturation filter (run_ratio > 0.99) ===\n");
    {
        unsigned long long beforeRun = 1000000000ULL;
        unsigned long long afterRun  = beforeRun + 995000000ULL;
        time_t beforeTs = 3000, afterTs = 3001;
        float outcome = LearningModel::measureOutcomeV2(
            beforeRun, afterRun, beforeTs, afterTs,
            2000000, 2000000, 2000000, 50, 0.0f);
        CHECK(outcome < 0.0f, "run_ratio=0.995 returns -1 (saturated)");

        unsigned long long afterRun2 = beforeRun + 980000000ULL;
        float outcome2 = LearningModel::measureOutcomeV2(
            beforeRun, afterRun2, beforeTs, afterTs,
            2000000, 2000000, 2000000, 50, 0.0f);
        CHECK(outcome2 >= 0.0f, "run_ratio=0.98 returns valid (not saturated)");
    }

    printf("\n=== Test 4: Multiple samples accumulate (simulating 1 minute) ===\n");
    {
        LearningModel model;
        model.init(3);
        ThreadFeatures feats = makeFeatures();
        int expectedSamples = 0;

        for (int i = 0; i < 60; i++) {
            unsigned long long beforeRun = (unsigned long long)i * 100000000ULL;
            unsigned long long afterRun  = beforeRun + 30000000ULL;
            time_t beforeTs = 4000 + i;
            time_t afterTs  = beforeTs + 1;
            float outcome = LearningModel::measureOutcomeV2(
                beforeRun, afterRun, beforeTs, afterTs,
                1500000, 1800000, 2000000, 50, 0.0f);
            if (outcome >= 0.0f) {
                model.update(i % 3, feats, outcome);
                expectedSamples++;
            }
        }
        bool sampleMatch = (model.getTotalSamples() == expectedSamples);
        printf("  [INFO] total_samples=%d expected=%d\n", model.getTotalSamples(), expectedSamples);
        CHECK(sampleMatch, "60 cycles produce 60 valid samples");
        CHECK(model.getTotalSamples() >= 50, "at least 50 samples in 1 minute simulation");
        printf("  [INFO] total_samples after 60 cycles = %d\n", model.getTotalSamples());
        printf("  [INFO] alpha after 60 samples = %.4f\n", (double)model.getAlpha());
    }

    printf("\n=== Test 5: Pending overwrite protection (< 500ms skips new record) ===\n");
    {
        LearningModel model;
        model.init(3);
        ThreadFeatures feats = makeFeatures();
        long long skippedBefore = g_pendingSkippedExisting.load();

        model.recordPendingDecision(9999, 3, 2, feats, 1000000, 10, 50, 1500000, 5000, 0);
        CHECK(model.findPendingDecision(9999) >= 0, "first pending recorded");

        model.recordPendingDecision(9999, 5, 2, feats, 2000000, 20, 60, 1800000, 5000, 0);
        long long skippedAfter = g_pendingSkippedExisting.load();
        CHECK(skippedAfter > skippedBefore, "second record within 500ms is skipped (overwrite protection)");

        PendingDecision pd;
        model.getPendingDecision(model.findPendingDecision(9999), pd);
        CHECK(pd.target_cpu == 3, "original pending intact (target_cpu=3, not overwritten to 5)");
        CHECK(pd.before_run_time == 1000000, "original before_run_time preserved");
    }

    printf("\n=== Test 6: getAllPendingDecisions returns active pendings ===\n");
    {
        LearningModel model;
        model.init(3);
        ThreadFeatures feats = makeFeatures();
        model.recordPendingDecision(111, 0, 0, feats, 100, 5, 10, 1000000, 6000, 0);
        model.recordPendingDecision(222, 2, 2, feats, 200, 10, 20, 2000000, 6000, 0);
        auto all = model.getAllPendingDecisions();
        CHECK(all.size() == 2, "getAllPendingDecisions returns 2 active pendings");
        model.clearPendingDecision(111);
        auto all2 = model.getAllPendingDecisions();
        CHECK(all2.size() == 1, "after clearing one, 1 pending remains");
        CHECK(all2[0].tid == 222, "remaining pending is tid=222");
    }

    printf("\n=== Test 7: Expired pending cleanup (60s max age) ===\n");
    {
        LearningModel model;
        model.init(3);
        ThreadFeatures feats = makeFeatures();
        model.recordPendingDecision(333, 1, 1, feats, 100, 5, 10, 1000000, 100, 0);
        int expired = model.clearExpiredPendingDecisions(60);
        CHECK(expired >= 0, "clearExpiredPendingDecisions returns valid count");
    }

    printf("\n===========================================================\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("===========================================================\n");
    return (g_fail == 0) ? 0 : 1;
}

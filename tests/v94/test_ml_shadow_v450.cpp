#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <string>
#include "aatpe_ml.h"
#include "aatped_types.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;

#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static ThreadFeatures makeFeeds(float runUs) {
    ThreadFeatures f;
    f.avg_run_us = runUs;
    f.nr_switches = 10.0f;
    f.cpu_usage = 0.4f;
    f.priority = 0.5f;
    f.nice = 0.0f;
    f.is_game = 1.0f;
    f.thermal = 0.2f;
    f.capacity = 0.7f;
    f.current_cpu_match = 1.0f;
    f.migration_count = 0.0f;
    f.cross_run_cap = 0.28f;
    f.cross_run_usage = 0.16f;
    f.cross_cap_thermal = 0.14f;
    f.cross_run_thermal = 0.08f;
    return f;
}

int main() {
    printf("=== shadow pending tagging ===\n");
    {
        LearningModel m;
        PendingDecision pd;
        CHECK(pd.shadow == false, "PendingDecision.shadow defaults to false (formal)");

        m.recordPendingDecision( 101, 2, 2,
                                makeFeeds(1000.0f), 1000ULL,
 50ULL, 40);
        int idx = m.findPendingDecision(101);
        CHECK(idx >= 0, "pending created for tid=101");
        PendingDecision got;
        CHECK(m.getPendingDecision(idx, got), "getPendingDecision round-trip");
        CHECK(got.shadow == false, "fresh pending is NOT shadow (formal by default)");
        m.markPendingShadow(101);
        CHECK(m.getPendingDecision(idx, got), "re-read pending after markShadow");
        CHECK(got.shadow == true, "markPendingShadow tags pending as shadow");
    }

    printf("=== shadow sample is recorded but does not pollute online model ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);

        int pool = 2;
        long long formalBefore = m.getPoolSampleCount(pool);
        int totalBefore = m.getTotalSamples();
        float wBefore = 0.0f;
        {
            std::vector<float> w = m.getWeights();
            if (!w.empty()) wBefore = w[0];
        }
        long long colBefore = m.getShadowSamplesCollected();

        m.recordShadowOutcome(pool, makeFeeds(2000.0f), 0.62f);

        CHECK(m.getShadowSamplesCollected() == colBefore + 1, "shadow collected counter +1");
        CHECK(m.getShadowRingSize() >= 1, "shadow ring holds the sample");
        CHECK(m.getPoolSampleCount(pool) == formalBefore, "formal sample_count unchanged by shadow");
        CHECK(m.getTotalSamples() == totalBefore, "total_samples unchanged by shadow");
        {
            std::vector<float> w = m.getWeights();
            CHECK(w.empty() || w[0] == wBefore, "live model weight unchanged by shadow");
        }
    }

    printf("=== formal update() still trains (control path unchanged) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        int pool = 1;
        long long before = m.getPoolSampleCount(pool);
        UpdateResult ur = m.update(pool, makeFeeds(500.0f), 0.7f);
        CHECK(ur == UpdateResult::TRAINED, "formal update() returns TRAINED");
        CHECK(m.getPoolSampleCount(pool) == before + 1, "formal sample_count +1 from update()");
    }

    printf("=== shadow ring is bounded EXACTLY at 512 (oldest evicted) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        const size_t kCap = 512;
        const int N = 4000;
        for (int i = 0; i < N; i++) {
            m.recordShadowOutcome(i % 3, makeFeeds(1000.0f + i), 0.5f + 0.01f * (i % 10));
        }
        CHECK(m.getShadowSamplesCollected() >= N, "collected counter is cumulative (>= pushed)");
        CHECK(m.getShadowRingSize() == kCap,
              "shadow ring size == kMaxShadowSamples (512) after 4000 pushes");
        auto drained = m.drainShadowSamples(kCap, false);
        CHECK(drained.size() == kCap, "drain (snapshot) returns exactly 512");
        CHECK(drained[0].features.avg_run_us > 0.0f, "drained[0] features non-empty");
        ThreadFeatures expectedOldest = makeFeeds(1000.0f + (N - (int)kCap));
        auto ffeq = [](const ThreadFeatures& a, const ThreadFeatures& b) {
            return a.avg_run_us == b.avg_run_us && a.nr_switches == b.nr_switches &&
                   a.cpu_usage == b.cpu_usage && a.priority == b.priority &&
                   a.nice == b.nice && a.is_game == b.is_game &&
                   a.thermal == b.thermal && a.capacity == b.capacity &&
                   a.current_cpu_match == b.current_cpu_match &&
                   a.migration_count == b.migration_count &&
                   a.cross_run_cap == b.cross_run_cap &&
                   a.cross_run_usage == b.cross_run_usage &&
                   a.cross_cap_thermal == b.cross_cap_thermal &&
                   a.cross_run_thermal == b.cross_run_thermal;
        };
        CHECK(ffeq(drained[0].features, expectedOldest),
              "oldest ring entry == sample #3488 (FIFO eviction pinned)");
        ThreadFeatures expectedNewest = makeFeeds(1000.0f + (N - 1));
        CHECK(ffeq(drained.back().features, expectedNewest),
              "newest ring entry == sample #3999 (no newer sample dropped)");
    }

    printf("=== drainShadowSamples round-trip (bounded / clear / maxN) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        for (int i = 0; i < 10; i++) {
            m.recordShadowOutcome(i % 3, makeFeeds(2000.0f + i), 0.1f * (float)i);
        }
        CHECK(m.getShadowRingSize() == 10, "precondition: 10 samples in ring");
        auto head = m.drainShadowSamples(3, true);
        CHECK(head.size() == 3, "drain(3) returned exactly 3");
        CHECK(m.getShadowRingSize() == 7, "after drain(3)+clear, 7 remain");
        auto rest = m.drainShadowSamples(100, false);
        CHECK(rest.size() == 7, "snapshot of remaining == 7");
        CHECK(rest[0].features.avg_run_us == makeFeeds(2000.0f + 3).avg_run_us,
              "remaining ring starts at sample #3 (drained prefix removed)");
        auto all = m.drainShadowSamples(512, false);
        CHECK(all.size() == 7, "drain(512) clamps to ring size 7");
        (void)m.drainShadowSamples(512, true);
        CHECK(m.getShadowRingSize() == 0, "drain-all with clear empties the ring");
        auto none = m.drainShadowSamples(512, true);
        CHECK(none.empty(), "drain on empty ring returns empty vector");
    }

    printf("=== shadow switch off => zero collection ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(false);
        CHECK(m.isShadowSamplingEnabled() == false, "isShadowSamplingEnabled reflects off");
        long long colBefore = m.getShadowSamplesCollected();
        size_t ringBefore = m.getShadowRingSize();
        for (int i = 0; i < 50; i++) {
            m.recordShadowOutcome(0, makeFeeds(1000.0f), 0.6f);
        }
        CHECK(m.getShadowSamplesCollected() == colBefore, "switch off: collected counter unchanged");
        CHECK(m.getShadowRingSize() == ringBefore, "switch off: ring unchanged");
    }

    printf("=== invalid shadow outcome rejected (not collected) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        long long colBefore = m.getShadowSamplesCollected();
        long long dropBefore = m.getShadowSamplesDropped();
        m.recordShadowOutcome(0, makeFeeds(1000.0f), -1.0f);
        CHECK(m.getShadowSamplesCollected() == colBefore, "invalid outcome not collected");
        CHECK(m.getShadowSamplesDropped() == dropBefore + 1, "invalid outcome counted as dropped");
    }

    printf("=== shadow pool out-of-range rejected (not clamped) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        long long colBefore = m.getShadowSamplesCollected();
        long long dropBefore = m.getShadowSamplesDropped();
        size_t ringBefore = m.getShadowRingSize();
        m.recordShadowOutcome(-1, makeFeeds(1000.0f), 0.6f);
        m.recordShadowOutcome(99, makeFeeds(1000.0f), 0.6f);
        CHECK(m.getShadowSamplesCollected() == colBefore,
              "out-of-range pool not collected");
        CHECK(m.getShadowSamplesDropped() == dropBefore + 2,
              "out-of-range pool counted as dropped (x2)");
        CHECK(m.getShadowRingSize() == ringBefore, "ring unchanged by rejected pool");
        m.recordShadowOutcome(1, makeFeeds(1000.0f), 0.6f);
        CHECK(m.getShadowSamplesCollected() == colBefore + 1,
              "in-range pool still collected");
    }

    printf("=== shadow has no drift/alpha side effects ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        for (int i = 0; i < 50; i++) {
            m.recordShadowOutcome(2, makeFeeds(1000.0f), 0.1f);
        }
        CHECK(!g_mlDriftDetected.load(), "shadow outcomes do not latch global drift");
        CHECK(m.getAlpha() == 0.0f, "shadow never matures the live model (alpha stays 0)");
    }

    printf("\n=== shadow v450: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

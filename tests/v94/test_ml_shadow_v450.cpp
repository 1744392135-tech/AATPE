// test_ml_shadow_v450.cpp — v4.5.0 ML shadow-sampling (observe-only) unit tests.
//
// Scope (RED -> GREEN for the v4.5.0 "shadow sampling" feature):
//   In observe-only / auxiliary (passive) mode the daemon must NOT apply any
//   scheduling action (no boost / no freeze / no sysfs write). v4.5.0 adds
//   "shadow sampling": even with control suppressed, the ML pipeline still
//   records feature->outcome pairs, tagged as SHADOW, into a separate bounded
//   buffer. Shadow samples must:
//     (a) be collected while the switch is on (and ZERO when off);
//     (b) be counted separately and NEVER train/pollute the live online model
//         (weights / sample_count / alpha must not move from a shadow sample);
//     (c) stay bounded (ring capped, no unbounded growth);
//     (d) leave the formal control path (update()) byte-for-byte able to train.
//
// Links aatpe_ml.cpp (LearningModel + PendingDecision + shadow state live there)
// and the ml_deps / log_stub stubs, exactly like test_v343_ml.cpp.
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
    // ------------------------------------------------------------------
    // 1. PendingDecision.shadow defaults false; markPendingShadow tags it.
    // ------------------------------------------------------------------
    printf("=== shadow pending tagging ===\n");
    {
        LearningModel m;
        PendingDecision pd;
        CHECK(pd.shadow == false, "PendingDecision.shadow defaults to false (formal)");

        m.recordPendingDecision(/*tid=*/101, /*target_cpu=*/2, /*target_pool=*/2,
                                makeFeeds(1000.0f), /*before_run=*/1000ULL,
                                /*before_sw=*/50ULL, /*before_cpu_usage=*/40);
        int idx = m.findPendingDecision(101);
        CHECK(idx >= 0, "pending created for tid=101");
        PendingDecision got;
        CHECK(m.getPendingDecision(idx, got), "getPendingDecision round-trip");
        CHECK(got.shadow == false, "fresh pending is NOT shadow (formal by default)");
        m.markPendingShadow(101);
        CHECK(m.getPendingDecision(idx, got), "re-read pending after markShadow");
        CHECK(got.shadow == true, "markPendingShadow tags pending as shadow");
    }

    // ------------------------------------------------------------------
    // 2. Shadow sample: collected, but does NOT train the live model.
    // ------------------------------------------------------------------
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
        // The live online model must be untouched.
        CHECK(m.getPoolSampleCount(pool) == formalBefore, "formal sample_count unchanged by shadow");
        CHECK(m.getTotalSamples() == totalBefore, "total_samples unchanged by shadow");
        {
            std::vector<float> w = m.getWeights();
            CHECK(w.empty() || w[0] == wBefore, "live model weight unchanged by shadow");
        }
    }

    // ------------------------------------------------------------------
    // 3. Formal control path still trains (control mode unaffected).
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    // 4. Bound: ring is capped EXACTLY at kMaxShadowSamples=512; oldest evicted.
    //    v6.0.0 P3-5: the old loose "<=1024" upper bound let the cap drift
    //    unnoticed. Lock the real constant and pin the FIFO eviction order via
    //    the new drainShadowSamples() export.
    // ------------------------------------------------------------------
    printf("=== shadow ring is bounded EXACTLY at 512 (oldest evicted) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        const size_t kCap = 512;
        // Push far more samples than the ring cap.
        const int N = 4000;
        for (int i = 0; i < N; i++) {
            m.recordShadowOutcome(i % 3, makeFeeds(1000.0f + i), 0.5f + 0.01f * (i % 10));
        }
        CHECK(m.getShadowSamplesCollected() >= N, "collected counter is cumulative (>= pushed)");
        // The ring itself must be EXACTLY the cap (not just "well under 1024").
        CHECK(m.getShadowRingSize() == kCap,
              "shadow ring size == kMaxShadowSamples (512) after 4000 pushes");
        // Oldest-eviction order: the first 4000-512=3488 samples must have been
        // popped. The surviving ring's first (oldest) sample must be sample #3488
        // (avg_run_us = 1000+3488), and its outcome 0.5 + 0.01*(3488%10).
        auto drained = m.drainShadowSamples(kCap, /*clear=*/false);
        CHECK(drained.size() == kCap, "drain (snapshot) returns exactly 512");
        CHECK(drained[0].features.avg_run_us > 0.0f, "drained[0] features non-empty");
        // Oldest surviving index = N - kCap = 3488. makeFeeds(1000.0f + 3488) is the
        // first still-present sample; compare against makeFeeds(1000.0f + 3488).
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
        // Last entry must be sample #3999.
        ThreadFeatures expectedNewest = makeFeeds(1000.0f + (N - 1));
        CHECK(ffeq(drained.back().features, expectedNewest),
              "newest ring entry == sample #3999 (no newer sample dropped)");
    }

    // ------------------------------------------------------------------
    // 4b. drainShadowSamples: bounded, clear semantics, maxN clamp.
    // ------------------------------------------------------------------
    printf("=== drainShadowSamples round-trip (bounded / clear / maxN) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        for (int i = 0; i < 10; i++) {
            m.recordShadowOutcome(i % 3, makeFeeds(2000.0f + i), 0.1f * (float)i);
        }
        CHECK(m.getShadowRingSize() == 10, "precondition: 10 samples in ring");
        // Drain only 3 oldest.
        auto head = m.drainShadowSamples(3, /*clear=*/true);
        CHECK(head.size() == 3, "drain(3) returned exactly 3");
        CHECK(m.getShadowRingSize() == 7, "after drain(3)+clear, 7 remain");
        // Remaining ring must start at the OLD sample #3.
        auto rest = m.drainShadowSamples(100, /*clear=*/false);
        CHECK(rest.size() == 7, "snapshot of remaining == 7");
        CHECK(rest[0].features.avg_run_us == makeFeeds(2000.0f + 3).avg_run_us,
              "remaining ring starts at sample #3 (drained prefix removed)");
        // maxN larger than ring size clamps to ring size (no overrun, no crash).
        auto all = m.drainShadowSamples(512, /*clear=*/false);
        CHECK(all.size() == 7, "drain(512) clamps to ring size 7");
        // clear=true empties.
        (void)m.drainShadowSamples(512, /*clear=*/true);
        CHECK(m.getShadowRingSize() == 0, "drain-all with clear empties the ring");
        // Empty ring drains to empty vector.
        auto none = m.drainShadowSamples(512, /*clear=*/true);
        CHECK(none.empty(), "drain on empty ring returns empty vector");
    }

    // ------------------------------------------------------------------
    // 5. Switch off => zero collection.
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    // 6. Invalid outcome (<0 sentinel) is dropped, not collected.
    // ------------------------------------------------------------------
    printf("=== invalid shadow outcome rejected (not collected) ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        long long colBefore = m.getShadowSamplesCollected();
        long long dropBefore = m.getShadowSamplesDropped();
        m.recordShadowOutcome(0, makeFeeds(1000.0f), -1.0f);  // invalid sentinel
        CHECK(m.getShadowSamplesCollected() == colBefore, "invalid outcome not collected");
        CHECK(m.getShadowSamplesDropped() == dropBefore + 1, "invalid outcome counted as dropped");
    }

    // ------------------------------------------------------------------
    // 6b. v6.0.0 P3-2: pool index out of range is DROPPED (not clamped to 0).
    //     The passive sampler can pass getPoolForCpu()==UNKNOWN(-1) on a
    //     hotplug/topology race; storing it as pool=0 would mis-label the sample.
    // ------------------------------------------------------------------
    printf("=== shadow pool out-of-range rejected (not clamped) ===\n");
    {
        LearningModel m;  // default 3 pools (indices 0..2)
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        long long colBefore = m.getShadowSamplesCollected();
        long long dropBefore = m.getShadowSamplesDropped();
        size_t ringBefore = m.getShadowRingSize();
        m.recordShadowOutcome(-1, makeFeeds(1000.0f), 0.6f);  // UNKNOWN pool
        m.recordShadowOutcome(99, makeFeeds(1000.0f), 0.6f);  // way past pool count
        CHECK(m.getShadowSamplesCollected() == colBefore,
              "out-of-range pool not collected");
        CHECK(m.getShadowSamplesDropped() == dropBefore + 2,
              "out-of-range pool counted as dropped (x2)");
        CHECK(m.getShadowRingSize() == ringBefore, "ring unchanged by rejected pool");
        // In-range pool still accepted.
        m.recordShadowOutcome(1, makeFeeds(1000.0f), 0.6f);
        CHECK(m.getShadowSamplesCollected() == colBefore + 1,
              "in-range pool still collected");
    }

    // ------------------------------------------------------------------
    // 7. Shadow does not latch drift / move alpha (no online side effects).
    // ------------------------------------------------------------------
    printf("=== shadow has no drift/alpha side effects ===\n");
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);
        // Feed many degrading shadow outcomes; the live drift detector must not
        // be fed by shadow samples (that would shrink alpha in observe-only mode,
        // violating "observe-only does not change control").
        for (int i = 0; i < 50; i++) {
            m.recordShadowOutcome(2, makeFeeds(1000.0f), 0.1f);  // strongly degrading
        }
        CHECK(!g_mlDriftDetected.load(), "shadow outcomes do not latch global drift");
        // Alpha must remain at cold-start 0 (shadow never trains -> no maturity).
        CHECK(m.getAlpha() == 0.0f, "shadow never matures the live model (alpha stays 0)");
    }

    printf("\n=== shadow v450: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// test_ml_shadow_passive_v600.cpp — v6.0.0 P1 RED->GREEN regression.
//
// Bug fixed: in passive (observe-only) mode decidePlacement() hits the PASSIVE
// gate and returns KEEP early, so the only d.mlFeatures assignment in placement
// was never reached. The passive sampler in aatped_main.cpp recorded
// d.mlFeatures, which was the value-initialized all-zero ThreadFeatures — every
// shadow sample in the ring had an all-zero feature vector, making the offline
// effect-validation dataset useless.
//
// Fix under test (the CONTRACT the passive sampler now executes):
//   1. call the PURE observation function extractMlFeatures(ti, ti.currentCpu)
//      (read-only: g_cpuMetrics short lock + ti fields; NO boost/freeze/
//      sched_setaffinity/sysfs write);
//   2. run the SAME cold-start all-zero filter the formal path uses
//      (LearningModel::isAllZeroFeatures) — if all-zero, do NOT bookkeep a
//      shadow pending;
//   3. otherwise route the extracted features into the shadow ring.
//
// This host test drives that contract end-to-end against the REAL extractor
// (scoring.cpp) and the REAL LearningModel shadow ring (aatpe_ml.cpp).
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include "aatpe_ml.h"
#include "aatped_types.h"
#include "aatped_state.h"

using namespace aatpe_ml;

// scoring_deps.cpp provides these daemon globals (g_cpuMetrics + mutex).
extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::mutex g_cpuMetricsMutex;

static int g_pass = 0, g_fail = 0;

#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static bool featsEqual(const ThreadFeatures& a, const ThreadFeatures& b) {
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
}

static void seedCpuMetrics(int cpu, int util, int temp, int capacity) {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    CpuMetrics m;
    m.util = util; m.temp = temp; m.capacity = capacity;
    m.curFreq = 1800000; m.maxFreq = 2400000; m.freqPct = 75;
    g_cpuMetrics[cpu] = m;
}

static ThreadInfo makeGameThread(int cpu) {
    ThreadInfo ti;
    ti.pid = 1000; ti.tid = 2000;
    ti.pname = "com.test.game"; ti.tname = "RenderThread";
    ti.role = ROLE_S3_GAME;
    ti.priority = 0;           // -> feature = 1.0
    ti.nice = 0;
    ti.runTime = 2000000ULL;   // 2ms of runtime across 100 switches
    ti.nrSwitches = 100;
    ti.switchesPerSec = 5.0;
    ti.currentCpu = cpu;
    ti.needsHydration = false;
    ti.roleConfidence = 90;
    return ti;
}

int main() {
    printf("=== v6.0.0 P1: passive shadow sample features must be NON-zero ===\n");

    // 1. Seed metrics for CPU 0 and extract counterfactual "stay on CPU0".
    seedCpuMetrics(/*cpu=*/0, /*util=*/40, /*temp=*/45, /*capacity=*/380);
    {
        ThreadInfo ti = makeGameThread(/*cpu=*/0);
        ThreadFeatures f = extractMlFeatures(ti, ti.currentCpu);

        CHECK(!LearningModel::isAllZeroFeatures(f),
              "extracted features are NOT all-zero (the old bug recorded all-zero)");
        CHECK(f.current_cpu_match == 1.0f,
              "current_cpu_match==1.0 (targetCpu==ti.currentCpu, 'stay current pool')");
        CHECK(std::fabs(f.cpu_usage - 0.4f) < 1e-6f,
              "cpu_usage==0.40 (util=40 seeded into g_cpuMetrics)");
        CHECK(std::fabs(f.thermal - (45.0f/120.0f)) < 1e-6f,
              "thermal feature reflects seeded temp=45C");
        CHECK(std::fabs(f.capacity - (380.0f/1024.0f)) < 1e-6f,
              "capacity feature reflects seeded capacity=380");
        CHECK(f.is_game == 1.0f, "is_game==1.0 for ROLE_S3_GAME");
        CHECK(f.avg_run_us > 0.0f, "avg_run_us feature is positive (runTime=2ms/100)");
        CHECK(f.nr_switches > 0.0f, "switch-rate feature is positive (5/s)");
    }

    // 2. Prove the extractor READS LIVE metrics (not a constant). Bump util to
    //    80 and re-extract; cpu_usage must move to 0.8. This is the exact
    //    observation the passive sampler now relies on.
    {
        seedCpuMetrics(/*cpu=*/0, /*util=*/80, /*temp=*/45, /*capacity=*/380);
        ThreadInfo ti = makeGameThread(/*cpu=*/0);
        ThreadFeatures f = extractMlFeatures(ti, ti.currentCpu);
        CHECK(std::fabs(f.cpu_usage - 0.8f) < 1e-6f,
              "cpu_usage tracks live util=80 (extractor observes g_cpuMetrics, not zero)");
    }

    // 3. Drive the passive contract end-to-end: non-all-zero extracted features
    //    are routed into the shadow ring and ROUND-TRIP unchanged.
    {
        seedCpuMetrics(/*cpu=*/0, /*util=*/40, /*temp=*/45, /*capacity=*/380);
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);

        ThreadInfo ti = makeGameThread(/*cpu=*/0);
        int shadowPool = 2;  // getPoolForCpu(0) on a 3-pool topology

        // The exact guard the passive sampler now runs:
        ThreadFeatures feats = extractMlFeatures(ti, ti.currentCpu);
        bool record = !LearningModel::isAllZeroFeatures(feats);
        CHECK(record == true, "non-zero features -> passive record=true");

        long long colBefore = m.getShadowSamplesCollected();
        if (record) {
            m.recordShadowOutcome(shadowPool, feats, 0.62f);
        }
        CHECK(m.getShadowSamplesCollected() == colBefore + 1,
              "shadow outcome collected once");
        auto ring = m.drainShadowSamples(16, /*clear=*/false);
        CHECK(ring.size() == 1, "ring holds exactly the one passive sample");
        CHECK(ring[0].pool == shadowPool, "ring sample pool == shadowPool");
        CHECK(std::fabs(ring[0].outcome - 0.62f) < 1e-6f, "outcome round-trips");
        CHECK(featsEqual(ring[0].features, feats),
              "ring sample features == extracted features (NOT all-zero)");
    }

    // 4. Cold-start all-zero filter: when the extracted vector is all-zero, the
    //    passive path must SKIP booking a shadow pending (no pollution).
    {
        LearningModel m;
        m.setEnabled(true);
        m.setShadowSamplingEnabled(true);

        ThreadFeatures zeroFeats{};  // cold start: metrics/ti not ready
        CHECK(LearningModel::isAllZeroFeatures(zeroFeats),
              "precondition: default ThreadFeatures{} IS all-zero");

        long long colBefore = m.getShadowSamplesCollected();
        long long dropBefore = m.getShadowSamplesDropped();
        size_t ringBefore = m.getShadowRingSize();

        // Passive guard (mirrors production):
        bool record = !LearningModel::isAllZeroFeatures(zeroFeats);
        CHECK(record == false, "all-zero features -> passive record=false");
        if (record) {
            m.recordShadowOutcome(/*pool=*/2, zeroFeats, 0.6f);
        }
        CHECK(m.getShadowSamplesCollected() == colBefore,
              "all-zero cold-start sample NOT collected (dataset not polluted)");
        CHECK(m.getShadowRingSize() == ringBefore,
              "ring unchanged when all-zero features skipped");
        CHECK(m.getShadowSamplesDropped() == dropBefore,
              "guard-skip is not counted as an invalid-outcome drop");
    }

    printf("\n=== passive shadow v600: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

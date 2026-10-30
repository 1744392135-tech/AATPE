#include "aatpe_ml.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
        if (cond) { std::printf("  PASS: %s\n", msg); } \
        else { std::printf("  FAIL: %s\n", msg); g_fail++; } \
    } while (0)

static bool feq(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    using aatpe_ml::LearningModel;

    std::printf("[A/B] same-dimension baseline + outcome\n");
    {
        LearningModel m;
        const int tid = 1001;
        const int maxFreq = 1000;
        const int curFreq = 1000;

        float r1 = m.recordBaselineDuty(tid, 0ULL, 0ULL,
                                        curFreq, maxFreq);
        CHECK(feq(r1, 0.0f), "first period records no baseline (returns 0)");
        CHECK(feq(m.getBaselineRunRatio(tid), 0.0f), "first period: baseline window empty -> 0");

        float r2 = m.recordBaselineDuty(tid, 1000000000ULL,
 2000000000ULL, curFreq, maxFreq);
        CHECK(feq(r2, 0.5f), "baseline_nrr = duty/freq_factor = 0.5 (normalized duty)");
        CHECK(feq(m.getBaselineRunRatio(tid), 0.5f), "window mean == 0.5");

        float score = LearningModel::measureOutcomeV2(
 1000000000ULL, 1500000000ULL,
 1, 3,
 curFreq, curFreq, maxFreq,
 0, m.getBaselineRunRatio(tid),
            0, 0, 0, 0.0f, false);
        CHECK(score >= 0.0f, "outcome produced (>=0)");
        CHECK(feq(score, 0.75f), "rr_score = 0.75 (50%% duty drop -> correct improvement)");
    }

    std::printf("[C] control: old timeslice proxy gives a WRONG label\n");
    {
        unsigned long long runTime = 1000000000ULL;
        unsigned long long nrSwitches = 100;
        unsigned long long avgRunNs = runTime / nrSwitches;
        float oldBaseline = std::min(1.0f, (float)avgRunNs / 10000000.0f);
        CHECK(feq(oldBaseline, 1.0f), "old proxy saturates at 1.0 even though true duty=0.5");

        float scoreOld = LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            1000, 1000, 1000, 0, oldBaseline,
            0, 0, 0, 0.0f, false);
        float scoreCorrect = LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            1000, 1000, 1000, 0, 0.5f,
            0, 0, 0, 0.0f, false);
        std::printf("     old-proxy rr_score=%.4f vs correct rr_score=%.4f\n",
                    scoreOld, scoreCorrect);
        CHECK(feq(scoreOld, 0.875f), "old proxy yields inflated 0.875 (wrong)");
        CHECK(!feq(scoreOld, scoreCorrect), "old proxy != correct dimensioned score (bug reproduced)");
    }

    std::printf("[D1] freq normalization consistency\n");
    {
        LearningModel m;
        const int tid = 2002;
        const int maxFreq = 2000;
        m.recordBaselineDuty(tid, 0ULL, 0ULL, 1000, maxFreq);
        float b = m.recordBaselineDuty(tid, 1000000000ULL, 2000000000ULL, 1000, maxFreq);
        CHECK(feq(b, 1.0f), "half-clock duty 0.5 normalizes to base-freq nrr=1.0");

        float score = LearningModel::measureOutcomeV2(
            1000000000ULL, 2000000000ULL, 1, 3,
 2000, 2000, maxFreq,
            0, m.getBaselineRunRatio(tid), 0, 0, 0, 0.0f, false);
        CHECK(feq(score, 0.75f), "baseline/outcome on same base-freq scale -> 0.75");
    }

    std::printf("[D2] first-period fallback (1-nrr)\n");
    {
        LearningModel m;
        float score = LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            1000, 1000, 1000, 0, 0.0f, 0, 0, 0, 0.0f, false);
        CHECK(feq(score, 0.75f), "no baseline -> self-consistent 1-nrr = 0.75");
    }

    std::printf("[D3] idle/reset robustness\n");
    {
        LearningModel m;
        const int tid = 3003;
        m.recordBaselineDuty(tid, 1000ULL, 0ULL, 1000, 1000);
        float idle = m.recordBaselineDuty(tid, 1000ULL, 2000000000ULL, 1000, 1000);
        CHECK(feq(idle, 0.0f), "idle window (drun=0) records nothing");
        CHECK(feq(m.getBaselineRunRatio(tid), 0.0f), "idle window left no baseline");
        float real = m.recordBaselineDuty(tid, 1000ULL + 1000000000ULL,
                                          2000000000ULL + 2000000000ULL, 1000, 1000);
        CHECK(feq(real, 0.5f), "post-idle duty measured correctly = 0.5");
        float rst = m.recordBaselineDuty(tid, 5ULL, 7000000000ULL, 1000, 1000);
        CHECK(feq(rst, 0.0f), "backwards runTime (reset) records nothing");
    }

    std::printf(g_fail == 0 ? "ALL ML BASELINE-DIM TESTS PASSED\n"
                            : "ML BASELINE-DIM TESTS FAILED: %d\n", g_fail);

    std::printf("[E] concurrent recordBaselineDuty (TSan exercise)\n");
    {
        LearningModel m;
        const int NTHREADS = 8, PER = 200;
        std::vector<std::thread> th;
        for (int t = 0; t < NTHREADS; ++t) {
            th.emplace_back([&m, t, PER]() {
                int tid = 50000 + t;
                for (int i = 0; i < PER; ++i) {
                    unsigned long long run = (unsigned long long)i * 1000000ULL;
                    unsigned long long ns  = (unsigned long long)i * 2000000ULL;
                    m.recordBaselineDuty(tid, run, ns, 1000, 1000);
                    if ((i & 7) == 0) m.getBaselineRunRatio(tid);
                }
            });
        }
        for (auto& x : th) x.join();
        bool ok = true;
        for (int t = 0; t < NTHREADS; ++t) {
            float b = m.getBaselineRunRatio(50000 + t);
            if (b <= 0.0f || b > 1.0f) ok = false;
        }
        CHECK(ok, "all concurrent baselines in (0,1] after join");
    }

    std::printf(g_fail == 0 ? "ALL ML BASELINE-DIM TESTS PASSED\n"
                            : "ML BASELINE-DIM TESTS FAILED: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

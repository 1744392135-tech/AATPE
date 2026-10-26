// =============================================================================
// F1.1 regression — ML baseline / outcome DIMENSION alignment (v4.3.8)
//
// Guards the fix for fresh-eyes audit F1.1: the old baseline was
//   avgRunNs = runTime/nrSwitches;  run_ratio = avgRunNs/10ms
// which is a TIMESLICE proxy (mean scheduler-run length / 10ms), while the
// outcome side computes
//   nrr = (delta_run/delta_time) / freq_factor
// which is a normalized DUTY CYCLE. Subtracting them to derive "improvement"
// was dimensionally invalid whenever a baseline existed.
//
// This test:
//   A) feeds recordBaselineDuty() a KNOWN schedstat increment and asserts the
//      recorded baseline_nrr is a normalized duty cycle in [0,1] — the SAME
//      dimension as measureOutcomeV2's nrr.
//   B) feeds a matching outcome window and asserts the resulting rr_score is
//      the mathematically correct relative improvement.
//   C) proves the OLD timeslice proxy would have produced a DIFFERENT, WRONG
//      improvement (control case demonstrating the bug the fix removes).
//   D) first-period fallback (no baseline -> 1-nrr), freq normalization, and
//      idle/reset robustness.
//
// Hermetic: links only the real aatpe_ml TU + the ml_deps host stub. No /proc.
// =============================================================================
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

    // ---------------------------------------------------------------------
    // A + B: known schedstat increment -> baseline_nrr is a duty cycle, and
    // the improvement scored against it is dimensionally correct.
    // ---------------------------------------------------------------------
    std::printf("[A/B] same-dimension baseline + outcome\n");
    {
        LearningModel m;
        const int tid = 1001;
        const int maxFreq = 1000;   // kHz, base frequency
        const int curFreq = 1000;   // running at base -> freq_factor = 1.0

        // Period 1 (first observation): latch reference, must NOT record.
        float r1 = m.recordBaselineDuty(tid, /*runTime=*/0ULL, /*nowNs=*/0ULL,
                                        curFreq, maxFreq);
        CHECK(feq(r1, 0.0f), "first period records no baseline (returns 0)");
        CHECK(feq(m.getBaselineRunRatio(tid), 0.0f), "first period: baseline window empty -> 0");

        // Period 2: over a 2s window the thread consumed 1s of CPU runtime.
        //   duty = 1e9 / 2e9 = 0.5 ; freq_factor = 1.0 ; baseline_nrr = 0.5.
        float r2 = m.recordBaselineDuty(tid, /*runTime=*/1000000000ULL,
                                        /*nowNs=*/2000000000ULL, curFreq, maxFreq);
        CHECK(feq(r2, 0.5f), "baseline_nrr = duty/freq_factor = 0.5 (normalized duty)");
        CHECK(feq(m.getBaselineRunRatio(tid), 0.5f), "window mean == 0.5");

        // Outcome window: after migration, over a 2s wall window the thread
        // consumed 0.5s of runtime. nrr = (0.5e9/2e9)/1.0 = 0.25.
        //   baseline = 0.5, nrr = 0.25 -> imp = (0.5-0.25)/0.5 = 0.5
        //   rr_score = 0.5 + 0.5*0.5 = 0.75  (duty halved -> clearly improved)
        float score = LearningModel::measureOutcomeV2(
            /*before_run_time=*/1000000000ULL, /*after_run_time=*/1500000000ULL,
            /*before_ts=*/1, /*after_ts=*/3,
            /*before_freq=*/curFreq, /*after_freq=*/curFreq, /*base_freq=*/maxFreq,
            /*loadInt=*/0, /*baseline_run_ratio=*/m.getBaselineRunRatio(tid),
            0, 0, 0, 0.0f, false);
        CHECK(score >= 0.0f, "outcome produced (>=0)");
        CHECK(feq(score, 0.75f), "rr_score = 0.75 (50%% duty drop -> correct improvement)");
    }

    // ---------------------------------------------------------------------
    // C: CONTROL — the OLD timeslice proxy would miscompute the improvement.
    // ---------------------------------------------------------------------
    std::printf("[C] control: old timeslice proxy gives a WRONG label\n");
    {
        // Same physical window as [A/B]: runTime=1e9 consumed, nrSwitches=100.
        // Old proxy: avgRunNs = runTime/nrSwitches = 1e7 ns = 10ms;
        //            run_ratio = min(1, 10ms/10ms) = 1.0.
        unsigned long long runTime = 1000000000ULL;
        unsigned long long nrSwitches = 100;
        unsigned long long avgRunNs = runTime / nrSwitches;      // 1e7 ns = 10ms
        float oldBaseline = std::min(1.0f, (float)avgRunNs / 10000000.0f); // = 1.0
        CHECK(feq(oldBaseline, 1.0f), "old proxy saturates at 1.0 even though true duty=0.5");

        // Feed that wrong baseline into the SAME outcome (nrr=0.25).
        float scoreOld = LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            1000, 1000, 1000, 0, /*baseline=*/oldBaseline,
            0, 0, 0, 0.0f, false);
        // Correct improvement (true baseline 0.5) = 0.75.
        float scoreCorrect = LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            1000, 1000, 1000, 0, /*baseline=*/0.5f,
            0, 0, 0, 0.0f, false);
        std::printf("     old-proxy rr_score=%.4f vs correct rr_score=%.4f\n",
                    scoreOld, scoreCorrect);
        // old proxy: imp=(1.0-0.25)/1.0=0.75 -> rr_score=0.875. Inflated.
        CHECK(feq(scoreOld, 0.875f), "old proxy yields inflated 0.875 (wrong)");
        CHECK(!feq(scoreOld, scoreCorrect), "old proxy != correct dimensioned score (bug reproduced)");
    }

    // ---------------------------------------------------------------------
    // D1: freq normalization — baseline and outcome both normalize to base freq.
    // ---------------------------------------------------------------------
    std::printf("[D1] freq normalization consistency\n");
    {
        LearningModel m;
        const int tid = 2002;
        const int maxFreq = 2000;
        // Baseline observed while running at HALF clock (curFreq=1000).
        // duty=0.5 over 2s -> nrr = 0.5 / (1000/2000=0.5) = 1.0.
        m.recordBaselineDuty(tid, 0ULL, 0ULL, /*curFreq=*/1000, maxFreq);           // latch
        float b = m.recordBaselineDuty(tid, 1000000000ULL, 2000000000ULL, 1000, maxFreq);
        CHECK(feq(b, 1.0f), "half-clock duty 0.5 normalizes to base-freq nrr=1.0");

        // Outcome measured at BASE clock (curFreq=2000 -> factor 1.0): duty=0.5
        // (ran 1s of the 2s window). nrr=0.5. baseline=1.0 ->
        // imp=(1.0-0.5)/1.0=0.5 -> rr_score=0.75.
        float score = LearningModel::measureOutcomeV2(
            1000000000ULL, 2000000000ULL, 1, 3,
            /*before_freq=*/2000, /*after_freq=*/2000, /*base_freq=*/maxFreq,
            0, m.getBaselineRunRatio(tid), 0, 0, 0, 0.0f, false);
        CHECK(feq(score, 0.75f), "baseline/outcome on same base-freq scale -> 0.75");
    }

    // ---------------------------------------------------------------------
    // D2: first-period fallback — no baseline -> rr_score = 1 - nrr.
    // ---------------------------------------------------------------------
    std::printf("[D2] first-period fallback (1-nrr)\n");
    {
        LearningModel m;
        // No recordBaselineDuty calls -> baseline 0. Outcome nrr=0.25.
        // Fallback path: rr_score = 1 - 0.25 = 0.75.
        float score = LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            1000, 1000, 1000, 0, /*baseline=*/0.0f, 0, 0, 0, 0.0f, false);
        CHECK(feq(score, 0.75f), "no baseline -> self-consistent 1-nrr = 0.75");
    }

    // ---------------------------------------------------------------------
    // D3: idle window (drun==0) and counter reset do NOT corrupt the baseline.
    // ---------------------------------------------------------------------
    std::printf("[D3] idle/reset robustness\n");
    {
        LearningModel m;
        const int tid = 3003;
        // latch
        m.recordBaselineDuty(tid, 1000ULL, 0ULL, 1000, 1000);
        // idle window: runTime unchanged, 2s elapsed -> drun==0, must skip.
        float idle = m.recordBaselineDuty(tid, 1000ULL, 2000000000ULL, 1000, 1000);
        CHECK(feq(idle, 0.0f), "idle window (drun=0) records nothing");
        CHECK(feq(m.getBaselineRunRatio(tid), 0.0f), "idle window left no baseline");
        // Now a real window from the refreshed reference: ran 1s of 2s.
        float real = m.recordBaselineDuty(tid, 1000ULL + 1000000000ULL,
                                          2000000000ULL + 2000000000ULL, 1000, 1000);
        CHECK(feq(real, 0.5f), "post-idle duty measured correctly = 0.5");
        // Counter reset (runTime backwards): must skip, not record garbage.
        float rst = m.recordBaselineDuty(tid, 5ULL, 7000000000ULL, 1000, 1000);
        CHECK(feq(rst, 0.0f), "backwards runTime (reset) records nothing");
    }

    std::printf(g_fail == 0 ? "ALL ML BASELINE-DIM TESTS PASSED\n"
                            : "ML BASELINE-DIM TESTS FAILED: %d\n", g_fail);

    // ---------------------------------------------------------------------
    // Thread-safety exercise (TSan target): many threads record baselines on
    // distinct tids concurrently through the recursive_mutex-guarded model.
    // Under TSan this must report zero data races on baseline_refs_/baseline_windows.
    // ---------------------------------------------------------------------
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
            // each thread: duty = (run delta)/(ns delta) = 0.5 -> nrr 0.5.
            if (b <= 0.0f || b > 1.0f) ok = false;
        }
        CHECK(ok, "all concurrent baselines in (0,1] after join");
    }

    std::printf(g_fail == 0 ? "ALL ML BASELINE-DIM TESTS PASSED\n"
                            : "ML BASELINE-DIM TESTS FAILED: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}

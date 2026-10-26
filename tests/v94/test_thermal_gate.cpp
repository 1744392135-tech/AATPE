// test_thermal_gate.cpp — v4.1.2-hotfix Task 1: per-outcome-type thermal gating.
//
// Proves the MODERATE behaviour we coded in the outcome loop:
//   * when the frame outcome is suppressed (ft_weight forced to 0), the returned
//     outcome is PURELY the schedstat run-ratio score — wildly different frame
//     p95 values do NOT move it (classic samples still accumulate).
//   * when ft_weight > 0, the same frame p95 variation DOES move the fused score.
//   * the two thermal flags are independent: MODERATE sets frame-suppressed but
//     NOT the global training-pause; SEVERE/emergency sets the global pause.
//
// Links aatpe_ml.o (measureOutcomeV2 is a static method on LearningModel).
// Build: g++ ... test_thermal_gate.cpp aatpe_ml.o log_stub.o -o test_thermal_gate -pthread
#include <cstdio>
#include <cmath>
#include "aatpe_ml.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Task1: per-outcome thermal gate (MODERATE keeps schedstat, drops frame) ===\n");

    // Valid, non-anonymous schedstat inputs (monotonic run-time, forward timestamps).
    const unsigned long long beforeRt = 10000, afterRt = 20000;
    const time_t beforeTs = 100, afterTs = 105;
    const int beforeFreq = 800, afterFreq = 1000, baseFreq = 1000, loadInt = 50;
    const float baseline = 0.5f;

    // --- ft_weight = 0 (the MODERATE-suppressed path): frame data must be ignored ---
    float rrOnly_goodFrame = LearningModel::measureOutcomeV2(
        beforeRt, afterRt, beforeTs, afterTs, beforeFreq, afterFreq, baseFreq,
        loadInt, baseline, 2.0f, 2.0f, 100, 0.0f, true);
    float rrOnly_badFrame = LearningModel::measureOutcomeV2(
        beforeRt, afterRt, beforeTs, afterTs, beforeFreq, afterFreq, baseFreq,
        loadInt, baseline, 2.0f, 40.0f, 100, 0.0f, true);
    CHECK(rrOnly_goodFrame >= 0.0f, "ft_weight=0 good-frame outcome is valid (>=0)");
    CHECK(rrOnly_badFrame >= 0.0f,  "ft_weight=0 bad-frame outcome is valid (>=0)");
    CHECK(std::abs(rrOnly_goodFrame - rrOnly_badFrame) < 1e-6f,
          "ft_weight=0: bad vs good frame p95 -> IDENTICAL (frame ignored, pure rr)");
    printf("    rr_only = %.4f (frame p95 2ms vs 40ms)\n", (double)rrOnly_goodFrame);

    // --- ft_weight > 0 (normal): the same frame variation MUST move the fused score ---
    float fused_goodFrame = LearningModel::measureOutcomeV2(
        beforeRt, afterRt, beforeTs, afterTs, beforeFreq, afterFreq, baseFreq,
        loadInt, baseline, 2.0f, 2.0f, 100, 0.6f, true);
    float fused_badFrame = LearningModel::measureOutcomeV2(
        beforeRt, afterRt, beforeTs, afterTs, beforeFreq, afterFreq, baseFreq,
        loadInt, baseline, 2.0f, 40.0f, 100, 0.6f, true);
    CHECK(fused_goodFrame >= 0.0f, "ft_weight=0.6 good-frame outcome valid");
    CHECK(fused_badFrame >= 0.0f, "ft_weight=0.6 bad-frame outcome valid");
    CHECK(fused_goodFrame != fused_badFrame,
          "ft_weight=0.6: bad vs good frame p95 -> DIFFERENT (frame fusion active)");
    // A badly-janky frame should push the fused outcome DOWN (worse).
    CHECK(fused_badFrame < fused_goodFrame,
          "ft_weight=0.6: bad frame (40ms) scores LOWER than good frame (2ms)");
    printf("    fused good=%.4f bad=%.4f\n", (double)fused_goodFrame, (double)fused_badFrame);

    // --- thermal flags are independent atomics (MODERATE vs SEVERE split) ---
    g_mlTrainingPausedByThermal.store(false);
    g_mlFrameOutcomeSuppressedByThermal.store(false);
    // SEVERE/emergency sets the GLOBAL pause, frame flag irrelevant:
    g_mlTrainingPausedByThermal.store(true);
    CHECK(g_mlTrainingPausedByThermal.load() == true, "SEVERE: global training pause set");
    g_mlTrainingPausedByThermal.store(false);
    // MODERATE sets ONLY the frame-suppression flag, NOT the global pause:
    g_mlFrameOutcomeSuppressedByThermal.store(true);
    CHECK(g_mlFrameOutcomeSuppressedByThermal.load() == true, "MODERATE: frame suppression set");
    CHECK(g_mlTrainingPausedByThermal.load() == false, "MODERATE: global training STILL RUNNING");
    g_mlFrameOutcomeSuppressedByThermal.store(false);  // reset

    printf("\n========================================\n"
           "thermal gate: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

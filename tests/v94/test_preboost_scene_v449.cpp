// test_preboost_scene_v449.cpp — R2-L1 regression: preBoostTick() must NOT leak
// the 15s timebox when the caller omits nowMs (nowMs==0).
//
// Compiles pre_boost_predict.cpp in RELEASE mode (no -DAATPE_HOST_TEST) so the
// production realNowMs() body is linked. Pre-fix that body returned 0, so with
// nowMs==0 the timebox (nowMs - s_armStartMs) >= kPreBoostMaxMs never fired and the
// arm leaked. Post-fix realNowMs() is a steady_clock fallback, so after >15s the
// timebox disarms even when every tick passes nowMs==0.
#include <cstdio>
#include <chrono>
#include <thread>

#include "pre_boost_predict.h"

using namespace aatpe;
using namespace aatpe::preboost;

static int s_lastLevel = -1;
static int s_armCount = 0;
static int s_disarmCount = 0;
static void execRecorder(int level) {
    s_lastLevel = level;
    if (level > 0) s_armCount++;
    else s_disarmCount++;
}

static PreBoostFeatures armingFeatures() {
    PreBoostFeatures f{};
    f.fpsNow         = 55.0f;   // margin = (60-55)/60 = 0.083 -> in [0,0.10]
    f.fpsTarget      = 60.0f;
    f.missRate       = 0.01f;   // < 0.02 no-jank
    f.sfLatAvgUs     = 12000.0f; // > 0.6*16667=10000 -> lat arm
    f.sfLatBudgetUs  = 16667.0f;
    f.cpuUtilSlope   = 2.0f;    // > 1.0 slope arm
    f.headroom       = 0.5f;    // > 0.3 heat-ok
    f.thermalEmergency = false;
    f.passiveMode      = false;
    f.jankVoteActive   = false;
    return f;
}

int main() {
    printf("=== R2-L1 preBoostTick timebox with nowMs==0 (release clock) ===\n");
    setBoostExecutor(execRecorder);

    PreBoostFeatures f = armingFeatures();

    // 1) First tick with nowMs==0 -> should arm (fallback clock yields a real start).
    bool r1 = preBoostTick(0, f);
    printf("  tick1 (nowMs=0, arm) -> armed=%d executorLevel=%d\n", (int)r1, s_lastLevel);
    if (!r1) { printf("  [FAIL] did not arm on first tick\n"); return 1; }
    if (s_lastLevel <= 0) { printf("  [FAIL] executor not notified with arm level\n"); return 1; }

    // 2) Immediate second tick -> still armed (elapsed ~0 < 15s).
    bool r2 = preBoostTick(0, f);
    printf("  tick2 (nowMs=0, immediate) -> armed=%d\n", (int)r2);
    if (!r2) { printf("  [FAIL] disarmed immediately (clock inconsistent?)\n"); return 1; }

    // 3) Wait past the 15s hard cap, still passing nowMs==0.
    printf("  sleeping 16s to exceed kPreBoostMaxMs=15000 ...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(16000));

    // 4) After the wait the timebox MUST have disarmed even without an injected clock.
    bool r3 = preBoostTick(0, f);
    printf("  tick3 (nowMs=0, after 16s) -> armed=%d executorLevel=%d disarms=%d\n",
           (int)r3, s_lastLevel, s_disarmCount);

    if (r3) {
        printf("  [FAIL] BOOST LEAK: still armed after 16s with nowMs==0 "
               "(release realNowMs() still returns 0?)\n");
        return 1;
    }
    if (s_lastLevel != 0) {
        printf("  [FAIL] disarmed but executor not notified with level=0\n");
        return 1;
    }
    printf("  [PASS] timebox disarmed after >15s with nowMs==0; no leak\n");
    printf("=== R2-L1 PASS (armCount=%d disarmCount=%d) ===\n", s_armCount, s_disarmCount);
    return 0;
}

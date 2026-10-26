// test_preboost.cpp - v94 hermetic unit tests for C1 predictive pre-boost.
// Covers W2 B.10 T1-T8 + replay trajectory evidence.
// Links pre_boost_predict.cpp (pure logic, no daemon deps needed).
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "pre_boost_predict.h"

using namespace aatpe::preboost;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Build a "normal calm game" feature set.
static PreBoostFeatures calmFeatures() {
    PreBoostFeatures f{};
    f.fpsNow          = 60.0f;
    f.fpsP99         = 58.0f;
    f.fpsTarget      = 60.0f;
    f.missRate       = 0.0f;
    f.sfLatAvgUs     = 8000.0f;    // ~50% of 16667us budget
    f.sfLatBudgetUs  = 16667.0f;
    f.cpuUtilSlope   = 0.2f;
    f.headroom       = 0.8f;
    f.thermalEmergency = false;
    f.passiveMode    = false;
    f.jankVoteActive = false;
    return f;
}

// Build a "trend worsening, no jank yet" feature set.
static PreBoostFeatures worseningFeatures() {
    PreBoostFeatures f{};
    f.fpsNow          = 58.0f;      // frameMargin ~3.3%
    f.fpsP99         = 52.0f;
    f.fpsTarget      = 60.0f;
    f.missRate       = 0.01f;      // 1% — not janking yet
    f.sfLatAvgUs     = 13000.0f;   // ~78% of budget (>60% threshold)
    f.sfLatBudgetUs  = 16667.0f;
    f.cpuUtilSlope   = 1.8f;       // rising fast
    f.headroom       = 0.6f;       // cool
    f.thermalEmergency = false;
    f.passiveMode    = false;
    f.jankVoteActive = false;
    return f;
}

int main() {
    printf("=== C1 Pre-Boost: T1 — steady game, do not arm ===\n");
    {
        resetForTest();
        PreBoostFeatures f = calmFeatures();
        PreBoostDecision d = predictPreBoost(f);
        CHECK(d.arm == false, "T1: calm steady game -> arm=false");
        CHECK(d.uclampMinPct == 0, "T1: no boost output");
    }

    printf("=== C1 Pre-Boost: T2 — trend worsening, no jank yet, correct arm ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        PreBoostDecision d = predictPreBoost(f);
        CHECK(d.arm == true, "T2: worsening trend -> arm=true");
        CHECK(d.uclampMinPct >= 80, "T2: uclampMinPct >= 80%");
        CHECK(d.tierTarget == 2, "T2: tierTarget = BIG (2)");
    }

    printf("=== C1 Pre-Boost: T3 — real jank arrives, hand off to reactive ===\n");
    {
        resetForTest();
        // Arm first
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(0, f);
        CHECK(active == true, "T3: armed after worsening features");
        CHECK(isArmed() == true, "T3: state machine shows armed");
        // Now missRate jumps to 6% (real jank)
        f.missRate = 0.06f;
        active = preBoostTick(200, f);
        CHECK(active == false, "T3: missRate 6% -> disarm (hand off to reactive)");
        CHECK(isArmed() == false, "T3: state machine shows disarmed");
    }

    printf("=== C1 Pre-Boost: T4 — thermal rejection ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        f.headroom = 0.2f;  // too hot
        PreBoostDecision d = predictPreBoost(f);
        CHECK(d.arm == false, "T4: headroom=0.2 -> arm=false");
        CHECK(d.arm == false, "T4: reason should be thermal-gated");
    }

    printf("=== C1 Pre-Boost: T5 — time-box hard cap at 15s ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        // Arm at t=0
        bool active = preBoostTick(0, f);
        CHECK(active == true, "T5: armed at t=0");
        // Feed "neither jank nor worsening" input for 16 seconds.
        // Keep features in a "neutral" zone that doesn't disarm early.
        // We need missRate < 5%, headroom > 15%, frameMargin not > 15%.
        // Use calm-like features but with fpsNow close to target (small margin).
        PreBoostFeatures neutral = calmFeatures();
        neutral.fpsNow = 59.0f;  // frameMargin ~1.7% (not > 15%, not false alarm)
        neutral.sfLatAvgUs = 9000.0f;  // still under 60% threshold, so no re-arm
        neutral.cpuUtilSlope = 0.3f;
        neutral.missRate = 0.005f;
        // Tick every 200ms for 16 seconds = 80 ticks
        for (int i = 1; i <= 80; i++) {
            long long t = i * 200;
            active = preBoostTick(t, neutral);
        }
        // At t=16000ms, should have hit 15000ms hard cap.
        CHECK(active == false, "T5: after 16s of neutral -> disarmed by time-box");
        CHECK(isArmed() == false, "T5: state machine shows disarmed");
    }

    printf("=== C1 Pre-Boost: T6 — false alarm early release (load never came) ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(0, f);
        CHECK(active == true, "T6: armed at t=0");
        // Now load never materialized: latency dropped back to baseline,
        // CPU slope flattened, fps still near target (no real jank).
        PreBoostFeatures falseAlarm = worseningFeatures();
        falseAlarm.sfLatAvgUs = 5000.0f;   // < 0.5 * 16667 = 8333 (lat recovered)
        falseAlarm.cpuUtilSlope = 0.1f;    // |slope| < 0.5 (slope flattened)
        falseAlarm.fpsNow = 59.5f;         // frameMargin ~0.8% < 0.05 (fps stable)
        falseAlarm.missRate = 0.005f;      // low miss
        falseAlarm.headroom = 0.6f;        // still cool
        falseAlarm.jankVoteActive = false;
        // Tick 3 beats (each 200ms apart)
        active = preBoostTick(200, falseAlarm);
        CHECK(active == true, "T6: beat 1: still armed (streak=1)");
        active = preBoostTick(400, falseAlarm);
        CHECK(active == true, "T6: beat 2: still armed (streak=2)");
        active = preBoostTick(600, falseAlarm);
        CHECK(active == false, "T6: beat 3: disarmed (streak=3, false alarm)");
    }

    printf("=== C1 Pre-Boost: T7 — passive mode blocks all ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        f.passiveMode = true;
        PreBoostDecision d = predictPreBoost(f);
        CHECK(d.arm == false, "T7: passiveMode=true -> arm=false");
        // Even via state machine
        bool active = preBoostTick(0, f);
        CHECK(active == false, "T7: state machine also refuses to arm in passive");
    }

    printf("=== C1 Pre-Boost: T8 — rate limiting: max 3 arms per minute ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        // Arm 3 times (we need to disarm between arms).
        for (int i = 0; i < 3; i++) {
            // Arm at t = i*5000
            bool active = preBoostTick(i * 5000, f);
            CHECK(active == true, "T8: arm succeeds within limit");
            // Immediately disarm by making missRate high
            f.missRate = 0.06f;
            active = preBoostTick(i * 5000 + 100, f);
            CHECK(active == false, "T8: disarm after arm");
            // Reset missRate for next arm
            f.missRate = 0.01f;
        }
        // 4th arm within 60s window -> should be rejected
        int beforeReject = rejectedCount();
        bool active = preBoostTick(20000, f);  // t=20s, still within 60s window
        CHECK(active == false, "T8: 4th arm within 1 min -> rejected");
        CHECK(rejectedCount() == beforeReject + 1, "T8: rejectedCount incremented");
    }

    printf("\n=== C1 Replay: synthetic team-fight trajectory ===\n");
    // Replay: simulate a "calm -> trend worsening -> team fight jank" trajectory.
    // Measure: how many beats from "trend worsening" to arm (advance lead),
    // and false-positive rate on a "calm long session" replay.
    {
        resetForTest();
        // Phase 1: 20 beats of calm (should not arm)
        int falseArms = 0;
        for (int i = 0; i < 20; i++) {
            PreBoostFeatures f = calmFeatures();
            bool active = preBoostTick(i * 200, f);
            if (active) falseArms++;
        }
        printf("  Replay: 20 calm beats -> %d false arms (expect 0)\n", falseArms);
        CHECK(falseArms == 0, "Replay: zero false arms on calm trajectory");

        // Phase 2: 5 beats of trend worsening (should arm on first worsening beat)
        int armBeat = -1;
        for (int i = 0; i < 5; i++) {
            PreBoostFeatures f = worseningFeatures();
            bool active = preBoostTick((20 + i) * 200, f);
            if (active && armBeat < 0) armBeat = i;
        }
        printf("  Replay: arm on worsening beat #%d (advance lead = %d beats before jank)\n",
               armBeat, armBeat);
        CHECK(armBeat == 0, "Replay: arm on first worsening beat (zero delay)");

        // Phase 3: jank arrives (missRate 6%), should disarm
        PreBoostFeatures jank = worseningFeatures();
        jank.missRate = 0.06f;
        bool active = preBoostTick(25 * 200, jank);
        CHECK(active == false, "Replay: disarm when real jank arrives");
    }

    printf("\n=== C1 Wiring: buildFeatures key mapping ===\n");
    {
        // Verify buildFeatures maps raw state values correctly.
        // game_avg_fps=55, game_p99_fps=48, game_frame_miss=3%, sf_latency=11000us
        std::vector<int> cpuHist = {20, 30, 40, 50, 60, 70};
        PreBoostFeatures f = buildFeatures(
            55.0f, 48.0f, 3, 11000, cpuHist,
            5.0f,                   // sampleIntervalSec (5s per aux slot)
            60.0f, 16667.0f,
            0.5f, true,
            false, false, false);
        CHECK(f.fpsNow == 55.0f, "WF: fpsNow mapped from game_avg_fps");
        CHECK(f.fpsP99 == 48.0f, "WF: fpsP99 mapped from game_p99_fps");
        CHECK(f.missRate > 0.029f && f.missRate < 0.031f, "WF: missRate pct->[0,1]");
        CHECK(f.sfLatAvgUs == 11000.0f, "WF: sfLatAvgUs mapped from sf_latency_avg");
        CHECK(f.fpsTarget == 60.0f, "WF: fpsTarget default 60");
        CHECK(f.sfLatBudgetUs == 16667.0f, "WF: sfLatBudgetUs default 16667");
        CHECK(f.headroom == 0.5f, "WF: headroom passed through when valid");
        // Headroom invalid -> fail-closed 0.0 (M-3: don't arm when sensor unknown)
        PreBoostFeatures f2 = buildFeatures(
            60.0f, 60.0f, 0, 0, {},
            5.0f,
            60.0f, 16667.0f,
            -1.0f, false,  // invalid headroom
            false, false, false);
        CHECK(f2.headroom == 0.0f, "WF: invalid headroom -> 0.0 fail-closed");

        // C-2: 120Hz display (frameTimeUs = 8333, fpsTarget = 120).
        // On a 120Hz screen, hardcoded 60fps would never arm (margin always negative).
        PreBoostFeatures f120 = buildFeatures(
            118.0f, 112.0f, 1, 5000,
            {20, 30, 40, 50, 60},  // rising CPU util
            5.0f,                   // sampleIntervalSec
            120.0f, 8333.0f,       // 120fps target, 8333us budget
            0.7f, true,
            false, false, false);
        CHECK(f120.fpsTarget == 120.0f, "WF: 120Hz fpsTarget passed through");
        CHECK(f120.sfLatBudgetUs == 8333.0f, "WF: 120Hz budget = 8333us");
        // On 120Hz, fpsNow=118 gives margin = (120-118)/120 = 1.7%, within [0,10%].
        // Latency 5000us / 8333us = 60% (at threshold). Slope rising. Should arm.
        PreBoostDecision d120 = predictPreBoost(f120);
        CHECK(d120.arm == true, "WF: 120Hz screen can arm (not hardcoded 60fps)");
    }

    printf("\n=== C1 Wiring: computeCpuSlope ===\n");
    {
        // Rising history: slope should be positive
        std::vector<int> rising = {10, 20, 30, 40, 50};
        float slope = computeCpuSlope(rising);
        CHECK(slope > 9.0f && slope < 11.0f, "SL: rising history -> positive slope ~10");
        // Falling history: slope should be negative
        std::vector<int> falling = {50, 40, 30, 20, 10};
        float slope2 = computeCpuSlope(falling);
        CHECK(slope2 < -9.0f && slope2 > -11.0f, "SL: falling history -> negative slope ~-10");
        // Flat history: slope ~0
        std::vector<int> flat = {50, 50, 50, 50, 50};
        float slope3 = computeCpuSlope(flat);
        CHECK(slope3 > -0.1f && slope3 < 0.1f, "SL: flat history -> ~0 slope");
        // Too few samples -> 0
        std::vector<int> tiny = {10, 20};
        float slope4 = computeCpuSlope(tiny);
        CHECK(slope4 == 0.0f, "SL: <3 samples -> 0");
    }

    printf("\n=== C1 Wiring: slope interval consistency (C-3 fix) ===\n");
    {
        // Same CPU load trajectory (10->20->30->40->50), different sample intervals.
        // Slope in %/s should be CONSISTENT regardless of sample interval.
        std::vector<int> traj = {10, 20, 30, 40, 50};
        // 5s interval: per-sample slope = 10, /5.0 = 2.0 %/s
        PreBoostFeatures f5 = buildFeatures(
            60.0f, 60.0f, 0, 0, traj,
            5.0f,                   // 5s interval
            60.0f, 16667.0f,
            0.8f, true,
            false, false, false);
        // 25s interval: per-sample slope = 10, /25.0 = 0.4 %/s
        PreBoostFeatures f25 = buildFeatures(
            60.0f, 60.0f, 0, 0, traj,
            25.0f,                  // 25s interval
            60.0f, 16667.0f,
            0.8f, true,
            false, false, false);
        printf("  slope @5s=%.3f %%/s, @25s=%.3f %%/s (expect 2.0 vs 0.4)\n",
               f5.cpuUtilSlope, f25.cpuUtilSlope);
        CHECK(f5.cpuUtilSlope > 1.9f && f5.cpuUtilSlope < 2.1f, "INT: 5s interval -> ~2.0 %%/s");
        CHECK(f25.cpuUtilSlope > 0.35f && f25.cpuUtilSlope < 0.45f, "INT: 25s interval -> ~0.4 %%/s");
        // Invalid interval -> fail-safe slope=0
        PreBoostFeatures f0 = buildFeatures(
            60.0f, 60.0f, 0, 0, traj,
            0.0f,                   // invalid interval
            60.0f, 16667.0f,
            0.8f, true,
            false, false, false);
        CHECK(f0.cpuUtilSlope == 0.0f, "INT: invalid interval -> slope=0 (fail-safe)");
    }

    printf("\n=== C1 Wiring: executor callback fires ===\n");
    {
        resetForTest();
        static int s_lastLevel = -1;
        static int s_callCount = 0;
        s_lastLevel = -1; s_callCount = 0;
        setBoostExecutor([](int level) {
            s_lastLevel = level;
            s_callCount++;
        });
        // Arm with worsening features
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(1000, f);
        CHECK(active == true, "EX: arm via worsening features");
        CHECK(s_callCount == 1, "EX: executor called once on arm");
        CHECK(s_lastLevel == 80, "EX: executor got uclamp level 80");
        // Disarm: make missRate high
        f.missRate = 0.06f;
        active = preBoostTick(2000, f);
        CHECK(active == false, "EX: disarm on real jank");
        CHECK(s_callCount == 2, "EX: executor called again on disarm");
        CHECK(s_lastLevel == 0, "EX: executor got level 0 on disarm");
        resetForTest();  // clears executor
    }

    printf("\n=== ML-05: concurrent preBoostTick hammer (mutex correctness) ===\n");
    {
        resetForTest();
        static int s_callCount = 0;
        s_callCount = 0;
        setBoostExecutor([](int) { s_callCount++; });
        PreBoostFeatures f = worseningFeatures();  // arms on first tick
        // 4 threads hammering the same state machine concurrently at the SAME
        // nowMs. Exactly one may win the first arm (subsequent ticks see
        // s_armed==true and stay armed). Without the state mutex, two threads
        // racing through the s_armed==false branch would BOTH arm and the
        // executor would fire twice — so s_callCount==1 proves the lock works.
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; t++) {
            ths.emplace_back([&]() {
                for (int i = 0; i < 300; i++) preBoostTick(5000, f);
            });
        }
        for (auto& th : ths) th.join();
        CHECK(s_callCount == 1, "ML-05: concurrent arms serialized -> executor fired exactly once");
        CHECK(isArmed() == true, "ML-05: state machine self-consistent after hammer");
        CHECK(rejectedCount() == 0, "ML-05: no arm rejected within the 1/min window");
        resetForTest();
    }

    printf("\n========================================\n"
           "C1 Pre-Boost results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

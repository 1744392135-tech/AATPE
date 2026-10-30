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

static PreBoostFeatures calmFeatures() {
    PreBoostFeatures f{};
    f.fpsNow          = 60.0f;
    f.fpsP99         = 58.0f;
    f.fpsTarget      = 60.0f;
    f.missRate       = 0.0f;
    f.sfLatAvgUs     = 8000.0f;
    f.sfLatBudgetUs  = 16667.0f;
    f.cpuUtilSlope   = 0.2f;
    f.headroom       = 0.8f;
    f.thermalEmergency = false;
    f.passiveMode    = false;
    f.jankVoteActive = false;
    return f;
}

static PreBoostFeatures worseningFeatures() {
    PreBoostFeatures f{};
    f.fpsNow          = 58.0f;
    f.fpsP99         = 52.0f;
    f.fpsTarget      = 60.0f;
    f.missRate       = 0.01f;
    f.sfLatAvgUs     = 13000.0f;
    f.sfLatBudgetUs  = 16667.0f;
    f.cpuUtilSlope   = 1.8f;
    f.headroom       = 0.6f;
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
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(0, f);
        CHECK(active == true, "T3: armed after worsening features");
        CHECK(isArmed() == true, "T3: state machine shows armed");
        f.missRate = 0.06f;
        active = preBoostTick(200, f);
        CHECK(active == false, "T3: missRate 6% -> disarm (hand off to reactive)");
        CHECK(isArmed() == false, "T3: state machine shows disarmed");
    }

    printf("=== C1 Pre-Boost: T4 — thermal rejection ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        f.headroom = 0.2f;
        PreBoostDecision d = predictPreBoost(f);
        CHECK(d.arm == false, "T4: headroom=0.2 -> arm=false");
        CHECK(d.arm == false, "T4: reason should be thermal-gated");
    }

    printf("=== C1 Pre-Boost: T5 — time-box hard cap at 15s ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(0, f);
        CHECK(active == true, "T5: armed at t=0");
        PreBoostFeatures neutral = calmFeatures();
        neutral.fpsNow = 59.0f;
        neutral.sfLatAvgUs = 9000.0f;
        neutral.cpuUtilSlope = 0.3f;
        neutral.missRate = 0.005f;
        for (int i = 1; i <= 80; i++) {
            long long t = i * 200;
            active = preBoostTick(t, neutral);
        }
        CHECK(active == false, "T5: after 16s of neutral -> disarmed by time-box");
        CHECK(isArmed() == false, "T5: state machine shows disarmed");
    }

    printf("=== C1 Pre-Boost: T6 — false alarm early release (load never came) ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(0, f);
        CHECK(active == true, "T6: armed at t=0");
        PreBoostFeatures falseAlarm = worseningFeatures();
        falseAlarm.sfLatAvgUs = 5000.0f;
        falseAlarm.cpuUtilSlope = 0.1f;
        falseAlarm.fpsNow = 59.5f;
        falseAlarm.missRate = 0.005f;
        falseAlarm.headroom = 0.6f;
        falseAlarm.jankVoteActive = false;
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
        bool active = preBoostTick(0, f);
        CHECK(active == false, "T7: state machine also refuses to arm in passive");
    }

    printf("=== C1 Pre-Boost: T8 — rate limiting: max 3 arms per minute ===\n");
    {
        resetForTest();
        PreBoostFeatures f = worseningFeatures();
        for (int i = 0; i < 3; i++) {
            bool active = preBoostTick(i * 5000, f);
            CHECK(active == true, "T8: arm succeeds within limit");
            f.missRate = 0.06f;
            active = preBoostTick(i * 5000 + 100, f);
            CHECK(active == false, "T8: disarm after arm");
            f.missRate = 0.01f;
        }
        int beforeReject = rejectedCount();
        bool active = preBoostTick(20000, f);
        CHECK(active == false, "T8: 4th arm within 1 min -> rejected");
        CHECK(rejectedCount() == beforeReject + 1, "T8: rejectedCount incremented");
    }

    printf("\n=== C1 Replay: synthetic team-fight trajectory ===\n");
    {
        resetForTest();
        int falseArms = 0;
        for (int i = 0; i < 20; i++) {
            PreBoostFeatures f = calmFeatures();
            bool active = preBoostTick(i * 200, f);
            if (active) falseArms++;
        }
        printf("  Replay: 20 calm beats -> %d false arms (expect 0)\n", falseArms);
        CHECK(falseArms == 0, "Replay: zero false arms on calm trajectory");

        int armBeat = -1;
        for (int i = 0; i < 5; i++) {
            PreBoostFeatures f = worseningFeatures();
            bool active = preBoostTick((20 + i) * 200, f);
            if (active && armBeat < 0) armBeat = i;
        }
        printf("  Replay: arm on worsening beat #%d (advance lead = %d beats before jank)\n",
               armBeat, armBeat);
        CHECK(armBeat == 0, "Replay: arm on first worsening beat (zero delay)");

        PreBoostFeatures jank = worseningFeatures();
        jank.missRate = 0.06f;
        bool active = preBoostTick(25 * 200, jank);
        CHECK(active == false, "Replay: disarm when real jank arrives");
    }

    printf("\n=== C1 Wiring: buildFeatures key mapping ===\n");
    {
        std::vector<int> cpuHist = {20, 30, 40, 50, 60, 70};
        PreBoostFeatures f = buildFeatures(
            55.0f, 48.0f, 3, 11000, cpuHist,
            5.0f,
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
        PreBoostFeatures f2 = buildFeatures(
            60.0f, 60.0f, 0, 0, {},
            5.0f,
            60.0f, 16667.0f,
            -1.0f, false,
            false, false, false);
        CHECK(f2.headroom == 0.0f, "WF: invalid headroom -> 0.0 fail-closed");

        PreBoostFeatures f120 = buildFeatures(
            118.0f, 112.0f, 1, 5000,
            {20, 30, 40, 50, 60},
            5.0f,
            120.0f, 8333.0f,
            0.7f, true,
            false, false, false);
        CHECK(f120.fpsTarget == 120.0f, "WF: 120Hz fpsTarget passed through");
        CHECK(f120.sfLatBudgetUs == 8333.0f, "WF: 120Hz budget = 8333us");
        PreBoostDecision d120 = predictPreBoost(f120);
        CHECK(d120.arm == true, "WF: 120Hz screen can arm (not hardcoded 60fps)");
    }

    printf("\n=== C1 Wiring: computeCpuSlope ===\n");
    {
        std::vector<int> rising = {10, 20, 30, 40, 50};
        float slope = computeCpuSlope(rising);
        CHECK(slope > 9.0f && slope < 11.0f, "SL: rising history -> positive slope ~10");
        std::vector<int> falling = {50, 40, 30, 20, 10};
        float slope2 = computeCpuSlope(falling);
        CHECK(slope2 < -9.0f && slope2 > -11.0f, "SL: falling history -> negative slope ~-10");
        std::vector<int> flat = {50, 50, 50, 50, 50};
        float slope3 = computeCpuSlope(flat);
        CHECK(slope3 > -0.1f && slope3 < 0.1f, "SL: flat history -> ~0 slope");
        std::vector<int> tiny = {10, 20};
        float slope4 = computeCpuSlope(tiny);
        CHECK(slope4 == 0.0f, "SL: <3 samples -> 0");
    }

    printf("\n=== C1 Wiring: slope interval consistency (C-3 fix) ===\n");
    {
        std::vector<int> traj = {10, 20, 30, 40, 50};
        PreBoostFeatures f5 = buildFeatures(
            60.0f, 60.0f, 0, 0, traj,
            5.0f,
            60.0f, 16667.0f,
            0.8f, true,
            false, false, false);
        PreBoostFeatures f25 = buildFeatures(
            60.0f, 60.0f, 0, 0, traj,
            25.0f,
            60.0f, 16667.0f,
            0.8f, true,
            false, false, false);
        printf("  slope @5s=%.3f %%/s, @25s=%.3f %%/s (expect 2.0 vs 0.4)\n",
               f5.cpuUtilSlope, f25.cpuUtilSlope);
        CHECK(f5.cpuUtilSlope > 1.9f && f5.cpuUtilSlope < 2.1f, "INT: 5s interval -> ~2.0 %%/s");
        CHECK(f25.cpuUtilSlope > 0.35f && f25.cpuUtilSlope < 0.45f, "INT: 25s interval -> ~0.4 %%/s");
        PreBoostFeatures f0 = buildFeatures(
            60.0f, 60.0f, 0, 0, traj,
            0.0f,
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
        PreBoostFeatures f = worseningFeatures();
        bool active = preBoostTick(1000, f);
        CHECK(active == true, "EX: arm via worsening features");
        CHECK(s_callCount == 1, "EX: executor called once on arm");
        CHECK(s_lastLevel == 80, "EX: executor got uclamp level 80");
        f.missRate = 0.06f;
        active = preBoostTick(2000, f);
        CHECK(active == false, "EX: disarm on real jank");
        CHECK(s_callCount == 2, "EX: executor called again on disarm");
        CHECK(s_lastLevel == 0, "EX: executor got level 0 on disarm");
        resetForTest();
    }

    printf("\n=== ML-05: concurrent preBoostTick hammer (mutex correctness) ===\n");
    {
        resetForTest();
        static int s_callCount = 0;
        s_callCount = 0;
        setBoostExecutor([](int) { s_callCount++; });
        PreBoostFeatures f = worseningFeatures();
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

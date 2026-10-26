// test_thermheadroom.cpp - v94 hermetic unit tests for C3 thermal headroom.
// Covers W2 C.9 T1-T7 + smoothness replay evidence.
#include <cstdio>
#include <cmath>
#include "thermal_headroom.h"

using namespace aatpe::thermh;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Standard walls per W2 C.9: warning=75, critical=85, emergency=95.
static constexpr float kWarn = 75.0f;
static constexpr float kCrit = 85.0f;
static constexpr float kEmerg = 95.0f;

static ThermalHeadroomInput makeInput(float tempC, float slopeCps,
                                       bool sensorOk = true,
                                       bool emergency = false) {
    ThermalHeadroomInput in{};
    in.tempC          = tempC;
    in.warningWallC   = kWarn;
    in.criticalWallC  = kCrit;
    in.emergencyWallC = kEmerg;
    in.slopeCps       = slopeCps;
    in.etaWarningS    = 120.0f;
    in.sensorOk       = sensorOk;
    in.thermalEmergency = emergency;
    return in;
}

int main() {
    printf("=== C3 Thermal Headroom: T1 — steady 30C, full scale ===\n");
    {
        // temp 30, well below warning 75 -> headroom=1, scale=1
        HeadroomOutput o = thermalHeadroom(makeInput(30.0f, 0.0f));
        CHECK(o.headroom01 > 0.95f, "T1: temp 30C -> headroom ~1.0");
        CHECK(o.placementScale > 0.95f, "T1: placementScale ~1.0");
        CHECK(o.nearWall == false, "T1: not near wall");
    }

    printf("=== C3 Thermal Headroom: T2 — slow rise 60->75C, scale drops gradually ===\n");
    {
        // temp=75 (at warning), slope=0.3 C/s
        // headroom = (85-75)/(85-75) = 1.0
        // slope correction: 1.0 / (1 + 0.3*0.5) = 1.0/1.15 = 0.870
        // scale = 0.4 + 0.6 * 0.870 = 0.922
        HeadroomOutput o = thermalHeadroom(makeInput(75.0f, 0.3f));
        CHECK(o.placementScale < 1.0f, "T2: at warning wall, scale < 1.0");
        CHECK(o.placementScale > 0.85f, "T2: still mostly perf (0.92 expected)");
        CHECK(o.nearWall == false, "T2: at warning, not yet near critical");
    }

    printf("=== C3 Thermal Headroom: T3 — rapid spike 60->88C in 5s, scale drops early ===\n");
    {
        // temp=80, slope=5 C/s (rapid heating)
        // headroom = (85-80)/(85-75) = 0.5
        // slope correction: 0.5 / (1 + 5*0.5) = 0.5/3.5 = 0.143
        // scale = 0.4 + 0.6 * 0.143 = 0.486
        HeadroomOutput o = thermalHeadroom(makeInput(80.0f, 5.0f));
        CHECK(o.placementScale < 0.7f, "T3: rapid slope correction -> scale < 0.7 BEFORE critical");
        CHECK(o.nearWall == false, "T3: temp=80 < critical-2=83, not near wall yet");
    }

    printf("=== C3 Thermal Headroom: T4 — slow cooling 88->60C, scale recovers with delay ===\n");
    {
        // First, heat up to get a low smoothed scale.
        float prev = 1.0f;
        int relaxStreak = 0;

        // Simulate 5 beats of hot (temp=88, slope=-2 cooling)
        for (int i = 0; i < 5; i++) {
            ThermalHeadroomInput in = makeInput(88.0f, -2.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        float hotScale = prev;
        CHECK(hotScale < 0.8f, "T4: after 5 hot beats, smoothed scale dropped");

        // Now cool down: temp drops, raw scale goes up.
        // But EMA + relax-later hysteresis means it shouldn't bounce back immediately.
        float prevBeforeCooling = prev;
        for (int i = 0; i < 3; i++) {
            ThermalHeadroomInput in = makeInput(60.0f, -1.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        // After 3 cooling beats, the scale should have started recovering
        // but NOT jumped back to 1.0 instantly (hysteresis).
        CHECK(prev > prevBeforeCooling, "T4: scale starts recovering after cooling");
        CHECK(prev < 1.0f, "T4: not back to full scale yet (hysteresis working)");
    }

    printf("=== C3 Thermal Headroom: T5 — over wall, nearWall, defer to hard wall ===\n");
    {
        // temp=86 > critical=85
        HeadroomOutput o = thermalHeadroom(makeInput(86.0f, 0.0f));
        CHECK(o.nearWall == true, "T5: temp > critical -> nearWall=true");
        // Emergency flag set
        HeadroomOutput o2 = thermalHeadroom(makeInput(96.0f, 0.0f, true, true));
        CHECK(o2.nearWall == true, "T5: emergency flag -> nearWall=true");
        CHECK(o2.placementScale == 1.0f, "T5: emergency -> fail-open scale=1 (hard wall handles it)");
    }

    printf("=== C3 Thermal Headroom: T6 — sensor fault, fail-open ===\n");
    {
        HeadroomOutput o = thermalHeadroom(makeInput(80.0f, 5.0f, false));
        CHECK(o.placementScale == 1.0f, "T6: sensorOk=false -> scale=1.0 (fail-open)");
        CHECK(o.headroom01 == 1.0f, "T6: sensorOk=false -> headroom=1.0");
    }

    printf("=== C3 Thermal Headroom: T7 — degenerate walls, fail-open ===\n");
    {
        // warning=80, critical=80 (degenerate, wallSpan=0)
        ThermalHeadroomInput in = makeInput(70.0f, 0.0f);
        in.warningWallC = 80.0f;
        in.criticalWallC = 80.0f;
        HeadroomOutput o = thermalHeadroom(in);
        CHECK(o.placementScale == 1.0f, "T7: degenerate walls -> scale=1.0 (fail-open)");
    }

    printf("\n=== C3 Replay: team-fight rapid heating trajectory ===\n");
    // Simulate: 10 beats of cool (30C) -> 10 beats of rapid heating (60->85C).
    // Measure: at which beat does placementScale drop below 0.7?
    // Compare to "no slope correction" baseline.
    {
        float prev = 1.0f;
        int relaxStreak = 0;
        int beatBelow07 = -1;

        // 10 cool beats
        for (int i = 0; i < 10; i++) {
            ThermalHeadroomInput in = makeInput(30.0f, 0.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        printf("  Replay: after 10 cool beats, scale = %.3f\n", prev);

        // 10 heating beats: temp rises from 60 to 85, slope ~2.5 C/s
        for (int i = 0; i < 10; i++) {
            float temp = 60.0f + i * 2.5f;
            ThermalHeadroomInput in = makeInput(temp, 2.5f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
            printf("  Replay: beat %d, temp=%.1fC, raw scale=%.3f, smoothed=%.3f\n",
                   i, temp, o.placementScale, prev);
            if (prev < 0.7f && beatBelow07 < 0) beatBelow07 = i;
        }
        printf("  Replay: placementScale dropped below 0.7 at heating beat #%d\n", beatBelow07);
        CHECK(beatBelow07 >= 0, "Replay: scale dropped below 0.7 at some point during heating");
        // Without slope correction, at temp=80C raw scale = 0.7 exactly.
        // With slope=2.5 C/s correction, smoothed scale at temp=80C (beat 8) = 0.674 < 0.7.
        // This proves the slope correction provides early warning: the scale is below
        // the no-correction baseline (0.7) by the time temp reaches 80C.
        CHECK(beatBelow07 <= 8, "Replay: smoothed scale < 0.7 by temp=80C (early warning vs no-slope baseline)");
    }

    printf("\n=== C3 Replay: slow cooling trajectory (no oscillation) ===\n");
    {
        float prev = 1.0f;
        int relaxStreak = 0;
        // Heat up to low scale
        for (int i = 0; i < 10; i++) {
            ThermalHeadroomInput in = makeInput(82.0f, 1.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        float minScale = prev;
        printf("  Replay: min scale after heating = %.3f\n", minScale);

        // Cool down gradually
        bool oscillated = false;
        float prevScale = prev;
        for (int i = 0; i < 20; i++) {
            ThermalHeadroomInput in = makeInput(65.0f, -0.5f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
            // Check for oscillation: if scale goes down again after going up, flag it.
            if (i > 5 && prev < prevScale - 0.02f) {
                oscillated = true;
            }
            prevScale = prev;
        }
        printf("  Replay: after cooling, final scale = %.3f, oscillated = %s\n",
               prev, oscillated ? "YES" : "no");
        CHECK(!oscillated, "Replay: no oscillation during gradual cooling");
    }

    printf("\n========================================\n"
           "C3 Thermal Headroom results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

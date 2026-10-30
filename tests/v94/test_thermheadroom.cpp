#include <cstdio>
#include <cmath>
#include "thermal_headroom.h"

using namespace aatpe::thermh;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

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
        HeadroomOutput o = thermalHeadroom(makeInput(30.0f, 0.0f));
        CHECK(o.headroom01 > 0.95f, "T1: temp 30C -> headroom ~1.0");
        CHECK(o.placementScale > 0.95f, "T1: placementScale ~1.0");
        CHECK(o.nearWall == false, "T1: not near wall");
    }

    printf("=== C3 Thermal Headroom: T2 — slow rise 60->75C, scale drops gradually ===\n");
    {
        HeadroomOutput o = thermalHeadroom(makeInput(75.0f, 0.3f));
        CHECK(o.placementScale < 1.0f, "T2: at warning wall, scale < 1.0");
        CHECK(o.placementScale > 0.85f, "T2: still mostly perf (0.92 expected)");
        CHECK(o.nearWall == false, "T2: at warning, not yet near critical");
    }

    printf("=== C3 Thermal Headroom: T3 — rapid spike 60->88C in 5s, scale drops early ===\n");
    {
        HeadroomOutput o = thermalHeadroom(makeInput(80.0f, 5.0f));
        CHECK(o.placementScale < 0.7f, "T3: rapid slope correction -> scale < 0.7 BEFORE critical");
        CHECK(o.nearWall == false, "T3: temp=80 < critical-2=83, not near wall yet");
    }

    printf("=== C3 Thermal Headroom: T4 — slow cooling 88->60C, scale recovers with delay ===\n");
    {
        float prev = 1.0f;
        int relaxStreak = 0;

        for (int i = 0; i < 5; i++) {
            ThermalHeadroomInput in = makeInput(88.0f, -2.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        float hotScale = prev;
        CHECK(hotScale < 0.8f, "T4: after 5 hot beats, smoothed scale dropped");

        float prevBeforeCooling = prev;
        for (int i = 0; i < 3; i++) {
            ThermalHeadroomInput in = makeInput(60.0f, -1.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        CHECK(prev > prevBeforeCooling, "T4: scale starts recovering after cooling");
        CHECK(prev < 1.0f, "T4: not back to full scale yet (hysteresis working)");
    }

    printf("=== C3 Thermal Headroom: T5 — over wall, nearWall, defer to hard wall ===\n");
    {
        HeadroomOutput o = thermalHeadroom(makeInput(86.0f, 0.0f));
        CHECK(o.nearWall == true, "T5: temp > critical -> nearWall=true");
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
        ThermalHeadroomInput in = makeInput(70.0f, 0.0f);
        in.warningWallC = 80.0f;
        in.criticalWallC = 80.0f;
        HeadroomOutput o = thermalHeadroom(in);
        CHECK(o.placementScale == 1.0f, "T7: degenerate walls -> scale=1.0 (fail-open)");
    }

    printf("\n=== C3 Replay: team-fight rapid heating trajectory ===\n");
    {
        float prev = 1.0f;
        int relaxStreak = 0;
        int beatBelow07 = -1;

        for (int i = 0; i < 10; i++) {
            ThermalHeadroomInput in = makeInput(30.0f, 0.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        printf("  Replay: after 10 cool beats, scale = %.3f\n", prev);

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
        CHECK(beatBelow07 <= 8, "Replay: smoothed scale < 0.7 by temp=80C (early warning vs no-slope baseline)");
    }

    printf("\n=== C3 Replay: slow cooling trajectory (no oscillation) ===\n");
    {
        float prev = 1.0f;
        int relaxStreak = 0;
        for (int i = 0; i < 10; i++) {
            ThermalHeadroomInput in = makeInput(82.0f, 1.0f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
        }
        float minScale = prev;
        printf("  Replay: min scale after heating = %.3f\n", minScale);

        bool oscillated = false;
        float prevScale = prev;
        for (int i = 0; i < 20; i++) {
            ThermalHeadroomInput in = makeInput(65.0f, -0.5f);
            HeadroomOutput o = thermalHeadroom(in);
            smoothPlacementScale(o.placementScale, prev, relaxStreak);
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

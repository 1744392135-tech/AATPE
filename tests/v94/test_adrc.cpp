// test_adrc.cpp - v94 hermetic unit tests for adrc.cpp (ADRC control).
// Links adrc.cpp + log_stub (see build.sh recipe).
// Exercises: invalid-input divide-by-zero guard, first-step step response,
// steady-state on-target tracking, clamped output ranges, reset(), and params.
#include <cstdio>
#include <cmath>
#include "adrc.h"

using namespace aatpe::adrc;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static AdrcInput makeInput(double frameUs, double targetUs) {
    AdrcInput in{};
    in.frameTimeUs = frameUs;
    in.targetFrameTimeUs = targetUs;
    in.cpuLoad = 0.3;
    in.thermalLevel = 0.1;     // cool
    in.mlSignal = 0.0;
    in.mlConfidence = 0.5;
    return in;
}

int main() {
    const double kTarget = 16667.0;  // 60fps

    printf("=== ADRC: default params round-trip ===\n");
    AdrcController ctl;
    CHECK(ctl.getParams().h == 0.05, "default sample period h=0.05");
    CHECK(ctl.getParams().b0 == 1.0, "default control gain b0=1.0");
    CHECK(ctl.getParams().r == 50.0, "default tracking speed r=50");

    printf("=== ADRC: invalid input guard (divide-by-zero protection) ===\n");
    {
        AdrcController c;
        AdrcOutput o = c.update(makeInput(0.0, kTarget));
        CHECK(o.urgency == 0.0, "frameTimeUs=0 -> urgency=0 (no divide by zero)");
        CHECK(o.riskScore == 0.0, "frameTimeUs=0 -> riskScore=0");
        CHECK(o.shouldBoost == false, "frameTimeUs=0 -> no boost");
        AdrcOutput o2 = c.update(makeInput(10000.0, 0.0));
        CHECK(o2.urgency == 0.0, "targetFrameTimeUs=0 -> urgency=0");
        AdrcOutput o3 = c.update(makeInput(-5.0, kTarget));
        CHECK(o3.urgency == 0.0, "negative frameTimeUs -> urgency=0");
    }

    printf("=== ADRC: first step response (slow frame -> urgent) ===\n");
    {
        AdrcController c;
        // First valid sample: frame took 2x target -> deviation = target.
        AdrcOutput o = c.update(makeInput(kTarget * 2.0, kTarget));
        CHECK(o.urgency == 1.0, "first slow frame (2x target) -> urgency clamped to 1.0");
        // After the first ESO step the non-zero NLSEF control perturbs z2 slightly,
        // so riskScore is tiny but not exactly 0. Assert it stays negligible and
        // bounded (the disturbance z3 itself is still ~0).
        CHECK(o.riskScore < 0.05, "first sample -> riskScore negligible (no accumulated disturbance)");
        CHECK(o.riskScore >= 0.0 && o.riskScore <= 1.0, "riskScore clamped in [0,1]");
        CHECK(o.shouldBoost == false, "risk negligible on first sample -> boost gated off");
        CHECK(o.predictedFrameTime > 0.0, "predicted frame time is set after valid sample");
    }

    printf("=== ADRC: on-target steady state -> low urgency ===\n");
    {
        AdrcController c;
        AdrcOutput o;
        for (int i = 0; i < 50; i++) {
            o = c.update(makeInput(kTarget, kTarget));
        }
        CHECK(o.urgency < 0.2, "converged on-target -> urgency < 0.2");
        CHECK(o.riskScore >= 0.0 && o.riskScore <= 1.0, "riskScore stays clamped in [0,1]");
        CHECK(o.urgency >= 0.0 && o.urgency <= 1.0, "urgency stays clamped in [0,1]");
    }

    printf("=== ADRC: sustained slow frames raise risk over time ===\n");
    {
        AdrcController c;
        AdrcOutput o;
        for (int i = 0; i < 60; i++) {
            o = c.update(makeInput(kTarget * 1.8, kTarget));
        }
        CHECK(o.urgency > 0.5, "sustained slow frames -> high urgency");
        CHECK(o.riskScore > 0.0, "ESO accumulates disturbance -> riskScore > 0");
    }

    printf("=== ADRC: reset() clears state ===\n");
    {
        AdrcController c;
        c.update(makeInput(kTarget * 2.0, kTarget));
        c.reset();
        AdrcOutput o = c.update(makeInput(kTarget, kTarget));
        CHECK(o.urgency == 0.0, "after reset, on-target first sample -> urgency=0");
    }

    printf("=== ADRC: thermal gate blocks boost when hot ===\n");
    {
        AdrcController c;
        AdrcInput hot = makeInput(kTarget * 2.2, kTarget);
        hot.thermalLevel = 0.9;  // above 0.8 safety gate
        AdrcOutput o;
        for (int i = 0; i < 60; i++) o = c.update(hot);
        CHECK(o.shouldBoost == false, "thermalLevel 0.9 blocks boost despite slow frames");
    }

    printf("=== G5-005: (params) constructor clamps divide-by-zero / instability ===\n");
    {
        // Old constructor stored params raw: b0=0 -> u = u0 - z3/b0 divides by zero.
        AdrcParams bad = kDefaultAdrcParams;
        bad.h = 0.0;
        bad.b0 = 0.0;
        bad.r = 0.0;
        bad.beta01 = 1e9;   // would violate h*beta <= 2 once h is clamped
        AdrcController c(bad);
        AdrcParams p = c.getParams();
        CHECK(p.b0 >= 1e-9, "ctor: b0 clamped away from 0 (no later div-by-zero)");
        CHECK(p.h  >= 1e-6, "ctor: h clamped away from 0");
        CHECK(p.r  >= 1e-6, "ctor: r clamped away from 0");
        // beta must be capped to keep forward-Euler ESO stable.
        CHECK(p.beta01 <= 2.0 / p.h + 1e-9, "ctor: beta01 clamped to h*beta<=2 envelope");
        // And a few update() steps must stay finite (no NaN/inf propagation).
        AdrcOutput o;
        for (int i = 0; i < 20; i++) o = c.update(makeInput(kTarget * 1.5, kTarget));
        CHECK(!std::isnan(o.riskScore) && !std::isinf(o.riskScore),
              "ctor-clamped controller: update() stays finite (no NaN/inf)");
        CHECK(o.riskScore >= 0.0 && o.riskScore <= 1.0, "riskScore still clamped in [0,1]");
    }

    printf("\n========================================\n"
           "ADRC results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

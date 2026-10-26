// thermal_headroom.cpp - C3 continuous thermal-headroom smooth derating.
//
// Per W2 Part C: headroom01 = clamp((critical - temp) / (critical - warning), 0, 1)
// slope correction: headroom_eff = headroom / (1 + slope * 0.5)
// placementScale = 0.4 + 0.6 * headroom_eff
// nearWall: temp >= critical - 2 OR thermalEmergency
// Smooth: EMA alpha=0.2, escalate now, relax after 3 beats above band.
#include "thermal_headroom.h"

#include <algorithm>
#include <cmath>

namespace aatpe {
namespace thermh {

// Tunables per W2 C.3/C.4
static constexpr float kSlopeCorrectionFactor = 0.5f;  // C.3: slope * 0.5
static constexpr float kScaleFloor             = 0.4f;  // C.3: min scale
static constexpr float kScaleRange             = 0.6f;  // C.3: 0.4 + 0.6 * h
static constexpr float kNearWallMarginC       = 2.0f;  // C.3: temp >= critical - 2
static constexpr float kEmaAlpha              = 0.2f;  // C.4: 5-beat convergence
static constexpr float kRelaxBand              = 0.15f; // C.4: raw > smoothed + 0.15
static constexpr int   kRelaxStreakRequired   = 3;      // C.4: 3 beats to relax

HeadroomOutput thermalHeadroom(const ThermalHeadroomInput& in)
{
    HeadroomOutput out{};
    out.headroom01     = 1.0f;
    out.placementScale = 1.0f;
    out.nearWall       = false;
    out.boostAllowed   = true;   // default: boost direction allowed

    // Fail-open for PLACEMENT but fail-safe for BOOST (T-06).
    // - placementScale stays 1.0 on sensor fault: the kernel thermal wall is
    //   authoritative and this layer must not add an extra clamp on top of it.
    // - boostAllowed=false: GPU devfreq floor + schedtune must back off, because
    //   a sensor outage must never be interpreted as "0C = boost hard".
    if (!in.sensorOk) { out.boostAllowed = false; return out; }
    if (in.thermalEmergency) {
        out.nearWall = true;
        out.boostAllowed = false;
        return out;
    }
    // Wall sanity (C.6): if walls are degenerate, fail open.
    float wallSpan = in.criticalWallC - in.warningWallC;
    if (wallSpan <= 0.0f) return out;

    // headroom01 = clamp((critical - temp) / (critical - warning), 0, 1)
    // temp <= warningWall -> headroom = 1; temp >= criticalWall -> headroom = 0
    float hr = (in.criticalWallC - in.tempC) / wallSpan;
    if (hr < 0.0f) hr = 0.0f;
    if (hr > 1.0f) hr = 1.0f;

    // Slope correction (C.3): if warming up, effective headroom shrinks.
    // headroom_eff = headroom / (1 + slope * 0.5)
    float slopeEff = in.slopeCps;
    if (slopeEff < 0.0f) slopeEff = 0.0f; // cooling doesn't boost headroom
    float hrEff = hr / (1.0f + slopeEff * kSlopeCorrectionFactor);
    if (hrEff < 0.0f) hrEff = 0.0f;
    if (hrEff > 1.0f) hrEff = 1.0f;

    out.headroom01 = hrEff;

    // placementScale = 0.4 + 0.6 * headroom_eff
    out.placementScale = kScaleFloor + kScaleRange * hrEff;

    // nearWall: temp within 2 deg of critical, or emergency.
    if (in.tempC >= (in.criticalWallC - kNearWallMarginC)) {
        out.nearWall = true;
    }
    // T-06/T-05: when headroom is <20% (hrEff < 0.2), boost direction must back
    // off even if the sensor is healthy. This mirrors gpu_boost.cpp's own
    // kMinHeadroomPctForBoost=20 gate; both signals agree.
    if (hrEff < 0.2f) {
        out.boostAllowed = false;
    }

    return out;
}

void smoothPlacementScale(float raw, float& prev, int& relaxStreak)
{
    if (prev < 0.0f) prev = 1.0f; // first call: start at full scale

    // Escalate-now: if raw <= prev (temperature rising, scale dropping), apply
    // the EMA immediately — no kRelaxStreakRequired wait on the way down.
    // THM-09: this is still a smoothed EMA (alpha=0.2, ~5-beat convergence), not
    // an instant jump; the hardware/hard-wall is authoritative for fast trips.
    if (raw <= prev) {
        float smoothed = kEmaAlpha * raw + (1.0f - kEmaAlpha) * prev;
        prev = smoothed;
        relaxStreak = 0;
        return;
    }

    // Relax-later: raw > prev (cooling, scale recovering). Only relax after
    // raw has stayed above prev + band for kRelaxStreakRequired consecutive beats.
    if (raw > prev + kRelaxBand) {
        relaxStreak++;
        if (relaxStreak >= kRelaxStreakRequired) {
            float smoothed = kEmaAlpha * raw + (1.0f - kEmaAlpha) * prev;
            prev = smoothed;
            // Don't reset streak here; keep accumulating until raw drops below band.
        }
    } else {
        relaxStreak = 0;
    }
}

#ifdef AATPE_HOST_TEST
void resetForTest()
{
    // smoothPlacementScale state is passed in/out by caller, nothing file-static.
    // This is a no-op kept for API symmetry with C1.
}
#endif

} // namespace thermh
} // namespace aatpe

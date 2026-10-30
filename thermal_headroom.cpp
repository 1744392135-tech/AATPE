#include "thermal_headroom.h"

#include <algorithm>
#include <cmath>

namespace aatpe {
namespace thermh {

static constexpr float kSlopeCorrectionFactor = 0.5f;
static constexpr float kScaleFloor             = 0.4f;
static constexpr float kScaleRange             = 0.6f;
static constexpr float kNearWallMarginC       = 2.0f;
static constexpr float kEmaAlpha              = 0.2f;
static constexpr float kRelaxBand              = 0.15f;
static constexpr int   kRelaxStreakRequired   = 3;

HeadroomOutput thermalHeadroom(const ThermalHeadroomInput& in)
{
    HeadroomOutput out{};
    out.headroom01     = 1.0f;
    out.placementScale = 1.0f;
    out.nearWall       = false;
    out.boostAllowed   = true;

    if (!in.sensorOk) { out.boostAllowed = false; return out; }
    if (in.thermalEmergency) {
        out.nearWall = true;
        out.boostAllowed = false;
        return out;
    }
    float wallSpan = in.criticalWallC - in.warningWallC;
    if (wallSpan <= 0.0f) return out;

    float hr = (in.criticalWallC - in.tempC) / wallSpan;
    if (hr < 0.0f) hr = 0.0f;
    if (hr > 1.0f) hr = 1.0f;

    float slopeEff = in.slopeCps;
    if (slopeEff < 0.0f) slopeEff = 0.0f;
    float hrEff = hr / (1.0f + slopeEff * kSlopeCorrectionFactor);
    if (hrEff < 0.0f) hrEff = 0.0f;
    if (hrEff > 1.0f) hrEff = 1.0f;

    out.headroom01 = hrEff;

    out.placementScale = kScaleFloor + kScaleRange * hrEff;

    if (in.tempC >= (in.criticalWallC - kNearWallMarginC)) {
        out.nearWall = true;
    }
    if (hrEff < 0.2f) {
        out.boostAllowed = false;
    }

    return out;
}

void smoothPlacementScale(float raw, float& prev, int& relaxStreak)
{
    if (prev < 0.0f) prev = 1.0f;

    if (raw <= prev) {
        float smoothed = kEmaAlpha * raw + (1.0f - kEmaAlpha) * prev;
        prev = smoothed;
        relaxStreak = 0;
        return;
    }

    if (raw > prev + kRelaxBand) {
        relaxStreak++;
        if (relaxStreak >= kRelaxStreakRequired) {
            float smoothed = kEmaAlpha * raw + (1.0f - kEmaAlpha) * prev;
            prev = smoothed;
        }
    } else {
        relaxStreak = 0;
    }
}

#ifdef AATPE_HOST_TEST
void resetForTest()
{
}
#endif

}
}

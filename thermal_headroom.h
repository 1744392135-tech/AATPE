#ifndef AATPE_THERMAL_HEADROOM_H
#define AATPE_THERMAL_HEADROOM_H

namespace aatpe {
namespace thermh {

struct ThermalHeadroomInput {
    float tempC;
    float warningWallC;
    float criticalWallC;
    float emergencyWallC;
    float slopeCps;
    float etaWarningS;
    bool  sensorOk;
    bool  thermalEmergency;
};

struct HeadroomOutput {
    float headroom01;
    float placementScale;
    bool  nearWall;
    bool  boostAllowed;
};

HeadroomOutput thermalHeadroom(const ThermalHeadroomInput& in);

void smoothPlacementScale(float raw, float& prev, int& relaxStreak);

#ifdef AATPE_HOST_TEST
void resetForTest();
#endif

}
}

#endif

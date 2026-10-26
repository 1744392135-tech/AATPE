// thermal_headroom.h - C3 continuous thermal-headroom smooth derating core.
//
// Per W2 Part C: map headroom [0,1] to placementScale [0.4,1.0], slope-corrected,
// EMA-smoothed with escalate-now / relax-later hysteresis.
// PURE LOGIC. No sysfs, no global state in the thermalHeadroom() function.
// smoothPlacementScale() carries a small in/out state for EMA + hysteresis.
#ifndef AATPE_THERMAL_HEADROOM_H
#define AATPE_THERMAL_HEADROOM_H

namespace aatpe {
namespace thermh {

// Input to the headroom calculator.
struct ThermalHeadroomInput {
    float tempC;            // current trusted CPU temp
    float warningWallC;     // WARNING wall (normalized)
    float criticalWallC;    // CRITICAL wall
    float emergencyWallC;   // EMERGENCY wall
    float slopeCps;         // therm_ff OLS slope (deg C/s)
    float etaWarningS;      // therm_ff ETA to warning (seconds)
    bool  sensorOk;         // sensor healthy
    bool  thermalEmergency; // hard wall already tripped
};

// Output recommendation.
struct HeadroomOutput {
    float headroom01;      // [0,1]: 1=cool, 0=at CRITICAL
    float placementScale;  // [0.4,1.0]: 1=full perf, 0.4=max derating
    bool  nearWall;         // close to hard wall; defer to wall handler
    // T-06 fail-safe for the BOOST direction. sensorOk=false or thermalEmergency
    // used to fail-open to placementScale=1.0 (no extra clamp). That is correct
    // for placement (the kernel wall is authoritative) but DANGEROUS for boost:
    // a sensor outage must NOT be read as "0C = boost hard". boostAllowed=false
    // tells gpu_boost/schedtune consumers to release devfreq floor + schedtune.
    bool  boostAllowed;
};

// Pure function: compute headroom + raw placement scale recommendation.
// No state, no sysfs. Host-testable.
HeadroomOutput thermalHeadroom(const ThermalHeadroomInput& in);

// Smooth the placement scale output with EMA + escalate-now/relax-later hysteresis.
// prev is in/out: pass the previous smoothed value, get the new one.
// Returns the new smoothed scale.
//   raw:     raw placementScale from thermalHeadroom()
//   prev:    in/out: previous smoothed value (caller stores between calls)
// relaxStreak: in/out: counter for how many beats raw has been above prev+band.
void smoothPlacementScale(float raw, float& prev, int& relaxStreak);

#ifdef AATPE_HOST_TEST
// Test seam.
void resetForTest();
#endif

} // namespace thermh
} // namespace aatpe

#endif // AATPE_THERMAL_HEADROOM_H

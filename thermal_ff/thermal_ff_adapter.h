#ifndef AATPE_THERMAL_FF_ADAPTER_H
#define AATPE_THERMAL_FF_ADAPTER_H

#include "thermal_ff.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline ThermHardWall aatpe_therm_ff_map_wall(bool emergency,
                                                    bool criticalActive,
                                                    bool warningActive)
{
    if (emergency)      return THERM_HW_EMERGENCY;
    if (criticalActive) return THERM_HW_CRITICAL;
    if (warningActive)  return THERM_HW_WARNING;
    return THERM_HW_NONE;
}

static inline FfGear aatpe_therm_ff_final_gear(FfGear wall_gear, FfGear ff_gear)
{
    return (ff_gear > wall_gear) ? ff_gear : wall_gear;
}

static inline bool aatpe_therm_ff_pre_throttle(FfGear final_gear, FfGear wall_gear)
{
    return final_gear > wall_gear;
}

#ifdef __cplusplus
}
#endif

#endif

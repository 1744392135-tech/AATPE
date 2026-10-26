/*
 * thermal_ff_adapter.h - v4.3.6 thermal feedforward wiring adapter.
 *
 * PURE / host-testable glue between the existing reactive thermal state machine
 * (thermal.cpp WARNING/CRITICAL/EMERGENCY hard wall) and the verified,
 * logic-frozen predictor core (thermal_ff.h/.c).
 *
 * This header intentionally adds NO prediction logic of its own. It only:
 *   1. maps the daemon's existing reactive wall state into the core's
 *      ThermHardWall passthrough enum;
 *   2. applies the actuator composition contract  final = max(wall_gear, ff_gear)
 *      (EMERGENCY hard wall always authoritative; feedforward can only ease load
 *      EARLY, never cancel a wall);
 *   3. exposes the "feedforward is leading" pre-throttle bit.
 *
 * Included by BOTH thermal.cpp (production) and the host regression test, so
 * the regression exercises the EXACT composition the daemon runs -- not a copy.
 */
#ifndef AATPE_THERMAL_FF_ADAPTER_H
#define AATPE_THERMAL_FF_ADAPTER_H

#include "thermal_ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Map the existing reactive thermal state machine (thermal.cpp) onto the core's
 * passthrough hard-wall level. EMERGENCY wins over CRITICAL over WARNING by
 * construction (the highest active wall dictates the level), matching the
 * daemon's own precedence where EMERGENCY is the only global override.
 */
static inline ThermHardWall aatpe_therm_ff_map_wall(bool emergency,
                                                    bool criticalActive,
                                                    bool warningActive)
{
    if (emergency)      return THERM_HW_EMERGENCY;
    if (criticalActive) return THERM_HW_CRITICAL;
    if (warningActive)  return THERM_HW_WARNING;
    return THERM_HW_NONE;
}

/*
 * Actuator landing: final gear = max(hard_wall_gear, feedforward_gear).
 *
 * RED LINE: the EMERGENCY hard wall maps to FF_GEAR_SEVERE (=3) and the
 * feedforward gear can never exceed 3, so whenever the wall is EMERGENCY the
 * final gear is clamped to SEVERE regardless of what the predictor proposed.
 * Feedforward may RAISE an early pre-throttle (ease load before the wall trips)
 * but can NEVER LOWER a wall.
 */
static inline FfGear aatpe_therm_ff_final_gear(FfGear wall_gear, FfGear ff_gear)
{
    return (ff_gear > wall_gear) ? ff_gear : wall_gear;
}

/*
 * Pre-throttle bit: true only when the feedforward gear is LEADING the reactive
 * wall (i.e. we are easing load BEFORE any wall has tripped). When the wall is
 * already active, final_gear == wall_gear and this is false -- the wall, not the
 * predictor, is driving. This is the "提前缓载" (early easing) signal.
 */
static inline bool aatpe_therm_ff_pre_throttle(FfGear final_gear, FfGear wall_gear)
{
    return final_gear > wall_gear;
}

#ifdef __cplusplus
}
#endif

#endif /* AATPE_THERMAL_FF_ADAPTER_H */

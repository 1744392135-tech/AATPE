/*
 * thermal_ff.h - temperature trend FEEDFORWARD prediction core.
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 *
 * Maintains an EWMA-smoothed temperature level and a sliding-window linear
 * slope (deg C/s), and predicts the time-to-threshold (ETA) to the existing
 * WARNING / EMERGENCY trips. It then proposes a *pre-throttle* gear to begin
 * easing load BEFORE the reactive wall trips.
 *
 * Coordination contract (IMPORTANT):
 *   This module does NOT replace the existing reactive thermal state machine
 *   (thermal.cpp WARNING/CRITICAL/EMERGENCY hard wall). It reports the hard
 *   wall through unchanged and emits a purely additive feedforward gear. The
 *   daemon actuator MUST apply max(hard_wall_gear, feedforward_gear); the
 *   EMERGENCY hard wall stays authoritative. The feedforward gear can never
 *   cancel a hard wall.
 *
 * Failure contract:
 *   - NaN / out-of-range / repeated-bad samples -> sensor_ok=0, slope=0,
 *     ETA = +infinity (no prediction), gear = FF_NONE (fail safe: rely on
 *     the reactive wall, never inject garbage pre-throttle).
 *   - Stuck sensor (constant temp) -> slope ~ 0 -> ETA infinite -> FF_NONE.
 *   - Cooling (slope < 0) -> ETA infinite -> gear relaxes.
 */
#ifndef AATPE_THERMAL_FF_H
#define AATPE_THERMAL_FF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define THERM_FF_MAX_WINDOW 16u

/* Passthrough of the existing reactive state machine (thermal.cpp:38-46). */
typedef enum {
    THERM_HW_NONE = 0,
    THERM_HW_WARNING = 1,
    THERM_HW_CRITICAL = 2,
    THERM_HW_EMERGENCY = 3
} ThermHardWall;

/* Feedforward pre-throttle suggestion gear (additive, below the wall). */
typedef enum {
    FF_GEAR_NONE = 0,
    FF_GEAR_LIGHT = 1,   /* start easing before WARNING trips */
    FF_GEAR_MODERATE = 2,
    FF_GEAR_SEVERE = 3  /* imminent; nearly at WARNING */
} FfGear;

typedef struct {
    /* Thresholds copied from the existing thermal config (Celsius). */
    float warning_temp_c;    /* e.g. 75.0 */
    float emergency_temp_c;  /* e.g. 95.0 */

    float ewma_alpha;        /* level smoothing 0..1. default 0.30 */
    uint32_t window;         /* OLS sample window (<= MAX_WINDOW). default 10 */

    float min_slope_eps;     /* |slope| below this => stable. default 0.02 C/s */
    float max_slope_abs;     /* slope clamp. default 3.0 C/s */
    float outlier_delta_c;   /* reject jump > this from EWMA. default 12.0 C */

    /* ETA (seconds) at which to climb the feedforward gear. */
    float eta_light_s;       /* default 8.0 */
    float eta_moderate_s;    /* default 4.0 */
    float eta_severe_s;      /* default 2.0 */
    float gear_exit_band_s;  /* hysteresis: relax gear only after eta exceeds
                                (enter_threshold + band). default 2.0 s */

    float max_horizon_s;     /* clamp reported ETA. default 120.0 */
    uint32_t max_consec_bad; /* after this many bad samples => sensor fault.
                                default 5 */
} ThermFfConfig;

typedef struct {
    float    temp[THERM_FF_MAX_WINDOW]; /* EWMA-smoothed samples */
    double   tms[THERM_FF_MAX_WINDOW];  /* monotonic ms of each sample */
    uint32_t n;                        /* samples currently in window */
    uint32_t head;                     /* next write index */
    float    ewma;
    int      have_ewma;
    uint32_t consec_bad;
    float    slope_cps;
    FfGear   gear;                     /* hysteresis-held gear */
    int      sensor_ok;
    double   last_ms;                  /* newest accepted sample timestamp */
} ThermFfState;

typedef struct {
    float    slope_cps;         /* smoothed rate, clamped [-max,max] */
    float    eta_warning_s;     /* seconds to warning_temp; +inf if cooling/stable
                                   or sensor fault. 0.0 if already at/above. */
    float    eta_emergency_s;   /* seconds to emergency_temp */
    FfGear   ff_gear;           /* additive pre-throttle suggestion */
    FfGear   wall_gear;          /* hard wall mapped to gear (max of 0..3) */
    int      sensor_ok;          /* 0 = sensor fault, ignore feedforward */
    ThermHardWall hard_wall_in;  /* passthrough */
} ThermFfOut;

void therm_ff_config_default(ThermFfConfig *cfg);
void therm_ff_state_init(ThermFfState *st);

/*
 * Advance the predictor by one sample.
 *  now_ms    : monotonic timestamp (ms). <=0 or non-monotonic => sample skipped.
 *  temp_c    : measured temperature; NaN/inf/out-of-range => bad sample.
 *  hard_wall : current reactive wall level (passthrough).
 */
void therm_ff_update(ThermFfState *st, const ThermFfConfig *cfg,
                     double now_ms, float temp_c, ThermHardWall hard_wall,
                     ThermFfOut *out);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_THERMAL_FF_H */

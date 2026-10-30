#ifndef AATPE_THERMAL_FF_H
#define AATPE_THERMAL_FF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define THERM_FF_MAX_WINDOW 16u

typedef enum {
    THERM_HW_NONE = 0,
    THERM_HW_WARNING = 1,
    THERM_HW_CRITICAL = 2,
    THERM_HW_EMERGENCY = 3
} ThermHardWall;

typedef enum {
    FF_GEAR_NONE = 0,
    FF_GEAR_LIGHT = 1,
    FF_GEAR_MODERATE = 2,
    FF_GEAR_SEVERE = 3
} FfGear;

typedef struct {

    float warning_temp_c;
    float emergency_temp_c;

    float ewma_alpha;
    uint32_t window;

    float min_slope_eps;
    float max_slope_abs;
    float outlier_delta_c;

    float eta_light_s;
    float eta_moderate_s;
    float eta_severe_s;
    float gear_exit_band_s;

    float max_horizon_s;
    uint32_t max_consec_bad;
} ThermFfConfig;

typedef struct {
    float    temp[THERM_FF_MAX_WINDOW];
    double   tms[THERM_FF_MAX_WINDOW];
    uint32_t n;
    uint32_t head;
    float    ewma;
    int      have_ewma;
    uint32_t consec_bad;
    float    slope_cps;
    FfGear   gear;
    int      sensor_ok;
    double   last_ms;
} ThermFfState;

typedef struct {
    float    slope_cps;
    float    eta_warning_s;
    float    eta_emergency_s;
    FfGear   ff_gear;
    FfGear   wall_gear;
    int      sensor_ok;
    ThermHardWall hard_wall_in;
} ThermFfOut;

void therm_ff_config_default(ThermFfConfig *cfg);
void therm_ff_state_init(ThermFfState *st);

void therm_ff_update(ThermFfState *st, const ThermFfConfig *cfg,
                     double now_ms, float temp_c, ThermHardWall hard_wall,
                     ThermFfOut *out);

#ifdef __cplusplus
}
#endif

#endif

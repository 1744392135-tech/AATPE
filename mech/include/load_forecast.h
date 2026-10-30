#ifndef AATPE_LOAD_FORECAST_H
#define AATPE_LOAD_FORECAST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define LF_MAX_FORECAST_WINDOWS 8

typedef struct {
    float cpu_util_pct;
    float gpu_util_pct;
    float frame_time_ms;
    float psi_cpu_avg10;
} LoadObservation;

typedef struct {

    float cpu_forecast[LF_MAX_FORECAST_WINDOWS];
    float gpu_forecast[LF_MAX_FORECAST_WINDOWS];
    float frame_forecast[LF_MAX_FORECAST_WINDOWS];

    uint32_t num_windows;

    float confidence;

    int trend_rising;
    int trend_falling;

    int boost_hint;
    int deboost_hint;
} LoadForecast;

typedef struct {

    float level_alpha;
    float trend_beta;

    uint32_t horizon_windows;

    uint32_t min_samples;

    float cpu_boost_threshold;
    float cpu_deboost_threshold;
    float trend_rising_margin;
    float trend_falling_margin;

    float max_util_pct;
    float min_frame_ms;
    float max_frame_ms;
} LoadForecastConfig;

typedef struct {

    float cpu_level;
    float cpu_trend;

    float gpu_level;
    float gpu_trend;

    float frame_level;
    float frame_trend;

    uint32_t valid_samples;
    int      initialised;
} LoadForecastState;

void load_forecast_config_default(LoadForecastConfig *cfg);
void load_forecast_state_init(LoadForecastState *st);

void load_forecast_step(const LoadObservation *obs,
                         const LoadForecastConfig *cfg,
                         LoadForecastState *st,
                         LoadForecast *out);

#ifdef __cplusplus
}
#endif

#endif

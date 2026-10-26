/*
 * load_forecast.h - Short-horizon load prediction (pure logic).
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 *
 * Predicts future CPU/GPU/frame-load using EWMA + linear trend (Holt's method).
 * No DRL, no deep model, no network. Just smoothing and trend extrapolation.
 *
 * Input : recent load observations per tick (CPU util, GPU util, frame time, PSI).
 * Output: predicted load for the next N windows, with confidence.
 *         Drives *proactive* boost / de-boost decisions.
 *
 * Fail-safe:
 *   - Non-finite (NaN/inf) samples are skipped, never poison the forecast.
 *   - Fewer than min_samples observations => confidence = 0, prediction = last
 *     valid value (or neutral if no valid value yet).
 *   - All predictions clamped to physical bounds.
 *
 * Design basis:
 *   - Blueprint 短板-4: no load forecast, all reactive. This core provides
 *     short-horizon prediction (1-5 ticks ahead) for proactive boosting.
 *   - Holt's linear method: level EWMA + trend EWMA. Simple, robust,
 *     O(1) per tick, no matrix inversion.
 */
#ifndef AATPE_LOAD_FORECAST_H
#define AATPE_LOAD_FORECAST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define LF_MAX_FORECAST_WINDOWS 8

/* ------- observation sample (one tick) ------- */
typedef struct {
    float cpu_util_pct;     /* [0..100] */
    float gpu_util_pct;     /* [0..100] */
    float frame_time_ms;    /* actual frame period observed */
    float psi_cpu_avg10;    /* [0..100], PSI cpu some avg10 */
} LoadObservation;

/* ------- forecast output ------- */
typedef struct {
    /* Predicted load for window k (k=0 is next tick, k=1 is +1 tick, etc.) */
    float cpu_forecast[LF_MAX_FORECAST_WINDOWS];
    float gpu_forecast[LF_MAX_FORECAST_WINDOWS];
    float frame_forecast[LF_MAX_FORECAST_WINDOWS];

    /* How many windows are valid (0..LF_MAX_FORECAST_WINDOWS). */
    uint32_t num_windows;

    /* Overall confidence [0..1]. 0 = no data / unreliable. */
    float confidence;

    /* Trend direction: +1 = load rising, 0 = flat, -1 = falling. */
    int trend_rising;
    int trend_falling;

    /* Suggested proactive action:
     *   boost_hint   : +1 = should boost ahead of predicted load
     *   deboost_hint : +1 = should de-boost (load predicted to drop)
     */
    int boost_hint;
    int deboost_hint;
} LoadForecast;

/* ------- tunables ------- */
typedef struct {
    /* Holt's method smoothing constants. */
    float level_alpha;      /* EWMA alpha for level. default 0.4 */
    float trend_beta;       /* EWMA beta for trend. default 0.2 */

    /* Prediction horizon: how many windows ahead to forecast. */
    uint32_t horizon_windows;  /* default 4 */

    /* Minimum valid samples before forecast is trustworthy. */
    uint32_t min_samples;       /* default 5 */

    /* Thresholds for boost/de-boost hints. */
    float cpu_boost_threshold;  /* predicted CPU >= this => boost. default 70.0 */
    float cpu_deboost_threshold; /* predicted CPU <= this => deboost. default 30.0 */
    float trend_rising_margin;  /* trend > this => rising. default 2.0 (%/tick) */
    float trend_falling_margin;  /* trend < -this => falling. default 2.0 (%/tick) */

    /* Bounds for clamping. */
    float max_util_pct;         /* default 100.0 */
    float min_frame_ms;         /* default 1.0 */
    float max_frame_ms;         /* default 100.0 */
} LoadForecastConfig;

/* ------- persistent state (caller owns) ------- */
typedef struct {
    /* Holt's state for CPU */
    float cpu_level;
    float cpu_trend;
    /* Holt's state for GPU */
    float gpu_level;
    float gpu_trend;
    /* Holt's state for frame time */
    float frame_level;
    float frame_trend;

    uint32_t valid_samples;    /* count of non-NaN observations ingested */
    int      initialised;      /* 1 after first valid observation */
} LoadForecastState;

/* ------- API ------- */
void load_forecast_config_default(LoadForecastConfig *cfg);
void load_forecast_state_init(LoadForecastState *st);

/*
 * Feed one observation tick and produce a forecast.
 *   obs : current observation (must be non-NULL).
 *   cfg : configuration (must be non-NULL, defaulted/overridden).
 *   st  : persistent state (must be non-NULL, initialised once).
 *   out : forecast written here (must be non-NULL).
 *
 * Non-finite observation fields are skipped safely.
 */
void load_forecast_step(const LoadObservation *obs,
                         const LoadForecastConfig *cfg,
                         LoadForecastState *st,
                         LoadForecast *out);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_LOAD_FORECAST_H */

#include "load_forecast.h"

#include <math.h>

static float sanf_util(float v)
{
    if (!isfinite(v)) return -1.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

static float sanf_frame(float v)
{
    if (!isfinite(v)) return -1.0f;
    if (v < 0.0f) return -1.0f;
    return v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void load_forecast_config_default(LoadForecastConfig *cfg)
{
    if (!cfg) return;
    cfg->level_alpha        = 0.4f;
    cfg->trend_beta         = 0.2f;
    cfg->horizon_windows    = 4;
    cfg->min_samples        = 5;
    cfg->cpu_boost_threshold  = 70.0f;
    cfg->cpu_deboost_threshold = 30.0f;
    cfg->trend_rising_margin   = 2.0f;
    cfg->trend_falling_margin  = 2.0f;
    cfg->max_util_pct         = 100.0f;
    cfg->min_frame_ms          = 1.0f;
    cfg->max_frame_ms          = 100.0f;
}

void load_forecast_state_init(LoadForecastState *st)
{
    if (!st) return;
    st->cpu_level = 0.0f;
    st->cpu_trend = 0.0f;
    st->gpu_level = 0.0f;
    st->gpu_trend = 0.0f;
    st->frame_level = 0.0f;
    st->frame_trend = 0.0f;
    st->valid_samples = 0;
    st->initialised = 0;
}

static void holt_update(float x, float alpha, float beta,
                         float *level, float *trend)
{
    float prev_level = *level;
    float prev_trend = *trend;

    float new_level = alpha * x + (1.0f - alpha) * (prev_level + prev_trend);
    float new_trend = beta * (new_level - prev_level) + (1.0f - beta) * prev_trend;

    *level = new_level;
    *trend = new_trend;
}

void load_forecast_step(const LoadObservation *obs,
                         const LoadForecastConfig *cfg,
                         LoadForecastState *st,
                         LoadForecast *out)
{

    if (out) {
        for (uint32_t i = 0; i < LF_MAX_FORECAST_WINDOWS; i++) {
            out->cpu_forecast[i]   = 0.0f;
            out->gpu_forecast[i]   = 0.0f;
            out->frame_forecast[i] = 0.0f;
        }
        out->num_windows    = 0;
        out->confidence     = 0.0f;
        out->trend_rising   = 0;
        out->trend_falling  = 0;
        out->boost_hint     = 0;
        out->deboost_hint   = 0;
    }
    if (!obs || !cfg || !st || !out) return;

    float cpu  = sanf_util(obs->cpu_util_pct);
    float gpu  = sanf_util(obs->gpu_util_pct);
    float frame = sanf_frame(obs->frame_time_ms);
    (void)sanf_util(obs->psi_cpu_avg10);

    int any_valid = (cpu >= 0.0f) || (gpu >= 0.0f) || (frame >= 0.0f);
    if (any_valid) {
        st->valid_samples++;
        st->initialised = 1;
    }

    if (cpu >= 0.0f)
        holt_update(cpu, cfg->level_alpha, cfg->trend_beta,
                    &st->cpu_level, &st->cpu_trend);
    if (gpu >= 0.0f)
        holt_update(gpu, cfg->level_alpha, cfg->trend_beta,
                    &st->gpu_level, &st->gpu_trend);
    if (frame >= 0.0f)
        holt_update(frame, cfg->level_alpha, cfg->trend_beta,
                    &st->frame_level, &st->frame_trend);

    if (st->valid_samples < cfg->min_samples) {
        out->confidence = 0.0f;

        uint32_t n = (cfg->horizon_windows < LF_MAX_FORECAST_WINDOWS)
                     ? cfg->horizon_windows : LF_MAX_FORECAST_WINDOWS;
        out->num_windows = n;
        for (uint32_t k = 0; k < n; k++) {
            out->cpu_forecast[k]   = clampf(st->cpu_level, 0.0f, cfg->max_util_pct);
            out->gpu_forecast[k]   = clampf(st->gpu_level, 0.0f, cfg->max_util_pct);
            out->frame_forecast[k] = clampf(st->frame_level,
                                             cfg->min_frame_ms, cfg->max_frame_ms);
        }
        return;
    }

    uint32_t horizon = cfg->horizon_windows;
    if (horizon > LF_MAX_FORECAST_WINDOWS) horizon = LF_MAX_FORECAST_WINDOWS;
    if (horizon < 1u) horizon = 1u;
    out->num_windows = horizon;

    for (uint32_t k = 0; k < horizon; k++) {
        float hk = (float)(k + 1);
        out->cpu_forecast[k] = clampf(st->cpu_level + hk * st->cpu_trend,
                                       0.0f, cfg->max_util_pct);
        out->gpu_forecast[k] = clampf(st->gpu_level + hk * st->gpu_trend,
                                       0.0f, cfg->max_util_pct);
        out->frame_forecast[k] = clampf(st->frame_level + hk * st->frame_trend,
                                         cfg->min_frame_ms, cfg->max_frame_ms);
    }

    if (cfg->min_samples <= 0) {
        out->confidence = 0.0f;
    } else {
        float conf = (float)(st->valid_samples - cfg->min_samples) /
                     (float)(cfg->min_samples * 2);
        out->confidence = clampf(conf, 0.0f, 1.0f);
    }

    if (st->cpu_trend > cfg->trend_rising_margin ||
        st->gpu_trend > cfg->trend_rising_margin)
        out->trend_rising = 1;
    if (st->cpu_trend < -cfg->trend_falling_margin ||
        st->gpu_trend < -cfg->trend_falling_margin)
        out->trend_falling = 1;

    float far_cpu = out->cpu_forecast[horizon - 1];
    if (far_cpu >= cfg->cpu_boost_threshold && out->trend_rising)
        out->boost_hint = 1;
    if (far_cpu <= cfg->cpu_deboost_threshold && out->trend_falling)
        out->deboost_hint = 1;
}

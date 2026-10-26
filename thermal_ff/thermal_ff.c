#include "thermal_ff.h"

#include <math.h>

/* THM-13: fallback horizon seconds used only when cfg is NULL (never in
   production). Kept as a named constant instead of a magic literal. */
#define THERM_FF_FALLBACK_HORIZON_S 120.0f

static FfGear wall_to_gear(ThermHardWall w)
{
    switch (w) {
    case THERM_HW_WARNING: return FF_GEAR_LIGHT;
    case THERM_HW_CRITICAL: return FF_GEAR_MODERATE;
    case THERM_HW_EMERGENCY: return FF_GEAR_SEVERE;
    default: return FF_GEAR_NONE;
    }
}

void therm_ff_config_default(ThermFfConfig *cfg)
{
    if (!cfg) return;
    cfg->warning_temp_c = 75.0f;
    cfg->emergency_temp_c = 95.0f;
    cfg->ewma_alpha = 0.30f;
    cfg->window = 10u;
    cfg->min_slope_eps = 0.02f;
    cfg->max_slope_abs = 3.0f;
    cfg->outlier_delta_c = 12.0f;
    cfg->eta_light_s = 8.0f;
    cfg->eta_moderate_s = 4.0f;
    cfg->eta_severe_s = 2.0f;
    cfg->gear_exit_band_s = 2.0f;
    cfg->max_horizon_s = 120.0f;
    cfg->max_consec_bad = 5u;
}

void therm_ff_state_init(ThermFfState *st)
{
    if (!st) return;
    for (uint32_t i = 0; i < THERM_FF_MAX_WINDOW; i++) {
        st->temp[i] = 0.0f;
        st->tms[i] = 0.0;
    }
    st->n = 0;
    st->head = 0;
    st->ewma = 0.0f;
    st->have_ewma = 0;
    st->consec_bad = 0;
    st->slope_cps = 0.0f;
    st->gear = FF_GEAR_NONE;
    st->sensor_ok = 1;
    st->last_ms = 0.0;
}

/* Ordinary least-squares slope (deg C/s) over the window. Returns 0 when the
 * sample count is too small or the time spread is degenerate (all equal). */
static float ols_slope(const ThermFfState *st)
{
    if (st->n < 2u) return 0.0f;

    double tbar = 0.0, ybar = 0.0;
    for (uint32_t i = 0; i < st->n; i++) {
        tbar += st->tms[i];
        ybar += st->temp[i];
    }
    tbar /= (double)st->n;
    ybar /= (double)st->n;

    double num = 0.0, den = 0.0;
    for (uint32_t i = 0; i < st->n; i++) {
        double dt = st->tms[i] / 1000.0 - tbar / 1000.0;
        double dy = (double)st->temp[i] - ybar;
        num += dt * dy;
        den += dt * dt;
    }
    if (den <= 1e-9) return 0.0f; /* all samples at the same time */
    double slope = num / den;
    if (!isfinite(slope)) return 0.0f;
    return (float)slope;
}

void therm_ff_update(ThermFfState *st, const ThermFfConfig *cfg,
                     double now_ms, float temp_c, ThermHardWall hard_wall,
                     ThermFfOut *out)
{
    /* default out (bounded) */
    if (out) {
        out->slope_cps = 0.0f;
        out->eta_warning_s = cfg ? cfg->max_horizon_s : THERM_FF_FALLBACK_HORIZON_S;
        out->eta_emergency_s = cfg ? cfg->max_horizon_s : THERM_FF_FALLBACK_HORIZON_S;
        out->ff_gear = FF_GEAR_NONE;
        out->wall_gear = wall_to_gear(hard_wall);
        out->sensor_ok = 1;
        out->hard_wall_in = hard_wall;
    }
    if (!st || !cfg || !out) return;

    uint32_t win = cfg->window;
    if (win < 2u) win = 2u;
    if (win > THERM_FF_MAX_WINDOW) win = THERM_FF_MAX_WINDOW;

    /* THM-07: if the configured window shrinks at runtime below the number of
       samples already in the ring, reset the ring so stale samples at indices
       >= win are not fed to the OLS slope. Production sets window once; this
       is a defensive reset. */
    if (st->n > win) { st->n = 0; st->head = 0; }

    int good = 1;

    /* sample validity: NaN/inf or physically absurd range */
    if (!isfinite(temp_c) || temp_c < -40.0f || temp_c > 200.0f) good = 0;
    /* monotonic time */
    if (!isfinite(now_ms) || now_ms <= st->last_ms) good = 0;

    if (good && st->have_ewma &&
        fabsf(temp_c - st->ewma) > cfg->outlier_delta_c) {
        /* reject a wild jump as a sensor spike, not a real trend change */
        good = 0;
    }

    if (!good) {
        st->consec_bad++;
        if (st->consec_bad >= cfg->max_consec_bad) {
            st->sensor_ok = 0;
            st->gear = FF_GEAR_NONE; /* fail safe */
        }
    } else {
        st->consec_bad = 0;
        st->sensor_ok = 1;

        if (!st->have_ewma) {
            st->ewma = temp_c;
            st->have_ewma = 1;
        } else {
            st->ewma = cfg->ewma_alpha * temp_c + (1.0f - cfg->ewma_alpha) * st->ewma;
        }

        st->temp[st->head] = st->ewma;
        st->tms[st->head] = now_ms;
        st->head = (st->head + 1u) % win;
        if (st->n < win) st->n++;
        st->last_ms = now_ms;
    }

    /* --- slope --- */
    float slope = ols_slope(st);
    if (slope > cfg->max_slope_abs) slope = cfg->max_slope_abs;
    if (slope < -cfg->max_slope_abs) slope = -cfg->max_slope_abs;
    st->slope_cps = slope;

    /* --- ETA to threshold (bounded; cooling/stable/fault => horizon) --- */
    float horizon = cfg->max_horizon_s;
    float eta_w = horizon;
    float eta_e = horizon;

    if (st->sensor_ok && st->have_ewma && slope > cfg->min_slope_eps) {
        float cur = st->ewma;
        if (cur >= cfg->warning_temp_c) eta_w = 0.0f;
        else {
            float e = (cfg->warning_temp_c - cur) / slope;
            eta_w = (e < 0.0f) ? 0.0f : (e > horizon ? horizon : e);
        }
        if (cur >= cfg->emergency_temp_c) eta_e = 0.0f;
        else {
            float e = (cfg->emergency_temp_c - cur) / slope;
            eta_e = (e < 0.0f) ? 0.0f : (e > horizon ? horizon : e);
        }
    }

    /* --- feedforward gear with escalate-now / relax-later hysteresis --- */
    FfGear desired = FF_GEAR_NONE;
    if (st->sensor_ok) {
        if (eta_w <= cfg->eta_severe_s) desired = FF_GEAR_SEVERE;
        else if (eta_w <= cfg->eta_moderate_s) desired = FF_GEAR_MODERATE;
        else if (eta_w <= cfg->eta_light_s) desired = FF_GEAR_LIGHT;
        else desired = FF_GEAR_NONE;
    }

    if (!st->sensor_ok) {
        st->gear = FF_GEAR_NONE;
    } else if (desired > st->gear) {
        st->gear = desired; /* escalate immediately */
    } else if (desired < st->gear) {
        /* relax only once ETA has grown comfortably past the current gear's
         * entry threshold + exit band -> prevents threshold flapping. */
        float relax_at;
        switch (st->gear) {
        case FF_GEAR_LIGHT: relax_at = cfg->eta_light_s + cfg->gear_exit_band_s; break;
        case FF_GEAR_MODERATE: relax_at = cfg->eta_moderate_s + cfg->gear_exit_band_s; break;
        case FF_GEAR_SEVERE: relax_at = cfg->eta_severe_s + cfg->gear_exit_band_s; break;
        default: relax_at = 0.0f; break;
        }
        if (eta_w > relax_at) st->gear = desired;
    }

    out->slope_cps = st->slope_cps;
    out->eta_warning_s = eta_w;
    out->eta_emergency_s = eta_e;
    out->ff_gear = st->gear;
    out->wall_gear = wall_to_gear(hard_wall);
    out->sensor_ok = st->sensor_ok;
    out->hard_wall_in = hard_wall;
}

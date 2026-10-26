#include "context.h"

#include <math.h>

static float sanf_hours(float v)
{
    if (!isfinite(v)) return 12.0f; /* default to noon */
    if (v < 0.0f) return 0.0f;
    if (v >= 24.0f) return 23.999f;
    return v;
}

static float sanf_pct(float v)
{
    if (!isfinite(v)) return 50.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

static float sanf_ms(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    return v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void ctx_config_default(CtxConfig *cfg)
{
    if (!cfg) return;
    cfg->day_start_hour      = 7.0f;
    cfg->evening_start_hour  = 19.0f;
    cfg->night_start_hour    = 23.0f;
    cfg->battery_high_pct    = 50.0f;
    cfg->battery_low_pct     = 20.0f;
    cfg->idle_timeout_ms      = 30000.0f;   /* 30 seconds */
    cfg->long_session_ms      = 1800000.0f; /* 30 minutes */
    cfg->night_efficiency_bias = -0.2f;
    cfg->low_battery_bias      = -0.4f;
    cfg->charging_perf_bias    = 0.1f;
}

void ctx_state_init(CtxState *st)
{
    if (!st) return;
    st->screen_on_elapsed_ms = 0.0f;
    st->session_elapsed_ms   = 0.0f;
    st->was_screen_on        = 0;
}

static CtxTimeOfDay classify_time(float hour, const CtxConfig *c)
{
    if (hour >= c->night_start_hour || hour < c->day_start_hour)
        return CTX_TIME_NIGHT;
    if (hour >= c->evening_start_hour)
        return CTX_TIME_EVENING;
    return CTX_TIME_DAY;
}

static CtxBatteryState classify_battery(float pct, int charging,
                                         const CtxConfig *c)
{
    if (charging) return CTX_BATTERY_CHARGING;
    if (pct <= c->battery_low_pct) return CTX_BATTERY_LOW;
    if (pct >= c->battery_high_pct) return CTX_BATTERY_HIGH;
    return CTX_BATTERY_MEDIUM;
}

static CtxSessionState classify_session(int screen_on, float idle_ms,
                                         const CtxConfig *c)
{
    if (!screen_on) return CTX_SESSION_SCREEN_OFF;
    if (idle_ms > c->idle_timeout_ms) return CTX_SESSION_IDLE;
    return CTX_SESSION_ACTIVE;
}

const char *ctx_time_name(CtxTimeOfDay t)
{
    switch (t) {
    case CTX_TIME_DAY:     return "DAY";
    case CTX_TIME_EVENING: return "EVENING";
    case CTX_TIME_NIGHT:    return "NIGHT";
    case CTX_TIME_UNKNOWN:
    default:               return "UNKNOWN";
    }
}

const char *ctx_battery_name(CtxBatteryState b)
{
    switch (b) {
    case CTX_BATTERY_CHARGING: return "CHARGING";
    case CTX_BATTERY_HIGH:      return "BATTERY_HIGH";
    case CTX_BATTERY_MEDIUM:   return "BATTERY_MEDIUM";
    case CTX_BATTERY_LOW:      return "BATTERY_LOW";
    case CTX_BATTERY_UNKNOWN:
    default:                   return "UNKNOWN";
    }
}

const char *ctx_session_name(CtxSessionState s)
{
    switch (s) {
    case CTX_SESSION_ACTIVE:    return "ACTIVE";
    case CTX_SESSION_IDLE:      return "IDLE";
    case CTX_SESSION_SCREEN_OFF: return "SCREEN_OFF";
    case CTX_SESSION_UNKNOWN:
    default:                    return "UNKNOWN";
    }
}

void ctx_decide(const CtxSample *s, const CtxConfig *cfg,
                CtxState *st, CtxDecision *out)
{
    /* Zero output. */
    if (out) {
        out->time_of_day       = CTX_TIME_UNKNOWN;
        out->battery_state     = CTX_BATTERY_UNKNOWN;
        out->session_state     = CTX_SESSION_UNKNOWN;
        out->efficiency_bias   = 0.0f;
        out->boost_bias        = 0.0f;
        out->night_mode_suggested  = 0;
        out->battery_saver_suggested = 0;
        out->long_session_warning  = 0;
    }
    if (!s || !cfg || !st || !out) return;

    /* Sanitise inputs. */
    float hour     = sanf_hours(s->hours_of_day);
    float batt     = sanf_pct(s->battery_pct);
    float idle_ago = sanf_ms(s->last_interaction_ago_ms);
    float dt        = sanf_ms(s->tick_dt_ms);
    int   scr_on    = s->screen_on ? 1 : 0;
    int   charging  = s->charging ? 1 : 0;

    /* --- classify --- */
    out->time_of_day   = classify_time(hour, cfg);
    out->battery_state = classify_battery(batt, charging, cfg);
    out->session_state = classify_session(scr_on, idle_ago, cfg);

    /* --- track session timing --- */
    if (scr_on) {
        st->screen_on_elapsed_ms += dt;
        if (out->session_state == CTX_SESSION_ACTIVE) {
            st->session_elapsed_ms += dt;
        }
    } else {
        /* Screen off: reset elapsed timers. */
        st->screen_on_elapsed_ms = 0.0f;
        st->session_elapsed_ms   = 0.0f;
    }

    /* --- compute gentle biases --- */
    float eff_bias = 0.0f;
    float boost_b  = 0.0f;

    /* Night: lean slightly toward efficiency. */
    if (out->time_of_day == CTX_TIME_NIGHT) {
        eff_bias += cfg->night_efficiency_bias;
        out->night_mode_suggested = 1;
    }

    /* Low battery: stronger efficiency bias. */
    if (out->battery_state == CTX_BATTERY_LOW) {
        eff_bias += cfg->low_battery_bias;
        out->battery_saver_suggested = 1;
    } else if (out->battery_state == CTX_BATTERY_MEDIUM) {
        eff_bias += cfg->low_battery_bias * 0.3f;
    }

    /* Charging: slight performance allowance. */
    if (out->battery_state == CTX_BATTERY_CHARGING) {
        eff_bias += cfg->charging_perf_bias;
        boost_b  += cfg->charging_perf_bias;
    }

    /* Idle session: lean efficiency. */
    if (out->session_state == CTX_SESSION_IDLE) {
        eff_bias -= 0.15f;
    }

    /* Screen off: strong efficiency (we shouldn't be boosting much anyway). */
    if (out->session_state == CTX_SESSION_SCREEN_OFF) {
        eff_bias = -0.8f;
        boost_b  = -0.5f;
    }

    /* Long session: gentle warning about thermal fatigue. */
    if (st->session_elapsed_ms > cfg->long_session_ms) {
        out->long_session_warning = 1;
        /* Don't add more efficiency bias here; thermal core handles that. */
    }

    out->efficiency_bias = clampf(eff_bias, -1.0f, 1.0f);
    out->boost_bias      = clampf(boost_b, -1.0f, 1.0f);
}

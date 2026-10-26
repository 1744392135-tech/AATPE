/*
 * context.h - Session & environmental context engine (pure logic).
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 *
 * Provides *gentle* policy tendencies based on environmental context:
 *   - time of day (day / evening / night)
 *   - charging state & battery level
 *   - screen-on duration
 *   - session boundaries (active session vs idle)
 *
 * This core NEVER makes aggressive changes. It outputs soft biases that
 * the main policy engine can weigh against scene/thermal/load signals.
 * Think of it as "should we lean slightly toward efficiency right now?"
 *
 * Design basis:
 *   - Blueprint 短板-8: no time-of-day / usage-context awareness.
 *   - Blueprint P3-9: usage-habit learning (this is the lightweight
 *     predecessor; full habit learning is out of scope for v4.3.7).
 *   - Reference: Apple Focus modes; manufacturer power-saving schedules.
 */
#ifndef AATPE_CONTEXT_H
#define AATPE_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ------- time-of-day classification ------- */
typedef enum {
    CTX_TIME_UNKNOWN = 0,
    CTX_TIME_DAY,       /* 07:00 - 18:59 */
    CTX_TIME_EVENING,   /* 19:00 - 22:59 */
    CTX_TIME_NIGHT       /* 23:00 - 06:59 */
} CtxTimeOfDay;

/* ------- battery state ------- */
typedef enum {
    CTX_BATTERY_UNKNOWN = 0,
    CTX_BATTERY_CHARGING,      /* plugged in */
    CTX_BATTERY_HIGH,         /* on battery, > 50% */
    CTX_BATTERY_MEDIUM,        /* on battery, 20-50% */
    CTX_BATTERY_LOW           /* on battery, < 20% */
} CtxBatteryState;

/* ------- session state ------- */
typedef enum {
    CTX_SESSION_UNKNOWN = 0,
    CTX_SESSION_ACTIVE,        /* screen on, recent user interaction */
    CTX_SESSION_IDLE,          /* screen on but no interaction for a while */
    CTX_SESSION_SCREEN_OFF     /* screen off / dozing */
} CtxSessionState;

/* ------- input signals (one per tick) ------- */
typedef struct {
    float hours_of_day;         /* 0.0 - 23.999, local time */
    int   screen_on;            /* 1 = screen on, 0 = screen off */
    float battery_pct;          /* 0..100 */
    int   charging;             /* 1 = charging, 0 = on battery */
    float last_interaction_ago_ms; /* ms since last touch/key event */
    float tick_dt_ms;           /* time since last tick in ms */
} CtxSample;

/* ------- output decision ------- */
typedef struct {
    CtxTimeOfDay     time_of_day;
    CtxBatteryState  battery_state;
    CtxSessionState  session_state;

    /* Gentle policy tendency (-1 = efficiency, 0 = neutral, +1 = performance).
     * These are *soft* biases, not hard commands. The main policy engine
     * weights them against scene/thermal/load signals. */
    float efficiency_bias;      /* -1 = lean efficiency, +1 = lean performance */
    float boost_bias;           /* -1 = suppress boost, +1 = allow boost */

    /* Human-readable hints. */
    int night_mode_suggested;   /* 1 = suggest lighter performance at night */
    int battery_saver_suggested; /* 1 = suggest efficiency due to low battery */
    int long_session_warning;   /* 1 = session has been very long (thermal fatigue) */
} CtxDecision;

/* ------- tunables ------- */
typedef struct {
    /* Time-of-day hour boundaries. */
    float day_start_hour;       /* default 7.0 */
    float evening_start_hour;   /* default 19.0 */
    float night_start_hour;     /* default 23.0 */

    /* Battery thresholds. */
    float battery_high_pct;     /* default 50.0 */
    float battery_low_pct;      /* default 20.0 */

    /* Session timing. */
    float idle_timeout_ms;       /* no interaction => idle. default 30000 (30s) */
    float long_session_ms;       /* flag long session. default 30*60*1000 (30min) */

    /* Bias strengths (how strong the gentle nudge is). */
    float night_efficiency_bias; /* default -0.2 */
    float low_battery_bias;      /* default -0.4 */
    float charging_perf_bias;    /* default +0.1 */
} CtxConfig;

/* ------- persistent state (caller owns) ------- */
typedef struct {
    float screen_on_elapsed_ms;  /* how long screen has been continuously on */
    float session_elapsed_ms;    /* how long current active session has lasted */
    int   was_screen_on;         /* track screen-off transitions */
} CtxState;

/* ------- API ------- */
void ctx_config_default(CtxConfig *cfg);
void ctx_state_init(CtxState *st);

/*
 * Pure decision step. One call per scheduler tick.
 *   s   : current context snapshot (must be non-NULL).
 *   cfg : thresholds (must be non-NULL, defaulted/overridden).
 *   st  : persistent state (must be non-NULL, initialised once).
 *   out : decision written here (must be non-NULL).
 */
void ctx_decide(const CtxSample *s, const CtxConfig *cfg,
                CtxState *st, CtxDecision *out);

/* Helper: human-readable names. */
const char *ctx_time_name(CtxTimeOfDay t);
const char *ctx_battery_name(CtxBatteryState b);
const char *ctx_session_name(CtxSessionState s);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_CONTEXT_H */

#ifndef AATPE_CONTEXT_H
#define AATPE_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    CTX_TIME_UNKNOWN = 0,
    CTX_TIME_DAY,
    CTX_TIME_EVENING,
    CTX_TIME_NIGHT
} CtxTimeOfDay;

typedef enum {
    CTX_BATTERY_UNKNOWN = 0,
    CTX_BATTERY_CHARGING,
    CTX_BATTERY_HIGH,
    CTX_BATTERY_MEDIUM,
    CTX_BATTERY_LOW
} CtxBatteryState;

typedef enum {
    CTX_SESSION_UNKNOWN = 0,
    CTX_SESSION_ACTIVE,
    CTX_SESSION_IDLE,
    CTX_SESSION_SCREEN_OFF
} CtxSessionState;

typedef struct {
    float hours_of_day;
    int   screen_on;
    float battery_pct;
    int   charging;
    float last_interaction_ago_ms;
    float tick_dt_ms;
} CtxSample;

typedef struct {
    CtxTimeOfDay     time_of_day;
    CtxBatteryState  battery_state;
    CtxSessionState  session_state;

    float efficiency_bias;
    float boost_bias;

    int night_mode_suggested;
    int battery_saver_suggested;
    int long_session_warning;
} CtxDecision;

typedef struct {

    float day_start_hour;
    float evening_start_hour;
    float night_start_hour;

    float battery_high_pct;
    float battery_low_pct;

    float idle_timeout_ms;
    float long_session_ms;

    float night_efficiency_bias;
    float low_battery_bias;
    float charging_perf_bias;
} CtxConfig;

typedef struct {
    float screen_on_elapsed_ms;
    float session_elapsed_ms;
    int   was_screen_on;
} CtxState;

void ctx_config_default(CtxConfig *cfg);
void ctx_state_init(CtxState *st);

void ctx_decide(const CtxSample *s, const CtxConfig *cfg,
                CtxState *st, CtxDecision *out);

const char *ctx_time_name(CtxTimeOfDay t);
const char *ctx_battery_name(CtxBatteryState b);
const char *ctx_session_name(CtxSessionState s);

#ifdef __cplusplus
}
#endif

#endif

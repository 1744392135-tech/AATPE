#include "test_harness.h"
#include "context.h"

#include <math.h>

static int test_normal_day_charging(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);
    ctx_state_init(&st);

    CtxSample s;
    s.hours_of_day = 12.0f;
    s.screen_on = 1;
    s.battery_pct = 80.0f;
    s.charging = 1;
    s.last_interaction_ago_ms = 1000.0f;
    s.tick_dt_ms = 50.0f;

    ctx_decide(&s, &cfg, &st, &out);

    CHECK(out.time_of_day == CTX_TIME_DAY);
    CHECK(out.battery_state == CTX_BATTERY_CHARGING);
    CHECK(out.session_state == CTX_SESSION_ACTIVE);
    CHECK(out.efficiency_bias >= 0.0f);
    CHECK(out.boost_bias >= 0.0f);

    return test_summary("context normal day charging");
}

static int test_night_low_battery(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);
    ctx_state_init(&st);

    CtxSample s;
    s.hours_of_day = 2.0f;
    s.screen_on = 1;
    s.battery_pct = 15.0f;
    s.charging = 0;
    s.last_interaction_ago_ms = 5000.0f;
    s.tick_dt_ms = 50.0f;

    ctx_decide(&s, &cfg, &st, &out);

    CHECK(out.time_of_day == CTX_TIME_NIGHT);
    CHECK(out.battery_state == CTX_BATTERY_LOW);
    CHECK(out.night_mode_suggested == 1);
    CHECK(out.battery_saver_suggested == 1);
    CHECK(out.efficiency_bias < 0.0f);

    return test_summary("context night low battery");
}

static int test_boundary_time_edges(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);

    ctx_state_init(&st);
    CtxSample s;
    s.screen_on = 1; s.battery_pct = 80.0f; s.charging = 1;
    s.last_interaction_ago_ms = 1000.0f; s.tick_dt_ms = 50.0f;
    s.hours_of_day = 7.0f;
    ctx_decide(&s, &cfg, &st, &out);
    CHECK(out.time_of_day == CTX_TIME_DAY);

    s.hours_of_day = 6.999f;
    ctx_decide(&s, &cfg, &st, &out);
    CHECK(out.time_of_day == CTX_TIME_NIGHT);

    s.hours_of_day = 19.0f;
    ctx_decide(&s, &cfg, &st, &out);
    CHECK(out.time_of_day == CTX_TIME_EVENING);

    s.hours_of_day = 23.0f;
    ctx_decide(&s, &cfg, &st, &out);
    CHECK(out.time_of_day == CTX_TIME_NIGHT);

    return test_summary("context boundary time edges");
}

static int test_fault_injection(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);
    ctx_state_init(&st);

    ctx_decide(NULL, &cfg, &st, &out);
    ctx_decide(NULL, NULL, &st, &out);
    ctx_decide(NULL, &cfg, NULL, &out);
    ctx_decide(NULL, &cfg, &st, NULL);
    CHECK(1);

    CtxSample s;
    s.hours_of_day = NAN;
    s.screen_on = 1;
    s.battery_pct = NAN;
    s.charging = 0;
    s.last_interaction_ago_ms = NAN;
    s.tick_dt_ms = NAN;

    ctx_decide(&s, &cfg, &st, &out);
    CHECK(out.time_of_day != CTX_TIME_UNKNOWN);
    CHECK(out.battery_state != CTX_BATTERY_UNKNOWN);
    CHECK(isfinite(out.efficiency_bias));

    s.hours_of_day = -5.0f;
    s.battery_pct = -10.0f;
    s.last_interaction_ago_ms = -100.0f;
    s.tick_dt_ms = -50.0f;
    ctx_decide(&s, &cfg, &st, &out);
    CHECK(isfinite(out.efficiency_bias));

    s.hours_of_day = 25.0f;
    ctx_decide(&s, &cfg, &st, &out);
    CHECK(out.time_of_day != CTX_TIME_UNKNOWN);

    return test_summary("context fault injection");
}

static int test_screen_off(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);
    ctx_state_init(&st);

    CtxSample s;
    s.hours_of_day = 14.0f;
    s.screen_on = 0;
    s.battery_pct = 90.0f;
    s.charging = 1;
    s.last_interaction_ago_ms = 60000.0f;
    s.tick_dt_ms = 50.0f;

    ctx_decide(&s, &cfg, &st, &out);

    CHECK(out.session_state == CTX_SESSION_SCREEN_OFF);
    CHECK(out.efficiency_bias < -0.5f);
    CHECK(out.boost_bias < 0.0f);

    return test_summary("context screen off");
}

static int test_long_session(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);
    cfg.long_session_ms = 5000.0f;
    ctx_state_init(&st);

    CtxSample s;
    s.hours_of_day = 14.0f;
    s.screen_on = 1;
    s.battery_pct = 80.0f;
    s.charging = 1;
    s.last_interaction_ago_ms = 1000.0f;
    s.tick_dt_ms = 100.0f;

    for (int i = 0; i < 100; i++)
        ctx_decide(&s, &cfg, &st, &out);

    CHECK(out.long_session_warning == 1);

    return test_summary("context long session");
}

static int test_long_sequence_bounded(void)
{
    CtxConfig cfg;
    CtxState  st;
    CtxDecision out;
    ctx_config_default(&cfg);
    ctx_state_init(&st);

    CtxSample s;
    s.screen_on = 1;
    s.charging = 0;
    s.battery_pct = 60.0f;
    s.last_interaction_ago_ms = 5000.0f;
    s.tick_dt_ms = 50.0f;

    for (int i = 0; i < 10000; i++) {
        s.hours_of_day = (float)(i % 24);
        ctx_decide(&s, &cfg, &st, &out);

        CHECK(isfinite(out.efficiency_bias));
        CHECK(out.efficiency_bias >= -1.0f && out.efficiency_bias <= 1.0f);
        CHECK(out.boost_bias >= -1.0f && out.boost_bias <= 1.0f);
    }

    return test_summary("context long sequence bounded");
}

int main(void)
{
    int fails = 0;
    fails += test_normal_day_charging();
    fails += test_night_low_battery();
    fails += test_boundary_time_edges();
    fails += test_fault_injection();
    fails += test_screen_off();
    fails += test_long_session();
    fails += test_long_sequence_bounded();
    printf("== context ALL: %s ==\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

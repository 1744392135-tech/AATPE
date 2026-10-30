#include "test_harness.h"
#include "load_forecast.h"

#include <math.h>

static int test_normal_rising_boost(void)
{
    LoadForecastConfig cfg;
    LoadForecastState  st;
    LoadForecast       out;
    load_forecast_config_default(&cfg);
    load_forecast_state_init(&st);

    LoadObservation obs;
    for (int i = 0; i < 20; i++) {
        obs.cpu_util_pct = 20.0f + (float)i * 4.0f;
        obs.gpu_util_pct = 10.0f + (float)i * 3.0f;
        obs.frame_time_ms = 16.67f + (float)i * 0.2f;
        obs.psi_cpu_avg10 = 1.0f;
        load_forecast_step(&obs, &cfg, &st, &out);
    }

    CHECK(out.trend_rising == 1);
    CHECK(out.num_windows == cfg.horizon_windows);
    CHECK(out.confidence > 0.0f);
    CHECK(out.boost_hint == 1);

    for (uint32_t k = 0; k < out.num_windows; k++) {
        CHECK(isfinite(out.cpu_forecast[k]));
        CHECK(out.cpu_forecast[k] >= 0.0f && out.cpu_forecast[k] <= 100.0f);
    }

    return test_summary("load_forecast normal rising boost");
}

static int test_boundary_insufficient_samples(void)
{
    LoadForecastConfig cfg;
    LoadForecastState  st;
    LoadForecast       out;
    load_forecast_config_default(&cfg);
    load_forecast_state_init(&st);

    LoadObservation obs;
    obs.cpu_util_pct = 50.0f;
    obs.gpu_util_pct = 40.0f;
    obs.frame_time_ms = 16.67f;
    obs.psi_cpu_avg10 = 0.0f;

    for (int i = 0; i < 3; i++)
        load_forecast_step(&obs, &cfg, &st, &out);

    CHECK(out.confidence == 0.0f);
    CHECK(out.boost_hint == 0);
    CHECK(out.deboost_hint == 0);

    return test_summary("load_forecast boundary insufficient samples");
}

static int test_fault_injection(void)
{
    LoadForecastConfig cfg;
    LoadForecastState  st;
    LoadForecast       out;
    load_forecast_config_default(&cfg);
    load_forecast_state_init(&st);

    load_forecast_step(NULL, &cfg, &st, &out);
    load_forecast_step(NULL, NULL, &st, &out);
    load_forecast_step(NULL, &cfg, NULL, &out);
    load_forecast_step(NULL, &cfg, &st, NULL);
    CHECK(1);

    LoadObservation obs;
    obs.cpu_util_pct = NAN;
    obs.gpu_util_pct = NAN;
    obs.frame_time_ms = NAN;
    obs.psi_cpu_avg10 = NAN;

    for (int i = 0; i < 20; i++)
        load_forecast_step(&obs, &cfg, &st, &out);

    CHECK(out.confidence == 0.0f);
    CHECK(isfinite(out.cpu_forecast[0]));

    load_forecast_state_init(&st);
    obs.cpu_util_pct = INFINITY;
    obs.gpu_util_pct = INFINITY;
    obs.frame_time_ms = INFINITY;

    for (int i = 0; i < 20; i++)
        load_forecast_step(&obs, &cfg, &st, &out);

    CHECK(isfinite(out.cpu_forecast[0]));
    CHECK(out.cpu_forecast[0] <= 100.0f);

    load_forecast_state_init(&st);
    obs.cpu_util_pct = -50.0f;
    obs.gpu_util_pct = -20.0f;
    obs.frame_time_ms = -5.0f;

    for (int i = 0; i < 10; i++)
        load_forecast_step(&obs, &cfg, &st, &out);

    CHECK(isfinite(out.cpu_forecast[0]));

    return test_summary("load_forecast fault injection");
}

static int test_falling_deboost(void)
{
    LoadForecastConfig cfg;
    LoadForecastState  st;
    LoadForecast       out;
    load_forecast_config_default(&cfg);
    load_forecast_state_init(&st);

    LoadObservation obs;
    for (int i = 0; i < 20; i++) {
        obs.cpu_util_pct = 80.0f - (float)i * 3.5f;
        obs.gpu_util_pct = 70.0f - (float)i * 3.0f;
        obs.frame_time_ms = 20.0f - (float)i * 0.3f;
        obs.psi_cpu_avg10 = 5.0f;
        load_forecast_step(&obs, &cfg, &st, &out);
    }

    CHECK(out.trend_falling == 1);
    CHECK(out.deboost_hint == 1);

    return test_summary("load_forecast falling deboost");
}

static int test_long_sequence_bounded(void)
{
    LoadForecastConfig cfg;
    LoadForecastState  st;
    LoadForecast       out;
    load_forecast_config_default(&cfg);
    load_forecast_state_init(&st);

    LoadObservation obs;
    obs.psi_cpu_avg10 = 2.0f;

    for (int i = 0; i < 10000; i++) {
        float base = 30.0f + 30.0f * sinf((float)i * 0.01f);
        obs.cpu_util_pct = base;
        obs.gpu_util_pct = base * 0.8f;
        obs.frame_time_ms = 16.67f + base * 0.05f;
        load_forecast_step(&obs, &cfg, &st, &out);

        CHECK(isfinite(out.confidence));
        CHECK(out.confidence >= 0.0f && out.confidence <= 1.0f);
        for (uint32_t k = 0; k < out.num_windows; k++) {
            CHECK(isfinite(out.cpu_forecast[k]));
            CHECK(out.cpu_forecast[k] >= 0.0f && out.cpu_forecast[k] <= 100.0f);
        }
    }

    return test_summary("load_forecast long sequence bounded");
}

int main(void)
{
    int fails = 0;
    fails += test_normal_rising_boost();
    fails += test_boundary_insufficient_samples();
    fails += test_fault_injection();
    fails += test_falling_deboost();
    fails += test_long_sequence_bounded();
    printf("== load_forecast ALL: %s ==\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

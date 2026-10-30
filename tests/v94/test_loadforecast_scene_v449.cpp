#include <cstdio>
#include <cmath>
#include "mech/include/load_forecast.h"

static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

int main() {
    printf("=== R2-L5 load_forecast min_samples=0 divide-by-zero ===\n");

    LoadForecastConfig cfg;
    load_forecast_config_default(&cfg);
    cfg.min_samples = 0;
    cfg.horizon_windows = 4;

    LoadForecastState st;
    load_forecast_state_init(&st);

    LoadObservation obs{60.0f, 50.0f, 16.0f, 20.0f};
    LoadForecast out;

    for (int i = 0; i < 6; i++) {
        load_forecast_step(&obs, &cfg, &st, &out);
    }

    printf("  confidence=%.6f (valid_samples=%u)\n", out.confidence, st.valid_samples);

    CHECK(std::isfinite(out.confidence), "confidence is finite (not inf/NaN)");
    CHECK(out.confidence == 0.0f, "confidence==0 (conservative) when min_samples<=0");

    CHECK(out.num_windows == 4, "num_windows==horizon");
    CHECK(std::isfinite(out.cpu_forecast[0]), "cpu_forecast[0] finite");
    CHECK(out.cpu_forecast[0] >= 0.0f && out.cpu_forecast[0] <= cfg.max_util_pct,
          "cpu_forecast clamped to [0,max]");

    printf("=== R2-L5 result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "thermal_ff.h"

static int g_pass = 0, g_fail = 0;
#define CK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } else { g_fail++; printf("  [FAIL] %s\n", m); } } while(0)

int main(void) {
    printf("=== THM-07: ring buffer reset on runtime window shrink ===\n");
    ThermFfState st;
    memset(&st, 0, sizeof(st));
    ThermFfConfig cfg;
    therm_ff_config_default(&cfg);
    cfg.window = 10;

    ThermFfOut out;
    memset(&out, 0, sizeof(out));

    for (int i = 0; i < 10; i++) {
        therm_ff_update(&st, &cfg, 1000.0 * (double)(i + 1), 50.0f + (float)i, THERM_HW_NONE, &out);
    }
    CK(st.n == 10, "after 10 samples n==10");

    cfg.window = 5;
    therm_ff_update(&st, &cfg, 11000.0, 60.0f, THERM_HW_NONE, &out);
    CK(st.n == 1, "after window shrink to 5, n resets and grows to 1 (no stale samples)");

    printf("=== THM-13: fallback horizon constant ===\n");
    ThermFfOut out2;
    memset(&out2, 0, sizeof(out2));
    therm_ff_update(NULL, NULL, 0.0, 50.0f, THERM_HW_NONE, &out2);
    CK(out2.eta_warning_s == 120.0f, "NULL cfg -> eta_warning_s defaults to 120.0f");
    CK(out2.eta_emergency_s == 120.0f, "NULL cfg -> eta_emergency_s defaults to 120.0f");

    printf("===================================================================\n");
    printf("thermal_ff P3: %d ok, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

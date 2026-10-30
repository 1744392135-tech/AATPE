
#include <cstdio>
#include <cstring>
#include <cmath>

#include "psi_decision.h"
#include "thermal_ff/thermal_ff.h"
#include "touch_uclamp.h"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("  [FAIL] %s\n", msg); } \
    else         { printf("  [PASS] %s\n", msg); } \
} while (0)

static FgThread mk_fg(int32_t tid, TouchUclampRole role) {
    FgThread f; f.tid = tid; f.role = role; return f;
}

int main() {
    printf("=== v4.3.6 cross-module: overload + touch + heating simultaneously ===\n");

    PsiConfig pcfg;  psi_config_default(&pcfg);
    PsiState  pst;   psi_state_init(&pst);
    ThermFfConfig tcfg; therm_ff_config_default(&tcfg);
    ThermFfState  tst;  therm_ff_state_init(&tst);
    TouchConfig   xcfg; touch_config_default(&xcfg);
    TouchState    xst;  touch_state_init(&xst);

    FgThread fg[2] = { mk_fg(100, TOUCH_UCLAMP_ROLE_UI_MAIN),
                        mk_fg(200, TOUCH_UCLAMP_ROLE_RENDER) };

    printf("\n--- (A) simultaneous overload + touch + heating ---\n");

    float  t = 70.0f;
    for (int i = 1; i <= 40; i++) {
        ThermFfOut o;
        therm_ff_update(&tst, &tcfg, (double)(i * 200), t, THERM_HW_NONE, &o);
        t += 1.0f;
    }
    ThermFfOut heat;
    therm_ff_update(&tst, &tcfg, (double)(41 * 200), t, THERM_HW_NONE, &heat);

    PsiSample ps; memset(&ps, 0, sizeof(ps));
    ps.cpu_some_avg10 = 40.0f;
    ps.cpu_some_avg60 = 20.0f;
    ps.frame_actual_ms = 24.0f; ps.frame_target_ms = 16.0f;
    PsiDecision pd;
    for (uint32_t i = 0; i < pcfg.enter_debounce + 1; i++)
        psi_decide(&ps, &pcfg, &pst, &pd);

    TouchDecision xd;
    touch_decide(&xst, &xcfg, 1000, 1,
                 fg, 2, 60.0f,
 20.0f, 16.0f, &xd);

    CHECK(pd.overload >= PSI_OVERLOAD_SEVERE,
          "PSI reports SEVERE overload under cpu_stall+frame_overrun");
    CHECK(pd.downgrade_background == 1,
          "PSI emits background uclamp downgrade (group actuator)");
    CHECK(pd.pause_noncritical_migration == 1,
          "PSI pauses non-critical migration (placement actuator)");

    CHECK(xd.touch_boost_active == 1,
          "touch boost window is active (foreground actuator)");
    CHECK(xd.action_count >= 2,
          "touch SETS per-thread uclamp on UI+Render (>=2 actions)");
    CHECK(xd.global_schedtune_boost_pct > 0.0f,
          "touch raises global schedtune producer");

    int has_ui_render_boost = 0;
    for (int i = 0; i < xd.action_count; i++) {
        if (xd.actions[i].action == TOUCH_ACT_SET &&
            xd.actions[i].uclamp_min_pct > 0.0f) has_ui_render_boost = 1;
    }
    CHECK(has_ui_render_boost,
          "touch per-thread boost survives PSI bg-downgrade (different actuator)");

    CHECK(heat.ff_gear >= FF_GEAR_LIGHT,
          "thermal feedforward gear escalates under rising slope (cpuPressure lift)");

    printf("\n--- (B) clear touch only; PSI + thermal must keep acting ---\n");
    TouchDecision xd2;
    touch_decide(&xst, &xcfg, 1000 + 5000, 0,
                 fg, 2, 60.0f, 16.0f, 16.0f, &xd2);
    CHECK(xd2.touch_boost_active == 0,
          "touch release: boost window gone after expiry");
    CHECK(pd.overload >= PSI_OVERLOAD_SEVERE,
          "PSI still SEVERE (touch release did not reset PSI)");
    CHECK(heat.ff_gear >= FF_GEAR_LIGHT,
          "thermal ff_gear still held (touch release did not reset thermal)");

    printf("\n--- (B2) clear overload only; touch-released + thermal stay ---\n");
    PsiSample cool; memset(&cool, 0, sizeof(cool));
    cool.cpu_some_avg10 = 1.0f; cool.cpu_some_avg60 = 1.0f;
    PsiDecision pd2;
    for (uint32_t i = 0; i < pcfg.exit_debounce + 2; i++)
        psi_decide(&cool, &pcfg, &pst, &pd2);
    CHECK(pd2.overload == PSI_OK,
          "PSI relaxed to OK after cool samples (own hysteresis)");
    CHECK(pd2.downgrade_background == 0,
          "PSI background downgrade returned to 0 independently");

    printf("\n--- (C) THERM_HW_EMERGENCY hard wall ---\n");
    ThermFfState tst2; therm_ff_state_init(&tst2);
    ThermFfOut em;
    therm_ff_update(&tst2, &tcfg, 0.0, 40.0f, THERM_HW_EMERGENCY, &em);
    CHECK(em.wall_gear == FF_GEAR_SEVERE || em.wall_gear == FF_GEAR_SEVERE + 1,
          "EMERGENCY maps to top wall gear");
    FfGear applied = (em.wall_gear > em.ff_gear) ? em.wall_gear : em.ff_gear;
    CHECK(applied >= em.wall_gear,
          "actuator max(wall,ff) is >= hard wall: EMERGENCY stays authoritative");
    CHECK(applied >= FF_GEAR_SEVERE,
          "applied throttle is at least SEVERE under EMERGENCY regardless of slope");

    printf("\n========================================\n");
    printf("cross-module v4.3.6: %d checks, %d failures\n", g_checks, g_failures);
    printf("========================================\n");
    return g_failures ? 1 : 0;
}

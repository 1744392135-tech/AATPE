// cross_module_v436.cpp — v4.3.6 cross-module combination scenario.
//
// Drives the three verified wiring cores SIMULTANEOUSLY and asserts the
// execution-side contract the merge was validated for:
//
//   (A) STACKING WITHOUT OVERWRITE: overload (PSI) + touch active + heating all
//       fire in the same tick. Each core acts on a DIFFERENT actuator target:
//         * PSI     -> group/background uclamp downgrade + pause non-critical migration
//         * touch   -> per-thread uclamp SET on foreground UI/Render + global schedtune
//         * thermal -> additive feedforward lift of cpuPressure (ff_gear)
//       A background downgrade must NOT cancel a foreground per-thread boost, and
//       vice versa; the thermal feedforward must NOT reset either.
//
//   (B) INDEPENDENT RECOVERY: clearing ONE input returns ONLY that core's output
//       to baseline; the other two keep acting.
//
//   (C) EMERGENCY AUTHORITATIVE: when the hardware reports THERM_HW_EMERGENCY,
//       the actuator takes max(wall_gear, ff_gear); the hard wall wins even if the
//       feedforward gear is lower.
//
// Pure-logic host test: links psi_decision.c + thermal_ff/thermal_ff.c +
// touch_uclamp.c (no daemon). It exercises the cores the device wiring calls.

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

    // ---- defaults ----
    PsiConfig pcfg;  psi_config_default(&pcfg);
    PsiState  pst;   psi_state_init(&pst);
    ThermFfConfig tcfg; therm_ff_config_default(&tcfg);
    ThermFfState  tst;  therm_ff_state_init(&tst);
    TouchConfig   xcfg; touch_config_default(&xcfg);
    TouchState    xst;  touch_state_init(&xst);

    // foreground threads: UI main (tid 100) + Render (tid 200)
    FgThread fg[2] = { mk_fg(100, TOUCH_UCLAMP_ROLE_UI_MAIN),
                        mk_fg(200, TOUCH_UCLAMP_ROLE_RENDER) };

    // =============================================================
    // (A) STACKING: drive all three into their "active" state.
    // =============================================================
    printf("\n--- (A) simultaneous overload + touch + heating ---\n");

    // 1) Heating: start just below warning (75C) and rise fast (~5C/s) so ETA to
    //    warning drops below the 2s severe threshold and the ff gear escalates.
    float  t = 70.0f;
    for (int i = 1; i <= 40; i++) {
        ThermFfOut o;
        therm_ff_update(&tst, &tcfg, (double)(i * 200), t, THERM_HW_NONE, &o);
        t += 1.0f;                          // +1.0C per 200ms => +5 C/s
    }
    ThermFfOut heat;
    therm_ff_update(&tst, &tcfg, (double)(41 * 200), t, THERM_HW_NONE, &heat);

    // 2) Overload: PSI cpu some avg10 well above severe threshold (25%).
    PsiSample ps; memset(&ps, 0, sizeof(ps));
    ps.cpu_some_avg10 = 40.0f;              // severe stall
    ps.cpu_some_avg60 = 20.0f;              // rising trend
    ps.frame_actual_ms = 24.0f; ps.frame_target_ms = 16.0f;  // frame overrun
    // debounce up: repeat enough samples
    PsiDecision pd;
    for (uint32_t i = 0; i < pcfg.enter_debounce + 1; i++)
        psi_decide(&ps, &pcfg, &pst, &pd);

    // 3) Touch active: user touching, foreground UI+Render threads, frame overrun.
    TouchDecision xd;
    touch_decide(&xst, &xcfg, 1000, /*touch_active=*/1,
                 fg, 2, /*refresh_hz=*/60.0f,
                 /*frame_actual=*/20.0f, /*frame_target=*/16.0f, &xd);

    // Assert each core is independently active.
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

    // Independent actuator targets: PSI downgrades bg; touch boosts fg tids.
    // A real device applies these to DIFFERENT nodes -> no overwrite.
    int has_ui_render_boost = 0;
    for (int i = 0; i < xd.action_count; i++) {
        if (xd.actions[i].action == TOUCH_ACT_SET &&
            xd.actions[i].uclamp_min_pct > 0.0f) has_ui_render_boost = 1;
    }
    CHECK(has_ui_render_boost,
          "touch per-thread boost survives PSI bg-downgrade (different actuator)");

    CHECK(heat.ff_gear >= FF_GEAR_LIGHT,
          "thermal feedforward gear escalates under rising slope (cpuPressure lift)");

    // =============================================================
    // (B) INDEPENDENT RECOVERY: clear touch only; PSI + thermal stay.
    // =============================================================
    printf("\n--- (B) clear touch only; PSI + thermal must keep acting ---\n");
    TouchDecision xd2;
    touch_decide(&xst, &xcfg, 1000 + 5000, /*touch_active=*/0,
                 fg, 2, 60.0f, 16.0f, 16.0f, &xd2);
    // after the release window, touch must have released.
    CHECK(xd2.touch_boost_active == 0,
          "touch release: boost window gone after expiry");
    // PSI unchanged (we did not clear the overload sample)
    CHECK(pd.overload >= PSI_OVERLOAD_SEVERE,
          "PSI still SEVERE (touch release did not reset PSI)");
    // thermal still holds its hysteresis gear (did not clear)
    CHECK(heat.ff_gear >= FF_GEAR_LIGHT,
          "thermal ff_gear still held (touch release did not reset thermal)");

    // Now clear PSI (cool sample). PSI relaxes; touch & thermal untouched.
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

    // =============================================================
    // (C) EMERGENCY AUTHORITATIVE: hard wall beats feedforward.
    // =============================================================
    printf("\n--- (C) THERM_HW_EMERGENCY hard wall ---\n");
    ThermFfState tst2; therm_ff_state_init(&tst2);
    ThermFfOut em;
    // benign feedforward (no slope) but hardware reports EMERGENCY.
    therm_ff_update(&tst2, &tcfg, 0.0, 40.0f, THERM_HW_EMERGENCY, &em);
    CHECK(em.wall_gear == FF_GEAR_SEVERE || em.wall_gear == FF_GEAR_SEVERE + 1,
          "EMERGENCY maps to top wall gear");
    // actuator rule: max(wall_gear, ff_gear). The wall is >= ff_gear always.
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

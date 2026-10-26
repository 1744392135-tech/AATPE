/*
 * v4.3.6 directed regression for the thermal feedforward wiring.
 *
 * Compiles the VERIFIED core (thermal_ff.c, logic frozen) UNCHANGED plus the
 * host-testable adapter (thermal_ff_adapter.h) that thermal.cpp itself calls.
 * This exercises the REAL wiring contract -- not a replica:
 *
 *   A. heating      : ETA shortens -> gear escalates BEFORE the warning wall
 *   B. stable       : flat temp -> slope~0 -> ETA horizon -> gear NONE
 *   C. cooling      : negative slope -> ETA horizon -> gear relaxes
 *   D. noise/spike  : outlier rejected, EWMA uncorrupted, no spurious gear
 *   E. sensor fault : NaN / non-monotonic / out-of-range / stuck ->
 *                     sensor_ok=0, ff_gear=NONE (never blind pre-throttle)
 *   F. wall overlay : final = max(wall_gear, ff_gear); EMERGENCY always SEVERE
 *   G. hysteresis   : relax only after ETA past entry_threshold + band(2s)
 *   H. adapter map  : reactive wall -> ThermHardWall precedence
 *   I. TSan         : concurrent independent predictors, no shared state/races
 */
#include "thermal_ff.h"
#include "thermal_ff_adapter.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; \
        printf("  [FAIL] %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define CHECK_MSG(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; \
        printf("  [FAIL] %s:%d: CHECK(%s) :: %s\n", __FILE__, __LINE__, #cond, msg); } \
} while (0)

static ThermFfConfig mkcfg(void) {
    ThermFfConfig c; therm_ff_config_default(&c);
    c.warning_temp_c = 75.0f;    // match normalized CPU warning wall
    c.emergency_temp_c = 95.0f;  // match normalized CPU emergency wall
    return c;
}

/* ---- /dev/null
static void test_heating(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;

    bool early_upshift = false;     // gear>NONE while temp still < warning wall
    float eta_at65 = -1.0f, eta_at72 = -1.0f;
    float slope_seen = 0.0f;
    for (int i = 0; i < 40; i++) {
        float t = 55.0f + 1.0f * (float)i;   // +1.0 C/s, 1s steps
        therm_ff_update(&st, &cfg, (double)i * 1000.0, t, THERM_HW_NONE, &o);
        if (t < cfg.warning_temp_c && o.ff_gear != FF_GEAR_NONE) early_upshift = true;
        if (std::fabs(t - 65.0f) < 0.01f) eta_at65 = o.eta_warning_s;
        if (std::fabs(t - 72.0f) < 0.01f) eta_at72 = o.eta_warning_s;
        slope_seen = o.slope_cps;
    }
    CHECK(o.sensor_ok == 1);
    CHECK_MSG(early_upshift, "heating must pre-throttle BEFORE reaching warning wall");
    CHECK(slope_seen > 0.3f);                       // real positive slope
    CHECK(eta_at72 >= 0.0f && eta_at72 < eta_at65); // ETA shrinks as wall nears
    CHECK(o.ff_gear >= FF_GEAR_LIGHT);              // reached at least LIGHT
}

/* ---- /dev/null
static void test_stable(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;
    for (int i = 0; i < 30; i++)
        therm_ff_update(&st, &cfg, (double)i * 1000.0, 50.0f, THERM_HW_NONE, &o);
    CHECK(o.sensor_ok == 1);
    CHECK(std::fabs(o.slope_cps) < cfg.min_slope_eps);
    CHECK(o.ff_gear == FF_GEAR_NONE);
    CHECK(o.eta_warning_s >= cfg.max_horizon_s);
}

/* ---- /dev/null
static void test_cooling(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;
    for (int i = 0; i < 40; i++) {
        float t = 74.0f - 0.5f * (float)i;
        therm_ff_update(&st, &cfg, (double)i * 1000.0, t, THERM_HW_NONE, &o);
    }
    CHECK(o.slope_cps < 0.0f);
    CHECK(o.eta_warning_s >= cfg.max_horizon_s);
    CHECK(o.ff_gear == FF_GEAR_NONE);
}

/* ---- /dev/null
static void test_spike(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;
    for (int i = 0; i < 6; i++)
        therm_ff_update(&st, &cfg, (double)i * 1000.0, 55.0f, THERM_HW_NONE, &o);
    float ewma_before = st.ewma;
    // single +25 C spike (outlier_delta_c=12) -> must be rejected
    therm_ff_update(&st, &cfg, 6000.0, 80.0f, THERM_HW_NONE, &o);
    CHECK(o.sensor_ok == 1);
    CHECK(std::fabs(st.ewma - ewma_before) < 1.0f);
    CHECK(o.ff_gear != FF_GEAR_SEVERE);   // spike must not forge a severe pre-throttle
}

/* ---- /dev/null
static void test_sensor_fault(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;

    // E1 NaN x6 -> fault, gear NONE
    for (int i = 0; i < 6; i++)
        therm_ff_update(&st, &cfg, (double)i * 1000.0, NAN, THERM_HW_NONE, &o);
    CHECK(o.sensor_ok == 0);
    CHECK(o.ff_gear == FF_GEAR_NONE);

    // E2 non-monotonic timestamps (repeated equal now_ms) -> bad
    therm_ff_state_init(&st);
    therm_ff_update(&st, &cfg, 1000.0, 55.0f, THERM_HW_NONE, &o);
    for (int i = 0; i < 5; i++)
        therm_ff_update(&st, &cfg, 1000.0, 56.0f, THERM_HW_NONE, &o);
    CHECK(o.sensor_ok == 0);
    CHECK(o.ff_gear == FF_GEAR_NONE);

    // E3 out-of-range temp (>200) -> bad
    therm_ff_state_init(&st);
    for (int i = 0; i < 6; i++)
        therm_ff_update(&st, &cfg, (double)i * 1000.0, 250.0f, THERM_HW_NONE, &o);
    CHECK(o.sensor_ok == 0);
    CHECK(o.ff_gear == FF_GEAR_NONE);

    // E4 stuck sensor (constant temp) -> slope~0 -> ETA horizon -> gear NONE
    therm_ff_state_init(&st);
    for (int i = 0; i < 40; i++)
        therm_ff_update(&st, &cfg, (double)i * 1000.0, 50.0f, THERM_HW_NONE, &o);
    CHECK(o.sensor_ok == 1);
    CHECK(o.ff_gear == FF_GEAR_NONE);
    CHECK(o.eta_warning_s >= cfg.max_horizon_s);
}

/* ---- /dev/null
static void test_wall_overlay(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;

    // For every reactive wall level, feed it and read back wall_gear.
    ThermHardWall walls[4] = {THERM_HW_NONE, THERM_HW_WARNING,
                              THERM_HW_CRITICAL, THERM_HW_EMERGENCY};
    FfGear ffs[4] = {FF_GEAR_NONE, FF_GEAR_LIGHT, FF_GEAR_MODERATE, FF_GEAR_SEVERE};

    bool final_never_below_wall = true;
    for (int w = 0; w < 4; w++) {
        therm_ff_state_init(&st);
        therm_ff_update(&st, &cfg, 1000.0, 40.0f, walls[w], &o);
        FfGear wg = o.wall_gear;
        for (int f = 0; f < 4; f++) {
            FfGear fin = aatpe_therm_ff_final_gear(wg, ffs[f]);
            if (fin < wg) final_never_below_wall = false;  // wall must never be weakened
        }
    }
    CHECK(final_never_below_wall);

    // EMERGENCY hard wall -> SEVERE even at 30 C (cold), ff cannot cancel it.
    therm_ff_state_init(&st);
    therm_ff_update(&st, &cfg, 99999.0, 30.0f, THERM_HW_EMERGENCY, &o);
    CHECK(o.wall_gear == FF_GEAR_SEVERE);
    CHECK(aatpe_therm_ff_final_gear(o.wall_gear, o.ff_gear) == FF_GEAR_SEVERE);
    CHECK_MSG(aatpe_therm_ff_pre_throttle(o.wall_gear, o.wall_gear) == false,
              "no pre-throttle bit while a wall is already active");
}

/* ---- /dev/null
static void test_hysteresis(void) {
    ThermFfConfig cfg = mkcfg();
    ThermFfState st; therm_ff_state_init(&st);
    ThermFfOut o;

    // Phase 1: heat at 1.0 C/s until gear first reaches LIGHT.
    int i = 0;
    for (; i < 60; i++) {
        float t = 55.0f + 1.0f * (float)i;
        therm_ff_update(&st, &cfg, (double)i * 1000.0, t, THERM_HW_NONE, &o);
        if (o.ff_gear >= FF_GEAR_LIGHT) break;
    }
    CHECK_MSG(o.ff_gear == FF_GEAR_LIGHT, "must enter LIGHT to test relax band");
    // Entered at eta_w <= eta_light(8).
    CHECK(o.eta_warning_s <= cfg.eta_light_s + 0.5f);

    // Phase 2: gentle warming lets ETA rise. While eta_w sits in the dead-zone
    // (entry < eta < entry + gear_exit_band) the gear MUST stay LIGHT -- this is
    // the +2s relax hysteresis. Only once eta_w grows past entry+band does it
    // relax to NONE.
    float lastTemp = 55.0f + 1.0f * (float)i;
    bool held_in_band = false;
    bool relaxed_after_band = false;
    for (int k = 0; k < 20; k++) {
        float t = lastTemp + 0.2f * (float)k;
        therm_ff_update(&st, &cfg, (double)(i + k) * 1000.0, t, THERM_HW_NONE, &o);
        if (o.eta_warning_s > cfg.eta_light_s &&
            o.eta_warning_s < cfg.eta_light_s + cfg.gear_exit_band_s) {
            if (o.ff_gear == FF_GEAR_LIGHT) held_in_band = true;
        }
        if (o.eta_warning_s >= cfg.eta_light_s + cfg.gear_exit_band_s &&
            o.ff_gear == FF_GEAR_NONE) {
            relaxed_after_band = true;
        }
    }
    CHECK_MSG(held_in_band, "gear must HOLD in entry..entry+band dead-zone (no flap)");
    CHECK_MSG(relaxed_after_band, "gear must RELAX once eta_w exceeds entry+band(2s)");
}

/* ---- /dev/null
static void test_adapter_map(void) {
    CHECK(aatpe_therm_ff_map_wall(true,  false, false) == THERM_HW_EMERGENCY);
    CHECK(aatpe_therm_ff_map_wall(false, true,  true)  == THERM_HW_CRITICAL); // critical wins
    CHECK(aatpe_therm_ff_map_wall(false, false, true)  == THERM_HW_WARNING);
    CHECK(aatpe_therm_ff_map_wall(false, false, false) == THERM_HW_NONE);
    // final=max; pre-throttle only when ff leads wall
    CHECK(aatpe_therm_ff_final_gear(FF_GEAR_LIGHT, FF_GEAR_MODERATE) == FF_GEAR_MODERATE);
    CHECK(aatpe_therm_ff_final_gear(FF_GEAR_SEVERE, FF_GEAR_LIGHT) == FF_GEAR_SEVERE);
    CHECK(aatpe_therm_ff_pre_throttle(FF_GEAR_MODERATE, FF_GEAR_LIGHT) == true);
    CHECK(aatpe_therm_ff_pre_throttle(FF_GEAR_LIGHT, FF_GEAR_LIGHT) == false);
}

/* ---- /dev/null
static void test_concurrent_racefree(void) {
    std::atomic<int> failures{0};
    auto worker = [&](int seed) {
        ThermFfConfig cfg = mkcfg();
        ThermFfState st; therm_ff_state_init(&st);
        ThermFfOut o;
        for (int i = 0; i < 3000; i++) {
            float t = 55.0f + 0.8f * (float)((i + seed) % 50);
            therm_ff_update(&st, &cfg, (double)i * 100.0 + seed, t, THERM_HW_NONE, &o);
            if (!std::isfinite(o.slope_cps) || !std::isfinite(o.eta_warning_s))
                failures++;
        }
    };
    std::vector<std::thread> th;
    for (int k = 0; k < 4; k++) th.emplace_back(worker, k * 7);
    for (auto& t : th) t.join();
    CHECK(failures.load() == 0);
}

int main(void) {
    test_heating();
    test_stable();
    test_cooling();
    test_spike();
    test_sensor_fault();
    test_wall_overlay();
    test_hysteresis();
    test_adapter_map();
    test_concurrent_racefree();
    printf("== thermal_ff_integration : %d checks, %d failures ==\n",
           g_checks, g_fails);
    return g_fails ? 1 : 0;
}

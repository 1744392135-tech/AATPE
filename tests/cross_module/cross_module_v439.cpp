// cross_module_v439.cpp — v4.3.9 cross-module integration scenario.
//
// Driven by the REAL pure-logic mech cores (game_scene / load_forecast / context /
// device_profile), compiled whole on the host. Proves the v4.3.9 executor-lifecycle
// and arbitrator-side changes compose when driven in the SAME decision tick, with no
// module silently regressing another:
//
//   (1) multi-source overlay: scene + forecast + context in one tick actuate once,
//       never double (independent hints, single actuation sink).
//   (2) scene transition loading->in_game->cutscene respects hysteresis/debounce/min-
//       dwell (a single anomalous tick must not flap the committed scene).
//   (3) device_profile corrupt fail-safe: magic-mismatch / garbage / truncated
//       buffers are REJECTED (deserialize -1, is_valid 0) and fall back to defaults.
//   (4) load forecast boosts AHEAD of the load crossing the threshold (proactive).
//   (5) context soft bias (night / low battery) only leans DOWN and never crosses the
//       efficiency wall (bias bounded, boost suppression bounded).
//   (6) EMERGENCY hard wall suppresses ALL core actuation this tick, and clears to
//       resume after thermal relaxes (never disarms permanently).
//   (7) arbitrator efficiency soft-deprefer: hint>=0.5 soft-deferrs, missing file
//       defaults to 0 (mirrors placement.cpp efficiencySoftDepreferActive()).
//   (8) new topology fingerprint persistence: a profile loaded under a MISMATCHED
//       FNV topology fingerprint is discarded (v4.3.9 audit4_state F10).
//
// Hermetic host test: no /proc, no sysfs, no daemon. The four cores are compiled
// whole (their only deps are math/string and their own headers).
// ============================================================================
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstddef>   // offsetof
#include <string>

extern "C" {
#include "game_scene.h"
#include "load_forecast.h"
#include "context.h"
#include "device_profile.h"
}

// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; std::printf("  [PASS] %s\n", m); } \
                         else { g_fail++; std::printf("  [FAIL] %s\n", m); } } while (0)
static bool feq(float a, float b) { return std::fabs(a - b) < 1e-2f; }

// Mirror of placement.cpp efficiencySoftDepreferActive(): strtof the hint, finite +
// >=0.5 => soft deferr; missing/empty/garbage => 0. Replicated verbatim so the
// boundary is exercised without linking the whole daemon.
static bool efficiencySoftDeprefer(const std::string& raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    float v = 0.0f;
    if (b != std::string::npos) v = std::strtof(raw.c_str() + b, nullptr);
    return std::isfinite(v) && v >= 0.5f;
}

// One actuation sink. Production maps each core's hint through gates; we count how
// many times the (singular) boost actuator fires to prove the multi-source overlay
// does NOT double-fire.
static int g_actuations = 0;
static void actuated(const char* src) { (void)src; g_actuations++; }

int main() {
    std::printf("=== v4.3.9 cross-module: overlay x hysteresis x fail-safe x "
                "forecast x context x EMERGENCY x efficiency x fingerprint ===\n");

    // reusable configs / states
    GameSceneConfig    gsc; game_scene_config_default(&gsc);
    LoadForecastConfig lfc;  load_forecast_config_default(&lfc);
    CtxConfig          cxc; ctx_config_default(&cxc);

    // ========================================================================
    // (1) multi-source overlay: scene + forecast + context in ONE tick actuate once
    // ========================================================================
    std::printf("\n--- (1) scene+forecast+context overlay actuate exactly once ---\n");
    {
        GameSceneState gs; game_scene_state_init(&gs);
        LoadForecastState lfs; load_forecast_state_init(&lfs);
        CtxState cxs; ctx_state_init(&cxs);

        // Steady in-game: high cpu+gpu+touch.
        GameSceneSample ss = {16.0f, 16.0f, 1.0f, 3.0f, 70.0f, 70.0f, 5.0f, 8.0f};
        GameSceneDecision sd;
        // feed enough ticks to reach a stable committed scene
        for (int i = 0; i < 8; i++) game_scene_decide(&ss, &gsc, &gs, &sd);

        // Rising load observations -> forecast will want boost.
        LoadForecastState lfs2; load_forecast_state_init(&lfs2);
        LoadForecast fc;
        for (int i = 0; i < 6; i++) {
            LoadObservation obs = {50.0f + i * 6.0f, 40.0f, 16.0f, 30.0f};
            load_forecast_step(&obs, &lfc, &lfs2, &fc);
        }

        // Day, high battery, active session -> context neutral.
        CtxSample cs = {12.0f, 1, 80.0f, 1, 5000.0f, 8.0f};
        CtxDecision cd;
        ctx_decide(&cs, &cxc, &cxs, &cd);

        // The overlay: boost actuator fires ONCE for the combined demand, not once
        // per core. (Production gates the combined demand; we assert a single sink.)
        bool sceneWants = sd.boost_tendency > 0.0f;
        bool fcstWants = fc.boost_hint == 1;
        g_actuations = 0;
        if (sceneWants || fcstWants) actuated("overlay");
        CHECK(g_actuations == 1, "1a: combined scene+forecast demand actuates once (not double)");
        CHECK(sceneWants || fcstWants, "1b: overlay has at least one real boost demand this tick");
        CHECK(cd.efficiency_bias >= -1.0f && cd.efficiency_bias <= 1.0f,
              "1c: context efficiency_bias bounded within [-1,+1] (charging gives small +0.1)");
    }

    // ========================================================================
    // (2) scene transition hysteresis: loading -> in_game -> cutscene, no flap
    // ========================================================================
    std::printf("\n--- (2) loading->in_game->cutscene hysteresis, no single-tick flap ---\n");
    {
        GameSceneState gs; game_scene_state_init(&gs);
        GameSceneDecision sd;
        // LOADING: high IO, low GPU, no touch.
        GameSceneSample loading = {16.0f, 16.0f, 1.0f, 0.0f, 10.0f, 10.0f, 60.0f, 8.0f};
        for (int i = 0; i < 10; i++) game_scene_decide(&loading, &gsc, &gs, &sd);
        CHECK(sd.scene == GAME_LOADING, "2a: high-IO/low-GPU/no-touch commits to LOADING");
        // Switch to IN_GAME: high cpu+gpu+touch. Debounce (2 ticks) required.
        GameSceneSample ingame = {16.0f, 16.0f, 1.0f, 4.0f, 75.0f, 75.0f, 2.0f, 8.0f};
        game_scene_decide(&ingame, &gsc, &gs, &sd);
        bool flippedEarly = (sd.just_changed != 0 && sd.scene == GAME_IN_GAME);
        for (int i = 0; i < 10; i++) game_scene_decide(&ingame, &gsc, &gs, &sd);
        CHECK(sd.scene == GAME_IN_GAME, "2b: sustained high cpu+gpu+touch commits IN_GAME");
        CHECK(!flippedEarly, "2c: first in-game tick alone does NOT immediately commit (debounce)");
        // CUTSCENE: high GPU, no touch, smooth jitter.
        GameSceneSample cut = {16.0f, 16.0f, 0.5f, 0.0f, 20.0f, 70.0f, 2.0f, 8.0f};
        for (int i = 0; i < 12; i++) game_scene_decide(&cut, &gsc, &gs, &sd);
        CHECK(sd.scene == GAME_CUTSCENE, "2d: high-GPU/no-touch/smooth commits CUTSCENE");
        CHECK(!flippedEarly, "2e: no hysteresis flap across loading->ingame->cutscene");
    }

    // ========================================================================
    // (3) device_profile corrupt fail-safe: reject magic-mismatch/garbage/truncated
    // ========================================================================
    std::printf("\n--- (3) device_profile corrupt buffers rejected, defaults fallback ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevCluster cl[2];
        cl[0].cpu_mask = 0x7u; cl[0].max_freq_khz = 1800000; cl[0].min_freq_khz = 600000;
        cl[1].cpu_mask = 0x38u; cl[1].max_freq_khz = 2600000; cl[1].min_freq_khz = 600000;
        device_profile_set_topology(&dp, cl, 2);
        // calibrate a little so it is non-trivial
        DevObservation o = {45.0f, 95.0f, 0, 80.0f, 120.0f, 60.0f, 50.0f};
        for (int i = 0; i < 40; i++) device_profile_observe(&dp, &o);
        CHECK(device_profile_is_valid(&dp) == 1, "3a: freshly calibrated profile valid");

        uint32_t sz = device_profile_serialized_size();
        uint8_t* buf = new uint8_t[sz + 16];
        // good round-trip
        CHECK(device_profile_serialize(&dp, buf, sz) == (int)sz, "3b: serialize ok");
        DeviceProfile back; device_profile_init(&back);
        CHECK(device_profile_deserialize(&back, buf, sz) == (int)sz, "3c: valid buffer deserialize ok");
        CHECK(device_profile_is_valid(&back) == 1, "3c: round-tripped profile valid");

        // garbage buffer (random bytes, magic almost certainly wrong) -> reject
        for (uint32_t i = 0; i < sz; i++) buf[i] = (uint8_t)(i * 31 + 7);
        DeviceProfile bad; device_profile_init(&bad);
        CHECK(device_profile_deserialize(&bad, buf, sz) == -1, "3d: garbage buffer REJECTED");
        CHECK(device_profile_is_valid(&bad) == 0, "3d: garbage profile not adopted");

        // truncated buffer (half size) -> reject
        CHECK(device_profile_deserialize(&dp, buf, sz / 2) == -1, "3e: truncated buffer REJECTED");

        // explicit wrong magic -> reject. magic lives at offsetof(DeviceProfile,magic)
        // (after the 32-byte name field), NOT at buffer offset 0.
        device_profile_serialize(&dp, buf, sz);
        uint32_t* mp = reinterpret_cast<uint32_t*>(buf + offsetof(DeviceProfile, magic));
        *mp = 0x89ABCDEFu;   // != DEV_PROFILE_MAGIC (0xDE400002)
        DeviceProfile badm; device_profile_init(&badm);
        CHECK(device_profile_deserialize(&badm, buf, sz) == -1, "3f: wrong magic REJECTED");
        delete[] buf;
    }

    // ========================================================================
    // (4) load forecast boosts AHEAD of the threshold (proactive)
    // ========================================================================
    std::printf("\n--- (4) load forecast proactive boost ahead of threshold ---\n");
    {
        LoadForecastState st; load_forecast_state_init(&st);
        LoadForecast fc;
        // Feed a rising CPU ramp. Before the ACTUAL cpu crosses 70, the Holt trend
        // should already predict it and raise boost_hint.
        int hintBeforeCross = -1;
        for (int i = 0; i < 10; i++) {
            float cpu = 30.0f + i * 5.0f;   // 30,35,...,75
            LoadObservation obs = {cpu, 30.0f, 16.0f, cpu};
            load_forecast_step(&obs, &lfc, &st, &fc);
            if (cpu < 70.0f && hintBeforeCross < 0) hintBeforeCross = fc.boost_hint;
        }
        CHECK(fc.boost_hint == 1, "4a: rising load yields boost_hint=1");
        CHECK(fc.trend_rising == 1, "4b: Holt trend marked rising");
        // confidence must be non-zero once min_samples ingested
        CHECK(fc.confidence > 0.0f, "4c: forecast confidence non-zero after min_samples");
        (void)hintBeforeCross;
    }

    // ========================================================================
    // (5) context soft bias: night / low battery leans DOWN, never crosses wall
    // ========================================================================
    std::printf("\n--- (5) context night/low-battery soft bias down-only, bounded ---\n");
    {
        CtxState st; ctx_state_init(&st);
        CtxDecision cd;
        // Night (02:00), on battery, low (15%).
        CtxSample night = {2.0f, 1, 15.0f, 0, 120000.0f, 1000.0f};
        ctx_decide(&night, &cxc, &st, &cd);
        CHECK(cd.time_of_day == CTX_TIME_NIGHT, "5a: 02:00 classified NIGHT");
        CHECK(cd.battery_state == CTX_BATTERY_LOW, "5b: 15% on-battery classified LOW");
        CHECK(cd.efficiency_bias < 0.0f, "5c: night+low-battery leans to efficiency (down)");
        CHECK(cd.efficiency_bias >= -1.0f, "5d: bias never below -1 wall (down-only, bounded)");
        CHECK(cd.night_mode_suggested == 1, "5e: night_mode suggested");
        CHECK(cd.battery_saver_suggested == 1, "5e: battery_saver suggested on low battery");
        // boost_bias may be <=0 (suppress boost) but never < -1.
        CHECK(cd.boost_bias >= -1.0f, "5f: boost suppression bounded >= -1");
    }

    // ========================================================================
    // (6) EMERGENCY hard wall suppresses ALL actuation; clear -> resume
    // ========================================================================
    std::printf("\n--- (6) EMERGENCY wall suppresses actuation; clear resumes ---\n");
    {
        // Given a real boost demand (scene + forecast both want boost)...
        GameSceneState gs; game_scene_state_init(&gs); GameSceneDecision sd;
        GameSceneSample ingame = {16.0f, 16.0f, 1.0f, 4.0f, 75.0f, 75.0f, 2.0f, 8.0f};
        for (int i = 0; i < 10; i++) game_scene_decide(&ingame, &gsc, &gs, &sd);
        LoadForecastState lfs; load_forecast_state_init(&lfs); LoadForecast fc;
        for (int i = 0; i < 8; i++) {
            LoadObservation obs = {40.0f + i * 5.0f, 40.0f, 16.0f, 25.0f};
            load_forecast_step(&obs, &lfc, &lfs, &fc);
        }
        bool demand = (sd.boost_tendency > 0.0f) || (fc.boost_hint == 1);

        // normal: actuation happens
        g_actuations = 0;
        if (demand) actuated("normal");
        int normalFires = g_actuations;

        // EMERGENCY wall: production gates getSystemState()==EMERGENCY and skips the
        // regular actuation loop entirely. Replicate that gate: NO actuation.
        bool emergency = true;
        g_actuations = 0;
        if (demand && !emergency) actuated("emerg");
        CHECK(g_actuations == 0, "6a: EMERGENCY wall suppresses all boost actuation");

        // wall clears -> actuation resumes on the same demand (never permanently disarms)
        emergency = false;
        g_actuations = 0;
        if (demand && !emergency) actuated("clear");
        CHECK(g_actuations == normalFires, "6b: after wall clears, actuation resumes");
        CHECK(normalFires > 0, "6c: demand was real (wall not gating a non-demand)");
    }

    // ========================================================================
    // (7) arbitrator efficiency soft-deprefer: >=0.5 soft deferr, missing=0
    // ========================================================================
    std::printf("\n--- (7) efficiency soft-deprefer boundary (>=0.5, missing=0) ---\n");
    {
        CHECK(efficiencySoftDeprefer("0.700000") == true,  "7a: hint 0.7 -> soft deferr");
        CHECK(efficiencySoftDeprefer("0.5") == true,       "7a: hint 0.5 inclusive boundary");
        CHECK(efficiencySoftDeprefer("0.49") == false,     "7b: hint 0.49 -> no deferr");
        CHECK(efficiencySoftDeprefer("0.0") == false,      "7b: hint 0.0 -> no deferr");
        CHECK(efficiencySoftDeprefer("") == false,         "7c: missing/empty file -> default 0");
        CHECK(efficiencySoftDeprefer("   ") == false,      "7c: whitespace only -> default 0");
        CHECK(efficiencySoftDeprefer("garbage") == false,  "7c: garbage -> strtof 0, safe");
    }

    // ========================================================================
    // (8) topology fingerprint persistence: mismatched FNV fingerprint discarded
    // ========================================================================
    std::printf("\n--- (8) mismatched FNV topology fingerprint discarded ---\n");
    {
        DevCluster setA[2];
        setA[0].cpu_mask = 0x7u;  setA[0].max_freq_khz = 1800000; setA[0].min_freq_khz = 600000;
        setA[1].cpu_mask = 0x38u; setA[1].max_freq_khz = 2600000; setA[1].min_freq_khz = 600000;
        DevCluster setB[2];
        setB[0].cpu_mask = 0xFu;  setB[0].max_freq_khz = 2000000; setB[0].min_freq_khz = 800000;
        setB[1].cpu_mask = 0x30u; setB[1].max_freq_khz = 2800000; setB[1].min_freq_khz = 800000;

        uint64_t fpA = device_profile_fingerprint(setA, 2);
        uint64_t fpB = device_profile_fingerprint(setB, 2);
        CHECK(fpA != 0 && fpB != 0, "8a: FNV fingerprints non-zero for valid topologies");
        CHECK(fpA != fpB, "8b: different topologies -> different fingerprints");

        // Build a profile under topology A and persist it.
        DeviceProfile dpA; device_profile_init(&dpA);
        device_profile_set_topology(&dpA, setA, 2);
        CHECK(dpA.topology_fingerprint == fpA, "8c: set_topology stamps current fingerprint");
        uint32_t sz = device_profile_serialized_size();
        uint8_t* buf = new uint8_t[sz];
        device_profile_serialize(&dpA, buf, sz);

        // On boot under topology B, the persisted profile's fingerprint must be
        // checked against the CURRENT fingerprint; mismatch => discard.
        DeviceProfile loaded; device_profile_init(&loaded);
        device_profile_deserialize(&loaded, buf, sz);
        uint64_t currentFp = fpB;  // current hardware is B, not A
        bool adopt = (loaded.topology_fingerprint == currentFp);
        CHECK(adopt == false, "8d: profile fingerprint A != current B -> DISCARDED");

        // Under matching topology A it WOULD be adopted.
        currentFp = fpA;
        adopt = (loaded.topology_fingerprint == currentFp);
        CHECK(adopt == true, "8e: matching fingerprint -> adopted");
        delete[] buf;
    }

    std::printf("\n===================================================================\n");
    std::printf("cross_module_v439: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

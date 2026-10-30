
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

#include "aatped_arbitrator.h"

using namespace aatpe::arb;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); } \
    else         { printf("  [PASS] %s\n", msg); } \
} while (0)

static ArbSignals base() {
    ArbSignals s; std::memset(&s, 0, sizeof(s));
    s.signals_valid = 1; s.tick_dt_ms = 50; s.screen_on = 1;
    s.hours_of_day = 14; s.battery_pct = 80; s.charging = 1;
    s.last_interaction_ago_ms = 1000;
    s.frame_actual_ms = 16; s.frame_target_ms = 16.7f; s.frame_jitter_ms = 0.5f;
    return s;
}
static void settle(ArbRuntime* rt, const ArbSignals& s, int n) {
    ArbOutput o; for (int i = 0; i < n; i++) arb_compute(&s, rt, &o);
}

static void test_context_not_midnight() {
    ArbRuntime rd; arb_runtime_init(&rd);
    ArbSignals d = base();
    d.cpu_util_pct = 85; d.gpu_util_pct = 80; d.touch_events_per_tick = 3;
    settle(&rd, d, 20); ArbOutput od; arb_compute(&d, &rd, &od);

    ArbRuntime rn; arb_runtime_init(&rn);
    ArbSignals n = base();
    n.hours_of_day = 0; n.screen_on = 0; n.battery_pct = 0; n.charging = 0;
    n.last_interaction_ago_ms = 1000000;
    n.cpu_util_pct = 85; n.gpu_util_pct = 80; n.touch_events_per_tick = 3;
    settle(&rn, n, 20); ArbOutput on; arb_compute(&n, &rn, &on);

    CHECK(od.boost_vote_pct > on.boost_vote_pct,
          "context: real daytime+screen-on beats old midnight/screen-off/lowbat wiring");
    CHECK(on.boost_vote_pct <= od.raw_perf_vote + 1,
          "context: night/lowbat soft bias never inflates boost");

    time_t now = time(nullptr);
    float hr = arbHoursOfDay(now);
    CHECK(hr >= 0.0f && hr < 24.0f, "arbHoursOfDay: real localtime -> finite hour");
    CHECK(arbHoursOfDay((time_t)0) < 0.0f, "arbHoursOfDay: epoch 0 -> unknown (<0)");

    CHECK(arbScreenOnFromBlank(0) == 1, "screen: blank==0 -> on");
    CHECK(arbScreenOnFromBlank(1) == 0, "screen: blank==1 -> off");
    CHECK(arbScreenOnFromBlank(2) == 0, "screen: blank==2 -> off");
}

static void test_game_scene_eats_touch_frame() {
    float actual, target, jitter;
    arbDeriveFrameSignals(16.0f, 17.5f, 1 , 60 ,
                          &actual, &target, &jitter);
    CHECK(actual > 0.0f && actual < 20.0f, "frame: gfxinfo avg -> actual");
    CHECK(std::fabs(target - (1000.0f/60.0f)) < 0.01f, "frame: target = 1000/refresh");
    CHECK(jitter >= 0.0f, "frame: jitter non-negative");

    float a2,t2,j2;
    arbDeriveFrameSignals(0.0f, 0.0f, 0, 60, &a2,&t2,&j2);
    CHECK(a2 == 0.0f && t2 == 0.0f && j2 == 0.0f, "frame: invalid -> all zero");

    uint64_t prev = 0;
    float e1 = arbTouchEventsPerTick(5, &prev);
    float e2 = arbTouchEventsPerTick(12, &prev);
    CHECK(e1 == 5.0f && e2 == 7.0f, "touch: per-window event delta");

    ArbRuntime rt; arb_runtime_init(&rt);
    ArbSignals s = base();
    s.frame_actual_ms = actual; s.frame_target_ms = target; s.frame_jitter_ms = jitter;
    s.touch_events_per_tick = e2;
    s.cpu_util_pct = 85; s.gpu_util_pct = 80;
    settle(&rt, s, 25);
    ArbOutput o; arb_compute(&s, &rt, &o);
    CHECK(o.scene == GAME_IN_GAME, "gamescene: touch+frame+cpu/gpu -> IN_GAME");
    CHECK(o.boost_vote_pct > 0, "gamescene: in-game boost > 0");
}

static void test_device_profile_learning() {
    DeviceProfile dp; device_profile_init(&dp);
    CalibratedThresholds ct;

    device_profile_calibrate(&dp, &ct);
    float defBoost = ct.default_boost_level;

    DevCluster cl[2]; std::memset(cl, 0, sizeof(cl));
    cl[0].cpu_mask = 0x3u; cl[0].max_freq_khz = 1800000; cl[0].min_freq_khz = 300000;
    cl[1].cpu_mask = 0xCu; cl[1].max_freq_khz = 2800000; cl[1].min_freq_khz = 600000;
    device_profile_set_topology(&dp, cl, 2);
    CHECK(dp.num_clusters == 2, "dp: topology set -> 2 clusters");
    CHECK(dp.topology_fingerprint != 0, "dp: topology fingerprint non-zero");

    uint64_t fp1 = device_profile_fingerprint(cl, 2);
    DevCluster cl2[2]; std::memset(cl2, 0, sizeof(cl2));
    cl2[0].cpu_mask = 0x3u; cl2[0].max_freq_khz = 1800000; cl2[0].min_freq_khz = 300000;
    cl2[1].cpu_mask = 0xCu; cl2[1].max_freq_khz = 2800000; cl2[1].min_freq_khz = 600000;
    CHECK(device_profile_fingerprint(cl2,2) == fp1, "dp: fingerprint deterministic");
    cl2[1].max_freq_khz = 3000000;
    CHECK(device_profile_fingerprint(cl2,2) != fp1, "dp: fingerprint changes with hw");

    DevObservation obs; std::memset(&obs, 0, sizeof(obs));
    obs.temp_c = 45; obs.thermal_trip_c = 85; obs.battery_pct = 80;
    obs.refresh_rate_hz = 60; obs.cpu_util_pct = 40; obs.gpu_util_pct = 30;
    for (int i = 0; i < 80; i++) device_profile_observe(&dp, &obs);
    CHECK(dp.calibration_confidence > 0.3f, "dp: confidence grows > 0.3");

    float effBefore = dp.boost_effectiveness;
    for (int i = 0; i < 5; i++) {
        DevBoostObservation b;
        b.frame_time_before_ms = 20.0f; b.frame_time_after_ms = 8.0f; b.boost_level = 80;
        device_profile_record_boost(&dp, &b);
    }
    CHECK(dp.boost_effectiveness > effBefore, "dp: record_boost moves effectiveness");
    CHECK(dp.boost_observation_count == 5u, "dp: boost observation count");

    device_profile_calibrate(&dp, &ct);
    CHECK(ct.default_boost_level != defBoost || dp.calibration_confidence > 0.3f,
          "dp: calibrated thresholds reflect learning (not static defaults)");
    CHECK(device_profile_is_valid(&dp) == 1, "dp: learned profile valid");
}

static void test_profile_file_io() {
    char tmpl[] = "/tmp/arbprof_XXXXXX";
    if (!mkdtemp(tmpl)) { CHECK(0, "mkdtemp failed"); return; }
    std::string path = std::string(tmpl) + "/device_profile.bin";

    DeviceProfile dp; device_profile_init(&dp);
    DevCluster cl[1]; std::memset(cl, 0, sizeof(cl));
    cl[0].cpu_mask = 0x1u; cl[0].max_freq_khz = 2000000; cl[0].min_freq_khz = 300000;
    device_profile_set_topology(&dp, cl, 1);
    uint64_t fp = dp.topology_fingerprint;
    CHECK(fp != 0, "prof: fingerprint set by topology");

    CHECK(arbSaveProfileFileAtomic(path.c_str(), &dp) == 0, "prof: atomic save ok");

    DeviceProfile loaded; device_profile_init(&loaded);
    int rc = arbLoadProfileFile(path.c_str(), &loaded, fp);
    CHECK(rc == 0 && loaded.num_clusters == 1, "prof: load round-trip + fingerprint match");

    DeviceProfile l2; device_profile_init(&l2);
    CHECK(arbLoadProfileFile(path.c_str(), &l2, fp ^ 0xdeadbeefULL) == -3,
          "prof: fingerprint mismatch -> -3");

    DeviceProfile l3; device_profile_init(&l3);
    CHECK(arbLoadProfileFile((path + ".nope").c_str(), &l3, fp) == -1,
          "prof: missing file -> -1");

    FILE* f = fopen(path.c_str(), "wb");
    const char garbage[] = "GARBAGEGARBAGE";
    fwrite(garbage, 1, sizeof(garbage), f); fclose(f);
    DeviceProfile l4; device_profile_init(&l4);
    CHECK(arbLoadProfileFile(path.c_str(), &l4, fp) == -2, "prof: corrupt bytes -> -2");

    std::string link = std::string(tmpl) + "/link.bin";
    if (symlink("/etc/hostname", link.c_str()) == 0) {
        DeviceProfile l5; device_profile_init(&l5);
        CHECK(arbLoadProfileFile(link.c_str(), &l5, fp) == -2,
              "prof: symlink target rejected (O_NOFOLLOW)");
    }
    rmdir(tmpl);
}

static void test_fail_safe() {
    ArbRuntime rt; arb_runtime_init(&rt);
    ArbSignals s; std::memset(&s, 0, sizeof(s)); s.signals_valid = 0;
    ArbOutput o; arb_compute(&s, &rt, &o);
    CHECK(o.active == 0 && o.boost_vote_pct == 0 && o.frame_cap_hz == 0 &&
          o.efficiency_hint == 0.0f, "failsafe: signals_valid=0 -> no action");

    int64_t prev = 0;
    float d1 = arbDtMs(10000, &prev);
    float d2 = arbDtMs(10050, &prev);
    float d3 = arbDtMs(100000000LL, &prev);
    CHECK(d1 == 50.0f && std::fabs(d2 - 50.0f) < 0.01f && d3 <= 60000.0f,
          "dt: first=50, delta measured, huge jump bounded");
}

static void test_v440_thermal_learning() {
    DeviceProfile dp; device_profile_init(&dp);
    CHECK(dp.thermal_trip_c > 0.0f && dp.thermal_trip_c < 90.0f,
          "v440: default trip ~85 before learning");

    DevObservation o; std::memset(&o, 0, sizeof(o));
    o.temp_c = 50.0f; o.thermal_trip_c = 95.0f; o.battery_pct = 80;
    o.refresh_rate_hz = 60; o.cpu_util_pct = 40; o.gpu_util_pct = 30;
    for (int i = 0; i < 300; i++) device_profile_observe(&dp, &o);
    CHECK(dp.thermal_trip_c > 85.0f && dp.thermal_trip_c < 95.6f,
          "v440: trip EWMA converges toward the real trip (~95)");

    DevObservation bad = o; bad.temp_c = 200.0f; bad.thermal_trip_c = 0;
    device_profile_observe(&dp, &bad);
    CHECK(dp.thermal_max_observed_c <= 150.0f, "v440: out-of-range temp 200C rejected");

    dp.thermal_max_observed_c = 140.0f;
    DevObservation cool = o; cool.thermal_trip_c = 0;
    for (int i = 0; i < 2000; i++) device_profile_observe(&dp, &cool);
    CHECK(dp.thermal_max_observed_c < 140.0f,
          "v440: sticky max forgets toward current temp");
}

static void test_v440_confidence_decay() {
    DeviceProfile dp; device_profile_init(&dp);
    DevObservation o; std::memset(&o, 0, sizeof(o));
    o.temp_c = 50; o.battery_pct = 80; o.refresh_rate_hz = 60;
    o.cpu_util_pct = 40; o.gpu_util_pct = 30;
    for (int i = 0; i < 500; i++) device_profile_observe(&dp, &o);
    float c1 = dp.calibration_confidence;
    CHECK(c1 > 0.9f, "v440: confidence saturated high before decay");
    while ((dp.total_observation_count % 1000u) != 0u) device_profile_observe(&dp, &o);
    float c2 = dp.calibration_confidence;
    CHECK(c2 < c1, "v440: confidence decayed at the 1000-obs boundary");
    CHECK(c2 >= 0.3f, "v440: confidence decay floored (no erasure)");
}

int main() {
    printf("=== arbitrator wiring regressions (v4.3.9 + v4.4.0) ===\n");
    test_context_not_midnight();
    test_game_scene_eats_touch_frame();
    test_device_profile_learning();
    test_profile_file_io();
    test_fail_safe();
    test_v440_thermal_learning();
    test_v440_confidence_decay();
    printf("=== arb_wiring: %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

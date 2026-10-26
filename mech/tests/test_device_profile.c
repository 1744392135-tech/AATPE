#include "test_harness.h"
#include "device_profile.h"

#include <math.h>
#include <string.h>

/* ---- normal: init, observe, calibrate ---- */
static int test_normal_calibration(void)
{
    DeviceProfile dp;
    CalibratedThresholds ct;
    device_profile_init(&dp);

    /* Set up a 3-cluster topology: little, mid, big. */
    DevCluster clusters[3];
    memset(clusters, 0, sizeof(clusters));
    clusters[0].cpu_mask = 0x0Fu;    /* 4 little cores */
    clusters[0].max_freq_khz = 1800000;
    clusters[0].min_freq_khz = 300000;
    clusters[1].cpu_mask = 0x30u;    /* 2 mid cores */
    clusters[1].max_freq_khz = 2400000;
    clusters[1].min_freq_khz = 600000;
    clusters[2].cpu_mask = 0x40u;    /* 1 big core */
    clusters[2].max_freq_khz = 3000000;
    clusters[2].min_freq_khz = 800000;

    device_profile_set_topology(&dp, clusters, 3);
    CHECK(dp.num_clusters == 3);
    /* capacity_ratio = max_freq * nr_cpus, normalized to largest cluster.
     * Cluster 0: 4 * 1.8G = 7.2 (largest total throughput)
     * Cluster 1: 2 * 2.4G = 4.8
     * Cluster 2: 1 * 3.0G = 3.0 (smallest total throughput)
     * So cluster 0 should have ratio 1.0, cluster 2 < 1.0. */
    CHECK(dp.clusters[0].capacity_ratio == 1.0f);
    CHECK(dp.clusters[2].capacity_ratio < 1.0f);

    /* Feed observations. */
    DevObservation obs;
    memset(&obs, 0, sizeof(obs));
    obs.temp_c = 45.0f;
    obs.thermal_trip_c = 85.0f;
    obs.charging = 0;
    obs.battery_pct = 80.0f;
    obs.refresh_rate_hz = 60.0f;
    obs.cpu_util_pct = 30.0f;
    obs.gpu_util_pct = 20.0f;

    for (int i = 0; i < 100; i++)
        device_profile_observe(&dp, &obs);

    CHECK(dp.total_observation_count == 100u);
    CHECK(dp.num_refresh_rates >= 1);
    CHECK(dp.thermal_margin_norm > 0.0f);

    /* Record boost observations. */
    DevBoostObservation bobs;
    bobs.frame_time_before_ms = 20.0f;
    bobs.frame_time_after_ms  = 14.0f;
    bobs.boost_level = 80.0f;
    device_profile_record_boost(&dp, &bobs);
    CHECK(dp.boost_observation_count == 1u);
    CHECK(dp.boost_effectiveness > 0.0f);

    /* Calibrate. */
    device_profile_calibrate(&dp, &ct);
    CHECK(ct.cpu_high_load_pct > 0.0f);
    CHECK(ct.thermal_warning_margin_c > 0.0f);
    CHECK(ct.default_boost_level > 0.0f);

    CHECK(device_profile_is_valid(&dp) == 1);

    return test_summary("device_profile normal calibration");
}

/* ---- boundary: insufficient data => defaults ---- */
static int test_boundary_insufficient_data(void)
{
    DeviceProfile dp;
    CalibratedThresholds ct;
    device_profile_init(&dp);

    /* Only 5 observations: calibration confidence should be low. */
    DevObservation obs;
    memset(&obs, 0, sizeof(obs));
    obs.temp_c = 40.0f;
    obs.refresh_rate_hz = 60.0f;

    for (int i = 0; i < 5; i++)
        device_profile_observe(&dp, &obs);

    device_profile_calibrate(&dp, &ct);
    /* Should return defaults (60, 55, 10, 50, 80). */
    CHECK(ct.cpu_high_load_pct == 60.0f);
    CHECK(ct.gpu_high_load_pct == 55.0f);
    CHECK(ct.default_boost_level == 50.0f);

    return test_summary("device_profile boundary insufficient data");
}

/* ---- fault injection: NaN / NULL ---- */
static int test_fault_injection(void)
{
    DeviceProfile dp;
    CalibratedThresholds ct;
    device_profile_init(&dp);

    /* NULL inputs */
    device_profile_observe(NULL, NULL);
    device_profile_set_topology(NULL, NULL, 0);
    device_profile_calibrate(NULL, NULL);
    CHECK(device_profile_is_valid(NULL) == 0);

    /* NaN observations */
    DevObservation obs;
    memset(&obs, 0, sizeof(obs));
    obs.temp_c = NAN;
    obs.refresh_rate_hz = NAN;
    obs.cpu_util_pct = NAN;
    obs.gpu_util_pct = NAN;
    obs.battery_pct = NAN;

    for (int i = 0; i < 20; i++)
        device_profile_observe(&dp, &obs);

    CHECK(isfinite(dp.thermal_margin_norm));
    CHECK(isfinite(dp.calibration_confidence));

    /* Calibrate should still return sane defaults even with NaN history. */
    device_profile_calibrate(&dp, &ct);
    CHECK(isfinite(ct.cpu_high_load_pct));

    /* NaN boost observation */
    DevBoostObservation bobs;
    bobs.frame_time_before_ms = NAN;
    bobs.frame_time_after_ms = NAN;
    bobs.boost_level = NAN;
    device_profile_record_boost(&dp, &bobs);
    CHECK(isfinite(dp.boost_effectiveness));

    /* Zero before-frame-time (divide-by-zero guard) */
    bobs.frame_time_before_ms = 0.0f;
    bobs.frame_time_after_ms = 10.0f;
    device_profile_record_boost(&dp, &bobs);
    CHECK(isfinite(dp.boost_effectiveness));

    /* Serialize / deserialize round-trip */
    uint8_t buf[512];
    int wlen = device_profile_serialize(&dp, buf, sizeof(buf));
    CHECK(wlen > 0);

    DeviceProfile dp2;
    int rlen = device_profile_deserialize(&dp2, buf, (uint32_t)wlen);
    CHECK(rlen > 0);
    CHECK(device_profile_is_valid(&dp2) == 1);
    CHECK(dp2.boost_observation_count == dp.boost_observation_count);

    /* Corrupted magic */
    DeviceProfile dp3;
    memcpy(&dp3, &dp, sizeof(dp));
    dp3.magic = 0xDEADBEEF;
    CHECK(device_profile_is_valid(&dp3) == 0);

    return test_summary("device_profile fault injection");
}

/* ---- long sequence: statistics stay bounded ---- */
static int test_long_sequence_bounded(void)
{
    DeviceProfile dp;
    device_profile_init(&dp);

    DevObservation obs;
    memset(&obs, 0, sizeof(obs));

    /* 5000 alternating observations. */
    for (int i = 0; i < 5000; i++) {
        obs.temp_c = 35.0f + (float)(i % 30);   /* 35..64C */
        obs.refresh_rate_hz = (i % 3 == 0) ? 60.0f : 120.0f;
        obs.cpu_util_pct = (float)(i % 100);
        obs.gpu_util_pct = (float)(i % 80);
        device_profile_observe(&dp, &obs);

        CHECK(isfinite(dp.thermal_margin_norm));
        CHECK(isfinite(dp.calibration_confidence));
        CHECK(dp.thermal_margin_norm >= 0.0f && dp.thermal_margin_norm <= 1.0f);
        CHECK(dp.calibration_confidence >= 0.0f && dp.calibration_confidence <= 1.0f);
        CHECK(dp.thermal_max_observed_c >= dp.thermal_min_observed_c);
    }

    CHECK(dp.num_refresh_rates <= DEV_MAX_REFRESH_RATES);

    return test_summary("device_profile long sequence bounded");
}

/* ---- refresh rate set accumulation ---- */
static int test_refresh_rate_accumulation(void)
{
    DeviceProfile dp;
    device_profile_init(&dp);

    DevObservation obs;
    memset(&obs, 0, sizeof(obs));
    obs.temp_c = 40.0f;

    float rates[] = {30.0f, 60.0f, 90.0f, 120.0f, 144.0f, 30.0f, 60.0f};
    for (int i = 0; i < 7; i++) {
        obs.refresh_rate_hz = rates[i];
        device_profile_observe(&dp, &obs);
    }

    CHECK(dp.num_refresh_rates == 5); /* 5 unique rates */

    return test_summary("device_profile refresh rate accumulation");
}

int main(void)
{
    int fails = 0;
    fails += test_normal_calibration();
    fails += test_boundary_insufficient_data();
    fails += test_fault_injection();
    fails += test_long_sequence_bounded();
    fails += test_refresh_rate_accumulation();
    printf("== device_profile ALL: %s ==\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

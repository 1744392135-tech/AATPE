#include "device_profile.h"

#include <math.h>
#include <string.h>

#define DEV_PROFILE_MAGIC 0xDE400002u
#define DEV_EWMA_ALPHA_THERMAL   0.05f
#define DEV_EWMA_ALPHA_BOOST     0.2f
#define DEV_CALIBRATION_MIN_OBS  50u
#define DEV_TEMP_LO_C            0.0f
#define DEV_TEMP_HI_C            150.0f
#define DEV_EXTREME_FORGET       0.001f

#define DEV_TRIP_EWMA_ALPHA      0.2f
#define DEV_CONF_DECAY_EVERY     1000u
#define DEV_CONF_DECAY           0.95f

static uint64_t fnv_rotate(uint64_t h, uint64_t v)
{
    h ^= v & 0xFFu;  h *= 0x100000001B3ull;
    h ^= (v >> 8) & 0xFFu;  h *= 0x100000001B3ull;
    h ^= (v >> 16) & 0xFFu; h *= 0x100000001B3ull;
    h ^= (v >> 24) & 0xFFu; h *= 0x100000001B3ull;
    return h;
}

uint64_t device_profile_fingerprint(const DevCluster *clusters, uint32_t num_clusters)
{
    if (!clusters || num_clusters == 0 || num_clusters > DEV_MAX_CLUSTERS)
        return 0ull;
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv_rotate(h, (uint64_t)num_clusters);
    for (uint32_t i = 0; i < num_clusters; i++) {
        h = fnv_rotate(h, (uint64_t)clusters[i].cpu_mask);
        h = fnv_rotate(h, (uint64_t)clusters[i].min_freq_khz);
        h = fnv_rotate(h, (uint64_t)clusters[i].max_freq_khz);
    }
    return h;
}

static float sanf_c(float v)
{
    if (!isfinite(v)) return 0.0f;
    return v;
}

__attribute__((unused))
static float sanf_pct(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void device_profile_init(DeviceProfile *dp)
{
    if (!dp) return;
    memset(dp, 0, sizeof(*dp));
    dp->magic = DEV_PROFILE_MAGIC;
    strncpy(dp->name, "unknown", DEV_NAME_MAX_LEN - 1);

    dp->thermal_trip_c          = 85.0f;
    dp->thermal_max_observed_c  = 0.0f;
    dp->thermal_min_observed_c   = 100.0f;
    dp->thermal_margin_norm     = 1.0f;

    dp->boost_effectiveness     = 0.5f;
    dp->boost_observation_count = 0;
    dp->calibration_confidence  = 0.0f;
    dp->total_observation_count = 0;
}

void device_profile_set_topology(DeviceProfile *dp,
                                  const DevCluster *clusters,
                                  uint32_t num_clusters)
{
    if (!dp || !clusters || num_clusters == 0 || num_clusters > DEV_MAX_CLUSTERS)
        return;

    dp->num_clusters = num_clusters;

    dp->topology_fingerprint = device_profile_fingerprint(clusters, num_clusters);
    uint32_t max_score = 0;

    for (uint32_t i = 0; i < num_clusters; i++) {
        dp->clusters[i] = clusters[i];

        uint32_t nr_cpus = 0;
        uint32_t mask = clusters[i].cpu_mask;
        while (mask) { nr_cpus += (mask & 1u); mask >>= 1; }
        uint32_t score = clusters[i].max_freq_khz * nr_cpus;
        dp->clusters[i].capacity_ratio = 0.0f;
        if (score > max_score) max_score = score;
    }

    if (max_score > 0) {
        for (uint32_t i = 0; i < num_clusters; i++) {
            uint32_t nr_cpus = 0;
            uint32_t mask = dp->clusters[i].cpu_mask;
            while (mask) { nr_cpus += (mask & 1u); mask >>= 1; }
            uint32_t score = dp->clusters[i].max_freq_khz * nr_cpus;
            dp->clusters[i].capacity_ratio = (float)score / (float)max_score;
        }
    }
}

void device_profile_observe(DeviceProfile *dp, const DevObservation *obs)
{
    if (!dp || !obs) return;

    dp->total_observation_count++;

    float temp = sanf_c(obs->temp_c);
    int temp_valid = (temp >= DEV_TEMP_LO_C && temp <= DEV_TEMP_HI_C) ? 1 : 0;
    if (temp_valid) {
        if (temp > dp->thermal_max_observed_c) dp->thermal_max_observed_c = temp;
        if (temp < dp->thermal_min_observed_c) dp->thermal_min_observed_c = temp;
        dp->thermal_max_observed_c += (temp - dp->thermal_max_observed_c) * DEV_EXTREME_FORGET;
        dp->thermal_min_observed_c += (temp - dp->thermal_min_observed_c) * DEV_EXTREME_FORGET;
    }

    float trip = sanf_c(obs->thermal_trip_c);
    if (trip > 0.0f && trip < 150.0f) {
        dp->thermal_trip_c += DEV_TRIP_EWMA_ALPHA * (trip - dp->thermal_trip_c);
    }

    if (dp->thermal_trip_c > 0.0f && temp_valid) {
        float margin = (dp->thermal_trip_c - temp) / dp->thermal_trip_c;
        dp->thermal_margin_norm = clampf(margin, 0.0f, 1.0f);
    }

    float rr = sanf_c(obs->refresh_rate_hz);
    if (rr > 1.0f && rr < 1000.0f) {
        int found = 0;
        for (uint32_t i = 0; i < dp->num_refresh_rates; i++) {
            if (fabsf(dp->refresh_rates[i] - rr) < 1.0f) { found = 1; break; }
        }
        if (!found && dp->num_refresh_rates < DEV_MAX_REFRESH_RATES) {
            dp->refresh_rates[dp->num_refresh_rates++] = rr;
        }
    }

    float target = clampf((float)dp->total_observation_count /
                           (float)DEV_CALIBRATION_MIN_OBS, 0.0f, 1.0f);

    dp->calibration_confidence += DEV_EWMA_ALPHA_THERMAL *
                                 (target - dp->calibration_confidence);

    if (dp->total_observation_count % DEV_CONF_DECAY_EVERY == 0) {
        dp->calibration_confidence *= DEV_CONF_DECAY;
        if (dp->calibration_confidence < 0.3f) dp->calibration_confidence = 0.3f;
    }
}

void device_profile_record_boost(DeviceProfile *dp,
                                  const DevBoostObservation *obs)
{
    if (!dp || !obs) return;

    float before = sanf_c(obs->frame_time_before_ms);
    float after  = sanf_c(obs->frame_time_after_ms);

    if (before <= 0.0f || after < 0.0f) return;

    float eff = (before - after) / before;
    eff = clampf(eff, 0.0f, 1.0f);

    dp->boost_effectiveness += DEV_EWMA_ALPHA_BOOST *
                              (eff - dp->boost_effectiveness);
    dp->boost_observation_count++;
}

void device_profile_calibrate(const DeviceProfile *dp, CalibratedThresholds *out)
{
    if (!dp || !out) return;

    out->cpu_high_load_pct       = 60.0f;
    out->gpu_high_load_pct       = 55.0f;
    out->thermal_warning_margin_c = 10.0f;
    out->default_boost_level     = 50.0f;
    out->ingame_boost_level      = 80.0f;
    out->cutscene_frame_cap_hz   = 0.0f;

    float conf = clampf(dp->calibration_confidence, 0.0f, 1.0f);

    if (conf < 0.3f) return;

    if (dp->num_clusters > 0) {
        float max_cap = 0.0f;
        for (uint32_t i = 0; i < dp->num_clusters; i++) {
            if (dp->clusters[i].capacity_ratio > max_cap)
                max_cap = dp->clusters[i].capacity_ratio;
        }

        out->cpu_high_load_pct += max_cap * 15.0f * conf;
    }

    if (dp->thermal_max_observed_c > 0.0f && dp->thermal_trip_c > 0.0f) {
        float headroom = dp->thermal_trip_c - dp->thermal_max_observed_c;

        out->thermal_warning_margin_c = clampf(headroom * 0.5f, 5.0f, 15.0f);
    }

    float eff = clampf(dp->boost_effectiveness, 0.0f, 1.0f);
    out->default_boost_level = 30.0f + eff * 40.0f * conf;
    out->ingame_boost_level  = 60.0f + eff * 30.0f * conf;

    float lowest_usable = 0.0f;
    for (uint32_t i = 0; i < dp->num_refresh_rates; i++) {
        float rr = dp->refresh_rates[i];
        if (rr >= 30.0f && rr < 100.0f) {
            if (lowest_usable == 0.0f || rr < lowest_usable)
                lowest_usable = rr;
        }
    }
    out->cutscene_frame_cap_hz = lowest_usable;
}

uint32_t device_profile_serialized_size(void)
{
    return (uint32_t)sizeof(DeviceProfile);
}

int device_profile_serialize(const DeviceProfile *dp,
                               uint8_t *buf, uint32_t buf_len)
{
    if (!dp || !buf) return -1;
    if (buf_len < sizeof(DeviceProfile)) return -1;
    memcpy(buf, dp, sizeof(DeviceProfile));
    return (int)sizeof(DeviceProfile);
}

int device_profile_deserialize(DeviceProfile *dp,
                                 const uint8_t *buf, uint32_t buf_len)
{
    if (!dp || !buf) return -1;
    if (buf_len < sizeof(DeviceProfile)) return -1;
    memcpy(dp, buf, sizeof(DeviceProfile));
    if (dp->magic != DEV_PROFILE_MAGIC) return -1;
    return (int)sizeof(DeviceProfile);
}

int device_profile_is_valid(const DeviceProfile *dp)
{
    if (!dp) return 0;
    if (dp->magic != DEV_PROFILE_MAGIC) return 0;
    if (dp->num_clusters > DEV_MAX_CLUSTERS) return 0;
    if (dp->num_refresh_rates > DEV_MAX_REFRESH_RATES) return 0;
    if (!isfinite(dp->boost_effectiveness)) return 0;
    if (!isfinite(dp->calibration_confidence)) return 0;
    if (dp->thermal_trip_c < 0.0f || dp->thermal_trip_c > 150.0f) return 0;
    return 1;
}

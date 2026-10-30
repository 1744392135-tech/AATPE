#ifndef AATPE_DEVICE_PROFILE_H
#define AATPE_DEVICE_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define DEV_MAX_CLUSTERS       8
#define DEV_MAX_REFRESH_RATES  8
#define DEV_NAME_MAX_LEN       32

typedef struct {
    uint32_t cpu_mask;
    uint32_t max_freq_khz;
    uint32_t min_freq_khz;
    float capacity_ratio;
} DevCluster;

typedef struct {

    float frame_time_before_ms;

    float frame_time_after_ms;

    float boost_level;
} DevBoostObservation;

typedef struct {

    float temp_c;

    float thermal_trip_c;

    int charging;

    float battery_pct;

    float refresh_rate_hz;

    float cpu_util_pct;

    float gpu_util_pct;
} DevObservation;

typedef struct {

    char     name[DEV_NAME_MAX_LEN];
    uint32_t magic;
    uint64_t topology_fingerprint;

    uint32_t   num_clusters;
    DevCluster clusters[DEV_MAX_CLUSTERS];

    uint32_t num_refresh_rates;
    float    refresh_rates[DEV_MAX_REFRESH_RATES];

    float thermal_trip_c;
    float thermal_max_observed_c;
    float thermal_min_observed_c;
    float thermal_margin_norm;

    float boost_effectiveness;
    uint32_t boost_observation_count;

    float calibration_confidence;
    uint32_t total_observation_count;
} DeviceProfile;

typedef struct {

    float cpu_high_load_pct;

    float gpu_high_load_pct;

    float thermal_warning_margin_c;

    float default_boost_level;

    float ingame_boost_level;

    float cutscene_frame_cap_hz;
} CalibratedThresholds;

void device_profile_init(DeviceProfile *dp);

void device_profile_set_topology(DeviceProfile *dp,
                                  const DevCluster *clusters,
                                  uint32_t num_clusters);

uint64_t device_profile_fingerprint(const DevCluster *clusters, uint32_t num_clusters);

void device_profile_observe(DeviceProfile *dp, const DevObservation *obs);

void device_profile_record_boost(DeviceProfile *dp,
                                  const DevBoostObservation *obs);

void device_profile_calibrate(const DeviceProfile *dp, CalibratedThresholds *out);

uint32_t device_profile_serialized_size(void);
int      device_profile_serialize(const DeviceProfile *dp,
                                    uint8_t *buf, uint32_t buf_len);
int      device_profile_deserialize(DeviceProfile *dp,
                                     const uint8_t *buf, uint32_t buf_len);

int device_profile_is_valid(const DeviceProfile *dp);

#ifdef __cplusplus
}
#endif

#endif

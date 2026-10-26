/*
 * device_profile.h - Device self-portrait & adaptive calibration (pure logic).
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 *
 * The device profile is a *learned* description of the running hardware:
 *   - cluster count and per-cluster capacity tiers
 *   - supported refresh-rate set
 *   - thermal trip points and observed margin
 *   - measured boost effectiveness (how much schedtune.boost actually helps)
 *
 * This module provides:
 *   1. A serializable profile struct. Persistence is best-effort: when a
 *      writable flash store exists it survives reboots; otherwise the daemon
 *      falls back to tmpfs and the profile is SINGLE-BOOT learning only. The
 *      profile is always bound to a CPU-topology fingerprint and a mismatch
 *      causes it to be discarded.
 *   2. Observation ingestion: feed raw measurements, the profile updates.
 *   3. Normalized queries: capacity ratios, thermal margin, boost efficacy.
 *   4. Threshold calibration: derive device-specific decision thresholds
 *      from the observed hardware, with safe defaults as fallback.
 *
 * Zero hardcoding: no SoC names, no fixed frequency tables. Everything is
 * learned from observations or falls back to conservative defaults.
 *
 * Design basis:
 *   - Blueprint 设计-5: cross-device generalization relied on auto-detection
 *     but scheduling parameters were universal. This core learns per-device
 *     calibration so thresholds scale with the actual hardware.
 *   - Reference: Xiaomi thermal level feedback; OPPO cold/hot start timed boost.
 */
#ifndef AATPE_DEVICE_PROFILE_H
#define AATPE_DEVICE_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ------- limits ------- */
#define DEV_MAX_CLUSTERS       8
#define DEV_MAX_REFRESH_RATES  8
#define DEV_NAME_MAX_LEN       32

/* ------- cluster descriptor ------- */
typedef struct {
    uint32_t cpu_mask;           /* bitmask of CPUs in this cluster */
    uint32_t max_freq_khz;       /* max frequency of this cluster */
    uint32_t min_freq_khz;       /* min frequency */
    /* Normalized capacity: 0.0 = smallest cluster, 1.0 = largest.
     * Computed from max_freq * num_cpus. */
    float capacity_ratio;
} DevCluster;

/* ------- boost effectiveness observation ------- */
typedef struct {
    /* Before boost: average frame time (ms) under load. */
    float frame_time_before_ms;
    /* After boost: average frame time (ms) under same-ish load. */
    float frame_time_after_ms;
    /* Observed at what boost level (0..100). */
    float boost_level;
} DevBoostObservation;

/* ------- raw observation (one tick's worth) ------- */
typedef struct {
    /* Thermal: current skin/sensor temp in Celsius. */
    float temp_c;
    /* Thermal trip threshold if known (0 = unknown). */
    float thermal_trip_c;
    /* Current charging state: 1 = charging, 0 = on battery. */
    int charging;
    /* Current battery level percent [0..100]. */
    float battery_pct;
    /* Current observed refresh rate Hz. */
    float refresh_rate_hz;
    /* Current average CPU utilization percent [0..100]. */
    float cpu_util_pct;
    /* Current average GPU utilization percent [0..100]. */
    float gpu_util_pct;
} DevObservation;

/* ------- the persistent device profile ------- */
typedef struct {
    /* --- identity --- */
    char     name[DEV_NAME_MAX_LEN];   /* e.g. SoC codename or "unknown" */
    uint32_t magic;                    /* version magic for serialization */
    /* FNV-1a fingerprint of the CPU topology (filled by set_topology). v4.3.9: a
     * profile loaded from flash is discarded unless this matches the CURRENT
     * topology fingerprint -- prevents an OTA / swap-device / different-device
     * profile from being adopted (audit4_state F10). */
    uint64_t topology_fingerprint;

    /* --- topology --- */
    uint32_t   num_clusters;
    DevCluster clusters[DEV_MAX_CLUSTERS];

    /* --- refresh rates observed --- */
    uint32_t num_refresh_rates;
    float    refresh_rates[DEV_MAX_REFRESH_RATES];

    /* --- thermal learned --- */
    float thermal_trip_c;              /* learned or provided trip point */
    float thermal_max_observed_c;     /* hottest temp seen */
    float thermal_min_observed_c;     /* coolest temp seen */
    float thermal_margin_norm;        /* 0.0 = at trip, 1.0 = very cool */

    /* --- boost effectiveness --- */
    /* EWMA of how much boost reduces frame time:
     *   0.0 = boost has no effect, 1.0 = boost halves frame time. */
    float boost_effectiveness;
    uint32_t boost_observation_count;

    /* --- calibration confidence --- */
    /* 0.0 = brand new / no data, 1.0 = fully calibrated. */
    float calibration_confidence;
    uint32_t total_observation_count;
} DeviceProfile;

/* ------- calibrated thresholds output ------- */
typedef struct {
    /* Suggested CPU util threshold for "high load" (normalized 0..100). */
    float cpu_high_load_pct;
    /* Suggested GPU util threshold for "high load". */
    float gpu_high_load_pct;
    /* Suggested thermal warning margin below trip (Celsius). */
    float thermal_warning_margin_c;
    /* Suggested default boost level (0..100). */
    float default_boost_level;
    /* Suggested min boost level when in-game. */
    float ingame_boost_level;
    /* Frame cap suggestion for cutscene (Hz). 0 = unknown. */
    float cutscene_frame_cap_hz;
} CalibratedThresholds;

/* ------- API ------- */

/* Initialize a blank profile with conservative defaults. */
void device_profile_init(DeviceProfile *dp);

/* Provide cluster topology (called once at startup when topology is known).
 * num_clusters must be <= DEV_MAX_CLUSTERS. Clusters are copied and
 * capacity_ratio is computed. */
void device_profile_set_topology(DeviceProfile *dp,
                                  const DevCluster *clusters,
                                  uint32_t num_clusters);

/* Compute the FNV-1a topology fingerprint from a cluster set. Pure/deterministic.
 * Returns 0 for an empty/invalid set (caller treats 0 as "no fingerprint" and
 * MUST NOT persist/adopt a fingerprint=0 profile). v4.3.9. */
uint64_t device_profile_fingerprint(const DevCluster *clusters, uint32_t num_clusters);

/* Feed one observation tick. The profile updates EWMA statistics.
 * NaN/inf/out-of-range inputs are safely ignored. */
void device_profile_observe(DeviceProfile *dp, const DevObservation *obs);

/* Record a boost-effectiveness observation (before/after frame time). */
void device_profile_record_boost(DeviceProfile *dp,
                                  const DevBoostObservation *obs);

/* Derive calibrated thresholds from the current profile state.
 * If calibration_confidence < 0.3, conservative defaults are returned. */
void device_profile_calibrate(const DeviceProfile *dp, CalibratedThresholds *out);

/* Serialize / deserialize the profile to/from a byte buffer.
 * Returns number of bytes written/read, or -1 on error.
 * The buffer must be at least device_profile_serialized_size() bytes. */
uint32_t device_profile_serialized_size(void);
int      device_profile_serialize(const DeviceProfile *dp,
                                    uint8_t *buf, uint32_t buf_len);
int      device_profile_deserialize(DeviceProfile *dp,
                                     const uint8_t *buf, uint32_t buf_len);

/* Sanity check: returns 1 if the profile looks valid, 0 if corrupted. */
int device_profile_is_valid(const DeviceProfile *dp);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_DEVICE_PROFILE_H */

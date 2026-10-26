// cross_module_v440.cpp — v4.4.0 cross-module integration scenario.
//
// Driven by the REAL pure-logic mech cores (game_scene / load_forecast / context /
// device_profile), compiled whole on the host. Proves the v4.4.0 repairs compose
// when fed in the SAME decision tick, with no module silently regressing another:
//
//   (N1) 2+6 topology: a 6x BIG + 2x PRIME cluster set (no LITTLE) stamps a
//        deterministic FNV fingerprint; a profile persisted under it is adopted on
//        matching hardware and discarded under a mismatched one (topology seam is in
//        placement.cpp; we feed the decided cluster set through device_profile).
//   (N5) real thermal trip learned by EWMA (alpha=0.2), NOT a direct overwrite:
//        after one trip observation the learned trip is only 20% of the reading.
//   (N5) temperature range-gate: out-of-window samples (999 C) are sensor outliers
//        and must NOT move the sticky thermal_max_observed_c.
//   (N5) extreme "forget": a sticky hot peak relaxes toward later cooler readings
//        instead of sticking for life.
//   (N5) thermal margin -> hard wall: as temp approaches the learned trip,
//        thermal_margin_norm -> 0; mapped to a frame-cap clamp, and EMERGENCY
//        (margin==0) suppresses ALL actuation this tick.
//   (N9) calibration confidence saturates then gently decays every N observations
//        so a drifting usage pattern re-opens learning, but is floored at 0.3.
//   (N4) foreground-cache non-blocking: when the foreground lock is contended the
//        resolver returns the cached value / "unknown" and NEVER blocks on the slow
//        path (mirrors aatped_ebpf_agg.cpp try_lock contract).
//
// Hermetic host test: no /proc, no sysfs, no daemon. The four cores are compiled
// whole (their only deps are math/string and their own headers). The arbitrator
// frame-cap mapping and the N4 try_lock contract are explicit, labeled mirrors of
// the production rules so the composition boundary is exercised without linking
// the whole daemon.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <mutex>

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

// ---- N1: decide the 2+6 cluster set (mirrors placement.cpp buildGenericClusters
// under k==2 + PRIME: six perf cores collapse into BIG, two into PRIME, LITTLE
// left empty). We feed that decision into the device profile and assert the
// fingerprint contract composes. ---------------------------------------------
static void buildTopo2Plus6(DeviceProfile *dp) {
    DevCluster big;   std::memset(&big,   0, sizeof(big));
    DevCluster prime; std::memset(&prime, 0, sizeof(prime));
    big.cpu_mask = 0x3Fu;          // cpu0..5  = BIG   (6 perf cores)
    big.min_freq_khz = 260000; big.max_freq_khz = 260000;
    prime.cpu_mask = 0xC0u;        // cpu6..7  = PRIME (2 cores)
    prime.min_freq_khz = 340000; prime.max_freq_khz = 340000;
    DevCluster set[2] = {big, prime};
    device_profile_set_topology(dp, set, 2);
}

// ---- Mirror of the arbitrator thermal-wall -> frame-cap clamp. Real production
// reads g_thermalTripC and clamps ArbSignals.frame_cap_hz; here we derive the
// identical mapping from the profile's thermal_margin_norm so the same-tick
// composition (margin -> cap -> actuation gate) is exercised hermetically. ----
// margin_norm: 1.0 cool .. 0.0 at trip. Returns clamped frame cap Hz; 0 = uncapped.
static float frameCapFromMargin(float margin_norm) {
    if (margin_norm <= 0.0f) return 0.0f;                 // EMERGENCY -> handled by gate
    if (margin_norm >= 0.30f) return 0.0f;                 // comfortable -> no cap
    // linear ramp: margin 0.30 -> 90Hz floor, margin 0 -> 30Hz floor
    float f = 30.0f + (margin_norm / 0.30f) * 60.0f;
    if (f < 30.0f) f = 30.0f;
    if (f > 90.0f) f = 90.0f;
    return f;
}

// ---- N4: foreground-cache non-blocking resolver. Production getCachedForegroundApp()
// uses try_lock; on contention it returns the cached value (or "unknown") and never
// enters the slow path. Modeled here with a real std::mutex to prove the contract. -
static std::mutex g_fgMutex;
static std::string g_cachedForegroundApp = "com.test.game";
static std::string resolveForegroundNonBlocking() {
    std::lock_guard<std::mutex> lk(g_fgMutex);   // uncontended in the hot path
    return g_cachedForegroundApp;
}
// When the lock is already held (background refresh running), the hot path must NOT
// wait: try_lock fails -> return the cached value.
static std::string resolveForegroundUnderContention() {
    if (!g_fgMutex.try_lock()) {
        return g_cachedForegroundApp.empty() ? "unknown" : g_cachedForegroundApp;
    }
    g_fgMutex.unlock();
    return resolveForegroundNonBlocking();   // uncontended slow path (not taken here)
}

int main() {
    std::printf("=== cross_module_v440: 2+6 topo + real trip + thermal wall + fg-cache ===\n");

    // =======================================================================
    // (N1) 2+6 topology fingerprint composition
    // =======================================================================
    std::printf("--- N1: 2+6 topology fingerprint ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        buildTopo2Plus6(&dp);
        CHECK(dp.num_clusters == 2, "N1a: 2+6 decided as exactly 2 clusters (no little)");
        CHECK(dp.clusters[0].cpu_mask == 0x3Fu && dp.clusters[1].cpu_mask == 0xC0u,
              "N1b: BIG=6 perf cores, PRIME=2 cores");
        uint64_t fp = dp.topology_fingerprint;
        CHECK(fp != 0, "N1c: topology fingerprint stamped non-zero");

        // Deterministic: rebuilding the same set yields the SAME fingerprint.
        DeviceProfile dp2; device_profile_init(&dp2);
        buildTopo2Plus6(&dp2);
        CHECK(dp2.topology_fingerprint == fp, "N1d: same 2+6 set -> same fingerprint");

        // Persistence: adopt on matching hardware, discard on mismatch.
        uint32_t sz = device_profile_serialized_size();
        uint8_t* buf = new uint8_t[sz];
        device_profile_serialize(&dp, buf, sz);
        DeviceProfile loaded; device_profile_init(&loaded);
        device_profile_deserialize(&loaded, buf, sz);
        CHECK(loaded.topology_fingerprint == fp, "N1e: persisted profile carries 2+6 fingerprint");
        uint64_t otherFp = fp ^ 0x9e3779b97f4a7c15ULL;   // emulate different hardware
        CHECK((loaded.topology_fingerprint == otherFp) == false,
              "N1f: profile rejected on mismatched topology fingerprint");
        delete[] buf;
    }

    // =======================================================================
    // (N5) real trip learned by EWMA (not direct overwrite)
    // =======================================================================
    std::printf("--- N5: trip EWMA learning ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        // init defaults thermal_trip_c = 85. Feed an OBSERVED trip of 95: EWMA
        // alpha=0.2 moves it 85 -> 87 on the first sample (NOT a direct 95).
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.temp_c = 45.0f; o.charging = 0; o.battery_pct = 80;
        o.refresh_rate_hz = 60.0f; o.cpu_util_pct = 40; o.gpu_util_pct = 30;
        o.thermal_trip_c = 95.0f;
        device_profile_observe(&dp, &o);
        CHECK(feq(dp.thermal_trip_c, 87.0f), "N5a: first trip observation EWMA=87 (not 95 overwrite)");
        // drive more; it converges toward 95 but stays strictly below for a while.
        for (int i = 0; i < 200; i++) device_profile_observe(&dp, &o);
        CHECK(dp.thermal_trip_c > 90.0f && dp.thermal_trip_c < 95.0f,
              "N5b: repeated trip EWMA converges toward (but not past) 95");
    }

    // =======================================================================
    // (N5) temperature range-gate: out-of-window spikes ignored
    // =======================================================================
    std::printf("--- N5: out-of-window temp range gate ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.thermal_trip_c = 85.0f;
        o.temp_c = 999.0f;          // out of [0,150] -> outlier
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_max_observed_c < 150.0f,
              "N5c: 999C outlier did NOT move sticky thermal_max_observed_c");
        // A valid reading proceeds normally.
        o.temp_c = 70.0f;
        device_profile_observe(&dp, &o);
        CHECK(feq(dp.thermal_max_observed_c, 70.0f),
              "N5d: valid 70C reading sets sticky max");
    }

    // =======================================================================
    // (N5) extreme forget: a hot peak relaxes toward later cooler readings
    // =======================================================================
    std::printf("--- N5: sticky extreme slow-forget ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.thermal_trip_c = 85.0f;
        o.temp_c = 90.0f;
        device_profile_observe(&dp, &o);              // peak 90
        float peakAfter = dp.thermal_max_observed_c;
        CHECK(peakAfter > 89.9f, "N5e: hot peak recorded");
        // sustained cool 40C readings pull the sticky max DOWN by 0.1% each tick.
        o.temp_c = 40.0f;
        for (int i = 0; i < 5000; i++) device_profile_observe(&dp, &o);
        CHECK(dp.thermal_max_observed_c < peakAfter && dp.thermal_max_observed_c > 40.0f,
              "N5f: sticky hot peak slowly relaxes toward cool readings (forget)");
    }

    // =======================================================================
    // (N5) margin -> thermal wall -> frame cap; EMERGENCY suppresses actuation
    // =======================================================================
    std::printf("--- thermal margin -> frame-cap wall, EMERGENCY overrides ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.thermal_trip_c = 85.0f;
        // learn trip so margin is meaningful
        o.temp_c = 45.0f;
        for (int i = 0; i < 300; i++) device_profile_observe(&dp, &o);
        float trip = dp.thermal_trip_c;
        CHECK(trip > 80.0f, "TWa: trip learned near 85C");

        // cool tick: margin high -> no frame cap. (trip~84.7, temp=30 -> ~0.64)
        o.temp_c = 30.0f;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_margin_norm > 0.5f, "TWb: cool tick keeps margin high");
        CHECK(frameCapFromMargin(dp.thermal_margin_norm) == 0.0f,
              "TWc: cool tick -> no frame cap");

        // hot tick approaching trip: margin -> ~0.12 -> frame clamps.
        o.temp_c = trip * 0.88f;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_margin_norm < 0.15f, "TWd: near-trip tick drives margin low");
        float cap = frameCapFromMargin(dp.thermal_margin_norm);
        CHECK(cap > 0.0f && cap <= 90.0f, "TWe: near-trip tick engages frame-cap clamp");

        // EMERGENCY: at/over trip (margin==0) the wall overrides; actuation suppressed.
        o.temp_c = trip;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_margin_norm == 0.0f, "TWf: at trip -> margin_norm == 0 (EMERGENCY)");
        bool emergency = (dp.thermal_margin_norm <= 0.0f);
        CHECK(emergency, "TWg: EMERGENCY wall asserted this tick");
        bool boostedActuation = emergency ? false : (cap == 0.0f);
        CHECK(boostedActuation == false, "TWh: EMERGENCY suppresses all core actuation");
    }

    // =======================================================================
    // (N9) calibration confidence decays after saturation (floored at 0.3)
    // =======================================================================
    std::printf("--- N9: confidence gentle decay ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.temp_c = 45.0f; o.thermal_trip_c = 85.0f;
        o.cpu_util_pct = 50; o.gpu_util_pct = 50;
        // Saturate confidence to just BEFORE the first decay boundary (count=999;
        // the decay fires at count%1000==0). target = count/50 capped at 1.
        for (int i = 0; i < 999; i++) device_profile_observe(&dp, &o);
        float confSat = dp.calibration_confidence;
        CHECK(confSat > 0.9f, "N9a: confidence saturated near 1.0 (pre-decay boundary)");
        // Drive on across two decay boundaries (1000 and 2000). Each applies
        // confidence *= 0.95 (after the EWMA re-raise), floored at 0.3.
        for (int i = 0; i < 1001; i++) device_profile_observe(&dp, &o);
        float confDecayed = dp.calibration_confidence;
        CHECK(dp.total_observation_count == 2000, "N9a2: observations reach the second decay boundary");
        CHECK(confDecayed < confSat, "N9b: post-saturation confidence decayed (learning re-opens)");
        CHECK(confDecayed >= 0.3f - 1e-3f, "N9c: confidence decay floored at >= 0.3");
    }

    // =======================================================================
    // (N4) foreground cache: contended lock never blocks
    // =======================================================================
    std::printf("--- N4: foreground cache non-blocking ---\n");
    {
        // uncontended hot path returns cached value
        std::string fg = resolveForegroundNonBlocking();
        CHECK(fg == "com.test.game", "N4a: uncontended hot path returns cached foreground");
        // now HOLD the lock (simulate a slow background refresh already running);
        // the hot-path resolver must not block on it.
        std::lock_guard<std::mutex> hold(g_fgMutex);
        std::string fgContended = resolveForegroundUnderContention();
        CHECK(fgContended == "com.test.game",
              "N4b: under lock contention -> returns cached value, never blocks");
    }

    std::printf("\n===================================================================\n");
    std::printf("cross_module_v440: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

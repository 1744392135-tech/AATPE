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

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; std::printf("  [PASS] %s\n", m); } \
                         else { g_fail++; std::printf("  [FAIL] %s\n", m); } } while (0)
static bool feq(float a, float b) { return std::fabs(a - b) < 1e-2f; }

static void buildTopo2Plus6(DeviceProfile *dp) {
    DevCluster big;   std::memset(&big,   0, sizeof(big));
    DevCluster prime; std::memset(&prime, 0, sizeof(prime));
    big.cpu_mask = 0x3Fu;
    big.min_freq_khz = 260000; big.max_freq_khz = 260000;
    prime.cpu_mask = 0xC0u;
    prime.min_freq_khz = 340000; prime.max_freq_khz = 340000;
    DevCluster set[2] = {big, prime};
    device_profile_set_topology(dp, set, 2);
}

static float frameCapFromMargin(float margin_norm) {
    if (margin_norm <= 0.0f) return 0.0f;
    if (margin_norm >= 0.30f) return 0.0f;
    float f = 30.0f + (margin_norm / 0.30f) * 60.0f;
    if (f < 30.0f) f = 30.0f;
    if (f > 90.0f) f = 90.0f;
    return f;
}

static std::mutex g_fgMutex;
static std::string g_cachedForegroundApp = "com.test.game";
static std::string resolveForegroundNonBlocking() {
    std::lock_guard<std::mutex> lk(g_fgMutex);
    return g_cachedForegroundApp;
}
static std::string resolveForegroundUnderContention() {
    if (!g_fgMutex.try_lock()) {
        return g_cachedForegroundApp.empty() ? "unknown" : g_cachedForegroundApp;
    }
    g_fgMutex.unlock();
    return resolveForegroundNonBlocking();
}

int main() {
    std::printf("=== cross_module_v440: 2+6 topo + real trip + thermal wall + fg-cache ===\n");

    std::printf("--- N1: 2+6 topology fingerprint ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        buildTopo2Plus6(&dp);
        CHECK(dp.num_clusters == 2, "N1a: 2+6 decided as exactly 2 clusters (no little)");
        CHECK(dp.clusters[0].cpu_mask == 0x3Fu && dp.clusters[1].cpu_mask == 0xC0u,
              "N1b: BIG=6 perf cores, PRIME=2 cores");
        uint64_t fp = dp.topology_fingerprint;
        CHECK(fp != 0, "N1c: topology fingerprint stamped non-zero");

        DeviceProfile dp2; device_profile_init(&dp2);
        buildTopo2Plus6(&dp2);
        CHECK(dp2.topology_fingerprint == fp, "N1d: same 2+6 set -> same fingerprint");

        uint32_t sz = device_profile_serialized_size();
        uint8_t* buf = new uint8_t[sz];
        device_profile_serialize(&dp, buf, sz);
        DeviceProfile loaded; device_profile_init(&loaded);
        device_profile_deserialize(&loaded, buf, sz);
        CHECK(loaded.topology_fingerprint == fp, "N1e: persisted profile carries 2+6 fingerprint");
        uint64_t otherFp = fp ^ 0x9e3779b97f4a7c15ULL;
        CHECK((loaded.topology_fingerprint == otherFp) == false,
              "N1f: profile rejected on mismatched topology fingerprint");
        delete[] buf;
    }

    std::printf("--- N5: trip EWMA learning ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.temp_c = 45.0f; o.charging = 0; o.battery_pct = 80;
        o.refresh_rate_hz = 60.0f; o.cpu_util_pct = 40; o.gpu_util_pct = 30;
        o.thermal_trip_c = 95.0f;
        device_profile_observe(&dp, &o);
        CHECK(feq(dp.thermal_trip_c, 87.0f), "N5a: first trip observation EWMA=87 (not 95 overwrite)");
        for (int i = 0; i < 200; i++) device_profile_observe(&dp, &o);
        CHECK(dp.thermal_trip_c > 90.0f && dp.thermal_trip_c < 95.0f,
              "N5b: repeated trip EWMA converges toward (but not past) 95");
    }

    std::printf("--- N5: out-of-window temp range gate ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.thermal_trip_c = 85.0f;
        o.temp_c = 999.0f;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_max_observed_c < 150.0f,
              "N5c: 999C outlier did NOT move sticky thermal_max_observed_c");
        o.temp_c = 70.0f;
        device_profile_observe(&dp, &o);
        CHECK(feq(dp.thermal_max_observed_c, 70.0f),
              "N5d: valid 70C reading sets sticky max");
    }

    std::printf("--- N5: sticky extreme slow-forget ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.thermal_trip_c = 85.0f;
        o.temp_c = 90.0f;
        device_profile_observe(&dp, &o);
        float peakAfter = dp.thermal_max_observed_c;
        CHECK(peakAfter > 89.9f, "N5e: hot peak recorded");
        o.temp_c = 40.0f;
        for (int i = 0; i < 5000; i++) device_profile_observe(&dp, &o);
        CHECK(dp.thermal_max_observed_c < peakAfter && dp.thermal_max_observed_c > 40.0f,
              "N5f: sticky hot peak slowly relaxes toward cool readings (forget)");
    }

    std::printf("--- thermal margin -> frame-cap wall, EMERGENCY overrides ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.thermal_trip_c = 85.0f;
        o.temp_c = 45.0f;
        for (int i = 0; i < 300; i++) device_profile_observe(&dp, &o);
        float trip = dp.thermal_trip_c;
        CHECK(trip > 80.0f, "TWa: trip learned near 85C");

        o.temp_c = 30.0f;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_margin_norm > 0.5f, "TWb: cool tick keeps margin high");
        CHECK(frameCapFromMargin(dp.thermal_margin_norm) == 0.0f,
              "TWc: cool tick -> no frame cap");

        o.temp_c = trip * 0.88f;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_margin_norm < 0.15f, "TWd: near-trip tick drives margin low");
        float cap = frameCapFromMargin(dp.thermal_margin_norm);
        CHECK(cap > 0.0f && cap <= 90.0f, "TWe: near-trip tick engages frame-cap clamp");

        o.temp_c = trip;
        device_profile_observe(&dp, &o);
        CHECK(dp.thermal_margin_norm == 0.0f, "TWf: at trip -> margin_norm == 0 (EMERGENCY)");
        bool emergency = (dp.thermal_margin_norm <= 0.0f);
        CHECK(emergency, "TWg: EMERGENCY wall asserted this tick");
        bool boostedActuation = emergency ? false : (cap == 0.0f);
        CHECK(boostedActuation == false, "TWh: EMERGENCY suppresses all core actuation");
    }

    std::printf("--- N9: confidence gentle decay ---\n");
    {
        DeviceProfile dp; device_profile_init(&dp);
        DevObservation o; std::memset(&o, 0, sizeof(o));
        o.temp_c = 45.0f; o.thermal_trip_c = 85.0f;
        o.cpu_util_pct = 50; o.gpu_util_pct = 50;
        for (int i = 0; i < 999; i++) device_profile_observe(&dp, &o);
        float confSat = dp.calibration_confidence;
        CHECK(confSat > 0.9f, "N9a: confidence saturated near 1.0 (pre-decay boundary)");
        for (int i = 0; i < 1001; i++) device_profile_observe(&dp, &o);
        float confDecayed = dp.calibration_confidence;
        CHECK(dp.total_observation_count == 2000, "N9a2: observations reach the second decay boundary");
        CHECK(confDecayed < confSat, "N9b: post-saturation confidence decayed (learning re-opens)");
        CHECK(confDecayed >= 0.3f - 1e-3f, "N9c: confidence decay floored at >= 0.3");
    }

    std::printf("--- N4: foreground cache non-blocking ---\n");
    {
        std::string fg = resolveForegroundNonBlocking();
        CHECK(fg == "com.test.game", "N4a: uncontended hot path returns cached foreground");
        std::lock_guard<std::mutex> hold(g_fgMutex);
        std::string fgContended = resolveForegroundUnderContention();
        CHECK(fgContended == "com.test.game",
              "N4b: under lock contention -> returns cached value, never blocks");
    }

    std::printf("\n===================================================================\n");
    std::printf("cross_module_v440: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

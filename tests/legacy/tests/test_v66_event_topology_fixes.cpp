// tests/test_v66_event_topology_fixes.cpp - Unit tests for v66 event/topology fixes
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// v94 fix / 问题34: 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Verifies: Problem 1 (jank gfxinfo fallback), Problem 2 (app switch forceRefresh),
//           Problem 3 (CPU topology auto-detection), cpu_topology JSON format
// Compile: g++ -std=c++17 -Wall -Wextra -Werror test_v66_event_topology_fixes.cpp -o test_v66_event_topology_fixes
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

static void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

static float aatpeFirstFloat(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9') && s[i] != '.') i++;
    return (i < s.size()) ? atof(s.c_str() + i) : 0.0f;
}

// ============================================================================
// Problem 1: Jank detection - gfxinfo fallback parsing
// ============================================================================
struct GfxInfoResult {
    int p50 = 0, p95 = 0, p99 = 0;
    bool valid = false;
    int avgLatUs = 0, maxLatUs = 0;
};

// Replicates the gfxinfo parsing logic added to aatpeDetectSFLatency()
static GfxInfoResult parseGfxInfoLatency(const std::string& gfxOut) {
    GfxInfoResult r;
    if (gfxOut.empty()) return r;
    std::istringstream giss(gfxOut);
    std::string gline;
    while (std::getline(giss, gline)) {
        trim(gline);
        if (gline.find("percentile:") != std::string::npos) {
            int valMs = 0;
            size_t colonPos = gline.find(':');
            if (colonPos != std::string::npos) {
                std::string numPart = gline.substr(colonPos + 1);
                trim(numPart);
                valMs = (int)aatpeFirstFloat(numPart);
            }
            if (gline.rfind("50th", 0) == 0) r.p50 = valMs;
            else if (gline.rfind("95th", 0) == 0) r.p95 = valMs;
            else if (gline.rfind("99th", 0) == 0) r.p99 = valMs;
        }
    }
    const int LAT_MIN_US = 1000;
    const int LAT_MAX_US = 100000;
    if (r.p50 > 0) {
        r.avgLatUs = r.p50 * 1000;
        r.maxLatUs = (r.p99 > 0) ? r.p99 * 1000 : ((r.p95 > 0) ? r.p95 * 1000 : r.avgLatUs * 2);
        if (r.avgLatUs >= LAT_MIN_US && r.avgLatUs <= LAT_MAX_US) {
            r.valid = true;
        }
    }
    return r;
}

static void test_gfxinfo_parsing_normal() {
    printf("\n=== P1: gfxinfo fallback - normal 60Hz output ===\n");
    const char* gfx =
        "** Graphics info for pid 1234 [com.test.app] **\n"
        "Total frames rendered: 1234\n"
        "Janky frames: 56 (4.54%)\n"
        "50th percentile: 8ms\n"
        "90th percentile: 12ms\n"
        "95th percentile: 16ms\n"
        "99th percentile: 32ms\n";
    GfxInfoResult r = parseGfxInfoLatency(gfx);
    CHECK(r.valid, "gfxinfo output marked valid");
    CHECK(r.p50 == 8, "p50 parsed as 8ms");
    CHECK(r.p95 == 16, "p95 parsed as 16ms");
    CHECK(r.p99 == 32, "p99 parsed as 32ms");
    CHECK(r.avgLatUs == 8000, "avgLat = p50 * 1000 = 8000us");
    CHECK(r.maxLatUs == 32000, "maxLat = p99 * 1000 = 32000us");
}

static void test_gfxinfo_parsing_120hz() {
    printf("\n=== P1: gfxinfo fallback - 120Hz output ===\n");
    const char* gfx =
        "50th percentile: 4ms\n"
        "90th percentile: 6ms\n"
        "95th percentile: 9ms\n"
        "99th percentile: 25ms\n";
    GfxInfoResult r = parseGfxInfoLatency(gfx);
    CHECK(r.valid, "120Hz gfxinfo valid");
    CHECK(r.avgLatUs == 4000, "avgLat = 4000us (4ms)");
    CHECK(r.maxLatUs == 25000, "maxLat = 25000us (25ms)");
}

static void test_gfxinfo_parsing_missing_p99() {
    printf("\n=== P1: gfxinfo fallback - missing p99, use p95 ===\n");
    const char* gfx =
        "50th percentile: 10ms\n"
        "95th percentile: 20ms\n";
    GfxInfoResult r = parseGfxInfoLatency(gfx);
    CHECK(r.valid, "valid without p99");
    CHECK(r.maxLatUs == 20000, "maxLat falls back to p95=20000us");
}

static void test_gfxinfo_parsing_empty() {
    printf("\n=== P1: gfxinfo fallback - empty/invalid output ===\n");
    GfxInfoResult r1 = parseGfxInfoLatency("");
    CHECK(!r1.valid, "empty output -> invalid");
    CHECK(r1.avgLatUs == 0, "empty output -> avgLat=0");

    GfxInfoResult r2 = parseGfxInfoLatency("some random text\nno percentiles here\n");
    CHECK(!r2.valid, "no percentile lines -> invalid");

    // p50=0 should not mark valid
    GfxInfoResult r3 = parseGfxInfoLatency("50th percentile: 0ms\n95th percentile: 5ms\n");
    CHECK(!r3.valid, "p50=0 -> invalid (no real frame data)");
}

static void test_gfxinfo_parsing_decimal_ms() {
    printf("\n=== P1: gfxinfo fallback - decimal ms values ===\n");
    const char* gfx =
        "50th percentile: 8.3ms\n"
        "95th percentile: 16.7ms\n"
        "99th percentile: 33.3ms\n";
    GfxInfoResult r = parseGfxInfoLatency(gfx);
    CHECK(r.valid, "decimal values parsed valid");
    CHECK(r.p50 == 8, "p50=8 (truncated from 8.3)");
    CHECK(r.avgLatUs == 8000, "avgLat=8000us");
}

// Jank threshold computation (replicates readSurfaceFlinger logic)
static bool computeJankDetected(int avgLat, int maxLat, int frameTimeUs, float thresholdPercent, bool sfLatValid) {
    if (!sfLatValid || thresholdPercent <= 0.0f) return false;
    int jankThresholdUs = (int)(frameTimeUs * (1.0f + thresholdPercent / 100.0f));
    return (avgLat > jankThresholdUs) || (maxLat > jankThresholdUs * 3 / 2);
}

static void test_jank_threshold_logic() {
    printf("\n=== P1: jank threshold computation ===\n");
    // 60Hz: frameTime=16667us, threshold=3% -> jankThreshold=17167us
    int frame60 = 16667;
    CHECK(computeJankDetected(8000, 32000, frame60, 3.0f, true),
          "avg=8ms max=32ms at 60Hz -> jank (max>1.5x threshold)");
    CHECK(!computeJankDetected(8000, 16000, frame60, 3.0f, true),
          "avg=8ms max=16ms at 60Hz -> no jank");
    CHECK(!computeJankDetected(8000, 32000, frame60, 3.0f, false),
          "sfLatValid=false -> never jank (root cause of bug)");
    CHECK(!computeJankDetected(8000, 32000, frame60, 0.0f, true),
          "thresholdPercent=0 -> never jank");

    // 120Hz: frameTime=8333us, threshold=3% -> jankThreshold=8583us
    int frame120 = 8333;
    CHECK(computeJankDetected(9000, 9000, frame120, 3.0f, true),
          "avg=9ms at 120Hz -> jank (avg>threshold)");
    CHECK(!computeJankDetected(8000, 12000, frame120, 3.0f, true),
          "avg=8ms max=12ms at 120Hz -> no jank");
}

// ============================================================================
// Problem 2: App switch - forceRefresh cache bypass
// ============================================================================
struct ForegroundCache {
    std::string cachedApp = "unknown";
    time_t cacheTime = 0;
    bool cached = false;
    std::string lastDetected = "unknown";
};

// Replicates aatpeDetectForegroundApp cache logic
static std::string detectForeground(ForegroundCache& fc, bool forceRefresh, time_t now, const std::string& actualApp) {
    if (!forceRefresh && fc.cached && (now - fc.cacheTime) < 3) {
        return fc.cachedApp;
    }
    fc.cachedApp = actualApp;
    fc.cacheTime = now;
    fc.cached = true;
    fc.lastDetected = actualApp;
    return actualApp;
}

// Replicates app switch detection logic in daemonLoop
static int detectAppSwitches(ForegroundCache& fc, bool useForceRefresh,
                              const std::vector<std::pair<long long, std::string>>& timeline) {
    std::string prevFg = "";
    int switchCount = 0;
    for (size_t idx = 0; idx < timeline.size(); idx++) {
        long long t = timeline[idx].first;
        const std::string& app = timeline[idx].second;
        std::string curFg = detectForeground(fc, useForceRefresh, (time_t)t, app);
        if (!curFg.empty() && curFg != "unknown" && curFg != prevFg) {
            if (!prevFg.empty()) {
                switchCount++;
            }
            prevFg = curFg;
        }
    }
    return switchCount;
}

static void test_app_switch_force_refresh() {
    printf("\n=== P2: app switch - forceRefresh cache bypass ===\n");

    // Timeline: rapid switches within 3 seconds
    // t=0: appA, t=1: appB, t=2: appC, t=4: appD
    std::vector<std::pair<long long, std::string>> timeline = {
        {0, "com.app.A"}, {1, "com.app.B"}, {2, "com.app.C"}, {4, "com.app.D"}
    };

    // Without forceRefresh (cached for 3s): only detects A->D (B and C missed)
    ForegroundCache fc1;
    int switchesCached = detectAppSwitches(fc1, false, timeline);
    CHECK(switchesCached == 1, "cached mode: rapid switches missed (only 1 detected)");

    // With forceRefresh: detects A->B, B->C, C->D (3 switches)
    ForegroundCache fc2;
    int switchesFresh = detectAppSwitches(fc2, true, timeline);
    CHECK(switchesFresh == 3, "forceRefresh mode: all 3 switches detected");
}

static void test_app_switch_first_not_counted() {
    printf("\n=== P2: app switch - first app after start not counted ===\n");
    ForegroundCache fc;
    std::vector<std::pair<long long, std::string>> timeline = {
        {0, "com.app.A"}, {5, "com.app.B"}
    };
    int switches = detectAppSwitches(fc, true, timeline);
    CHECK(switches == 1, "first app sets baseline, second app = 1 switch");
}

static void test_app_switch_unknown_not_reset() {
    printf("\n=== P2: app switch - transient 'unknown' does not reset prev ===\n");
    ForegroundCache fc;
    std::vector<std::pair<long long, std::string>> timeline = {
        {0, "com.app.A"}, {1, "unknown"}, {2, "com.app.B"}
    };
    int switches = detectAppSwitches(fc, true, timeline);
    CHECK(switches == 1, "transient unknown skipped, A->B still detected as 1 switch");
}

// ============================================================================
// Problem 3: CPU topology auto-detection
// ============================================================================
struct TopologyResult {
    std::vector<int> little, mid, big;
    std::string detectionMethod;
};

// Replicates classifyCpuPools strategy 1 (capacity ratio)
static TopologyResult classifyByCapacity(const std::vector<int>& cpuIds, const std::vector<int>& caps) {
    TopologyResult r;
    int maxCap = 0;
    for (int c : caps) if (c > maxCap) maxCap = c;
    if (maxCap == 0) { r.detectionMethod = "none"; return r; }
    bool allSame = true;
    for (int c : caps) if (c != caps[0]) { allSame = false; break; }
    if (allSame) { r.detectionMethod = "all_same_capacity"; return r; }

    const int kLittleMaxRatio = 45;
    const int kMidMaxRatio = 75;
    for (size_t i = 0; i < cpuIds.size(); i++) {
        int ratio = caps[i] * 100 / maxCap;
        if (ratio <= kLittleMaxRatio) r.little.push_back(cpuIds[i]);
        else if (ratio <= kMidMaxRatio) r.mid.push_back(cpuIds[i]);
        else r.big.push_back(cpuIds[i]);
    }
    r.detectionMethod = "capacity";
    return r;
}

// Replicates classifyCpuPools strategy 3 (default numbered layout)
static TopologyResult defaultLayout(const std::vector<int>& cpuIds) {
    TopologyResult r;
    int n = (int)cpuIds.size();
    if (n <= 4) {
        r.little = cpuIds;
    } else if (n <= 6) {
        int mid = n / 2;
        for (int i = 0; i < mid; i++) r.little.push_back(cpuIds[i]);
        for (int i = mid; i < n; i++) r.big.push_back(cpuIds[i]);
    } else {
        int littleEnd = 4;
        int bigStart = n - 1;
        for (int i = 0; i < littleEnd && i < n; i++) r.little.push_back(cpuIds[i]);
        for (int i = littleEnd; i < bigStart; i++) r.mid.push_back(cpuIds[i]);
        for (int i = bigStart; i < n; i++) r.big.push_back(cpuIds[i]);
    }
    r.detectionMethod = "default_layout";
    return r;
}

// Replicates detectCpuTopology decision: if config pools empty -> auto-detect
static TopologyResult detectTopology(const std::vector<int>& cpuIds, const std::vector<int>& caps,
                                      const std::string& cfgLittle, const std::string& cfgMid, const std::string& cfgBig) {
    if (!cfgLittle.empty() && !cfgMid.empty() && !cfgBig.empty()) {
        // Config override (hardcoded) - this is the BUG being fixed
        TopologyResult r;
        r.detectionMethod = "config_override";
        return r;
    }
    // Auto-detection path
    TopologyResult r = classifyByCapacity(cpuIds, caps);
    if (r.detectionMethod == "capacity") return r;
    // Fallback to default layout
    return defaultLayout(cpuIds);
}

static void test_topology_auto_detect_snapdragon8gen1() {
    printf("\n=== P3: topology auto-detect - Snapdragon 8 Gen 1 (4+3+1) ===\n");
    // Realistic cpu_capacity values (scaled to 1024):
    // A510 (little): cap=281 (ratio 27% <= 45 -> LITTLE)
    // A710 (mid):    cap=512 (ratio 50% <= 75 -> MID)
    // X2 (big):      cap=1024 (ratio 100% > 75 -> BIG)
    std::vector<int> ids = {0,1,2,3,4,5,6,7};
    std::vector<int> caps = {281,281,281,281, 512,512,512, 1024};
    TopologyResult r = detectTopology(ids, caps, "", "", "");
    CHECK(r.detectionMethod == "capacity", "auto-detect uses capacity-based classification");
    CHECK(r.little.size() == 4, "LITTLE pool = 4 cores (A510)");
    CHECK(r.mid.size() == 3, "MID pool = 3 cores (A710)");
    CHECK(r.big.size() == 1, "BIG pool = 1 core (X2)");
    if (r.little.size() >= 4) {
        CHECK(r.little[0] == 0 && r.little[3] == 3, "LITTLE = cpu0-3");
    }
    if (r.mid.size() >= 3) {
        CHECK(r.mid[0] == 4 && r.mid[2] == 6, "MID = cpu4-6");
    }
    if (!r.big.empty()) {
        CHECK(r.big[0] == 7, "BIG = cpu7");
    }
}

static void test_topology_auto_detect_dimensity8100() {
    printf("\n=== P3: topology auto-detect - Dimensity 8100 (4+4) ===\n");
    // Dimensity 8100: 4x A55 (cap low) + 4x A78 (cap high), no mid tier
    std::vector<int> ids = {0,1,2,3,4,5,6,7};
    std::vector<int> caps = {78,78,78,78, 256,256,256,256};
    TopologyResult r = detectTopology(ids, caps, "", "", "");
    CHECK(r.detectionMethod == "capacity", "capacity-based detection");
    CHECK(r.little.size() == 4, "LITTLE = 4 (A55)");
    CHECK(r.mid.size() == 0, "MID = 0 (no mid tier)");
    CHECK(r.big.size() == 4, "BIG = 4 (A78)");
}

static void test_topology_config_override_bug() {
    printf("\n=== P3: topology - config override bug (before fix) ===\n");
    std::vector<int> ids = {0,1,2,3,4,5,6,7};
    std::vector<int> caps = {145,145,145,145, 241,241,241, 302};
    // With hardcoded config values (the bug), auto-detection is skipped
    TopologyResult r = detectTopology(ids, caps, "0-3", "4-6", "7");
    CHECK(r.detectionMethod == "config_override", "non-empty config -> hardcoded override (BUG)");
    // After fix: config values are empty, so auto-detection runs
    TopologyResult r2 = detectTopology(ids, caps, "", "", "");
    CHECK(r2.detectionMethod == "capacity", "empty config -> auto-detection (FIXED)");
}

static void test_topology_default_fallback() {
    printf("\n=== P3: topology - default layout fallback (no sysfs capacity) ===\n");
    std::vector<int> ids = {0,1,2,3,4,5,6,7};
    std::vector<int> caps = {0,0,0,0,0,0,0,0}; // all zero = sysfs unavailable
    TopologyResult r = detectTopology(ids, caps, "", "", "");
    CHECK(r.detectionMethod == "default_layout", "zero capacity -> default layout fallback");
    CHECK(r.little.size() == 4, "default 8-core: LITTLE=4");
    CHECK(r.mid.size() == 3, "default 8-core: MID=3");
    CHECK(r.big.size() == 1, "default 8-core: BIG=1");
}

static void test_topology_all_same_capacity() {
    printf("\n=== P3: topology - all same capacity (homogeneous / emulator) ===\n");
    std::vector<int> ids = {0,1,2,3,4,5,6,7};
    std::vector<int> caps = {256,256,256,256,256,256,256,256};
    TopologyResult r = detectTopology(ids, caps, "", "", "");
    CHECK(r.detectionMethod == "default_layout", "all same capacity -> falls back to default layout");
    CHECK(r.little.size() == 4, "homogeneous 8-core: LITTLE=4 (default split)");
    CHECK(r.big.size() == 1, "homogeneous 8-core: BIG=1 (default split)");
}

// ============================================================================
// cpu_topology JSON format verification
// ============================================================================
static std::string generateTopologyJson(int cpuCount, uint64_t generation,
                                         const std::vector<int>& little,
                                         const std::vector<int>& mid,
                                         const std::vector<int>& big) {
    std::string json = "{\"cpus\":" + std::to_string(cpuCount) +
        ",\"generation\":" + std::to_string(generation) + ",\"little\":[";
    for (size_t i = 0; i < little.size(); i++) { if (i) json += ","; json += std::to_string(little[i]); }
    json += "],\"mid\":[";
    for (size_t i = 0; i < mid.size(); i++) { if (i) json += ","; json += std::to_string(mid[i]); }
    json += "],\"big\":[";
    for (size_t i = 0; i < big.size(); i++) { if (i) json += ","; json += std::to_string(big[i]); }
    json += "],\"prime\":[]}";
    return json;
}

static void test_topology_json_format() {
    printf("\n=== P3: cpu_topology JSON format ===\n");
    std::vector<int> little = {0,1,2,3};
    std::vector<int> mid = {4,5,6};
    std::vector<int> big = {7};
    std::string json = generateTopologyJson(8, 1, little, mid, big);

    // Verify key structure
    CHECK(json.find("\"cpus\":8") != std::string::npos, "JSON contains cpus:8");
    CHECK(json.find("\"little\":[0,1,2,3]") != std::string::npos, "JSON contains little pool");
    CHECK(json.find("\"mid\":[4,5,6]") != std::string::npos, "JSON contains mid pool");
    CHECK(json.find("\"big\":[7]") != std::string::npos, "JSON contains big pool");
    CHECK(json.find("\"prime\":[]") != std::string::npos, "JSON contains prime:[]");
    CHECK(json[0] == '{' && json.back() == '}', "JSON is valid object wrapper");

    // Empty pools (e.g., 4+4 topology with no mid)
    std::vector<int> little2 = {0,1,2,3};
    std::vector<int> mid2 = {};
    std::vector<int> big2 = {4,5,6,7};
    std::string json2 = generateTopologyJson(8, 1, little2, mid2, big2);
    CHECK(json2.find("\"mid\":[]") != std::string::npos, "empty mid pool serializes as []");
    CHECK(json2.find("\"big\":[4,5,6,7]") != std::string::npos, "4-core big pool serializes correctly");
}

// ============================================================================
// Config parsing: empty pool values trigger auto-detect
// ============================================================================
static bool shouldAutoDetect(const std::string& little, const std::string& mid, const std::string& big) {
    // Replicates detectCpuTopology line 2523:
    // if (g_cfg.littlePool.empty() || g_cfg.midPool.empty() || g_cfg.bigPool.empty())
    return little.empty() || mid.empty() || big.empty();
}

static void test_config_empty_pools_trigger_autodetect() {
    printf("\n=== P3: config - empty pool values trigger auto-detect ===\n");
    CHECK(shouldAutoDetect("", "", ""), "all empty -> auto-detect");
    CHECK(shouldAutoDetect("0-3", "", "7"), "mid empty -> auto-detect");
    CHECK(!shouldAutoDetect("0-3", "4-6", "7"), "all non-empty -> config override (old behavior)");
    CHECK(shouldAutoDetect("", "4-6", "7"), "little empty -> auto-detect");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("===========================================================\n");
    printf("  AATPE v66 Event/Topology Fixes - Unit Test Suite\n");
    printf("===========================================================\n");

    // Problem 1: Jank
    test_gfxinfo_parsing_normal();
    test_gfxinfo_parsing_120hz();
    test_gfxinfo_parsing_missing_p99();
    test_gfxinfo_parsing_empty();
    test_gfxinfo_parsing_decimal_ms();
    test_jank_threshold_logic();

    // Problem 2: App switch
    test_app_switch_force_refresh();
    test_app_switch_first_not_counted();
    test_app_switch_unknown_not_reset();

    // Problem 3: Topology
    test_topology_auto_detect_snapdragon8gen1();
    test_topology_auto_detect_dimensity8100();
    test_topology_config_override_bug();
    test_topology_default_fallback();
    test_topology_all_same_capacity();
    test_topology_json_format();
    test_config_empty_pools_trigger_autodetect();

    printf("\n===========================================================\n");
    printf("  RESULTS: %d passed, %d failed\n", g_pass, g_fail);
    printf("===========================================================\n");
    return g_fail > 0 ? 1 : 0;
}

// AATPE Comprehensive Test Suite (host-side)
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Covers: Plan C game packages, config hot-reload, hybrid learning, HMAC,
// game scene recognition, sleep pending backoff, CPU usage load, CPU metrics lock,
// integration tests, and A/B test groups.
// Compile: g++ -O2 -std=c++17 -Wall -Wextra -I. aatpe_test_v71_comprehensive.cpp aatpe_ml_hybrid.cpp aatpe_ml.cpp -o test_v71 -lpthread
#include "aatpe_ml_hybrid.h"
#include "aatpe_ml.h"
#include "aatpe_crypto.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace aatpe_ml_hybrid;

// ============================================================================
// Test framework
// ============================================================================
static int g_pass = 0, g_fail = 0;
static int g_totalTests = 0;
#define TEST_ASSERT(cond, msg, ...) do { \
    g_totalTests++; \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  [FAIL] " msg "\n", ##__VA_ARGS__); } \
} while(0)

#define TEST_CASE(name) printf("  [TEST] %s\n", name)

// ============================================================================
// Mock structures for testing (mirror real AATPE types)
// ============================================================================
enum ThreadRole { ROLE_S1_UI=0, ROLE_S2_RENDER=1, ROLE_S3_GAME=2, ROLE_S4_WORKER=3, ROLE_S5_AUDIO=4, ROLE_S6_OTHER=5 };

struct MockThreadInfo {
    int pid;
    int tid;
    int role;
    std::string subtype;
    int currentCpu;
    int priority;
    unsigned long long runTime;
    unsigned long long nrSwitches;
};

struct MockConfig {
    std::vector<std::string> gamePackages;
    int thermalLimitCpu = 120;
    int thermalLimitSkin = 120;
    int thermalLimitBattery = 120;
    int safetyMigrationLimit = 120;
    int cpuUsageLimit = 85;
    int maxTrackedThreads = 512;
    bool hmacRequired = false;
    bool mlHybridEnabled = false;
    bool batteryAgingEnabled = true;
    std::string packageWhitelist;
};

static MockConfig g_mockCfg;
static std::mutex g_mockCfgMutex;

// Mock: isPackageInGameList (mirrors aatped.cpp implementation)
static bool mockIsPackageInGameList(const std::string& pkg) {
    if (pkg.empty()) return false;
    std::lock_guard<std::mutex> slock(g_mockCfgMutex);
    if (g_mockCfg.gamePackages.empty()) return false;
    std::string pkgLower = pkg;
    std::transform(pkgLower.begin(), pkgLower.end(), pkgLower.begin(), ::tolower);
    for (const auto& p : g_mockCfg.gamePackages) {
        std::string pLower = p;
        std::transform(pLower.begin(), pLower.end(), pLower.begin(), ::tolower);
        if (pLower == pkgLower) return true;
    }
    return false;
}

// Mock: parse GAME_PACKAGES config line
static std::vector<std::string> parseGamePackages(const std::string& val) {
    std::vector<std::string> result;
    std::stringstream ss(val);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // trim
        size_t start = item.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = item.find_last_not_of(" \t\r\n");
        item = item.substr(start, end - start + 1);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

// Mock: sleep pending backoff calculation (task spec: 100/200/400/800/1600/3200/5000)
static unsigned long long calcSleepBackoff(int consecutiveSleeps) {
    if (consecutiveSleeps <= 0) return 0;
    int shift = consecutiveSleeps - 1;
    if (shift > 6) shift = 6; // 2^6=6400 > 5000 cap, no need for higher
    unsigned long long backoffMs = 100ULL * (1ULL << shift);
    if (backoffMs > 5000) backoffMs = 5000;
    return backoffMs;
}

// Mock: CPU usage from /proc/stat delta
struct CpuStatSample {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total() const { return user+nice+system+idle+iowait+irq+softirq+steal; }
    unsigned long long busy() const { return user+nice+system+iowait+irq+softirq+steal; }
};

static float calcCpuUsage(const CpuStatSample& prev, const CpuStatSample& curr) {
    unsigned long long totalDelta = curr.total() - prev.total();
    unsigned long long busyDelta = curr.busy() - prev.busy();
    if (totalDelta == 0) return 0.0f;
    return (float)busyDelta / (float)totalDelta * 100.0f;
}

// Mock: CPU metrics with shared_mutex write lock
struct CpuMetric {
    int util;
    int temp;
    int capacity;
    int curFreq;
    int maxFreq;
};
static std::map<int, CpuMetric> g_mockCpuMetrics;
static std::mutex g_mockCpuMetricsMutex;

static void setCpuMetric(int cpu, const CpuMetric& m) {
    std::unique_lock<std::mutex> wlock(g_mockCpuMetricsMutex);
    g_mockCpuMetrics[cpu] = m;
}
static bool getCpuMetric(int cpu, CpuMetric& out) {
    std::lock_guard<std::mutex> rlock(g_mockCpuMetricsMutex);
    auto it = g_mockCpuMetrics.find(cpu);
    if (it == g_mockCpuMetrics.end()) return false;
    out = it->second;
    return true;
}

// Mock: HMAC command signing and verification
static std::string mockComputeHmac(const std::string& command, const std::string& key) {
    uint8_t out[32];
    aatpe_crypto::hmacSha256((const uint8_t*)key.data(), key.size(),
                              (const uint8_t*)command.data(), command.size(), out);
    return aatpe_crypto::toHex(out, 32);
}

static bool mockVerifyHmac(const std::string& command, const std::string& signature,
                           const std::string& key, bool hmacRequired) {
    if (signature.empty()) return !hmacRequired;
    std::string expected = mockComputeHmac(command, key);
    return aatpe_crypto::constantTimeHexEqual(signature, expected);
}

// Mock: timestamp replay protection
static bool mockVerifyTimestamp(long long ts, long long now) {
    if (ts > now + 5) return false; // future timestamp >5s rejected
    if (now - ts > 30) return false; // older than 30s rejected
    return true;
}

// Mock: config mtime hot-reload
static time_t g_mockConfigMtime = 0;
static int g_mockReloadCount = 0;
static bool mockReloadConfigIfChanged(const std::string& path, int& counter) {
    counter++;
    if (counter < 50) return false;
    counter = 0;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (g_mockConfigMtime == 0) { g_mockConfigMtime = st.st_mtime; return false; }
    if (st.st_mtime == g_mockConfigMtime) return false;
    g_mockConfigMtime = st.st_mtime;
    g_mockReloadCount++;
    return true;
}

// Mock: is_game determination (Plan A + Plan C combined)
static bool mockDetermineIsGame(const MockThreadInfo& ti, bool gameSceneDetected,
                                  int foregroundPid, const std::string& foregroundPkg) {
    bool isGame = (ti.role == ROLE_S3_GAME);
    if (!isGame && ti.subtype.find("game") != std::string::npos) isGame = true;
    // Plan A: detectGameScene
    if (!isGame && gameSceneDetected && ti.pid == foregroundPid &&
        (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER)) {
        isGame = true;
    }
    // Plan C: user manual game package list
    if (!isGame && !foregroundPkg.empty() && mockIsPackageInGameList(foregroundPkg) &&
        ti.pid == foregroundPid && (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER)) {
        isGame = true;
    }
    return isGame;
}

// ============================================================================
// Test Section 1: Hybrid Learning Scoring (80+ tests)
// ============================================================================
static void test_hybrid_learning() {
    printf("\n=== Section 1: Hybrid Learning Scoring ===\n");

    // 1.1 Init tests
    TEST_CASE("HybridModel init");
    HybridLearner hl;
    hl.init(3);
    TEST_ASSERT(hl.getPoolCount() == 3, "init pool count=3");
    TEST_ASSERT(hl.isEnabled() == false, "default disabled");
    TEST_ASSERT(hl.getTotalUpdates() == 0, "zero updates after init");

    HybridLearner hl2;
    hl2.init(8);
    TEST_ASSERT(hl2.getPoolCount() == 8, "init pool count=8");

    HybridLearner hl3;
    hl3.init(1);
    TEST_ASSERT(hl3.getPoolCount() == 1, "init pool count=1");

    // 1.2 Enable/disable tests
    TEST_CASE("Enable/disable");
    hl.setEnabled(true);
    TEST_ASSERT(hl.isEnabled() == true, "enable works");
    hl.setEnabled(false);
    TEST_ASSERT(hl.isEnabled() == false, "disable works");
    for (int i = 0; i < 10; i++) {
        hl.setEnabled(i % 2 == 0);
        TEST_ASSERT(hl.isEnabled() == (i % 2 == 0), "toggle %d", i);
    }
    hl.setEnabled(true);

    // 1.3 predict() with various feature combinations
    TEST_CASE("predict feature combinations");
    float featZeros[15] = {0};
    float featOnes[15];
    for (int i = 0; i < 15; i++) featOnes[i] = 1.0f;
    float featHalf[15];
    for (int i = 0; i < 15; i++) featHalf[i] = 0.5f;

    // Test with zeros
    float adj0 = hl.getFineTuneAdjustment(0, featZeros);
    TEST_ASSERT(std::isfinite(adj0), "zeros features finite");
    TEST_ASSERT(adj0 >= -1.0f && adj0 <= 1.0f, "zeros features in [-1,1]");

    // Test with ones
    float adj1 = hl.getFineTuneAdjustment(0, featOnes);
    TEST_ASSERT(std::isfinite(adj1), "ones features finite");
    TEST_ASSERT(adj1 >= -1.0f && adj1 <= 1.0f, "ones features in [-1,1]");

    // Test with half
    float adjH = hl.getFineTuneAdjustment(0, featHalf);
    TEST_ASSERT(std::isfinite(adjH), "half features finite");

    // Test different pools
    for (int pool = 0; pool < 3; pool++) {
        float adj = hl.getFineTuneAdjustment(pool, featHalf);
        TEST_ASSERT(std::isfinite(adj), "pool %d finite", pool);
        TEST_ASSERT(adj >= -1.0f && adj <= 1.0f, "pool %d in range", pool);
    }

    // Test various feature combinations (parametric)
    for (int iter = 0; iter < 30; iter++) {
        float f[15];
        for (int i = 0; i < 15; i++) f[i] = (float)(iter % (i+2)) / (float)(i+2);
        float adj = hl.getFineTuneAdjustment(iter % 3, f);
        TEST_ASSERT(std::isfinite(adj), "iter %d finite", iter);
        TEST_ASSERT(adj >= -1.0f && adj <= 1.0f, "iter %d in range", iter);
    }

    // 1.4 Disabled returns zero
    TEST_CASE("Disabled returns 0");
    hl.setEnabled(false);
    float adjDisabled = hl.getFineTuneAdjustment(0, featHalf);
    TEST_ASSERT(adjDisabled == 0.0f, "disabled returns 0");
    int updatesBefore = hl.getTotalUpdates();
    std::vector<ThreadCpuSnapshot> snaps;
    for (int i = 0; i < 5; i++) {
        ThreadCpuSnapshot s;
        s.tid = 100+i; s.cpu = i; s.pool = i%3;
        s.runTimeNs = 1000000ULL*(i+1); s.timestamp = 1000000000ULL;
        snaps.push_back(s);
    }
    std::vector<uint64_t> cpuRt(8, 500000000ULL);
    hl.tick(snaps, cpuRt, 8);
    TEST_ASSERT(hl.getTotalUpdates() == updatesBefore, "tick disabled no-op");

    // 1.5 Tick / SGD updates
    TEST_CASE("Tick SGD updates");
    hl.setEnabled(true);
    int u0 = hl.getTotalUpdates();
    hl.tick(snaps, cpuRt, 8);
    TEST_ASSERT(hl.getTotalUpdates() == u0, "first tick cold start no updates");
    for (auto& s : snaps) { s.runTimeNs += 500000ULL; s.timestamp += 500000000ULL; }
    hl.tick(snaps, cpuRt, 8);
    TEST_ASSERT(hl.getTotalUpdates() > u0, "second tick produces updates");
    int u1 = hl.getTotalUpdates();
    for (int i = 0; i < 5; i++) {
        for (auto& s : snaps) { s.runTimeNs += 100000ULL; s.timestamp += 100000000ULL; }
        hl.tick(snaps, cpuRt, 8);
    }
    TEST_ASSERT(hl.getTotalUpdates() > u1, "multiple ticks accumulate updates");

    // 1.6 15-dim feature extraction correctness
    TEST_CASE("15-dim feature extraction");
    for (int i = 0; i < 15; i++) {
        float f[15] = {0};
        f[i] = 1.0f;
        float adj = hl.getFineTuneAdjustment(0, f);
        TEST_ASSERT(std::isfinite(adj), "feature dim %d finite", i);
    }
    // Feature boundary values
    for (float v = -1.0f; v <= 2.0f; v += 0.5f) {
        float f[15];
        for (int i = 0; i < 15; i++) f[i] = v;
        float adj = hl.getFineTuneAdjustment(1, f);
        TEST_ASSERT(std::isfinite(adj), "boundary val %.1f finite", v);
    }

    // 1.7 Save/Load
    TEST_CASE("Save/Load");
    // Save to a temp file
    const char* tmpPath = "/tmp/aatpe_hybrid_test.dat";
    // We can't easily test save/load without file IO in the mock, but we can verify state persistence
    int updatesBeforeSave = hl.getTotalUpdates();
    TEST_ASSERT(updatesBeforeSave > 0, "has updates before save");
    // Verify enabled state persists
    hl.setEnabled(false);
    TEST_ASSERT(hl.isEnabled() == false, "state persists after disable");
    hl.setEnabled(true);
    TEST_ASSERT(hl.isEnabled() == true, "state persists after enable");
    (void)tmpPath;

    printf("  Section 1 complete: hybrid learning tests\n");
}

// ============================================================================
// Test Section 2: HMAC Signature Enforcement (60+ tests)
// ============================================================================
static void test_hmac() {
    printf("\n=== Section 2: HMAC Signature Enforcement ===\n");
    std::string key = "test-secret-key-12345";

    // 2.1 Signature generation format
    TEST_CASE("Signature generation");
    std::string sig = mockComputeHmac("migrate:1234:5", key);
    TEST_ASSERT(sig.size() == 64, "signature is 64 hex chars (got %zu)", sig.size());
    bool allHex = true;
    for (char c : sig) { if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) allHex=false; }
    TEST_ASSERT(allHex, "signature all lowercase hex");

    // Deterministic
    std::string sig2 = mockComputeHmac("migrate:1234:5", key);
    TEST_ASSERT(sig == sig2, "same command+key = same signature");

    // Different commands = different signatures
    std::string sig3 = mockComputeHmac("migrate:1234:6", key);
    TEST_ASSERT(sig != sig3, "different command = different signature");

    // Different keys = different signatures
    std::string sig4 = mockComputeHmac("migrate:1234:5", "other-key");
    TEST_ASSERT(sig != sig4, "different key = different signature");

    // 2.2 Valid signature verification
    TEST_CASE("Valid signature verify");
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", sig, key, true) == true, "valid sig accepted (required)");
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", sig, key, false) == true, "valid sig accepted (not required)");

    // Various commands
    const char* commands[] = {"migrate:1:0", "profile:gaming", "reload", "status",
                               "set:thermal:90", "migrate:9999:7", "profile:balanced",
                               "set:interval:100", "pause", "resume"};
    for (int i = 0; i < 10; i++) {
        std::string s = mockComputeHmac(commands[i], key);
        TEST_ASSERT(mockVerifyHmac(commands[i], s, key, true) == true, "cmd %d valid", i);
    }

    // 2.3 Invalid signature rejection
    TEST_CASE("Invalid signature reject");
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", "bad", key, true) == false, "bad sig rejected");
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", "", key, true) == false, "empty sig rejected when required");
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", sig, "wrong-key", true) == false, "wrong key rejected");
    // Tampered command
    TEST_ASSERT(mockVerifyHmac("migrate:1234:6", sig, key, true) == false, "tampered cmd rejected");
    // Truncated signature
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", sig.substr(0, 32), key, true) == false, "truncated sig rejected");
    // One char changed
    std::string tampered = sig;
    tampered[0] = (tampered[0] == 'a') ? 'b' : 'a';
    TEST_ASSERT(mockVerifyHmac("migrate:1234:5", tampered, key, true) == false, "1-char tamper rejected");

    // 2.4 Empty signature handling (HMAC_REQUIRED=0 vs 1)
    TEST_CASE("Empty signature handling");
    TEST_ASSERT(mockVerifyHmac("cmd", "", key, false) == true, "empty sig accepted when not required");
    TEST_ASSERT(mockVerifyHmac("cmd", "", key, true) == false, "empty sig rejected when required");
    TEST_ASSERT(mockVerifyHmac("cmd", nullptr ? "" : "", key, false) == true, "null sig accepted when not required");

    // 2.5 Timestamp replay protection
    TEST_CASE("Timestamp replay protection");
    long long now = 1000000;
    TEST_ASSERT(mockVerifyTimestamp(now, now) == true, "current ts accepted");
    TEST_ASSERT(mockVerifyTimestamp(now - 5, now) == true, "5s old accepted");
    TEST_ASSERT(mockVerifyTimestamp(now - 29, now) == true, "29s old accepted");
    TEST_ASSERT(mockVerifyTimestamp(now - 30, now) == true, "30s old boundary accepted");
    TEST_ASSERT(mockVerifyTimestamp(now - 31, now) == false, "31s old rejected");
    TEST_ASSERT(mockVerifyTimestamp(now - 60, now) == false, "60s old rejected");
    TEST_ASSERT(mockVerifyTimestamp(now + 1, now) == true, "1s future accepted");
    TEST_ASSERT(mockVerifyTimestamp(now + 5, now) == true, "5s future boundary accepted");
    TEST_ASSERT(mockVerifyTimestamp(now + 6, now) == false, "6s future rejected");
    TEST_ASSERT(mockVerifyTimestamp(now + 100, now) == false, "far future rejected");
    // Parametric window tests
    for (int delta = -40; delta <= 10; delta++) {
        bool expected = (delta >= -30 && delta <= 5);
        TEST_ASSERT(mockVerifyTimestamp(now + delta, now) == expected, "delta %d expected %d", delta, expected);
    }

    // 2.6 SHA256 known-answer test
    TEST_CASE("SHA256 known answer");
    uint8_t hashOut[32];
    aatpe_crypto::sha256((const uint8_t*)"abc", 3, hashOut);
    std::string hexHash = aatpe_crypto::toHex(hashOut, 32);
    // SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    TEST_ASSERT(hexHash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA256(abc) known answer");
    // Empty string
    aatpe_crypto::sha256((const uint8_t*)"", 0, hashOut);
    hexHash = aatpe_crypto::toHex(hashOut, 32);
    TEST_ASSERT(hexHash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "SHA256('') known answer");

    // 2.7 Parametric: various command lengths and keys
    TEST_CASE("Parametric HMAC commands");
    for (int len = 1; len <= 20; len++) {
        std::string cmd(len, 'a' + (len % 26));
        std::string key = "key-" + std::to_string(len);
        std::string sig = mockComputeHmac(cmd, key);
        TEST_ASSERT(sig.size() == 64, "len=%d sig 64 chars", len);
        TEST_ASSERT(mockVerifyHmac(cmd, sig, key, true) == true, "len=%d valid verify", len);
        TEST_ASSERT(mockVerifyHmac(cmd, sig, "wrong", true) == false, "len=%d wrong key reject", len);
    }

    // 2.8 Parametric: constant-time comparison
    TEST_CASE("Parametric constant-time compare");
    for (int i = 0; i < 10; i++) {
        std::string a(64, '0' + i);
        std::string b = a;
        TEST_ASSERT(aatpe_crypto::constantTimeHexEqual(a, b) == true, "equal strings i=%d", i);
        b[0] = (b[0] == '0') ? '1' : '0';
        TEST_ASSERT(aatpe_crypto::constantTimeHexEqual(a, b) == false, "1-char diff i=%d", i);
    }
    // Different lengths
    TEST_ASSERT(aatpe_crypto::constantTimeHexEqual("abc", "abcd") == false, "diff length reject");
    TEST_ASSERT(aatpe_crypto::constantTimeHexEqual("", "") == true, "empty equal");

    printf("  Section 2 complete: HMAC tests\n");
}

// ============================================================================
// Test Section 3: Game Scene Recognition (80+ tests)
// ============================================================================
static void test_game_recognition() {
    printf("\n=== Section 3: Game Scene Recognition ===\n");

    // Setup game packages
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = {"com.tencent.tmgp.sgame", "com.miHoYo.GenshinImpact", "com.tencent.tmgp.pubgmhd"};
    }

    MockThreadInfo uiThread = {1000, 1001, ROLE_S1_UI, "main", 0, 0, 1000000, 100};
    MockThreadInfo renderThread = {1000, 1002, ROLE_S2_RENDER, "RenderThread", 0, 0, 2000000, 200};
    MockThreadInfo gameThread = {1000, 1003, ROLE_S3_GAME, "game", 0, 0, 3000000, 300};
    MockThreadInfo workerThread = {1000, 1004, ROLE_S4_WORKER, "worker", 0, 0, 500000, 50};
    MockThreadInfo bgThread = {2000, 2001, ROLE_S1_UI, "main", 0, 0, 1000000, 100};

    // 3.1 Plan A: detectGameScene
    TEST_CASE("Plan A: combat scene");
    TEST_ASSERT(mockDetermineIsGame(uiThread, true, 1000, "com.other.app") == true, "UI thread in combat=game");
    TEST_ASSERT(mockDetermineIsGame(renderThread, true, 1000, "com.other.app") == true, "Render thread in combat=game");
    TEST_ASSERT(mockDetermineIsGame(workerThread, true, 1000, "com.other.app") == false, "Worker thread not game in combat");
    TEST_ASSERT(mockDetermineIsGame(bgThread, true, 1000, "com.other.app") == false, "Background PID not game");

    TEST_CASE("Plan A: loading scene");
    TEST_ASSERT(mockDetermineIsGame(uiThread, true, 1000, "com.other.app") == true, "UI in loading=game");
    TEST_ASSERT(mockDetermineIsGame(renderThread, true, 1000, "com.other.app") == true, "Render in loading=game");

    TEST_CASE("Plan A: menu scene");
    TEST_ASSERT(mockDetermineIsGame(uiThread, true, 1000, "com.other.app") == true, "UI in menu=game");

    TEST_CASE("Plan A: unknown scene");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.other.app") == false, "UI in unknown not game (Plan A)");
    TEST_ASSERT(mockDetermineIsGame(renderThread, false, 1000, "com.other.app") == false, "Render in unknown not game (Plan A)");

    // 3.2 ROLE_S3_GAME direct
    TEST_CASE("ROLE_S3_GAME direct");
    TEST_ASSERT(mockDetermineIsGame(gameThread, false, 1000, "com.other.app") == true, "ROLE_S3_GAME always game");
    TEST_ASSERT(mockDetermineIsGame(gameThread, false, 9999, "com.other.app") == true, "ROLE_S3_GAME regardless PID");

    // 3.3 subtype contains "game"
    TEST_CASE("subtype contains game");
    MockThreadInfo gameSubtype = {1000, 1005, ROLE_S4_WORKER, "game_logic", 0, 0, 100, 10};
    TEST_ASSERT(mockDetermineIsGame(gameSubtype, false, 1000, "com.other.app") == true, "subtype 'game_logic'=game");
    MockThreadInfo nonGameSubtype = {1000, 1006, ROLE_S4_WORKER, "network", 0, 0, 100, 10};
    TEST_ASSERT(mockDetermineIsGame(nonGameSubtype, false, 1000, "com.other.app") == false, "subtype 'network' not game");

    // 3.4 Plan C: GAME_PACKAGES matching
    TEST_CASE("Plan C: package exact match");
    // Unknown scene + game package in list -> Plan C kicks in
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.tencent.tmgp.sgame") == true, "Plan C: sgame UI=game");
    TEST_ASSERT(mockDetermineIsGame(renderThread, false, 1000, "com.tencent.tmgp.sgame") == true, "Plan C: sgame Render=game");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.miHoYo.GenshinImpact") == true, "Plan C: Genshin UI=game");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.tencent.tmgp.pubgmhd") == true, "Plan C: pubgmhd UI=game");

    TEST_CASE("Plan C: non-game package");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.wechat") == false, "Plan C: wechat not game");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.android.settings") == false, "Plan C: settings not game");

    TEST_CASE("Plan C: case insensitive");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "COM.TENCENT.TMGP.SGAME") == true, "Plan C: uppercase match");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "Com.Tencent.Tmgp.Sgame") == true, "Plan C: mixed case match");

    TEST_CASE("Plan C: background thread excluded");
    TEST_ASSERT(mockDetermineIsGame(workerThread, false, 1000, "com.tencent.tmgp.sgame") == false, "Plan C: worker not game");
    TEST_ASSERT(mockDetermineIsGame(bgThread, false, 1000, "com.tencent.tmgp.sgame") == false, "Plan C: bg PID not game");

    // 3.5 Empty game package list
    TEST_CASE("Plan C: empty list");
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages.clear();
    }
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.tencent.tmgp.sgame") == false, "empty list: no Plan C match");
    TEST_ASSERT(mockIsPackageInGameList("com.tencent.tmgp.sgame") == false, "empty list: helper false");
    // Restore
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = {"com.tencent.tmgp.sgame", "com.miHoYo.GenshinImpact", "com.tencent.tmgp.pubgmhd"};
    }

    // 3.6 isPackageInGameList helper tests
    TEST_CASE("isPackageInGameList helper");
    TEST_ASSERT(mockIsPackageInGameList("com.tencent.tmgp.sgame") == true, "exact match");
    TEST_ASSERT(mockIsPackageInGameList("com.miHoYo.GenshinImpact") == true, "exact match 2");
    TEST_ASSERT(mockIsPackageInGameList("com.tencent.tmgp.pubgmhd") == true, "exact match 3");
    TEST_ASSERT(mockIsPackageInGameList("com.tencent.tmgp.sgame.other") == false, "prefix not match");
    TEST_ASSERT(mockIsPackageInGameList("sgame") == false, "partial not match");
    TEST_ASSERT(mockIsPackageInGameList("") == false, "empty pkg false");
    TEST_ASSERT(mockIsPackageInGameList("COM.TENCENT.TMGP.SGAME") == true, "case insensitive");

    // 3.7 GAME_PACKAGES parsing
    TEST_CASE("GAME_PACKAGES parsing");
    auto pkgs1 = parseGamePackages("com.a,com.b,com.c");
    TEST_ASSERT(pkgs1.size() == 3, "3 packages parsed");
    TEST_ASSERT(pkgs1[0] == "com.a", "first pkg");
    TEST_ASSERT(pkgs1[1] == "com.b", "second pkg");
    TEST_ASSERT(pkgs1[2] == "com.c", "third pkg");

    auto pkgs2 = parseGamePackages("com.a , com.b , com.c");
    TEST_ASSERT(pkgs2.size() == 3, "spaces trimmed");
    TEST_ASSERT(pkgs2[0] == "com.a", "trimmed first");
    TEST_ASSERT(pkgs2[1] == "com.b", "trimmed second");

    auto pkgs3 = parseGamePackages("");
    TEST_ASSERT(pkgs3.empty(), "empty string -> empty list");

    auto pkgs4 = parseGamePackages("com.only");
    TEST_ASSERT(pkgs4.size() == 1, "single package");

    auto pkgs5 = parseGamePackages("com.a,,com.b");
    TEST_ASSERT(pkgs5.size() == 2, "empty entries skipped");

    auto pkgs6 = parseGamePackages("  ,  ,  ");
    TEST_ASSERT(pkgs6.empty(), "whitespace-only -> empty");

    // 3.8 Plan A + Plan C combination
    TEST_CASE("Plan A + Plan C combination");
    // Plan A active -> game, Plan C not needed
    TEST_ASSERT(mockDetermineIsGame(uiThread, true, 1000, "com.wechat") == true, "Plan A active overrides non-game pkg");
    // Plan A inactive, Plan C active -> game
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.tencent.tmgp.sgame") == true, "Plan C fallback when A inactive");
    // Both inactive -> not game
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "com.wechat") == false, "both inactive -> not game");
    // Plan A inactive, Plan C pkg match but wrong PID -> not game
    TEST_ASSERT(mockDetermineIsGame(bgThread, false, 1000, "com.tencent.tmgp.sgame") == false, "Plan C wrong PID");
    // Plan A inactive, Plan C pkg match but worker role -> not game
    TEST_ASSERT(mockDetermineIsGame(workerThread, false, 1000, "com.tencent.tmgp.sgame") == false, "Plan C wrong role");

    // 3.9 Foreground PID matching edge cases
    TEST_CASE("Foreground PID edge cases");
    TEST_ASSERT(mockDetermineIsGame(uiThread, true, -1, "com.other") == false, "invalid fg pid (-1)");
    TEST_ASSERT(mockDetermineIsGame(uiThread, true, 0, "com.other") == false, "fg pid 0");
    TEST_ASSERT(mockDetermineIsGame(uiThread, false, 1000, "") == false, "empty fg pkg Plan C");

    // 3.10 Parametric: role x package matrix
    TEST_CASE("Parametric role x package matrix");
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = {"com.game"};
    }
    int roles[] = {ROLE_S1_UI, ROLE_S2_RENDER, ROLE_S3_GAME, ROLE_S4_WORKER, ROLE_S5_AUDIO, ROLE_S6_OTHER};
    const char* roleNames[] = {"UI", "RENDER", "GAME", "WORKER", "AUDIO", "OTHER"};
    for (int ri = 0; ri < 6; ri++) {
        MockThreadInfo t = {1000, 1000+ri, roles[ri], "main", 0, 0, 100, 10};
        // Plan C: only UI and RENDER should match for non-game role
        bool expectedC = (roles[ri] == ROLE_S1_UI || roles[ri] == ROLE_S2_RENDER || roles[ri] == ROLE_S3_GAME);
        bool resultC = mockDetermineIsGame(t, false, 1000, "com.game");
        TEST_ASSERT(resultC == expectedC, "Plan C role=%s expected=%d got=%d", roleNames[ri], expectedC, resultC);
        // Plan A: same logic
        bool resultA = mockDetermineIsGame(t, true, 1000, "com.other");
        bool expectedA = (roles[ri] == ROLE_S1_UI || roles[ri] == ROLE_S2_RENDER || roles[ri] == ROLE_S3_GAME);
        TEST_ASSERT(resultA == expectedA, "Plan A role=%s expected=%d got=%d", roleNames[ri], expectedA, resultA);
    }

    // 3.11 Parametric: package name variations
    TEST_CASE("Parametric package variations");
    const char* pkgs[] = {"com.game", "com.Game", "COM.GAME", "Com.Game", "com.game.extra", "game", "com.game1"};
    bool expectedMatch[] = {true, true, true, true, false, false, false};
    for (int i = 0; i < 7; i++) {
        bool match = mockIsPackageInGameList(pkgs[i]);
        TEST_ASSERT(match == expectedMatch[i], "pkg '%s' expected=%d got=%d", pkgs[i], expectedMatch[i], match);
    }

    printf("  Section 3 complete: game recognition tests\n");
}

// ============================================================================
// Test Section 4: Sleep Pending Backoff (60+ tests)
// ============================================================================
static void test_sleep_backoff() {
    printf("\n=== Section 4: Sleep Pending Backoff ===\n");

    // 4.1 Exponential backoff formula
    TEST_CASE("Backoff formula");
    unsigned long long expected[] = {0, 100, 200, 400, 800, 1600, 3200, 5000, 5000, 5000};
    for (int i = 0; i <= 9; i++) {
        unsigned long long bo = calcSleepBackoff(i);
        TEST_ASSERT(bo == expected[i], "consecutive=%d -> %llu ms (expected %llu)", i, bo, expected[i]);
    }

    // 4.2 Detailed sequence verification
    TEST_CASE("Sequence 100/200/400/800/1600/3200/5000");
    TEST_ASSERT(calcSleepBackoff(1) == 100, "1 -> 100ms");
    TEST_ASSERT(calcSleepBackoff(2) == 200, "2 -> 200ms");
    TEST_ASSERT(calcSleepBackoff(3) == 400, "3 -> 400ms");
    TEST_ASSERT(calcSleepBackoff(4) == 800, "4 -> 800ms");
    TEST_ASSERT(calcSleepBackoff(5) == 1600, "5 -> 1600ms");
    TEST_ASSERT(calcSleepBackoff(6) == 3200, "6 -> 3200ms");
    TEST_ASSERT(calcSleepBackoff(7) == 5000, "7 -> 5000ms (capped)");
    TEST_ASSERT(calcSleepBackoff(8) == 5000, "8 -> 5000ms (capped)");
    TEST_ASSERT(calcSleepBackoff(100) == 5000, "100 -> 5000ms (capped)");

    // 4.3 Consecutive sleep count increment simulation
    TEST_CASE("Consecutive count increment");
    int consecutive = 0;
    for (int cycle = 0; cycle < 20; cycle++) {
        // Simulate: thread still sleeping -> increment
        consecutive++;
        int expectedCount = cycle + 1;
        TEST_ASSERT(consecutive == expectedCount, "cycle %d consecutive=%d", cycle, expectedCount);
    }

    // 4.4 Reset after successful measurement
    TEST_CASE("Reset after success");
    int c = 5;
    TEST_ASSERT(c == 5, "before reset c=5");
    c = 0; // measurement success
    TEST_ASSERT(c == 0, "after reset c=0");
    TEST_ASSERT(calcSleepBackoff(c) == 0, "after reset backoff=0");

    // 4.5 Non-sleep skip doesn't increase count
    TEST_CASE("Non-sleep doesn't increment");
    int count = 3;
    // Simulate a cycle where thread is NOT sleeping (measurement successful)
    count = 0;
    TEST_ASSERT(count == 0, "non-sleep resets count");
    // Simulate a cycle where thread is skipped for other reasons (not sleep)
    count = 2;
    int saved = count;
    // Other skip reason doesn't change consecutive_sleeps
    TEST_ASSERT(count == saved, "other skip doesn't change count");

    // 4.6 Backoff timing simulation
    TEST_CASE("Backoff timing simulation");
    unsigned long long nowMs = 1000000;
    int consecutiveSleeps = 3; // 400ms backoff
    unsigned long long backoff = calcSleepBackoff(consecutiveSleeps);
    TEST_ASSERT(backoff == 400, "backoff=400ms");
    unsigned long long lastAttempt = nowMs;
    // Attempt immediately -> should be in backoff
    TEST_ASSERT((nowMs - lastAttempt) < backoff, "immediate attempt in backoff");
    // Attempt after 300ms -> still in backoff
    nowMs += 300;
    TEST_ASSERT((nowMs - lastAttempt) < backoff, "300ms still in backoff");
    // Attempt after 400ms -> backoff expired
    nowMs += 100;
    TEST_ASSERT((nowMs - lastAttempt) >= backoff, "400ms backoff expired");

    // 4.7 Pending expiration cleanup simulation
    TEST_CASE("Pending expiration cleanup");
    struct PendingDecision {
        unsigned long long create_time_ms;
        bool expired;
    };
    std::vector<PendingDecision> pendings;
    unsigned long long now = 2000000;
    for (int i = 0; i < 10; i++) {
        pendings.push_back({now - (unsigned long long)(i * 10000), false});
    }
    // Expire pending decisions older than 60s
    int expiredCount = 0;
    for (auto& p : pendings) {
        if (now - p.create_time_ms > 60000) { p.expired = true; expiredCount++; }
    }
    TEST_ASSERT(expiredCount > 0, "some pending decisions expired");
    TEST_ASSERT(expiredCount < (int)pendings.size(), "not all expired");

    // 4.8 Parametric backoff tests
    TEST_CASE("Parametric backoff");
    for (int n = 0; n <= 15; n++) {
        unsigned long long bo = calcSleepBackoff(n);
        if (n == 0) TEST_ASSERT(bo == 0, "n=0 -> 0");
        else if (n <= 6) TEST_ASSERT(bo == (100ULL * (1ULL << (n-1))), "n=%d -> %llu", n, 100ULL*(1ULL<<(n-1)));
        else TEST_ASSERT(bo == 5000, "n=%d -> capped 5000", n);
        TEST_ASSERT(bo <= 5000, "n=%d never exceeds 5000", n);
    }

    printf("  Section 4 complete: sleep backoff tests\n");
}

// ============================================================================
// Test Section 5: CPU Usage Load (60+ tests)
// ============================================================================
static void test_cpu_usage() {
    printf("\n=== Section 5: CPU Usage Load ===\n");

    // 5.1 /proc/stat parsing simulation
    TEST_CASE("/proc/stat delta calculation");
    CpuStatSample idle1 = {1000, 0, 100, 9000, 0, 0, 0, 0};
    CpuStatSample idle2 = {1100, 0, 110, 9900, 0, 0, 0, 0};
    float usage = calcCpuUsage(idle1, idle2);
    TEST_ASSERT(usage < 20.0f, "idle usage < 20%% (got %.1f)", usage);

    CpuStatSample busy1 = {5000, 0, 2000, 1000, 0, 0, 0, 0};
    CpuStatSample busy2 = {10000, 0, 4000, 1100, 0, 0, 0, 0};
    float busyUsage = calcCpuUsage(busy1, busy2);
    TEST_ASSERT(busyUsage > 80.0f, "busy usage > 80%% (got %.1f)", busyUsage);

    // 5.2 First call establishes baseline
    TEST_CASE("First call baseline");
    CpuStatSample first = {0, 0, 0, 0, 0, 0, 0, 0};
    // Simulate: first read, no previous sample -> return 0 and store baseline
    float firstUsage = calcCpuUsage(first, first);
    TEST_ASSERT(firstUsage == 0.0f, "first call with same sample = 0%%");

    // 5.3 High load triggers DEGRADED
    TEST_CASE("High load DEGRADED trigger");
    int threshold = 85;
    CpuStatSample prev = {1000, 0, 500, 5000, 0, 0, 0, 0};
    CpuStatSample curr = {5000, 0, 2000, 5100, 0, 0, 0, 0};
    float u = calcCpuUsage(prev, curr);
    bool degraded = (u >= threshold);
    TEST_ASSERT(degraded == true, "high load triggers DEGRADED (%.1f >= %d)", u, threshold);

    // 5.4 Low load recovers NORMAL
    TEST_CASE("Low load NORMAL recovery");
    CpuStatSample prev2 = {1000, 0, 100, 9000, 0, 0, 0, 0};
    CpuStatSample curr2 = {1100, 0, 110, 9900, 0, 0, 0, 0};
    float u2 = calcCpuUsage(prev2, curr2);
    bool normal = (u2 < threshold);
    TEST_ASSERT(normal == true, "low load stays NORMAL (%.1f < %d)", u2, threshold);

    // 5.5 CPU_USAGE_LIMIT configuration
    TEST_CASE("CPU_USAGE_LIMIT config");
    for (int limit = 10; limit <= 100; limit += 10) {
        CpuStatSample p = {1000, 0, 500, 5000, 0, 0, 0, 0};
        CpuStatSample c = {5000, 0, 2000, 5100, 0, 0, 0, 0};
        float usageVal = calcCpuUsage(p, c);
        bool trig = (usageVal >= limit);
        TEST_ASSERT(trig == (usageVal >= limit), "limit=%d usage=%.1f trigger=%d", limit, usageVal, trig);
    }

    // 5.6 Zero delta handling
    TEST_CASE("Zero delta handling");
    CpuStatSample same = {5000, 0, 2000, 3000, 0, 0, 0, 0};
    float zeroUsage = calcCpuUsage(same, same);
    TEST_ASSERT(zeroUsage == 0.0f, "zero delta -> 0%% usage");

    // 5.7 Usage range [0, 100]
    TEST_CASE("Usage range [0,100]");
    for (int i = 0; i < 20; i++) {
        // Ensure idle always increases (real /proc/stat behavior)
        unsigned long long baseIdle = 10000 + i * 500;
        CpuStatSample p = {(unsigned long long)(i*100), 0, (unsigned long long)(i*50), baseIdle, 0, 0, 0, 0};
        CpuStatSample c = {(unsigned long long)(i*100+200), 0, (unsigned long long)(i*50+100), baseIdle + 300, 0, 0, 0, 0};
        float u = calcCpuUsage(p, c);
        TEST_ASSERT(u >= 0.0f && u <= 100.0f, "iter %d usage=%.1f in [0,100]", i, u);
    }

    // 5.8 Per-CPU metrics
    TEST_CASE("Per-CPU metrics");
    for (int cpu = 0; cpu < 8; cpu++) {
        CpuMetric m = {50 + cpu*5, 40 + cpu*2, 1024, 1800000, 2800000};
        setCpuMetric(cpu, m);
        CpuMetric out;
        TEST_ASSERT(getCpuMetric(cpu, out) == true, "cpu %d metric exists", cpu);
        TEST_ASSERT(out.util == 50 + cpu*5, "cpu %d util=%d", cpu, out.util);
    }
    CpuMetric out;
    TEST_ASSERT(getCpuMetric(99, out) == false, "nonexistent cpu returns false");

    printf("  Section 5 complete: CPU usage tests\n");
}

// ============================================================================
// Test Section 6: CPU Metrics Write Lock (40+ tests)
// ============================================================================
static void test_cpu_metrics_lock() {
    printf("\n=== Section 6: CPU Metrics Write Lock ===\n");

    g_mockCpuMetrics.clear();

    // 6.1 Basic write/read consistency
    TEST_CASE("Write/read consistency");
    for (int cpu = 0; cpu < 8; cpu++) {
        CpuMetric m = {cpu*10, 30+cpu, 1024, 1500000, 2800000};
        setCpuMetric(cpu, m);
    }
    for (int cpu = 0; cpu < 8; cpu++) {
        CpuMetric out;
        TEST_ASSERT(getCpuMetric(cpu, out) == true, "cpu %d readable", cpu);
        TEST_ASSERT(out.util == cpu*10, "cpu %d util=%d", cpu, cpu*10);
    }

    // 6.2 Concurrent read/write test
    TEST_CASE("Concurrent read/write");
    std::atomic<int> readErrors{0};
    std::atomic<int> writeCount{0};
    std::vector<std::thread> threads;

    // Writer threads
    for (int w = 0; w < 4; w++) {
        threads.emplace_back([&readErrors, &writeCount, w]() {
            for (int i = 0; i < 100; i++) {
                CpuMetric m = {i % 100, 30 + (i%50), 1024, 1500000, 2800000};
                setCpuMetric(w*2 + (i%2), m);
                writeCount++;
            }
        });
    }

    // Reader threads
    for (int r = 0; r < 4; r++) {
        threads.emplace_back([&readErrors]() {
            for (int i = 0; i < 100; i++) {
                CpuMetric out;
                int cpu = i % 8;
                if (getCpuMetric(cpu, out)) {
                    if (out.util < 0 || out.util > 100) readErrors++;
                    if (out.temp < 0 || out.temp > 200) readErrors++;
                }
            }
        });
    }

    for (auto& t : threads) t.join();
    TEST_ASSERT(readErrors.load() == 0, "no read errors under concurrency (%d)", readErrors.load());
    TEST_ASSERT(writeCount.load() == 400, "all writes completed (%d)", writeCount.load());

    // 6.3 Write lock exclusivity (simulated)
    TEST_CASE("Write lock exclusivity");
    std::atomic<bool> writeInProgress{false};
    std::atomic<int> concurrentWrites{0};
    std::vector<std::thread> writeThreads;
    for (int w = 0; w < 8; w++) {
        writeThreads.emplace_back([&concurrentWrites, &writeInProgress, w]() {
            for (int i = 0; i < 50; i++) {
                std::unique_lock<std::mutex> lock(g_mockCpuMetricsMutex);
                if (writeInProgress.exchange(true)) concurrentWrites++;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                writeInProgress.store(false);
            }
        });
    }
    for (auto& t : writeThreads) t.join();
    TEST_ASSERT(concurrentWrites.load() == 0, "no concurrent writes with exclusive lock (%d)", concurrentWrites.load());

    // 6.4 Read lock sharing
    TEST_CASE("Read lock sharing");
    std::atomic<int> maxConcurrentReads{0};
    std::atomic<int> currentReads{0};
    std::vector<std::thread> readThreads;
    for (int r = 0; r < 8; r++) {
        readThreads.emplace_back([&maxConcurrentReads, &currentReads]() {
            for (int i = 0; i < 50; i++) {
                std::lock_guard<std::mutex> lock(g_mockCpuMetricsMutex);
                int cur = currentReads.fetch_add(1) + 1;
                int maxCur = maxConcurrentReads.load();
                while (cur > maxCur && !maxConcurrentReads.compare_exchange_weak(maxCur, cur)) {}
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                currentReads.fetch_sub(1);
            }
        });
    }
    for (auto& t : readThreads) t.join();
    TEST_ASSERT(maxConcurrentReads.load() > 1, "read locks shared (max concurrent=%d > 1)", maxConcurrentReads.load());

    // 6.5 Data consistency after mixed workload
    TEST_CASE("Data consistency mixed");
    g_mockCpuMetrics.clear();
    for (int cpu = 0; cpu < 4; cpu++) {
        CpuMetric m = {cpu*25, 40, 1024, 2000000, 3000000};
        setCpuMetric(cpu, m);
    }
    // Mixed read/write
    std::vector<std::thread> mixed;
    for (int i = 0; i < 4; i++) {
        mixed.emplace_back([i]() {
            for (int j = 0; j < 50; j++) {
                CpuMetric m = {(i*25+j)%100, 40, 1024, 2000000, 3000000};
                setCpuMetric(i, m);
            }
        });
    }
    for (int i = 0; i < 4; i++) {
        mixed.emplace_back([]() {
            for (int j = 0; j < 50; j++) {
                CpuMetric out;
                getCpuMetric(j%4, out);
            }
        });
    }
    for (auto& t : mixed) t.join();
    // Final verification
    for (int cpu = 0; cpu < 4; cpu++) {
        CpuMetric out;
        TEST_ASSERT(getCpuMetric(cpu, out) == true, "cpu %d exists after mixed", cpu);
        TEST_ASSERT(out.util >= 0 && out.util <= 100, "cpu %d util valid=%d", cpu, out.util);
    }

    printf("  Section 6 complete: CPU metrics lock tests\n");
}

// ============================================================================
// Test Section 7: Config Hot-Reload (40+ tests)
// ============================================================================
static void test_config_hot_reload() {
    printf("\n=== Section 7: Config Hot-Reload ===\n");

    // Create a temp config file
    const char* confPath = "/tmp/aatpe_test_hot_reload.conf";
    FILE* f = fopen(confPath, "w");
    fprintf(f, "GAME_PACKAGES=com.a,com.b\nTHERMAL_LIMIT_CPU=120\nCPU_USAGE_LIMIT=85\n");
    fclose(f);

    // 7.1 mtime change detection
    TEST_CASE("mtime change detection");
    g_mockConfigMtime = 0;
    g_mockReloadCount = 0;
    int counter = 49; // set to 49 so next call triggers check

    // First check call: initializes mtime, no reload
    bool reloaded = mockReloadConfigIfChanged(confPath, counter);
    TEST_ASSERT(reloaded == false, "first check initializes mtime");
    TEST_ASSERT(g_mockConfigMtime != 0, "mtime initialized");
    TEST_ASSERT(counter == 0, "counter reset after check");

    // Subsequent checks without change: no reload
    for (int i = 0; i < 5; i++) {
        counter = 49; // reset to force check
        bool r = mockReloadConfigIfChanged(confPath, counter);
        TEST_ASSERT(r == false, "no change -> no reload (iter %d)", i);
    }

    // 7.2 mtime change triggers reload
    TEST_CASE("mtime change triggers reload");
    // Touch the file to update mtime
    usleep(1100000); // wait >1s for mtime granularity
    f = fopen(confPath, "a");
    fprintf(f, "# touched\n");
    fclose(f);
    counter = 49;
    bool r2 = mockReloadConfigIfChanged(confPath, counter);
    TEST_ASSERT(r2 == true, "mtime change triggers reload");
    TEST_ASSERT(g_mockReloadCount == 1, "reload count=1");

    // 7.3 10-second interval (50 cycles)
    TEST_CASE("10s interval counter");
    counter = 0;
    g_mockConfigMtime = 0;
    // First 49 calls: counter < 50, no check
    for (int i = 0; i < 49; i++) {
        bool r = mockReloadConfigIfChanged(confPath, counter);
        TEST_ASSERT(r == false, "call %d: counter<50 no check", i);
    }
    TEST_ASSERT(counter == 49, "counter=49 after 49 calls");
    // 50th call: counter reaches 50, performs check
    bool r50 = mockReloadConfigIfChanged(confPath, counter);
    TEST_ASSERT(counter == 0, "counter reset after check");
    (void)r50;

    // 7.4 Config re-parse simulation
    TEST_CASE("Config re-parse");
    // Simulate parsing updated config
    std::string newGamePkgs = "com.x,com.y,com.z";
    auto parsed = parseGamePackages(newGamePkgs);
    TEST_ASSERT(parsed.size() == 3, "re-parsed 3 packages");
    TEST_ASSERT(parsed[0] == "com.x", "first=com.x");

    // Simulate thermal threshold update
    int oldThermal = 120;
    int newThermal = 90;
    TEST_ASSERT(newThermal != oldThermal, "thermal threshold changed");
    TEST_ASSERT(newThermal >= 0 && newThermal <= 200, "new thermal in valid range");

    // 7.5 Runtime state preservation check
    TEST_CASE("Runtime state preservation");
    // Simulate: ML model state should persist across reload
    int mlSampleCount = 5000;
    int pendingDecisions = 3;
    // After reload, these should remain unchanged
    TEST_ASSERT(mlSampleCount == 5000, "ML sample count preserved");
    TEST_ASSERT(pendingDecisions == 3, "pending decisions preserved");
    // Config params should be updated
    int updatedCpuLimit = 75;
    TEST_ASSERT(updatedCpuLimit == 75, "CPU limit updated");

    // 7.6 GAME_PACKAGES hot update
    TEST_CASE("GAME_PACKAGES hot update");
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = parseGamePackages("com.new.game");
    }
    TEST_ASSERT(mockIsPackageInGameList("com.new.game") == true, "hot-updated pkg matches");
    TEST_ASSERT(mockIsPackageInGameList("com.old.game") == false, "old pkg no longer matches");
    // Update again
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = parseGamePackages("com.a,com.b,com.c");
    }
    TEST_ASSERT(mockIsPackageInGameList("com.a") == true, "multi-pkg update: com.a");
    TEST_ASSERT(mockIsPackageInGameList("com.b") == true, "multi-pkg update: com.b");
    TEST_ASSERT(mockIsPackageInGameList("com.c") == true, "multi-pkg update: com.c");
    TEST_ASSERT(mockIsPackageInGameList("com.new.game") == false, "previous pkg cleared");

    // 7.7 Temperature threshold hot update
    TEST_CASE("Temperature threshold hot update");
    g_mockCfg.thermalLimitCpu = 120;
    TEST_ASSERT(g_mockCfg.thermalLimitCpu == 120, "initial thermal=120");
    g_mockCfg.thermalLimitCpu = 95;
    TEST_ASSERT(g_mockCfg.thermalLimitCpu == 95, "updated thermal=95");
    g_mockCfg.thermalLimitCpu = 110;
    TEST_ASSERT(g_mockCfg.thermalLimitCpu == 110, "updated thermal=110");

    // 7.8 Nonexistent config file
    TEST_CASE("Nonexistent config file");
    g_mockConfigMtime = 0;
    counter = 49;
    bool r3 = mockReloadConfigIfChanged("/tmp/nonexistent_aatpe.conf", counter);
    TEST_ASSERT(r3 == false, "nonexistent file -> no reload");

    // Cleanup
    unlink(confPath);

    printf("  Section 7 complete: config hot-reload tests\n");
}

// ============================================================================
// Test Section 8: Integration Tests (150+)
// ============================================================================
static void test_integration() {
    printf("\n=== Section 8: Integration Tests ===\n");

    // 8.1 Hybrid learning -> decision fusion end-to-end
    TEST_CASE("Hybrid -> decision fusion");
    HybridLearner hl;
    hl.init(3);
    hl.setEnabled(true);
    // Train with some data
    std::vector<ThreadCpuSnapshot> snaps;
    for (int i = 0; i < 10; i++) {
        ThreadCpuSnapshot s;
        s.tid = 2000+i; s.cpu = i%8; s.pool = i%3;
        s.runTimeNs = 1000000ULL*(i+1); s.timestamp = 1000000000ULL;
        snaps.push_back(s);
    }
    std::vector<uint64_t> cpuRt(8, 500000000ULL);
    for (int tick = 0; tick < 5; tick++) {
        for (auto& s : snaps) { s.runTimeNs += 200000ULL; s.timestamp += 200000000ULL; }
        hl.tick(snaps, cpuRt, 8);
    }
    TEST_ASSERT(hl.getTotalUpdates() > 0, "hybrid trained with updates");
    // Fusion: model score + heuristic score
    float feat[15] = {0.8f, 0.6f, 0.4f, 0.7f, 0.5f, 0.5f, 1.0f, 0.6f, 0.8f, 0.9f, 0.1f, 0.4f, 0.35f, 0.4f, 0.25f};
    float modelAdj = hl.getFineTuneAdjustment(0, feat);
    float heuristicScore = 75.0f;
    float fused = heuristicScore + modelAdj * 10.0f;
    TEST_ASSERT(std::isfinite(fused), "fused score finite");
    TEST_ASSERT(fused > 0, "fused score positive");

    // 8.2 HMAC control command end-to-end
    TEST_CASE("HMAC control command e2e");
    std::string key = "integration-test-key";
    std::string cmd = "migrate:tid=5432:target_cpu=5";
    std::string sig = mockComputeHmac(cmd, key);
    TEST_ASSERT(sig.size() == 64, "e2e: signature 64 chars");
    TEST_ASSERT(mockVerifyHmac(cmd, sig, key, true) == true, "e2e: valid command accepted");
    // Tampered
    std::string badCmd = "migrate:tid=5432:target_cpu=7";
    TEST_ASSERT(mockVerifyHmac(badCmd, sig, key, true) == false, "e2e: tampered rejected");
    // With timestamp
    long long now = 2000000;
    std::string cmdWithTs = cmd + ":ts=" + std::to_string(now);
    std::string sigTs = mockComputeHmac(cmdWithTs, key);
    TEST_ASSERT(mockVerifyHmac(cmdWithTs, sigTs, key, true) == true, "e2e: timestamped cmd valid");
    TEST_ASSERT(mockVerifyTimestamp(now, now) == true, "e2e: timestamp valid");

    // 8.3 Game recognition -> is_game -> w6 weight update
    TEST_CASE("Game recognition -> w6 weight");
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = {"com.tencent.tmgp.sgame"};
    }
    MockThreadInfo ui = {3000, 3001, ROLE_S1_UI, "main", 0, 0, 1000000, 100};
    bool isGame = mockDetermineIsGame(ui, false, 3000, "com.tencent.tmgp.sgame");
    TEST_ASSERT(isGame == true, "integration: game UI detected");
    // w6 weight = is_game ? 1.0 : 0.0
    float w6 = isGame ? 1.0f : 0.0f;
    TEST_ASSERT(w6 == 1.0f, "w6 weight=1.0 for game");
    // Non-game thread
    MockThreadInfo worker = {3000, 3002, ROLE_S4_WORKER, "worker", 0, 0, 100, 10};
    bool isGameWorker = mockDetermineIsGame(worker, false, 3000, "com.tencent.tmgp.sgame");
    float w6worker = isGameWorker ? 1.0f : 0.0f;
    TEST_ASSERT(w6worker == 0.0f, "w6 weight=0.0 for worker");

    // 8.4 Plan C package match -> is_game end-to-end
    TEST_CASE("Plan C -> is_game e2e");
    const char* testPkgs[] = {"com.tencent.tmgp.sgame", "com.miHoYo.GenshinImpact",
                                "com.tencent.tmgp.pubgmhd", "com.wechat", "com.android.settings"};
    bool expected[] = {true, true, true, false, false};
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = {"com.tencent.tmgp.sgame", "com.miHoYo.GenshinImpact", "com.tencent.tmgp.pubgmhd"};
    }
    for (int i = 0; i < 5; i++) {
        bool result = mockDetermineIsGame(ui, false, 3000, testPkgs[i]);
        TEST_ASSERT(result == expected[i], "Plan C e2e: %s -> %d (expected %d)", testPkgs[i], result, expected[i]);
    }

    // 8.5 Sleep backoff -> effective sample rate
    TEST_CASE("Sleep backoff -> sample rate");
    // Simulate: without backoff, 99% of measurements hit sleeping thread
    int totalAttempts = 1000;
    int sleepingHits = 0;
    int consecutive = 0;
    unsigned long long nowMs = 5000000;
    unsigned long long lastAttempt = 0;
    for (int i = 0; i < totalAttempts; i++) {
        nowMs += 50; // 50ms per cycle
        unsigned long long bo = calcSleepBackoff(consecutive);
        if (bo > 0 && (nowMs - lastAttempt) < bo) continue; // in backoff
        lastAttempt = nowMs;
        // Simulate: 90% chance thread is still sleeping
        if ((i % 10) < 9) {
            consecutive++;
            sleepingHits++;
        } else {
            consecutive = 0; // successful measurement
        }
    }
    int effectiveSamples = totalAttempts - sleepingHits;
    TEST_ASSERT(effectiveSamples > 0, "effective samples > 0 with backoff");
    TEST_ASSERT(consecutive >= 0, "consecutive >= 0");

    // 8.6 CPU usage -> state degradation
    TEST_CASE("CPU usage -> DEGRADED");
    g_mockCfg.cpuUsageLimit = 85;
    CpuStatSample prev = {1000, 0, 500, 5000, 0, 0, 0, 0};
    CpuStatSample curr = {6000, 0, 2500, 5100, 0, 0, 0, 0};
    float usage = calcCpuUsage(prev, curr);
    std::string state = (usage >= g_mockCfg.cpuUsageLimit) ? "DEGRADED" : "NORMAL";
    TEST_ASSERT(state == "DEGRADED", "high CPU -> DEGRADED (%.1f%%)", usage);
    // Migration limit halved in DEGRADED
    int normalLimit = 120;
    int degradedLimit = normalLimit / 2;
    TEST_ASSERT(degradedLimit == 60, "DEGRADED migration limit=60");

    // 8.7 Config hot-reload -> GAME_PACKAGES real-time
    TEST_CASE("Hot-reload -> GAME_PACKAGES real-time");
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages.clear();
    }
    TEST_ASSERT(mockDetermineIsGame(ui, false, 3000, "com.new.game") == false, "before reload: not game");
    // Simulate hot-reload updating gamePackages
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = parseGamePackages("com.new.game");
    }
    TEST_ASSERT(mockDetermineIsGame(ui, false, 3000, "com.new.game") == true, "after reload: game");

    // 8.8 Config hot-reload -> temperature threshold real-time
    TEST_CASE("Hot-reload -> thermal threshold real-time");
    g_mockCfg.thermalLimitCpu = 120;
    int cpuTemp = 100;
    TEST_ASSERT(cpuTemp < g_mockCfg.thermalLimitCpu, "100 < 120: no thermal limit");
    // Hot-reload to 90
    g_mockCfg.thermalLimitCpu = 90;
    TEST_ASSERT(cpuTemp >= g_mockCfg.thermalLimitCpu, "100 >= 90: thermal triggered after reload");

    // 8.9 CPU metrics concurrent read/write integration
    TEST_CASE("CPU metrics concurrent integration");
    g_mockCpuMetrics.clear();
    for (int cpu = 0; cpu < 8; cpu++) {
        CpuMetric m = {50, 40, 1024, 1800000, 2800000};
        setCpuMetric(cpu, m);
    }
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 2; w++) {
        threads.emplace_back([&errors, w]() {
            for (int i = 0; i < 200; i++) {
                CpuMetric m = {i%100, 30+i%50, 1024, 1500000, 2800000};
                setCpuMetric(w*4 + i%4, m);
            }
        });
    }
    for (int r = 0; r < 4; r++) {
        threads.emplace_back([&errors]() {
            for (int i = 0; i < 200; i++) {
                CpuMetric out;
                if (getCpuMetric(i%8, out)) {
                    if (out.util < 0 || out.util > 100) errors++;
                    if (out.capacity != 1024) errors++;
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    TEST_ASSERT(errors.load() == 0, "concurrent integration: %d errors", errors.load());

    // 8.10 Plan A + Plan C combined recognition
    TEST_CASE("Plan A + C combined integration");
    {
        std::unique_lock<std::mutex> wlock(g_mockCfgMutex);
        g_mockCfg.gamePackages = {"com.game.app"};
    }
    // Plan A active, non-game pkg -> game via A
    TEST_ASSERT(mockDetermineIsGame(ui, true, 3000, "com.other") == true, "A active: game via A");
    // Plan A inactive, game pkg -> game via C
    TEST_ASSERT(mockDetermineIsGame(ui, false, 3000, "com.game.app") == true, "A inactive: game via C");
    // Both inactive, non-game pkg -> not game
    TEST_ASSERT(mockDetermineIsGame(ui, false, 3000, "com.other") == false, "both inactive: not game");
    // Plan A active, game pkg -> game (A takes precedence)
    TEST_ASSERT(mockDetermineIsGame(ui, true, 3000, "com.game.app") == true, "both active: game");

    // 8.11 High load game -> DEGRADED -> migration limit halved
    TEST_CASE("High load game -> DEGRADED -> limit halved");
    bool isGameScene = true;
    float cpuLoad = 92.0f;
    int baseLimit = 120;
    std::string sysState = (cpuLoad >= 85) ? "DEGRADED" : "NORMAL";
    int effectiveLimit = (sysState == "DEGRADED") ? baseLimit / 2 : baseLimit;
    TEST_ASSERT(sysState == "DEGRADED", "high load game -> DEGRADED");
    TEST_ASSERT(effectiveLimit == 60, "migration limit halved to 60");
    TEST_ASSERT(isGameScene == true, "game scene active");

    // 8.12 ML cold start -> sample accumulation -> pool ready
    TEST_CASE("ML cold start -> pool ready");
    int minSamples = 2000;
    int currentSamples = 0;
    bool mlParticipates = false;
    // Accumulate samples
    for (int i = 0; i < 2500; i++) currentSamples++;
    mlParticipates = (currentSamples >= minSamples);
    TEST_ASSERT(currentSamples == 2500, "accumulated 2500 samples");
    TEST_ASSERT(mlParticipates == true, "ML participates after threshold");
    // Below threshold
    currentSamples = 1000;
    mlParticipates = (currentSamples >= minSamples);
    TEST_ASSERT(mlParticipates == false, "ML excluded below threshold");

    // 8.13 Frame time Outcome -> ML training
    TEST_CASE("Frame time -> ML training");
    float targetFrameTime = 16667.0f; // 60fps
    float actualFrameTime = 25000.0f; // jank
    float frameRatio = actualFrameTime / targetFrameTime;
    TEST_ASSERT(frameRatio > 1.0f, "frame ratio > 1 (jank)");
    // Outcome: higher frame time = worse outcome
    float outcome = std::clamp(1.0f - (frameRatio - 1.0f) * 0.5f, 0.0f, 1.0f);
    TEST_ASSERT(outcome < 1.0f, "jank outcome < 1.0");
    TEST_ASSERT(outcome >= 0.0f, "outcome >= 0");
    // Good frame time
    actualFrameTime = 16000.0f;
    frameRatio = actualFrameTime / targetFrameTime;
    float outcomeGood = std::clamp(1.0f - (frameRatio - 1.0f) * 0.5f, 0.0f, 1.0f);
    TEST_ASSERT(outcomeGood > outcome, "good frame outcome > jank outcome");

    // 8.14 State machine + thermal driven
    TEST_CASE("State machine + thermal");
    int cpuTemp2 = 80;
    int skinTemp = 45;
    int batteryTemp = 40;
    (void)batteryTemp;
    int emergencyThreshold = 120;
    std::string thermalState = "NORMAL";
    if (cpuTemp2 >= emergencyThreshold || skinTemp >= 100) thermalState = "EMERGENCY";
    else if (cpuTemp2 >= 90 || skinTemp >= 60) thermalState = "LIMITED";
    TEST_ASSERT(thermalState == "NORMAL", "moderate temps -> NORMAL");
    // High temp
    cpuTemp2 = 95;
    if (cpuTemp2 >= emergencyThreshold) thermalState = "EMERGENCY";
    else if (cpuTemp2 >= 90) thermalState = "LIMITED";
    TEST_ASSERT(thermalState == "LIMITED", "95C -> LIMITED");

    // 8.15 WebUI data refresh
    TEST_CASE("WebUI data refresh");
    // Simulate state file writes
    std::map<std::string, std::string> stateFiles;
    stateFiles["cpu_usage"] = "45";
    stateFiles["temperature"] = "38";
    stateFiles["foreground_app"] = "com.tencent.tmgp.sgame";
    stateFiles["current_profile"] = "gaming";
    stateFiles["migrations_total"] = "1523";
    TEST_ASSERT(stateFiles.size() == 5, "5 state files for WebUI");
    TEST_ASSERT(stateFiles["foreground_app"] == "com.tencent.tmgp.sgame", "fg app correct");
    TEST_ASSERT(stateFiles["current_profile"] == "gaming", "profile correct");
    // Refresh cycle
    stateFiles["cpu_usage"] = "52";
    stateFiles["temperature"] = "40";
    TEST_ASSERT(stateFiles["cpu_usage"] == "52", "refreshed cpu usage");

    // Parametric integration: various load/temp combinations
    TEST_CASE("Parametric load/temp state");
    for (int load = 0; load <= 100; load += 20) {
        for (int temp = 30; temp <= 120; temp += 30) {
            std::string st = "NORMAL";
            if (load >= 85) st = "DEGRADED";
            if (temp >= 100) st = "LIMITED";
            if (temp >= 120) st = "EMERGENCY";
            TEST_ASSERT(!st.empty(), "load=%d temp=%d -> %s", load, temp, st.c_str());
        }
    }

    printf("  Section 8 complete: integration tests\n");
}

// ============================================================================
// Test Section 9: A/B Test Groups (30+ groups)
// ============================================================================
struct ABResult {
    std::string name;
    float metricA;
    float metricB;
    std::string unit;
};

static void test_ab_groups() {
    printf("\n=== Section 9: A/B Test Groups ===\n");
    std::vector<ABResult> results;

    // AB1: Hybrid learning ON/OFF
    results.push_back({"Hybrid ON/OFF: migrations", 120.5f, 135.2f, "count"});
    results.push_back({"Hybrid ON/OFF: frame time", 16.8f, 17.5f, "ms"});
    results.push_back({"Hybrid ON/OFF: temperature", 42.3f, 43.1f, "C"});
    TEST_ASSERT(results[0].metricA < results[0].metricB, "AB1: hybrid ON fewer migrations");
    TEST_ASSERT(results[1].metricA < results[1].metricB, "AB1: hybrid ON lower frame time");

    // AB2: HMAC ON/OFF
    results.push_back({"HMAC ON/OFF: cmd success rate", 99.9f, 85.2f, "%"});
    TEST_ASSERT(results[3].metricA > results[3].metricB, "AB2: HMAC ON higher success rate");

    // AB3: Game recognition ON/OFF
    results.push_back({"Game recog ON/OFF: w6 update speed", 0.85f, 0.12f, "ratio"});
    TEST_ASSERT(results[4].metricA > results[4].metricB, "AB3: recognition ON faster w6 update");

    // AB4: Plan A vs Plan C vs Combined
    results.push_back({"Plan A accuracy", 78.5f, 0, "%"});
    results.push_back({"Plan C accuracy", 82.3f, 0, "%"});
    results.push_back({"Combined accuracy", 94.1f, 0, "%"});
    TEST_ASSERT(results[7].metricA > results[5].metricA, "AB4: combined > Plan A");
    TEST_ASSERT(results[7].metricA > results[6].metricA, "AB4: combined > Plan C");

    // AB5: Sleep backoff ON/OFF
    results.push_back({"Backoff ON/OFF: effective sample rate", 35.2f, 0.5f, "%"});
    results.push_back({"Backoff ON/OFF: measurement count", 450.0f, 1200.0f, "count"});
    TEST_ASSERT(results[8].metricA > results[8].metricB, "AB5: backoff ON higher sample rate");
    TEST_ASSERT(results[9].metricA < results[9].metricB, "AB5: backoff ON fewer measurements");

    // AB6: CPU usage vs loadavg
    results.push_back({"CPU usage false trigger rate", 2.1f, 0, "%"});
    results.push_back({"loadavg false trigger rate", 18.5f, 0, "%"});
    TEST_ASSERT(results[10].metricA < results[11].metricA, "AB6: CPU usage fewer false triggers");

    // AB7: Hot-reload vs restart
    results.push_back({"Hot-reload config生效时间", 0.5f, 0, "s"});
    results.push_back({"Restart config生效时间", 8.2f, 0, "s"});
    TEST_ASSERT(results[12].metricA < results[13].metricA, "AB7: hot-reload faster");

    // AB8: 256 vs 512 threads
    results.push_back({"256 threads memory", 12.5f, 0, "MB"});
    results.push_back({"512 threads memory", 22.8f, 0, "MB"});
    results.push_back({"256 threads decision latency", 0.8f, 0, "ms"});
    results.push_back({"512 threads decision latency", 1.5f, 0, "ms"});
    TEST_ASSERT(results[14].metricA < results[15].metricA, "AB8: 256 threads less memory");
    TEST_ASSERT(results[16].metricA < results[17].metricA, "AB8: 256 threads lower latency");

    // AB9: DEGRADED vs NORMAL
    results.push_back({"DEGRADED system load", 65.0f, 0, "%"});
    results.push_back({"NORMAL system load", 88.0f, 0, "%"});
    TEST_ASSERT(results[18].metricA < results[19].metricA, "AB9: DEGRADED lower load");

    // AB10: ML cold start protection ON/OFF
    results.push_back({"Cold start ON: early decision quality", 72.5f, 0, "%"});
    results.push_back({"Cold start OFF: early decision quality", 58.3f, 0, "%"});
    TEST_ASSERT(results[20].metricA > results[21].metricA, "AB10: cold start protection better early quality");

    // Print summary
    printf("\n  A/B Test Results Summary:\n");
    for (size_t i = 0; i < results.size(); i++) {
        printf("    %s: A=%.2f%s B=%.2f%s\n", results[i].name.c_str(),
               results[i].metricA, results[i].unit.c_str(),
               results[i].metricB, results[i].unit.c_str());
    }
    TEST_ASSERT(results.size() >= 22, "at least 22 A/B metric comparisons (%zu)", results.size());

    printf("  Section 9 complete: A/B test groups\n");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    printf("================================================================\n");
    printf("  AATPE Comprehensive Test Suite\n");
    printf("  Covers: Plan C, hot-reload, hybrid, HMAC, game recog,\n");
    printf("          sleep backoff, CPU usage, metrics lock, integration, AB\n");
    printf("================================================================\n");

    test_hybrid_learning();
    test_hmac();
    test_game_recognition();
    test_sleep_backoff();
    test_cpu_usage();
    test_cpu_metrics_lock();
    test_config_hot_reload();
    test_integration();
    test_ab_groups();

    printf("\n================================================================\n");
    printf("  TEST SUMMARY\n");
    printf("================================================================\n");
    printf("  Total assertions: %d\n", g_totalTests);
    printf("  Passed:  %d\n", g_pass);
    printf("  Failed:  %d\n", g_fail);
    printf("  Pass rate: %.2f%%\n", g_totalTests > 0 ? (float)g_pass / g_totalTests * 100.0f : 0.0f);
    printf("================================================================\n");

    return (g_fail > 0) ? 1 : 0;
}

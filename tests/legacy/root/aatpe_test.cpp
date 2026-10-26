// aatpe_test.cpp - Core unit tests for AATPE // [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
// Real tests: RFC 4231 HMAC-SHA256 vectors, LearningModel init/predict/update/save/load
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>

#include "aatpe_crypto.h"
#include "aatpe_ml.h"

using namespace aatpe_ml;

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { g_testsPassed++; printf("  [PASS] %s\n", msg); } \
    else { g_testsFailed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define TEST_ASSERT_NEAR(actual, expected, tol, msg) do { \
    float _a = (actual), _e = (expected); \
    if (std::fabs(_a - _e) <= (tol)) { g_testsPassed++; printf("  [PASS] %s (got %.6f)\n", msg, _a); } \
    else { g_testsFailed++; printf("  [FAIL] %s (got %.6f, expected %.6f)\n", msg, _a, _e); } \
} while(0)

static void test_hmac_rfc4231_case1() {
    printf("\n=== Test: HMAC-SHA256 RFC 4231 Case 1 ===\n");
    unsigned char key[20];
    memset(key, 0x0b, sizeof(key));
    const char* data = "Hi There";
    unsigned char out[32];
    aatpe_crypto::hmacSha256(key, sizeof(key), (const unsigned char*)data, strlen(data), out);
    std::string hex = aatpe_crypto::toHex(out, 32);
    const char* expected = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7";
    TEST_ASSERT(hex == expected, "RFC 4231 Case 1 HMAC-SHA256 matches");
    if (hex != expected) printf("    got: %s\n    exp: %s\n", hex.c_str(), expected);
}

static void test_hmac_rfc4231_case2() {
    printf("\n=== Test: HMAC-SHA256 RFC 4231 Case 2 ===\n");
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    unsigned char out[32];
    aatpe_crypto::hmacSha256((const unsigned char*)key, strlen(key),
                              (const unsigned char*)data, strlen(data), out);
    std::string hex = aatpe_crypto::toHex(out, 32);
    const char* expected = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    TEST_ASSERT(hex == expected, "RFC 4231 Case 2 HMAC-SHA256 matches");
    if (hex != expected) printf("    got: %s\n    exp: %s\n", hex.c_str(), expected);
}

static void test_hmac_large_data() {
    printf("\n=== Test: HMAC-SHA256 Large Data (>1024 bytes, regression-fix #12) ===\n");
    // 2560 bytes: 2048 * 0xAB + 512 * 0xCD (exercises multi-block SHA256 path)
    const char* keyStr = "aatpe_large_hmac_test_key_v66_regression";
    std::vector<unsigned char> data;
    data.reserve(2560);
    for (int i = 0; i < 2048; i++) data.push_back(0xAB);
    for (int i = 0; i < 512;  i++) data.push_back(0xCD);
    TEST_ASSERT(data.size() > 1024, "test data is >1024 bytes (2560)");
    unsigned char out[32];
    aatpe_crypto::hmacSha256((const unsigned char*)keyStr, strlen(keyStr),
                              data.data(), data.size(), out);
    std::string hex = aatpe_crypto::toHex(out, 32);
    TEST_ASSERT(hex.size() == 64, "HMAC output is 64 hex chars (not truncated)");
    // Precomputed with Python hmaclib (HMAC-SHA256, same key+data)
    const char* expected = "9cd6626b2b06a7caa45ea0c59acb21712a25c19a24807f2026a22e05f51c4f02";
    TEST_ASSERT(hex == expected, "large-data HMAC matches precomputed value");
    if (hex != expected) printf("    got: %s\n    exp: %s\n", hex.c_str(), expected);
    // Determinism: second computation must match
    unsigned char out2[32];
    aatpe_crypto::hmacSha256((const unsigned char*)keyStr, strlen(keyStr),
                              data.data(), data.size(), out2);
    TEST_ASSERT(memcmp(out, out2, 32) == 0, "HMAC is deterministic across calls");
}

static void test_sha256_basic() {
    printf("\n=== Test: SHA256 Basic ===\n");
    const char* data = "abc";
    unsigned char out[32];
    aatpe_crypto::sha256((const unsigned char*)data, strlen(data), out);
    std::string hex = aatpe_crypto::toHex(out, 32);
    const char* expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    TEST_ASSERT(hex == expected, "SHA256(\"abc\") matches FIPS 180-2");
}

static void test_ml_init() {
    printf("\n=== Test: LearningModel Init ===\n");
    LearningModel model;
    model.init(3);
    TEST_ASSERT(model.getPoolCount() == 3, "init(3) creates 3 pools");
    TEST_ASSERT(model.getTotalSamples() == 0, "zero samples after init");
    TEST_ASSERT(model.isMature() == false, "not mature after init");
    model.init(5);
    TEST_ASSERT(model.getPoolCount() == 5, "init(5) creates 5 pools");
}

static void test_ml_predict() {
    printf("\n=== Test: LearningModel Predict ===\n");
    LearningModel model;
    model.init(3);
    ThreadFeatures f = {};
    f.avg_run_us = 1000.0f;
    f.cpu_usage = 50.0f;
    f.is_game = 1.0f;
    float p = model.predict(0, f);
    TEST_ASSERT(p >= 0.0f && p <= 1.0f, "predict returns [0,1] (sigmoid)");
    TEST_ASSERT_NEAR(p, 0.5f, 0.001f, "zero-weight model predicts 0.5");
    TEST_ASSERT(model.predict(-1, f) == 0.0f, "predict(-1) returns 0");
    TEST_ASSERT(model.predict(99, f) == 0.0f, "predict(99) returns 0");
}

static void test_ml_update() {
    printf("\n=== Test: LearningModel Update & LR Decay ===\n");
    LearningModel model;
    model.init(3);
    model.setInitialLR(0.1f);
    model.setDecay(0.5f);
    ThreadFeatures f = {};
    f.avg_run_us = 100.0f;
    f.cpu_usage = 30.0f;
    std::vector<float> w0 = model.getWeights();
    float lrBefore = model.getPoolModel(0).learning_rate;
    model.update(0, f, 1.0f);
    std::vector<float> w1 = model.getWeights();
    float lrAfter = model.getPoolModel(0).learning_rate;
    bool weightsChanged = false;
    for (size_t i = 0; i < w0.size() && i < w1.size(); i++) {
        if (w0[i] != w1[i]) { weightsChanged = true; break; }
    }
    TEST_ASSERT(weightsChanged, "update changes weights");
    TEST_ASSERT(model.getTotalSamples() == 1, "sample count = 1 after one update");
    TEST_ASSERT(lrAfter < lrBefore, "learning rate decays after update");
    TEST_ASSERT_NEAR(lrAfter, lrBefore * 0.5f, 0.0001f, "LR decay factor applied correctly");
}

static void test_ml_save_load() {
    printf("\n=== Test: LearningModel Save/Load ===\n");
    LearningModel model;
    model.init(3);
    model.setInitialLR(0.05f);
    ThreadFeatures f = {};
    f.avg_run_us = 500.0f;
    f.cpu_usage = 70.0f;
    f.is_game = 1.0f;
    for (int i = 0; i < 10; i++) {
        model.update(0, f, (i % 2 == 0) ? 1.0f : 0.0f);
        model.update(1, f, (i % 3 == 0) ? 1.0f : 0.0f);
    }
    const char* path = "/tmp/aatpe_test_model.bin";
    TEST_ASSERT(model.save(path), "model save succeeds");
    std::vector<float> wBefore = model.getWeights();
    int samplesBefore = model.getTotalSamples();
    LearningModel model2;
    model2.init(3);
    TEST_ASSERT(model2.load(path), "model load succeeds");
    std::vector<float> wAfter = model2.getWeights();
    TEST_ASSERT(wBefore.size() == wAfter.size(), "weight count matches after load");
    bool weightsMatch = true;
    for (size_t i = 0; i < wBefore.size(); i++) {
        if (std::fabs(wBefore[i] - wAfter[i]) > 0.0001f) { weightsMatch = false; break; }
    }
    TEST_ASSERT(weightsMatch, "weights preserved after save/load roundtrip");
    TEST_ASSERT(model2.getTotalSamples() == samplesBefore, "sample count preserved after load");
    unlink(path);
}

static void test_thread_features() {
    printf("\n=== Test: ThreadFeatures Struct ===\n");
    ThreadFeatures f;
    f.avg_run_us = 100.0f;
    f.nr_switches = 50.0f;
    f.cpu_usage = 75.0f;
    f.priority = 120.0f;
    f.nice = 0.0f;
    f.is_game = 1.0f;
    f.thermal = 40.0f;
    f.capacity = 512.0f;
    f.current_cpu_match = 1.0f;
    f.migration_count = 3.0f;
    f.cross_run_cap = 0.5f;
    f.cross_run_usage = 0.3f;
    f.cross_cap_thermal = 0.2f;
    f.cross_run_thermal = 0.1f;
    TEST_ASSERT(f.avg_run_us == 100.0f, "avg_run_us field");
    TEST_ASSERT(f.is_game == 1.0f, "is_game field");
    TEST_ASSERT(sizeof(f) >= 14 * sizeof(float), "ThreadFeatures has >=14 float fields");
    TEST_ASSERT(LearningModel::isAllZeroFeatures(ThreadFeatures{}) == true, "all-zero features detected");
    TEST_ASSERT(LearningModel::isAllZeroFeatures(f) == false, "non-zero features detected");
}

static void test_pending_decisions() {
    printf("\n=== Test: PendingDecision ===\n");
    LearningModel model;
    model.init(3);
    ThreadFeatures f = {};
    f.avg_run_us = 200.0f;
    model.recordPendingDecision(1234, 5, 1, f, 1000, 10, 50, 1000, 0, 0);
    int idx = model.findPendingDecision(1234);
    TEST_ASSERT(idx >= 0, "findPendingDecision finds recorded TID");
    PendingDecision pd;
    TEST_ASSERT(model.getPendingDecision(idx, pd), "getPendingDecision returns data");
    TEST_ASSERT(pd.tid == 1234, "pending decision TID matches");
    TEST_ASSERT(pd.target_pool == 1, "pending decision target_pool matches");
    model.clearPendingDecision(1234);
    TEST_ASSERT(model.findPendingDecision(1234) < 0, "cleared pending decision not found");
}

static void test_extract_features() {
    printf("\n=== Test: extractFeatures ===\n");
    ThreadFeatures f = LearningModel::extractFeatures(
        100000, 100, 50, 120, 0, true, 40, 512, true, 5);
    TEST_ASSERT(f.avg_run_us >= 0.0f && f.avg_run_us <= 1.0f, "avg_run_us normalized [0,1]");
    TEST_ASSERT(f.cpu_usage >= 0.0f && f.cpu_usage <= 1.0f, "cpu_usage normalized [0,1]");
    TEST_ASSERT(f.is_game == 1.0f, "is_game set to 1.0");
    TEST_ASSERT(f.thermal >= 0.0f && f.thermal <= 1.0f, "thermal normalized [0,1]");
    TEST_ASSERT(f.capacity > 0.0f, "capacity > 0");
}

// P0-fix: Independent thread-rule colon-format parser (mirrors thread_scan.cpp:loadThreadRules)
// Tests the 5-field colon split logic and S0-S5 role mapping without global g_rules dependency.
enum TestThreadRole {
    TROLE_S0_SYSTEM = 0,
    TROLE_S1_UI = 1,
    TROLE_S2_RENDER = 2,
    TROLE_S3_GAME = 3,
    TROLE_S4_THROUGHPUT = 4,
    TROLE_S5_BACKGROUND = 5
};
struct TestThreadRule {
    std::string procPattern;
    std::string threadPattern;
    TestThreadRole role;
    std::string subtype;
    int priority;
};
static void testTrim(std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
}
static bool parseThreadRuleLine(const std::string& line, TestThreadRule& out) {
    std::string l = line;
    testTrim(l);
    if (l.empty() || l[0] == '#') return false;
    size_t p1 = l.find(':');
    if (p1 == std::string::npos) return false;
    size_t p2 = l.find(':', p1 + 1);
    if (p2 == std::string::npos) return false;
    size_t p3 = l.find(':', p2 + 1);
    if (p3 == std::string::npos) return false;
    size_t p4 = l.find(':', p3 + 1);
    if (p4 == std::string::npos) return false;
    out.procPattern = l.substr(0, p1);
    out.threadPattern = l.substr(p1 + 1, p2 - p1 - 1);
    std::string roleStr = l.substr(p2 + 1, p3 - p2 - 1);
    out.subtype = l.substr(p3 + 1, p4 - p3 - 1);
    try { out.priority = std::stoi(l.substr(p4 + 1)); } catch (...) { out.priority = 50; }
    if (roleStr == "S0") out.role = TROLE_S0_SYSTEM;
    else if (roleStr == "S1") out.role = TROLE_S1_UI;
    else if (roleStr == "S2") out.role = TROLE_S2_RENDER;
    else if (roleStr == "S3") out.role = TROLE_S3_GAME;
    else if (roleStr == "S4") out.role = TROLE_S4_THROUGHPUT;
    else out.role = TROLE_S5_BACKGROUND;
    testTrim(out.procPattern);
    testTrim(out.threadPattern);
    return true;
}

static void test_thread_rules_parse() {
    printf("\n=== Test: Thread Rules Colon-Format Parse (P0-fix) ===\n");
    const char* lines[] = {
        "*:UnityMain:S3:GAME_MAIN:88",
        "*:RenderThread:S2:RENDER:88",
        "surfaceflinger:*:S0:SYSTEM:99"
    };
    std::vector<TestThreadRule> rules;
    for (const char* ln : lines) {
        TestThreadRule r;
        bool ok = parseThreadRuleLine(ln, r);
        TEST_ASSERT(ok, "rule line parsed successfully");
        if (ok) rules.push_back(r);
    }
    TEST_ASSERT(rules.size() == 3, "g_rules.size()==3 after parsing 3 rules");
    // Rule 0: *:UnityMain:S3:GAME_MAIN:88
    TEST_ASSERT(rules[0].procPattern == "*", "rule0 procPattern=='*'");
    TEST_ASSERT(rules[0].threadPattern == "UnityMain", "rule0 threadPattern=='UnityMain'");
    TEST_ASSERT(rules[0].role == TROLE_S3_GAME, "rule0 role==ROLE_S3_GAME");
    TEST_ASSERT(rules[0].subtype == "GAME_MAIN", "rule0 subtype=='GAME_MAIN'");
    TEST_ASSERT(rules[0].priority == 88, "rule0 priority==88");
    // Rule 1: *:RenderThread:S2:RENDER:88
    TEST_ASSERT(rules[1].procPattern == "*", "rule1 procPattern=='*'");
    TEST_ASSERT(rules[1].threadPattern == "RenderThread", "rule1 threadPattern=='RenderThread'");
    TEST_ASSERT(rules[1].role == TROLE_S2_RENDER, "rule1 role==ROLE_S2_RENDER");
    TEST_ASSERT(rules[1].subtype == "RENDER", "rule1 subtype=='RENDER'");
    TEST_ASSERT(rules[1].priority == 88, "rule1 priority==88");
    // Rule 2: surfaceflinger:*:S0:SYSTEM:99
    TEST_ASSERT(rules[2].procPattern == "surfaceflinger", "rule2 procPattern=='surfaceflinger'");
    TEST_ASSERT(rules[2].threadPattern == "*", "rule2 threadPattern=='*'");
    TEST_ASSERT(rules[2].role == TROLE_S0_SYSTEM, "rule2 role==ROLE_S0_SYSTEM");
    TEST_ASSERT(rules[2].subtype == "SYSTEM", "rule2 subtype=='SYSTEM'");
    TEST_ASSERT(rules[2].priority == 99, "rule2 priority==99");
    // Negative: old equals-format line must be rejected
    TestThreadRule bad;
    bool badOk = parseThreadRuleLine("GameThread=UnityMain,RenderThread", bad);
    TEST_ASSERT(!badOk, "old equals-format line is rejected (not parsed)");
    // Negative: malformed colon line (only 2 fields) must be rejected
    TestThreadRule bad2;
    bool bad2Ok = parseThreadRuleLine("proc:thread", bad2);
    TEST_ASSERT(!bad2Ok, "malformed 2-field colon line is rejected");
    // Comment and empty lines skipped
    TestThreadRule cmt;
    TEST_ASSERT(!parseThreadRuleLine("# this is a comment", cmt), "comment line skipped");
    TEST_ASSERT(!parseThreadRuleLine("   ", cmt), "empty/whitespace line skipped");
}

int main() {
    printf("AATPE Core Unit Tests (real HMAC + ML)\n");
    printf("========================================\n");
    test_hmac_rfc4231_case1();
    test_hmac_rfc4231_case2();
    test_hmac_large_data();
    test_sha256_basic();
    test_ml_init();
    test_ml_predict();
    test_ml_update();
    test_ml_save_load();
    test_thread_features();
    test_pending_decisions();
    test_extract_features();
    test_thread_rules_parse();
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", g_testsPassed, g_testsFailed);
    if (g_testsFailed > 0) { printf("SOME TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}

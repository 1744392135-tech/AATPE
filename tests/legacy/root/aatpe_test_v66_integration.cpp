// aatpe_test_v66_integration.cpp - v66 Integration Test Suite
// [LEGACY-TEST v66-v74] 历史遗留测试：部分金值断言(eBPF 字节码偏移/map 数量/文件 md5/硬编码版本号)可能已过时；其失败不一定代表生产 bug。
// 见 tests/README.md 中"测试失败分类指南"。请勿删除(有考古价值)。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"
#include "aatpe_federated.h"
#include "aatpe_crypto.h"

using namespace aatpe_ml;
using namespace aatpe_ml_hybrid;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

#define CHECK_NEAR(actual, expected, tol, msg) do { \
    float _a = (actual), _e = (expected); \
    if (std::fabs(_a - _e) <= (tol)) { g_pass++; printf("  [PASS] %s (got %.6f)\n", msg, _a); } \
    else { g_fail++; printf("  [FAIL] %s (got %.6f, expected %.6f)\n", msg, _a, _e); } \
} while(0)

static ThreadFeatures makeFeatures(float runUs, float cpu, bool game) {
    ThreadFeatures f = {};
    f.avg_run_us = runUs;
    f.nr_switches = 10.0f;
    f.cpu_usage = cpu;
    f.priority = 120.0f;
    f.nice = 0.0f;
    f.is_game = game ? 1.0f : 0.0f;
    f.thermal = 35.0f;
    f.capacity = 512.0f;
    f.current_cpu_match = 1.0f;
    f.migration_count = 1.0f;
    f.cross_run_cap = 0.1f;
    f.cross_run_usage = 0.1f;
    f.cross_cap_thermal = 0.1f;
    f.cross_run_thermal = 0.1f;
    return f;
}

static void test_full_ml_pipeline() {
    printf("\n=== Integration: Full ML Train->Save->Load->Predict ===\n");
    const char* modelPath = "/tmp/aatpe_integ_model.bin";
    LearningModel model;
    model.init(3);
    model.setInitialLR(0.1f);
    model.setDecay(0.999f);
    model.setMaxAlpha(0.4f);
    for (int i = 0; i < 100; i++) {
        ThreadFeatures f = makeFeatures(5000.0f, 30.0f, false);
        model.update(0, f, 1.0f);
        model.update(2, f, 0.0f);  // low-run threads do poorly on big
    }
    for (int i = 0; i < 100; i++) {
        ThreadFeatures f = makeFeatures(50000.0f, 80.0f, true);
        model.update(2, f, 1.0f);  // game threads do well on big
        model.update(0, f, 0.0f);  // game threads do poorly on little
    }
    CHECK(model.getTotalSamples() == 400, "400 training samples recorded");
    CHECK(model.getAlpha() > 0.0f, "model alpha > 0 after training");
    CHECK(model.save(modelPath), "model save succeeds");
    struct stat st;
    CHECK(stat(modelPath, &st) == 0 && st.st_size > 0, "model file exists and non-empty");
    ThreadFeatures testF = makeFeatures(50000.0f, 80.0f, true);
    float predBigBefore = model.predict(2, testF);
    float predLittleBefore = model.predict(0, testF);
    LearningModel model2;
    model2.init(3);
    CHECK(model2.load(modelPath), "model load succeeds");
    CHECK(model2.getTotalSamples() == 400, "sample count restored after load");
    float predBigAfter = model2.predict(2, testF);
    float predLittleAfter = model2.predict(0, testF);
    CHECK_NEAR(predBigBefore, predBigAfter, 0.0001f, "big-pool prediction preserved after load");
    CHECK_NEAR(predLittleBefore, predLittleAfter, 0.0001f, "little-pool prediction preserved after load");
    CHECK(predBigAfter > predLittleAfter, "game thread scores higher on big pool");
    unlink(modelPath);
}

static void test_hybrid_integration() {
    printf("\n=== Integration: Hybrid Learning Fine-Tune ===\n");
    HybridLearner hl;
    hl.init(3);
    hl.setEnabled(true);
    CHECK(hl.isEnabled() == true, "hybrid learner enabled");
    std::vector<ThreadCpuSnapshot> snaps;
    ThreadCpuSnapshot s;
    s.tid = 100; s.cpu = 7; s.pool = 2; s.runTimeNs = 1000000; s.timestamp = 0;
    snaps.push_back(s);
    std::vector<uint64_t> cpuRuntime = {0, 0, 0, 0, 0, 0, 0, 1000000};
    hl.tick(snaps, cpuRuntime, 8);
    CHECK(hl.getPoolCount() == 3, "hybrid pool count = 3");
    float features[15] = {0.1f, 0.1f, 0.5f, 0.5f, 0.0f, 1.0f, 0.3f, 0.5f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
    float adj = hl.getFineTuneAdjustment(2, features);
    CHECK(adj >= -1.0f && adj <= 1.0f, "fine-tune adjustment in valid range");
    const char* hyPath = "/tmp/aatpe_integ_hybrid.bin";
    CHECK(hl.save(hyPath), "hybrid save succeeds");
    HybridLearner hl2;
    hl2.init(3);
    CHECK(hl2.load(hyPath), "hybrid load succeeds");
    CHECK(hl2.getTotalUpdates() == hl.getTotalUpdates(), "hybrid update count preserved");
    unlink(hyPath);
}

static void test_federated_integration() {
    printf("\n=== Integration: Federated Learning Export/Import ===\n");
    const char* exchangeDir = "/tmp/aatpe_fed_exchange";
    mkdir(exchangeDir, 0755);
    // point PSK at a writable tmp path so HMAC .sig is generated
    const char* pskPath = "/tmp/aatpe_fed_test_psk.bin";
    unlink(pskPath);
    setenv("AATPE_PSK_PATH", pskPath, 1);
    aatpe_federated::FederatedLearner fed;
    fed.init(exchangeDir);
    std::vector<float> weights(15, 0.1f);
    weights[0] = 0.5f;
    fed.setLocalModel(weights, 0.3f, 100);
    fed.exportLocalModel();
    int fileCount = 0;
    DIR* d = opendir(exchangeDir);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] != '.') fileCount++;
        }
        closedir(d);
    }
    CHECK(fileCount > 0, "federated export created files");
    // verify .sig file exists and is non-empty
    std::string sigPath = std::string(exchangeDir) + "/local_model.bin.sig";
    struct stat sigSt;
    CHECK(stat(sigPath.c_str(), &sigSt) == 0, "HMAC .sig file exists after export");
    if (stat(sigPath.c_str(), &sigSt) == 0) {
        CHECK(sigSt.st_size > 0, "HMAC .sig file is non-empty");
        CHECK(sigSt.st_size == 64, "HMAC .sig file is exactly 64 hex chars");
    }
    // Verify .sig content is valid hex
    {
        FILE* sf = fopen(sigPath.c_str(), "r");
        if (sf) {
            char buf[128] = {0};
            size_t rd = fread(buf, 1, 127, sf);
            fclose(sf);
            bool allHex = (rd == 64);
            for (size_t i = 0; i < rd && allHex; i++) {
                char c = buf[i];
                allHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }
            CHECK(allHex, "HMAC .sig contains valid hex characters");
        }
    }
    // computeGradients() was dead production code and has
    // been removed. This block used to exercise it; exportModel/importModel
    // below still cover the real federated wire format.
    std::string exported = aatpe_federated::exportModel(weights, 0.3f, 100);
    CHECK(!exported.empty(), "exportModel produces non-empty string");
    std::vector<float> impWeights;
    float impAlpha;
    int impSamples;
    CHECK(aatpe_federated::importModel(exported, impWeights, impAlpha, impSamples),
          "importModel parses exported data");
    CHECK(impWeights.size() == weights.size(), "imported weight count matches");
    CHECK_NEAR(impAlpha, 0.3f, 0.001f, "imported alpha matches");
    CHECK(impSamples == 100, "imported sample count matches");
    unlink(pskPath);  // cleanup test PSK
    std::string rmCmd = "rm -rf ";
    rmCmd += exchangeDir;
    int _rc = system(rmCmd.c_str()); (void)_rc;
}

static void test_combined_scoring() {
    printf("\n=== Integration: ML + Hybrid Combined Scoring ===\n");
    LearningModel model;
    model.init(3);
    model.setInitialLR(0.1f);
    ThreadFeatures f = makeFeatures(30000.0f, 60.0f, true);
    for (int i = 0; i < 20; i++) model.update(2, f, 1.0f);
    HybridLearner hl;
    hl.init(3);
    hl.setEnabled(true);
    float feats[15];
    feats[0] = f.avg_run_us; feats[1] = f.nr_switches; feats[2] = f.cpu_usage;
    feats[3] = f.priority; feats[4] = f.nice; feats[5] = f.is_game;
    feats[6] = f.thermal; feats[7] = f.capacity; feats[8] = f.current_cpu_match;
    feats[9] = f.migration_count; feats[10] = f.cross_run_cap;
    feats[11] = f.cross_run_usage; feats[12] = f.cross_cap_thermal;
    feats[13] = f.cross_run_thermal; feats[14] = 0.0f;
    float baseScore = model.predict(2, f);
    float hybridAdj = hl.getFineTuneAdjustment(2, feats);
    float combined = baseScore + hybridAdj * 0.2f;
    CHECK(baseScore >= 0.0f && baseScore <= 1.0f, "base ML score in [0,1]");
    CHECK(hybridAdj >= -1.0f && hybridAdj <= 1.0f, "hybrid adjustment in [-1,1]");
    CHECK(combined >= -0.2f && combined <= 1.2f, "combined score in valid range");
    CHECK(model.getAlpha() >= 0.0f && model.getAlpha() <= 0.401f,
          "alpha within max_alpha bound (0.4)");
}

static void test_model_staleness() {
    printf("\n=== Integration: Model Staleness Detection ===\n");
    LearningModel model;
    model.init(3);
    model.setTopologyFingerprint("fp_v1_8core");
    CHECK(model.isModelStale() == false, "model not stale with initial fingerprint");
    model.setTopologyFingerprint("fp_v2_8core");
    CHECK(model.isModelStale() == true, "model stale after topology fingerprint change");
    model.markModelStale();
    model.setTopologyFingerprint("fp_v2_8core");
    CHECK(model.isModelStale() == true, "model remains stale after same fingerprint set");
}

// P0-fix: Thread role classification integration test.
// Mirrors thread_scan.cpp matchPattern() + classifyThread() logic with an
// isolated rule set, verifying end-to-end role assignment for key threads.
enum IntegThreadRole {
    IROLE_S0_SYSTEM = 0,
    IROLE_S1_UI = 1,
    IROLE_S2_RENDER = 2,
    IROLE_S3_GAME = 3,
    IROLE_S4_THROUGHPUT = 4,
    IROLE_S5_BACKGROUND = 5
};
struct IntegThreadRule {
    std::string procPattern;
    std::string threadPattern;
    IntegThreadRole role;
    std::string subtype;
    int priority;
};
static bool integMatchPattern(const std::string& text, const std::string& pattern) {
    if (pattern == "*") return true;
    if (!pattern.empty() && pattern[0] == '=') return text == pattern.substr(1);
    if (!pattern.empty() && pattern[0] == '^') {
        std::string prefix = pattern.substr(1);
        if (text.size() < prefix.size()) return false;
        return text.compare(0, prefix.size(), prefix) == 0;
    }
    if (!pattern.empty() && pattern.back() == '$') {
        std::string suffix = pattern.substr(0, pattern.size() - 1);
        if (text.size() < suffix.size()) return false;
        return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    size_t star = pattern.find('*');
    if (star != std::string::npos) {
        std::vector<std::string> segs;
        size_t start = 0;
        while (start <= pattern.size()) {
            size_t pos = pattern.find('*', start);
            if (pos == std::string::npos) { segs.push_back(pattern.substr(start)); break; }
            segs.push_back(pattern.substr(start, pos - start));
            start = pos + 1;
        }
        size_t textPos = 0;
        for (size_t i = 0; i < segs.size(); i++) {
            const std::string& seg = segs[i];
            if (seg.empty()) continue;
            size_t found = text.find(seg, textPos);
            if (found == std::string::npos) return false;
            if (i == 0 && pattern[0] != '*' && found != 0) return false;
            if (i == segs.size() - 1 && pattern.back() != '*' && found + seg.size() != text.size()) return false;
            textPos = found + seg.size();
        }
        return true;
    }
    return text.find(pattern) != std::string::npos;
}
static IntegThreadRole integClassifyThread(const std::string& tname, const std::string& pname,
                                           const std::vector<IntegThreadRule>& rules,
                                           int& outPriority, std::string& outSubtype) {
    for (const auto& r : rules) {
        if (integMatchPattern(pname, r.procPattern) && integMatchPattern(tname, r.threadPattern)) {
            outPriority = r.priority;
            outSubtype = r.subtype;
            return r.role;
        }
    }
    outPriority = 50;
    outSubtype = "unknown";
    return IROLE_S5_BACKGROUND;
}

static void test_thread_role_classification() {
    printf("\n=== Integration: Thread Role Classification (P0-fix) ===\n");
    // Load 3 test rules (same format as production thread_roles.conf)
    std::vector<IntegThreadRule> rules = {
        {"surfaceflinger", "*", IROLE_S0_SYSTEM, "SYSTEM", 99},
        {"*", "main", IROLE_S1_UI, "UI_MAIN", 90},
        {"*", "RenderThread", IROLE_S2_RENDER, "RENDER", 88},
        {"*", "UnityMain", IROLE_S3_GAME, "GAME_MAIN", 88},
    };
    CHECK(rules.size() == 4, "test rule set has 4 rules");
    int prio = 0;
    std::string subtype;
    // UnityMain -> S3_GAME
    IntegThreadRole r1 = integClassifyThread("UnityMain", "com.example.game", rules, prio, subtype);
    CHECK(r1 == IROLE_S3_GAME, "UnityMain classified as ROLE_S3_GAME");
    CHECK(prio == 88, "UnityMain priority=88");
    CHECK(subtype == "GAME_MAIN", "UnityMain subtype=GAME_MAIN");
    // RenderThread -> S2_RENDER
    IntegThreadRole r2 = integClassifyThread("RenderThread", "com.example.app", rules, prio, subtype);
    CHECK(r2 == IROLE_S2_RENDER, "RenderThread classified as ROLE_S2_RENDER");
    CHECK(prio == 88, "RenderThread priority=88");
    CHECK(subtype == "RENDER", "RenderThread subtype=RENDER");
    // surfaceflinger process (any thread) -> S0_SYSTEM
    IntegThreadRole r3 = integClassifyThread("any_thread", "surfaceflinger", rules, prio, subtype);
    CHECK(r3 == IROLE_S0_SYSTEM, "surfaceflinger process classified as ROLE_S0_SYSTEM");
    CHECK(prio == 99, "surfaceflinger priority=99");
    CHECK(subtype == "SYSTEM", "surfaceflinger subtype=SYSTEM");
    // main thread -> S1_UI
    IntegThreadRole r4 = integClassifyThread("main", "com.example.app", rules, prio, subtype);
    CHECK(r4 == IROLE_S1_UI, "main thread classified as ROLE_S1_UI");
    // unknown thread -> S5_BACKGROUND (fallback)
    IntegThreadRole r5 = integClassifyThread("RandomWorker", "com.example.app", rules, prio, subtype);
    CHECK(r5 == IROLE_S5_BACKGROUND, "unknown thread falls back to ROLE_S5_BACKGROUND");
    // Wildcard matching: Binder_* should match if rule existed; verify multi-star glob
    CHECK(integMatchPattern("Binder_1", "Binder_*"), "glob 'Binder_*' matches 'Binder_1'");
    CHECK(integMatchPattern("kworker/0:1", "kworker*"), "glob 'kworker*' matches 'kworker/0:1'");
    CHECK(!integMatchPattern("RenderThread", "Unity*"), "glob 'Unity*' does not match 'RenderThread'");
    // Exact match with '=' prefix
    CHECK(integMatchPattern("main", "=main"), "exact '=main' matches 'main'");
    CHECK(!integMatchPattern("mainloop", "=main"), "exact '=main' does not match 'mainloop'");
}

int main() {
    printf("AATPE Integration Test Suite\n");
    printf("========================================\n");
    test_full_ml_pipeline();
    test_hybrid_integration();
    test_federated_integration();
    test_combined_scoring();
    test_model_staleness();
    test_thread_role_classification();
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail > 0) { printf("SOME TESTS FAILED\n"); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}

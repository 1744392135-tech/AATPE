// test_thread_scan.cpp - v94 hermetic unit tests for thread_scan.cpp
// Focuses on the public pure matcher matchPattern() (glob/exact/prefix/suffix).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include "aatped.h"
#include "aatpe-constants.h"
#include "aatped_threadutil.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== ThreadScan: '*' matches everything ===\n");
    CHECK(matchPattern("anything", "*") == true, "'*' wildcard matches all");

    printf("=== ThreadScan: exact match '=' ===\n");
    CHECK(matchPattern("RenderThread", "=RenderThread") == true, "exact = match");
    CHECK(matchPattern("RenderThrea", "=RenderThread") == false, "exact no match");

    printf("=== ThreadScan: prefix match '^' ===\n");
    CHECK(matchPattern("RenderThread", "^Render") == true, "prefix ^Render");
    CHECK(matchPattern("Render", "^RenderThread") == false, "prefix longer than text");

    printf("=== ThreadScan: suffix match '$' ===\n");
    CHECK(matchPattern("main", "main$") == true, "suffix main$");
    CHECK(matchPattern("background", "main$") == false, "suffix no match");

    printf("=== ThreadScan: multi-star glob ===\n");
    CHECK(matchPattern("SurfaceTextureRender", "*Texture*") == true, "*Texture*");
    CHECK(matchPattern("aXbYc", "a*b*c") == true, "a*b*c in order");
    CHECK(matchPattern("aXc", "a*b*c") == false, "a*b*c missing b");
    CHECK(matchPattern("Thread", "Thread*") == true, "leading-fixed trailing-star");

    printf("=== ThreadScan: plain substring ===\n");
    CHECK(matchPattern("GlueThread", "Thread") == true, "plain substring");
    CHECK(matchPattern("xyz", "Thread") == false, "plain no substring");

    printf("=== G5-003: classification threshold ordering contract ===\n");
    // The role classifier must evaluate the high-util-worker -> GAME branch BEFORE
    // the generic UI branch. That is only correct while
    //   kUiAvgRunUs < kGameAvgRunUs < kThroughputAvgRunUs.
    // Guard this invariant so a future constant edit cannot silently re-introduce
    // the old misclassification (2-5ms high-switch unnamed worker -> UI).
    CHECK(aatpe::thread_classify::kUiAvgRunUs < aatpe::thread_classify::kGameAvgRunUs, "kUiAvgRunUs < kGameAvgRunUs");
    CHECK(aatpe::thread_classify::kGameAvgRunUs < aatpe::thread_classify::kThroughputAvgRunUs, "kGameAvgRunUs < kThroughputAvgRunUs");
    CHECK(aatpe::thread_classify::kUiMinSwitches > 0, "kUiMinSwitches positive");

    printf("=== ROUND-005: comm UTF-8 boundary repair (kernel 15-byte truncation) ===\n");
    extern std::string threadScanSanitizeCommForTest(const std::string& in);
    CHECK(threadScanSanitizeCommForTest("RenderThread") == "RenderThread", "ascii name untouched");
    CHECK(threadScanSanitizeCommForTest("kworker/u8:2") == "kworker/u8:2", "kworker/colon preserved");
    {
        std::string good; good += (char)0xE4; good += (char)0xB8; good += (char)0xAD;  // 中
        CHECK(threadScanSanitizeCommForTest(good) == good, "complete UTF-8 char preserved");
    }
    {
        std::string cut; cut += (char)0xE4; cut += (char)0xB8;  // truncated 中
        std::string out = threadScanSanitizeCommForTest(cut);
        bool valid = true;
        for (unsigned char c : out) if (c >= 0x80) valid = false;
        CHECK(out.empty() || valid, "truncated 中 -> invalid tail stripped, no orphan byte");
    }
    {
        std::string s = "Game"; s += (char)0xE4; s += (char)0xB8;  // cut mid-中
        CHECK(threadScanSanitizeCommForTest(s) == "Game", "ascii prefix kept, truncated tail stripped");
    }
    CHECK(threadScanSanitizeCommForTest("").empty(), "empty stays empty");

    printf("\n=== ROUND-012: Flutter engine + WebView thread recognition ===\n");
    {
        int pri; std::string sub; int conf;
        ThreadRole r1 = classifyThread(1, 1001, "1.ui", "com.x", pri, sub, &conf, 0);
        CHECK(r1 == ROLE_S1_UI, "Flutter: '1.ui' -> S1_UI");
        ThreadRole r2 = classifyThread(1, 1002, "com.x.ui", "com.x", pri, sub, &conf, 0);
        // ROUND-014 P3: tight regex ^[0-9]+.type$ — "com.x.ui" is NOT a flutter thread.
        CHECK(r2 != ROLE_S1_UI, "ROUND-014 P3: 'com.x.ui' NOT S1 (tightened)");
        ThreadRole r3 = classifyThread(1, 1003, "1.platform", "com.x", pri, sub, &conf, 0);
        CHECK(r3 == ROLE_S1_UI, "Flutter: '1.platform' -> S1_UI");
        ThreadRole r4 = classifyThread(1, 1004, "1.raster", "com.x", pri, sub, &conf, 0);
        CHECK(r4 == ROLE_S2_RENDER, "Flutter: '1.raster' -> S2_RENDER");
        ThreadRole r5 = classifyThread(1, 1005, "2.raster", "com.x", pri, sub, &conf, 0);
        CHECK(r5 == ROLE_S2_RENDER, "Flutter: '2.raster' -> S2_RENDER");
        ThreadRole r6 = classifyThread(1, 1006, "1.gpu", "com.x", pri, sub, &conf, 0);
        CHECK(r6 == ROLE_S2_RENDER, "Flutter: '1.gpu' -> S2_RENDER");
        ThreadRole r7 = classifyThread(1, 1007, "1.io", "com.x", pri, sub, &conf, 0);
        CHECK(r7 != ROLE_S1_UI && r7 != ROLE_S2_RENDER, "Flutter: '1.io' not S1/S2");
        ThreadRole r8 = classifyThread(1, 1008, "CrRendererMain", "com.x", pri, sub, &conf, 0);
        CHECK(r8 == ROLE_S1_UI, "WebView: 'CrRendererMain' -> S1_UI");
        ThreadRole r9 = classifyThread(1, 1009, "mtkPowerAIDL", "com.x", pri, sub, &conf, 0);
        CHECK(r9 != ROLE_S3_GAME, "WebView: 'mtkPowerAIDL' not S3_GAME");
    }

    // ROUND-014 P3: G4 unified threadutil truth table (38 samples, key regressions).
    printf("=== ThreadUtil: Flutter tight regex ^[0-9]+.(ui|raster|gpu|platform)$ ===\n");
    using namespace aatpe;
    CHECK(isFlutterEngineThread("1.ui")==true,  "1.ui -> flutter UI");
    CHECK(isFlutterEngineThread("1.raster")==true, "1.raster -> flutter raster");
    CHECK(isFlutterEngineThread("1.gpu")==true,  "1.gpu -> flutter gpu");
    CHECK(isFlutterEngineThread("1.platform")==true,"1.platform -> flutter platform");
    CHECK(isFlutterEngineThread("2.ui")==true,  "2.ui multi-engine instance");
    CHECK(isFlutterEngineThread("2.raster")==true,"2.raster multi-engine");
    CHECK(isFlutterEngineThread("1.io")==false, "1.io NOT promoted (IO thread)");
    CHECK(isFlutterEngineThread("AppDebug.ui")==false, "AppDebug.ui NOT S1 (tight regex)");
    CHECK(isFlutterEngineThread("networker.raster")==false, "networker.raster NOT S2");

    printf("=== ThreadUtil: render whitelist (incl. truncated comm) ===\n");
    CHECK(isRenderWhitelisted("RenderThread")==true, "RenderThread whitelisted");
    CHECK(isRenderWhitelisted("CrRendererMain")==true, "CrRendererMain whitelisted");
    CHECK(isRenderWhitelisted("CrGpuMain")==true, "CrGpuMain whitelisted (GPU main)");
    CHECK(isRenderWhitelisted("VizCompositorTh")==true, "VizCompositorTh (truncated) whitelisted");
    CHECK(isRenderWhitelisted("UnityGfxDeviceW")==true, "UnityGfxDeviceW (truncated prefix) whitelisted");
    CHECK(isRenderWhitelisted("FinalizerDaemon")==false, "FinalizerDaemon NOT render");

    printf("=== ThreadUtil: blacklisted consumers not boosted ===\n");
    CHECK(isBlacklistedConsumerThread("FinalizerDaemon")==true, "FinalizerDaemon blacklisted");
    CHECK(isBlacklistedConsumerThread("HeapTaskDaemon")==true, "HeapTaskDaemon blacklisted");
    CHECK(isBlacklistedConsumerThread("ReferenceQueueDaemon")==true, "ReferenceQueueDaemon blacklisted");

    printf("=== CFG-binder: loadThreadRules multi-colon tolerance ===\n");
    {
        // Point CONF_DIR at a temp tree and write a rules/thread_roles.conf.
        char tmp[] = "/tmp/aatpe_tscan_XXXXXX";
        char* t = mkdtemp(tmp);
        std::string base = t;
        mkdir((base + "/rules").c_str(), 0755);
        extern const char* CONF_DIR;
        CONF_DIR = base.c_str();
        {
            std::ofstream rf(base + "/rules/thread_roles.conf");
            rf << "# test\n";
            rf << "*:binder:*:S0:BINDER:95\n";        // malformed 6-field (the bug)
            rf << "*:RenderThread:S2:RENDER:88\n";       // normal 5-field
            rf << "survive:me\n";                        // too short (<5 fields) -> skip
            rf << "one:two:three:four\n";                // 4 fields -> skip
            rf << "surfaceflinger:*:S0:SYSTEM:99\n";     // normal wildcard
        }
        extern std::vector<ThreadRule> g_rules;
        loadThreadRules();
        // Find the binder rule (threadPattern should recover "binder*").
        bool binderOk = false, binderPriOk = false, binderSubOk = false;
        bool normalOk = false, normalPriOk = false;
        bool wildcardOk = false;
        for (const auto& r : g_rules) {
            if (r.threadPattern == "binder*") {
                binderOk = (r.role == ROLE_S0_SYSTEM);
                binderPriOk = (r.priority == 95);
                binderSubOk = (r.subtype == "BINDER");
            }
            if (r.threadPattern == "RenderThread" && r.procPattern == "*") {
                normalOk = (r.role == ROLE_S2_RENDER);
                normalPriOk = (r.priority == 88);
            }
            if (r.procPattern == "surfaceflinger" && r.threadPattern == "*") {
                wildcardOk = (r.role == ROLE_S0_SYSTEM);
            }
        }
        CHECK(binderOk,    "CFG-binder: 6-field line -> role S0 (not S5)");
        CHECK(binderPriOk, "CFG-binder: 6-field line -> priority 95 (no stoi throw)");
        CHECK(binderSubOk, "CFG-binder: 6-field line -> subtype BINDER (not 'S0')");
        CHECK(normalOk,    "CFG-binder: normal 5-field S2 still parses");
        CHECK(normalPriOk, "CFG-binder: normal 5-field priority 88 preserved");
        CHECK(wildcardOk,  "CFG-binder: proc=surfaceflinger thread=* wildcard parses");
        CHECK(g_rules.size() == 3, "CFG-binder: short/bad lines skipped (3 valid rules)");
    }

    printf("\n========================================\n"
           "ThreadScan results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

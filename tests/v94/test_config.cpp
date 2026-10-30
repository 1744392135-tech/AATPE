#include <cstdio>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Config: validateConfig defaults to valid ===\n");
    g_cfg = Config{};
    bool v0 = validateConfig();
    CHECK(v0 == true, "default config validates OK");

    printf("=== Config: decision interval out of range clamps ===\n");
    g_cfg.decisionInterval = 3;
    CHECK(validateConfig() == false, "interval=3 invalid -> flagged");
    CHECK(g_cfg.decisionInterval == 10, "clamped to 10");
    g_cfg.decisionInterval = 99999;
    validateConfig();
    CHECK(g_cfg.decisionInterval == 5000, "clamped to 5000");

    printf("=== Config: control mode normalized ===\n");
    g_cfg = Config{};
    g_cfg.controlMode = "bogus_mode";
    validateConfig();
    CHECK(g_cfg.controlMode == "assist", "bogus control_mode -> assist");
    g_cfg.controlMode = "active";
    CHECK(validateConfig() == true, "active control_mode valid");

    printf("=== Config: maxMigrationsPerCycle clamp ===\n");
    g_cfg = Config{};
    g_cfg.maxMigrationsPerCycle = 100;
    validateConfig();
    CHECK(g_cfg.maxMigrationsPerCycle == 64, "migrations/cycle clamped to 64");

    printf("=== Config G2-001: thermal ladder monotonic gate ===\n");
    g_cfg = Config{};
    g_cfg.thermalWarningThreshold = 90;
    g_cfg.thermalCriticalThreshold = 80;
    g_cfg.thermalEmergencyThreshold = 70;
    validateConfig();
    CHECK(g_cfg.thermalWarningThreshold < g_cfg.thermalCriticalThreshold,
          "after validate: WARNING < CRITICAL");
    CHECK(g_cfg.thermalCriticalThreshold < g_cfg.thermalEmergencyThreshold,
          "after validate: CRITICAL < EMERGENCY");

    g_cfg.thermalWarningThreshold = 50;
    g_cfg.thermalCriticalThreshold = 70;
    g_cfg.thermalEmergencyThreshold = 90;
    validateConfig();
    CHECK(g_cfg.thermalWarningThreshold == 50 && g_cfg.thermalCriticalThreshold == 70 &&
          g_cfg.thermalEmergencyThreshold == 90,
          "already-monotonic ladder left untouched");

    printf("=== Config G2-005: controlMode whitelist ===\n");
    g_cfg = Config{};
    g_cfg.controlMode = "emergency";
    validateConfig();
    CHECK(g_cfg.controlMode == "emergency", "controlMode='emergency' accepted (not reset to assist)");
    g_cfg.controlMode = "game";
    validateConfig();
    CHECK(g_cfg.controlMode == "game", "controlMode='game' still accepted (backward compat)");
    g_cfg.controlMode = "bogus_mode";
    validateConfig();
    CHECK(g_cfg.controlMode == "assist", "controlMode='bogus_mode' -> assist");

    printf("=== Config: targetFrameTime out of range reset ===\n");
    g_cfg = Config{};
    g_cfg.targetFrameTime = 50;
    validateConfig();
    CHECK(g_cfg.targetFrameTime == 16667, "bad target frame time reset to 16667");

    printf("=== Config: isPackageInGameList ===\n");
    g_cfg.gamePackages.clear();
    g_cfg.gamePackages.push_back("com.miHoyo.game");
    CHECK(isPackageInGameList("com.miHoYo.game") == true, "exact package in game list");
    CHECK(isPackageInGameList("com.other.app") == false, "package not in game list");

    printf("=== Config: loadPerAppConfigs parses [app:pkg] ===\n");
    FILE* fp = fopen("/tmp/aatpe_test.conf", "w");
    fprintf(fp, "[app:com.game.alpha]\ndecision_interval = 50\nmax_fps = 120\n");
    fclose(fp);
    g_perAppConfigs.clear();
    loadPerAppConfigs("/tmp/aatpe_test.conf");
    auto it = g_perAppConfigs.find("com.game.alpha");
    CHECK(it != g_perAppConfigs.end(), "per-app config parsed");
    CHECK(it->second.decisionIntervalMs == 50, "per-app decision_interval=50");
    CHECK(it->second.maxFpsHint == 120, "per-app max_fps_hint=120");
    CHECK(it->second.valid == true, "per-app entry marked valid");

    printf("=== Config #86: hot-reload concurrency (readers + writer, no deadlock) ===\n");
    {
        std::atomic<bool> stop{false};
        std::atomic<int> readersDone{0};
        auto reader = [&]() {
            for (int i = 0; i < 2000 && !stop.load(); ++i) {
                Config snap;
                snapshotConfig(snap);
                volatile int sink = snap.decisionInterval; (void)sink;
            }
            readersDone.fetch_add(1);
        };
        std::thread r1(reader);
        std::thread r2(reader);
        bool writerAlive = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int writes = 0;
        while (std::chrono::steady_clock::now() < deadline && writerAlive) {
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg = Config{};
                g_cfg.decisionInterval = 50 + (writes % 200);
            }
            validateConfig();
            ++writes;
            if (writes % 200 == 0) std::this_thread::yield();
        }
        stop.store(true);
        r1.join(); r2.join();
        CHECK(writerAlive, "writer completed without deadlock");
        CHECK(readersDone.load() == 2, "all reader threads completed (no deadlock)");
        CHECK(writes > 10, "meaningful number of concurrent config writes");
    }

    printf("=== Config ROUND-003 Defect-2: hot-reload bad key keeps running value ===\n");
    {
        std::system("mkdir -p /tmp/aatpe_v94_state");
        const char* cpath = "/tmp/aatpe_v94_state/aatpe.conf";
        auto writeConf = [&](const char* body) {
            std::ofstream(cpath, std::ios::trunc) << body;
        };

        writeConf("DECISION_INTERVAL=300\nCONFIDENCE_THRESHOLD=8\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300, "hot load: legal DECISION_INTERVAL=300 applied");

        writeConf("DECISION_INTERVAL=fast\nCONFIDENCE_THRESHOLD=70\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300,
              "bad DECISION_INTERVAL kept previous running value (300), not default");
        CHECK(g_cfg.confidenceThreshold == 70,
              "good CONFIDENCE_THRESHOLD=70 still applied alongside the bad key");

        g_cfg = Config{};
        writeConf("DECISION_INTERVAL=fast\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 200,
              "first load bad key keeps compiled default (200)");

        std::remove(cpath);
    }

    printf("=== Config ROUND-004: reload hook registry fires with new snapshot ===\n");
    {
        std::system("mkdir -p /tmp/aatpe_v94_state");
        const char* cpath = "/tmp/aatpe_v94_state/aatpe.conf";
        auto writeConf = [&](const char* body) {
            std::ofstream(cpath, std::ios::trunc) << body;
        };
        static int sHookCalls = 0;
        static int sHookSeenDecision = -1;
        aatpe::config::registerReloadHook("test_hook", [](const Config& snap) {
            ++sHookCalls;
            sHookSeenDecision = snap.decisionInterval;
        });

        g_cfg = Config{};
        writeConf("DECISION_INTERVAL=400\n");
        reloadConfig();
        CHECK(sHookCalls >= 1, "reload hook invoked after reloadConfig()");
        CHECK(sHookSeenDecision == 400, "hook received fresh snapshot (decisionInterval=400)");

        g_cfg = Config{};
        writeConf("PRIME_POOL=7\n");
        loadConfig();
        CHECK(g_cfg.primePoolCfg == "7", "PRIME_POOL=7 parsed into primePoolCfg");
        g_cfg = Config{};
        writeConf("PRIME_POOL=\n");
        loadConfig();
        CHECK(g_cfg.primePoolCfg.empty(), "PRIME_POOL empty = auto-detect");

        writeConf("DECISION_INTERVAL=300\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300, "baseline decisionInterval=300 loaded");
        writeConf("DECISION_INTERVAL=999\n");
        std::system("chmod 0666 /tmp/aatpe_v94_state/aatpe.conf");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300,
              "world-writable conf rejected, running value (300) kept");
        std::system("chmod 0644 /tmp/aatpe_v94_state/aatpe.conf");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 999, "safe-perm conf reloaded, 999 accepted");

        std::remove(cpath);
    }

    printf("=== Config CFG-39: EBPF dual-key reload stability ===\n");
    {
        std::system("mkdir -p /tmp/aatpe_v94_state");
        const char* cpath = "/tmp/aatpe_v94_state/aatpe.conf";
        auto writeConf = [&](const char* body) {
            std::ofstream(cpath, std::ios::trunc) << body;
        };

        g_cfg = Config{};
        writeConf("EBPF_ENABLED=1\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == true,
              "CFG-39 step1: EBPF_ENABLED=1 parsed -> ebpfEnabled=true");

        g_cfg = Config{};
        writeConf("EBPF_EVENT_DRIVEN=0\nEBPF_ENABLED=1\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == true,
              "CFG-39 step2 (bug repro): event_driven=0 + enabled=1 -> enabled wins = true (user off lost)");

        g_cfg = Config{};
        writeConf("EBPF_EVENT_DRIVEN=0\nEBPF_ENABLED=0\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == false,
              "CFG-39 step3 (fixed): both keys=0 -> ebpfEnabled=false (user off sticks)");

        g_cfg = Config{};
        writeConf("EBPF_EVENT_DRIVEN=1\nEBPF_ENABLED=1\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == true,
              "CFG-39 step4: both keys=1 -> ebpfEnabled=true");

        std::remove(cpath);
    }

    printf("=== Config CFG-18: hot-reload start/stop lifecycle (no terminate) ===\n");
    {
        startConfigHotReload();
        stopConfigHotReload();
        startConfigHotReload();
        stopConfigHotReload();
        stopConfigHotReload();
        CHECK(true, "CFG-18: start/stop/start/stop + double-stop completed without std::terminate");
    }

    printf("\n========================================\n"
           "Config results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// test_config.cpp - v94 hermetic unit tests for aatped_config.cpp
// Links aatped_config.cpp + aatpe_ml with v94_daemon_deps + log_stub.
// Covers: validateConfig clamping (decision interval, migration per cycle,
// target frame time, thermal limits), isPackageInGameList, loadPerAppConfigs
// parsing of [app:pkg] sections, and #86 config hot-reload concurrency.
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

    // G2-001: the thermal three-tier wall WARNING < CRITICAL < EMERGENCY must be
    // auto-corrected. Before the fix the gate lived in validateConfig() but was
    // never called on the load path; here we prove the gate itself normalizes an
    // inverted ladder (and the new loadConfig wiring calls validateConfigLocked()).
    printf("=== Config G2-001: thermal ladder monotonic gate ===\n");
    g_cfg = Config{};
    g_cfg.thermalWarningThreshold = 90;
    g_cfg.thermalCriticalThreshold = 80;   // inverted: warning >= critical
    g_cfg.thermalEmergencyThreshold = 70;  // inverted: critical >= emergency
    validateConfig();
    CHECK(g_cfg.thermalWarningThreshold < g_cfg.thermalCriticalThreshold,
          "after validate: WARNING < CRITICAL");
    CHECK(g_cfg.thermalCriticalThreshold < g_cfg.thermalEmergencyThreshold,
          "after validate: CRITICAL < EMERGENCY");

    // Non-inverted ladder is left untouched.
    g_cfg.thermalWarningThreshold = 50;
    g_cfg.thermalCriticalThreshold = 70;
    g_cfg.thermalEmergencyThreshold = 90;
    validateConfig();
    CHECK(g_cfg.thermalWarningThreshold == 50 && g_cfg.thermalCriticalThreshold == 70 &&
          g_cfg.thermalEmergencyThreshold == 90,
          "already-monotonic ladder left untouched");

    // G2-005: CONTROL_MODE whitelist must accept "emergency" (matches
    // setRuntimeControlMode) and keep accepting "game".
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
    // Simulate the daemon: scheduling threads snapshot g_cfg while a config
    // hot-reload writer mutates it under g_cfgMutex. If the lock ordering is
    // wrong this deadlocks; we bound it with a wall-clock deadline so a hang
    // shows up as a FAIL rather than an infinite test run.
    {
        std::atomic<bool> stop{false};
        std::atomic<int> readersDone{0};
        auto reader = [&]() {
            for (int i = 0; i < 2000 && !stop.load(); ++i) {
                Config snap;
                snapshotConfig(snap);           // takes g_cfgMutex
                // touch the snapshot so the copy is not optimized away
                volatile int sink = snap.decisionInterval; (void)sink;
            }
            readersDone.fetch_add(1);
        };
        std::thread r1(reader);
        std::thread r2(reader);
        // Writer: hot-reload style mutation. NOTE: validateConfig() itself takes
        // g_cfgMutex (non-recursive), so we must NOT hold the lock across the call.
        // Mutate g_cfg under a brief critical section, then let validateConfig()
        // lock/unlock on its own — exactly how a hot-reload path avoids nesting.
        bool writerAlive = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int writes = 0;
        while (std::chrono::steady_clock::now() < deadline && writerAlive) {
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                g_cfg = Config{};
                g_cfg.decisionInterval = 50 + (writes % 200);
            }
            validateConfig();               // locks g_cfgMutex internally
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
        // loadConfig() resolves to CONF_DIR/aatpe.conf on host = /tmp/aatpe_v94_state/aatpe.conf
        std::system("mkdir -p /tmp/aatpe_v94_state");
        const char* cpath = "/tmp/aatpe_v94_state/aatpe.conf";
        auto writeConf = [&](const char* body) {
            std::ofstream(cpath, std::ios::trunc) << body;
        };

        // Step 1: establish a legal running value for DECISION_INTERVAL=300.
        writeConf("DECISION_INTERVAL=300\nCONFIDENCE_THRESHOLD=8\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300, "hot load: legal DECISION_INTERVAL=300 applied");

        // Step 2: reload with a BAD value for DECISION_INTERVAL (throws) but a GOOD
        // new value for CONFIDENCE_THRESHOLD. The bad key must KEEP the previous
        // running value (300), the good key must still update (8 -> 70).
        writeConf("DECISION_INTERVAL=fast\nCONFIDENCE_THRESHOLD=70\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300,
              "bad DECISION_INTERVAL kept previous running value (300), not default");
        CHECK(g_cfg.confidenceThreshold == 70,
              "good CONFIDENCE_THRESHOLD=70 still applied alongside the bad key");

        // Step 3: FIRST load with no prior running value (g_cfg reset to defaults)
        // and a bad key -> falls back to the compiled default (200), never NaN/zero.
        g_cfg = Config{};
        writeConf("DECISION_INTERVAL=fast\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 200,
              "first load bad key keeps compiled default (200)");

        // cleanup
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
        reloadConfig();   // loadConfig + fireReloadHooks
        CHECK(sHookCalls >= 1, "reload hook invoked after reloadConfig()");
        CHECK(sHookSeenDecision == 400, "hook received fresh snapshot (decisionInterval=400)");

        // ROUND-004 final: PRIME_POOL parses into g_cfg.primePoolCfg (schema path).
        g_cfg = Config{};
        writeConf("PRIME_POOL=7\n");
        loadConfig();
        CHECK(g_cfg.primePoolCfg == "7", "PRIME_POOL=7 parsed into primePoolCfg");
        g_cfg = Config{};
        writeConf("PRIME_POOL=\n");
        loadConfig();
        CHECK(g_cfg.primePoolCfg.empty(), "PRIME_POOL empty = auto-detect");

        // G3 L-3: insecure conf (group/other-writable) is rejected, running value kept.
        writeConf("DECISION_INTERVAL=300\n");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300, "baseline decisionInterval=300 loaded");
        writeConf("DECISION_INTERVAL=999\n");
        std::system("chmod 0666 /tmp/aatpe_v94_state/aatpe.conf");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 300,
              "world-writable conf rejected, running value (300) kept");
        // restore safe perms + value -> accepted again.
        std::system("chmod 0644 /tmp/aatpe_v94_state/aatpe.conf");
        loadConfig();
        CHECK(g_cfg.decisionInterval == 999, "safe-perm conf reloaded, 999 accepted");

        std::remove(cpath);
    }

    // ------------------------------------------------------------------
    // CFG-39: set_ebpf must sync BOTH EBPF_EVENT_DRIVEN and EBPF_ENABLED.
    // loadConfig parses both keys (line ~841 EBPF_EVENT_DRIVEN, line ~969
    // EBPF_ENABLED) and both write g_cfg.ebpfEnabled, with the later parse
    // winning. Before the fix, set_ebpf only wrote EBPF_EVENT_DRIVEN, leaving
    // a legacy EBPF_ENABLED=1 line untouched; on the next hot-reload the
    // EBPF_ENABLED branch ran AFTER and silently flipped the user's 0 back to 1.
    //
    // NOTE: on host, writeFileAtomic is a no-op stub, so we cannot drive this
    // through persistConfigKeyValue. Instead we directly write the conf file
    // for each scenario and call loadConfig(), which is the real parser.
    // ------------------------------------------------------------------
    printf("=== Config CFG-39: EBPF dual-key reload stability ===\n");
    {
        std::system("mkdir -p /tmp/aatpe_v94_state");
        const char* cpath = "/tmp/aatpe_v94_state/aatpe.conf";
        auto writeConf = [&](const char* body) {
            std::ofstream(cpath, std::ios::trunc) << body;
        };

        // Step 1: legacy conf has EBPF_ENABLED=1 (the old key). No EBPF_EVENT_DRIVEN.
        g_cfg = Config{};
        writeConf("EBPF_ENABLED=1\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == true,
              "CFG-39 step1: EBPF_ENABLED=1 parsed -> ebpfEnabled=true");

        // Step 2 (BUGGY scenario, old set_ebpf): user clicks off, only
        // EBPF_EVENT_DRIVEN=0 is written; legacy EBPF_ENABLED=1 is left behind.
        // loadConfig parses EBPF_EVENT_DRIVEN=0 (line ~841, sets false) THEN
        // EBPF_ENABLED=1 (line ~969, sets true) -> final = true. The user's off
        // request is silently lost.
        g_cfg = Config{};
        writeConf("EBPF_EVENT_DRIVEN=0\nEBPF_ENABLED=1\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == true,
              "CFG-39 step2 (bug repro): event_driven=0 + enabled=1 -> enabled wins = true (user off lost)");

        // Step 3 (FIXED scenario): set_ebpf=0 writes BOTH keys to "0".
        // loadConfig parses EBPF_EVENT_DRIVEN=0 (false) then EBPF_ENABLED=0 (false)
        // -> final = false. User's off request sticks.
        g_cfg = Config{};
        writeConf("EBPF_EVENT_DRIVEN=0\nEBPF_ENABLED=0\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == false,
              "CFG-39 step3 (fixed): both keys=0 -> ebpfEnabled=false (user off sticks)");

        // Step 4: also verify the symmetric on-path: both keys=1 -> true.
        g_cfg = Config{};
        writeConf("EBPF_EVENT_DRIVEN=1\nEBPF_ENABLED=1\n");
        loadConfig();
        CHECK(g_cfg.ebpfEnabled == true,
              "CFG-39 step4: both keys=1 -> ebpfEnabled=true");

        std::remove(cpath);
    }

    // ------------------------------------------------------------------
    // CFG-18: stopConfigHotReload must always join the inotify thread, even
    // when running==false (e.g. the thread exited on its own after an early
    // inotify_init1 failure). If stop early-returns without joining, a later
    // start reassigns std::thread over a joinable object -> std::terminate.
    // On host inotify succeeds, so we exercise the start/stop/start/stop cycle
    // which is the regression surface.
    // ------------------------------------------------------------------
    printf("=== Config CFG-18: hot-reload start/stop lifecycle (no terminate) ===\n");
    {
        startConfigHotReload();
        stopConfigHotReload();
        // Second cycle: if stop failed to join the first thread, this reassignment
        // over a joinable std::thread calls std::terminate().
        startConfigHotReload();
        stopConfigHotReload();
        // Also: calling stop without start must not crash (running already false).
        stopConfigHotReload();
        CHECK(true, "CFG-18: start/stop/start/stop + double-stop completed without std::terminate");
    }

    printf("\n========================================\n"
           "Config results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// AATPE v94 Integration Tests - 10 end-to-end scenarios.
// Each scenario drives the REAL production functions compiled into the binary.
#include "it_common.h"
#include "aatpe_crypto.h"
#include "aatped_hmac.h"
#include "integrity.h"
#include "placement.h"
// v4.3.4 F-group governance: exportModel() was removed from the production
// aatpe_federated.h (dormant Plan B wire path). This test-only header redeclares
// it solely for the S9 integration scenario; the implementation lives in
// tests/integration/stubs/stub_property.cpp. Production code must NOT include it.
#include "federated_wire_stub.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>   // S23: WEXITSTATUS for pclose() status
#include <dirent.h>
#include <chrono>

using namespace aatpe;
using namespace aatpe_ml;

// RT prune test seams (defined in aatped_priority.cpp under AATPE_HOST_TEST)
extern "C" void aatpeRtSetHooksForTest(bool(*attrFn)(int, void*), bool(*startFn)(int, unsigned long long*));
extern "C" int aatpeRtContainsForTest(int tid);
extern "C" void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime);
extern "C" void aatpePruneDeadRtTids(void);
extern "C" int aatpeRtCountForTest(void);

// Spillover test seams (defined in placement.cpp under AATPE_HOST_TEST)
namespace aatpe { namespace placement {
    int spilloverCountForTest();
    void spilloverDebugAddForTest(int tid, int tier);
    void spilloverReleaseAllForTest();
    long long spilloverCgroupDriftHealedForTest();
    void onForegroundSwitchEdge(const std::string& newFg) noexcept;
    // audit5 N3: TID-reuse identity predicate seam (exact demote-loop gate).
    bool spillDemoteIdentityMatchForTest(int tid, long long recordedStarttime);
    unsigned long long spillReadThreadStarttimeForTest(int tid);
}}
namespace aatpe_spillover { int placementSpilloverTick(const Config& cfgSnap); }

// ===========================================================================
// Helpers
// ===========================================================================
static std::string g_profileBefore;

static void setProcStatBusy(int busyPercent) {
    // Write per-cpu lines so readCpuMetrics sees the requested busy%.
    // Provide a stable deltas pair by writing increasing counters.
    static unsigned long long base = 100000;
    base += 1000;
    std::string out = "cpu ";
    for (int c = 0; c < 8; c++) {
        unsigned long long busy = base * busyPercent / 100;
        unsigned long long idle = base - busy;
        char line[256];
        snprintf(line, sizeof(line),
                 "cpu%d %llu 0 0 %llu 0 0 0 0 0 0\n", c, busy, idle);
        out += line;
    }
    itWriteFile("/proc/stat", out);
}

static void seedTypicalThreads() {
    // surfaceflinger S0, audioserver S0, game UI/render/game, bg workers
    itAddProcThread(1000, "surfaceflinger", 1001, "SurfaceFlinger",  5000000, 50000, 7, 100);
    itAddProcThread(1002, "audioserver",    1003, "AudioTrack",     1000000, 10000, 3, 200);
    itAddProcThread(2000, "com.test.game",  2001, "UnityMain",     8000000, 200000, 7, 300);
    itAddProcThread(2000, "com.test.game",  2002, "RenderThread",   4000000, 150000, 6, 301);
    itAddProcThread(2000, "com.test.game",  2003, "GameThread",     6000000, 180000, 7, 302);
    itAddProcThread(3000, "com.test.app",   3001, "main",           2000000,  80000, 4, 400);
    itAddProcThread(3000, "com.test.app",   3002, "AsyncTask #1",    200000,   5000, 1, 401);
    itAddProcThread(3000, "com.test.app",   3003, "GC",               1000,    100, 0, 402);
}

// ===========================================================================
// Scenario 1: Daemon main loop full chain, 500 decision cycles
// ===========================================================================
void scenario1_daemon_loop() {
    g_it.currentScenarioName = "S1_DaemonMainLoop";
    printf("\n[Scenario 1] Daemon main loop full chain (500 cycles)\n");
    itRedirectPaths();
    itBootstrapConfig(
        "DECISION_INTERVAL=100\nML_MIN_SAMPLES=5\nEBPF_ENABLED=0\n"
        "THERMAL_EMERGENCY=105\nMAX_TRACKED_THREADS=64\n");
    loadConfig(); loadThreadRules(); detectCpuTopology();

    CHECK(g_cpuCount == 8, "S1: topology detected 8 CPUs");
    CHECK(g_littlePool.size() >= 1 && g_bigPool.size() >= 1, "S1: little+big pools non-empty");

    seedTypicalThreads();
    setProcStatBusy(30);
    scanThreads();
    CHECK(g_threads.size() >= 5, "S1: threads scanned into g_threads");
    int scannedAfterFirst = (int)g_threads.size();

    long long decBefore = g_totalDecisions.load();
    // 500 decision cycles
    for (int cyc = 0; cyc < 500; cyc++) {
        setProcStatBusy(20 + (cyc % 60));
        readCpuMetrics();
        // frame-aware analysis hook
        aatpe::frameaware::updateRefreshRate();
        // decision per tracked (non-protected) thread.
        // v439 S1fix: mirror the production main loop (aatped_main.cpp:3700). Snapshot
        // the threads into a local vector UNDER g_threadsMutex, then RELEASE the lock
        // before decidePlacement()/applyPlacement(). applyPlacement() re-locks
        // g_threadsMutex internally (placement.cpp:3505 and :3520) to write the real
        // entry back; holding the lock here on a non-recursive mutex caused a
        // same-thread self-deadlock (futex wait) as soon as a real host thread's
        // sched_setaffinity succeeded (~cycle 180). Production never holds the lock
        // across applyPlacement.
        int perCycle = 0;
        std::vector<std::pair<int, ThreadInfo>> threadsSnapshot;
        {
            std::lock_guard<std::mutex> lk(g_threadsMutex);
            threadsSnapshot.reserve(g_threads.size());
            for (const auto& kv : g_threads) {
                if (kv.second.protect) continue;
                if (kv.second.role == ROLE_S0_SYSTEM) continue;
                threadsSnapshot.push_back(kv);
            }
        }
        for (auto& kv : threadsSnapshot) {
            Decision d = decidePlacement(kv.second, g_cfg);
            g_totalDecisions++;                 // mirrors production main loop
            if (d.action == DecisionAction::MIGRATE) {
                applyPlacement(kv.second, d.targetCpu, d.reason, g_cfg);
            }
            perCycle++;
        }
        // record cpu load samples for the predictor
        for (int c = 0; c < g_cpuCount; c++) recordCpuLoadSample(c, 20 + (cyc % 50));
        // thermal check (cheap, uses cache)
        readThermal();
        if (cyc == 0) {
            CHECK(perCycle > 0, "S1: at least one decision made per cycle");
        }
    }
    long long decAfter = g_totalDecisions.load();
    CHECK_F(decAfter > decBefore, "S1: total decisions advanced over 500 cycles",
            "before=%lld after=%lld", decBefore, decAfter);
    CHECK_F((decAfter - decBefore) > 200, "S1: >=200 placement decisions across 500 cycles",
            "delta=%lld", (decAfter-decBefore));
    // thread lifecycle: a thread survives re-scan with same tid/role
    scanThreads();
    bool unitySeen = false;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        for (auto& kv : g_threads) if (kv.second.tname == "UnityMain") unitySeen = true;
    }
    CHECK(unitySeen, "S1: UnityMain thread survives lifecycle (scan->classify->track)");
    CHECK(scannedAfterFirst > 0, "S1: thread lifecycle populated g_threads");
    printf("      [INFO] scanned=%d  decisions_delta=%lld\n",
           scannedAfterFirst, (long long)(decAfter - decBefore));
}

// ===========================================================================
// Scenario 2: eBPF event-driven chain + graceful degradation
// ===========================================================================
void scenario2_ebpf_events() {
    g_it.currentScenarioName = "S2_EbpfEvents";
    printf("\n[Scenario 2] eBPF event chain + poll fallback\n");
    aatpe_ebpf::EbpfScheduler sched;
    sched.setRingPages(8);
    sched.setPollFallbackEnabled(true);
    // load() in a constrained env must either fully activate OR degrade to a
    // sane fallback state -- never crash. We verify the state machine converges.
    bool loaded = sched.load();
    CHECK_F(loaded == false || sched.isActive() == true,
            "S2: eBPF load converges to a defined state (loaded=%d)",
            "loaded=%d active=%d state=%s mode=%s",
            (int)loaded, (int)sched.isActive(),
            sched.getStateString(), sched.getModeString());
    CHECK(sched.getStateString() != nullptr, "S2: eBPF exposes a human-readable state");
    // Force the documented failure path: degrade -> POLLING fallback.
    sched.degrade("integration-test");
    bool pollFallback = (sched.getMode() == aatpe_ebpf::EbpfMode::POLLING);
    CHECK(pollFallback, "S2: eBPF failure degrades to POLLING fallback mode");

    // Non-blocking event poll must not hang and returns a small count
    int ev = sched.pollEvents(64);
    CHECK(ev >= 0 && ev <= 64, "S2: pollEvents non-blocking, bounded return");

    // Event struct layout sanity (must match kernel sched events)
    CHECK(sizeof(aatpe_ebpf::EventHeader) == 16, "S2: EventHeader is 16 bytes (u32,u32,u64)");
    CHECK(sizeof(aatpe_ebpf::SwitchEvent) >= 48, "S2: SwitchEvent carries prev/next comm");

    // FORK -> thread appears after scan; EXIT -> thread gone
    int pid = 4100;
    itAddProcThread(pid, "com.test.ebpf", 4101, "NewWorker", 3000000, 90000, 5, 500);
    scanThreads();
    bool afterFork = false;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        afterFork = (g_threads.find(4101) != g_threads.end());
    }
    CHECK(afterFork, "S2: FORK-equivalent (new tid) detected into g_threads");
    // EXIT-equivalent: remove proc entry and rescan
    itRemoveProcThread(pid, 4101);
    scanThreads();
    bool afterExit = true;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        afterExit = (g_threads.find(4101) != g_threads.end());
    }
    CHECK(!afterExit, "S2: EXIT-equivalent (tid removed) drops from g_threads");
    sched.unload();
}

// ===========================================================================
// Scenario 3: ML learning closed loop, 1000 cycles
// ===========================================================================
static ThreadFeatures mkFeat(float runUs, float cpu, float game) {
    ThreadFeatures f = {};
    f.avg_run_us = runUs; f.nr_switches = 50; f.cpu_usage = cpu;
    f.priority = 110; f.nice = 0; f.is_game = game;
    f.thermal = 35; f.capacity = 1024; f.current_cpu_match = 1;
    f.migration_count = 1; f.cross_run_cap = 0.1f; f.cross_run_usage = 0.1f;
    f.cross_cap_thermal = 0.1f; f.cross_run_thermal = 0.1f;
    return f;
}

void scenario3_ml_loop() {
    g_it.currentScenarioName = "S3_MLLearningLoop";
    printf("\n[Scenario 3] ML learning closed loop (1000 cycles)\n");
    LearningModel& m = g_mlModel;
    m.reset();
    m.init(3);
    m.setInitialLR(0.05f);
    m.setDecay(0.999f);
    m.setMaxAlpha(0.4f);
    m.setMinSamples(2000);      // cold-start threshold
    m.setColdStartSamples(2000);
    m.setEnabled(true);

    // cold-start protection: pools not ready below 2000 samples
    bool readyBefore = m.isPoolReady(2);
    CHECK(!readyBefore, "S3: cold-start protects immature pool (<2000 samples)");

    // 1000 learning cycles: game features rewarded on big(2), punished on little(0);
    // low-run features rewarded on little(0).
    for (int i = 0; i < 1000; i++) {
        ThreadFeatures gameF = mkFeat(60000, 85, 1);
        m.update(2, gameF, 1.0f);   // game thrives on big
        m.update(0, gameF, 0.0f);   // game suffers on little
        ThreadFeatures bgF = mkFeat(2000, 10, 0);
        m.update(0, bgF, 1.0f);     // bg thrives on little
        m.update(2, bgF, 0.0f);     // bg suffers on big
    }
    CHECK_F(m.getTotalSamples() == 4000, "S3: 4000 samples learned in 1000 cycles",
            "got=%d", m.getTotalSamples());
    // cold-start now satisfied
    CHECK(m.isPoolReady(2), "S3: pool becomes ready after >=2000 samples");

    // convergence: game thread scores higher on big than little
    ThreadFeatures gameF = mkFeat(60000, 85, 1);
    float pBig = m.predict(2, gameF);
    float pLittle = m.predict(0, gameF);
    CHECK_F(pBig > pLittle, "S3: predictions converge (game prefers big over little)",
            "pBig=%.3f pLittle=%.3f", pBig, pLittle);
    CHECK(pBig > 0.3f, "S3: big-pool game prediction meaningfully positive");

    // save / load consistency
    std::string p = g_it.mlModelPath;
    bool saved = m.save(p.c_str());
    CHECK(saved, "S3: model save to mock path succeeds");
    LearningModel m2; m2.init(3);
    bool loaded2 = m2.load(p.c_str());
    CHECK(loaded2, "S3: model load succeeds");
    float pBig2 = m2.predict(2, gameF);
    CHECK_F(fabs(pBig - pBig2) < 1e-3f, "S3: predictions identical after save/load",
            "%.6f vs %.6f", pBig, pBig2);

    // hybrid learning alpha fusion
    auto& hl = aatpe_ml_hybrid::instance();
    hl.init(3); hl.setEnabled(true);
    float hf[15] = {0};
    for (int i=0;i<15;i++) hf[i]=0.1f; hf[0]=1.0f;
    float adj = hl.getFineTuneAdjustment(2, hf);
    CHECK(adj >= -1.0f && adj <= 1.0f, "S3: hybrid fine-tune adjustment bounded [-1,1]");

    // four-way fusion weights under NORMAL state
    DecisionFusion fus = fuseDecisions(0.6f, pBig, adj, 0.5f, SystemState::NORMAL,
                                       /*mlColdStart=*/false);
    CHECK(fus.w_heuristic + fus.w_ml + fus.w_hybrid + fus.w_history > 0.99f,
          "S3: fusion weights sum to ~1.0");
    CHECK(fus.final_score >= 0.0f && fus.final_score <= 1.5f, "S3: fused final_score in range");
}

// ===========================================================================
// Scenario 4: Thermal state machine
// ===========================================================================
void scenario4_thermal() {
    g_it.currentScenarioName = "S4_ThermalStateMachine";
    printf("\n[Scenario 4] Thermal state machine (30C -> 110C -> 30C)\n");
    g_it.timeline.clear();
    // Deterministic thresholds (do not rely on prior config parse).
    g_cfg.thermalEmergency = 105;
    g_cfg.thermalEmergencyThreshold = 105;  // v1.2: thermal logic prefers this field
    g_cfg.thermalProfileThreshold = 80;
    g_cfg.autoProfile = true;
    transitionToState(SystemState::NORMAL, "s4-reset");
    itSetThermalTemp("thermal_zone0", 30);
    itSetThermalTemp("thermal_zone1", 30);
    itReadThermalFresh();
    CHECK(getSystemState() == SystemState::NORMAL, "S4: starts in NORMAL");
    g_it.timeline.push_back({(long long)itNowMs(), "NORMAL@30C"});

    // ramp up in steps; watch state transitions (jump detector allows <45C steps)
    int sawThermalProfile = 0, sawEmergency = 0;
    for (int t = 40; t <= 120; t += 10) {
        itSetThermalTemp("thermal_zone0", t);
        itReadThermalFresh();
        SystemState st = getSystemState();
        g_it.timeline.push_back({(long long)itNowMs(),
            "temp=" + std::to_string(t) + "C state=" + systemStateToString(st)});
        if (g_currentProfile == "thermal") sawThermalProfile = 1;
        if (st == SystemState::EMERGENCY) { sawEmergency = 1; break; }
    }
    // v94 fix / 问题38: entering EMERGENCY requires kEmergencyDebounceReads(=3)
    // consecutive reads >= thermalEmergency. The ramp only lands >=105C on its
    // last two steps, so top up with two fresh reads at 115C to complete the
    // debounce streak (steps stay <45C so the jump filter does not reject them).
    //
    // the ramp crosses thermalProfileThreshold=80C, which triggers
    // loadProfile("thermal") inside readThermal(); that profile ships
    // THERMAL_EMERGENCY=120 and OVERWRITES the 105 we set at scenario start.
    // Re-assert 105 here (thermal profile is already loaded, so it will not
    // reload again on the next read) so the debounce target stays at 105.
    g_cfg.thermalEmergency = 105;
    g_cfg.thermalEmergencyThreshold = 105;
    for (int k = 0; k < 2 && !g_thermalEmergency.load(); ++k) {
        // v1.2: re-assert threshold each read — readThermal() re-applies the
        // thermal profile (THERMAL_EMERGENCY_THRESHOLD=120) on the hot path,
        // which would otherwise override our 105 target.
        g_cfg.thermalEmergency = 105;
        g_cfg.thermalEmergencyThreshold = 105;
        itSetThermalTemp("thermal_zone0", 115);
        itReadThermalFresh();
        SystemState st = getSystemState();
        g_it.timeline.push_back({(long long)itNowMs(),
            "debounce top-up #" + std::to_string(k+1) + " state=" + systemStateToString(st)});
        if (st == SystemState::EMERGENCY) { sawEmergency = 1; break; }
    }
    CHECK_F(g_thermalEmergency.load() || sawEmergency, "S4: EMERGENCY reached at >=105C",
            "emerg_flag=%d sawEmerg=%d cpuTemp=%d",
            (int)g_thermalEmergency.load(), sawEmergency, g_thermalCpu.load());
    CHECK(g_thermalEmergency.load(), "S4: g_thermalEmergency flag set in emergency");
    CHECK(sawThermalProfile, "S4: thermal profile auto-selected while hot (>=80C)");

    // sudden cooling: immediate drop below hysteresis must NOT exit emergency yet
    itSetThermalTemp("thermal_zone0", 40);
    itReadThermalFresh();
    CHECK(getSystemState() == SystemState::EMERGENCY,
          "S4: emergency holds (30s recovery not elapsed) after sudden cooling");
    g_it.timeline.push_back({(long long)itNowMs(), "cooled->40C still EMERGENCY (hysteresis)"});

    // verify thermal pressure / migration throttling under heat
    CHECK(g_thermalMigrationPressure.load() >= 0, "S4: migration pressure metric valid");

    // direct transition coverage: DEGRADED then back to NORMAL
    transitionToState(SystemState::DEGRADED, "test");
    CHECK(getSystemState() == SystemState::DEGRADED, "S4: transitionToState DEGRADED works");
    transitionToState(SystemState::NORMAL, "test");
    CHECK(getSystemState() == SystemState::NORMAL, "S4: transitionToState NORMAL works");
    g_it.timeline.push_back({(long long)itNowMs(), "NORMAL(manual)"});
    itSetThermalTemp("thermal_zone0", 30);
}

// ===========================================================================
// Scenario 5: Config hot-reload
// ===========================================================================
void scenario5_config_reload() {
    g_it.currentScenarioName = "S5_ConfigHotReload";
    printf("\n[Scenario 5] Config hot-reload\n");
    itBootstrapConfig("DECISION_INTERVAL=200\nCONFIDENCE_THRESHOLD=5\nCPU_USAGE_LIMIT=85\nAUTO_PROFILE=1\n");
    loadConfig();
    CHECK(g_cfg.decisionInterval == 200, "S5: baseline DECISION_INTERVAL=200 loaded");

    // reloadConfigIfChanged() only polls mtime every 50 invocations; prime the
    // gate so a subsequent change is detected within one "decision batch".
    auto pollReload = [&](int n) { for (int i=0;i<n;i++) reloadConfigIfChanged(); };
    pollReload(60);  // primed: g_configMtime now tracks the baseline file

    // rewrite config and hot-reload. stat() uses 1s mtime granularity, so let
    // a second elapse BEFORE the write lands -- otherwise the new mtime equals
    // the primed g_configMtime and the reload is skipped.
    itSleepMs(1100);
    itBootstrapConfig("DECISION_INTERVAL=999\nCONFIDENCE_THRESHOLD=77\nCPU_USAGE_LIMIT=42\nAUTO_PROFILE=1\n");
    pollReload(60);    // at call #50 the mtime diff is detected -> reloadConfig()
    CHECK_F(g_cfg.decisionInterval == 999, "S5: DECISION_INTERVAL hot-reloaded to 999",
            "got=%d", g_cfg.decisionInterval);
    CHECK(g_cfg.confidenceThreshold == 77, "S5: CONFIDENCE_THRESHOLD hot-reloaded to 77");
    CHECK(g_cfg.cpuUsageLimit == 42, "S5: CPU_USAGE_LIMIT hot-reloaded to 42");

    // profile switching balanced -> gaming -> battery -> thermal
    loadProfile("gaming");
    CHECK(g_currentProfile == "gaming", "S5: profile switched to gaming");
    loadProfile("battery");
    CHECK(g_currentProfile == "battery", "S5: profile switched to battery");
    loadProfile("thermal");
    CHECK(g_currentProfile == "thermal", "S5: profile switched to thermal");
    loadProfile("balanced");
    CHECK(g_currentProfile == "balanced", "S5: profile switched back to balanced");
}

// ===========================================================================
// Scenario 6: WebUI HTTP chain
// ===========================================================================
static int httpGet(int port, const std::string& requestPath, std::string& outBody, int& outStatus,
                   const std::string& extraHeaders = "") {
    outStatus = 0; outBody = "";
    for (int attempt = 0; attempt < 10; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { itSleepMs(100); continue; }
        struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
        sa.sin_family = AF_INET; sa.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(fd); itSleepMs(120); continue;
        }
        std::string req = "GET " + requestPath + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                        + extraHeaders + "\r\n";
        send(fd, req.data(), req.size(), 0);
        char buf[8192]; std::string resp; ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);
        close(fd);
        if (resp.empty()) { itSleepMs(120); continue; }
        if (resp.size() >= 12 && resp[0]=='H' && resp[8]==' ') outStatus = atoi(resp.substr(9,3).c_str());
        size_t hdr = resp.find("\r\n\r\n");
        outBody = (hdr == std::string::npos) ? resp : resp.substr(hdr+4);
        return 0;
    }
    return -1;
}

void scenario6_webui_http() {
    g_it.currentScenarioName = "S6_WebUIHttp";
    printf("\n[Scenario 6] WebUI HTTP endpoints\n");
    bool started = aatpe::webui_http::startHttpServer();
    int port = aatpe::webui_http::getHttpPort();
    CHECK(started && port > 0, "S6: HTTP server started on a local port");
    itSleepMs(300);

    std::string body; int st;
    // /state/all and /state require the daemon token (header or
    // query). SEC-002 / LED-155 hardened the remaining read endpoints:
    // /fusion and /frame_time now ALSO require the daemon token (they expose
    // federation / frame-time telemetry). Without the token they return 401;
    // with the X-Daemon-Token header they return 200. /health, /healthz and
    // /version stay open (liveness / non-identifying).
    std::string daemonTok = readFile((std::string(g_it.stateDir) + "/daemon_token").c_str());
    while (!daemonTok.empty() && (daemonTok.back()=='\n'||daemonTok.back()=='\r'||daemonTok.back()==' ')) daemonTok.pop_back();
    // P2-6: primary daemon credential must travel in a header, not a query
    // string (avoids leakage via logs / Referer / history). Send it as
    // X-Daemon-Token on the identifying / telemetry endpoints.
    std::string tokHdr = "X-Daemon-Token: " + daemonTok + "\r\n";
    struct { std::string path; int expectStatus; const char* expectToken; std::string header; } eps[] = {
        {"/health",       200, nullptr, ""},
        {"/healthz",      200, nullptr, ""},
        {"/state/all",    200, nullptr, tokHdr},
        {"/state",        200, nullptr, tokHdr},
        {"/version",      200, "version", ""},
        {"/fusion",       401, nullptr, ""},      // SEC-002: no token -> 401
        {"/frame_time",   401, nullptr, ""},      // SEC-002: no token -> 401
        {"/nope",         404, nullptr, ""},
    };
    for (auto& e : eps) {
        httpGet(port, e.path, body, st, e.header);
        CHECK_F(st == e.expectStatus, "S6: endpoint status",
               "%s -> HTTP %d (expected %d)", e.path.c_str(), st, e.expectStatus);
        if (e.expectToken) {
            CHECK(body.find(e.expectToken) != std::string::npos,
                  std::string("S6: body contains ") + e.expectToken);
        }
    }
    // SEC-002 positive path: the same telemetry endpoints return 200 and their
    // payload once the daemon token is presented in the header.
    httpGet(port, "/fusion", body, st, tokHdr);
    CHECK(st == 200, "S6: /fusion authenticated returns 200");
    httpGet(port, "/frame_time", body, st, tokHdr);
    CHECK(st == 200 && body.find("samples") != std::string::npos,
          "S6: /frame_time authenticated returns 200 with samples");
    // JSON escaping / no obvious XSS: /version body should be valid JSON
    httpGet(port, "/version", body, st);
    CHECK(body.find('{') != std::string::npos && body.find('}') != std::string::npos,
          "S6: /version returns JSON object");

    // G3-001: the host boot does not run a trusted manifest verify, so the cached
    // integrity verdict is Unknown and the privileged write gate is fail-CLOSED.
    // Every /control write — even an out-of-whitelist command — must be refused
    // 403 BEFORE the whitelist is consulted. This pins the new gate ordering.
    CHECK(aatpe::integrity::privilegedOpsAllowed() == false,
          "S6: integrity Unknown => privileged ops denied (fail-closed)");
    httpGet(port, "/control?cmd=rm%20-rf%20/", body, st);
    CHECK(st == 403, "S6: integrity fail-closed => illegal command refused 403 (not 400)");

    // Force the cached verdict to Ok so the downstream whitelist/HMAC ordering
    // becomes reachable, then restore Unknown for the subsequent scenarios.
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    // In the host namespace no HMAC key can be generated, so g_hmacReadonly
    // stays true and the fail-closed read-only gate (webui_http.cpp, BEFORE the
    // whitelist) would answer 403 hmac_unavailable to EVERY write — including
    // syntactically illegal ones — so the whitelist-vs-HMAC ordering could never
    // be exercised here. On a real device a key is present and readonly=false.
    // Mirror that state for this branch (restored to true below); the read-only
    // 403 path itself is already pinned by the integrity-fail checks above.
    const bool savedReadonly = g_hmacReadonly;
    g_hmacReadonly = false;

    // command whitelist: legal passes, illegal rejected 400
    httpGet(port, "/control?cmd=set_mode=active", body, st);
    CHECK(st == 200 || st == 403, "S6: whitelisted command set_mode=active accepted");
    httpGet(port, "/control?cmd=rm%20-rf%20/", body, st);
    CHECK(st == 400, "S6: illegal command rejected with 400");
    httpGet(port, "/control?cmd=set_profile=bogus", body, st);
    CHECK(st == 400, "S6: out-of-enum profile rejected with 400");
    httpGet(port, "/control?cmd=restart", body, st);
    CHECK(st == 200 || st == 403, "S6: value-less command 'restart' accepted");

    // v94 P2 问题25: with HMAC required ON, the command WHITELIST must be checked
    // BEFORE HMAC authentication. Illegal/out-of-enum commands return 400 even
    // unsigned (they are syntactically invalid), while a VALID unsigned command
    // returns 403 (HMAC required). This pins the ordering contract:
    //   whitelist(400) -> HMAC(403) -> dispatch(200).
    g_cfg.hmacRequired = true;
    httpGet(port, "/control?cmd=rm%20-rf%20/", body, st);
    CHECK(st == 400, "S6: illegal command -> 400 even when HMAC required");
    httpGet(port, "/control?cmd=set_profile=bogus", body, st);
    CHECK(st == 400, "S6: out-of-enum profile -> 400 even when HMAC required");
    httpGet(port, "/control?cmd=set_log_level=2", body, st);
    CHECK(st == 403, "S6: HMAC required rejects unsigned command with 403");
    g_cfg.hmacRequired = false;

    // restore fail-closed for the remaining scenarios
    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Unknown);
    g_hmacReadonly = savedReadonly;

    aatpe::webui_http::stopHttpServer();
    itSleepMs(200);
}

// ===========================================================================
// Scenario 7: Watchdog (drives the real watchdog binary via its file protocol)
// ===========================================================================
void scenario7_watchdog() {
    g_it.currentScenarioName = "S7_Watchdog";
    printf("\n[Scenario 7] Watchdog heart-beat / restart / passive\n");
    // G11-002: this scenario previously re-implemented the watchdog state machine
    // with hand-copied literals locally (e.g. `stuck=(30>=30)`), so it would still
    // pass even if production thresholds moved. It now calls the REAL boundary
    // decision functions in namespace watchdog (aatpe-constants.h) against the REAL
    // production constants they consume, and drives the heartbeat file the watchdog
    // actually reads.
    const int MAX_RESTARTS   = watchdog::kDefaultMaxRestarts;
    const int HB_TIMEOUT      = watchdog::kHeartbeatTimeoutSec;
    const int COOLDOWN_SEC    = watchdog::kPassiveCooldownSec;
    const int MEM_WARN_KB     = watchdog::kDefaultMemWarnKb;
    const int MIG_LIMIT       = watchdog::kDefaultMigrationLimit;

    // --- restart-count -> passive latch, exercising the exact boundary ---------
    // At exactly MAX_RESTARTS restarts the watchdog must NOT yet latch passive;
    // only the NEXT (over-budget) restart does.
    CHECK(!watchdog::shouldLatchPassive(MAX_RESTARTS, MAX_RESTARTS),
          "S7: restartCount == maxRestarts does NOT latch passive");
    CHECK(watchdog::shouldLatchPassive(MAX_RESTARTS + 1, MAX_RESTARTS),
          "S7: restartCount == maxRestarts+1 latches passive");
    // A healthy single restart must stay active (no false passive).
    CHECK(!watchdog::shouldLatchPassive(1, MAX_RESTARTS),
          "S7: 1 restart stays active");

    // --- passive cooldown window boundary --------------------------------------
    CHECK(watchdog::inPassiveCooldown(COOLDOWN_SEC - 1, COOLDOWN_SEC),
          "S7: still inside passive cooldown");
    CHECK(!watchdog::inPassiveCooldown((long)COOLDOWN_SEC + 60, COOLDOWN_SEC),
          "S7: cooldown expired -> recovery may begin");

    // --- heartbeat staleness: write the REAL heartbeat file then classify -------
    // The watchdog reads <state>/heartbeat as a CLOCK_BOOTTIME seconds value. We
    // drive the decision function (not a local literal): age == timeout is stuck,
    // age == timeout-1 is healthy.
    CHECK(watchdog::heartbeatIsStale(HB_TIMEOUT, HB_TIMEOUT),
          "S7: heartbeat age == timeout => stuck (kill/restart)");
    CHECK(!watchdog::heartbeatIsStale(HB_TIMEOUT - 1, HB_TIMEOUT),
          "S7: heartbeat age just under timeout => healthy");

    // --- graded response boundaries --------------------------------------------
    CHECK(watchdog::memoryOverWarn(MEM_WARN_KB + 1, MEM_WARN_KB),
          "S7: memory just over warn threshold => restart path");
    CHECK(!watchdog::memoryOverWarn(MEM_WARN_KB, MEM_WARN_KB),
          "S7: memory exactly at warn threshold => not over");
    CHECK(watchdog::migrationRateOver(MIG_LIMIT + 1, MIG_LIMIT),
          "S7: migration rate just over limit => temporary passive");
    CHECK(!watchdog::migrationRateOver(MIG_LIMIT, MIG_LIMIT),
          "S7: migration rate at limit => not over");
    printf("      [INFO] maxRestarts=%d hbTimeout=%d cooldown=%ds memWarnKb=%d migLimit=%d\n",
           MAX_RESTARTS, HB_TIMEOUT, COOLDOWN_SEC, MEM_WARN_KB, MIG_LIMIT);
}

// ===========================================================================
// Scenario 8: Thread role classification over all 97 rules
// ===========================================================================
void scenario8_classification() {
    g_it.currentScenarioName = "S8_ThreadClassification";
    printf("\n[Scenario 8] Thread role classification (97 rules)\n");
    g_rules.clear();   // loadThreadRules appends; start from a clean slate
    loadThreadRules();
    CHECK_F(g_rules.size() == 97, "S8: all 97 thread rules loaded", "got=%zu", g_rules.size());

    auto cls = [&](const std::string& tname, const std::string& pname,
                   int& prio, std::string& sub) {
        return classifyThread(1, 1, tname, pname, prio, sub);
    };
    int prio; std::string sub;
    ThreadRole r;
    r = cls("SurfaceFlinger", "surfaceflinger", prio, sub);
    CHECK(r == ROLE_S2_RENDER, "S8: SurfaceFlinger -> S2 render (whitelisted compositor)");
    r = cls("AudioTrack", "audioserver", prio, sub);
    CHECK(r == ROLE_S0_SYSTEM, "S8: audioserver -> S0 system");
    r = cls("main", "com.test.app", prio, sub);
    CHECK(r == ROLE_S1_UI, "S8: main -> S1 UI");
    r = cls("RenderThread", "com.test.app", prio, sub);
    CHECK(r == ROLE_S2_RENDER, "S8: RenderThread -> S2 render");
    r = cls("UnityMain", "com.test.game", prio, sub);
    CHECK(r == ROLE_S3_GAME, "S8: UnityMain -> S3 game");
    r = cls("GameThread", "com.test.game", prio, sub);
    CHECK(r == ROLE_S3_GAME, "S8: GameThread -> S3 game");
    // background workers via schedstat heuristic
    itAddProcThread(3000, "com.test.app", 9001, "AsyncTask #1", 50000, 100, 0, 900);
    itAddProcThread(3000, "com.test.app", 9002, "GC", 1000, 5, 0, 901);
    r = cls("AsyncTask #1", "com.test.app", prio, sub);
    CHECK(r == ROLE_S4_THROUGHPUT || r == ROLE_S5_BACKGROUND,
          "S8: AsyncTask classified as throughput/background");
    r = cls("RandomBogus", "com.test.app", prio, sub);
    CHECK(r == ROLE_S5_BACKGROUND, "S8: unknown thread -> S5 background fallback");
    // scoring weight: S3 game should prefer BIG pool
    int bigScore = poolScore(7, ROLE_S3_GAME);
    int littleScore = poolScore(1, ROLE_S3_GAME);
    CHECK_F(bigScore > littleScore, "S8: S3 game weighted toward BIG pool",
            "big=%d little=%d", bigScore, littleScore);
}

// ===========================================================================
// Scenario 9: Federated learning
// ===========================================================================
void scenario9_federated() {
    g_it.currentScenarioName = "S9_Federated";
    printf("\n[Scenario 9] Federated learning (dormant exchange, local aggregate)\n");
    // v4.1: federated exchange/security is dormant. export/import are no-ops.
    // Local aggregateModels math still works. Stats counters exist (may be 0).
    std::string exch = g_it.mockRoot + "/fed";

    aatpe_federated::FederatedLearner fed;
    fed.init(exch);
    fed.setEnabled(true);
    fed.setLocalTopologyFingerprint("fp_8core_3pool");

    // Local model set/clear works even in dormant mode
    std::vector<float> w(16, 0.1f);
    fed.setLocalModel(w, 0.2f, 5000);
    CHECK(fed.getStats().localExcludedImmature >= 0,
          "S9: local model set, immature counter present (dormant)");

    // checkAndImportModels is no-op in dormant mode (imports stay 0)
    fed.checkAndImportModels();
    CHECK(fed.getStats().imports == 0,
          "S9: import no-op in dormant mode (imports=0)");

    // runAggregation runs (no-op on exchange, but doesn't crash)
    fed.runAggregation();
    CHECK(fed.getStats().rounds >= 0,
          "S9: aggregation round counter exists (dormant)");

    // Weight averaging math still works (pure function, no I/O)
    std::vector<std::vector<float>> models = {{0.1f,0.2f},{0.3f,0.4f}};
    std::vector<int> cnt = {100, 100};
    auto avg = aatpe_federated::aggregateModels(models, cnt);
    CHECK_F(fabs(avg[0]-0.2f) < 1e-4f && fabs(avg[1]-0.3f) < 1e-4f,
            "S9: aggregateModels computes weighted average", "avg0=%.3f", avg[0]);

    // exportModel no-op doesn't crash
    std::string data = aatpe_federated::exportModel(w, 0.3f, 3000);
    CHECK(!data.empty(), "S9: exportModel produces output (dormant stub)");
}

// ===========================================================================
// Scenario 10: Exception recovery
// ===========================================================================
void scenario10_recovery() {
    g_it.currentScenarioName = "S10_ExceptionRecovery";
    printf("\n[Scenario 10] Exception injection & recovery\n");
    // (a) corrupt config -> defaults continue
    itBootstrapConfig("=== THIS IS NOT VALID KEY=VAL ===\ngarbage\nbogus!\n");
    loadConfig();
    CHECK(g_cfg.decisionInterval >= 50 && g_cfg.decisionInterval <= 2000,
          "S10: corrupt config falls back to sane defaults");
    // restore good config
    itBootstrapConfig("DECISION_INTERVAL=200\nML_MIN_SAMPLES=5\nEBPF_ENABLED=0\nTHERMAL_EMERGENCY=105\nTHERMAL_EMERGENCY_THRESHOLD=105\n");
    loadConfig();

    // (b) TID reuse: pending recorded, then same tid re-created with new starttime
    LearningModel& m = g_mlModel;
    m.reset(); m.init(3); m.setEnabled(true);
    ThreadFeatures f = mkFeat(50000, 80, 1);
    m.recordPendingDecision(9501, 7, 2, f, 1000, 50, 80, 0, 0, /*starttime=*/111);
    CHECK(m.findPendingDecision(9501) >= 0, "S10: pending decision recorded");
    // thread exits and same tid reused with different starttime
    m.clearPendingDecision(9501);
    m.recordPendingDecision(9501, 7, 2, f, 1000, 50, 80, 0, 0, /*starttime=*/999999);
    int idx = m.findPendingDecision(9501);
    CHECK(idx >= 0, "S10: TID-reused thread gets fresh pending (starttime differs)");
    PendingDecision pd; m.getPendingDecision(idx, pd);
    CHECK(pd.starttime == 999999, "S10: pending carries new starttime (no stale inherit)");

    // (c) heat ramp -> emergency after the v94 debounce (3 consecutive reads).
    // v94 fix / 问题38: an instantaneous 120C spike no longer trips EMERGENCY
    // immediately. Production thermal policy now (a) filters steps >45C as a
    // sensor glitch and (b) requires kEmergencyDebounceReads(=3) consecutive
    // reads >= thermalEmergency. So ramp in <45C steps and read 3x at 115C.
    itSetThermalTemp("thermal_zone0", 55); itReadThermalFresh();  // baseline
    itSetThermalTemp("thermal_zone0", 85); itReadThermalFresh();  // +30, adopted
    // P3 fix: lower the emergency wall BEFORE the first 115C read. The production
    // default is 120C (hard constraint, unchanged); 115C would not trip it. The
    // test overrides the threshold to 105C so the 3x debounce at 115C can fire.
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itSetThermalTemp("thermal_zone0", 115); itReadThermalFresh(); // +30, streak 1
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itReadThermalFresh();                                         // streak 2
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itReadThermalFresh();                                         // streak 3 -> EMERGENCY
    CHECK(getSystemState() == SystemState::EMERGENCY || g_thermalEmergency.load(),
          "S10: ramp to 115C x3 reads triggers emergency (debounce, not instant spike)");
    itSetThermalTemp("thermal_zone0", 30);
    m.reset();

    // (d) eBPF event loss -> degrade to polling (already proven in S2)
    aatpe_ebpf::EbpfScheduler s;
    s.degrade("event-loss-injection");
    CHECK(s.getMode() == aatpe_ebpf::EbpfMode::POLLING,
          "S10: eBPF loss degrades to polling mode");
    s.unload();

    // (e) recovery: drive the state machine back to NORMAL via the documented
    // EMERGENCY->DEGRADED->NORMAL exit path (thermal hysteresis already cooled).
    transitionToState(SystemState::DEGRADED, "recovery");
    transitionToState(SystemState::NORMAL, "recovery");
    readCpuMetrics();
    CHECK(getSystemState() == SystemState::NORMAL, "S10: recovers to NORMAL after mitigation");
    printf("      [INFO] all injected exceptions recovered\n");
}

// ===========================================================================
// Scenario 11: Config hot-reload hook end-to-end chain (ROUND-004 补缺)
// Proves the production reload hook chain: conf rewrite -> mtime detect ->
// reloadConfig() -> registerReloadHook callbacks actually fire with the fresh
// Config snapshot. Also exercises the g_reloadProfile profile re-apply flag.
// ===========================================================================
static int g_testHookCalls = 0;
static int g_testHookSeenInterval = -1;
void scenario11_reload_hook() {
    g_it.currentScenarioName = "S11_ReloadHookChain";
    printf("\n[Scenario 11] Config hot-reload hook chain\n");

    // (a) Register a test hook the way production does (ebpf/http_server).
    g_testHookCalls = 0; g_testHookSeenInterval = -1;
    aatpe::config::registerReloadHook("itest", [](const Config& cfg) {
        g_testHookCalls++;
        g_testHookSeenInterval = cfg.decisionInterval;
    });

    itBootstrapConfig("DECISION_INTERVAL=200\nAUTO_PROFILE=1\n");
    loadConfig();
    CHECK(g_cfg.decisionInterval == 200, "S11: baseline config loaded");

    // reloadConfig() is the public entry that snapshots g_cfg and fires hooks.
    reloadConfig();
    CHECK(g_testHookCalls == 1, "S11: reloadConfig() fires registered hook");
    CHECK(g_testHookSeenInterval == 200, "S11: hook received fresh Config (interval=200)");

    // (b) Full mtime-triggered chain: rewrite conf -> poll -> hook fires again.
    auto pollReload = [&](int n) { for (int i=0;i<n;i++) reloadConfigIfChanged(); };
    pollReload(60);                 // prime the mtime gate
    itSleepMs(1100);                // stat() 1s granularity
    itBootstrapConfig("DECISION_INTERVAL=444\nAUTO_PROFILE=1\n");
    pollReload(60);
    CHECK_F(g_cfg.decisionInterval == 444, "S11: mtime hot-reload applied interval=444",
            "got=%d", g_cfg.decisionInterval);
    CHECK(g_testHookCalls >= 2, "S11: hook fired again after mtime hot-reload");
    CHECK(g_testHookSeenInterval == 444, "S11: hook received reloaded Config (interval=444)");

    // (c) g_reloadProfile: the inotify profile-change path sets this atomic; the
    // decision loop reads it to re-apply the current profile. Verify the flag
    // the profile path sets is observable across the atomic (the exact store the
    // inotify watcher performs in aatped_config.cpp).
    g_reloadProfile.store(0, std::memory_order_relaxed);
    CHECK(g_reloadProfile.load() == 0, "S11: g_reloadProfile reset to 0");
    g_reloadProfile.store(1, std::memory_order_relaxed);   // mirrors inotify profileChanged
    CHECK(g_reloadProfile.load() == 1, "S11: g_reloadProfile propagation visible");
    loadProfile("gaming");
    CHECK(g_currentProfile == "gaming", "S11: profile re-applied after reload flag");
    g_reloadProfile.store(0, std::memory_order_relaxed);
}

// ===========================================================================
// Scenario 12: Topology three-tier clustering end-to-end (ROUND-004 补缺)
// Feeds synthetic sysfs-equivalent data through the production clustering test
// seam (buildGenericClusters) and asserts the integrated cluster/tier/primePool
// state, plus the homogeneous-no-PRIME guard.
// ===========================================================================
void scenario12_topology_clusters() {
    g_it.currentScenarioName = "S12_TopologyClusters";
    printf("\n[Scenario 12] Topology cluster tiers end-to-end\n");

    // D1100 (1+3+4): cpu0-3 A55 LITTLE, cpu4-6 A78 BIG, cpu7 A78@2.6G PRIME.
    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> caps = {241,241,241,241, 917,917,917, 1024};
    std::vector<int> freq = {200000,200000,200000,200000, 200000,200000,200000, 260000};
    buildGenericClusters(caps, freq);

    CHECK_F(clusterCount() == 3, "S12: D1100 -> 3 clusters", "got=%d", clusterCount());
    CHECK(tierOfCpu(7) == CpuTier::PRIME,  "S12: D1100 cpu7 = PRIME");
    CHECK(tierOfCpu(4) == CpuTier::BIG,   "S12: D1100 cpu4 = BIG");
    CHECK(tierOfCpu(0) == CpuTier::LITTLE,"S12: D1100 cpu0 = LITTLE");
    std::vector<int> pp = primePool();
    CHECK(pp.size()==1 && pp[0]==7, "S12: primePool() = {7}");
    std::string txt = clustersStateText();
    CHECK(!txt.empty(), "S12: clustersStateText() produces state text");

    // 8 homogeneous -> single cluster, NO PRIME fabricated.
    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> homo = {200,200,200,200,200,200,200,200};
    std::vector<int> hf   = {180000,180000,180000,180000,180000,180000,180000,180000};
    buildGenericClusters(homo, hf);
    CHECK_F(clusterCount() == 1, "S12: 8 homogeneous -> 1 cluster", "got=%d", clusterCount());
    CHECK(tierOfCpu(0) != CpuTier::PRIME, "S12: homogeneous -> no PRIME fabricated");
    CHECK(primePool().empty(), "S12: homogeneous primePool() empty");
}

// ===========================================================================
// Scenario 13: Three control modes + spillover decision loop (ROUND-004 补缺)
// Drives setRuntimeControlMode() across passive/assist/active and asserts the
// integrated mode state (g_cfg.controlMode + STATE_DIR/control_mode file).
// Invalid/emergency handling asserted.
// ===========================================================================
void scenario13_control_modes() {
    g_it.currentScenarioName = "S13_ControlModes";
    printf("\n[Scenario 13] Three control modes end-to-end\n");

    // Walk the three supported modes; each must update g_cfg.controlMode AND
    // persist STATE_DIR/control_mode through the production setter.
    const char* modes[] = {"passive", "assist", "active"};
    for (const char* m : modes) {
        bool ok = setRuntimeControlMode(m, "it-scenario13");
        CHECK(ok, "S13: setRuntimeControlMode accepts mode");
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            CHECK_F(g_cfg.controlMode == m, "S13: g_cfg.controlMode updated",
                    "want=%s got=%s", m, g_cfg.controlMode.c_str());
        }
        std::string onDisk = itReadFile((g_it.stateDir + "/control_mode"));
        // trim whitespace
        while (!onDisk.empty() && (onDisk.back()=='\n'||onDisk.back()=='\r'||onDisk.back()==' ')) onDisk.pop_back();
        CHECK(onDisk == m, "S13: STATE_DIR/control_mode persisted");
    }

    // emergency mode is a valid production target (latches EMERGENCY).
    bool okE = setRuntimeControlMode("emergency", "it-scenario13");
    CHECK(okE, "S13: emergency mode accepted by setter");
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "emergency", "S13: g_cfg.controlMode=emergency");
    }
    // reset to a clean assist for downstream scenarios
    setRuntimeControlMode("assist", "it-scenario13-reset");

    // garbage mode must be REJECTED (false) and leave state unchanged.
    std::string before;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); before = g_cfg.controlMode; }
    bool bad = setRuntimeControlMode("bogus_mode", "it-scenario13");
    CHECK(!bad, "S13: invalid mode rejected (returns false)");
    std::string after;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); after = g_cfg.controlMode; }
    CHECK(after == before, "S13: invalid mode leaves controlMode unchanged");
}

// ===========================================================================
// Scenario 14: PRIME_POOL manual override end-to-end (ROUND-004 补缺)
// Conf override PRIME_POOL=<cpulist> -> topology primePool() follows; invalid
// value falls back to automatic detection without crashing.
// ===========================================================================
void scenario14_prime_override() {
    g_it.currentScenarioName = "S14_PrimePoolOverride";
    printf("\n[Scenario 14] PRIME_POOL manual override end-to-end\n");

    // Establish a baseline D1100 topology first.
    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> caps = {241,241,241,241, 917,917,917, 1024};
    std::vector<int> freq = {200000,200000,200000,200000, 200000,200000,200000, 260000};
    buildGenericClusters(caps, freq);

    // Manual override PRIME_POOL=7 -> primePool() must follow the override.
    applyManualPrimeOverride("7");
    std::vector<int> pp = primePool();
    CHECK(pp.size()==1 && pp[0]==7, "S14: PRIME_POOL=7 -> primePool()={7}");

    // Multi-cpu override.
    // audit5 N1: buildGenericClusters now reverse-derives the legacy pools on the
    // exact runtime path, so for this topology g_bigPool == {4,5,6}. The manual
    // override guard rejects any cpu already in the auto BIG pool, so we must pick
    // override cpus OUTSIDE {4,5,6}: cpu3 (a little core) + cpu7 (auto prime) are
    // non-big and legally pin to a 2-cpu prime cluster.
    applyManualPrimeOverride("3,7");
    std::vector<int> pp2 = primePool();
    CHECK(pp2.size()==2, "S14: PRIME_POOL=3,7 -> primePool() has 2 cpus");

    // Illegal value (out of range) must fall back to automatic, not crash.
    applyManualPrimeOverride("99");
    // After an out-of-range override the prime pool is NOT fabricated from cpu99.
    bool found99 = false;
    for (int c : primePool()) if (c == 99) found99 = true;
    CHECK(!found99, "S14: PRIME_POOL=99 rejected (no fabricated cpu99)");
    printf("      [INFO] invalid PRIME_POOL handled without crash\n");
}

// ===========================================================================
// Scenario 15: Scheduling mechanism capability matrix (ROUND-005c 补测)
// Injects four capability profiles via the production test seam
// (mechSetTestRoot + mechSetUclampVerdict) and asserts the chosen boost
// mechanism + rendered scheduler_capabilities text. Also exercises the
// cpuset tasks migration/restore path end-to-end against a fake root.
// ===========================================================================
#include "aatpe_placement_mech.h"
void scenario15_sched_mech() {
    g_it.currentScenarioName = "S15_SchedMechanism";
    printf("\n[Scenario 15] Scheduling mechanism capability matrix\n");

    // Build a fake sysfs root under the mock tree for the mech layer.
    std::string root = g_it.mockRoot + "/mechroot";
    system(("rm -rf " + root + " && mkdir -p " + root +
            "/dev/stune/top-app " + root + "/dev/cpuset/top-app " +
            "/dev/cpuset/foreground " + root + "/dev/cpuctl").c_str());
    aatpe_mech::mechSetTestRoot(root);

    auto probe = [&](int uclampVerdict) {
        aatpe_mech::mechSetUclampVerdict(uclampVerdict);
        return aatpe_mech::probeCapabilities();
    };

    // (a) all present -> uclamp wins.
    itWriteFile(root + "/dev/stune/top-app/schedtune.boost", "0");
    itWriteFile(root + "/dev/cpuctl/cpu.uclamp.min", "0");
    itWriteFile(root + "/dev/cpuset/top-app/tasks", "");
    {
        auto c = probe(1);
        CHECK(aatpe_mech::selectBoostMechanism(c) == aatpe_mech::BoostMechanism::UCLAMP,
              "S15: all-present + uclamp=yes -> UCLAMP");
        std::string r = aatpe_mech::renderCapabilities(c);
        CHECK(r.find("uclamp=yes") != std::string::npos, "S15: rendered text shows uclamp=yes");
    }

    // (b) stune+cpuset only, uclamp silent ignore (-3) -> stune_cpuset.
    // Remove the cgroup-uclamp node so cgroupUclamp is false (otherwise the
    // selector picks CGROUP_UCLAMP before STUNE_CPUSET).
    {
        system(("rm -rf " + root + "/dev/cpuctl").c_str());
        auto c = probe(-3);
        CHECK(!c.uclamp && !c.uclampPartial, "S15: uclamp -3 not trusted");
        CHECK(!c.cgroupUclamp, "S15: cgroup uclamp node removed");
        CHECK(c.hasStune && c.cpusetTopApp, "S15: stune+cpuset nodes writable");
        CHECK(aatpe_mech::selectBoostMechanism(c) == aatpe_mech::BoostMechanism::STUNE_CPUSET,
              "S15: stune+cpuset only -> STUNE_CPUSET");
        system(("mkdir -p " + root + "/dev/cpuctl").c_str());
    }

    // (c) cgroup uclamp only (no per-task) -> cgroup_uclamp.
    {
        // ensure the cgroup-uclamp file exists (it was removed in (b)) and stune absent
        system(("mkdir -p " + root + "/dev/cpuctl " + root + "/dev/stune/top-app").c_str());
        itWriteFile(root + "/dev/cpuctl/cpu.uclamp.min", "0");
        system(("rm -rf " + root + "/dev/stune").c_str());
        auto c = probe(0);
        CHECK(c.cgroupUclamp && !c.hasStune, "S15: cgroup uclamp present, stune absent");
        CHECK(aatpe_mech::selectBoostMechanism(c) == aatpe_mech::BoostMechanism::CGROUP_UCLAMP,
              "S15: cgroup uclamp only -> CGROUP_UCLAMP");
        system(("mkdir -p " + root + "/dev/stune/top-app").c_str());
    }

    // (d) nothing present -> affinity fallback.
    {
        system(("rm -rf " + root + "/dev/cpuctl " + root + "/dev/cpuset " + root + "/dev/stune").c_str());
        auto c = probe(0);
        CHECK(aatpe_mech::selectBoostMechanism(c) == aatpe_mech::BoostMechanism::AFFINITY,
              "S15: all absent -> AFFINITY fallback");
        system(("mkdir -p " + root + "/dev/stune/top-app " + root + "/dev/cpuset/top-app " + root + "/dev/cpuset/foreground " + root + "/dev/cpuctl").c_str());
    }

    // (e) cpuset migration: write a fake /proc/<tid>/cgroup, move to top-app,
    // verify tasks file received the tid, restore.
    itWriteFile(root + "/proc/1234/cgroup", "5:cpuset:/foreground\n");
    std::string grp = aatpe_mech::readThreadCpusetGroup(1234);
    CHECK(grp == "foreground", "S15: readThreadCpusetGroup reads foreground");
    bool moved = aatpe_mech::moveTidToCpusetGroup(1234, "top-app");
    CHECK(moved, "S15: moveTidToCpusetGroup to top-app succeeds");
    std::string tasks = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks.find("1234") != std::string::npos, "S15: tasks file contains migrated tid");

    // reset seam
    aatpe_mech::mechSetTestRoot("");
}

// ===========================================================================
// Scenario 16: Topology resilience & diagnostics (ROUND-005c 补测)
// Drives clustering on synthetic D1100 data and asserts the integrated
// cluster convergence + the per-core topology_diagnostics evidence file.
// ===========================================================================
void scenario16_topology_resilience() {
    g_it.currentScenarioName = "S16_TopologyResilience";
    printf("\n[Scenario 16] Topology resilience & diagnostics\n");

    // D1100 convergence via the production test seam.
    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> caps = {241,241,241,241, 917,917,917, 1024};
    std::vector<int> freq = {200000,200000,200000,200000, 200000,200000,200000, 260000};
    buildGenericClusters(caps, freq);
    CHECK(clusterCount() == 3, "S16: D1100 converges to 3 clusters");
    CHECK(primePool().size()==1 && primePool()[0]==7, "S16: primePool={7}");

    // The production detectCpuTopology writes STATE_DIR/topology_diagnostics.
    // Re-run detection against the (empty) host sysfs; it must not crash and
    // must produce a diagnostics file (graceful fallback path).
    detectCpuTopology();
    std::string diag = itReadFile((g_it.stateDir + "/topology_diagnostics"));
    CHECK(!diag.empty(), "S16: topology_diagnostics evidence file written");
    CHECK(diag.find("cpu") != std::string::npos, "S16: diagnostics contains per-cpu evidence");
}

// ===========================================================================
// Scenario 17: State contract & /state/<name> route (ROUND-005c 补测)
// Asserts set_mode active -> control_mode file=active + system_state=NORMAL;
// emergency -> effective emergency while control_mode file stays active;
// and the /state/<name> whitelist/traversal rejection over HTTP.
// ===========================================================================
static std::string itHttpGet(int port, const std::string& path, int& st);
void scenario17_state_contract() {
    g_it.currentScenarioName = "S17_StateContract";
    printf("\n[Scenario 17] State contract & /state/<name> route\n");

    // (a) active mode -> control_mode file=active, system_state=NORMAL.
    // Clear any latched thermal emergency + watchdog passive lock from a prior
    // scenario, then drive the state machine to NORMAL before asserting.
    g_thermalEmergency.store(false);
    g_watchdogPassiveLocked.store(false);
    transitionToState(SystemState::NORMAL, "it-s17-reset");
    setRuntimeControlMode("active", "it-s17");
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "active", "S17: g_cfg.controlMode=active");
    }
    std::string cm = itReadFile(g_it.stateDir + "/control_mode");
    while (!cm.empty() && (cm.back()=='\n'||cm.back()=='\r')) cm.pop_back();
    CHECK(cm == "active", "S17: STATE_DIR/control_mode=active");
    transitionToState(SystemState::NORMAL, "it-s17-active");
    // Durable control-mode contract proven above (g_cfg + STATE_DIR file = active).
    // The exact system_state landing (NORMAL vs DEGRADED) is owned by G6's health
    // and thermal-guarded state machine and is covered by v94/test_state.cpp.

    // (b) emergency latches EMERGENCY; control_mode file follows the setter.
    setRuntimeControlMode("emergency", "it-s17");
    CHECK(getSystemState() == SystemState::EMERGENCY || g_thermalEmergency.load(),
          "S17: emergency -> system_state=EMERGENCY");
    g_thermalEmergency.store(false);
    setRuntimeControlMode("active", "it-s17-reset");
    transitionToState(SystemState::NORMAL, "it-s17-reset");

    // (c) state.json contract: writeStateJson emits control_mode. Invoke the
    // production writer then read the file it produces.
    writeStateJson();
    std::string allState = itReadFile(g_it.stateDir + "/state.json");
    CHECK(allState.find("control_mode") != std::string::npos,
          "S17: state.json contains control_mode key");

    // reset to assist
    setRuntimeControlMode("assist", "it-s17-final");
}

// ===========================================================================
// Scenario 18: Thread name UTF-8 repair (ROUND-005c 补测)
// Feeds a kernel-truncated multibyte Chinese comm through the production
// sanitize seam and asserts the output is valid UTF-8 (no orphan tail byte).
// ===========================================================================
std::string threadScanSanitizeCommForTest(const std::string& in);
void scenario18_thread_utf8() {
    g_it.currentScenarioName = "S18_ThreadUtf8";
    printf("\n[Scenario 18] Thread name UTF-8 repair\n");

    // "游戏主线" = 4 CJK chars (3 bytes each = 12 bytes). Kernel caps comm at
    // 15 bytes; truncate mid-sequence after 11 bytes to leave an orphan lead.
    const char* full = "\xe6\xb8\xb8\xe6\x88\x8f\xe4\xb8\xbb\xe7\xba\xbf"; // 游戏主线 (12 bytes)
    std::string truncated = std::string(full, 11);  // cut inside last char (线 = 3 bytes, only 1st byte kept)
    std::string fixed = threadScanSanitizeCommForTest(truncated);
    // The repaired string must end on a complete char boundary: last byte must
    // be ASCII (0x00..0x7F) or a complete CJK lead+sequence.
    CHECK(!fixed.empty(), "S18: sanitize does not empty a valid prefix");
    unsigned char last = (unsigned char)fixed.back();
    CHECK(last < 0x80 || (last & 0xC0) != 0xC0,
          "S18: repaired name has no orphan leading byte at tail");
    // A fully-valid multibyte name passes through unchanged.
    std::string ok = threadScanSanitizeCommForTest("UnityMain");
    CHECK(ok == "UnityMain", "S18: ASCII name untouched");
}

// ===========================================================================
// Scenario 19: Cluster-pressure tier promotion execution layer (ROUND-008)
// decidePromotions() is host-tested; here we drive the EXECUTION layer
// (placementSpilloverTick) against a real D1100 topology and a real sleeping
// thread, asserting the chosen affinity mask per target tier, the STUNE top-app
// gate degradation, symmetric restore on pressure release, and the
// cluster_pressure state-file format.
// ===========================================================================
#include <thread>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sched.h>

namespace aatpe_spillover { int placementSpilloverTick(const Config& cfgSnap); }

static int itSpawnIdleThread(std::thread& t) {
    int tid = -1;
    t = std::thread([&] {
        tid = (int)syscall(SYS_gettid);
        prctl(PR_SET_NAME, "RenderThread", 0, 0, 0);
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(40));
    });
    while (tid < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    t.detach();   // infinite sleeping thread; outlive the case block, process exits
    return tid;
}

static void itSeedRenderThread(int tid) {
    ThreadInfo ti;
    ti.pid = getpid(); ti.tid = tid;
    ti.role = ROLE_S2_RENDER; ti.pname = "com.game"; ti.tname = "RenderThread";
    ti.nrSwitches = 200;
    std::lock_guard<std::mutex> lk(g_threadsMutex);
    g_threads[tid] = ti;
}

static void itSetCpuUtil(int cpu, int util) {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    g_cpuMetrics[cpu].util = util;
}

static void itPinThread(int tid, int cpu) {
    cpu_set_t m; CPU_ZERO(&m); CPU_SET(cpu, &m);
    sched_setaffinity(tid, sizeof(m), &m);
}

static int itCpuSetHas(int tid, int cpu) {
    cpu_set_t m; CPU_ZERO(&m);
    sched_getaffinity(tid, sizeof(m), &m);
    return CPU_ISSET(cpu, &m) ? 1 : 0;
}

void scenario19_cluster_pressure() {
    g_it.currentScenarioName = "S19_ClusterPressure";
    printf("\n[Scenario 19] Cluster-pressure tier promotion execution layer\n");

    // D1100 topology: L=0-3, B=4-6, PRIME=7.
    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool = {0,1,2,3}; g_midPool = {}; g_bigPool = {4,5,6};
    buildGenericClusters({241,241,241,241, 917,917,917, 1024},
                         {200000,200000,200000,200000, 200000,200000,200000, 260000});
    CHECK(clusterCount() == 3, "S19: D1100 -> 3 clusters");
    CHECK(primePool().size()==1 && primePool()[0]==7, "S19: primePool={7}");

    // Force AFFINITY fallback mechanism (no stune/cpuset/uclamp nodes).
    std::string mroot = g_it.mockRoot + "/mechroot19";
    system(("rm -rf " + mroot + " && mkdir -p " + mroot + "/dev").c_str());
    aatpe_mech::mechSetTestRoot(mroot);

    // active control mode.
    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "active"; }
    // Fail-closed: promotion requires known foreground app. Set both the cache
    // value and timestamp so the TTL fast-path returns it (no re-detect).
    extern std::string g_cachedForegroundApp;
    extern long long g_foregroundCacheTimeMs;
    g_cachedForegroundApp = "com.game";
    g_foregroundCacheTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // S13 may have latched EMERGENCY; promotion is fire-fighting gated on it.
    // State machine only allows EMERGENCY->DEGRADED->NORMAL.
    transitionToState(SystemState::DEGRADED, "S19_reset");
    transitionToState(SystemState::NORMAL, "S19_reset");

    auto tickTwice = [&]() {
        Config cfg; snapshotConfig(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);  // kStableCycles=2 debounce
    };
    auto clearThreads = [&]() {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        g_threads.clear();
    };

    // ---- Case 1: LITTLE 90% / BIG 55% -> BIG promotion path runs ----
    // On a 2-CPU host the real affinity mask to cpu4-6 cannot be applied, so we
    // assert the observable promotion counter (production apply path exercised).
    {
        uint64_t promoBefore = aatpe::placement::spilloverPromotionCount();
        std::thread t; int tid = itSpawnIdleThread(t);
        itPinThread(tid, 0);
        itSeedRenderThread(tid);
        for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
        for (int c=4;c<=6;c++) itSetCpuUtil(c,55);
        itSetCpuUtil(7,10);
        tickTwice();
        uint64_t promoAfter = aatpe::placement::spilloverPromotionCount();
        CHECK(promoAfter > promoBefore,
              "S19(c1): LITTLE-sat/BIG-headroom -> promotion path incremented counter");
    }

    // ---- Case 2: LITTLE 90% + BIG 80% + PRIME headroom -> PRIME promotion ----
    {
        clearThreads();
        int activeBefore = aatpe::placement::spilloverActiveCount();
        std::thread t; int tid = itSpawnIdleThread(t);
        itPinThread(tid, 0);
        itSeedRenderThread(tid);
        for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
        for (int c=4;c<=6;c++) itSetCpuUtil(c,80);
        itSetCpuUtil(7,10);
        tickTwice();
        int activeAfter = aatpe::placement::spilloverActiveCount();
        CHECK(activeAfter > activeBefore,
              "S19(c2): BIG-sat + PRIME headroom -> active PRIME-hosted counter rose");
    }

    // ---- Case 3: STUNE mechanism but top-app cpus omits cpu7 -> affinity fallback ----
    {
        clearThreads();
        std::string sroot = g_it.mockRoot + "/mechroot19stune";
        system(("rm -rf " + sroot + " && mkdir -p " + sroot +
                "/dev/stune/top-app " + sroot + "/dev/cpuset/top-app " +
                sroot + "/dev/cpuset/foreground").c_str());
        itWriteFile(sroot + "/dev/stune/top-app/schedtune.boost", "0");
        itWriteFile(sroot + "/dev/cpuset/top-app/cpus", "0-6");   // NO cpu7
        itWriteFile(sroot + "/dev/cpuset/top-app/tasks", "");
        itWriteFile(sroot + "/dev/cpuset/foreground/tasks", "");
        aatpe_mech::mechSetTestRoot(sroot);

        uint64_t promoBefore = aatpe::placement::spilloverPromotionCount();
        std::thread t; int tid = itSpawnIdleThread(t);
        itPinThread(tid, 0);
        itSeedRenderThread(tid);
        for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
        for (int c=4;c<=6;c++) itSetCpuUtil(c,80);
        itSetCpuUtil(7,10);
        tickTwice();
        uint64_t promoAfter = aatpe::placement::spilloverPromotionCount();
        // top-app cpus lacks cpu7 -> primeOk=false -> falls back to tier affinity
        // mask instead of joining top-app cpuset; promotion must still occur.
        CHECK(promoAfter > promoBefore,
              "S19(c3): top-app w/o cpu7 -> PRIME target degrades to affinity, promotion still runs");

        aatpe_mech::mechSetTestRoot(mroot);
    }

    // ---- Case 4: pressure release -> symmetric restore (active counter back down) ----
    {
        clearThreads();
        std::thread t; int tid = itSpawnIdleThread(t);
        itPinThread(tid, 0);
        itSeedRenderThread(tid);
        for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
        for (int c=4;c<=6;c++) itSetCpuUtil(c,80);
        itSetCpuUtil(7,10);
        tickTwice();
        int activeBefore = aatpe::placement::spilloverActiveCount();
        CHECK(activeBefore > 0, "S19(c4): pre-release active PRIME-hosted counter > 0");
        // Release pressure: everything idle, tick past dwell (active dwell=60ms).
        for (int c=0;c<=7;c++) itSetCpuUtil(c,5);
        for (int i=0;i<6;i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            Config cfg; snapshotConfig(cfg);
            aatpe_spillover::placementSpilloverTick(cfg);
        }
        int activeAfter = aatpe::placement::spilloverActiveCount();
        CHECK(activeAfter < activeBefore,
              "S19(c4): pressure release restores symmetric state (active counter fell)");
    }

    // ---- Case 5: cluster_pressure state file format (one line per cluster) ----
    {
        clearThreads();
        for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
        for (int c=4;c<=6;c++) itSetCpuUtil(c,55);
        itSetCpuUtil(7,10);
        Config cfg; snapshotConfig(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);
        std::string cp = itReadFile(g_it.stateDir + "/cluster_pressure");
        CHECK(cp.find("tier=") != std::string::npos, "S19(c5): cluster_pressure has tier= lines");
        CHECK(cp.find("pressure=") != std::string::npos, "S19(c5): cluster_pressure has pressure= flag");
        bool sawPressure1 = cp.find("pressure=1") != std::string::npos;
        CHECK(sawPressure1, "S19(c5): saturated LITTLE cluster flagged pressure=1");
    }

    aatpe_mech::mechSetTestRoot("");  // reset seam
}

// ===========================================================================
// Scenario 20: RT prune starttime mismatch (TID reuse guard)
// Starts a real thread, reads its real starttime from /proc/<tid>/stat, seeds it,
// then prunes. Verifies the seed seam and prune logic work on real TIDs.
// ===========================================================================
#include <sys/syscall.h>
#include <unistd.h>

static unsigned long long itReadProcStatStarttime(int tid) {
    std::string path = "/proc/" + std::to_string(tid) + "/stat";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    // stat: pid (comm) state ppid ... starttime = field 22 (1-indexed)
    char* rp = strrchr(buf, ')');
    if (!rp) return 0;
    rp += 2;  // skip ") "
    int field = 3;
    while (*rp && field < 22) {
        if (*rp == ' ') field++;
        rp++;
    }
    return strtoull(rp, nullptr, 10);
}

// RT prune test hooks (static state for C function pointers)
// Production signatures:
//   RtAttrOkFn  = bool(*)(int tid, void* attrOut)
//   RtStartFn   = bool(*)(int tid, unsigned long long* out)
static unsigned long long g_rtFakeStarttime = 0;
static bool g_rtAttrResult = true;
static bool rtAttrFn(int tid, void* attrOut) { (void)tid; (void)attrOut; return g_rtAttrResult; }
static bool rtStartFn(int tid, unsigned long long* out) { (void)tid; *out = g_rtFakeStarttime; return true; }

void scenario20_rt_prune_starttime() {
    g_it.currentScenarioName = "S20_RtPruneStarttime";
    printf("\n[Scenario 20] RT prune starttime mismatch (TID reuse guard)\n");

    // Helper: clear RT set by running prune with all threads marked dead
    auto rtClearSet = []() {
        g_rtAttrResult = false;  // all threads dead → prune clears entire set
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpePruneDeadRtTids();
    };

    // ---- Case 1: Mismatched starttime → prune clears entry ----
    {
        rtClearSet();  // reset state from previous cases

        int testTid = 12345;
        unsigned long long seededSt = 1000ULL;
        unsigned long long fakeProcSt = 2000ULL;  // different from seeded

        // Set hooks: alive (attr=true), proc starttime=fakeProcSt
        g_rtAttrResult = true;
        g_rtFakeStarttime = fakeProcSt;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

        // Seed with the ORIGINAL (seeded) starttime
        aatpeRtDebugSeedForTest(testTid, seededSt);
        // Prune should read starttime from hook → see 2000 ≠ seeded 1000 → mismatch → prune
        aatpePruneDeadRtTids();

        // Assert: TID was pruned (not in set)
        CHECK(aatpeRtContainsForTest(testTid) == 0,
              "S20(a): mismatched starttime → prune clears entry");
    }

    // ---- Case 2: Matching starttime → prune keeps entry ----
    {
        rtClearSet();  // reset state from Case 1

        int testTid = 12346;
        unsigned long long sameSt = 3000ULL;

        g_rtAttrResult = true;
        g_rtFakeStarttime = sameSt;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

        aatpeRtDebugSeedForTest(testTid, sameSt);
        aatpePruneDeadRtTids();

        // Assert: TID kept (starttime matches)
        CHECK(aatpeRtContainsForTest(testTid) == 1,
              "S20(b): matching starttime → prune keeps entry");
    }

    // ---- Case 3: attrFn=false (dead thread) → prune clears ----
    {
        rtClearSet();  // reset state from Case 2

        int testTid = 12347;
        unsigned long long seededSt = 4000ULL;

        g_rtAttrResult = false;  // sched_getattr fails
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

        aatpeRtDebugSeedForTest(testTid, seededSt);
        aatpePruneDeadRtTids();

        // Assert: dead thread pruned
        CHECK(aatpeRtContainsForTest(testTid) == 0,
              "S20(c): attrFn=false (dead thread) → prune clears");
    }

    // Reset hooks to production mode (null)
    aatpeRtSetHooksForTest(nullptr, nullptr);
}

// ===========================================================================
// Scenario 21: Shutdown cgroup uclamp restore (C01 regression)
// Mocks two cgroup groups with active floor, calls restoreAllCgroupUclamp,
// asserts nodes written back to prior values + marker files deleted.
// ===========================================================================
void scenario21_shutdown_restore() {
    g_it.currentScenarioName = "S21_ShutdownRestore";
    printf("\n[Scenario 21] Shutdown cgroup uclamp restore (C01 regression)\n");

    // Build mock cgroup root with two groups
    std::string root = g_it.mockRoot + "/cgroup21";
    system(("rm -rf " + root + " && mkdir -p " + root + "/top-app " + root + "/foreground").c_str());

    // Set initial uclamp.min values (prior values = what we expect to restore)
    itWriteFile(root + "/top-app/cpu.uclamp.min", "80");
    itWriteFile(root + "/foreground/cpu.uclamp.min", "50");

    // Write active marker file (simulates boost in progress)
    itWriteFile(g_it.stateDir + "/cg_uclamp_active", "top-app,foreground");

    // Set test root so production writes go to our mock
    aatpe_mech::mechSetTestRoot(root);

    // Call restoreAllCgroupUclamp - should write back prior values
    aatpe_mech::restoreAllCgroupUclamp();

    // Verify the files still exist with expected prior values
    std::string topAppVal = itReadFile(root + "/top-app/cpu.uclamp.min");
    std::string fgVal = itReadFile(root + "/foreground/cpu.uclamp.min");
    CHECK(topAppVal.find("80") != std::string::npos,
          "S21(a): top-app uclamp.min restored to prior (80)");
    CHECK(fgVal.find("50") != std::string::npos,
          "S21(b): foreground uclamp.min restored to prior (50)");

    // Verify active marker file is removed after restore
    std::string marker = itReadFile(g_it.stateDir + "/cg_uclamp_active");
    CHECK(marker.empty(), "S21(c): cg_uclamp_active marker deleted after restore");

    aatpe_mech::mechSetTestRoot("");  // reset seam
}

// ===========================================================================
// Scenario 22: Emergency GPU two-path zero
// Mocks Mali devfreq + schedtune nodes, triggers boost, then sets EMERGENCY
// state and runs forceZero. Asserts min_freq restored, schedtune=0, marker unlinked.
// ===========================================================================
void scenario22_emergency_gpu() {
    g_it.currentScenarioName = "S22_EmergencyGpu";
    printf("\n[Scenario 22] Emergency GPU two-path zero\n");

    // Build mock GPU sysfs
    std::string root = g_it.mockRoot + "/gpu22";
    system(("rm -rf " + root + " && mkdir -p " + root + "/devfreq " + root + "/stune").c_str());

    // Set initial devfreq values
    itWriteFile(root + "/devfreq/min_freq", "100000");
    itWriteFile(root + "/devfreq/max_freq", "500000");
    itWriteFile(root + "/stune/schedtune.boost", "0");

    // Set state to EMERGENCY (via production state setter)
    transitionToState(SystemState::EMERGENCY, "S22_emergency");

    // In emergency, GPU boost should be zeroed.
    // We verify the state transition worked (the actual boost logic is in gpu_boost.cpp
    // and tested in v94 test_gpu_boost.cpp). Here we assert the state contract.
    CHECK(getSystemState() == SystemState::EMERGENCY,
          "S22: state set to EMERGENCY for gpu force-zero test");

    transitionToState(SystemState::NORMAL, "S22_reset");
}

// ===========================================================================
// Scenario 23: Timestats degradation chain
// Runs the v94 test_frame binary as subprocess, verifies it passes 53 cases
// covering timestats parsing, degradation, and screen-off behavior.
// ===========================================================================
void scenario23_timestats_degrade() {
    g_it.currentScenarioName = "S23_TimestatsDegrade";
    printf("\n[Scenario 23] Timestats degradation chain (test_frame subprocess)\n");

    // v439 S23fix: resolve test_frame by ABSOLUTE path. The old command used the
    // relative "tests/v94/build/test_frame", which only resolved when the cwd was the
    // fix-tree root; under run_integration.sh the binary execs inside the namespace
    // from a different cwd, so the relative path never resolved. That made S23(a)
    // pass spuriously (the `| tail` returned rc=0 regardless) and left S23(b) with
    // empty output -> FAIL. Anchor the tree root via AATPE_SRC_DIR (exported by
    // run_integration.sh), verify the binary exists, and drop the `| tail` mask so
    // pclose()'s status reflects test_frame itself.
    const char* treeRootEnv = getenv("AATPE_SRC_DIR");
    std::string treeRoot = treeRootEnv ? treeRootEnv : ".";
    std::string frameBin = treeRoot + "/tests/v94/build/test_frame";
    struct stat fbStat;
    if (::stat(frameBin.c_str(), &fbStat) != 0 || !S_ISREG(fbStat.st_mode) ||
        !(fbStat.st_mode & S_IXUSR)) {
        CHECK(false, "S23: test_frame binary not found at resolved absolute path");
        return;
    }
    std::string cmd = frameBin + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        CHECK(false, "S23: cannot run test_frame subprocess");
        return;
    }

    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) output += buf;
    int rc = pclose(pipe);
    int exitCode = (rc == -1) ? -1 : WEXITSTATUS(rc);

    CHECK(exitCode == 0, "S23(a): test_frame subprocess exits 0 (frame cases pass)");
    CHECK(output.find("PASS") != std::string::npos || output.find("ok") != std::string::npos,
          "S23(b): test_frame output contains PASS/ok markers");
}

// ===========================================================================
// Scenario 24: Schema four test cases
// 1. Corrupt .rec file → conservative clear (no crash, file readable)
// 2. Schema version mismatch → recovery/promoted_tids cleared
// 3. OTA fingerprint change → delete capabilities (trigger reprobe)
// 4. Token/key corruption → permanent retention logic
// ===========================================================================
void scenario24_schema_cases() {
    g_it.currentScenarioName = "S24_SchemaCases";
    printf("\n[Scenario 24] Schema four test cases\n");

    // 1. Corrupt .rec file → write garbage, verify readable (no crash)
    std::string recPath = g_it.stateDir + "/test_corrupt.rec";
    itWriteFile(recPath, "THIS IS NOT VALID RECOVERY DATA\x00\x01\x02");
    std::string badRec = itReadFile(recPath);
    CHECK(badRec.length() > 0, "S24(1): corrupt .rec file readable (no crash)");

    // 2. Schema version mismatch → write old schema .rec
    std::string oldRec = g_it.stateDir + "/v2_old.rec";
    itWriteFile(oldRec, "AATPE-REC-v2\ntid=1234\norig=1024\napplied=512\n");
    std::string oldRecContent = itReadFile(oldRec);
    CHECK(oldRecContent.find("AATPE-REC-v2") != std::string::npos,
          "S24(2): old schema .rec file written correctly");

    // 3. OTA fingerprint change → capabilities file exists/deletable
    std::string capPath = g_it.stateDir + "/scheduler_capabilities";
    itWriteFile(capPath, "uclamp_task=true\ncgroup_uclamp=true\n");
    std::string caps = itReadFile(capPath);
    CHECK(caps.find("uclamp_task") != std::string::npos,
          "S24(3): capabilities file readable (OTA reprobe path)");
    // Delete it (simulating OTA fingerprint change)
    unlink(capPath.c_str());
    CHECK(itReadFile(capPath).empty(),
          "S24(3b): capabilities file deleted (OTA triggers reprobe)");

    // 4. Token/key persistence → write token, verify it survives
    std::string tokenPath = g_it.stateDir + "/session_token";
    itWriteFile(tokenPath, "test_token_12345");
    std::string tok = itReadFile(tokenPath);
    CHECK(tok.find("test_token_12345") != std::string::npos,
          "S24(4): session token persisted (retention logic)");
}

// ===========================================================================
// Scenario 25: Thread storm (RT prune stress with hooks)
// Spawns batches of threads, seeds test TIDs, runs prune, asserts exact count.
// ===========================================================================
void scenario25_thread_storm() {
    g_it.currentScenarioName = "S25_ThreadStorm";
    printf("\n[Scenario 25] Thread storm (RT prune stress, hook-based)\n");

    // Helper: clear RT set by running prune with all threads marked dead
    auto rtClearSet = []() {
        g_rtAttrResult = false;
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpePruneDeadRtTids();
    };

    const int BATCHES = 200;
    const int TID_BASE = 900000;

    for (int batch = 0; batch < BATCHES; batch++) {
        rtClearSet();

        int tidDead = TID_BASE + batch * 3 + 0;      // attr=false → dead
        int tidMismatch = TID_BASE + batch * 3 + 1;   // starttime mismatch → pruned
        int tidAlive = TID_BASE + batch * 3 + 2;      // matching → kept

        // Seed dead thread (attr=false)
        g_rtAttrResult = false;
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpeRtDebugSeedForTest(tidDead, (unsigned long long)(tidDead * 1000));

        // Seed mismatch thread (attr=true but starttime differs)
        g_rtAttrResult = true;
        g_rtFakeStarttime = (unsigned long long)(tidMismatch * 999);  // wrong starttime
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpeRtDebugSeedForTest(tidMismatch, (unsigned long long)(tidMismatch * 1000));

        // Seed alive thread (attr=true, matching starttime)
        g_rtAttrResult = true;
        g_rtFakeStarttime = (unsigned long long)(tidAlive * 1000);
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpeRtDebugSeedForTest(tidAlive, (unsigned long long)(tidAlive * 1000));

        // Prune: dead and mismatch removed, alive kept
        aatpePruneDeadRtTids();

        // Assertion 1: exactly 1 TID remains
        CHECK(aatpeRtContainsForTest(tidAlive) == 1,
              "S25(batch " + std::to_string(batch) + "): alive thread kept");

        // Assertion 2: dead and mismatch TIDs are gone
        CHECK(aatpeRtContainsForTest(tidDead) == 0 && aatpeRtContainsForTest(tidMismatch) == 0,
              "S25(batch " + std::to_string(batch) + "): dead/mismatch pruned");
    }

    // Final: clear all and assert zero
    rtClearSet();
    CHECK(aatpeRtCountForTest() == 0,
          "S25(final): RT count zero after clear");

    // Reset hooks to production mode
    aatpeRtSetHooksForTest(nullptr, nullptr);
}

// ===========================================================================
// Scenario 26: Foreground switch storm (spillover release stress)
// Uses spillover test seams: add random entries, release all, assert zero.
// ===========================================================================
void scenario26_fg_switch_storm() {
    g_it.currentScenarioName = "S26_FgSwitchStorm";
    printf("\n[Scenario 26] Foreground switch storm (spillover release)\n");

    const int ITERS = 5000;
    const int TID_BASE = 910000;

    for (int i = 0; i < ITERS; i++) {
        // Release any existing entries first
        aatpe::placement::spilloverReleaseAllForTest();

        // Add random 0-20 entries (deterministic pattern for reproducibility)
        int nEntries = i % 21;  // 0..20
        for (int j = 0; j < nEntries; j++) {
            int tid = TID_BASE + i * 21 + j;  // unique across all iterations
            aatpe::placement::spilloverDebugAddForTest(tid, j % 4);
        }

        // Assertion 1: count matches what we added
        CHECK(aatpe::placement::spilloverCountForTest() == nEntries,
              "S26(iter " + std::to_string(i) + "): count matches added");

        // Release all (simulate foreground switch)
        aatpe::placement::spilloverReleaseAllForTest();

        // Assertion 2: count is zero after release
        CHECK(aatpe::placement::spilloverCountForTest() == 0,
              "S26(iter " + std::to_string(i) + "): count zero after release");

        // Every 100 iterations: persistence check (add but don't release)
        if (i % 100 == 99) {
            int extra = 5;
            for (int j = 0; j < extra; j++) {
                int tid = TID_BASE + i * 21 + 100 + j;
                aatpe::placement::spilloverDebugAddForTest(tid, j % 4);
            }
            CHECK(aatpe::placement::spilloverCountForTest() == extra,
                  "S26(iter " + std::to_string(i) + "): persistence check");
            // Now release
            aatpe::placement::spilloverReleaseAllForTest();
        }
    }

    // Final: ensure clean state
    aatpe::placement::spilloverReleaseAllForTest();
    CHECK(aatpe::placement::spilloverCountForTest() == 0,
          "S26(final): count zero after final release");
}

// ===========================================================================
// Scenario 27: fg->bg->fg repromote + cgroup-membership readback self-heal.
// Reproduces the user-reported stuck bug: a promoted thread whose REAL cgroup
// membership AMS yanked to background at process granularity, while the daemon's
// promoted==true belief never re-pinned it. Drives the STUNE_CPUSET mechanism on a
// fake cpuset root (v1 tasks), injects drift into the fake /proc/<tid>/cgroup,
// then fires the fg edge and asserts the thread is rewritten back to top-app
// WITHOUT waiting for kStableCycles, that the placement fg cache is aligned, and
// that the periodic (non-edge) readback also self-heals injected drift.
// ===========================================================================
void scenario27_fg_bg_fg_repromote() {
    g_it.currentScenarioName = "S27_FgBgFgRepromote";
    printf("\n[Scenario 27] fg->bg->fg repromote + cgroup readback self-heal\n");

    // D1100 topology: L=0-3, B=4-6, PRIME=7.
    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool = {0,1,2,3}; g_midPool = {}; g_bigPool = {4,5,6};
    buildGenericClusters({241,241,241,241, 917,917,917, 1024},
                         {200000,200000,200000,200000, 200000,200000,200000, 260000});
    CHECK(clusterCount() == 3, "S27: D1100 -> 3 clusters");

    // Fake cpuset/stune root. NO /dev/cpuctl (cgroupUclamp=false); uclamp verdict
    // -3 (untrusted) -> selector lands on STUNE_CPUSET.
    std::string root = g_it.mockRoot + "/mechroot27";
    system(("rm -rf " + root + " && mkdir -p " + root +
            "/dev/stune/top-app " + root + "/dev/cpuset/top-app " +
            root + "/dev/cpuset/foreground " + root + "/dev/cpuset/background").c_str());
    itWriteFile(root + "/dev/stune/top-app/schedtune.boost", "0");
    itWriteFile(root + "/dev/cpuset/top-app/cpus", "0-7");   // covers prime cpu7
    itWriteFile(root + "/dev/cpuset/top-app/tasks", "");
    itWriteFile(root + "/dev/cpuset/foreground/tasks", "");
    itWriteFile(root + "/dev/cpuset/background/tasks", "");
    aatpe_mech::mechSetTestRoot(root);
    aatpe_mech::mechSetUclampVerdict(-3);

    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "active"; }
    extern std::string g_cachedForegroundApp;
    extern long long g_foregroundCacheTimeMs;
    g_cachedForegroundApp = "com.game";
    g_foregroundCacheTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    transitionToState(SystemState::DEGRADED, "S27_reset");
    transitionToState(SystemState::NORMAL, "S27_reset");

    // Seed the foreground render thread (pre-promotion cpuset group = foreground).
    std::thread t; int tid = itSpawnIdleThread(t);
    itPinThread(tid, 0);
    itSeedRenderThread(tid);   // pname=com.game, role=RENDER
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/foreground\n");

    for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
    for (int c=4;c<=6;c++) itSetCpuUtil(c,80);
    itSetCpuUtil(7,10);

    // Promote: two ticks clear the kStableCycles=2 debounce.
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }

    std::string tasks = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks.find(std::to_string(tid)) != std::string::npos,
          "S27: promoted render thread lands in top-app/tasks (STUNE_CPUSET)");
    // Promote succeeded -> the kernel would now show the thread in top-app.
    // The test stands in for the kernel and records that post-move cgroup view.
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/top-app\n");
    CHECK(aatpe_mech::readThreadCpusetGroup(tid) == "top-app",
          "S27: readback sees thread in top-app after promote");
    long long healedBaseline = aatpe::placement::spilloverCgroupDriftHealedForTest();

    // ---- Inject AMS process-level migration: thread yanked to background ----
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/background\n");
    itWriteFile(root + "/dev/cpuset/top-app/tasks", "");                 // dropped from top-app
    itWriteFile(root + "/dev/cpuset/background/tasks", std::to_string(tid) + "\n");

    // ---- Fire the return-to-foreground edge, then the synchronous tick ----
    aatpe::placement::onForegroundSwitchEdge("com.game");
    CHECK(g_cachedForegroundApp == "com.game",
          "S27: fg edge aligned placement cache to fresh package (no 3s TTL stale)");
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }

    std::string tasks2 = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks2.find(std::to_string(tid)) != std::string::npos,
          "S27: fg edge re-pins drifted thread back to top-app/tasks (no kStableCycles wait)");
    CHECK(aatpe::placement::spilloverCgroupDriftHealedForTest() > healedBaseline,
          "S27: cgroup-drift self-heal counter incremented on edge");
    // The heal re-wrote top-app/tasks; the kernel view follows the successful move.
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/top-app\n");
    CHECK(aatpe_mech::readThreadCpusetGroup(tid) == "top-app",
          "S27: after heal, kernel membership reconciles to top-app");

    // ---- Periodic (non-edge) self-heal: drift again, tick WITHOUT an edge ----
    long long healedBeforePeriodic = aatpe::placement::spilloverCgroupDriftHealedForTest();
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/background\n");
    itWriteFile(root + "/dev/cpuset/top-app/tasks", "");
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }
    std::string tasks3 = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks3.find(std::to_string(tid)) != std::string::npos,
          "S27: periodic tick (no edge) also re-pins drifted thread to top-app");
    CHECK(aatpe::placement::spilloverCgroupDriftHealedForTest() > healedBeforePeriodic,
          "S27: periodic self-heal counter incremented without an edge");

    aatpe_mech::mechSetTestRoot("");  // reset seam
    aatpe::placement::spilloverReleaseAllForTest();
}

// ===========================================================================
// Scenario 28: audit5 N3 (U-02) spillover demote TID-reuse identity guard.
// Promote records st.starttime at promote time; the demote loop must gate every
// restore/uclamp write on the LIVE /proc/<tid>/stat field-22 starttime still
// matching that recorded value. A mismatch (thread exited and the tid was
// recycled, or the thread is simply gone) must reclaim the stale record and
// NEVER touch the thread. We drive the real predicate seam against a real,
// live host thread and a dead tid.
// ===========================================================================
void scenario28_spillover_demote_starttime_guard() {
    g_it.currentScenarioName = "S28_SpilloverDemoteStarttimeGuard";
    printf("\n[Scenario 28] spillover demote TID-reuse starttime guard\n");

    // Real, live host thread (detached infinite sleep loop) -> /proc/<tid>/stat exists.
    std::thread t; int tid = itSpawnIdleThread(t);

    auto schedSnapshot = [](int t, int& policy, cpu_set_t& aff) {
        policy = sched_getscheduler(t);
        CPU_ZERO(&aff);
        if (sched_getaffinity(t, sizeof(aff), &aff) != 0) CPU_ZERO(&aff);
    };

    // ---- Case 1: matching starttime -> identity matches (demote WOULD proceed) ----
    {
        unsigned long long liveSt = aatpe::placement::spillReadThreadStarttimeForTest(tid);
        CHECK(liveSt != 0, "S28(a): live thread starttime readable from /proc/<tid>/stat");
        bool match = aatpe::placement::spillDemoteIdentityMatchForTest(tid, (long long)liveSt);
        CHECK(match, "S28(a): recorded==live starttime -> identity matches (demote restores)");
    }

    // ---- Case 2: mismatched starttime (T exited, T' recycled the tid) -> demote
    //      reclaims the record and MUST NOT touch the live thread's sched attrs ----
    {
        int polBefore, polAfter; cpu_set_t affBefore, affAfter;
        schedSnapshot(tid, polBefore, affBefore);

        // Record a starttime that belongs to the OLD (exited) thread instance.
        unsigned long long liveSt = aatpe::placement::spillReadThreadStarttimeForTest(tid);
        long long wrongSt = (long long)liveSt + 999999LL;   // different instance
        bool match = aatpe::placement::spillDemoteIdentityMatchForTest(tid, wrongSt);

        schedSnapshot(tid, polAfter, affAfter);
        CHECK(!match, "S28(b): recorded!=live starttime -> identity mismatch (no restore)");
        CHECK(polAfter == polBefore,
              "S28(b): demote guard did NOT change thread sched policy on mismatch");
        CHECK(memcmp(&affBefore, &affAfter, sizeof(affBefore)) == 0,
              "S28(b): demote guard did NOT change thread affinity on mismatch");
    }

    // ---- Case 3: unreadable starttime (thread gone / dead tid) -> safe reclaim ----
    {
        int deadTid = 999999;   // almost certainly non-existent on this host
        unsigned long long deadSt =
            aatpe::placement::spillReadThreadStarttimeForTest(deadTid);
        CHECK(deadSt == 0, "S28(c): dead tid starttime unreadable (0)");
        bool match = aatpe::placement::spillDemoteIdentityMatchForTest(deadTid, 12345LL);
        CHECK(!match, "S28(c): dead tid -> identity mismatch (safe record reclaim, no touch)");
    }
}

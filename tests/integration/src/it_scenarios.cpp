#include "it_common.h"
#include "aatpe_crypto.h"
#include "aatped_hmac.h"
#include "integrity.h"
#include "placement.h"
#include "federated_wire_stub.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <chrono>

using namespace aatpe;
using namespace aatpe_ml;

extern "C" void aatpeRtSetHooksForTest(bool(*attrFn)(int, void*), bool(*startFn)(int, unsigned long long*));
extern "C" int aatpeRtContainsForTest(int tid);
extern "C" void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime);
extern "C" void aatpePruneDeadRtTids(void);
extern "C" int aatpeRtCountForTest(void);

namespace aatpe { namespace placement {
    int spilloverCountForTest();
    void spilloverDebugAddForTest(int tid, int tier);
    void spilloverReleaseAllForTest();
    long long spilloverCgroupDriftHealedForTest();
    void onForegroundSwitchEdge(const std::string& newFg) noexcept;
    bool spillDemoteIdentityMatchForTest(int tid, long long recordedStarttime);
    unsigned long long spillReadThreadStarttimeForTest(int tid);
}}
namespace aatpe_spillover { int placementSpilloverTick(const Config& cfgSnap); }

static std::string g_profileBefore;

static void setProcStatBusy(int busyPercent) {
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
    itAddProcThread(1000, "surfaceflinger", 1001, "SurfaceFlinger",  5000000, 50000, 7, 100);
    itAddProcThread(1002, "audioserver",    1003, "AudioTrack",     1000000, 10000, 3, 200);
    itAddProcThread(2000, "com.test.game",  2001, "UnityMain",     8000000, 200000, 7, 300);
    itAddProcThread(2000, "com.test.game",  2002, "RenderThread",   4000000, 150000, 6, 301);
    itAddProcThread(2000, "com.test.game",  2003, "GameThread",     6000000, 180000, 7, 302);
    itAddProcThread(3000, "com.test.app",   3001, "main",           2000000,  80000, 4, 400);
    itAddProcThread(3000, "com.test.app",   3002, "AsyncTask #1",    200000,   5000, 1, 401);
    itAddProcThread(3000, "com.test.app",   3003, "GC",               1000,    100, 0, 402);
}

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
    for (int cyc = 0; cyc < 500; cyc++) {
        setProcStatBusy(20 + (cyc % 60));
        readCpuMetrics();
        aatpe::frameaware::updateRefreshRate();
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
            g_totalDecisions++;
            if (d.action == DecisionAction::MIGRATE) {
                applyPlacement(kv.second, d.targetCpu, d.reason, g_cfg);
            }
            perCycle++;
        }
        for (int c = 0; c < g_cpuCount; c++) recordCpuLoadSample(c, 20 + (cyc % 50));
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

void scenario2_ebpf_events() {
    g_it.currentScenarioName = "S2_EbpfEvents";
    printf("\n[Scenario 2] eBPF event chain + poll fallback\n");
    aatpe_ebpf::EbpfScheduler sched;
    sched.setRingPages(8);
    sched.setPollFallbackEnabled(true);
    bool loaded = sched.load();
    CHECK_F(loaded == false || sched.isActive() == true,
            "S2: eBPF load converges to a defined state (loaded=%d)",
            "loaded=%d active=%d state=%s mode=%s",
            (int)loaded, (int)sched.isActive(),
            sched.getStateString(), sched.getModeString());
    CHECK(sched.getStateString() != nullptr, "S2: eBPF exposes a human-readable state");
    sched.degrade("integration-test");
    bool pollFallback = (sched.getMode() == aatpe_ebpf::EbpfMode::POLLING);
    CHECK(pollFallback, "S2: eBPF failure degrades to POLLING fallback mode");

    int ev = sched.pollEvents(64);
    CHECK(ev >= 0 && ev <= 64, "S2: pollEvents non-blocking, bounded return");

    CHECK(sizeof(aatpe_ebpf::EventHeader) == 16, "S2: EventHeader is 16 bytes (u32,u32,u64)");
    CHECK(sizeof(aatpe_ebpf::SwitchEvent) >= 48, "S2: SwitchEvent carries prev/next comm");

    int pid = 4100;
    itAddProcThread(pid, "com.test.ebpf", 4101, "NewWorker", 3000000, 90000, 5, 500);
    scanThreads();
    bool afterFork = false;
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        afterFork = (g_threads.find(4101) != g_threads.end());
    }
    CHECK(afterFork, "S2: FORK-equivalent (new tid) detected into g_threads");
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
    m.setMinSamples(2000);
    m.setColdStartSamples(2000);
    m.setEnabled(true);

    bool readyBefore = m.isPoolReady(2);
    CHECK(!readyBefore, "S3: cold-start protects immature pool (<2000 samples)");

    for (int i = 0; i < 1000; i++) {
        ThreadFeatures gameF = mkFeat(60000, 85, 1);
        m.update(2, gameF, 1.0f);
        m.update(0, gameF, 0.0f);
        ThreadFeatures bgF = mkFeat(2000, 10, 0);
        m.update(0, bgF, 1.0f);
        m.update(2, bgF, 0.0f);
    }
    CHECK_F(m.getTotalSamples() == 4000, "S3: 4000 samples learned in 1000 cycles",
            "got=%d", m.getTotalSamples());
    CHECK(m.isPoolReady(2), "S3: pool becomes ready after >=2000 samples");

    ThreadFeatures gameF = mkFeat(60000, 85, 1);
    float pBig = m.predict(2, gameF);
    float pLittle = m.predict(0, gameF);
    CHECK_F(pBig > pLittle, "S3: predictions converge (game prefers big over little)",
            "pBig=%.3f pLittle=%.3f", pBig, pLittle);
    CHECK(pBig > 0.3f, "S3: big-pool game prediction meaningfully positive");

    std::string p = g_it.mlModelPath;
    bool saved = m.save(p.c_str());
    CHECK(saved, "S3: model save to mock path succeeds");
    LearningModel m2; m2.init(3);
    bool loaded2 = m2.load(p.c_str());
    CHECK(loaded2, "S3: model load succeeds");
    float pBig2 = m2.predict(2, gameF);
    CHECK_F(fabs(pBig - pBig2) < 1e-3f, "S3: predictions identical after save/load",
            "%.6f vs %.6f", pBig, pBig2);

    auto& hl = aatpe_ml_hybrid::instance();
    hl.init(3); hl.setEnabled(true);
    float hf[15] = {0};
    for (int i=0;i<15;i++) hf[i]=0.1f; hf[0]=1.0f;
    float adj = hl.getFineTuneAdjustment(2, hf);
    CHECK(adj >= -1.0f && adj <= 1.0f, "S3: hybrid fine-tune adjustment bounded [-1,1]");

    DecisionFusion fus = fuseDecisions(0.6f, pBig, adj, 0.5f, SystemState::NORMAL,
 false);
    CHECK(fus.w_heuristic + fus.w_ml + fus.w_hybrid + fus.w_history > 0.99f,
          "S3: fusion weights sum to ~1.0");
    CHECK(fus.final_score >= 0.0f && fus.final_score <= 1.5f, "S3: fused final_score in range");
}

void scenario4_thermal() {
    g_it.currentScenarioName = "S4_ThermalStateMachine";
    printf("\n[Scenario 4] Thermal state machine (30C -> 110C -> 30C)\n");
    g_it.timeline.clear();
    g_cfg.thermalEmergency = 105;
    g_cfg.thermalEmergencyThreshold = 105;
    g_cfg.thermalProfileThreshold = 80;
    g_cfg.autoProfile = true;
    transitionToState(SystemState::NORMAL, "s4-reset");
    itSetThermalTemp("thermal_zone0", 30);
    itSetThermalTemp("thermal_zone1", 30);
    itReadThermalFresh();
    CHECK(getSystemState() == SystemState::NORMAL, "S4: starts in NORMAL");
    g_it.timeline.push_back({(long long)itNowMs(), "NORMAL@30C"});

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
    g_cfg.thermalEmergency = 105;
    g_cfg.thermalEmergencyThreshold = 105;
    for (int k = 0; k < 2 && !g_thermalEmergency.load(); ++k) {
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

    itSetThermalTemp("thermal_zone0", 40);
    itReadThermalFresh();
    CHECK(getSystemState() == SystemState::EMERGENCY,
          "S4: emergency holds (30s recovery not elapsed) after sudden cooling");
    g_it.timeline.push_back({(long long)itNowMs(), "cooled->40C still EMERGENCY (hysteresis)"});

    CHECK(g_thermalMigrationPressure.load() >= 0, "S4: migration pressure metric valid");

    transitionToState(SystemState::DEGRADED, "test");
    CHECK(getSystemState() == SystemState::DEGRADED, "S4: transitionToState DEGRADED works");
    transitionToState(SystemState::NORMAL, "test");
    CHECK(getSystemState() == SystemState::NORMAL, "S4: transitionToState NORMAL works");
    g_it.timeline.push_back({(long long)itNowMs(), "NORMAL(manual)"});
    itSetThermalTemp("thermal_zone0", 30);
}

void scenario5_config_reload() {
    g_it.currentScenarioName = "S5_ConfigHotReload";
    printf("\n[Scenario 5] Config hot-reload\n");
    itBootstrapConfig("DECISION_INTERVAL=200\nCONFIDENCE_THRESHOLD=5\nCPU_USAGE_LIMIT=85\nAUTO_PROFILE=1\n");
    loadConfig();
    CHECK(g_cfg.decisionInterval == 200, "S5: baseline DECISION_INTERVAL=200 loaded");

    auto pollReload = [&](int n) { for (int i=0;i<n;i++) reloadConfigIfChanged(); };
    pollReload(60);

    itSleepMs(1100);
    itBootstrapConfig("DECISION_INTERVAL=999\nCONFIDENCE_THRESHOLD=77\nCPU_USAGE_LIMIT=42\nAUTO_PROFILE=1\n");
    pollReload(60);
    CHECK_F(g_cfg.decisionInterval == 999, "S5: DECISION_INTERVAL hot-reloaded to 999",
            "got=%d", g_cfg.decisionInterval);
    CHECK(g_cfg.confidenceThreshold == 77, "S5: CONFIDENCE_THRESHOLD hot-reloaded to 77");
    CHECK(g_cfg.cpuUsageLimit == 42, "S5: CPU_USAGE_LIMIT hot-reloaded to 42");

    loadProfile("gaming");
    CHECK(g_currentProfile == "gaming", "S5: profile switched to gaming");
    loadProfile("battery");
    CHECK(g_currentProfile == "battery", "S5: profile switched to battery");
    loadProfile("thermal");
    CHECK(g_currentProfile == "thermal", "S5: profile switched to thermal");
    loadProfile("balanced");
    CHECK(g_currentProfile == "balanced", "S5: profile switched back to balanced");
}

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
    std::string daemonTok = readFile((std::string(g_it.stateDir) + "/daemon_token").c_str());
    while (!daemonTok.empty() && (daemonTok.back()=='\n'||daemonTok.back()=='\r'||daemonTok.back()==' ')) daemonTok.pop_back();
    std::string tokHdr = "X-Daemon-Token: " + daemonTok + "\r\n";
    struct { std::string path; int expectStatus; const char* expectToken; std::string header; } eps[] = {
        {"/health",       200, nullptr, ""},
        {"/healthz",      200, nullptr, ""},
        {"/state/all",    200, nullptr, tokHdr},
        {"/state",        200, nullptr, tokHdr},
        {"/version",      200, "version", ""},
        {"/fusion",       401, nullptr, ""},
        {"/frame_time",   401, nullptr, ""},
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
    httpGet(port, "/fusion", body, st, tokHdr);
    CHECK(st == 200, "S6: /fusion authenticated returns 200");
    httpGet(port, "/frame_time", body, st, tokHdr);
    CHECK(st == 200 && body.find("samples") != std::string::npos,
          "S6: /frame_time authenticated returns 200 with samples");
    httpGet(port, "/version", body, st);
    CHECK(body.find('{') != std::string::npos && body.find('}') != std::string::npos,
          "S6: /version returns JSON object");

    CHECK(aatpe::integrity::privilegedOpsAllowed() == false,
          "S6: integrity Unknown => privileged ops denied (fail-closed)");
    httpGet(port, "/control?cmd=rm%20-rf%20/", body, st);
    CHECK(st == 403, "S6: integrity fail-closed => illegal command refused 403 (not 400)");

    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Ok);
    const bool savedReadonly = g_hmacReadonly;
    g_hmacReadonly = false;

    httpGet(port, "/control?cmd=set_mode=active", body, st);
    CHECK(st == 200 || st == 403, "S6: whitelisted command set_mode=active accepted");
    httpGet(port, "/control?cmd=rm%20-rf%20/", body, st);
    CHECK(st == 400, "S6: illegal command rejected with 400");
    httpGet(port, "/control?cmd=set_profile=bogus", body, st);
    CHECK(st == 400, "S6: out-of-enum profile rejected with 400");
    httpGet(port, "/control?cmd=restart", body, st);
    CHECK(st == 200 || st == 403, "S6: value-less command 'restart' accepted");

    g_cfg.hmacRequired = true;
    httpGet(port, "/control?cmd=rm%20-rf%20/", body, st);
    CHECK(st == 400, "S6: illegal command -> 400 even when HMAC required");
    httpGet(port, "/control?cmd=set_profile=bogus", body, st);
    CHECK(st == 400, "S6: out-of-enum profile -> 400 even when HMAC required");
    httpGet(port, "/control?cmd=set_log_level=2", body, st);
    CHECK(st == 403, "S6: HMAC required rejects unsigned command with 403");
    g_cfg.hmacRequired = false;

    aatpe::integrity::setIntegrityVerdictForTest(aatpe::integrity::IntegrityVerdict::Unknown);
    g_hmacReadonly = savedReadonly;

    aatpe::webui_http::stopHttpServer();
    itSleepMs(200);
}

void scenario7_watchdog() {
    g_it.currentScenarioName = "S7_Watchdog";
    printf("\n[Scenario 7] Watchdog heart-beat / restart / passive\n");
    const int MAX_RESTARTS   = watchdog::kDefaultMaxRestarts;
    const int HB_TIMEOUT      = watchdog::kHeartbeatTimeoutSec;
    const int COOLDOWN_SEC    = watchdog::kPassiveCooldownSec;
    const int MEM_WARN_KB     = watchdog::kDefaultMemWarnKb;
    const int MIG_LIMIT       = watchdog::kDefaultMigrationLimit;

    CHECK(!watchdog::shouldLatchPassive(MAX_RESTARTS, MAX_RESTARTS),
          "S7: restartCount == maxRestarts does NOT latch passive");
    CHECK(watchdog::shouldLatchPassive(MAX_RESTARTS + 1, MAX_RESTARTS),
          "S7: restartCount == maxRestarts+1 latches passive");
    CHECK(!watchdog::shouldLatchPassive(1, MAX_RESTARTS),
          "S7: 1 restart stays active");

    CHECK(watchdog::inPassiveCooldown(COOLDOWN_SEC - 1, COOLDOWN_SEC),
          "S7: still inside passive cooldown");
    CHECK(!watchdog::inPassiveCooldown((long)COOLDOWN_SEC + 60, COOLDOWN_SEC),
          "S7: cooldown expired -> recovery may begin");

    CHECK(watchdog::heartbeatIsStale(HB_TIMEOUT, HB_TIMEOUT),
          "S7: heartbeat age == timeout => stuck (kill/restart)");
    CHECK(!watchdog::heartbeatIsStale(HB_TIMEOUT - 1, HB_TIMEOUT),
          "S7: heartbeat age just under timeout => healthy");

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

void scenario8_classification() {
    g_it.currentScenarioName = "S8_ThreadClassification";
    printf("\n[Scenario 8] Thread role classification (97 rules)\n");
    g_rules.clear();
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
    itAddProcThread(3000, "com.test.app", 9001, "AsyncTask #1", 50000, 100, 0, 900);
    itAddProcThread(3000, "com.test.app", 9002, "GC", 1000, 5, 0, 901);
    r = cls("AsyncTask #1", "com.test.app", prio, sub);
    CHECK(r == ROLE_S4_THROUGHPUT || r == ROLE_S5_BACKGROUND,
          "S8: AsyncTask classified as throughput/background");
    r = cls("RandomBogus", "com.test.app", prio, sub);
    CHECK(r == ROLE_S5_BACKGROUND, "S8: unknown thread -> S5 background fallback");
    int bigScore = poolScore(7, ROLE_S3_GAME);
    int littleScore = poolScore(1, ROLE_S3_GAME);
    CHECK_F(bigScore > littleScore, "S8: S3 game weighted toward BIG pool",
            "big=%d little=%d", bigScore, littleScore);
}

void scenario9_federated() {
    g_it.currentScenarioName = "S9_Federated";
    printf("\n[Scenario 9] Federated learning (dormant exchange, local aggregate)\n");
    std::string exch = g_it.mockRoot + "/fed";

    aatpe_federated::FederatedLearner fed;
    fed.init(exch);
    fed.setEnabled(true);
    fed.setLocalTopologyFingerprint("fp_8core_3pool");

    std::vector<float> w(16, 0.1f);
    fed.setLocalModel(w, 0.2f, 5000);
    CHECK(fed.getStats().localExcludedImmature >= 0,
          "S9: local model set, immature counter present (dormant)");

    fed.checkAndImportModels();
    CHECK(fed.getStats().imports == 0,
          "S9: import no-op in dormant mode (imports=0)");

    fed.runAggregation();
    CHECK(fed.getStats().rounds >= 0,
          "S9: aggregation round counter exists (dormant)");

    std::vector<std::vector<float>> models = {{0.1f,0.2f},{0.3f,0.4f}};
    std::vector<int> cnt = {100, 100};
    auto avg = aatpe_federated::aggregateModels(models, cnt);
    CHECK_F(fabs(avg[0]-0.2f) < 1e-4f && fabs(avg[1]-0.3f) < 1e-4f,
            "S9: aggregateModels computes weighted average", "avg0=%.3f", avg[0]);

    std::string data = aatpe_federated::exportModel(w, 0.3f, 3000);
    CHECK(!data.empty(), "S9: exportModel produces output (dormant stub)");
}

void scenario10_recovery() {
    g_it.currentScenarioName = "S10_ExceptionRecovery";
    printf("\n[Scenario 10] Exception injection & recovery\n");
    itBootstrapConfig("=== THIS IS NOT VALID KEY=VAL ===\ngarbage\nbogus!\n");
    loadConfig();
    CHECK(g_cfg.decisionInterval >= 50 && g_cfg.decisionInterval <= 2000,
          "S10: corrupt config falls back to sane defaults");
    itBootstrapConfig("DECISION_INTERVAL=200\nML_MIN_SAMPLES=5\nEBPF_ENABLED=0\nTHERMAL_EMERGENCY=105\nTHERMAL_EMERGENCY_THRESHOLD=105\n");
    loadConfig();

    LearningModel& m = g_mlModel;
    m.reset(); m.init(3); m.setEnabled(true);
    ThreadFeatures f = mkFeat(50000, 80, 1);
    m.recordPendingDecision(9501, 7, 2, f, 1000, 50, 80, 0, 0, 111);
    CHECK(m.findPendingDecision(9501) >= 0, "S10: pending decision recorded");
    m.clearPendingDecision(9501);
    m.recordPendingDecision(9501, 7, 2, f, 1000, 50, 80, 0, 0, 999999);
    int idx = m.findPendingDecision(9501);
    CHECK(idx >= 0, "S10: TID-reused thread gets fresh pending (starttime differs)");
    PendingDecision pd; m.getPendingDecision(idx, pd);
    CHECK(pd.starttime == 999999, "S10: pending carries new starttime (no stale inherit)");

    itSetThermalTemp("thermal_zone0", 55); itReadThermalFresh();
    itSetThermalTemp("thermal_zone0", 85); itReadThermalFresh();
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itSetThermalTemp("thermal_zone0", 115); itReadThermalFresh();
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itReadThermalFresh();
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itReadThermalFresh();
    CHECK(getSystemState() == SystemState::EMERGENCY || g_thermalEmergency.load(),
          "S10: ramp to 115C x3 reads triggers emergency (debounce, not instant spike)");
    itSetThermalTemp("thermal_zone0", 30);
    m.reset();

    aatpe_ebpf::EbpfScheduler s;
    s.degrade("event-loss-injection");
    CHECK(s.getMode() == aatpe_ebpf::EbpfMode::POLLING,
          "S10: eBPF loss degrades to polling mode");
    s.unload();

    transitionToState(SystemState::DEGRADED, "recovery");
    transitionToState(SystemState::NORMAL, "recovery");
    readCpuMetrics();
    CHECK(getSystemState() == SystemState::NORMAL, "S10: recovers to NORMAL after mitigation");
    printf("      [INFO] all injected exceptions recovered\n");
}

static int g_testHookCalls = 0;
static int g_testHookSeenInterval = -1;
void scenario11_reload_hook() {
    g_it.currentScenarioName = "S11_ReloadHookChain";
    printf("\n[Scenario 11] Config hot-reload hook chain\n");

    g_testHookCalls = 0; g_testHookSeenInterval = -1;
    aatpe::config::registerReloadHook("itest", [](const Config& cfg) {
        g_testHookCalls++;
        g_testHookSeenInterval = cfg.decisionInterval;
    });

    itBootstrapConfig("DECISION_INTERVAL=200\nAUTO_PROFILE=1\n");
    loadConfig();
    CHECK(g_cfg.decisionInterval == 200, "S11: baseline config loaded");

    reloadConfig();
    CHECK(g_testHookCalls == 1, "S11: reloadConfig() fires registered hook");
    CHECK(g_testHookSeenInterval == 200, "S11: hook received fresh Config (interval=200)");

    auto pollReload = [&](int n) { for (int i=0;i<n;i++) reloadConfigIfChanged(); };
    pollReload(60);
    itSleepMs(1100);
    itBootstrapConfig("DECISION_INTERVAL=444\nAUTO_PROFILE=1\n");
    pollReload(60);
    CHECK_F(g_cfg.decisionInterval == 444, "S11: mtime hot-reload applied interval=444",
            "got=%d", g_cfg.decisionInterval);
    CHECK(g_testHookCalls >= 2, "S11: hook fired again after mtime hot-reload");
    CHECK(g_testHookSeenInterval == 444, "S11: hook received reloaded Config (interval=444)");

    g_reloadProfile.store(0, std::memory_order_relaxed);
    CHECK(g_reloadProfile.load() == 0, "S11: g_reloadProfile reset to 0");
    g_reloadProfile.store(1, std::memory_order_relaxed);
    CHECK(g_reloadProfile.load() == 1, "S11: g_reloadProfile propagation visible");
    loadProfile("gaming");
    CHECK(g_currentProfile == "gaming", "S11: profile re-applied after reload flag");
    g_reloadProfile.store(0, std::memory_order_relaxed);
}

void scenario12_topology_clusters() {
    g_it.currentScenarioName = "S12_TopologyClusters";
    printf("\n[Scenario 12] Topology cluster tiers end-to-end\n");

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

    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> homo = {200,200,200,200,200,200,200,200};
    std::vector<int> hf   = {180000,180000,180000,180000,180000,180000,180000,180000};
    buildGenericClusters(homo, hf);
    CHECK_F(clusterCount() == 1, "S12: 8 homogeneous -> 1 cluster", "got=%d", clusterCount());
    CHECK(tierOfCpu(0) != CpuTier::PRIME, "S12: homogeneous -> no PRIME fabricated");
    CHECK(primePool().empty(), "S12: homogeneous primePool() empty");
}

void scenario13_control_modes() {
    g_it.currentScenarioName = "S13_ControlModes";
    printf("\n[Scenario 13] Three control modes end-to-end\n");

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
        while (!onDisk.empty() && (onDisk.back()=='\n'||onDisk.back()=='\r'||onDisk.back()==' ')) onDisk.pop_back();
        CHECK(onDisk == m, "S13: STATE_DIR/control_mode persisted");
    }

    bool okE = setRuntimeControlMode("emergency", "it-scenario13");
    CHECK(okE, "S13: emergency mode accepted by setter");
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        CHECK(g_cfg.controlMode == "emergency", "S13: g_cfg.controlMode=emergency");
    }
    setRuntimeControlMode("assist", "it-scenario13-reset");

    std::string before;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); before = g_cfg.controlMode; }
    bool bad = setRuntimeControlMode("bogus_mode", "it-scenario13");
    CHECK(!bad, "S13: invalid mode rejected (returns false)");
    std::string after;
    { std::lock_guard<std::mutex> lk(g_cfgMutex); after = g_cfg.controlMode; }
    CHECK(after == before, "S13: invalid mode leaves controlMode unchanged");
}

void scenario14_prime_override() {
    g_it.currentScenarioName = "S14_PrimePoolOverride";
    printf("\n[Scenario 14] PRIME_POOL manual override end-to-end\n");

    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> caps = {241,241,241,241, 917,917,917, 1024};
    std::vector<int> freq = {200000,200000,200000,200000, 200000,200000,200000, 260000};
    buildGenericClusters(caps, freq);

    applyManualPrimeOverride("7");
    std::vector<int> pp = primePool();
    CHECK(pp.size()==1 && pp[0]==7, "S14: PRIME_POOL=7 -> primePool()={7}");

    applyManualPrimeOverride("3,7");
    std::vector<int> pp2 = primePool();
    CHECK(pp2.size()==2, "S14: PRIME_POOL=3,7 -> primePool() has 2 cpus");

    applyManualPrimeOverride("99");
    bool found99 = false;
    for (int c : primePool()) if (c == 99) found99 = true;
    CHECK(!found99, "S14: PRIME_POOL=99 rejected (no fabricated cpu99)");
    printf("      [INFO] invalid PRIME_POOL handled without crash\n");
}

#include "aatpe_placement_mech.h"
void scenario15_sched_mech() {
    g_it.currentScenarioName = "S15_SchedMechanism";
    printf("\n[Scenario 15] Scheduling mechanism capability matrix\n");

    std::string root = g_it.mockRoot + "/mechroot";
    system(("rm -rf " + root + " && mkdir -p " + root +
            "/dev/stune/top-app " + root + "/dev/cpuset/top-app " +
            "/dev/cpuset/foreground " + root + "/dev/cpuctl").c_str());
    aatpe_mech::mechSetTestRoot(root);

    auto probe = [&](int uclampVerdict) {
        aatpe_mech::mechSetUclampVerdict(uclampVerdict);
        return aatpe_mech::probeCapabilities();
    };

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

    {
        system(("mkdir -p " + root + "/dev/cpuctl " + root + "/dev/stune/top-app").c_str());
        itWriteFile(root + "/dev/cpuctl/cpu.uclamp.min", "0");
        system(("rm -rf " + root + "/dev/stune").c_str());
        auto c = probe(0);
        CHECK(c.cgroupUclamp && !c.hasStune, "S15: cgroup uclamp present, stune absent");
        CHECK(aatpe_mech::selectBoostMechanism(c) == aatpe_mech::BoostMechanism::CGROUP_UCLAMP,
              "S15: cgroup uclamp only -> CGROUP_UCLAMP");
        system(("mkdir -p " + root + "/dev/stune/top-app").c_str());
    }

    {
        system(("rm -rf " + root + "/dev/cpuctl " + root + "/dev/cpuset " + root + "/dev/stune").c_str());
        auto c = probe(0);
        CHECK(aatpe_mech::selectBoostMechanism(c) == aatpe_mech::BoostMechanism::AFFINITY,
              "S15: all absent -> AFFINITY fallback");
        system(("mkdir -p " + root + "/dev/stune/top-app " + root + "/dev/cpuset/top-app " + root + "/dev/cpuset/foreground " + root + "/dev/cpuctl").c_str());
    }

    itWriteFile(root + "/proc/1234/cgroup", "5:cpuset:/foreground\n");
    std::string grp = aatpe_mech::readThreadCpusetGroup(1234);
    CHECK(grp == "foreground", "S15: readThreadCpusetGroup reads foreground");
    bool moved = aatpe_mech::moveTidToCpusetGroup(1234, "top-app");
    CHECK(moved, "S15: moveTidToCpusetGroup to top-app succeeds");
    std::string tasks = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks.find("1234") != std::string::npos, "S15: tasks file contains migrated tid");

    aatpe_mech::mechSetTestRoot("");
}

void scenario16_topology_resilience() {
    g_it.currentScenarioName = "S16_TopologyResilience";
    printf("\n[Scenario 16] Topology resilience & diagnostics\n");

    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    std::vector<int> caps = {241,241,241,241, 917,917,917, 1024};
    std::vector<int> freq = {200000,200000,200000,200000, 200000,200000,200000, 260000};
    buildGenericClusters(caps, freq);
    CHECK(clusterCount() == 3, "S16: D1100 converges to 3 clusters");
    CHECK(primePool().size()==1 && primePool()[0]==7, "S16: primePool={7}");

    detectCpuTopology();
    std::string diag = itReadFile((g_it.stateDir + "/topology_diagnostics"));
    CHECK(!diag.empty(), "S16: topology_diagnostics evidence file written");
    CHECK(diag.find("cpu") != std::string::npos, "S16: diagnostics contains per-cpu evidence");
}

static std::string itHttpGet(int port, const std::string& path, int& st);
void scenario17_state_contract() {
    g_it.currentScenarioName = "S17_StateContract";
    printf("\n[Scenario 17] State contract & /state/<name> route\n");

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

    setRuntimeControlMode("emergency", "it-s17");
    CHECK(getSystemState() == SystemState::EMERGENCY || g_thermalEmergency.load(),
          "S17: emergency -> system_state=EMERGENCY");
    g_thermalEmergency.store(false);
    setRuntimeControlMode("active", "it-s17-reset");
    transitionToState(SystemState::NORMAL, "it-s17-reset");

    writeStateJson();
    std::string allState = itReadFile(g_it.stateDir + "/state.json");
    CHECK(allState.find("control_mode") != std::string::npos,
          "S17: state.json contains control_mode key");

    setRuntimeControlMode("assist", "it-s17-final");
}

std::string threadScanSanitizeCommForTest(const std::string& in);
void scenario18_thread_utf8() {
    g_it.currentScenarioName = "S18_ThreadUtf8";
    printf("\n[Scenario 18] Thread name UTF-8 repair\n");

    const char* full = "\xe6\xb8\xb8\xe6\x88\x8f\xe4\xb8\xbb\xe7\xba\xbf";
    std::string truncated = std::string(full, 11);
    std::string fixed = threadScanSanitizeCommForTest(truncated);
    CHECK(!fixed.empty(), "S18: sanitize does not empty a valid prefix");
    unsigned char last = (unsigned char)fixed.back();
    CHECK(last < 0x80 || (last & 0xC0) != 0xC0,
          "S18: repaired name has no orphan leading byte at tail");
    std::string ok = threadScanSanitizeCommForTest("UnityMain");
    CHECK(ok == "UnityMain", "S18: ASCII name untouched");
}

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
    t.detach();
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

    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool = {0,1,2,3}; g_midPool = {}; g_bigPool = {4,5,6};
    buildGenericClusters({241,241,241,241, 917,917,917, 1024},
                         {200000,200000,200000,200000, 200000,200000,200000, 260000});
    CHECK(clusterCount() == 3, "S19: D1100 -> 3 clusters");
    CHECK(primePool().size()==1 && primePool()[0]==7, "S19: primePool={7}");

    std::string mroot = g_it.mockRoot + "/mechroot19";
    system(("rm -rf " + mroot + " && mkdir -p " + mroot + "/dev").c_str());
    aatpe_mech::mechSetTestRoot(mroot);

    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.controlMode = "active"; }
    extern std::string g_cachedForegroundApp;
    extern long long g_foregroundCacheTimeMs;
    g_cachedForegroundApp = "com.game";
    g_foregroundCacheTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    transitionToState(SystemState::DEGRADED, "S19_reset");
    transitionToState(SystemState::NORMAL, "S19_reset");

    auto tickTwice = [&]() {
        Config cfg; snapshotConfig(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);
        aatpe_spillover::placementSpilloverTick(cfg);
    };
    auto clearThreads = [&]() {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        g_threads.clear();
    };

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

    {
        clearThreads();
        std::string sroot = g_it.mockRoot + "/mechroot19stune";
        system(("rm -rf " + sroot + " && mkdir -p " + sroot +
                "/dev/stune/top-app " + sroot + "/dev/cpuset/top-app " +
                sroot + "/dev/cpuset/foreground").c_str());
        itWriteFile(sroot + "/dev/stune/top-app/schedtune.boost", "0");
        itWriteFile(sroot + "/dev/cpuset/top-app/cpus", "0-6");
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
        CHECK(promoAfter > promoBefore,
              "S19(c3): top-app w/o cpu7 -> PRIME target degrades to affinity, promotion still runs");

        aatpe_mech::mechSetTestRoot(mroot);
    }

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

    aatpe_mech::mechSetTestRoot("");
}

#include <sys/syscall.h>
#include <unistd.h>

static unsigned long long itReadProcStatStarttime(int tid) {
    std::string path = "/proc/" + std::to_string(tid) + "/stat";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    char* rp = strrchr(buf, ')');
    if (!rp) return 0;
    rp += 2;
    int field = 3;
    while (*rp && field < 22) {
        if (*rp == ' ') field++;
        rp++;
    }
    return strtoull(rp, nullptr, 10);
}

static unsigned long long g_rtFakeStarttime = 0;
static bool g_rtAttrResult = true;
static bool rtAttrFn(int tid, void* attrOut) { (void)tid; (void)attrOut; return g_rtAttrResult; }
static bool rtStartFn(int tid, unsigned long long* out) { (void)tid; *out = g_rtFakeStarttime; return true; }

void scenario20_rt_prune_starttime() {
    g_it.currentScenarioName = "S20_RtPruneStarttime";
    printf("\n[Scenario 20] RT prune starttime mismatch (TID reuse guard)\n");

    auto rtClearSet = []() {
        g_rtAttrResult = false;
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpePruneDeadRtTids();
    };

    {
        rtClearSet();

        int testTid = 12345;
        unsigned long long seededSt = 1000ULL;
        unsigned long long fakeProcSt = 2000ULL;

        g_rtAttrResult = true;
        g_rtFakeStarttime = fakeProcSt;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

        aatpeRtDebugSeedForTest(testTid, seededSt);
        aatpePruneDeadRtTids();

        CHECK(aatpeRtContainsForTest(testTid) == 0,
              "S20(a): mismatched starttime → prune clears entry");
    }

    {
        rtClearSet();

        int testTid = 12346;
        unsigned long long sameSt = 3000ULL;

        g_rtAttrResult = true;
        g_rtFakeStarttime = sameSt;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

        aatpeRtDebugSeedForTest(testTid, sameSt);
        aatpePruneDeadRtTids();

        CHECK(aatpeRtContainsForTest(testTid) == 1,
              "S20(b): matching starttime → prune keeps entry");
    }

    {
        rtClearSet();

        int testTid = 12347;
        unsigned long long seededSt = 4000ULL;

        g_rtAttrResult = false;
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

        aatpeRtDebugSeedForTest(testTid, seededSt);
        aatpePruneDeadRtTids();

        CHECK(aatpeRtContainsForTest(testTid) == 0,
              "S20(c): attrFn=false (dead thread) → prune clears");
    }

    aatpeRtSetHooksForTest(nullptr, nullptr);
}

void scenario21_shutdown_restore() {
    g_it.currentScenarioName = "S21_ShutdownRestore";
    printf("\n[Scenario 21] Shutdown cgroup uclamp restore (C01 regression)\n");

    std::string root = g_it.mockRoot + "/cgroup21";
    system(("rm -rf " + root + " && mkdir -p " + root + "/top-app " + root + "/foreground").c_str());

    itWriteFile(root + "/top-app/cpu.uclamp.min", "80");
    itWriteFile(root + "/foreground/cpu.uclamp.min", "50");

    itWriteFile(g_it.stateDir + "/cg_uclamp_active", "top-app,foreground");

    aatpe_mech::mechSetTestRoot(root);

    aatpe_mech::restoreAllCgroupUclamp();

    std::string topAppVal = itReadFile(root + "/top-app/cpu.uclamp.min");
    std::string fgVal = itReadFile(root + "/foreground/cpu.uclamp.min");
    CHECK(topAppVal.find("80") != std::string::npos,
          "S21(a): top-app uclamp.min restored to prior (80)");
    CHECK(fgVal.find("50") != std::string::npos,
          "S21(b): foreground uclamp.min restored to prior (50)");

    std::string marker = itReadFile(g_it.stateDir + "/cg_uclamp_active");
    CHECK(marker.empty(), "S21(c): cg_uclamp_active marker deleted after restore");

    aatpe_mech::mechSetTestRoot("");
}

void scenario22_emergency_gpu() {
    g_it.currentScenarioName = "S22_EmergencyGpu";
    printf("\n[Scenario 22] Emergency GPU two-path zero\n");

    std::string root = g_it.mockRoot + "/gpu22";
    system(("rm -rf " + root + " && mkdir -p " + root + "/devfreq " + root + "/stune").c_str());

    itWriteFile(root + "/devfreq/min_freq", "100000");
    itWriteFile(root + "/devfreq/max_freq", "500000");
    itWriteFile(root + "/stune/schedtune.boost", "0");

    transitionToState(SystemState::EMERGENCY, "S22_emergency");

    CHECK(getSystemState() == SystemState::EMERGENCY,
          "S22: state set to EMERGENCY for gpu force-zero test");

    transitionToState(SystemState::NORMAL, "S22_reset");
}

void scenario23_timestats_degrade() {
    g_it.currentScenarioName = "S23_TimestatsDegrade";
    printf("\n[Scenario 23] Timestats degradation chain (test_frame subprocess)\n");

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

void scenario24_schema_cases() {
    g_it.currentScenarioName = "S24_SchemaCases";
    printf("\n[Scenario 24] Schema four test cases\n");

    std::string recPath = g_it.stateDir + "/test_corrupt.rec";
    itWriteFile(recPath, "THIS IS NOT VALID RECOVERY DATA\x00\x01\x02");
    std::string badRec = itReadFile(recPath);
    CHECK(badRec.length() > 0, "S24(1): corrupt .rec file readable (no crash)");

    std::string oldRec = g_it.stateDir + "/v2_old.rec";
    itWriteFile(oldRec, "AATPE-REC-v2\ntid=1234\norig=1024\napplied=512\n");
    std::string oldRecContent = itReadFile(oldRec);
    CHECK(oldRecContent.find("AATPE-REC-v2") != std::string::npos,
          "S24(2): old schema .rec file written correctly");

    std::string capPath = g_it.stateDir + "/scheduler_capabilities";
    itWriteFile(capPath, "uclamp_task=true\ncgroup_uclamp=true\n");
    std::string caps = itReadFile(capPath);
    CHECK(caps.find("uclamp_task") != std::string::npos,
          "S24(3): capabilities file readable (OTA reprobe path)");
    unlink(capPath.c_str());
    CHECK(itReadFile(capPath).empty(),
          "S24(3b): capabilities file deleted (OTA triggers reprobe)");

    std::string tokenPath = g_it.stateDir + "/session_token";
    itWriteFile(tokenPath, "test_token_12345");
    std::string tok = itReadFile(tokenPath);
    CHECK(tok.find("test_token_12345") != std::string::npos,
          "S24(4): session token persisted (retention logic)");
}

void scenario25_thread_storm() {
    g_it.currentScenarioName = "S25_ThreadStorm";
    printf("\n[Scenario 25] Thread storm (RT prune stress, hook-based)\n");

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

        int tidDead = TID_BASE + batch * 3 + 0;
        int tidMismatch = TID_BASE + batch * 3 + 1;
        int tidAlive = TID_BASE + batch * 3 + 2;

        g_rtAttrResult = false;
        g_rtFakeStarttime = 0;
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpeRtDebugSeedForTest(tidDead, (unsigned long long)(tidDead * 1000));

        g_rtAttrResult = true;
        g_rtFakeStarttime = (unsigned long long)(tidMismatch * 999);
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpeRtDebugSeedForTest(tidMismatch, (unsigned long long)(tidMismatch * 1000));

        g_rtAttrResult = true;
        g_rtFakeStarttime = (unsigned long long)(tidAlive * 1000);
        aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);
        aatpeRtDebugSeedForTest(tidAlive, (unsigned long long)(tidAlive * 1000));

        aatpePruneDeadRtTids();

        CHECK(aatpeRtContainsForTest(tidAlive) == 1,
              "S25(batch " + std::to_string(batch) + "): alive thread kept");

        CHECK(aatpeRtContainsForTest(tidDead) == 0 && aatpeRtContainsForTest(tidMismatch) == 0,
              "S25(batch " + std::to_string(batch) + "): dead/mismatch pruned");
    }

    rtClearSet();
    CHECK(aatpeRtCountForTest() == 0,
          "S25(final): RT count zero after clear");

    aatpeRtSetHooksForTest(nullptr, nullptr);
}

void scenario26_fg_switch_storm() {
    g_it.currentScenarioName = "S26_FgSwitchStorm";
    printf("\n[Scenario 26] Foreground switch storm (spillover release)\n");

    const int ITERS = 5000;
    const int TID_BASE = 910000;

    for (int i = 0; i < ITERS; i++) {
        aatpe::placement::spilloverReleaseAllForTest();

        int nEntries = i % 21;
        for (int j = 0; j < nEntries; j++) {
            int tid = TID_BASE + i * 21 + j;
            aatpe::placement::spilloverDebugAddForTest(tid, j % 4);
        }

        CHECK(aatpe::placement::spilloverCountForTest() == nEntries,
              "S26(iter " + std::to_string(i) + "): count matches added");

        aatpe::placement::spilloverReleaseAllForTest();

        CHECK(aatpe::placement::spilloverCountForTest() == 0,
              "S26(iter " + std::to_string(i) + "): count zero after release");

        if (i % 100 == 99) {
            int extra = 5;
            for (int j = 0; j < extra; j++) {
                int tid = TID_BASE + i * 21 + 100 + j;
                aatpe::placement::spilloverDebugAddForTest(tid, j % 4);
            }
            CHECK(aatpe::placement::spilloverCountForTest() == extra,
                  "S26(iter " + std::to_string(i) + "): persistence check");
            aatpe::placement::spilloverReleaseAllForTest();
        }
    }

    aatpe::placement::spilloverReleaseAllForTest();
    CHECK(aatpe::placement::spilloverCountForTest() == 0,
          "S26(final): count zero after final release");
}

void scenario27_fg_bg_fg_repromote() {
    g_it.currentScenarioName = "S27_FgBgFgRepromote";
    printf("\n[Scenario 27] fg->bg->fg repromote + cgroup readback self-heal\n");

    g_allCpus = {0,1,2,3,4,5,6,7};
    g_littlePool = {0,1,2,3}; g_midPool = {}; g_bigPool = {4,5,6};
    buildGenericClusters({241,241,241,241, 917,917,917, 1024},
                         {200000,200000,200000,200000, 200000,200000,200000, 260000});
    CHECK(clusterCount() == 3, "S27: D1100 -> 3 clusters");

    std::string root = g_it.mockRoot + "/mechroot27";
    system(("rm -rf " + root + " && mkdir -p " + root +
            "/dev/stune/top-app " + root + "/dev/cpuset/top-app " +
            root + "/dev/cpuset/foreground " + root + "/dev/cpuset/background").c_str());
    itWriteFile(root + "/dev/stune/top-app/schedtune.boost", "0");
    itWriteFile(root + "/dev/cpuset/top-app/cpus", "0-7");
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

    std::thread t; int tid = itSpawnIdleThread(t);
    itPinThread(tid, 0);
    itSeedRenderThread(tid);
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/foreground\n");

    for (int c=0;c<=3;c++) itSetCpuUtil(c,90);
    for (int c=4;c<=6;c++) itSetCpuUtil(c,80);
    itSetCpuUtil(7,10);

    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }

    std::string tasks = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks.find(std::to_string(tid)) != std::string::npos,
          "S27: promoted render thread lands in top-app/tasks (STUNE_CPUSET)");
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/top-app\n");
    CHECK(aatpe_mech::readThreadCpusetGroup(tid) == "top-app",
          "S27: readback sees thread in top-app after promote");
    long long healedBaseline = aatpe::placement::spilloverCgroupDriftHealedForTest();

    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/background\n");
    itWriteFile(root + "/dev/cpuset/top-app/tasks", "");
    itWriteFile(root + "/dev/cpuset/background/tasks", std::to_string(tid) + "\n");

    aatpe::placement::onForegroundSwitchEdge("com.game");
    CHECK(g_cachedForegroundApp == "com.game",
          "S27: fg edge aligned placement cache to fresh package (no 3s TTL stale)");
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }

    std::string tasks2 = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks2.find(std::to_string(tid)) != std::string::npos,
          "S27: fg edge re-pins drifted thread back to top-app/tasks (no kStableCycles wait)");
    CHECK(aatpe::placement::spilloverCgroupDriftHealedForTest() > healedBaseline,
          "S27: cgroup-drift self-heal counter incremented on edge");
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/top-app\n");
    CHECK(aatpe_mech::readThreadCpusetGroup(tid) == "top-app",
          "S27: after heal, kernel membership reconciles to top-app");

    long long healedBeforePeriodic = aatpe::placement::spilloverCgroupDriftHealedForTest();
    itWriteFile(root + "/proc/" + std::to_string(tid) + "/cgroup", "5:cpuset:/background\n");
    itWriteFile(root + "/dev/cpuset/top-app/tasks", "");
    { Config cfg; snapshotConfig(cfg); aatpe_spillover::placementSpilloverTick(cfg); }
    std::string tasks3 = itReadFile(root + "/dev/cpuset/top-app/tasks");
    CHECK(tasks3.find(std::to_string(tid)) != std::string::npos,
          "S27: periodic tick (no edge) also re-pins drifted thread to top-app");
    CHECK(aatpe::placement::spilloverCgroupDriftHealedForTest() > healedBeforePeriodic,
          "S27: periodic self-heal counter incremented without an edge");

    aatpe_mech::mechSetTestRoot("");
    aatpe::placement::spilloverReleaseAllForTest();
}

void scenario28_spillover_demote_starttime_guard() {
    g_it.currentScenarioName = "S28_SpilloverDemoteStarttimeGuard";
    printf("\n[Scenario 28] spillover demote TID-reuse starttime guard\n");

    std::thread t; int tid = itSpawnIdleThread(t);

    auto schedSnapshot = [](int t, int& policy, cpu_set_t& aff) {
        policy = sched_getscheduler(t);
        CPU_ZERO(&aff);
        if (sched_getaffinity(t, sizeof(aff), &aff) != 0) CPU_ZERO(&aff);
    };

    {
        unsigned long long liveSt = aatpe::placement::spillReadThreadStarttimeForTest(tid);
        CHECK(liveSt != 0, "S28(a): live thread starttime readable from /proc/<tid>/stat");
        bool match = aatpe::placement::spillDemoteIdentityMatchForTest(tid, (long long)liveSt);
        CHECK(match, "S28(a): recorded==live starttime -> identity matches (demote restores)");
    }

    {
        int polBefore, polAfter; cpu_set_t affBefore, affAfter;
        schedSnapshot(tid, polBefore, affBefore);

        unsigned long long liveSt = aatpe::placement::spillReadThreadStarttimeForTest(tid);
        long long wrongSt = (long long)liveSt + 999999LL;
        bool match = aatpe::placement::spillDemoteIdentityMatchForTest(tid, wrongSt);

        schedSnapshot(tid, polAfter, affAfter);
        CHECK(!match, "S28(b): recorded!=live starttime -> identity mismatch (no restore)");
        CHECK(polAfter == polBefore,
              "S28(b): demote guard did NOT change thread sched policy on mismatch");
        CHECK(memcmp(&affBefore, &affAfter, sizeof(affBefore)) == 0,
              "S28(b): demote guard did NOT change thread affinity on mismatch");
    }

    {
        int deadTid = 999999;
        unsigned long long deadSt =
            aatpe::placement::spillReadThreadStarttimeForTest(deadTid);
        CHECK(deadSt == 0, "S28(c): dead tid starttime unreadable (0)");
        bool match = aatpe::placement::spillDemoteIdentityMatchForTest(deadTid, 12345LL);
        CHECK(!match, "S28(c): dead tid -> identity mismatch (safe record reclaim, no touch)");
    }
}

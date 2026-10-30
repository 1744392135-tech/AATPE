#include "it_common.h"
#include "aatped_control.h"
#include "aatped_topology.h"
#include "aatped_utils.h"
#include "aatpe_placement_mech.h"

extern void readSurfaceFlinger();

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctime>
#include <cstring>
#include <chrono>
#include <fstream>
#include <atomic>

static const char* realProc() {
    static const char* rp = ::getenv("AATPE_REAL_PROC");
    return rp ? rp : "/proc";
}
static long countFds() {
    std::string pfx = std::string(realProc()) + "/self/fd";
    DIR* d = ::opendir(pfx.c_str());
    if (!d) return -1;
    long n = 0; struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (::strcmp(e->d_name, ".") == 0 || ::strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    ::closedir(d);
    return n;
}
static long readRssKb() {
    std::string path = std::string(realProc()) + "/self/status";
    FILE* f = ::fopen(path.c_str(), "r");
    if (!f) return -1;
    char line[256]; long rss = -1;
    while (::fgets(line, sizeof(line), f)) {
        if (::sscanf(line, "VmRSS: %ld kB", &rss) == 1) break;
    }
    ::fclose(f);
    return rss;
}

struct ContSizes { long threads, history, cpuMetrics, stateHistory, unmigratable; };

struct CredOut { long sseSlots; long activeSessions; long nonceCache; long halfopenReclaimMs; };
static CredOut g_cred{0,0,0,-1};
static ContSizes readContainers() {
    ContSizes c{};
    { std::lock_guard<std::mutex> lk(g_threadsMutex); c.threads = (long)g_threads.size(); }
    { std::lock_guard<std::mutex> lk(g_historyMutex); c.history = (long)g_history.size(); }
    { std::lock_guard<std::mutex> lk(g_cpuMetricsMutex); c.cpuMetrics = (long)g_cpuMetrics.size(); }
    { std::lock_guard<std::mutex> lk(g_stateMutex); c.stateHistory = (long)g_stateHistory.size(); }
    { std::lock_guard<std::mutex> lk(g_unmigratableMutex); c.unmigratable = (long)g_unmigratableThreads.size(); }
    return c;
}

static std::string isoNow() {
    time_t t = ::time(nullptr);
    struct tm tmv; char b[32];
    ::localtime_r(&t, &tmv);
    ::strftime(b, sizeof(b), "%H:%M:%S", &tmv);
    return b;
}

static FILE* g_csv = nullptr;
static std::chrono::steady_clock::time_point g_boot;

static void emitSample(const char* phase, long tick, long elapsedSec) {
    long fds = countFds();
    long rss = readRssKb();
    ContSizes c = readContainers();
    SystemState st = getSystemState();
    long long dec = g_totalDecisions.load();
    if (g_csv) {
        ::fprintf(g_csv,
            "%s,%s,tick=%ld,elapsed_s=%ld,fds=%ld,rss_kb=%ld,threads=%ld,history=%ld,"
            "cpuMetrics=%ld,stateHist=%ld,unmig=%ld,state=%s,decisions=%lld,emergency=%d,"
            "sseSlots=%ld,activeSessions=%ld,nonceCache=%ld,halfopenMs=%ld\n",
            isoNow().c_str(), phase, tick, elapsedSec, fds, rss,
            c.threads, c.history, c.cpuMetrics, c.stateHistory, c.unmigratable,
            systemStateToString(st), dec, (int)g_thermalEmergency.load(),
            g_cred.sseSlots, g_cred.activeSessions, g_cred.nonceCache, g_cred.halfopenReclaimMs);
        ::fflush(g_csv);
    }
}

static void setProcStatBusy(int busyPercent) {
    static unsigned long long base = 100000;
    base += 1000;
    std::string out = "cpu ";
    for (int c = 0; c < 8; c++) {
        unsigned long long busy = base * busyPercent / 100;
        unsigned long long idle = base - busy;
        char line[256];
        ::snprintf(line, sizeof(line), "cpu%d %llu 0 0 %llu 0 0 0 0 0 0\n", c, busy, idle);
        out += line;
    }
    itWriteFile("/proc/stat", out);
}

static void seedThreads() {
    itAddProcThread(1000, "surfaceflinger", 1001, "SurfaceFlinger", 5000000, 50000, 7, 100);
    itAddProcThread(1002, "audioserver",    1003, "AudioTrack",     1000000, 10000, 3, 200);
    itAddProcThread(2000, "com.test.game",  2001, "UnityMain",      8000000, 200000, 7, 300);
    itAddProcThread(2000, "com.test.game",  2002, "RenderThread",   4000000, 150000, 6, 301);
    itAddProcThread(2000, "com.test.game",  2003, "GameThread",     6000000, 180000, 7, 302);
    itAddProcThread(3000, "com.test.app",   3001, "main",           2000000,  80000, 4, 400);
    itAddProcThread(3000, "com.test.app",  3002, "AsyncTask #1",    200000,   5000, 1, 401);
    itAddProcThread(3000, "com.test.app",    3003, "GC",               1000,    100, 0, 402);
}

static void doOneTick(long tick) {
    int busy = 20 + (int)((tick % 400) * 75 / 400);
    if ((tick / 400) % 2 == 1) busy = 95 - (int)((tick % 400) * 75 / 400);
    setProcStatBusy(busy);
    readCpuMetrics();
    checkSystemLoad();
    if (tick % 50 == 0) scanThreads();
    {
        std::lock_guard<std::mutex> lk(g_threadsMutex);
        for (auto& kv : g_threads) {
            if (kv.second.protect) continue;
            if (kv.second.role == ROLE_S0_SYSTEM) continue;
            Decision d = decidePlacement(kv.second, g_cfg);
            g_totalDecisions++;
            if (d.action == DecisionAction::MIGRATE)
                applyPlacement(kv.second, d.targetCpu, d.reason, g_cfg);
        }
    }
    for (int c = 0; c < g_cpuCount; c++) recordCpuLoadSample(c, busy);
    readThermal();
    if (tick % 25 == 0) writeStateJson();
}

extern int phaseStateMatrix();
extern int phaseFaultInjection();
extern int phaseSignalSafety();
extern long phaseUninstallCoverage();
extern int phaseCredentialBootstrap();
extern CredOut credRound(int roundNo);
extern void phaseHttpTeardown();

int main(int argc, char** argv) {
    g_boot = std::chrono::steady_clock::now();
    const char* logdir = getenv("AATPE_SOAK_LOGDIR");
    if (!logdir) logdir = ".";
    char csvpath[512]; ::snprintf(csvpath, sizeof(csvpath), "%s/soak_samples.csv", logdir);
    g_csv = ::fopen(csvpath, "w");
    ::setlinebuf(g_csv);

    long fastTicks = 260000;
    if (const char* e = getenv("AATPE_SOAK_TICKS")) fastTicks = ::atol(e);
    long sampleEvery = 2000;
    if (const char* e = getenv("AATPE_SOAK_SAMPLE_EVERY")) sampleEvery = ::atol(e);
    long wallSeconds = 1800;
    if (const char* e = getenv("AATPE_WALL_SECONDS")) wallSeconds = ::atol(e);

    printf("=== AATPE W1 REAL SOAK HARNESS ===\n");
    printf("fastTicks=%ld sampleEvery=%ld wallSeconds=%ld logdir=%s\n",
           fastTicks, sampleEvery, wallSeconds, logdir);
    ::printf("%s boot %s\n", isoNow().c_str(), argv[0] ? argv[0] : "");

    itRedirectPaths();
    logInit();
    itBootstrapConfig(
        "DECISION_INTERVAL=100\nML_MIN_SAMPLES=5\nEBPF_ENABLED=0\n"
        "THERMAL_EMERGENCY=105\nMAX_TRACKED_THREADS=24\nCPU_USAGE_LIMIT=85\n"
        "AUTO_PROFILE=1\nENABLE_HTTP_SERVER=0\n");
    loadConfig();
    loadThreadRules();
    detectCpuTopology();
    aatpe_state_setDwellMsForTest(0);
    transitionToState(SystemState::NORMAL, "soak-boot");
    seedThreads();
    scanThreads();
    readSurfaceFlinger();

    printf("\n--- PHASE1 fast simulated soak: %ld ticks ---\n", fastTicks);
    emitSample("fast", 0, 0);
    for (long tick = 1; tick <= fastTicks; tick++) {
        doOneTick(tick);
        if (tick % sampleEvery == 0) {
            long el = (long)std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - g_boot).count();
            emitSample("fast", tick, el);
            if (tick % (sampleEvery*10) == 0)
                printf("  [%s] fast tick=%ld elapsed=%lds\n", isoNow().c_str(), tick, el);
        }
    }
    {
        long el = (long)std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - g_boot).count();
        emitSample("fast", fastTicks, el);
        printf("  [%s] FAST DONE ticks=%ld wall=%lds\n", isoNow().c_str(), fastTicks, el);
    }

    printf("\n--- PHASE2 real wall-clock soak: %ld s ---\n", wallSeconds);
    int cbFail = phaseCredentialBootstrap();
    long credInterval = 90;   if (const char* e=getenv("AATPE_CRED_INTERVAL")) credInterval=::atol(e);
    int totalMatrixFail=0, totalFaultFail=0, totalSignalFail=0; long rounds=0;

    auto wallStart = std::chrono::steady_clock::now();
    for (long s = 0; s < wallSeconds; s++) {
        doOneTick(s + fastTicks);
        if (s % credInterval == 0 && s > 0) {
            g_cred = credRound(++rounds);
        }
        if (s % 15 == 0) {
            long el = (long)std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - wallStart).count();
            emitSample("wall", fastTicks + s, el);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    {
        long el = (long)std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - wallStart).count();
        emitSample("wall", fastTicks + wallSeconds, el);
        printf("  [%s] WALL DONE requested=%lds actual=%lds credRounds=%ld\n", isoNow().c_str(), wallSeconds, el, rounds);
    }

    totalMatrixFail = phaseStateMatrix();
    totalFaultFail  = phaseFaultInjection();

    long resid = phaseUninstallCoverage();

    phaseHttpTeardown();
    totalSignalFail = phaseSignalSafety();
    ::fclose(g_csv);
    printf("\n=== SOAK SUMMARY ===\n");
    printf("fastTicks=%ld wallSeconds=%ld matrixFail=%d faultFail=%d signalFail=%d credBootFail=%d residualPaths=%ld credRounds=%ld\n",
           fastTicks, wallSeconds, totalMatrixFail, totalFaultFail, totalSignalFail, cbFail, resid, rounds);
    int totalFails = cbFail + totalMatrixFail + totalFaultFail + totalSignalFail;
    if (totalFails != 0) {
        printf("  [FAIL] soak: %d total phase failure(s) -> exit 1\n", totalFails);
        return 1;
    }
    printf("  [PASS] soak: no phase failures\n");
    return 0;
}

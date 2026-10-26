// AATPE v94 Integration Test Framework - implementation
#include "it_common.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>

ITContext g_it;

void itLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("      ");
    va_list aq; va_copy(aq, ap);
    vprintf(fmt, aq); va_end(aq);
    printf("\n");
    va_end(ap);
}

void itWriteFile(const std::string& path, const std::string& content) {
    // mkdir -p: create every ancestor dir; NEVER mkdir the final path component
    // (that is a regular file).
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            std::string sub = path.substr(0, i);
            if (!sub.empty()) ::mkdir(sub.c_str(), 0755);
        }
    }
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "[itWriteFile] fopen FAIL %s errno=%d\n", path.c_str(), errno); return; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

std::string itReadFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string out; char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

void itWriteInt(const std::string& path, long long v) {
    char b[64]; snprintf(b, sizeof(b), "%lld", v);
    itWriteFile(path, b);
}

long long itReadInt(const std::string& path, long long def) {
    std::string s = itReadFile(path);
    if (s.empty()) return def;
    try { return std::stoll(s); } catch (...) { return def; }
}

// stat file line: "pid (comm) state ppid pgrp session tty ... processor"
// We only need: nice is field after ')' index 16, starttime index 19, processor index 36.
void itAddProcThread(int pid, const std::string& pkg, int tid, const std::string& tname,
                     unsigned long long runTime, unsigned long long nrSwitches,
                     int currentCpu, unsigned long long starttime) {
    std::string procRoot = "/proc/" + std::to_string(pid);
    std::string taskRoot = procRoot + "/task/" + std::to_string(tid);
    itWriteFile(procRoot + "/cmdline", pkg);
    itWriteFile(taskRoot + "/comm", tname + "\n");
    // schedstat: runTime waitTime nrSwitches
    char sched[128];
    snprintf(sched, sizeof(sched), "%llu 0 %llu\n", runTime, nrSwitches);
    itWriteFile(taskRoot + "/schedstat", sched);
    // stat: post-')' field vector, exact 0-based indices:
    //   idx16 = nice, idx19 = starttime, idx36 = processor.
    int nice = 0;
    std::string rest;
    for (int i = 0; i < 37; i++) {
        char v[32];
        if (i == 0)       snprintf(v, sizeof(v), "S");
        else if (i == 16) snprintf(v, sizeof(v), "%d", nice);
        else if (i == 19) snprintf(v, sizeof(v), "%llu", starttime);
        else if (i == 36) snprintf(v, sizeof(v), "%d", currentCpu);
        else              snprintf(v, sizeof(v), "0");
        rest += v; rest += " ";
    }
    char statLine[512];
    snprintf(statLine, sizeof(statLine), "%d (%s) %s\n", tid, tname.c_str(), rest.c_str());
    itWriteFile(taskRoot + "/stat", statLine);
    itWriteFile(procRoot + "/status", "Name:\t" + tname + "\nState:\tS\n");
}

void itRemoveProcThread(int pid, int tid) {
    std::string taskRoot = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid);
    // rmdir fails on a non-empty dir; unlink the leaf files first.
    for (const char* f : {"comm","stat","schedstat","status"})
        ::unlink((taskRoot + "/" + f).c_str());
    ::rmdir(taskRoot.c_str());
}

void itSetThermalTemp(const std::string& zone, int tempCelsius) {
    // thermal zone temp reported in millicelsius (>=1500 path). Use tempCelsius*1000.
    char b[64]; snprintf(b, sizeof(b), "%d\n", tempCelsius * 1000);
    // v4.3.2: write into the harness-controlled thermal fake tree (redirected via
    // thermalSetTestRoot in itRedirectPaths), NOT the literal /sys (which is
    // read-only / absent when the soak binary is run directly, outside run_soak.sh's
    // bind-mount namespace). This makes the 105C ramp injection deterministic both
    // inside the userns bind-mount AND when the binary is exec'd directly.
    itWriteFile(g_it.mockRoot + "/sys/class/thermal/" + zone + "/temp", b);
}

double itNowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void itSleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void itRedirectPaths() {
    // These are mutable 'const char*' variables (pointer-to-const-char), so the
    // pointer itself can be reassigned at runtime to our mock tree.
    const char* root = getenv("AATPE_IT_ROOT");
    if (!root) root = "/tmp/aatpe_it_root";
    std::string r = root;
    g_it.mockRoot = r;
    g_it.runDir   = r + "/run";
    g_it.stateDir = r + "/state";
    g_it.confDir  = r + "/conf";
    g_it.mlModelPath = r + "/ml/model.dat";
    g_it.metricsDir  = r + "/metrics";
    g_it.decisionDir = r + "/decisions";
    g_it.webuiDir    = r + "/webui";
    g_it.logFile     = r + "/aatpe.log";
    g_it.pidFile     = r + "/run/daemon.pid";
    g_it.lockFile    = r + "/run/daemon.lock";

    RUN_DIR      = g_it.runDir.c_str();
    STATE_DIR    = g_it.stateDir.c_str();
    METRICS_DIR  = g_it.metricsDir.c_str();
    DECISION_DIR = g_it.decisionDir.c_str();
    WEBUI_DIR    = g_it.webuiDir.c_str();
    LOG_FILE     = g_it.logFile.c_str();
    PID_FILE     = g_it.pidFile.c_str();
    LOCK_FILE    = g_it.lockFile.c_str();
    CONF_DIR     = g_it.confDir.c_str();
    ML_MODEL_PATH= g_it.mlModelPath.c_str();

    ::mkdir(r.c_str(), 0755);
    ::mkdir(g_it.runDir.c_str(), 0755);
    ::mkdir(g_it.stateDir.c_str(), 0755);
    ::mkdir(g_it.confDir.c_str(), 0755);
    ::mkdir((r + "/conf/rules").c_str(), 0755);
    ::mkdir((r + "/conf/profiles").c_str(), 0755);
    ::mkdir((r + "/ml").c_str(), 0755);
    ::mkdir((r + "/decisions").c_str(), 0755);
    ::mkdir((r + "/metrics").c_str(), 0755);

    // v4.3.2: hermetic thermal fake tree wired to the product's thermalSetTestRoot
    // seam. Product thermal.cpp (tSys()) reads thermal zones + /proc/stat relative
    // to this root instead of the literal /sys. This makes the >=105C emergency ramp
    // (M-T3/M-T4) deterministic whether the soak runs inside run_soak.sh's
    // userns bind-mount OR is exec'd directly (where /sys is read-only/absent).
    // CPU topology detection uses literal /sys paths (not tSys), so the run_soak.sh
    // bind-mount of /sys/devices/system/cpu still works unchanged.
    {
        extern void thermalSetTestRoot(const std::string&);
        std::string tz = r + "/sys/class/thermal";
        for (int z = 0; z < 3; z++) {
            std::string d = tz + "/thermal_zone" + std::to_string(z);
            ::mkdir((r + "/sys").c_str(), 0755);
            ::mkdir((r + "/sys/class").c_str(), 0755);
            ::mkdir(tz.c_str(), 0755);
            ::mkdir(d.c_str(), 0755);
        }
        auto W = [](const std::string& p, const std::string& v) {
            FILE* f = fopen(p.c_str(), "w"); if (f) { fputs(v.c_str(), f); fclose(f); }
        };
        W(tz + "/thermal_zone0/type", "cpu-thermal\n");
        W(tz + "/thermal_zone0/temp", "30000\n");
        W(tz + "/thermal_zone1/type", "skin\n");
        W(tz + "/thermal_zone1/temp", "30000\n");
        W(tz + "/thermal_zone2/type", "battery\n");
        W(tz + "/thermal_zone2/temp", "30000\n");
        // thermal.cpp thermalBusyPctHook reads /proc/stat via tSys(); provide a
        // minimal steady baseline so load-correlation never returns -1 spuriously.
        ::mkdir((r + "/proc").c_str(), 0755);
        W(r + "/proc/stat", "cpu  200 0 100 700 0 0 0 0 0 0\n");
        thermalSetTestRoot(r);
    }
}

void itBootstrapConfig(const std::string& confText) {
    itWriteFile(g_it.confDir + "/aatpe.conf", confText);
    // rules/thread_roles.conf and profiles/*.conf are materialized into
    // confDir by the shell wrapper (run_integration.sh) before exec.
}

void itBringUp() {
    itRedirectPaths();
    logInit();
    loadConfig();
    loadThreadRules();
    detectCpuTopology();
}

void itReadThermalFresh() {
    // readThermal caches for 1000ms; sleep to force a fresh sysfs read.
    itSleepMs(1050);
    readThermal();
}

// ---------------------------------------------------------------------------
// Scenario isolation (issue #17): reset EVERY in-process production global so
// each scenario starts from a deterministic clean slate. Called by it_main()
// BEFORE every scenario. Without this, a prior scenario pollutes global state
// (e.g. S1 leaves g_threads populated + g_totalDecisions advanced, S4 latches
// EMERGENCY, S5 primes g_configMtime, S3 leaves g_mlModel trained) and the next
// scenario's assertions silently depend on that carry-over.
//
// Lock-ordering note (see thermal.cpp v94-工程改进包C): transitionToState()
// re-takes g_cfgMutex internally, so we must NOT hold g_cfgMutex across the
// call below. Config resets are scoped in their own tight block and released
// before transitionToState().
// ---------------------------------------------------------------------------
void itResetToFreshState() {
    // config + profile
    {
        std::lock_guard<std::mutex> cfgLk(g_cfgMutex);
        g_cfg = Config();                 // restore member-initialized defaults
        g_configMtime = 0;                // force the next loadConfig() to reload
        g_currentProfile = "balanced";
        g_currentProfileEnum = ProfileType::BALANCED;
    }

    // scanned thread table + rule table
    {
        std::lock_guard<std::mutex> thLk(g_threadsMutex);
        g_threads.clear();
    }
    g_rules.clear();                      // loadThreadRules() appends; start empty

    // counters
    g_totalDecisions.store(0);

    // thermal state machine + cached temps
    g_thermalEmergency.store(false);
    g_thermalMigrationPressure.store(0);
    g_thermalCpu.store(0); g_thermalSkin.store(0); g_thermalBattery.store(0);
    g_thermalCpuZonePath.clear();
    // Wipe thermal.cpp runtime baselines (jump-filter s_lastZoneTemp, over-temp
    // debounce streak, cache timestamp) so S4's emergency ramp doesn't leak a
    // high-temperature baseline into S10 and filter the low-start ramp reads.
    thermal_resetRuntimeStateForTest();

    // ML learners
    g_mlModel.reset();
    aatpe_ml_hybrid::instance().reset();

    g_currentScene.store((int)GameScene::MENU);
    g_it.timeline.clear();

    // drive the unified state machine back to NORMAL (takes/releases its own
    // locks internally).
    transitionToState(SystemState::NORMAL, "it-reset");
}

// v4.2.9 E7: per-scenario on-disk teardown. itResetToFreshState() resets the
// in-process globals but leaves files under STATE_DIR and the run dir untouched;
// without this, e.g. S13's persisted control_mode=active or a leftover
// control/daemon.pid survives into the next scenario (the "full suite flaky,
// isolated green" symptom). Best-effort: unlink failures are ignored.
void itTeardownScenario() {
    DIR* d = ::opendir(g_it.stateDir.c_str());
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            std::string name(e->d_name);
            if (name == "." || name == "..") continue;
            ::unlink((g_it.stateDir + "/" + name).c_str());
        }
        ::closedir(d);
    }
    for (const char* f : {"control", "daemon.pid", "daemon.lock", "pidfile"})
        ::unlink((g_it.runDir + "/" + f).c_str());
}

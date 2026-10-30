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

void itAddProcThread(int pid, const std::string& pkg, int tid, const std::string& tname,
                     unsigned long long runTime, unsigned long long nrSwitches,
                     int currentCpu, unsigned long long starttime) {
    std::string procRoot = "/proc/" + std::to_string(pid);
    std::string taskRoot = procRoot + "/task/" + std::to_string(tid);
    itWriteFile(procRoot + "/cmdline", pkg);
    itWriteFile(taskRoot + "/comm", tname + "\n");
    char sched[128];
    snprintf(sched, sizeof(sched), "%llu 0 %llu\n", runTime, nrSwitches);
    itWriteFile(taskRoot + "/schedstat", sched);
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
    for (const char* f : {"comm","stat","schedstat","status"})
        ::unlink((taskRoot + "/" + f).c_str());
    ::rmdir(taskRoot.c_str());
}

void itSetThermalTemp(const std::string& zone, int tempCelsius) {
    char b[64]; snprintf(b, sizeof(b), "%d\n", tempCelsius * 1000);
    itWriteFile(g_it.mockRoot + "/sys/class/thermal/" + zone + "/temp", b);
}

double itNowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void itSleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void itRedirectPaths() {
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
        ::mkdir((r + "/proc").c_str(), 0755);
        W(r + "/proc/stat", "cpu  200 0 100 700 0 0 0 0 0 0\n");
        thermalSetTestRoot(r);
    }
}

void itBootstrapConfig(const std::string& confText) {
    itWriteFile(g_it.confDir + "/aatpe.conf", confText);
}

void itBringUp() {
    itRedirectPaths();
    logInit();
    loadConfig();
    loadThreadRules();
    detectCpuTopology();
}

void itReadThermalFresh() {
    itSleepMs(1050);
    readThermal();
}

void itResetToFreshState() {
    {
        std::lock_guard<std::mutex> cfgLk(g_cfgMutex);
        g_cfg = Config();
        g_configMtime = 0;
        g_currentProfile = "balanced";
        g_currentProfileEnum = ProfileType::BALANCED;
    }

    {
        std::lock_guard<std::mutex> thLk(g_threadsMutex);
        g_threads.clear();
    }
    g_rules.clear();

    g_totalDecisions.store(0);

    g_thermalEmergency.store(false);
    g_thermalMigrationPressure.store(0);
    g_thermalCpu.store(0); g_thermalSkin.store(0); g_thermalBattery.store(0);
    g_thermalCpuZonePath.clear();
    thermal_resetRuntimeStateForTest();

    g_mlModel.reset();
    aatpe_ml_hybrid::instance().reset();

    g_currentScene.store((int)GameScene::MENU);
    g_it.timeline.clear();

    transitionToState(SystemState::NORMAL, "it-reset");
}

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

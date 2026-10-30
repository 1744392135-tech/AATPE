#ifndef AATPE_IT_COMMON_H
#define AATPE_IT_COMMON_H

#include "aatped.h"
#include "aatpe_ebpf.h"
#include "aatpe_federated.h"
#include "aatpe_ml_hybrid.h"
#include "webui_http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <thread>

struct VPoint {
    std::string scenario;
    std::string name;
    bool pass;
    std::string detail;
};

struct ITContext {
    std::string mockRoot;
    std::string runDir;
    std::string stateDir;
    std::string confDir;
    std::string mlModelPath;
    std::string metricsDir, decisionDir, webuiDir, logFile, pidFile, lockFile;
    std::vector<VPoint> points;
    int  currentScenario = -1;
    std::string currentScenarioName;
    std::vector<std::pair<long long, std::string>> timeline;
};

extern ITContext g_it;

typedef void (*ScenarioFn)(void);
struct ScenarioDef { const char* name; ScenarioFn fn; };

void itLog(const char* fmt, ...);

#define VP(cond, name, detail) do { \
    bool _c = (cond); \
    g_it.points.push_back({g_it.currentScenarioName, (name), _c, (detail)}); \
    if (_c) printf("      [PASS] %s\n", (name)); \
    else    printf("      [FAIL] %s  :: %s\n", (name), (detail)); \
} while(0)

#define CHECK(cond, name) VP(cond, name, "")
#define CHECK_F(cond, name, ...) do { \
    char _b[256]; snprintf(_b,sizeof(_b),__VA_ARGS__); \
    VP((cond), name, _b); \
} while(0)

void itWriteFile(const std::string& path, const std::string& content);
std::string itReadFile(const std::string& path);
void itWriteInt(const std::string& path, long long v);
long long itReadInt(const std::string& path, long long def);
void itAddProcThread(int pid, const std::string& pkg, int tid, const std::string& tname,
                     unsigned long long runTime, unsigned long long nrSwitches,
                     int currentCpu, unsigned long long starttime);
void itRemoveProcThread(int pid, int tid);
void itSetThermalTemp(const std::string& zone, int tempCelsius);
void itFakeProcStatCpuBusy(long long totalClicks, long long busyClicks);

void itRedirectPaths();
void itBootstrapConfig(const std::string& confText);
void itBringUp();
void itResetToFreshState();
void itTeardownScenario();

double itNowMs();
void itSleepMs(int ms);
void itReadThermalFresh();

#endif

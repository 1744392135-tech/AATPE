// AATPE v94 Integration Test Framework - common header
// Provides: mock-fs helpers, quantified assertion macros, per-scenario result
// collection, and a multi-cycle driver. Links the REAL production objects.
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

// ---------------------------------------------------------------------------
// Result collector: every CHECK registers one quantified verification point.
// ---------------------------------------------------------------------------
struct VPoint {
    std::string scenario;
    std::string name;
    bool pass;
    std::string detail;
};

struct ITContext {
    std::string mockRoot;     // underlying root that was bind-mounted over /sys,/proc
    std::string runDir;
    std::string stateDir;
    std::string confDir;
    std::string mlModelPath;
    std::string metricsDir, decisionDir, webuiDir, logFile, pidFile, lockFile;
    std::vector<VPoint> points;
    int  currentScenario = -1;
    std::string currentScenarioName;
    // timeline capture for thermal / state machine scenarios
    std::vector<std::pair<long long, std::string>> timeline;
};

extern ITContext g_it;

// Scenario registry: each scenario fn returns void; it must register points.
typedef void (*ScenarioFn)(void);
struct ScenarioDef { const char* name; ScenarioFn fn; };

void itLog(const char* fmt, ...);

// CHECK macros: scenario-scoped, quantified assertions.
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

// ---- Mock filesystem helpers (operate on the bind-mounted /sys and /proc) ----
void itWriteFile(const std::string& path, const std::string& content);
std::string itReadFile(const std::string& path);
void itWriteInt(const std::string& path, long long v);
long long itReadInt(const std::string& path, long long def);
// create a synthetic process in the fake /proc: pid/tid/comm/schedstat/stat
void itAddProcThread(int pid, const std::string& pkg, int tid, const std::string& tname,
                     unsigned long long runTime, unsigned long long nrSwitches,
                     int currentCpu, unsigned long long starttime);
void itRemoveProcThread(int pid, int tid);
void itSetThermalTemp(const std::string& zone, int tempCelsius);
void itFakeProcStatCpuBusy(long long totalClicks, long long busyClicks);

// Reassign the production path pointers (RUN_DIR etc.) into the mock tree.
void itRedirectPaths();
// Build runtime dirs + aatpe.conf + rules + profiles into the mock conf/run dirs.
void itBootstrapConfig(const std::string& confText);
// Standard bring-up: redirect paths, bootstrap config, load config, rules, topology.
void itBringUp();
void itResetToFreshState();
// v4.2.9 E7: wipe the VOLATILE on-disk state + control artifacts between scenarios.
// itResetToFreshState() only clears in-process globals; this removes the on-disk
// leftovers (STATE_DIR/*, run control/pid/lock) so a scenario cannot read another
// scenario's state files. Config / ML model / decisions are intentionally kept.
void itTeardownScenario();

// Small helpers
double itNowMs();
void itSleepMs(int ms);
// Force readThermal to bypass its 1s cache by sleeping enough.
void itReadThermalFresh();

#endif // AATPE_IT_COMMON_H

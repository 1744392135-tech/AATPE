
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <climits>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <deque>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <ctime>
#include <thread>
#include <chrono>
#include <mutex>
#include <mutex>
#include <atomic>
#include <regex>

#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/wait.h>
#include <poll.h>

#include "aatpe-constants.h"
#include "aatpe-version.h"
#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"
#include "aatped.h"

using namespace aatpe;
using namespace aatpe_ml;

static std::map<int, int> g_testTidCpu;
static int g_testCycle = 0;

const char* RUN_DIR      = "/tmp/aatpe_sched_ab";
const char* STATE_DIR    = "/tmp/aatpe_sched_ab/state";
const char* METRICS_DIR  = "/tmp/aatpe_sched_ab/metrics";
const char* DECISION_DIR = "/tmp/aatpe_sched_ab/decisions";
const char* WEBUI_DIR    = "/tmp/aatpe_sched_ab/webui";
const char* LOG_FILE     = "/tmp/aatpe_sched_ab/aatpe.log";
const char* PID_FILE     = "/tmp/aatpe_sched_ab/daemon.pid";
const char* LOCK_FILE    = "/tmp/aatpe_sched_ab/lock";
const char* CONF_DIR     = "/tmp/aatpe_sched_ab/conf";
const char* ML_MODEL_PATH = "/tmp/aatpe_sched_ab/ml_model.bin";

std::atomic<unsigned int> g_debugSampleCounter{0};

Config g_cfg;
std::mutex g_cfgMutex;
Config g_preEmergencyCfg;
std::string g_preEmergencyProfile;
bool g_preEmergencyAutoProfile = false;
std::string g_currentProfile = "balanced";
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;
std::atomic<bool> g_reloadConfigRequested{false};

DynamicScoreWeights g_dynamicScoreWeights;
std::mutex g_dynamicWeightsMutex;

float g_fusionTemperature = 1.0f;

char g_hmacKey[128] = {0};
std::string g_hmacKeyStatus = "generated";
bool g_hmacActive = false;

int g_federatedRounds = 0;
int g_federatedExports = 0;
int g_federatedImports = 0;
int g_federatedTrustedImports = 0;

float g_abGroupAAvg = 0;
float g_abGroupBAvg = 0;
float g_abPValue = 1.0f;
ABTestStats g_abStats;

float g_avgPredictionMAE = 0;

int g_thermalCpu = 40;
int g_thermalSkin = 35;
int g_thermalBattery = 30;
std::atomic<bool> g_thermalEmergency{false};
std::atomic<int> g_thermalMigrationPressure{0};
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<long long> g_keepSafetyLatency{0};
std::atomic<long long> g_applySafetyLatency{0};

const char* g_gpuType = "Adreno-650";
float g_gpuUtilization = 0.0f;
int g_gpuBusyPercent = 0;
bool g_gpuBottleneck = false;

std::string g_ebpfLoader = "none";
bool g_ebpfActive = false;

std::string g_foregroundApp = "com.test.game";
time_t g_startTime = 0;
std::string g_cachedForegroundApp = "com.test.game";
long long g_foregroundCacheTimeMs = 0;

std::unordered_map<std::string, PerAppConfig> g_perAppConfigs;
PerAppConfig g_currentAppConfig;

GameScene g_currentScene = GameScene::COMBAT;
int g_sceneChangeCount = 0;

int g_currentPowerMw = 2500;
int g_batteryCurrentUa = 500000;
int g_batteryVoltageUv = 4000000;

int g_batteryHealthPercent = 95;
int g_batteryCycleCount = 120;
bool g_batteryAged = false;

bool g_audioModeActive = false;
int g_audioIsolatedCpu = -1;

std::unordered_map<std::string, AppLaunchRecord> g_appLaunchHistory;

std::atomic<bool> g_ueventFallbackActive{false};
std::atomic<int> g_ueventEventCount{0};

bool g_helperApkInstalled = false;

std::atomic<sig_atomic_t> g_shouldExit{0};
std::atomic<sig_atomic_t> g_reloadProfile{0};

int g_cpuCount = 8;
uint64_t g_topologyGeneration = 1;
std::vector<int> g_allCpus = {0, 1, 2, 3, 4, 5, 6, 7};
std::vector<int> g_littlePool = {0, 1, 2, 3};
std::vector<int> g_midPool = {4, 5, 6};
std::vector<int> g_bigPool = {7};

std::map<int, CpuMetrics> g_cpuMetrics;

std::vector<ThreadRule> g_rules;

std::unordered_map<int, ThreadInfo> g_threads;
std::mutex g_threadsMutex;

std::set<int> g_unmigratableThreads;
std::mutex g_unmigratableMutex;

std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;

aatpe_ml::LearningModel g_mlModel;
std::mutex g_mlMutex;
time_t g_mlLastSave = 0;
std::atomic<unsigned long> g_mlSaveSuccess{0};
std::atomic<unsigned long> g_mlSaveFail{0};

std::atomic<long long> g_totalDecisions{0};
std::atomic<long long> g_totalMigrations{0};
std::atomic<long long> g_highLoadCycles{0};
std::atomic<long long> g_keepProtected{0};
std::atomic<long long> g_keepPassive{0};
std::atomic<long long> g_keepThermalEmergency{0};
std::atomic<long long> g_keepNoCpu{0};
std::atomic<long long> g_keepLowConf{0};
std::atomic<long long> g_keepHighCost{0};
std::atomic<long long> g_keepAlreadyOptimal{0};
std::atomic<long long> g_migrateActual{0};
std::atomic<long long> g_applySuccess{0};
std::atomic<long long> g_applyFailed{0};
std::atomic<long long> g_applyFailedEinval{0};
std::atomic<long long> g_applyFailedEsrch{0};
std::atomic<long long> g_applyFailedEperm{0};
std::atomic<long long> g_applyFailedOther{0};
std::atomic<long long> g_applyRateLimited{0};
std::atomic<long long> g_applyPassive{0};
std::atomic<long long> g_applyThermalEmergency{0};
std::atomic<long long> g_applyCooldownSkip{0};
std::atomic<long long> g_applyCpusetSkip{0};
std::atomic<long long> g_applyKernelSkip{0};
std::atomic<long long> g_applyUnmigratableSkip{0};
std::atomic<long long> g_applyIntervalSkip{0};
std::atomic<long long> g_mlPendingCreated{0};
std::atomic<long long> g_mlOutcomeMeasured{0};
std::atomic<long long> g_mlOutcomeValid{0};
std::atomic<long long> g_mlOutcomeSkipSleeping{0};
std::atomic<long long> g_mlOutcomeSkipSaturated{0};
std::atomic<long long> g_mlOutcomeSkipReset{0};
std::atomic<long long> g_mlOutcomeSkipOther{0};
std::atomic<long long> g_mlPendingExpired{0};
std::atomic<long long> g_mlPendingTidReused{0};
std::atomic<long long> g_mlPendingThreadExited{0};
std::atomic<long long> g_cooldownBypassCount{0};

MigrationBudget g_migrationBudget;
int g_migrationCount = 0;
time_t g_migrationWindowStart = 0;
std::mutex g_lastMigMutex;
std::map<int, time_t> g_lastMigrationTime;
std::mutex g_migFailMutex;
std::map<int, time_t> g_migFailCooldown;
std::map<int, int> g_migFailCount;
std::map<int, int> g_migFailCpu;
time_t g_cooldownFloorWindowStart = 0;

std::vector<WebuiDecision> g_decisionRing;
std::mutex g_decisionRingMutex;
std::atomic<int> g_explainTid{-1};

std::map<int, std::pair<unsigned long long, unsigned long long>> g_prevCpuStat;
std::unordered_map<int, ThreadInfo> g_scannedThreadMap;
std::unordered_map<std::string, int> g_threadRoleCache;
std::vector<SchedstatCpuData> g_schedstatData;
float g_systemLoad = 50.0f;
std::atomic<bool> g_frameCritical{false};
std::atomic<bool> g_profileLock{false};

void logInit() {}
void logMsg(int level, const char* fmt, ...) {
    (void)level; (void)fmt;
}

std::string readFile(const char* path) { (void)path; return ""; }
bool writeFile(const char* path, const std::string& content) { (void)path; (void)content; return true; }
bool writeFile(const char* path, int val) { (void)path; (void)val; return true; }
bool writeFile(const char* path, float value) { (void)path; (void)value; return true; }
bool writeFile(const char* path, long long value) { (void)path; (void)value; return true; }
bool writeFileAtomic(const char* path, const std::string& content) { (void)path; (void)content; return true; }
std::string getModuleDir() { return "/tmp/aatpe_sched_ab"; }
bool validateConfig() { return true; }
int readInt(const char* path, int def) { (void)path; return def; }
void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}
std::string readFirstLine(const char* path) { (void)path; return ""; }

int aatpeAndroidSdkInt() { return 29; }
const char* aatpeAndroidVersionName() { return "10"; }
bool aatpeIsCgroupV2Cpuset() { return false; }
std::string aatpeCpusetMountRoot() { return "/dev/cpuset"; }

void detectCpuTopology() {}

void readCpuMetrics() {}
void readThermal() {}

void loadThreadRules() {}
bool matchPattern(const std::string& text, const std::string& pattern) {
    if (pattern == "*") return true;
    if (pattern.empty()) return text.empty();
    return text.find(pattern) != std::string::npos;
}
ThreadRole classifyThread(int pid, int tid, const std::string& tname,
                          const std::string& pname, int& outPriority, std::string& outSubtype) {
    (void)pid; (void)tid; (void)tname; (void)pname;
    outPriority = 50;
    outSubtype = "";
    return ROLE_S5_BACKGROUND;
}

int readThreadCurrentCpu(int pid, int tid) {
    (void)pid;
    auto it = g_testTidCpu.find(tid);
    if (it != g_testTidCpu.end()) return it->second;
    return -1;
}

std::set<int> getThreadAffinityCpus(int tid) {
    (void)tid;
    return std::set<int>(g_allCpus.begin(), g_allCpus.end());
}

std::set<int> parseAffinityMask(const std::string& maskStr) {
    (void)maskStr;
    return std::set<int>(g_allCpus.begin(), g_allCpus.end());
}

cpu_set_t getThreadAllowedCpus(int tid) {
    (void)tid;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int cpu : g_allCpus) CPU_SET(cpu, &mask);
    return mask;
}

bool isCpuAllowed(int cpu, const cpu_set_t& allowed) {
    return CPU_ISSET(cpu, &allowed);
}

cpu_set_t parseCpusetCpuList(const std::string& listStr) {
    (void)listStr;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int cpu : g_allCpus) CPU_SET(cpu, &mask);
    return mask;
}
std::string readThreadCpuset(int tid) { (void)tid; return ""; }
cpu_set_t readCpusetCpus(const std::string& cpusetPath) {
    (void)cpusetPath;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int cpu : g_allCpus) CPU_SET(cpu, &mask);
    return mask;
}

void readBatteryPower() {}
int computePowerPenalty(int cpuId, int threadImportance) {
    (void)cpuId; (void)threadImportance;
    return 0;
}

void readBatteryHealth() {}
int getBatteryAgePenalty(int cpuId) { (void)cpuId; return 0; }

void readGpuUtilization() {}
bool isGpuBottleneck(float cpuUtilization) { (void)cpuUtilization; return false; }
int getGpuBottleneckDecisionInterval() { return 200; }

bool detectAudioPlayback() { return false; }
bool isAudioThread(const char* threadName) {
    std::string n(threadName);
    return n.find("Audio") != std::string::npos || n.find("audio") != std::string::npos;
}
int getAudioThreadMigrationPenalty(const char* threadName) {
    return isAudioThread(threadName) ? 50 : 0;
}

void readSurfaceFlinger() {}
int aatpeDetectRefreshRate() { return 60; }
std::string aatpeExec(const std::string& cmd, const char* debugTag) { (void)cmd; (void)debugTag; return ""; }
bool dumpsysAvailable() { return false; }
void triggerDumpsysCooldown() {}
std::string aatpeDetectForegroundApp() { return "com.test.game"; }
bool detectGameProcess() { return true; }
int detectBatteryState() { return 0; }
void autoSelectProfile() {}
bool checkSystemLoad() { return false; }
void aatpeProbeEnvironment() {}

ProfileType parseProfileType(const std::string& s) {
    if (s == "balanced") return ProfileType::BALANCED;
    if (s == "gaming") return ProfileType::GAMING;
    if (s == "battery") return ProfileType::BATTERY;
    if (s == "thermal") return ProfileType::THERMAL;
    return ProfileType::UNKNOWN;
}
const char* profileTypeToString(ProfileType p) {
    switch (p) {
        case ProfileType::BALANCED: return "balanced";
        case ProfileType::GAMING: return "gaming";
        case ProfileType::BATTERY: return "battery";
        case ProfileType::THERMAL: return "thermal";
        default: return "unknown";
    }
}
RefreshRateMode parseRefreshRateMode(const std::string& s) { (void)s; return RefreshRateMode::PEAK; }
const char* refreshRateModeToString(RefreshRateMode m) { (void)m; return "peak"; }

float softmaxFuseScores(float modelScore, float heuristicScore, float temperature) {
    if (temperature < 0.0f) temperature = -temperature;
    if (temperature < 0.01f) temperature = 0.01f;
    float m = modelScore / temperature;
    float h = heuristicScore / temperature;
    float maxVal = (m > h) ? m : h;
    m -= maxVal; h -= maxVal;
    float em = std::exp(m);
    float eh = std::exp(h);
    float total = em + eh;
    if (total < 1e-10f) return 0.5f * (modelScore + heuristicScore);
    return (modelScore * em + heuristicScore * eh) / total;
}

void loadConfig() {}
void loadProfile(const std::string& profile) {
    g_currentProfile = profile;
    g_currentProfileEnum = parseProfileType(profile);
}

GameScene detectGameScene(float cpuLoad, float frameTimeJitter, float avgFrameTime) {
    (void)cpuLoad; (void)frameTimeJitter; (void)avgFrameTime;
    return GameScene::COMBAT;
}
const char* sceneToString(GameScene s) { (void)s; return "COMBAT"; }

void recordAppLaunch(const char* packageName, int mainTid, int renderTid) {
    (void)packageName; (void)mainTid; (void)renderTid;
}
void checkAndPreloadNewApps() {}
int getPreloadedPreferredCpu(const char* packageName) { (void)packageName; return 4; }

void loadPerAppConfigs(const char* confPath) { (void)confPath; }
void checkAndApplyPerAppConfig(const char* foregroundApp) { (void)foregroundApp; }

int computeAdaptiveInterval(float systemLoad, bool jankDetected, bool hasForeground) {
    (void)systemLoad; (void)jankDetected; (void)hasForeground;
    return 200;
}

std::string getCachedForegroundApp() { return g_cachedForegroundApp; }

void incrementalThreadScan() {}

void watchdogTakeoverScheduling() {}

void lazyCleanupDecisionsDir() {}

int getCachedThreadRole(const std::string& threadName) {
    auto it = g_threadRoleCache.find(threadName);
    return (it != g_threadRoleCache.end()) ? it->second : -1;
}
void setCachedThreadRole(const std::string& threadName, int role) {
    g_threadRoleCache[threadName] = role;
}
void invalidateThreadRoleCache(const std::string& threadName) {
    g_threadRoleCache.erase(threadName);
}

bool readSchedstatCpuUtil() { return false; }
float computeSchedstatCpuUtil(int cpuId) { (void)cpuId; return 0.0f; }
float readCpuUtilizationSchedstat(int cpuId) { (void)cpuId; return 0.0f; }

bool verifyCommandHmac(const char* command, const char* signature) { (void)command; (void)signature; return true; }
std::string computeCommandHmac(const char* command) { (void)command; return ""; }

void updateMlSaveStateFiles(bool success, const char* path) { (void)success; (void)path; }

int abTestAssignThread(int tid) { (void)tid; return 0; }
void abTestRecordFrameTime(int group, float frameTime) { (void)group; (void)frameTime; }
void abTestRecordJank(int group) { (void)group; }
void abTestRecordMigration(int group) { (void)group; }
int abTestDetermineWinner(void) { return -1; }

void recordWebuiDecision(int tid, const std::string& name, const std::string& role,
                          int fromCpu, int toCpu, int score, int confidence,
                          const std::string& type) {
    (void)tid; (void)name; (void)role; (void)fromCpu; (void)toCpu;
    (void)score; (void)confidence; (void)type;
}
void writeThreadsJson() {}
void writeDecisionsJson() {}
void writeExplainJson() {}
void writeModelJson() {}
void writeConfigJson() {}
void writeSystemJson() {}
void writePerCpuJson() {}
void writeLogsJson() {}

void scanThreads() {}
void persistThreadDb(const std::unordered_map<int, ThreadInfo>& threads) { (void)threads; }

void startConfigHotReload() {}
void stopConfigHotReload() {}

void signalHandler(int sig) { (void)sig; }
void processControlCommands() {}
void daemonInit() {}
void daemonLoop() {}

void cmdStatus() {}
void cmdThreads() {}
void cmdBenchmark() {}
void cmdExplain(int targetTid) { (void)targetTid; }

void checkUeventFallback() {}
void pollSysfsEvents() {}

bool MigrationBudget::tryAcquire(bool frameCritical) {
    (void)frameCritical;
    return true;
}

static double betaIncomplete(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    double lbeta = lgamma(a) + lgamma(b) - lgamma(a + b);
    double front = exp(log(x) * a + log(1.0 - x) * b - lbeta) / a;
    double fpmin = 1e-30;
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (fabs(d) < fpmin) d = fpmin;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= 200; m++) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < fpmin) d = fpmin;
        c = 1.0 + aa / c;
        if (fabs(c) < fpmin) c = fpmin;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < fpmin) d = fpmin;
        c = 1.0 + aa / c;
        if (fabs(c) < fpmin) c = fpmin;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 3e-7) break;
    }
    return front * h;
}

static double tDistCDF(double t, double df) {
    if (df <= 0.0) return 0.5;
    double x = df / (df + t * t);
    double ibeta = betaIncomplete(x, df / 2.0, 0.5);
    if (t > 0) return 1.0 - 0.5 * ibeta;
    return 0.5 * ibeta;
}

static double welchTTest(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() < 2 || b.size() < 2) return 1.0;
    double meanA = 0, meanB = 0;
    for (double v : a) meanA += v;
    for (double v : b) meanB += v;
    meanA /= (double)a.size();
    meanB /= (double)b.size();
    double varA = 0, varB = 0;
    for (double v : a) varA += (v - meanA) * (v - meanA);
    for (double v : b) varB += (v - meanB) * (v - meanB);
    varA /= (double)(a.size() - 1);
    varB /= (double)(b.size() - 1);
    if (varA < 0) varA = 0;
    if (varB < 0) varB = 0;
    double se = sqrt(varA / (double)a.size() + varB / (double)b.size());
    if (se < 1e-12) return 1.0;
    double t = (meanA - meanB) / se;
    double va = varA / (double)a.size();
    double vb = varB / (double)b.size();
    double df = (va + vb) * (va + vb) /
                (va * va / (double)(a.size() - 1) + vb * vb / (double)(b.size() - 1));
    if (df < 1.0) df = 1.0;
    double p = 2.0 * (1.0 - tDistCDF(fabs(t), df));
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
}

struct TestConfig {
    std::string name;
    std::string controlMode;
    std::string profile;
    bool mlLearningEnabled;
    bool mlHybridEnabled;
    int frameAware;
    int autoProfile;
};

struct SimThread {
    ThreadInfo info;
    float baseRunRatio;
    float baseSwitchRate;
    bool frameProducer;
    CpuPool expectedPool;
};

struct PendingML {
    int tid;
    int targetCpu;
    int targetPool;
    ThreadFeatures features;
    unsigned long long beforeRunTime;
    int createCycle;
};

struct CycleMetrics {
    int migrations = 0;
    float bigUtil = 0;
    float midUtil = 0;
    float littleUtil = 0;
    float avgScore = 0;
    float frameTimeDeviation = 0;
    int thermalPenalties = 0;
    int roleCorrect = 0;
    int roleTotal = 0;
};

struct GroupResult {
    std::string groupName;
    std::string configADesc;
    std::string configBDesc;
    TestConfig configA;
    TestConfig configB;
    std::vector<CycleMetrics> cyclesA;
    std::vector<CycleMetrics> cyclesB;
    int totalMigA = 0;
    float avgBigUtilA = 0, avgMidUtilA = 0, avgLittleUtilA = 0;
    float avgScoreA = 0;
    float avgFrameDevA = 0;
    int totalThermalA = 0;
    float roleCorrectnessA = 0;
    int mlSamplesA = 0;
    int hybridUpdatesA = 0;
    int totalMigB = 0;
    float avgBigUtilB = 0, avgMidUtilB = 0, avgLittleUtilB = 0;
    float avgScoreB = 0;
    float avgFrameDevB = 0;
    int totalThermalB = 0;
    float roleCorrectnessB = 0;
    int mlSamplesB = 0;
    int hybridUpdatesB = 0;
    double pMigrations = 1.0;
    double pScore = 1.0;
    double pBigUtil = 1.0;
    double pFrameDev = 1.0;
};

static uint32_t g_rngState = 0x12345678;
static inline uint32_t rngNext() {
    g_rngState ^= g_rngState << 13;
    g_rngState ^= g_rngState >> 17;
    g_rngState ^= g_rngState << 5;
    return g_rngState;
}
static inline float rngFloat(float lo, float hi) {
    return lo + (hi - lo) * ((float)rngNext() / 4294967295.0f);
}

static void setupCpuMetrics() {
    g_cpuMetrics.clear();
    for (int i = 0; i <= 3; i++) {
        CpuMetrics m;
        m.capacity = 380 + i * 5;
        m.curFreq = 1200;
        m.maxFreq = 1800;
        m.freqPct = 67;
        m.util = 20;
        m.temp = 38;
        g_cpuMetrics[i] = m;
    }
    for (int i = 4; i <= 6; i++) {
        CpuMetrics m;
        m.capacity = 750 + (i - 4) * 10;
        m.curFreq = 1600;
        m.maxFreq = 2400;
        m.freqPct = 67;
        m.util = 30;
        m.temp = 45;
        g_cpuMetrics[i] = m;
    }
    {
        CpuMetrics m;
        m.capacity = 1024;
        m.curFreq = 2000;
        m.maxFreq = 2900;
        m.freqPct = 69;
        m.util = 40;
        m.temp = 52;
        g_cpuMetrics[7] = m;
    }
}

static std::vector<SimThread> createSimulatedThreads() {
    std::vector<SimThread> threads;

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10001;
        st.info.starttime = 1000000;
        st.info.pname = "com.test.game";
        st.info.tname = "UnityMain";
        st.info.role = ROLE_S3_GAME;
        st.info.subtype = "game_main";
        st.info.priority = 30;
        st.info.protect = false;
        st.info.runTime = 5000000;
        st.info.nrSwitches = 2000;
        st.info.currentCpu = 1;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.82f;
        st.baseSwitchRate = 45.0f;
        st.frameProducer = true;
        st.expectedPool = CpuPool::BIG;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10002;
        st.info.starttime = 1000001;
        st.info.pname = "com.test.game";
        st.info.tname = "RenderThread";
        st.info.role = ROLE_S2_RENDER;
        st.info.subtype = "render";
        st.info.priority = 25;
        st.info.protect = false;
        st.info.runTime = 4800000;
        st.info.nrSwitches = 1500;
        st.info.currentCpu = 2;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.78f;
        st.baseSwitchRate = 30.0f;
        st.frameProducer = true;
        st.expectedPool = CpuPool::BIG;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10003;
        st.info.starttime = 1000002;
        st.info.pname = "com.test.game";
        st.info.tname = "UnityGfxWorker";
        st.info.role = ROLE_S3_GAME;
        st.info.subtype = "game_worker";
        st.info.priority = 35;
        st.info.protect = false;
        st.info.runTime = 3500000;
        st.info.nrSwitches = 800;
        st.info.currentCpu = 0;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.60f;
        st.baseSwitchRate = 18.0f;
        st.frameProducer = false;
        st.expectedPool = CpuPool::MID;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10004;
        st.info.starttime = 1000003;
        st.info.pname = "com.test.game";
        st.info.tname = "AudioTrack";
        st.info.role = ROLE_S1_UI;
        st.info.subtype = "audio";
        st.info.priority = 20;
        st.info.protect = false;
        st.info.runTime = 900000;
        st.info.nrSwitches = 400;
        st.info.currentCpu = 3;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.15f;
        st.baseSwitchRate = 10.0f;
        st.frameProducer = false;
        st.expectedPool = CpuPool::MID;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10005;
        st.info.starttime = 1000004;
        st.info.pname = "com.test.game";
        st.info.tname = "BackgroundLoader";
        st.info.role = ROLE_S5_BACKGROUND;
        st.info.subtype = "background";
        st.info.priority = 80;
        st.info.protect = false;
        st.info.runTime = 1800000;
        st.info.nrSwitches = 200;
        st.info.currentCpu = 4;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.30f;
        st.baseSwitchRate = 5.0f;
        st.frameProducer = false;
        st.expectedPool = CpuPool::LITTLE;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10006;
        st.info.starttime = 1000005;
        st.info.pname = "com.test.game";
        st.info.tname = "PhysicsWorker";
        st.info.role = ROLE_S4_THROUGHPUT;
        st.info.subtype = "physics";
        st.info.priority = 40;
        st.info.protect = false;
        st.info.runTime = 4200000;
        st.info.nrSwitches = 600;
        st.info.currentCpu = 5;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.70f;
        st.baseSwitchRate = 15.0f;
        st.frameProducer = false;
        st.expectedPool = CpuPool::MID;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 200;
        st.info.tid = 201;
        st.info.starttime = 500000;
        st.info.pname = "/system/bin/surfaceflinger";
        st.info.tname = "surfaceflinger";
        st.info.role = ROLE_S0_SYSTEM;
        st.info.subtype = "system";
        st.info.priority = 10;
        st.info.protect = true;
        st.info.runTime = 2400000;
        st.info.nrSwitches = 1200;
        st.info.currentCpu = 6;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.40f;
        st.baseSwitchRate = 25.0f;
        st.frameProducer = true;
        st.expectedPool = CpuPool::MID;
        threads.push_back(st);
    }

    {
        SimThread st;
        st.info.pid = 10000;
        st.info.tid = 10008;
        st.info.starttime = 1000007;
        st.info.pname = "com.test.game";
        st.info.tname = "InputReader";
        st.info.role = ROLE_S1_UI;
        st.info.subtype = "input";
        st.info.priority = 30;
        st.info.protect = false;
        st.info.runTime = 600000;
        st.info.nrSwitches = 350;
        st.info.currentCpu = 0;
        CPU_ZERO(&st.info.originalAffinity);
        st.info.affinityModified = false;
        st.baseRunRatio = 0.10f;
        st.baseSwitchRate = 8.0f;
        st.frameProducer = false;
        st.expectedPool = CpuPool::MID;
        threads.push_back(st);
    }

    return threads;
}

static const int NUM_CYCLES = 150;
static const int CYCLE_US = 200000;
static const int ML_OUTCOME_DELAY = 5;

static GroupResult runConfig(const TestConfig& cfg, const std::string& label) {
    GroupResult result;
    result.configA = cfg;
    (void)label;

    g_rngState = 0x12345678;

    g_totalDecisions = 0;
    g_totalMigrations = 0;
    g_keepProtected = 0;
    g_keepPassive = 0;
    g_keepNoCpu = 0;
    g_keepLowConf = 0;
    g_keepHighCost = 0;
    g_keepAlreadyOptimal = 0;
    g_migrateActual = 0;
    g_mlPendingCreated = 0;
    g_mlOutcomeMeasured = 0;
    g_mlOutcomeValid = 0;

    g_cfg.controlMode = cfg.controlMode;
    g_cfg.mlLearningEnabled = cfg.mlLearningEnabled;
    g_cfg.mlHybridEnabled = cfg.mlHybridEnabled;
    g_cfg.frameAware = cfg.frameAware;
    g_cfg.autoProfile = cfg.autoProfile;
    g_cfg.learningEnabled = false;
    g_cfg.confidenceThreshold = 8;
    g_cfg.migrationCostThreshold = 6;
    g_cfg.maxMigrationsPerSec = 80;
    g_cfg.maxMigrationsPerCycle = 20;
    g_cfg.decisionInterval = 200;
    g_cfg.targetFrameTime = 16667;
    g_cfg.frameCriticalThreshold = 12000;
    g_cfg.thermalEnabled = true;
    g_cfg.thermalLimitCpu = 120;
    g_cfg.mlMaxAlpha = 0.3f;
    g_cfg.mlInitialLR = 0.01f;
    g_cfg.mlDecay = 0.999f;
    g_cfg.mlSaveInterval = 30;
    g_cfg.mlWeightClamp = 10.0f;
    g_cfg.ignoreSystemThreads = 1;
    g_cfg.maxTrackedThreads = 512;
    g_cfg.threadScanInterval = 5;
    g_cfg.historyWeight = 30;
    g_cfg.learningDecay = 95;
    g_cfg.minSamplesForLearning = 5;

    g_currentProfile = cfg.profile;
    g_currentProfileEnum = parseProfileType(cfg.profile);

    setupCpuMetrics();

    initScoringArrays(g_cpuCount);

    {
        std::lock_guard<std::mutex> mlLock(g_mlMutex);
        g_mlModel.reset();
        g_mlModel.init(3);
        g_mlModel.setEnabled(cfg.mlLearningEnabled);
        g_mlModel.setMaxAlpha(g_cfg.mlMaxAlpha);
        g_mlModel.setInitialLR(g_cfg.mlInitialLR);
        g_mlModel.setDecay(g_cfg.mlDecay);
        g_mlModel.setWeightClamp(g_cfg.mlWeightClamp);
        std::vector<int> poolAssign(8, 0);
        for (int i = 0; i <= 3; i++) poolAssign[i] = 0;
        for (int i = 4; i <= 6; i++) poolAssign[i] = 1;
        poolAssign[7] = 2;
        g_mlModel.setCpuPoolAssignment(poolAssign);
    }

    aatpe_ml_hybrid::instance().reset();
    if (cfg.mlHybridEnabled) {
        aatpe_ml_hybrid::instance().init(3);
        aatpe_ml_hybrid::instance().setEnabled(true);
    }

    std::vector<SimThread> simThreads = createSimulatedThreads();

    g_testTidCpu.clear();
    for (auto& st : simThreads) {
        g_testTidCpu[st.info.tid] = st.info.currentCpu;
    }

    std::vector<PendingML> pendingML;

    int hybridUpdateCount = 0;
    int mlSampleCount = 0;

    for (int cycle = 0; cycle < NUM_CYCLES; cycle++) {
        g_testCycle = cycle;
        CycleMetrics cm;

        for (auto& st : simThreads) {
            float variation = rngFloat(0.85f, 1.15f);
            float runRatio = st.baseRunRatio * variation;
            if (runRatio > 1.0f) runRatio = 1.0f;
            st.info.runTime += (unsigned long long)(runRatio * CYCLE_US * 1000);
            st.info.nrSwitches += (unsigned long long)(st.baseSwitchRate * rngFloat(0.7f, 1.3f));
        }

        std::map<int, float> cpuUtil;
        for (int cpu : g_allCpus) cpuUtil[cpu] = 0.0f;
        for (auto& st : simThreads) {
            int cpu = st.info.currentCpu;
            cpuUtil[cpu] += st.baseRunRatio * 100.0f;
        }
        for (int cpu : g_allCpus) {
            if (cpuUtil[cpu] > 100.0f) cpuUtil[cpu] = 100.0f;
            g_cpuMetrics[cpu].util = (int)cpuUtil[cpu];
            int maxFreq = g_cpuMetrics[cpu].maxFreq;
            g_cpuMetrics[cpu].curFreq = (int)(maxFreq * (0.4f + 0.6f * cpuUtil[cpu] / 100.0f));
            g_cpuMetrics[cpu].freqPct = (int)(100.0f * g_cpuMetrics[cpu].curFreq / maxFreq);
        }

        int thermalPenalties = 0;
        for (int cpu : g_allCpus) {
            int baseTemp = (cpu <= 3) ? 38 : (cpu <= 6) ? 45 : 52;
            int targetTemp = baseTemp + (int)(cpuUtil[cpu] * 0.25f);
            int curTemp = g_cpuMetrics[cpu].temp;
            int newTemp = curTemp + (int)((targetTemp - curTemp) * 0.3f);
            g_cpuMetrics[cpu].temp = newTemp;
            if (newTemp > thermal::kHighTempThreshold) thermalPenalties++;
        }
        cm.thermalPenalties = thermalPenalties;

        int maxTemp = 0;
        for (int cpu : g_allCpus) maxTemp = std::max(maxTemp, g_cpuMetrics[cpu].temp);
        if (maxTemp > 75) {
            g_thermalMigrationPressure = (maxTemp - 75) * 3;
        } else {
            g_thermalMigrationPressure = 0;
        }

        float totalUtil = 0;
        for (int cpu : g_allCpus) totalUtil += cpuUtil[cpu];
        g_systemLoad = totalUtil / g_cpuCount;

        bool frameCritical = false;
        if (cfg.frameAware) {
            for (auto& st : simThreads) {
                if (st.frameProducer && st.info.role != ROLE_S0_SYSTEM) {
                    int cpu = st.info.currentCpu;
                    if (cpuUtil[cpu] > 75.0f) {
                        frameCritical = true;
                        break;
                    }
                }
            }
        }
        g_frameCritical = frameCritical;

        precomputeCpuBaseScores();

        int migrationsThisCycle = 0;
        float scoreSum = 0;
        int scoreCount = 0;
        int roleCorrect = 0;
        int roleTotal = 0;

        for (auto& st : simThreads) {
            if (st.info.protect || st.info.role == ROLE_S0_SYSTEM) {
                roleTotal++;
                CpuPool curPool = getPoolForCpu(st.info.currentCpu);
                if (curPool == st.expectedPool || st.expectedPool == CpuPool::UNKNOWN) {
                    roleCorrect++;
                }
                continue;
            }

            Decision d = decidePlacement(st.info, g_cfg);
            g_totalDecisions++;

            if (d.action == "MIGRATE") {
                migrationsThisCycle++;
                g_totalMigrations++;
                scoreSum += (float)d.score;
                scoreCount++;

                if (d.mlActive && cfg.mlLearningEnabled) {
                    PendingML pml;
                    pml.tid = st.info.tid;
                    pml.targetCpu = d.targetCpu;
                    pml.targetPool = (int)d.targetPool;
                    pml.features = d.mlFeatures;
                    pml.beforeRunTime = st.info.runTime;
                    pml.createCycle = cycle;
                    pendingML.push_back(pml);
                    g_mlPendingCreated++;
                }

                st.info.currentCpu = d.targetCpu;
                g_testTidCpu[st.info.tid] = d.targetCpu;
                recordMigrationCount(st.info.tid);

                roleTotal++;
                CpuPool newPool = getPoolForCpu(d.targetCpu);
                if (newPool == st.expectedPool) roleCorrect++;
            } else {
                if (d.score > 0) {
                    scoreSum += (float)d.score;
                    scoreCount++;
                }
                roleTotal++;
                CpuPool curPool = getPoolForCpu(st.info.currentCpu);
                if (curPool == st.expectedPool) roleCorrect++;
            }
        }

        cm.migrations = migrationsThisCycle;
        cm.avgScore = (scoreCount > 0) ? scoreSum / scoreCount : 0.0f;
        cm.roleCorrect = roleCorrect;
        cm.roleTotal = roleTotal;

        if (cfg.mlLearningEnabled) {
            std::vector<PendingML> stillPending;
            for (auto& pml : pendingML) {
                if (cycle - pml.createCycle >= ML_OUTCOME_DELAY) {
                    for (auto& st : simThreads) {
                        if (st.info.tid == pml.tid) {
                            CpuPool curPool = getPoolForCpu(st.info.currentCpu);
                            float outcome = 0.5f;
                            if (curPool == st.expectedPool) {
                                outcome = 0.85f;
                            } else if ((int)curPool > (int)st.expectedPool) {
                                outcome = 0.65f;
                            } else {
                                outcome = 0.25f;
                            }
                            outcome += rngFloat(-0.1f, 0.1f);
                            if (outcome < 0.0f) outcome = 0.0f;
                            if (outcome > 1.0f) outcome = 1.0f;

                            {
                                std::lock_guard<std::mutex> mlLock(g_mlMutex);
                                if (pml.targetPool >= 0 && pml.targetPool < g_mlModel.getPoolCount()) {
                                    g_mlModel.update(pml.targetPool, pml.features, outcome);
                                    mlSampleCount++;
                                    g_mlOutcomeValid++;
                                }
                                g_mlOutcomeMeasured++;
                            }
                            break;
                        }
                    }
                } else {
                    stillPending.push_back(pml);
                }
            }
            pendingML = stillPending;
        }

        if (cfg.mlHybridEnabled && (cycle % 5 == 0) && cycle > 0) {
            std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snapshots;
            std::vector<uint64_t> cpuRuntimeNs(8, 0);
            for (auto& st : simThreads) {
                aatpe_ml_hybrid::ThreadCpuSnapshot snap;
                snap.tid = st.info.tid;
                snap.cpu = st.info.currentCpu;
                snap.pool = (int)getPoolForCpu(st.info.currentCpu);
                snap.runTimeNs = st.info.runTime;
                snap.timestamp = (uint64_t)cycle * CYCLE_US * 1000ULL;
                snapshots.push_back(snap);
                cpuRuntimeNs[st.info.currentCpu] += (uint64_t)(st.baseRunRatio * CYCLE_US * 1000);
            }
            int prevUpdates = aatpe_ml_hybrid::instance().getTotalUpdates();
            aatpe_ml_hybrid::instance().tick(snapshots, cpuRuntimeNs, 8);
            int newUpdates = aatpe_ml_hybrid::instance().getTotalUpdates();
            hybridUpdateCount += (newUpdates - prevUpdates);
        }

        float bigUtilSum = 0, midUtilSum = 0, littleUtilSum = 0;
        for (int cpu : g_bigPool) bigUtilSum += cpuUtil[cpu];
        for (int cpu : g_midPool) midUtilSum += cpuUtil[cpu];
        for (int cpu : g_littlePool) littleUtilSum += cpuUtil[cpu];
        cm.bigUtil = bigUtilSum / g_bigPool.size();
        cm.midUtil = midUtilSum / g_midPool.size();
        cm.littleUtil = littleUtilSum / g_littlePool.size();

        float frameDevSum = 0;
        int frameProducers = 0;
        for (auto& st : simThreads) {
            if (st.frameProducer && st.info.role != ROLE_S0_SYSTEM) {
                int cpu = st.info.currentCpu;
                CpuPool pool = getPoolForCpu(cpu);
                float baseDev = 1.5f;
                if (pool == CpuPool::BIG) baseDev = 0.8f;
                else if (pool == CpuPool::MID) baseDev = 1.5f;
                else baseDev = 3.0f;
                baseDev += cpuUtil[cpu] * 0.03f;
                if (cfg.frameAware && frameCritical) baseDev *= 0.7f;
                frameDevSum += baseDev * rngFloat(0.8f, 1.2f);
                frameProducers++;
            }
        }
        cm.frameTimeDeviation = (frameProducers > 0) ? frameDevSum / frameProducers : 0.0f;

        result.cyclesA.push_back(cm);
    }

    int n = (int)result.cyclesA.size();
    for (auto& cm : result.cyclesA) {
        result.totalMigA += cm.migrations;
        result.avgBigUtilA += cm.bigUtil;
        result.avgMidUtilA += cm.midUtil;
        result.avgLittleUtilA += cm.littleUtil;
        result.avgScoreA += cm.avgScore;
        result.avgFrameDevA += cm.frameTimeDeviation;
        result.totalThermalA += cm.thermalPenalties;
        if (cm.roleTotal > 0) {
            result.roleCorrectnessA += (float)cm.roleCorrect / cm.roleTotal;
        }
    }
    if (n > 0) {
        result.avgBigUtilA /= n;
        result.avgMidUtilA /= n;
        result.avgLittleUtilA /= n;
        result.avgScoreA /= n;
        result.avgFrameDevA /= n;
        result.roleCorrectnessA /= n;
        result.roleCorrectnessA *= 100.0f;
    }
    result.mlSamplesA = mlSampleCount;
    result.hybridUpdatesA = hybridUpdateCount;

    return result;
}

static GroupResult runABPair(const std::string& groupName,
                               const std::string& descA, const TestConfig& cfgA,
                               const std::string& descB, const TestConfig& cfgB) {
    printf("\n  [%s] Running A: %s ...\n", groupName.c_str(), descA.c_str());
    GroupResult resA = runConfig(cfgA, "A");

    printf("  [%s] Running B: %s ...\n", groupName.c_str(), descB.c_str());
    GroupResult resB = runConfig(cfgB, "B");

    GroupResult r;
    r.groupName = groupName;
    r.configADesc = descA;
    r.configBDesc = descB;
    r.configA = cfgA;
    r.configB = cfgB;
    r.cyclesA = resA.cyclesA;
    r.cyclesB = resB.cyclesA;

    r.totalMigA = resA.totalMigA;
    r.avgBigUtilA = resA.avgBigUtilA;
    r.avgMidUtilA = resA.avgMidUtilA;
    r.avgLittleUtilA = resA.avgLittleUtilA;
    r.avgScoreA = resA.avgScoreA;
    r.avgFrameDevA = resA.avgFrameDevA;
    r.totalThermalA = resA.totalThermalA;
    r.roleCorrectnessA = resA.roleCorrectnessA;
    r.mlSamplesA = resA.mlSamplesA;
    r.hybridUpdatesA = resA.hybridUpdatesA;

    r.totalMigB = resB.totalMigA;
    r.avgBigUtilB = resB.avgBigUtilA;
    r.avgMidUtilB = resB.avgMidUtilA;
    r.avgLittleUtilB = resB.avgLittleUtilA;
    r.avgScoreB = resB.avgScoreA;
    r.avgFrameDevB = resB.avgFrameDevA;
    r.totalThermalB = resB.totalThermalA;
    r.roleCorrectnessB = resB.roleCorrectnessA;
    r.mlSamplesB = resB.mlSamplesA;
    r.hybridUpdatesB = resB.hybridUpdatesA;

    std::vector<double> migA, migB, scoreA, scoreB, bigA, bigB, frameA, frameB;
    for (auto& cm : r.cyclesA) {
        migA.push_back(cm.migrations);
        scoreA.push_back(cm.avgScore);
        bigA.push_back(cm.bigUtil);
        frameA.push_back(cm.frameTimeDeviation);
    }
    for (auto& cm : r.cyclesB) {
        migB.push_back(cm.migrations);
        scoreB.push_back(cm.avgScore);
        bigB.push_back(cm.bigUtil);
        frameB.push_back(cm.frameTimeDeviation);
    }

    r.pMigrations = welchTTest(migA, migB);
    r.pScore = welchTTest(scoreA, scoreB);
    r.pBigUtil = welchTTest(bigA, bigB);
    r.pFrameDev = welchTTest(frameA, frameB);

    return r;
}

static const char* sigLabel(double p) {
    if (p < 0.001) return "*** (p<0.001)";
    if (p < 0.01)  return "**  (p<0.01)";
    if (p < 0.05)  return "*   (p<0.05)";
    return "ns  (p>=0.05)";
}

static void printGroupReport(const GroupResult& r) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║  %-70s║\n", r.groupName.c_str());
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║  A: %-66s║\n", r.configADesc.c_str());
    printf("║  B: %-66s║\n", r.configBDesc.c_str());
    printf("╠════════════════╦════════════╦════════════╦════════════╦═════════════╣\n");
    printf("║  Metric        ║     A      ║     B      ║  Δ(B-A)    ║ Significance║\n");
    printf("╠════════════════╬════════════╬════════════╬════════════╬═════════════╣\n");

    auto row = [&](const char* metric, float a, float b, double p, const char* fmt) {
        printf("║  %-14s║ ", metric);
        printf(fmt, a);
        printf(" ║ ");
        printf(fmt, b);
        printf(" ║ ");
        printf(fmt, b - a);
        printf(" ║ %-11s ║\n", sigLabel(p));
    };

    row("Migrations",    (float)r.totalMigA, (float)r.totalMigB, r.pMigrations, "%8.0f   ");
    row("Avg Score",     r.avgScoreA,        r.avgScoreB,        r.pScore,      "%8.1f   ");
    row("BIG util%%",    r.avgBigUtilA,      r.avgBigUtilB,      r.pBigUtil,    "%8.1f   ");
    row("MID util%%",    r.avgMidUtilA,      r.avgMidUtilB,      1.0,           "%8.1f   ");
    row("LITTLE util%%", r.avgLittleUtilA,   r.avgLittleUtilB,   1.0,           "%8.1f   ");
    row("Frame dev(ms)", r.avgFrameDevA,     r.avgFrameDevB,     r.pFrameDev,   "%8.2f   ");

    printf("╠════════════════╬════════════╬════════════╬════════════╬═════════════╣\n");
    printf("║  Thermal pen.  ║ %8d   ║ %8d   ║ %8d   ║             ║\n",
           r.totalThermalA, r.totalThermalB, r.totalThermalB - r.totalThermalA);
    printf("║  Role correct%% ║ %8.1f   ║ %8.1f   ║ %8.1f   ║             ║\n",
           r.roleCorrectnessA, r.roleCorrectnessB, r.roleCorrectnessB - r.roleCorrectnessA);
    printf("║  ML samples     ║ %8d   ║ %8d   ║ %8d   ║             ║\n",
           r.mlSamplesA, r.mlSamplesB, r.mlSamplesB - r.mlSamplesA);
    printf("║  Hybrid updates ║ %8d   ║ %8d   ║ %8d   ║             ║\n",
           r.hybridUpdatesA, r.hybridUpdatesB, r.hybridUpdatesB - r.hybridUpdatesA);
    printf("╚════════════════╩════════════╩════════════╩════════════╩═════════════╝\n");

    printf("\n  ▶ Conclusion for %s:\n", r.groupName.c_str());
    bool anySignificant = (r.pMigrations < 0.05 || r.pScore < 0.05 ||
                           r.pBigUtil < 0.05 || r.pFrameDev < 0.05);
    if (!anySignificant) {
        printf("    No statistically significant difference found between A and B.\n");
        printf("    The two configurations perform equivalently on all measured metrics.\n");
    } else {
        if (r.pMigrations < 0.05) {
            const char* winner = (r.totalMigA < r.totalMigB) ? "A" : "B";
            printf("    • Migrations: %s is better (%d vs %d, %s)\n",
                   winner, r.totalMigA, r.totalMigB, sigLabel(r.pMigrations));
        }
        if (r.pScore < 0.05) {
            const char* winner = (r.avgScoreA > r.avgScoreB) ? "A" : "B";
            printf("    • Avg Score: %s is better (%.1f vs %.1f, %s)\n",
                   winner, r.avgScoreA, r.avgScoreB, sigLabel(r.pScore));
        }
        if (r.pBigUtil < 0.05) {
            const char* winner = (r.avgBigUtilA > r.avgBigUtilB) ? "A" : "B";
            printf("    • BIG utilization: %s higher (%.1f%% vs %.1f%%, %s)\n",
                   winner, r.avgBigUtilA, r.avgBigUtilB, sigLabel(r.pBigUtil));
        }
        if (r.pFrameDev < 0.05) {
            const char* winner = (r.avgFrameDevA < r.avgFrameDevB) ? "A" : "B";
            printf("    • Frame time deviation: %s lower (%.2fms vs %.2fms, %s)\n",
                   winner, r.avgFrameDevA, r.avgFrameDevB, sigLabel(r.pFrameDev));
        }
    }
}

int main() {
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║        AATPE - Scheduling Mode A/B Test Framework                ║\n");
    printf("║        Links REAL production: scoring.cpp placement.cpp aatpe_ml     ║\n");
    printf("║        5 groups × 2 configs × 150 cycles × 8 threads                 ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");

    int _rc = system("mkdir -p /tmp/aatpe_sched_ab/decisions /tmp/aatpe_sched_ab/state"); (void)_rc;

    std::vector<GroupResult> allResults;

    {
        TestConfig a;
        a.name = "G1-A assist";
        a.controlMode = "assist";
        a.profile = "balanced";
        a.mlLearningEnabled = true;
        a.mlHybridEnabled = false;
        a.frameAware = 1;
        a.autoProfile = 1;

        TestConfig b;
        b.name = "G1-B full";
        b.controlMode = "full";
        b.profile = "balanced";
        b.mlLearningEnabled = true;
        b.mlHybridEnabled = false;
        b.frameAware = 1;
        b.autoProfile = 1;

        GroupResult r = runABPair("G1: CONTROL_MODE",
                                    "assist (balanced+ML+frame)", a,
                                    "full (balanced+ML+frame)", b);
        allResults.push_back(r);
        printGroupReport(r);
    }

    {
        TestConfig a;
        a.name = "G2-A balanced";
        a.controlMode = "full";
        a.profile = "balanced";
        a.mlLearningEnabled = true;
        a.mlHybridEnabled = false;
        a.frameAware = 1;
        a.autoProfile = 1;

        TestConfig b;
        b.name = "G2-B gaming";
        b.controlMode = "full";
        b.profile = "gaming";
        b.mlLearningEnabled = true;
        b.mlHybridEnabled = false;
        b.frameAware = 1;
        b.autoProfile = 1;

        GroupResult r = runABPair("G2: PROFILE",
                                    "balanced profile (full+ML+frame)", a,
                                    "gaming profile (full+ML+frame)", b);
        allResults.push_back(r);
        printGroupReport(r);
    }

    {
        TestConfig a;
        a.name = "G3-A ML off";
        a.controlMode = "full";
        a.profile = "balanced";
        a.mlLearningEnabled = false;
        a.mlHybridEnabled = false;
        a.frameAware = 1;
        a.autoProfile = 1;

        TestConfig b;
        b.name = "G3-B ML on";
        b.controlMode = "full";
        b.profile = "balanced";
        b.mlLearningEnabled = true;
        b.mlHybridEnabled = false;
        b.frameAware = 1;
        b.autoProfile = 1;

        GroupResult r = runABPair("G3: ML_LEARNING",
                                    "ML disabled (full+balanced+frame)", a,
                                    "ML enabled (full+balanced+frame)", b);
        allResults.push_back(r);
        printGroupReport(r);
    }

    {
        TestConfig a;
        a.name = "G4-A hybrid off";
        a.controlMode = "full";
        a.profile = "balanced";
        a.mlLearningEnabled = true;
        a.mlHybridEnabled = false;
        a.frameAware = 1;
        a.autoProfile = 1;

        TestConfig b;
        b.name = "G4-B hybrid on";
        b.controlMode = "full";
        b.profile = "balanced";
        b.mlLearningEnabled = true;
        b.mlHybridEnabled = true;
        b.frameAware = 1;
        b.autoProfile = 1;

        GroupResult r = runABPair("G4: ML_HYBRID",
                                    "hybrid disabled (full+balanced+ML+frame)", a,
                                    "hybrid enabled (full+balanced+ML+frame)", b);
        allResults.push_back(r);
        printGroupReport(r);
    }

    {
        TestConfig a;
        a.name = "G5-A frame off";
        a.controlMode = "full";
        a.profile = "balanced";
        a.mlLearningEnabled = true;
        a.mlHybridEnabled = false;
        a.frameAware = 0;
        a.autoProfile = 1;

        TestConfig b;
        b.name = "G5-B frame on";
        b.controlMode = "full";
        b.profile = "balanced";
        b.mlLearningEnabled = true;
        b.mlHybridEnabled = false;
        b.frameAware = 1;
        b.autoProfile = 1;

        GroupResult r = runABPair("G5: FRAME_AWARE",
                                    "frame-aware disabled (full+balanced+ML)", a,
                                    "frame-aware enabled (full+balanced+ML)", b);
        allResults.push_back(r);
        printGroupReport(r);
    }

    printf("\n\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     OVERALL A/B TEST SUMMARY                          ║\n");
    printf("╠════════════╦════════════╦════════════╦════════════╦════════════════╣\n");
    printf("║  Group     ║  Mig A/B   ║ Score A/B  ║ BIG%% A/B   ║ FrameDev A/B   ║\n");
    printf("╠════════════╬════════════╬════════════╬════════════╬════════════════╣\n");
    for (auto& r : allResults) {
        printf("║  %-10s║ %4d/%4d  ║ %4.1f/%4.1f ║ %4.1f/%4.1f ║  %4.2f/%4.2fms  ║\n",
               r.groupName.c_str(),
               r.totalMigA, r.totalMigB,
               r.avgScoreA, r.avgScoreB,
               r.avgBigUtilA, r.avgBigUtilB,
               r.avgFrameDevA, r.avgFrameDevB);
    }
    printf("╚════════════╩════════════╩════════════╩════════════╩════════════════╝\n");

    printf("\n  Key findings:\n");
    for (auto& r : allResults) {
        printf("  %s: ", r.groupName.c_str());
        if (r.pMigrations >= 0.05 && r.pScore >= 0.05 && r.pBigUtil >= 0.05 && r.pFrameDev >= 0.05) {
            printf("no significant difference (A≈B)\n");
        } else {
            std::string parts;
            if (r.pMigrations < 0.05) parts += (r.totalMigA < r.totalMigB ? "A fewer mig" : "B fewer mig");
            if (r.pScore < 0.05) { if (!parts.empty()) parts += ", "; parts += (r.avgScoreA > r.avgScoreB ? "A higher score" : "B higher score"); }
            if (r.pBigUtil < 0.05) { if (!parts.empty()) parts += ", "; parts += (r.avgBigUtilA > r.avgBigUtilB ? "A more BIG" : "B more BIG"); }
            if (r.pFrameDev < 0.05) { if (!parts.empty()) parts += ", "; parts += (r.avgFrameDevA < r.avgFrameDevB ? "A less frame dev" : "B less frame dev"); }
            printf("%s\n", parts.c_str());
        }
    }

    printf("\n  Test parameters: %d cycles/config, %d threads, fixed RNG seed 0x12345678\n",
           NUM_CYCLES, (int)createSimulatedThreads().size());
    printf("  Production code linked: scoring.o placement.o aatpe_ml.o aatpe_ml_hybrid.o\n");
    printf("  Statistical test: Welch's two-tailed t-test (df via Welch-Satterthwaite)\n");
    printf("  Significance: * p<0.05, ** p<0.01, *** p<0.001, ns = not significant\n");

    printf("\n  AATPE Scheduling A/B Test COMPLETE.\n");
    return 0;
}

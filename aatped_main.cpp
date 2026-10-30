#include "event_monitor.h"
#include "input_monitor.h"
#include "cold_start_rules.h"
#include "gpu_boost.h"
#include "pre_boost_predict.h"
#include "thermal_headroom.h"
#include "scene_infer_wiring.h"
#include "c4_freezer.h"
#include "aatpe_federated.h"
#include "aatpe_ebpf.h"
#include "aatped.h"
#include "placement.h"
#include "webui_http.h"
#include "integrity.h"
#include "adrc.h"
#include "aatpe_state_ttl.h"
#include "aatpe_helper_audit.h"
#include "aatpe_session_token.h"
#include "aatpe_diag.h"
#include "aatpe_caps.h"
#include "aatpe_placement_mech.h"
#include "aatped_psi.h"
#include "aatped_arbitrator.h"
#include "aatpe_recovery.h"
namespace aatpe { namespace placement { void releaseAllSpilloverLocked(); } }
extern void aatpeGpuBoostForceZero(const char* reason);
namespace aatpe { int aatpeThermalHalLevel(); bool aatpeThermalDegraded();
bool aatpeThermalMayFreezeTraining(bool thermalEnabled); const char* aatpeThermalFreezeReason(); }
#include <mutex>
#include <thread>
#include <chrono>
#include <deque>
#include <map>
#include <unordered_set>
#include <regex>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/wait.h>
#include "aatpe-constants.h"
#include "aatpe_subsystem.h"

using namespace aatpe;

#include "aatpe-version.h"
#include "aatpe_pid_identity.h"
#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"
using namespace aatpe_ml;
using namespace aatpe_ml_hybrid;

const char* RUN_DIR       = "/dev/aatpe";
const char* STATE_DIR     = "/dev/aatpe/state";
const char* METRICS_DIR   = "/dev/aatpe/metrics";
const char* DECISION_DIR  = "/dev/aatpe/decisions";
const char* WEBUI_DIR     = "/dev/aatpe/webui";
const char* LOG_FILE      = "/dev/aatpe/aatpe.log";
const char* PID_FILE      = "/dev/aatpe/daemon.pid";
const char* LOCK_FILE     = "/dev/aatpe/daemon.lock";
const char* CONF_DIR      = "/system/etc/aatpe";
const char* ML_MODEL_PATH = "/data/adb/modules/aatpe/model.dat";

LearningModel g_mlModel;
time_t g_mlLastSave = 0;
std::mutex g_mlMutex;
std::atomic<unsigned long> g_mlSaveSuccess{0};
std::atomic<unsigned long> g_mlSaveFail{0};

aatpe_ml::GfxInfoFrameTimeProvider g_gfxInfoProvider;
aatpe_ml::SurfaceFlingerLatencyProvider g_sfLatencyProvider;

aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot() {
    bool frameTimeOutcome;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        frameTimeOutcome = g_cfg.mlFrameTimeOutcome;
    }
    if (!frameTimeOutcome) {
        aatpe_ml::FrameTimeOutcome empty;
        empty.sample_count = 0;
        empty.valid = false;
        return empty;
    }
    return g_gfxInfoProvider.getSnapshot();
}

static int sfLatencyCallbackWrapper(int refreshRate, int& outMaxLat, bool& outValid) {
    return aatpeDetectSFLatency(refreshRate, outMaxLat, outValid);
}

static std::string foregroundAppCallbackWrapper() {
    std::lock_guard<std::mutex> fgLock(g_foregroundMutex);
    return g_foregroundApp;
}

void initFrameTimeOutcomeCallbacks() {
    aatpe_ml::g_sfLatencyCallback = sfLatencyCallbackWrapper;
    aatpe_ml::g_foregroundAppCallback = foregroundAppCallbackWrapper;
    LOG_INFO("Frame-time outcome: callbacks initialized (ML_FRAME_TIME_OUTCOME=%d, weight=%.2f)",
             g_cfg.mlFrameTimeOutcome ? 1 : 0, (double)g_cfg.mlFrameTimeWeight);
}

std::mutex g_hybridMutex;
static aatpe_ebpf::ThreadCpuRuntimeAggregator g_threadCpuAggregator;
static long long g_hybridLastTickMs = 0;
static std::atomic<uint64_t> g_hybridEbpfSamplesTotal{0};

static std::string g_mlModelPathStr;
static std::once_flag g_mlModelPathOnce;
const char* mlModelPath() {
    std::call_once(g_mlModelPathOnce, []() {
        std::string mod = getModuleDir();
        if (!mod.empty()) {
            g_mlModelPathStr = mod + "/model.dat";
            return;
        }
        g_mlModelPathStr = ML_MODEL_PATH;
    });
    return g_mlModelPathStr.c_str();
}

GpuState g_gpuState;
AudioState g_audioState;
SceneState g_sceneState;

void backupModelFile(const char* path) {
    if (!path || !path[0]) return;
    std::string bak = std::string(path) + ".bak";
    std::string tmpBak = bak + ".tmp.XXXXXX";
    int in = open(path, O_RDONLY);
    if (in < 0) return;
    int out = mkstemp(&tmpBak[0]);
    if (out < 0) {
        LOG_WARN("backupModelFile: mkstemp(%s) failed: %s", tmpBak.c_str(), aatpeErrStr(errno));
        close(in);
        return;
    }
    {
        char buf[4096];
        ssize_t n;
        bool ok = true;
        while (true) {
            n = read(in, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;
            ssize_t totalWritten = 0;
            while (totalWritten < n) {
                ssize_t w = write(out, buf + totalWritten, (size_t)(n - totalWritten));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    ok = false;
                    break;
                }
                totalWritten += w;
            }
            if (!ok) break;
        }
        if (ok) {
            if (fsync(out) != 0) {
                LOG_WARN("backupModelFile: fsync(%s) failed: %s", tmpBak.c_str(), aatpeErrStr(errno));
            }
        }
        close(out);
        if (ok) {
            if (rename(tmpBak.c_str(), bak.c_str()) != 0) {
                LOG_WARN("backupModelFile: rename(%s->%s) failed: %s",
                         tmpBak.c_str(), bak.c_str(), aatpeErrStr(errno));
                unlink(tmpBak.c_str());
            }
        } else {
            unlink(tmpBak.c_str());
        }
    }
    close(in);
}
static bool loadModelWithBackup(const char* path) {
    if (g_mlModel.load(path)) return true;
    std::string bak = std::string(path) + ".bak";
    if (access(bak.c_str(), F_OK) == 0 && g_mlModel.load(bak.c_str())) {
        LOG_WARN("ML model: primary %s load failed, restored from backup %s", path, bak.c_str());
        return true;
    }
    return false;
}

#define LOG_ERROR(...) logMsg(0, __VA_ARGS__)
#define LOG_WARN(...)  logMsg(1, __VA_ARGS__)
#define LOG_INFO(...)  logMsg(2, __VA_ARGS__)

DynamicScoreWeights g_dynamicScoreWeights;
std::mutex g_dynamicWeightsMutex;

float g_fusionTemperature = 1.0f;
float softmaxFuseScores(float modelScore, float heuristicScore, float temperature) {
    if (temperature < 0.0f) temperature = -temperature;
    if (temperature < 0.01f) temperature = 0.01f;
    float m = modelScore / temperature;
    float h = heuristicScore / temperature;
    float maxVal = (m > h) ? m : h;
    m -= maxVal; h -= maxVal;
    float em = expf(m);
    float eh = expf(h);
    float sum = em + eh;
    if (sum < 1e-10f) return 0.5f * (modelScore + heuristicScore);
    return (modelScore * em + heuristicScore * eh) / sum;
}

std::atomic<int> g_federatedRounds{0};
std::atomic<int> g_federatedExports{0};
std::atomic<int> g_federatedImports{0};
int g_federatedTrustedImports = 0;

float g_abGroupAAvg = 0.0f;
float g_abGroupBAvg = 0.0f;
float g_abPValue = 1.0f;

float g_avgPredictionMAE = 0.0f;

std::atomic<int> g_thermalCpu{0};
std::atomic<int> g_thermalSkin{0};
std::atomic<int> g_thermalBattery{0};
int g_runtimeDecisionIntervalMs = 200;
std::string g_thermalCpuZonePath;

std::atomic<const char*> g_gpuType{"unknown"};
std::atomic<float> g_gpuUtilization{0.0f};

std::string g_ebpfLoader = "none";
std::atomic<bool> g_ebpfActive{false};
std::string g_ebpfMode = "polling";
std::mutex g_ebpfInfoMutex;
std::atomic<uint64_t> g_ebpfEventsTotal{0};
std::atomic<uint64_t> g_ebpfEventsFork{0};
std::atomic<uint64_t> g_ebpfEventsExit{0};
std::atomic<uint64_t> g_ebpfEventsSwitch{0};
std::atomic<uint64_t> g_ebpfRingLost{0};
std::atomic<double> g_ebpfEventRate{0.0};

std::atomic<time_t> g_startTime{0};

void lazyCleanupDecisionsDir(void) {
    static long long s_lastCleanupMs = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - s_lastCleanupMs < 10000) return;
    s_lastCleanupMs = nowMs;

    const char* dirPath = "/dev/aatpe/decisions";
    DIR* dir = opendir(dirPath);
    if (!dir) return;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') count++;
    }
    closedir(dir);
    if (count > 500) {
        dir = opendir(dirPath);
        if (!dir) return;
        std::vector<std::string> files;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] != '.') files.push_back(entry->d_name);
        }
        closedir(dir);
        std::sort(files.begin(), files.end());
        int toDelete = files.size() / 2;
        for (int i = 0; i < toDelete; i++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirPath, files[i].c_str());
            unlink(path);
        }
        LOG_INFO("fix95: lazy cleanup deleted %d decision files (was %d)", toDelete, count);
    }
}

void watchdogTakeoverScheduling(void) {
    LOG_INFO("fix95: watchdog takeover - binding critical threads to mid/big cores");
    std::vector<int> targetCpus;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        targetCpus.insert(targetCpus.end(), g_midPool.begin(), g_midPool.end());
        targetCpus.insert(targetCpus.end(), g_bigPool.begin(), g_bigPool.end());
    }
    if (targetCpus.empty()) {
        LOG_WARN("fix95: watchdog takeover skipped - no mid/big cores available");
        return;
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int c : targetCpus) CPU_SET(c, &mask);
    DIR* procDir = opendir("/proc");
    if (!procDir) return;
    struct dirent* entry;
    int bound = 0;
    while ((entry = readdir(procDir)) != NULL) {
        if (!isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
        int pid = parseIntSafe(entry->d_name);
        char taskDirPath[256];
        snprintf(taskDirPath, sizeof(taskDirPath), "/proc/%d/task", pid);
        DIR* taskDir = opendir(taskDirPath);
        if (!taskDir) continue;
        struct dirent* tent;
        while ((tent = readdir(taskDir)) != NULL) {
            if (!isdigit(static_cast<unsigned char>(tent->d_name[0]))) continue;
            int tid = parseIntSafe(tent->d_name);
            char commPath[256];
            snprintf(commPath, sizeof(commPath), "/proc/%d/task/%d/comm", pid, tid);
            FILE* fp = fopen(commPath, "r");
            if (!fp) continue;
            char comm[64] = {0};
            bool matched = false;
            if (fgets(comm, sizeof(comm), fp)) {
                comm[strcspn(comm, "\n")] = 0;
                matched = (strcmp(comm, "UI") == 0 ||
                           strcmp(comm, "RenderThread") == 0 ||
                           strcmp(comm, "main") == 0 ||
                           strcmp(comm, "binder:main") == 0);
            }
            fclose(fp);
            if (matched) {
                if (sched_setaffinity(tid, sizeof(mask), &mask) == 0) {
                    bound++;
                } else {
                    LOG_WARN("fix95: watchdog sched_setaffinity failed for tid=%d comm=%s errno=%d",
                             tid, comm, errno);
                }
            }
        }
        closedir(taskDir);
    }
    closedir(procDir);
    LOG_INFO("fix95: watchdog takeover bound %d threads", bound);
}

int computeAdaptiveInterval(float systemLoad, bool jankDetected, bool hasForeground, int configInterval) {
    (void)hasForeground;
    if (systemLoad < 0.0f) systemLoad = 50.0f;
    if (systemLoad < 30.0f) return 1000;
    if (systemLoad > 70.0f) return 1000;
    if (jankDetected) return std::max(1, std::min(configInterval, 50));
    return 500;
}

std::unordered_map<std::string, int> g_threadRoleCache;
std::mutex g_threadRoleCacheMutex;

int getCachedThreadRole(const std::string& threadName) {
    std::lock_guard<std::mutex> lock(g_threadRoleCacheMutex);
    auto it = g_threadRoleCache.find(threadName);
    if (it != g_threadRoleCache.end()) return it->second;
    return -1;
}

void setCachedThreadRole(const std::string& threadName, int role) {
    std::lock_guard<std::mutex> lock(g_threadRoleCacheMutex);
    g_threadRoleCache[threadName] = role;
}

void invalidateThreadRoleCache(const std::string& threadName) {
    std::lock_guard<std::mutex> lock(g_threadRoleCacheMutex);
    g_threadRoleCache.erase(threadName);
}

std::vector<SchedstatCpuData> g_schedstatData;

bool readSchedstatCpuUtil(void) {
    FILE* fp = fopen("/proc/schedstat", "r");
    if (!fp) return false;
    char line[512];
    g_schedstatData.clear();
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        SchedstatCpuData data;
        char name[16];
        if (sscanf(line, "%15s %llu %llu %llu", name,
                   &data.running, &data.waiting, &data.timeslices) == 4) {
            data.cpuId = parseIntSafe(name + 3);
            g_schedstatData.push_back(data);
        }
    }
    fclose(fp);
    return !g_schedstatData.empty();
}

float computeSchedstatCpuUtil(int cpuId) {
    for (const auto& d : g_schedstatData) {
        if (d.cpuId == cpuId) {
            unsigned long long total = d.running + d.waiting;
            if (total == 0) return 0.0f;
            return (float)d.running / (float)total * 100.0f;
        }
    }
    return 0.0f;
}

RefreshRateMode parseRefreshRateMode(const std::string& s) {
    if (s == "peak" || s == "PEAK") return RefreshRateMode::PEAK;
    if (s == "current" || s == "CURRENT") return RefreshRateMode::CURRENT;
    if (s == "auto" || s == "AUTO") return RefreshRateMode::AUTO;
    return RefreshRateMode::UNKNOWN;
}

ProfileType parseProfileType(const std::string& s) {
    if (s == "balanced" || s == "BALANCED") return ProfileType::BALANCED;
    if (s == "gaming" || s == "GAMING") return ProfileType::GAMING;
    if (s == "battery" || s == "BATTERY") return ProfileType::BATTERY;
    if (s == "thermal" || s == "THERMAL") return ProfileType::THERMAL;
    return ProfileType::UNKNOWN;
}

const char* refreshRateModeToString(RefreshRateMode m) {
    switch (m) {
        case RefreshRateMode::PEAK: return "peak";
        case RefreshRateMode::CURRENT: return "current";
        case RefreshRateMode::AUTO: return "auto";
        default: return "unknown";
    }
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

float g_systemLoad = 50.0f;

double g_abTestLatestP = 1.0;

static aatpe_ebpf::EbpfScheduler g_ebpfScheduler;

static aatpe_federated::FederatedLearner g_federatedLearner;
static long long g_lastFederatedExportMs = 0;
static long long g_lastFederatedImportMs = 0;
Config g_cfg;

std::map<int, CpuMetrics> g_cpuMetrics;
std::mutex g_cpuMetricsMutex;
std::unordered_map<int, ThreadInfo> g_threads;
std::mutex g_threadsMutex;

std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
std::mutex g_scoreCacheMutex;
const long long kScoreTtlMs   = 500;
const long long kInactiveMs   = 10000;
const long long kRebalanceMs  = 1000;
const int kDirtyRebalanceThreshold = 16;

void markThreadScoreDirty(int tid, long long nowMs) {
    std::lock_guard<std::mutex> lock(g_scoreCacheMutex);
    auto it = g_scoreCache.find(tid);
    if (it != g_scoreCache.end()) {
        it->second.dirty = true;
        it->second.lastSwitchMs = nowMs;
        it->second.wakeHint = true;
    }
}

void forceRescoreAllScores() {
    std::lock_guard<std::mutex> lock(g_scoreCacheMutex);
    for (auto& kv : g_scoreCache) kv.second.forceRescore = true;
}

void invalidateAllScores() {
    std::lock_guard<std::mutex> lock(g_scoreCacheMutex);
    for (auto& kv : g_scoreCache) kv.second.dirty = true;
}
std::atomic<sig_atomic_t> g_shouldExit{0};
std::atomic<sig_atomic_t> g_reloadProfile{0};
std::map<int, HistoryEntry> g_history;
std::mutex g_historyMutex;
std::vector<ThreadRule> g_rules;
std::mutex g_cfgMutex;

Config g_preEmergencyCfg;
std::string g_preEmergencyProfile = "balanced";
bool g_preEmergencyAutoProfile = 1;

std::string g_currentProfile = "balanced";
RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;

std::atomic<bool> g_reloadConfigRequested{false};

static std::mutex g_schedstatMutex;
static std::unordered_map<int, std::pair<long long, long long>> g_schedstatLastSample;
static std::unordered_map<int, float> g_schedstatLastUtil;
float readCpuUtilizationSchedstat(int cpuId) {
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpuId) + "/schedstat";
    std::ifstream f(path);
    if (!f) {
        std::lock_guard<std::mutex> lk(g_schedstatMutex);
        auto it = g_schedstatLastUtil.find(cpuId);
        if (it != g_schedstatLastUtil.end()) return it->second;
        return 50.0f;
    }
    long long runTime = 0, waitTime = 0, slices = 0;
    f >> runTime >> waitTime >> slices;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_schedstatMutex);
    auto& last = g_schedstatLastSample[cpuId];
    if (last.first == 0 && last.second == 0) {
        last = {runTime, nowMs};
        g_schedstatLastUtil[cpuId] = 0.0f;
        return 0.0f;
    }
    long long deltaRun = runTime - last.first;
    long long deltaMs = nowMs - last.second;
    last = {runTime, nowMs};
    if (deltaMs <= 0 || deltaRun < 0) return g_schedstatLastUtil[cpuId];
    float util = (float)deltaRun / (float)(deltaMs * 1000000LL) * 100.0f;
    util = std::min(100.0f, util);
    g_schedstatLastUtil[cpuId] = util;
    return util;
}

ProfileType g_currentProfileEnum = ProfileType::BALANCED;
std::atomic<bool> g_thermalEmergency{false};
std::atomic<int> g_thermalMigrationPressure{0};
std::atomic<int> g_thermalHeadroomPct{-1};
std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> preboostEnabled{false};
std::atomic<bool> thermalSmoothEnabled{false};
std::atomic<bool> sceneInferEnabled{false};
std::atomic<bool> c2EnvelopeEnabled{false};
std::atomic<bool> g_frameCritical{false};
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<bool> g_profileLock{false};
std::atomic<bool> g_skipNextSleep{false};
std::atomic<int> g_sseClientCount{0};
std::chrono::steady_clock::time_point g_lastDecisionSteady{};
int g_wakeupPipeReadFd = -1;
int g_wakeupPipeWriteFd = -1;
int g_controlInotifyFd = -1;
int g_controlInotifyWd = -1;
static inline void wakeMainLoopNow() {
    if (g_wakeupPipeWriteFd >= 0) {
        char b = 1;
        ssize_t r = write(g_wakeupPipeWriteFd, &b, 1);
        (void)r;
    }
}
int g_cpuCount = 0;
std::atomic<uint64_t> g_topologyGeneration{0};
std::vector<int> g_allCpus;
std::vector<int> g_littlePool;
std::vector<int> g_midPool;
std::vector<int> g_bigPool;

std::atomic<long long> g_totalDecisions{0};
std::atomic<long long> g_totalMigrations{0};
std::atomic<long long> g_highLoadCycles{0};
int g_migrationCount = 0;
time_t g_migrationWindowStart = 0;
std::atomic<long long> g_keepProtected{0};
std::atomic<long long> g_keepPassive{0};
std::atomic<long long> g_keepThermalEmergency{0};
std::atomic<long long> g_keepSafetyLatency{0};
std::atomic<long long> g_keepNoCpu{0};
std::atomic<long long> g_keepLowConf{0};
std::atomic<long long> g_keepHighCost{0};
std::atomic<long long> g_keepSafeMode{0};
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
std::atomic<long long> g_applySafetyLatency{0};
std::atomic<long long> g_applyCooldownSkip{0};
std::atomic<long long> g_applyCpusetSkip{0};
CpusetSkipSample g_lastCpusetSkip;
std::mutex g_lastCpusetSkipMutex;
std::atomic<long long> g_applyKernelSkip{0};
std::atomic<long long> g_applyUnmigratableSkip{0};
std::atomic<long long> g_applyIntervalSkip{0};
std::atomic<long long> g_mlPendingCreated{0};
std::atomic<long long> g_mlOutcomeMeasured{0};
std::atomic<long long> g_mlOutcomeValid{0};
std::atomic<long long> g_mlShadowOutcomeValid{0};
std::atomic<long long> g_mlOutcomeSkipSleeping{0};
std::atomic<long long> g_mlOutcomeSkipSaturated{0};
std::atomic<long long> g_mlOutcomeSkipReset{0};
std::atomic<long long> g_mlOutcomeSkipOther{0};
std::atomic<long long> g_mlPendingExpired{0};
std::atomic<long long> g_mlPendingTidReused{0};
std::atomic<long long> g_mlPendingThreadExited{0};
std::atomic<bool> g_helperApkInstalled{false};
std::string g_autoProfileMode = "module_controlled";
std::atomic<bool> g_helperAutoSuppressed{false};
std::mutex g_unmigratableMutex;
std::set<int> g_unmigratableThreads;
std::mutex g_lastMigMutex;
std::unordered_map<int, time_t> g_lastMigrationTime;

std::mutex g_migFailMutex;
std::unordered_map<int, time_t> g_migFailCooldown;
std::unordered_map<int, int> g_migFailCount;
std::unordered_map<int, int> g_migFailCpu;
std::unordered_map<int, long long> g_migFailBackoffUntilMs;
std::atomic<long long> g_cooldownBypassCount{0};
time_t g_cooldownFloorWindowStart = 0;

std::map<int, std::pair<unsigned long long, unsigned long long>> g_prevCpuStat;

static unsigned long long g_aggProcStatBusy = 0;
static unsigned long long g_aggProcStatTotal = 0;
static bool g_aggProcStatValid = false;

std::atomic<int> g_selfCpuUsagePercent{0};
std::atomic<bool> g_lowOverheadMode{false};
static unsigned long long g_prevSelfJiffies = 0;
static unsigned long long g_prevSelfTotalJiff = 0;
static bool g_prevSelfValid = false;

void readCpuMetrics() {
    static unsigned long long s_lastProcStatMs = 0;
    static unsigned long long s_lastFreqReadMs = 0;
    static std::map<int, int> s_lastCpuUtil;
    unsigned long long nowMetricsMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool doProcStat  = (nowMetricsMs - s_lastProcStatMs >= 2000);
    bool doFreqRead  = (nowMetricsMs - s_lastFreqReadMs >= 5000);
    if (doProcStat) s_lastProcStatMs = nowMetricsMs;
    if (doFreqRead) s_lastFreqReadMs = nowMetricsMs;

    std::map<int, int> cpuUtil;
    if (doProcStat) {
        std::ifstream statf("/proc/stat");
        if (statf) {
            std::string line;
            while (std::getline(statf, line)) {
                if (line.substr(0, 3) != "cpu") continue;
                size_t sp = line.find(' ');
                std::string cpuStr = line.substr(3, sp - 3);
                std::stringstream ss(line.substr(sp));
                unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
                ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
                unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;
                unsigned long long idleTotal = idle + iowait;
                if (cpuStr.empty()) {
                    g_aggProcStatBusy  = user + nice + system + irq + softirq;
                    g_aggProcStatTotal = g_aggProcStatBusy + idle + iowait + steal;
                    g_aggProcStatValid = true;

                    {
                        unsigned long long selfUtime = 0, selfStime = 0;
                        std::ifstream selfStat("/proc/self/stat");
                        if (selfStat) {
                            std::string selfLine;
                            std::getline(selfStat, selfLine);
                            size_t lastParen = selfLine.rfind(')');
                            if (lastParen != std::string::npos && lastParen + 2 < selfLine.size()) {
                                std::string after = selfLine.substr(lastParen + 2);
                                std::stringstream ss2(after);
                                unsigned long long skip;
                                for (int i = 0; i < 11; i++) ss2 >> skip;
                                ss2 >> selfUtime >> selfStime;
                            }
                        }
                        unsigned long long selfTotal = selfUtime + selfStime;
                        static std::chrono::steady_clock::time_point sSelfWallPrev;
                        if (g_prevSelfValid) {
                            auto nowWall = std::chrono::steady_clock::now();
                            double wallSec = std::chrono::duration<double>(
                                nowWall - sSelfWallPrev).count();
                            long ticksPerSec = sysconf(_SC_CLK_TCK);
                            unsigned long long dSelf = (selfTotal > g_prevSelfJiffies)
                                ? (selfTotal - g_prevSelfJiffies) : 0;
                            if (wallSec > 0.05 && ticksPerSec > 0) {
                                double pct = (double)dSelf / (double)ticksPerSec / wallSec * 100.0;
                                int selfPct = (int)pct;
                                if (selfPct < 0) selfPct = 0;
                                g_selfCpuUsagePercent.store(selfPct, std::memory_order_relaxed);
                            }
                        }
                        sSelfWallPrev = std::chrono::steady_clock::now();
                        g_prevSelfJiffies = selfTotal;
                        g_prevSelfTotalJiff = g_aggProcStatTotal;
                        g_prevSelfValid = true;
                    }
                    continue;
                }
                int cpuId = -1;
                try { cpuId = std::stoi(cpuStr); } catch (...) { continue; }
                auto prev = g_prevCpuStat.find(cpuId);
                if (prev != g_prevCpuStat.end()) {
                    if (total < prev->second.second || idleTotal < prev->second.first) {
                        LOG_WARN("readCpuMetrics: /proc/stat counter wrap detected for cpu%d (total %llu -> %llu), skipping sample",
                                 cpuId, prev->second.second, total);
                    } else {
                        unsigned long long dTotal = total - prev->second.second;
                        unsigned long long dIdle = idleTotal - prev->second.first;
                        if (dTotal > 0 && dIdle <= dTotal) {
                            cpuUtil[cpuId] = (int)((dTotal - dIdle) * 100 / dTotal);
                        }
                    }
                }
                g_prevCpuStat[cpuId] = {idleTotal, total};
            }
        }
        s_lastCpuUtil = cpuUtil;
    } else {
        g_aggProcStatValid = false;
        cpuUtil = s_lastCpuUtil;
    }

    int g_cachedCpuTemp = g_thermalCpu.load(std::memory_order_relaxed);

    static bool s_cpuStateDirsReady = false;
    static unsigned long long s_lastMetricsWriteMs = 0;
    bool doMetricsWrite = (!s_cpuStateDirsReady) || (nowMetricsMs - s_lastMetricsWriteMs >= 1000);
    if (doMetricsWrite) s_lastMetricsWriteMs = nowMetricsMs;

    std::vector<int> cpusSnapshot;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        cpusSnapshot = g_allCpus;
    }

    std::unique_lock<std::mutex> wlock(g_cpuMetricsMutex);
    char baseBuf[128];
    for (int cpuId : cpusSnapshot) {
        CpuMetrics m;
        snprintf(baseBuf, sizeof(baseBuf), "/sys/devices/system/cpu/cpu%d", cpuId);
        std::string base(baseBuf);
        auto prevIt = g_cpuMetrics.find(cpuId);
        if (doFreqRead) {
            m.curFreq = readInt((base + "/cpufreq/scaling_cur_freq").c_str(), 0);
            m.maxFreq = readInt((base + "/cpufreq/scaling_max_freq").c_str(), 1);
            if (m.curFreq == 0) {
                std::string onlinePath = base + "/online";
                int onlineVal = readInt(onlinePath.c_str(), 1);
                if (onlineVal != 0) {
                    LOG_WARN("readCpuMetrics: CPU%d cur_freq read failed (returned 0, core online=%d)", cpuId, onlineVal);
                }
            }
            m.capacity = readInt((base + "/cpu_capacity").c_str(), topology::kDefaultCapacity);
        } else {
            if (prevIt != g_cpuMetrics.end()) {
                m.curFreq   = prevIt->second.curFreq;
                m.maxFreq   = prevIt->second.maxFreq;
                m.capacity  = prevIt->second.capacity;
            } else {
                m.curFreq = 0;
                m.maxFreq = 1;
                m.capacity = topology::kDefaultCapacity;
            }
        }
        if (m.maxFreq == 0) m.maxFreq = 1;
        m.freqPct = m.curFreq * 100 / m.maxFreq;
        auto uit = cpuUtil.find(cpuId);
        m.util = (uit != cpuUtil.end()) ? uit->second : m.freqPct;
        m.temp = g_cachedCpuTemp;
        g_cpuMetrics[cpuId] = m;
        recordCpuLoadSample(cpuId, m.util);
        {
            float predicted = predictCpuLoad(cpuId);
            recordPredictionError(cpuId, predicted, (float)m.util);
        }
        if (doMetricsWrite) {
            std::string cpuStateDir = std::string(STATE_DIR) + "/cpu" + std::to_string(cpuId);
            if (!s_cpuStateDirsReady) mkdirIfNotExist(cpuStateDir.c_str(), 0700);
            writeFile((cpuStateDir + "/cur_freq").c_str(), m.curFreq);
            writeFile((cpuStateDir + "/freq_pct").c_str(), m.freqPct);
            writeFile((cpuStateDir + "/util").c_str(), m.util);
            writeFile((cpuStateDir + "/temp").c_str(), m.temp);
        }
    }
    s_cpuStateDirsReady = true;
}

bool isKernelThread(const std::string& tname, int tid);

static unsigned long long g_prevCpuBusy = 0;
static unsigned long long g_prevCpuTotal = 0;
static bool g_prevCpuValid = false;
static std::atomic<int> g_lastCpuUsagePercent{0};

static bool readProcStatCpu(unsigned long long& busyOut, unsigned long long& totalOut) {
    if (!g_aggProcStatValid) return false;
    busyOut  = g_aggProcStatBusy;
    totalOut = g_aggProcStatTotal;
    return true;
}

int getCpuUsagePercent() {
    unsigned long long busy = 0, total = 0;
    if (!readProcStatCpu(busy, total)) {
        return g_lastCpuUsagePercent.load(std::memory_order_relaxed);
    }
    if (!g_prevCpuValid) {
        g_prevCpuBusy = busy;
        g_prevCpuTotal = total;
        g_prevCpuValid = true;
        g_lastCpuUsagePercent.store(0, std::memory_order_relaxed);
        return 0;
    }
    unsigned long long deltaBusy = (busy > g_prevCpuBusy) ? (busy - g_prevCpuBusy) : 0;
    unsigned long long deltaTotal = (total > g_prevCpuTotal) ? (total - g_prevCpuTotal) : 0;
    g_prevCpuBusy = busy;
    g_prevCpuTotal = total;
    if (deltaTotal == 0) {
        return g_lastCpuUsagePercent.load(std::memory_order_relaxed);
    }
    int pct = (int)((deltaBusy * 100ULL) / deltaTotal);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_lastCpuUsagePercent.store(pct, std::memory_order_relaxed);
    return pct;
}

bool checkSystemLoad() {
    Config cfg; snapshotConfig(cfg);

    int cpuUsage = getCpuUsagePercent();

    double load1 = 0.0;
    int n = getloadavg(&load1, 1);
    if (n < 0) {
        FILE* f = fopen("/proc/loadavg", "r");
        if (f) {
            float fload;
            if (fscanf(f, "%f", &fload) == 1) load1 = fload;
            fclose(f);
        }
    }
    long onlineCpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (onlineCpus <= 0) onlineCpus = (g_cpuCount > 0) ? g_cpuCount : 1;
    double normalizedLoad = load1 / (double)onlineCpus;
    int loadInt = (int)(normalizedLoad * 100);

    {
        static time_t s_lastLoadStateWrite = 0;
        time_t nowLsw = time(nullptr);
        if (nowLsw - s_lastLoadStateWrite >= 1) {
            writeFile((std::string(STATE_DIR) + "/system_load").c_str(), cpuUsage);
            writeFile((std::string(STATE_DIR) + "/cpu_usage").c_str(), cpuUsage);
            writeFile((std::string(STATE_DIR) + "/self_cpu_usage").c_str(),
                      g_selfCpuUsagePercent.load(std::memory_order_relaxed));
            writeFile((std::string(STATE_DIR) + "/low_overhead_mode").c_str(),
                      g_lowOverheadMode.load(std::memory_order_relaxed) ? 1 : 0);
            char loadBuf[64];
            snprintf(loadBuf, sizeof(loadBuf), "%.2f", load1);
            writeFile((std::string(STATE_DIR) + "/loadavg_raw").c_str(), loadBuf);
            {
                struct statvfs vfs;
                if (statvfs(RUN_DIR, &vfs) == 0) {
                    unsigned long usedKb = (vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize / 1024;
                    writeFile((std::string(STATE_DIR) + "/tmpfs_usage_kb").c_str(), (int)usedKb);
                }
            }
            s_lastLoadStateWrite = nowLsw;
        }
    }

    bool cpuHigh = (cpuUsage >= cfg.cpuUsageLimit);
    bool loadExtreme = (loadInt >= cfg.safetyLoadLimit);
    bool backstop = loadExtreme && cpuUsage >= 80;
    bool userLockedNormal = g_userModeLock.load(std::memory_order_relaxed);

    static std::chrono::steady_clock::time_point s_highLoadStreakStart;
    auto nowSteady = std::chrono::steady_clock::now();
    bool anyHigh = cpuHigh || backstop;
    if (anyHigh) {
        if (s_highLoadStreakStart.time_since_epoch().count() == 0) {
            s_highLoadStreakStart = nowSteady;
        }
    } else {
        s_highLoadStreakStart = std::chrono::steady_clock::time_point{};
    }
    long long dwellMs = anyHigh
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              nowSteady - s_highLoadStreakStart).count()
        : 0;
    constexpr long long kHighLoadDwellMs = 10000;
    bool jankAbnormal = (readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0) != 0);
    bool allowDegrade = watchdog::highLoadShouldDegrade(jankAbnormal, dwellMs, kHighLoadDwellMs);

    if (cpuHigh) {
        static time_t s_lastHighCpuLog = 0;
        time_t nowHcl = time(nullptr);
        if (nowHcl - s_lastHighCpuLog >= 5) {
            LOG_WARN("System load high: cpu_usage=%d%% (threshold=%d), loadavg_raw=%.2f normalized=%d%% (cores=%ld) -> %s",
                     cpuUsage, cfg.cpuUsageLimit, load1, loadInt, onlineCpus,
                     userLockedNormal ? "DEGRADED suppressed by user NORMAL lock"
                      : (allowDegrade ? "degraded mode"
                        : (jankAbnormal ? "degraded (jank)" : "dwell-gating")));
            s_lastHighCpuLog = nowHcl;
        }
        if (!userLockedNormal && allowDegrade) {
            if (getSystemState() != SystemState::EMERGENCY) {
                transitionToState(SystemState::DEGRADED, "high_cpu_usage");
            }
            g_degradedNormalCycles.store(0, std::memory_order_relaxed);
        }
        return false;
    }
    if (backstop) {
        static time_t s_lastHighLoadBackstopLog = 0;
        time_t nowHbl = time(nullptr);
        if (nowHbl - s_lastHighLoadBackstopLog >= 5) {
            LOG_WARN("System load high (backstop): cpu_usage=%d%%, loadavg_raw=%.2f normalized=%d%% (threshold=%d, cores=%ld) -> %s",
                     cpuUsage, load1, loadInt, cfg.safetyLoadLimit, onlineCpus,
                     userLockedNormal ? "DEGRADED suppressed by user NORMAL lock"
                      : (allowDegrade ? "degraded" : "dwell-gating"));
            s_lastHighLoadBackstopLog = nowHbl;
        }
        if (!userLockedNormal && allowDegrade) {
            if (getSystemState() != SystemState::EMERGENCY) {
                transitionToState(SystemState::DEGRADED, "high_load_backstop");
            }
            g_degradedNormalCycles.store(0, std::memory_order_relaxed);
        }
        return false;
    }

    SystemState curState = getSystemState();
    if (curState == SystemState::DEGRADED) {
        int cycles = g_degradedNormalCycles.fetch_add(1, std::memory_order_relaxed) + 1;
        long degradedSecs = getStateDurationSec();
        constexpr long kDegradedRecoveryDwellSec = 15;
        if (cycles >= kDegradedRecoveryCycles && degradedSecs >= kDegradedRecoveryDwellSec) {
            transitionToState(SystemState::NORMAL, "load_normal_3_cycles");
        }
    } else {
        g_degradedNormalCycles.store(0, std::memory_order_relaxed);
    }
    return true;
}

static int resolveUidForPid(int pid) {
    if (pid <= 0) return -1;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE* f = fopen(path, "re");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            int euid = -1;
            if (sscanf(line, "Uid:\t%d %d", &uid, &euid) >= 1) break;
        }
    }
    fclose(f);
    return uid;
}

static std::atomic<bool> g_httpServerRunning{false};
static void writeHttpChannelState(const char* state) {
    atomicWriteFile((std::string(STATE_DIR) + "/http_state").c_str(), std::string(state));
}

void applyHttpServerState(bool wantStart) {
    if (wantStart) {
        if (g_httpServerRunning) { writeHttpChannelState("on"); return; }
        if (aatpe::webui_http::startHttpServer()) {
            g_httpServerRunning = true;
            LOG_INFO("v66-fix: HTTP server started on port %d", aatpe::webui_http::getHttpPort());
            atomicWriteFile((std::string(STATE_DIR) + "/http_port").c_str(), aatpe::webui_http::getHttpPort());
            writeHttpChannelState("on");
        } else {
            const char* reason = "failed_token";
            int probe = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (probe >= 0) {
                struct sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                sa.sin_port = htons(8888);
                if (bind(probe, (struct sockaddr*)&sa, sizeof(sa)) != 0 && errno == EADDRINUSE) {
                    reason = "ports_busy";
                }
                close(probe);
            }
            LOG_WARN("v66-fix: HTTP server failed to start (%s)", reason);
            atomicWriteFile((std::string(STATE_DIR) + "/http_port").c_str(), 0);
            writeHttpChannelState(reason);
        }
    } else {
        if (!g_httpServerRunning) { writeHttpChannelState("disabled_by_config"); return; }
        aatpe::webui_http::stopHttpServer();
        g_httpServerRunning = false;
        LOG_INFO("v66-fix: HTTP server stopped (ENABLE_HTTP_SERVER=0)");
        atomicWriteFile((std::string(STATE_DIR) + "/http_port").c_str(), 0);
        writeHttpChannelState("disabled_by_config");
    }
}

static std::mutex g_startupCrashMtx;
static void recordStartupCrash(const std::string& stage, const char* what) {
    std::string reason = stage + ": " + (what ? what : "unknown");
    LOG_ERROR("startup stage '%s' threw, degrading this stage and continuing: %s",
              stage.c_str(), reason.c_str());
    std::lock_guard<std::mutex> lk(g_startupCrashMtx);
    atomicWriteFile((std::string(STATE_DIR) + "/last_crash_reason").c_str(), reason.c_str());
    std::string cnt = readFile((std::string(STATE_DIR) + "/startup_crash_count").c_str());
    int n = 0; try { n = std::stoi(cnt); } catch (...) { n = 0; }
    atomicWriteFile((std::string(STATE_DIR) + "/startup_crash_count").c_str(), n + 1);
}

static inline int mlPoolCount() {
    int maxTier = 0;
    bool any = false;
    for (const auto& cl : clustersSnapshot()) {
        int t = (int)cl.tier;
        if (t < 0) continue;
        if (!any || t > maxTier) maxTier = t;
        any = true;
    }
    int n = any ? (maxTier + 1) : 3;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

static inline int mlDistinctClusterCount() {
    int n = (int)clustersSnapshot().size();
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

void daemonInit() {
    LOG_FORCE("daemonInit: entering initialization sequence");
    setsid();
    {
        int oomFd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
        if (oomFd >= 0) {
            write(oomFd, "-1000", 5);
            close(oomFd);
        } else {
            LOG_WARN("daemonInit: cannot write oom_score_adj (errno=%d)", errno);
        }
    }
    try {
        aatpe::caps::probe();
    } catch (const std::exception& e) { recordStartupCrash("caps_probe", e.what()); }
    catch (...) { recordStartupCrash("caps_probe", "?"); }
    g_threads.reserve(1024);
    mkdirIfNotExist(RUN_DIR, 0700);
    mkdirIfNotExist(STATE_DIR, 0700);
    mkdirIfNotExist(METRICS_DIR, 0700);
    mkdirIfNotExist(DECISION_DIR, 0700);
    mkdirIfNotExist(WEBUI_DIR, 0755);
    mkdirIfNotExist((std::string(RUN_DIR) + "/history").c_str(), 0700);
    scoringHistorySweepStale();
    mkdirIfNotExist((std::string(RUN_DIR) + "/thread_db").c_str(), 0700);
    try { aatpe::caps::writeStateFiles(STATE_DIR); }
    catch (const std::exception& e) { recordStartupCrash("caps_write", e.what()); }
    catch (...) { recordStartupCrash("caps_write", "?"); }
    helperDirAudit();

    {
        using namespace aatpe::subsys;
        auto& reg = Registry::instance();
        reg.setState("ebpf", SubsystemState::STARTING);
        reg.setState("webui", SubsystemState::STARTING);
        reg.setState("ml", SubsystemState::STARTING);
        reg.setState("frame", SubsystemState::STARTING);
        reg.setState("thermal", SubsystemState::STARTING);
        reg.setState("scheduler", SubsystemState::STARTING);
    }
    {
        int pipeFds[2];
        if (pipe(pipeFds) == 0) {
            g_wakeupPipeReadFd = pipeFds[0];
            g_wakeupPipeWriteFd = pipeFds[1];
            int fl = fcntl(g_wakeupPipeReadFd, F_GETFL, 0);
            if (fl >= 0) fcntl(g_wakeupPipeReadFd, F_SETFL, fl | O_NONBLOCK);
            fl = fcntl(g_wakeupPipeWriteFd, F_GETFL, 0);
            if (fl >= 0) fcntl(g_wakeupPipeWriteFd, F_SETFL, fl | O_NONBLOCK);
            fcntl(g_wakeupPipeReadFd, F_SETFD, FD_CLOEXEC);
            fcntl(g_wakeupPipeWriteFd, F_SETFD, FD_CLOEXEC);
            LOG_FORCE("[OPT1] wakeup self-pipe created (read=%d write=%d)",
                      g_wakeupPipeReadFd, g_wakeupPipeWriteFd);
        } else {
            LOG_WARN("[OPT1] wakeup self-pipe FAILED: %s (event-driven fallback to timed poll)",
                     aatpeErrStr(errno));
        }
        g_controlInotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_controlInotifyFd >= 0) {
            std::string ctrlPath = std::string(RUN_DIR) + "/control";
            int cf = open(ctrlPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (cf >= 0) close(cf);
            g_controlInotifyWd = inotify_add_watch(g_controlInotifyFd, RUN_DIR,
                                                  IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);
            if (g_controlInotifyWd < 0) {
                LOG_WARN("[OPT1] control inotify_add_watch on dir failed: %s (control latency up to 1s)",
                         aatpeErrStr(errno));
            } else {
                LOG_FORCE("[OPT1] control inotify watching dir %s (wd=%d, filtering by 'control')",
                          RUN_DIR, g_controlInotifyWd);
            }
        } else {
            LOG_WARN("[OPT1] control inotify_init failed: %s (control latency up to 1s)",
                     aatpeErrStr(errno));
        }
    }
    writeFile((std::string(RUN_DIR) + "/version").c_str(), AATPE_VERSION);
    aatpe::pid_identity::setStateDir(STATE_DIR);
    {
        long long selfSt = aatpe::pid_identity::readPidStarttime(getpid());
        if (selfSt >= 0) {
            atomicWriteFile((std::string(STATE_DIR) + "/daemon_starttime").c_str(), selfSt);
        }
    }
    {
        char selfExe[PATH_MAX] = {0};
        ssize_t n = readlink("/proc/self/exe", selfExe, sizeof(selfExe) - 1);
        if (n > 0) {
            atomicWriteFile((std::string(STATE_DIR) + "/daemon_exe").c_str(),
                            std::string(selfExe, n));
        }
    }
    writeFile(PID_FILE, getpid());
    atomicWriteFile((std::string(STATE_DIR) + "/daemon_pid").c_str(), getpid());
    g_startTime.store(time(nullptr));
    atomicWriteFile((std::string(STATE_DIR) + "/daemon_start").c_str(), (long long)g_startTime.load());
    writeFile((std::string(STATE_DIR) + "/daemon_uptime").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/decision_count").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/migration_count").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/decision_count_current").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/migration_rate_current").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/loop_count").c_str(), 0);
    atomicWriteFile((std::string(STATE_DIR) + "/total_decisions").c_str(), (long long)0);
    atomicWriteFile((std::string(STATE_DIR) + "/total_migrations").c_str(), (long long)0);
    atomicWriteFile((std::string(STATE_DIR) + "/prediction_proactive_migrations").c_str(), (long long)0);
    try {
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles(STATE_DIR);
    } catch (const std::exception& e) { recordStartupCrash("diag_collect", e.what()); }
    catch (...) { recordStartupCrash("diag_collect", "?"); }
    atomicWriteFile((std::string(STATE_DIR) + "/version").c_str(),
                    std::string(AATPE_VERSION) + " (" + std::to_string(AATPE_VERSION_CODE) + ")");
    writeFile((std::string(STATE_DIR) + "/keep_protected").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/keep_passive").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/keep_no_cpu").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/keep_low_conf").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/keep_high_cost").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/keep_already_optimal").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/migrate_actual").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_success").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_failed").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_rate_limited").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_passive").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_cooldown_skip").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_cpuset_skip").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/apply_interval_skip").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/helper_apk_installed").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/auto_profile_mode").c_str(), "module_controlled");
    writeFile((std::string(STATE_DIR) + "/foreground_app_source").c_str(), "module");
    try {
        loadConfig();
        aatpe_recovery::recoveryBootload();
        aatpe_recovery::recoveryRestoreSchedOnBoot();
    } catch (const std::exception& e) { recordStartupCrash("config_recovery", e.what()); }
    catch (...) { recordStartupCrash("config_recovery", "?"); }
    initHmacKey();
    aatpe::integrity::setIntegrityFailHook([](const char* reason) {
        hmacForceReadonly(reason);
    });
    aatpe::integrity::setIntegrityRestoreHook([](const char* reason) {
        hmacClearReadonly(reason);
    });
    try {
        (void)aatpe::integrity::runBootGate();
        aatpe::webui_http::ensureDaemonToken();
    } catch (const std::exception& e) { recordStartupCrash("integrity_token", e.what()); }
    catch (...) { recordStartupCrash("integrity_token", "?"); }
    setStateEnterTimeNow();
    g_systemState.store((int)SystemState::NORMAL, std::memory_order_release);
    strncpy(g_lastTransitionReason, "hardcore_default_normal", sizeof(g_lastTransitionReason) - 1);
    g_lastTransitionReason[sizeof(g_lastTransitionReason) - 1] = '\0';
    atomicWriteFile((std::string(STATE_DIR) + "/system_state").c_str(),
              systemStateToString(getSystemState()));
    atomicWriteFile((std::string(STATE_DIR) + "/last_transition_reason").c_str(),
              std::string(getLastTransitionReason()));
    atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(), g_cfg.controlMode);
    writeFile((std::string(STATE_DIR) + "/log_level").c_str(), g_logLevel.load());
    atomicWriteFile((std::string(STATE_DIR) + "/current_profile").c_str(), aatpe_profile::effectiveProfile());
    atomicWriteFile((std::string(STATE_DIR) + "/profile_lock").c_str(), g_profileLock ? 1 : 0);
    writeFile((std::string(STATE_DIR) + "/battery_aging_enabled").c_str(), g_cfg.batteryAgingProtection ? "1" : "0");
    atomicWriteFile((std::string(STATE_DIR) + "/battery_aged").c_str(), g_batteryAged ? "1" : "0");
    writeFile((std::string(STATE_DIR) + "/ml_cold_start_enabled").c_str(), g_cfg.mlColdStartProtection ? "1" : "0");
    writeFile((std::string(STATE_DIR) + "/ml_learning_enabled").c_str(), g_cfg.mlLearningEnabled ? "1" : "0");
    writeFile((std::string(STATE_DIR) + "/thermal_limit_cpu").c_str(), g_cfg.thermalLimitCpu);
    writeFile((std::string(STATE_DIR) + "/thermal_limit_skin").c_str(), g_cfg.thermalLimitSkin);
    writeFile((std::string(STATE_DIR) + "/thermal_limit_battery").c_str(), g_cfg.thermalLimitBattery);
    writeFile((std::string(STATE_DIR) + "/thermal_enabled").c_str(), g_cfg.thermalEnabled ? "1" : "0");
    {
        SystemState ss = getSystemState();
        std::string eff = (ss == SystemState::EMERGENCY) ? std::string("emergency") : g_cfg.controlMode;
        atomicWriteFile((std::string(STATE_DIR) + "/control_mode_effective").c_str(), eff);
    }
    {
        std::lock_guard<std::mutex> mlLock(g_mlMutex);
        g_mlModel.setMinSamples(g_cfg.mlMinSamples);
        g_mlModel.setEnabled(g_cfg.mlLearningEnabled != 0);
    }
    applyHelperApkDetection();
    {
        bool cgroupV1 = access("/sys/fs/cgroup/cpuset", F_OK) == 0 || access("/dev/cpuset", F_OK) == 0;
        bool cgroupV2 = access("/sys/fs/cgroup/cpuset.cpus.effective", F_OK) == 0;
        LOG_INFO("Capability: cgroup_v1=%d cgroup_v2=%d", cgroupV1, cgroupV2);
    }
    try {
        detectCpuTopology();
    } catch (const std::exception& e) { recordStartupCrash("cpu_topology", e.what()); }
    catch (...) { recordStartupCrash("cpu_topology", "?"); }
    if (g_cpuCount <= 0 || g_allCpus.empty()) {
        LOG_ERROR("daemonInit: CPU topology detection FAILED (g_cpuCount=%d, "
                  "g_allCpus.size=%zu). Placement will be inert until restarted on "
                  "a supported device.", g_cpuCount, g_allCpus.size());
    }
    {
        g_ebpfScheduler.setRingPages(g_cfg.ebpfRingPages);
        g_ebpfScheduler.setPollFallbackEnabled(g_cfg.ebpfPollFallback);
        g_ebpfScheduler.setEventConsumer(&g_eventConsumer);
        g_ebpfScheduler.registerConfigReloadHook();
        g_ebpfScheduler.setCriticalEventWakeHook([]() {
            static std::atomic<long long> s_lastWakeMs{0};
            long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            long long prev = s_lastWakeMs.load(std::memory_order_relaxed);

            int cpuPct = g_lastCpuUsagePercent.load(std::memory_order_relaxed);
            int selfPct = g_selfCpuUsagePercent.load(std::memory_order_relaxed);
            SystemState sysSt = getSystemState();
            bool underLoad = (cpuPct >= 80)
                          || (selfPct >= 50)
                          || (sysSt == SystemState::DEGRADED)
                          || (sysSt == SystemState::EMERGENCY)
                          || g_lowOverheadMode.load(std::memory_order_relaxed);
            const long long kCoalesceMs = underLoad ? 1000LL : 30LL;

            if (g_lowOverheadMode.load(std::memory_order_relaxed)) {
                s_lastWakeMs.store(nowMs, std::memory_order_relaxed);
                return;
            }
            if (prev != 0 && (nowMs - prev) < kCoalesceMs) {
                return;
            }
            s_lastWakeMs.store(nowMs, std::memory_order_relaxed);
            g_skipNextSleep.store(true, std::memory_order_release);
            wakeMainLoopNow();
        });

        bool havePrecompiled = false;
        {
            std::string diagRoot = getModuleDir();
            std::vector<std::string> diagPaths = {
                "/system/etc/aatpe/ebpf/sched_events.bpf.o",
                diagRoot + "/system/etc/aatpe/ebpf/sched_events.bpf.o",
            };
            for (const auto& dp : diagPaths) {
                struct stat st;
                if (stat(dp.c_str(), &st) == 0 && st.st_size > 0) { havePrecompiled = true; break; }
            }
        }
        bool haveBpftool = (access("/system/bin/bpftool", X_OK) == 0 ||
                            access("/vendor/bin/bpftool", X_OK) == 0 ||
                            access("/data/adb/ksu/bin/bpftool", X_OK) == 0);
        LOG_FORCE("[EBPF] initializing, enabled=%d", g_cfg.ebpfEnabled ? 1 : 0);
        LOG_FORCE("eBPF pre-flight: enabled=%d precompiled_o=%d bpftool=%d ring_pages=%d",
                 g_cfg.ebpfEnabled ? 1 : 0, havePrecompiled ? 1 : 0,
                 haveBpftool ? 1 : 0, g_cfg.ebpfRingPages);

        if (g_cfg.ebpfEnabled) {
            LOG_FORCE("eBPF: calling g_ebpfScheduler.load()...");
            g_ebpfScheduler.load();
            bool ebpfLive = g_ebpfScheduler.isEventDrivenLive();
            g_ebpfActive.store(ebpfLive, std::memory_order_release);
            {
                std::string loaderSnap = g_ebpfScheduler.getLoaderString();
                std::string modeSnap   = g_ebpfScheduler.getModeString();
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                g_ebpfLoader = loaderSnap;
                g_ebpfMode   = modeSnap;
            }
            std::string loaderForLog, modeForLog;
            {
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                loaderForLog = g_ebpfLoader;
                modeForLog   = g_ebpfMode;
            }
            LOG_FORCE("eBPF scheduler: %s via %s (mode=%s, state=%s)",
                     ebpfLive ? "ACTIVE" : "POLLING-FALLBACK",
                     loaderForLog.c_str(), modeForLog.c_str(),
                     g_ebpfScheduler.getStateString());
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_verified").c_str(),
                      g_ebpfScheduler.getOffsetVerifyResult());
            LOG_FORCE("eBPF offset verify: %s", g_ebpfScheduler.getOffsetVerifyResult().c_str());
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_status_switch").c_str(),
                      g_ebpfScheduler.getOffsetStatusSwitch());
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_status_fork").c_str(),
                      g_ebpfScheduler.getOffsetStatusFork());
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_status_exec").c_str(),
                      g_ebpfScheduler.getOffsetStatusExec());
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_status_exit").c_str(),
                      g_ebpfScheduler.getOffsetStatusExit());
        } else {
            LOG_FORCE("eBPF disabled by config (EBPF_ENABLED=0), using polling mode");
            g_ebpfActive.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
            g_ebpfLoader = "disabled";
            g_ebpfMode = "polling";
        }
    }
    {
        std::string ocList;
        for (size_t i = 0; i < g_allCpus.size(); i++) {
            if (i > 0) ocList += ",";
            ocList += std::to_string(g_allCpus[i]);
        }
        writeFile((std::string(STATE_DIR) + "/online_cpus").c_str(), ocList);
    }
    {
        std::vector<int> cpuToPool(g_cpuCount, -1);
        for (int cpu : g_littlePool) if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 0;
        for (int cpu : g_midPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 1;
        for (int cpu : g_bigPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 2;
        for (int cpu : primePool())  if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 3;
        g_mlModel.setCpuPoolAssignment(cpuToPool);
        std::string fp = "cpu=" + std::to_string(g_cpuCount) + ";pools=" + std::to_string(mlPoolCount());
        for (int i = 0; i < g_cpuCount; i++) {
            fp += ";cpu" + std::to_string(i) + "=" + std::to_string((int)getPoolForCpu(i));
        }
        (void)0;
        g_mlModel.setTopologyFingerprint(fp);
        LOG_INFO("Topology: cpuCount=%d assignment synced to ML (LITTLE=%zu MID=%zu BIG=%zu) fp_len=%zu",
                 g_cpuCount, g_littlePool.size(), g_midPool.size(), g_bigPool.size(), fp.size());
    }
    loadThreadRules();
    loadProfile(g_currentProfile);
    aatpe_profile::profileInitGlobal(g_currentProfile, g_currentProfile);

    {
        std::lock_guard<std::mutex> lock(g_mlMutex);
        g_mlModel.setEnabled(g_cfg.mlLearningEnabled != 0);
        g_mlModel.setMaxAlpha(g_cfg.mlMaxAlpha);
        g_mlModel.setInitialLR(g_cfg.mlInitialLR);
        g_mlModel.setDecay(g_cfg.mlDecay);
        g_mlModel.setWeightClamp(g_cfg.mlWeightClamp);
        g_mlModel.init(mlPoolCount());
        const char* mp = mlModelPath();
        bool loaded = loadModelWithBackup(mp);
        if (loaded && g_cfg.mlTopologyBindingEnabled) {
            std::string topoHash = aatpe_ml::computeDeviceTopologyHash();
            if (!aatpe_ml::loadTopologySidecar(mp, topoHash)) {
                LOG_WARN("ML topology mismatch: model was trained on a different device/ROM; cold-starting (hash=%s)",
                         topoHash.c_str());
                g_mlModel.init(mlPoolCount());
                loaded = false;
            }
        }
        if (loaded) {
            LOG_INFO("ML model: loaded from %s (pools=%d, samples=%d, alpha=%.4f)",
                     mp, g_mlModel.getPoolCount(), g_mlModel.getTotalSamples(), g_mlModel.getAlpha());
        } else {
            g_mlModel.init(mlPoolCount());
            struct stat st;
            int err = errno;
            LOG_WARN("ML model: COLD START - load failed path=%s errno=%d(%s) exists=%d size=%lld",
                     mp, err, aatpeErrStr(err),
                     (stat(mp, &st) == 0) ? 1 : 0,
                     (stat(mp, &st) == 0) ? (long long)st.st_size : -1LL);
            if (stat(mp, &st) == 0 && st.st_size > 0) {
                std::string corrupt = std::string(mp) + ".corrupt";
                if (::rename(mp, corrupt.c_str()) == 0) {
                    LOG_WARN("ML model: isolated corrupt model to %s, cold-starting fresh", corrupt.c_str());
                }
                writeFile((std::string(STATE_DIR) + "/ml_model_corrupt").c_str(), "1");
            }
        }
        writeFile((std::string(STATE_DIR) + "/ml_model_path").c_str(), mp);
    }
    g_mlLastSave = time(nullptr);

    {
        std::lock_guard<std::mutex> hlock(g_hybridMutex);
        auto& hybrid = aatpe_ml_hybrid::instance();
        hybrid.init(mlPoolCount(), mlDistinctClusterCount());
        const char* mp = mlModelPath();
        std::string hybridPath = std::string(mp) + ".hybrid";
        hybrid.load(hybridPath.c_str());
        g_cfg.mlHybridEnabled = hybrid.getLoadedEnabled();
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_enabled").c_str(),
                  g_cfg.mlHybridEnabled ? "1" : "0");
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_updates").c_str(), 0);
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_ebpf_samples").c_str(), 0);
        LOG_INFO("[ML-HYBRID] initialized: enabled=%d, path=%s",
                 g_cfg.mlHybridEnabled ? 1 : 0, hybridPath.c_str());
    }

    writeFile((std::string(STATE_DIR) + "/safety_migration_limit").c_str(), g_cfg.safetyMigrationLimit);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
    struct sigaction saChld;
    memset(&saChld, 0, sizeof(saChld));
    saChld.sa_handler = SIG_DFL;
    saChld.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&saChld.sa_mask);
    sigaction(SIGCHLD, &saChld, nullptr);
    installCrashHandlers();
    setenv("PATH", "/system/bin:/system/xbin:/vendor/bin:/sbin", 1);
    setenv("ANDROID_ROOT", "/system", 0);
    aatpeProbeEnvironment();
    LOG_INFO("Daemon initialized. Mode=%s Profile=%s", g_cfg.controlMode.c_str(), aatpe_profile::effectiveProfile().c_str());
    startConfigHotReload();

    g_federatedLearner.init("/dev/aatpe/federated");
    g_federatedLearner.setEnabled(g_cfg.federatedEnabled);
    LOG_INFO("fix97: Federated learning initialized, exchange dir=/dev/aatpe/federated");

    initFrameTimeOutcomeCallbacks();

    if (g_cfg.refreshRateEventDriven) {
        aatpe::frameaware::refreshRateEventStart();
    }

    if (g_cfg.adrcEnabled) {
        aatpe::adrc::init();
    } else {
        LOG_INFO("ADRC: disabled by config (ADRC_ENABLED=0)");
    }

    aatpe::psi::init();
    aatpe::arb::init();

{
        bool shouldStartHttp = g_cfg.enableHttpServer;
        bool magiskAuto = false;
        if (!shouldStartHttp) {
            struct stat st;
            bool hasKsu = (stat("/data/adb/ksu", &st) == 0) ||
                          (stat("/data/adb/ksud", &st) == 0);
            bool hasMagisk = (stat("/data/adb/magisk", &st) == 0);
            if (hasMagisk && !hasKsu) {
                std::string marker = std::string(STATE_DIR) + "/http_autostart";
                if (access(marker.c_str(), F_OK) == 0) {
                    shouldStartHttp = true;
                    magiskAuto = true;
                } else {
                    LOG_WARN("v66-fix: Magisk detected but local HTTP control surface NOT "
                             "auto-bound (no explicit opt-in). To enable it either set "
                             "ENABLE_HTTP_SERVER=1 or create %s and restart.",
                             marker.c_str());
                }
            }
        }
        if (magiskAuto) {
            LOG_INFO("v66-fix: Magisk environment detected, HTTP autostart confirmed via marker");
        }
        applyHttpServerState(shouldStartHttp);
    }

    aatpe::config::registerReloadHook("http_server", [](const Config& cfg) {
        applyHttpServerState(cfg.enableHttpServer);
    });

    {
        using namespace aatpe::subsys;
        auto& reg = Registry::instance();
        reg.setState("scheduler", SubsystemState::ACTIVE);
        reg.setState("thermal", SubsystemState::ACTIVE);
        reg.setState("frame", SubsystemState::ACTIVE);
        reg.setState("ml", SubsystemState::ACTIVE);
        reg.setState("ebpf", g_ebpfActive.load(std::memory_order_acquire) ? SubsystemState::ACTIVE
                                           : SubsystemState::DEGRADED);
        reg.setState("webui", g_cfg.enableHttpServer ? SubsystemState::ACTIVE
                                                      : SubsystemState::DEGRADED);
        reg.writeStateFile(STATE_DIR);
        safemode::writeStateFiles(STATE_DIR);
        selfobs::writeStateFiles(STATE_DIR);
        writeFile((std::string(STATE_DIR) + "/resource_budget").c_str(),
                  "rt_threads=32 cmd_queue=64 webui_sse_clients=16 "
                  "ebpf_ring_pages=64-128 log_max_bytes=8388608 self_cpu_budget_pct=50");
    }

}

static void loopThermalTick(const Config& loopCfg) {
    if (loopCfg.thermalEnabled) thermalFreshnessTick();
    g_thermalMigrationPressure.store(
        readInt((std::string(STATE_DIR) + "/thermal_migration_pressure").c_str(), 0),
        std::memory_order_relaxed);
}

static void loopSurfaceFlingerTick(const Config& loopCfg) {
    static long long s_lastSfReadMs = 0;
    long long nowSfMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (loopCfg.frameAware && (nowSfMs - s_lastSfReadMs) >= 200) {
        readSurfaceFlinger();
        s_lastSfReadMs = nowSfMs;
    }
}

static void loopJankAndEvents(void) {
    static long long s_jankFirstSeenMs = 0;
    static long long s_jankClearSinceMs = 0;
    static bool s_emergencyArmed = true;
    constexpr long long kJankConfirmMs = 600;
    constexpr long long kJankReleaseMs = 400;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    int curJank = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0);
    bool curJankBool = (curJank != 0);
    if (curJankBool) {
        if (s_jankFirstSeenMs == 0) s_jankFirstSeenMs = nowMs;
        s_jankClearSinceMs = 0;
        long long seenFor = nowMs - s_jankFirstSeenMs;
        if (s_emergencyArmed && seenFor >= kJankConfirmMs) {
            LOG_WARN("JANK confirmed (%lldms sustained): triggering emergency migration via event monitor", seenFor);
            aatpe::event::EventMonitor::instance().triggerEvent(aatpe::event::EventType::JANK, "jank detected");
            abTestRecordJank(abTestAssignThread(0) + 1);
            s_emergencyArmed = false;
        }
    } else {
        s_jankFirstSeenMs = 0;
        if (!s_emergencyArmed) {
            if (s_jankClearSinceMs == 0) s_jankClearSinceMs = nowMs;
            if (nowMs - s_jankClearSinceMs >= kJankReleaseMs) s_emergencyArmed = true;
        }
    }

    auto& em = aatpe::event::EventMonitor::instance();
    if (em.consumeHighPriorityEvent()) {
        LOG_INFO("EVENT: high-priority event detected, emergency migration");
        emergencyMigrateCriticalThreads();
    }
    if (em.consumeMediumPriorityEvent()) {
        if (aatpe::subsys::safemode::isActive()) {
        } else {
        SystemState medEvtState = getSystemState();
        bool medEvtHighCpu = (g_lastCpuUsagePercent.load(std::memory_order_relaxed) >= 80)
                          || (g_selfCpuUsagePercent.load(std::memory_order_relaxed) >= 50);
        if (medEvtState == SystemState::DEGRADED ||
            medEvtState == SystemState::EMERGENCY ||
            medEvtHighCpu) {
        } else {
            static long long s_lastMediumDecisionMs = 0;
            long long nowMediumMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (s_lastMediumDecisionMs == 0 || (nowMediumMs - s_lastMediumDecisionMs) >= 500) {
                s_lastMediumDecisionMs = nowMediumMs;
                LOG_INFO("EVENT: medium-priority event detected, immediate full decision");
                g_skipNextSleep.store(true);
            }
        }
        }
    }
}

static void loopFrameAwareTick(void) {
    static long long s_lastRefreshUpdateMs = 0;
    long long nowRefreshMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowRefreshMs - s_lastRefreshUpdateMs >= 10000 || s_lastRefreshUpdateMs == 0) {
        aatpe::frameaware::updateRefreshRate();
        s_lastRefreshUpdateMs = nowRefreshMs;
    }
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (aatpe::frameaware::shouldTriggerFrameAware(nowMs)) {
        aatpe::frameaware::executeFrameAwareDecision();
    }
}

static void loopPollEventDetection(const Config& loopCfg) {
    static int s_lastPerfMaxFreq = 0;
    static int s_lastCpuTemp = 0;
    static std::string s_lastOnlineCpus = "";
    static long long s_lastThermalEventMs = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    int curPerfMaxFreq = 0;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        std::vector<int> perfCores;
        for (int c : g_bigPool) perfCores.push_back(c);
        if (!g_midPool.empty()) perfCores.push_back(g_midPool[0]);
        if (perfCores.empty()) {
            perfCores.push_back(0);
            if (!g_allCpus.empty()) perfCores.push_back(g_allCpus.back());
        }
        for (int cpu : perfCores) {
            std::string freqPath = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq";
            int f = readInt(freqPath.c_str(), 0);
            if (f > curPerfMaxFreq) curPerfMaxFreq = f;
        }
    }
    if (curPerfMaxFreq > 0 && curPerfMaxFreq != s_lastPerfMaxFreq) {
        aatpe::event::EventMonitor::instance().triggerEvent(
            aatpe::event::EventType::FREQ_CHANGE,
            "perf_max_freq:" + std::to_string(curPerfMaxFreq));
    }
    s_lastPerfMaxFreq = curPerfMaxFreq;

    int curCpuTemp = 0;
    {
        std::string cpuZonePath = aatpe_getThermalCpuZonePath();
        if (!cpuZonePath.empty()) {
            int tRaw = readInt(cpuZonePath.c_str(), 0);
            if (tRaw > 1500) curCpuTemp = tRaw / 1000;
            else if (tRaw > 150) curCpuTemp = tRaw / 10;
            else curCpuTemp = tRaw;
        } else {
            DIR* tzDir = opendir("/sys/class/thermal");
            if (tzDir) {
                struct dirent* e;
                while ((e = readdir(tzDir)) != nullptr) {
                    if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
                    std::string tp = std::string("/sys/class/thermal/") + e->d_name + "/temp";
                    int t = readInt(tp.c_str(), 0);
                    if (t > 1500) t /= 1000;
                    else if (t > 150) t /= 10;
                    if (t > curCpuTemp && t <= 200) curCpuTemp = t;
                }
                closedir(tzDir);
            }
        }
    }
    int thermalFloor = loopCfg.thermalWarningThreshold;
    int tempDelta = curCpuTemp - s_lastCpuTemp;
    if (s_lastCpuTemp > 0 &&
        curCpuTemp >= thermalFloor &&
        tempDelta >= 5) {
        if (nowMs - s_lastThermalEventMs > 3000) {
            aatpe::event::EventMonitor::instance().triggerEvent(
                aatpe::event::EventType::THERMAL_SPIKE,
                "temp:" + std::to_string(curCpuTemp) + "C delta:" + std::to_string(tempDelta));
            s_lastThermalEventMs = nowMs;
        }
    }
    s_lastCpuTemp = curCpuTemp;

    std::string curOnlineCpus = readFile("/sys/devices/system/cpu/online");
    if (!s_lastOnlineCpus.empty() && curOnlineCpus != s_lastOnlineCpus) {
        aatpe::event::EventMonitor::instance().triggerEvent(
            aatpe::event::EventType::HOTPLUG, "online:" + curOnlineCpus);
    }
    s_lastOnlineCpus = curOnlineCpus;
}

static void loopForegroundTick(const Config& loopCfg) {
    static std::string s_prevForegroundApp = "";
    static long long s_fgBoostEdgeMs = 0;
    constexpr long long kFgBoostDwellMs = 2000;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string curFg = aatpeDetectForegroundApp();
    bool fgValid = !curFg.empty() && curFg != "unknown";
    if (fgValid) {
        checkAndApplyPerAppConfig(curFg.c_str());
        if (curFg != s_prevForegroundApp) {
            if (!s_prevForegroundApp.empty()) {
                aatpe::event::EventMonitor::instance().triggerEvent(
                    aatpe::event::EventType::APP_SWITCH, curFg);
                LOG_INFO("APP_SWITCH: %s -> %s", s_prevForegroundApp.c_str(), curFg.c_str());
            }
            s_prevForegroundApp = curFg;
            aatpe::coldStartOnForegroundSwitch(curFg);
            aatpe::placement::releaseForegroundDownpins(curFg);
            aatpe::placement::onForegroundSwitchEdge(curFg);
            aatpe::aatpeBoostSet("fg", 50);
            s_fgBoostEdgeMs = nowMs;
            aatpe::placement::onInputEvent();
        }
        if (s_fgBoostEdgeMs != 0 && nowMs - s_fgBoostEdgeMs > kFgBoostDwellMs) {
            aatpe::aatpeBoostClear("fg");
            s_fgBoostEdgeMs = 0;
        }
    } else {
        if (s_fgBoostEdgeMs != 0) {
            aatpe::aatpeBoostClear("fg");
            s_fgBoostEdgeMs = 0;
        }
        s_prevForegroundApp.clear();
    }

    if (fgValid) {
        aatpe::placement::healForegroundCgroupMembership(curFg);
    }

    if (preboostEnabled.load(std::memory_order_relaxed)) {
        static long long s_lastC1TickMs = 0;
        long long nowC1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (s_lastC1TickMs > 0 && (nowC1Ms - s_lastC1TickMs) < 1000) {
        } else {
        s_lastC1TickMs = nowC1Ms;
        using namespace aatpe::preboost;
        static bool s_execSet = false;
        if (!s_execSet) {
            setBoostExecutor([](int level) {
                if (level > 0) aatpeBoostSet("preboost", level);
                else           aatpeBoostClear("preboost");
            });
            s_execSet = true;
        }
        float fpsNow   = (float)readInt((std::string(STATE_DIR) + "/game_avg_fps").c_str(), 60);
        float fpsP99   = (float)readInt((std::string(STATE_DIR) + "/game_p99_fps").c_str(), 60);
        int   missPct  = readInt((std::string(STATE_DIR) + "/game_frame_miss").c_str(), 0);
        int   sfLatUs  = readInt((std::string(STATE_DIR) + "/sf_latency_avg").c_str(), 0);
        int   frameTimeUs = readInt((std::string(STATE_DIR) + "/target_frame_time").c_str(), 16667);
        if (frameTimeUs < 3000) frameTimeUs = 16667;
        float fpsTargetFps = 1e6f / (float)frameTimeUs;
        static std::vector<int> s_cpuHist;
        static long long s_lastCpuSampleMs = 0;
        long long nowCpuMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float sampleIntervalSec = 1.0f;
        if (s_lastCpuSampleMs > 0) {
            sampleIntervalSec = (float)(nowCpuMs - s_lastCpuSampleMs) / 1000.0f;
        }
        s_lastCpuSampleMs = nowCpuMs;
        int cpuLoadPct = readInt((std::string(STATE_DIR) + "/cpu_util_pct").c_str(), -1);
        if (cpuLoadPct >= 0) {
            s_cpuHist.push_back(cpuLoadPct);
            if ((int)s_cpuHist.size() > 6) s_cpuHist.erase(s_cpuHist.begin());
        }
        int hrPct = g_thermalHeadroomPct.load(std::memory_order_relaxed);
        bool hrValid = (hrPct >= 0);
        float hr01 = hrValid ? (float)hrPct / 100.0f : 0.0f;
        int jankVal = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0);
        bool controlModePassive;
        {
            std::lock_guard<std::mutex> cfgLk(g_cfgMutex);
            controlModePassive = (g_cfg.controlMode == "passive");
        }
        PreBoostFeatures pf = buildFeatures(
            fpsNow, fpsP99, missPct, sfLatUs, s_cpuHist,
            sampleIntervalSec,
            fpsTargetFps, (float)frameTimeUs,
            hr01, hrValid,
            g_thermalEmergency.load(std::memory_order_relaxed),
            controlModePassive,
            (jankVal != 0));
        if (!aatpe::scenewire::loadingActiveSuppressesPreboost()) {
            preBoostTick(nowCpuMs, pf);
        }
        }
    }

    aatpe::psi::tick();
    aatpe::arb::tick();
    bool mlThermalFreeze = aatpe::aatpeThermalMayFreezeTraining(loopCfg.thermalEnabled);
    bool thermalFreeze = g_thermalEmergency.load(std::memory_order_relaxed) || mlThermalFreeze;
    aatpe::coldStartSetThermalFreeze(thermalFreeze);
    int mlHalLevel = aatpe::aatpeThermalHalLevel();
    g_mlTrainingPausedByThermal.store(thermalFreeze, std::memory_order_relaxed);
    g_mlFrameOutcomeSuppressedByThermal.store(mlHalLevel >= 2 && mlHalLevel < 3,
                                              std::memory_order_relaxed);
    writeFile((std::string(STATE_DIR) + "/thermal_ml_freeze_reason").c_str(),
              aatpe::aatpeThermalFreezeReason());
    writeFile((std::string(STATE_DIR) + "/thermal_ml_freeze_pause_c").c_str(), "85");
    writeFile((std::string(STATE_DIR) + "/thermal_ml_freeze_resume_c").c_str(), "80");
    aatpe::coldStartTick();
    aatpe::gpuBoostTick();
    {
        static int s_throttleTickDiv = 0;
        if (++s_throttleTickDiv >= 3) {
            s_throttleTickDiv = 0;
            aatpeGpuThrottleTick();
        }
    }
}

static void loopAuxScan(time_t now) {
    static time_t s_lastAuxScan = 0;
    static int auxSlot = 0;
    if (now - s_lastAuxScan < 5) return;
    switch (auxSlot) {
    case 0:
        readBatteryPower();
        readBatteryHealth();
        break;
    case 1:
        readGpuUtilization();
        break;
    case 2:
        detectAudioPlayback();
        break;
    case 3: {
        float avgCpuLoad = 0.0f;
        int cpuCountForScene = 0;
        {
            std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
            for (const auto& _sb_item : g_cpuMetrics) {
                const auto& metrics = _sb_item.second;
                if (metrics.util >= 0) { avgCpuLoad += metrics.util; cpuCountForScene++; }
            }
        }
        if (cpuCountForScene > 0) avgCpuLoad /= (float)cpuCountForScene;
        int jankVal = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0);
        if (aatpe::scenewire::chooseRoute(sceneInferEnabled.load(std::memory_order_relaxed))
                == aatpe::scenewire::SceneRoute::NEW_SCENEFUSE) {
            aatpe::scenewire::auxTickSlot3(avgCpuLoad);
        } else {
            detectGameScene(avgCpuLoad, jankVal ? 5.0f : 0.0f, 0.0f);
        }
        if (aatpe::c4freezer::c4FreezerEnabled()) {
            aatpe::c4freezer::c4FreezerTick();
        }
        break;
    }
    case 4:
        checkAndPreloadNewApps();
        break;
    }
    auxSlot = (auxSlot + 1) % 5;
    s_lastAuxScan = now;
}

static void loopEbpfDrain(void) {
    std::string ebpfModeSnap;
    {
        std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
        ebpfModeSnap = g_ebpfMode;
    }
    if (!g_ebpfActive.load(std::memory_order_acquire) || ebpfModeSnap == "polling") return;
    size_t events = 0;
    if (g_ebpfScheduler.isPollThreadRunning()) {
        events = g_ebpfScheduler.dispatchQueuedEvents();
    } else {
        events = (size_t)g_ebpfScheduler.pollEvents(256);
    }
    if (events > 0) {
        g_ebpfEventRate.store(g_ebpfScheduler.getEventRatePerSec());
    }
    if (g_ebpfScheduler.shouldDegrade()) {
        g_ebpfScheduler.degrade("ring overflow or map read failure");
        std::string newMode = g_ebpfScheduler.getModeString();
        std::string oldMode;
        {
            std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
            oldMode = g_ebpfMode;
        }
        if (newMode != oldMode) {
            {
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                g_ebpfMode = newMode;
            }
            LOG_WARN("eBPF degraded to %s mode", newMode.c_str());
        }
    }
    g_ebpfScheduler.logDegradeSummaryIfDue();
    g_ebpfScheduler.recoverIfHealthy();
    g_ebpfScheduler.dynamicRingAdjustIfDue();
    g_ebpfScheduler.retryLoadIfDue();
    std::string newMode2 = g_ebpfScheduler.getModeString();
    std::string oldMode2;
    {
        std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
        oldMode2 = g_ebpfMode;
    }
    if (newMode2 != oldMode2) {
        {
            std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
            g_ebpfMode = newMode2;
        }
        LOG_WARN("eBPF recovered to %s mode", newMode2.c_str());
    }
}

void aatpeEvictDeadTidEntries() {
    auto snapshotLiveTids = []() {
        std::unordered_set<int> tids;
        std::lock_guard<std::mutex> tlock(g_threadsMutex);
        tids.reserve(g_threads.size());
        for (const auto& kv : g_threads) tids.insert(kv.first);
        return tids;
    };
    {
        auto liveTids = snapshotLiveTids();
        std::lock_guard<std::mutex> lk(g_lastMigMutex);
        for (auto it = g_lastMigrationTime.begin(); it != g_lastMigrationTime.end(); )
            it = liveTids.count(it->first) ? ++it : g_lastMigrationTime.erase(it);
    }
    {
        auto liveTids = snapshotLiveTids();
        std::lock_guard<std::mutex> lk(g_migFailMutex);
        for (auto it = g_migFailCooldown.begin(); it != g_migFailCooldown.end(); )
            it = liveTids.count(it->first) ? ++it : g_migFailCooldown.erase(it);
        for (auto it = g_migFailCount.begin(); it != g_migFailCount.end(); )
            it = liveTids.count(it->first) ? ++it : g_migFailCount.erase(it);
        for (auto it = g_migFailCpu.begin(); it != g_migFailCpu.end(); )
            it = liveTids.count(it->first) ? ++it : g_migFailCpu.erase(it);
        for (auto it = g_migFailBackoffUntilMs.begin(); it != g_migFailBackoffUntilMs.end(); )
            it = liveTids.count(it->first) ? ++it : g_migFailBackoffUntilMs.erase(it);
    }
    {
        auto liveTids = snapshotLiveTids();
        std::lock_guard<std::mutex> ssk(g_schedstatMutex);
        for (auto it = g_schedstatLastSample.begin(); it != g_schedstatLastSample.end(); )
            it = liveTids.count(it->first) ? ++it : g_schedstatLastSample.erase(it);
        for (auto it = g_schedstatLastUtil.begin(); it != g_schedstatLastUtil.end(); )
            it = liveTids.count(it->first) ? ++it : g_schedstatLastUtil.erase(it);
    }
}

void daemonLoop() {
    time_t lastThreadScan = 0;
    time_t lastDecisionCleanup = 0;
    time_t lastWebuiFastUpdate = 0;
    time_t lastWebuiModelUpdate = 0;
    std::chrono::steady_clock::time_point lastHeavyJsonSteady{};
    time_t lastApkDetect = 0;
    time_t lastDiagWrite = 0;
    bool diagTick = false;
    int loopCount = 0;
    time_t lastIntegrityCheck = 0;
    bool s_prevMlMature = false;
    while (!g_shouldExit.load(std::memory_order_relaxed)) {
        auto loopStart = std::chrono::steady_clock::now();
        g_lastDecisionSteady = loopStart;
        {
            static time_t s_lastHeartbeatWrite = 0;
            struct timespec hbTs;
            clock_gettime(CLOCK_BOOTTIME, &hbTs);
            time_t nowHb = (time_t)hbTs.tv_sec;
            if (nowHb - s_lastHeartbeatWrite >= 1) {
                writeFile((std::string(STATE_DIR) + "/heartbeat").c_str(), (int)nowHb);
                static time_t s_prevBootTime = 0;
                if (s_prevBootTime != 0 && (nowHb - s_prevBootTime) > 60) {
                    LOG_INFO("Wakeup: BOOTTIME jumped %llds since last tick, forcing rebalance",
                             (long long)(nowHb - s_prevBootTime));
                    g_skipNextSleep.store(true);
                }
                s_prevBootTime = nowHb;
            }
        }
        {
            time_t nowDiag = time(nullptr);
            diagTick = (nowDiag - lastDiagWrite >= 1);
            if (diagTick) lastDiagWrite = nowDiag;
        }
        processControlCommands();
        {
            time_t nowInt = time(nullptr);
            if (nowInt - lastIntegrityCheck >= 3600) {
                lastIntegrityCheck = nowInt;
                bool intOk = aatpe::integrity::verifyModuleIntegrity();
                if (!intOk) {
                    LOG_ERROR("INTEGRITY: hourly re-verification FAILED");
                    std::string rep = aatpe::integrity::getIntegrityReport();
                    if (!rep.empty()) LOG_ERROR("INTEGRITY report:\n%s", rep.c_str());
                } else {
                    LOG_INFO("INTEGRITY: hourly re-verification OK");
                }
            }
        }
        if (diagTick) {
            int uptime = (int)(time(nullptr) - g_startTime.load());
            writeFile((std::string(STATE_DIR) + "/daemon_uptime").c_str(), uptime);
        }
        if (g_reloadProfile.load(std::memory_order_relaxed)) {
            g_reloadProfile.store(0, std::memory_order_relaxed);
            LOG_INFO("SIGUSR1 received: re-applying current profile winner");
            aatpe_profile::profileReapplyWinner();
        }
        Config loopCfg;
        snapshotConfig(loopCfg);
        loopThermalTick(loopCfg);
        loopSurfaceFlingerTick(loopCfg);
        loopJankAndEvents();
        loopFrameAwareTick();
        readCpuMetrics();
        const int systemLoadNow = readInt((std::string(STATE_DIR) + "/system_load").c_str(), 0);
        const bool jankActiveNow = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0) != 0;
        precomputeCpuBaseScores();
        loopPollEventDetection(loopCfg);
        proactiveMigrationCheck();
        refreshCpuTopology();
        if (topologyRetryDue((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count())) {
            LOG_INFO("topology: scheduled evidence retry");
            detectCpuTopology();
        }
        {
            int wantPools = mlPoolCount();
            if (g_mlModel.getPoolCount() != wantPools) {
                LOG_WARN("ML pool dimension drift: model=%d topology=%d, migrating (preserve weights)",
                         g_mlModel.getPoolCount(), wantPools);
                std::lock_guard<std::mutex> lock(g_mlMutex);
                g_mlModel.migratePoolCount(wantPools);
                std::vector<int> cpuToPool(g_cpuCount, -1);
                for (int cpu : g_littlePool) if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 0;
                for (int cpu : g_midPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 1;
                for (int cpu : g_bigPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 2;
                for (int cpu : primePool())  if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 3;
                g_mlModel.setCpuPoolAssignment(cpuToPool);
            }
        }
        reprobeUnmigratableThreads();
        loopForegroundTick(loopCfg);
        bool effectiveAutoProfile = loopCfg.autoProfile && !g_helperAutoSuppressed.load();
        if (effectiveAutoProfile) autoSelectProfile();
        time_t now = time(nullptr);
        loopAuxScan(now);
        if (now - lastApkDetect >= 30) {
            bool prev = g_helperApkInstalled;
            applyHelperApkDetection();
            if (prev != g_helperApkInstalled) {
                LOG_INFO("APK state changed: %s -> %s",
                         prev ? "installed" : "not_installed",
                         g_helperApkInstalled ? "installed" : "not_installed");
            }
            lastApkDetect = now;
        }
        loopEbpfDrain();
        {
            int scanInterval = loopCfg.threadScanInterval;
            std::string ebpfModeSnap;
            {
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                ebpfModeSnap = g_ebpfMode;
            }
            if (g_ebpfActive.load(std::memory_order_acquire) && ebpfModeSnap == "event_driven") {
                scanInterval = 60;
            }
            if (now - lastThreadScan >= scanInterval) {
                scanThreads();
                lastThreadScan = now;
                g_eventConsumer.clearPending();
                aatpeEvictDeadTidEntries();
            }
        }
        if (now - lastDecisionCleanup >= daemon::kDecisionCleanupIntervalSec) {
            cleanupDecisionsDir();
            {
                Config cfgSnap;
                snapshotConfig(cfgSnap);
                stateTtlSweep(now, cfgSnap.stateCacheTtlHours);
            }
            writePlacementAvgStats();
            lastDecisionCleanup = now;
        }

        if (diagTick) {
            writeFile((std::string(STATE_DIR) + "/decision_count").c_str(), (long long)g_totalDecisions.load());
            writeFile((std::string(STATE_DIR) + "/migration_count").c_str(), (long long)g_totalMigrations.load());
            atomicWriteFile((std::string(STATE_DIR) + "/total_decisions").c_str(),
                            (long long)g_totalDecisions.load());
            atomicWriteFile((std::string(STATE_DIR) + "/total_migrations").c_str(),
                            (long long)g_totalMigrations.load());
        }
        {
            time_t nowWebui = time(nullptr);
            if (nowWebui - lastWebuiFastUpdate >= 2) {
                int sseClients = g_sseClientCount.load(std::memory_order_acquire);
                auto nowHeavy = std::chrono::steady_clock::now();
                bool heavyJsonDue = (sseClients > 0) ||
                    (lastHeavyJsonSteady == std::chrono::steady_clock::time_point{}) ||
                    (std::chrono::duration_cast<std::chrono::seconds>(
                        nowHeavy - lastHeavyJsonSteady).count() >= 5);
                if (heavyJsonDue) {
                    writeThreadsJson();
                    writeDecisionsJson();
                    writeC4Json();
                    writeExplainJson();
                    lastHeavyJsonSteady = nowHeavy;
                }
                writeSystemJson();
                writePerCpuJson();
                writeLogsJson();
                writeConfigJson();
                if (hasAnyPredictionErrorSamples()) {
                    writeFile((std::string(STATE_DIR) + "/prediction_mae").c_str(), getAveragePredictionMAE());
                } else {
                    writeFile((std::string(STATE_DIR) + "/prediction_mae").c_str(), "");
                }
                writeFile((std::string(STATE_DIR) + "/prediction_confidence").c_str(), getPredictionConfidence(0));
                aatpe::event::EventMonitor::instance().writeStats(STATE_DIR);
                aatpe::frameaware::writeStats(STATE_DIR);
                aatpe::diag::collect();
                aatpe::diag::writeStateFiles(STATE_DIR);
                lastWebuiFastUpdate = nowWebui;
            }
            if (nowWebui - lastWebuiModelUpdate >= 5) {
                writeModelJson();
                lastWebuiModelUpdate = nowWebui;
                {
                    std::lock_guard<std::mutex> mlLock(g_mlMutex);
                    writeFile((std::string(STATE_DIR) + "/ml_samples").c_str(), g_mlModel.getTotalSamples());
                    writeFile((std::string(STATE_DIR) + "/ml_drift_detected").c_str(),
                              aatpe_ml::g_mlDriftDetected.load() ? 1 : 0);
                    {
                        char alphaBuf[32];
                        snprintf(alphaBuf, sizeof(alphaBuf), "%.4f", (double)g_mlModel.getAlpha());
                        writeFile((std::string(STATE_DIR) + "/ml_alpha").c_str(), alphaBuf);
                    }
                    bool nowMature = g_mlModel.isMature();
                    writeFile((std::string(STATE_DIR) + "/ml_mature").c_str(), nowMature ? 1 : 0);
                    if (nowMature && !s_prevMlMature) {
                        LOG_INFO("ML matured (samples=%d, alpha=%.3f), confidence gate ENABLED",
                                 g_mlModel.getTotalSamples(), (double)g_mlModel.getAlpha());
                    } else if (!nowMature && s_prevMlMature) {
                        LOG_INFO("ML reset to immature (samples=%d, alpha=%.3f), confidence gate DISABLED",
                                 g_mlModel.getTotalSamples(), (double)g_mlModel.getAlpha());
                    }
                    s_prevMlMature = nowMature;
                    writeFile((std::string(STATE_DIR) + "/ml_pending_created").c_str(), (long long)g_mlPendingCreated.load());
                    writeFile((std::string(STATE_DIR) + "/ml_outcome_measured").c_str(), (long long)g_mlOutcomeMeasured.load());
                    writeFile((std::string(STATE_DIR) + "/ml_outcome_valid").c_str(), (long long)g_mlOutcomeValid.load());
                    writeFile((std::string(STATE_DIR) + "/ml_outcome_skip_sleeping").c_str(), (long long)g_mlOutcomeSkipSleeping.load());
                    writeFile((std::string(STATE_DIR) + "/ml_outcome_skip_saturated").c_str(), (long long)g_mlOutcomeSkipSaturated.load());
                    writeFile((std::string(STATE_DIR) + "/ml_outcome_skip_reset").c_str(), (long long)g_mlOutcomeSkipReset.load());
                    writeFile((std::string(STATE_DIR) + "/ml_outcome_skip_other").c_str(), (long long)g_mlOutcomeSkipOther.load());
                    writeFile((std::string(STATE_DIR) + "/ml_pending_expired").c_str(), (long long)g_mlPendingExpired.load());
                    writeFile((std::string(STATE_DIR) + "/ml_pending_tid_reused").c_str(), (long long)g_mlPendingTidReused.load());
                    writeFile((std::string(STATE_DIR) + "/ml_pending_thread_exited").c_str(), (long long)g_mlPendingThreadExited.load());
                    writeFile((std::string(STATE_DIR) + "/ml_pending_skipped_existing").c_str(), (long long)aatpe_ml::g_pendingSkippedExisting.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_thermal").c_str(), (long long)aatpe_ml::g_mlUpdateSkipThermal.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_disabled").c_str(), (long long)aatpe_ml::g_mlUpdateSkipDisabled.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_pool").c_str(), (long long)aatpe_ml::g_droppedInvalidTargetPool.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_outcome").c_str(), (long long)aatpe_ml::g_invalidOutcomeSkipped.load());
                    writeFile((std::string(STATE_DIR) + "/ml_training_paused_by_thermal").c_str(), aatpe_ml::g_mlTrainingPausedByThermal.load() ? 1 : 0);
                    writeFile((std::string(STATE_DIR) + "/ml_model_anomaly_latched").c_str(), g_mlModel.isAnomalyLatched() ? 1 : 0);
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_outcome_valid").c_str(), (long long)g_mlShadowOutcomeValid.load());
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_samples_collected").c_str(), (long long)aatpe_ml::g_mlShadowSamplesCollected.load());
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_samples_dropped").c_str(), (long long)aatpe_ml::g_mlShadowSamplesDropped.load());
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_sampling_enabled").c_str(), loopCfg.mlShadowSampling ? 1 : 0);
                }
            }
        }

        {
            static time_t s_lastExposureWrite = 0;
            if (now - s_lastExposureWrite >= 5) {
                writeFusionWeightsState();
                writeFrameTimeOutcomeState();
                writeSnapshotJson();
                s_lastExposureWrite = now;
            }
        }

        bool highLoad = !checkSystemLoad();
        SystemState sysState = getSystemState();
        bool degradedActive = (sysState == SystemState::DEGRADED);
        int effectiveMaxMigrations = loopCfg.maxMigrationsPerCycle;
        if (highLoad || degradedActive) {
            if (highLoad) g_highLoadCycles++;
            if (degradedActive) {
                effectiveMaxMigrations = std::max(1, loopCfg.maxMigrationsPerCycle / 4);
            } else {
                effectiveMaxMigrations = std::max(1, loopCfg.maxMigrationsPerCycle / 2);
            }
            static time_t s_lastHighLoadLog = 0;
            time_t nowLog = time(nullptr);
            if (nowLog - s_lastHighLoadLog >= 5) {
                LOG_WARN("System load high / DEGRADED state: max_migrations_per_cycle=%d (was %d), state=%s",
                         effectiveMaxMigrations, loopCfg.maxMigrationsPerCycle,
                         systemStateToString(sysState));
                s_lastHighLoadLog = nowLog;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            writeFile((std::string(STATE_DIR) + "/tracked_threads").c_str(), (int)g_threads.size());
        }
        if (loopCfg.mlLearningEnabled) {
            std::lock_guard<std::mutex> threadsLock(g_threadsMutex);
            std::lock_guard<std::mutex> mlLock(g_mlMutex);
            g_mlModel.setShadowSamplingEnabled(loopCfg.mlShadowSampling);
            g_mlPendingExpired += g_mlModel.clearExpiredPendingDecisions(60);
            g_mlModel.cleanExpiredBaselines(300);
            int loadInt = systemLoadNow;
            static unsigned long long s_lastOutcomeBatchMs = 0;
            unsigned long long nowOutcomeMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            bool runOutcomeBatch = (nowOutcomeMs - s_lastOutcomeBatchMs >= 500);
            if (runOutcomeBatch) s_lastOutcomeBatchMs = nowOutcomeMs;
            if (runOutcomeBatch) {
            std::vector<PendingDecision> allPending = g_mlModel.getAllPendingDecisions();
            for (const auto& pd : allPending) {
                int tid = pd.tid;
                auto tit = g_threads.find(tid);
                bool threadAlive = (tit != g_threads.end());
                if (!threadAlive) {
                    g_mlPendingThreadExited++;
                    g_mlModel.clearPendingDecision(tid);
                    continue;
                }
                const ThreadInfo& ti = tit->second;
                if (pd.starttime != 0 && ti.starttime != 0 && pd.starttime != ti.starttime) {
                    LOG_DEBUG("ML: tid=%d TID reused (starttime %llu -> %llu), discarding stale pending",
                              tid, (unsigned long long)pd.starttime, (unsigned long long)ti.starttime);
                    g_mlPendingTidReused++;
                    g_mlModel.clearPendingDecision(tid);
                    continue;
                }
                unsigned long long nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (pd.create_time_ms > 0) {
                    if (nowMs >= pd.create_time_ms &&
                        (nowMs - pd.create_time_ms) < aatpe_ml::MIN_MEASURE_INTERVAL_MS) {
                        continue;
                    }
                }
                if (pd.consecutive_sleeps > 0 && pd.last_attempt_ms > 0) {
                    int shift = pd.consecutive_sleeps - 1;
                    if (shift > 6) shift = 6;
                    unsigned long long backoffMs = 100ULL * (1ULL << shift);
                    if (backoffMs > 5000) backoffMs = 5000;
                    if ((nowMs - pd.last_attempt_ms) < backoffMs) {
                        continue;
                    }
                }
                unsigned long long afterRunTime = ti.runTime;
                {
                    std::ifstream sf("/proc/" + std::to_string(ti.pid) + "/task/" + std::to_string(tid) + "/schedstat");
                    if (sf) {
                        unsigned long long rt = 0, wt = 0;
                        unsigned long ns = 0;
                        sf >> rt >> wt >> ns;
                        if (rt > 0) afterRunTime = rt;
                    }
                }
                int afterFreq = 0;
                int curCpuMaxFreq = 0, tgtCpuMaxFreq = 0;
                {
                    std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                    auto mit = g_cpuMetrics.find(ti.currentCpu);
                    if (mit != g_cpuMetrics.end()) {
                        afterFreq = mit->second.curFreq;
                        curCpuMaxFreq = mit->second.maxFreq;
                    }
                    auto ttit = g_cpuMetrics.find(pd.target_cpu);
                    if (ttit != g_cpuMetrics.end()) tgtCpuMaxFreq = ttit->second.maxFreq;
                }
                int baseFreq = 1;
                if (curCpuMaxFreq > 0) {
                    baseFreq = curCpuMaxFreq;
                } else if (tgtCpuMaxFreq > 0) {
                    baseFreq = tgtCpuMaxFreq;
                }
                float baseline = g_mlModel.getBaselineRunRatio(tid);
                time_t after_ts = time(nullptr);
                unsigned long long after_mono_ns = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                g_mlOutcomeMeasured++;
                bool skipReset = (afterRunTime < pd.before_run_time);
                unsigned long long delta_run = skipReset ? 0 : (afterRunTime - pd.before_run_time);
                bool skipSleeping = (!skipReset && delta_run == 0);
                float ftAfterP95 = 0.0f;
                int ftAfterSamples = 0;
                bool isUiRenderThread = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
                bool ftSuppressed = g_mlFrameOutcomeSuppressedByThermal.load(std::memory_order_relaxed);
                if (loopCfg.mlFrameTimeOutcome && !ftSuppressed && isUiRenderThread && pd.frame_time_before_samples > 0) {
                    FrameTimeOutcome ftAfter = g_gfxInfoProvider.getSnapshot();
                    if (ftAfter.sample_count > 0 && ftAfter.frame_time_p95 > 0.0f) {
                        ftAfterP95 = ftAfter.frame_time_p95;
                        ftAfterSamples = ftAfter.sample_count;
                    }
                }
                float outcome = LearningModel::measureOutcomeV2(
                    pd.before_run_time, afterRunTime,
                    pd.before_timestamp, after_ts,
                    pd.before_freq, afterFreq,
                    baseFreq, loadInt, baseline,
                    pd.frame_time_before_p95, ftAfterP95,
                    ftAfterSamples, ftSuppressed ? 0.0f : loopCfg.mlFrameTimeWeight,
                    isUiRenderThread,
                    pd.before_mono_ns, after_mono_ns);
                if (outcome >= 0.0f) {
                    long long ageSec = (long long)after_ts - (long long)pd.before_timestamp;
                    if (ageSec >= 0 && ageSec < 2) {
                        outcome = 0.5f + (outcome - 0.5f) * 0.5f;
                    }
                    static bool s_jankLabelPrevVote = false;
                    static unsigned long long s_jankLabelPunishUntilMs = 0;
                    static constexpr unsigned long long kJankLabelPunishHoldMs = 1500;
                    bool jankVoteNow = aatpe::frameaware::jankBoostVoteActive();
                    unsigned long long nowJankLabelMs = (unsigned long long)std::chrono::duration_cast<
                        std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (jankVoteNow && !s_jankLabelPrevVote) {
                        s_jankLabelPunishUntilMs = nowJankLabelMs + kJankLabelPunishHoldMs;
                    }
                    if (!jankVoteNow) {
                        s_jankLabelPunishUntilMs = 0;
                    }
                    s_jankLabelPrevVote = jankVoteNow;
                    bool punishByJank = (jankVoteNow && nowJankLabelMs < s_jankLabelPunishUntilMs);
                    if (punishByJank) {
                        outcome = outcome * 0.5f + 0.2f * 0.5f;
                    }
                    int actualCpu = ti.currentCpu;
                    int actualPool = -1;
                    {
                        std::vector<int> pa = g_mlModel.getCpuPoolAssignment();
                        if (actualCpu >= 0 && actualCpu < (int)pa.size())
                            actualPool = pa[actualCpu];
                    }
                    if (actualPool < 0 || actualPool >= g_mlModel.getPoolCount()) {
                        actualPool = pd.target_pool;
                        if (actualPool < 0 || actualPool >= g_mlModel.getPoolCount()) {
                            std::vector<int> pa2 = g_mlModel.getCpuPoolAssignment();
                            actualPool = (pd.target_cpu >= 0 && pd.target_cpu < (int)pa2.size())
                                       ? pa2[pd.target_cpu] : -1;
                            if (actualPool < 0) actualPool = 0;
                        }
                    }
                    g_mlModel.setMadFilterEnabled(loopCfg.mlOutcomeMadFilter != 0);
                    g_mlModel.setDriftRelearnEnabled(loopCfg.mlDriftRelearn != 0);
                    float learnOutcome = outcome;
                    if (pd.counterfactualControl)
                        learnOutcome = LearningModel::counterfactualControlLabel(outcome);
                    if (pd.shadow) {
                        g_mlModel.recordShadowOutcome(actualPool, pd.features, learnOutcome);
                        g_mlShadowOutcomeValid++;
                    } else {
                    UpdateResult ur = g_mlModel.update(actualPool, pd.features, learnOutcome);
                    if (ur == UpdateResult::TRAINED) g_mlOutcomeValid++;
                    {
                        static aatpe_ml::DriftDetector s_drift;
                        static int s_driftLastWindow = 0;
                        if (loopCfg.mlDriftWindow != s_driftLastWindow) {
                            s_drift.setWindow(loopCfg.mlDriftWindow);
                            s_driftLastWindow = loopCfg.mlDriftWindow;
                        }
                        s_drift.recordOutcome(outcome, actualPool);
                        if (loopCfg.mlDriftRelearn != 0 && s_drift.isDriftDetected()) {
                            g_mlModel.nudgeTowardNeutral(actualPool);
                        }
                    }
                    }
                    LOG_DEBUG("ML update V2: tid=%d actualPool=%d actualCpu=%d targetPool=%d outcome=%.3f baseline=%.3f load=%d samples=%d alpha=%.4f%s",
                              tid, actualPool, actualCpu, pd.target_pool, outcome, baseline, loadInt,
                              g_mlModel.getTotalSamples(), g_mlModel.getAlpha(),
                              pd.shadow ? " [SHADOW]" : "");
                    g_mlModel.setPendingSleepBackoff(tid, 0, 0);
                    g_mlModel.clearPendingDecision(tid);
                } else {
                    if (skipReset) g_mlOutcomeSkipReset++;
                    else if (skipSleeping) {
                        g_mlOutcomeSkipSleeping++;
                        g_mlModel.setPendingSleepBackoff(tid, pd.consecutive_sleeps + 1, nowMs);
                    } else {
                        unsigned long long dt_ns = (after_ts > pd.before_timestamp) ?
                            (unsigned long long)(after_ts - pd.before_timestamp) * 1000000000ULL : 1;
                        float rr = (dt_ns > 0) ? (float)delta_run / (float)dt_ns : 0.0f;
                        if (rr > 0.99f) g_mlOutcomeSkipSaturated++;
                        else g_mlOutcomeSkipOther++;
                    }
                    LOG_DEBUG("ML update V2: tid=%d SKIPPED (anomaly/outlier), pending kept for retry", tid);
                }
            }
            static unsigned long long s_lastBaselineDutyMs = 0;
            bool runBaselineDuty = (nowOutcomeMs - s_lastBaselineDutyMs >= 2000);
            if (runBaselineDuty) {
                s_lastBaselineDutyMs = nowOutcomeMs;
                unsigned long long nowBaseNs = (unsigned long long)std::chrono::duration_cast<
                    std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                for (auto&_sb_item : g_threads) {
                    auto& tid = _sb_item.first; auto& ti = _sb_item.second; (void)tid;
                    if (g_mlModel.findPendingDecision(tid) >= 0) continue;
                    if (ti.nrSwitches == 0) continue;
                    unsigned long long curRun = ti.runTime;
                    {
                        std::ifstream sf("/proc/" + std::to_string(ti.pid) + "/task/" +
                                         std::to_string(tid) + "/schedstat");
                        if (sf) {
                            unsigned long long rt = 0, wt = 0; unsigned long long sw = 0;
                            sf >> rt >> wt >> sw;
                            if (rt > 0) curRun = rt;
                        }
                    }
                    int curFreq = 0, maxFreq = 1;
                    {
                        std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                        auto mit = g_cpuMetrics.find(ti.currentCpu);
                        if (mit != g_cpuMetrics.end()) {
                            curFreq = mit->second.curFreq;
                            if (mit->second.maxFreq > 0) maxFreq = mit->second.maxFreq;
                        }
                    }
                    g_mlModel.recordBaselineDuty(tid, curRun, nowBaseNs, curFreq, maxFreq);
                }
            }
            }
        }

        {
            long long nowHybridMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (loopCfg.mlHybridEnabled && (nowHybridMs - g_hybridLastTickMs) >= 500) {
                g_hybridLastTickMs = nowHybridMs;
                std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snapshots;
                struct ThreadRef {
                    pid_t pid; int tid; int cpu; int role;
                    int priority; int nice; uint64_t cachedRunTime;
                    uint64_t nrSwitches;
                    bool readFresh;
                };
                std::vector<ThreadRef> refs;
                {
                    std::lock_guard<std::mutex> tlock(g_threadsMutex);
                    refs.reserve(g_threads.size());
                    const int kHybridMaxNonCriticalSamples = 80;
                    int sampledNonCritical = 0;
                    for (const auto&_sb_item : g_threads) {
                        const auto& tid = _sb_item.first; const auto& ti = _sb_item.second;
                        if (ti.currentCpu < 0) continue;
                        bool isCriticalRole = (ti.role == ROLE_S1_UI ||
                                               ti.role == ROLE_S2_RENDER ||
                                               ti.role == ROLE_S3_GAME);
                        bool readFresh = isCriticalRole ||
                                         (sampledNonCritical < kHybridMaxNonCriticalSamples);
                        if (!isCriticalRole) sampledNonCritical++;
                        refs.push_back({ti.pid, tid, ti.currentCpu, (int)ti.role,
                                        ti.priority, ti.nice, ti.runTime, ti.nrSwitches, readFresh});
                    }
                }
                uint64_t nowNs = (uint64_t)nowHybridMs * 1000000ULL;
                snapshots.reserve(refs.size());
                std::vector<int> cpuToPoolSnap = g_mlModel.getCpuPoolAssignment();
                for (const auto& r : refs) {
                    uint64_t freshRunTime = r.cachedRunTime;
                    if (r.readFresh) {
                        std::ifstream sf("/proc/" + std::to_string(r.pid) + "/task/" + std::to_string(r.tid) + "/schedstat");
                        if (sf) {
                            unsigned long long rt = 0, wt = 0;
                            unsigned long ns = 0;
                            sf >> rt >> wt >> ns;
                            if (rt > 0) freshRunTime = rt;
                        }
                    }
                    aatpe_ml_hybrid::ThreadCpuSnapshot snap;
                    snap.tid = r.tid;
                    snap.cpu = r.cpu;
                    int sp = (r.cpu >= 0 && r.cpu < (int)cpuToPoolSnap.size()) ? cpuToPoolSnap[r.cpu] : -1;
                    snap.pool = (sp >= 0) ? sp : (int)getPoolForCpu(r.cpu);
                    snap.runTimeNs = freshRunTime;
                    snap.timestamp = nowNs;
                    snap.priority = r.priority;
                    snap.nice     = r.nice;
                    snap.nrSwitches = r.nrSwitches;
                    snap.role     = r.role;
                    snapshots.push_back(snap);
                    g_threadCpuAggregator.update(r.tid, r.cpu, freshRunTime, nowNs);
                }
                std::vector<uint64_t> cpuRuntime;
                static uint64_t s_lastEbpfTotalEvents = 0;
                uint64_t ebpfEventsThisTick = 0;
                if (g_ebpfActive.load(std::memory_order_acquire)) {
                    std::vector<uint64_t> counts;
                    if (g_ebpfScheduler.readEmbeddedCpuCounts(counts)) {
                        cpuRuntime = counts;
                        uint64_t totalNow = 0;
                        for (uint64_t c : counts) totalNow += c;
                        if (s_lastEbpfTotalEvents > 0 && totalNow >= s_lastEbpfTotalEvents) {
                            ebpfEventsThisTick = totalNow - s_lastEbpfTotalEvents;
                        }
                        s_lastEbpfTotalEvents = totalNow;
                    }
                }
                if (cpuRuntime.empty()) {
                    std::vector<aatpe_ebpf::CpuSchedStats> stats;
                    if (aatpe_ebpf::EbpfScheduler::readFallbackSchedstat(stats)) {
                        cpuRuntime.reserve(stats.size());
                        for (const auto& s : stats) cpuRuntime.push_back(s.totalRuntimeNs);
                    }
                }
                int cpuCount = (int)cpuRuntime.size();
                if (cpuCount == 0) cpuCount = (int)g_allCpus.size();
                std::vector<float> cpuThermalNorm(cpuCount, 0.5f);
                {
                    std::lock_guard<std::mutex> tlock(g_cpuMetricsMutex);
                    for (int c = 0; c < cpuCount; c++) {
                        auto it = g_cpuMetrics.find(c);
                        if (it != g_cpuMetrics.end() && it->second.temp > 0) {
                            cpuThermalNorm[c] = std::min(1.0f, std::max(0.0f,
                                (float)it->second.temp / 120.0f));
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> hlock(g_hybridMutex);
                    aatpe_ml_hybrid::instance().tick(snapshots, cpuRuntime, cpuCount,
                                                      cpuThermalNorm);
                }
                g_hybridEbpfSamplesTotal += ebpfEventsThisTick;
                static int s_hybridTickCounter = 0;
                if (++s_hybridTickCounter % 2 == 0) {
                    std::lock_guard<std::mutex> hlock(g_hybridMutex);
                    writeFile((std::string(STATE_DIR) + "/ml_hybrid_updates").c_str(),
                              (int)aatpe_ml_hybrid::instance().getTotalUpdates());
                    writeFile((std::string(STATE_DIR) + "/ml_hybrid_ebpf_samples").c_str(),
                              (int)g_hybridEbpfSamplesTotal.load());
                }
            }
        }

        int decisionCount = 0;
        struct MigrationResult { int tid; int pid; int targetCpu; bool placeOk; unsigned long long starttime; };
        std::vector<MigrationResult> migrationResults;

        long long nowDecisionMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            static long long s_lastRebalanceMs = 0;
            static int s_lastRebalanceTemp = -1;
            static int s_lastRebalanceLoad = -1;
            bool forceRebalance = false;
            if (nowDecisionMs - s_lastRebalanceMs >= kRebalanceMs) {
                forceRebalance = true;
                s_lastRebalanceMs = nowDecisionMs;
            }
            int dirtyCount = 0;
            {
                std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                for (const auto& kv : g_scoreCache) {
                    if (kv.second.dirty) dirtyCount++;
                }
            }
            if (dirtyCount >= kDirtyRebalanceThreshold) {
                forceRebalance = true;
                s_lastRebalanceMs = nowDecisionMs;
            }
            int curTemp = g_thermalCpu.load(std::memory_order_relaxed);
            int curLoad = systemLoadNow;
            if (s_lastRebalanceTemp >= 0 && abs(curTemp - s_lastRebalanceTemp) >= 5) forceRebalance = true;
            if (s_lastRebalanceLoad  >= 0 && abs(curLoad  - s_lastRebalanceLoad)  >= 20) forceRebalance = true;
            s_lastRebalanceTemp = curTemp;
            s_lastRebalanceLoad  = curLoad;
            if (forceRebalance) {
                invalidateAllScores();
                {
                    std::lock_guard<std::mutex> tlock(g_threadsMutex);
                    std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                    for (auto it = g_scoreCache.begin(); it != g_scoreCache.end(); ) {
                        if (g_threads.find(it->first) == g_threads.end()) it = g_scoreCache.erase(it);
                        else ++it;
                    }
                }
            }
        }

        std::vector<std::pair<int, ThreadInfo>> threadsSnapshot;
        {
            std::lock_guard<std::mutex> tlock(g_threadsMutex);
            threadsSnapshot.reserve(g_threads.size());
            for (const auto& kv : g_threads) threadsSnapshot.push_back(kv);
        }
        bool jankActive = jankActiveNow;
        {
            static SystemState s_prevSysState = getSystemState();
            SystemState curSysState = getSystemState();
            if (curSysState != s_prevSysState) {
                bool enteredNormal =
                    (curSysState == SystemState::NORMAL) &&
                    (s_prevSysState == SystemState::EMERGENCY ||
                     s_prevSysState == SystemState::DEGRADED ||
                     s_prevSysState == SystemState::PASSIVE);
                if (enteredNormal) {
                    forceRescoreAllScores();
                    LOG_WARN("[RESCORE] state transition %s->NORMAL: force one-shot rescore of all tracked threads",
                             systemStateToString(s_prevSysState));
                }
                s_prevSysState = curSysState;
            }
        }
        bool emergencySystem = (getSystemState() == SystemState::EMERGENCY);
        bool lowOverhead = g_lowOverheadMode.load(std::memory_order_relaxed);
        static int s_emgPlaceCounter = 0;
        bool emgDoPlacement = true;
        if (emergencySystem && !lowOverhead) {
            s_emgPlaceCounter++;
            emgDoPlacement = (s_emgPlaceCounter % 5 == 0);
        }
        static int s_loPlaceCounter = 0;
        bool loDoPlacement = true;
        if (lowOverhead) {
            s_loPlaceCounter++;
            loDoPlacement = (s_loPlaceCounter % 10 == 0);
        }
        for (auto&_sb_item : threadsSnapshot) {
        auto& tid = _sb_item.first; auto& ti = _sb_item.second; (void)tid;
                if ((emergencySystem && !emgDoPlacement) || (lowOverhead && !loDoPlacement)) continue;
                if (ti.role == ROLE_S0_SYSTEM) continue;
                if (jankActive && ti.role > ROLE_S3_GAME) continue;
                {
                    std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                    auto& entry = g_scoreCache[tid];
                    bool ebpfLive = g_ebpfActive.load(std::memory_order_acquire);
                    if (!shouldRescoreThread(entry, ti, nowDecisionMs,
                                             kScoreTtlMs, kInactiveMs, ebpfLive)) continue;
                }
                int abGroup;
                int resolvedUid = resolveUidForPid(ti.pid);
                if (resolvedUid >= 0) {
                    abGroup = abTestAssignByUid(resolvedUid) + 1;
                } else {
                    abGroup = abTestAssignThread(tid) + 1;
                }
                Decision d = decidePlacement(ti, loopCfg);
                g_totalDecisions++;
                {
                    std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                    auto& entry = g_scoreCache[tid];
                    entry.lastDecision = d;
                    entry.cachedAtMs = nowDecisionMs;
                    if (entry.lastSwitchMs == 0) entry.lastSwitchMs = nowDecisionMs;
                }
                if (d.action == DecisionAction::MIGRATE) {
                    int fromCpu = ti.currentCpu;
                    bool b3Suppress = false;
                    if (loopCfg.mlEpsilonGreedy > 0.0) {
                        g_mlModel.setEpsilonGreedy(loopCfg.mlEpsilonGreedy);
                        g_mlModel.setExploreJankBlocked(aatpe::frameaware::jankBoostVoteActive());
                        b3Suppress = g_mlModel.maybeExplore(tid, d.confidence * 0.01f);
                    }
                    if (b3Suppress) {
                        {
                            std::lock_guard<std::mutex> mlLock(g_mlMutex);
                            g_mlModel.recordPendingDecision(tid, d.targetCpu, (int)d.targetPool,
                                                             d.mlFeatures, ti.runTime, ti.nrSwitches,
                                                             0, 0, time(nullptr), ti.starttime,
                                                             0.0f, 0.0f, 0);
                            g_mlModel.markPendingCounterfactual(tid);
                        }
                        recordWebuiDecision(tid, ti.tname, role_names::getName((int)ti.role),
                            fromCpu, d.targetCpu, d.score, d.confidence, "EXPLORE_CONTROL");
                        continue;
                    }
                    bool placeOk = applyPlacement(ti, d.targetCpu, d.reason, loopCfg);
                    if (placeOk && d.mlActive) {
                        g_mlPendingCreated++;
                        int beforeFreq = 0;
                        {
                            std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                            auto bfmit = g_cpuMetrics.find(ti.currentCpu);
                            if (bfmit != g_cpuMetrics.end()) beforeFreq = bfmit->second.curFreq;
                        }
                        int poolIdx = (int)d.targetPool;
                        if (poolIdx < 0) poolIdx = (int)getPoolForCpu(d.targetCpu);
                        unsigned long long beforeRunTimeFresh = ti.runTime;
                        {
                            std::ifstream bsf("/proc/" + std::to_string(ti.pid) + "/task/" + std::to_string(tid) + "/schedstat");
                            if (bsf) {
                                unsigned long long brt = 0, bwt = 0;
                                unsigned long bns = 0;
                                bsf >> brt >> bwt >> bns;
                                if (brt > 0) beforeRunTimeFresh = brt;
                            }
                        }
                        float ftBeforeP95 = 0.0f, ftBeforeAvg = 0.0f;
                        int ftBeforeSamples = 0;
                        if (loopCfg.mlFrameTimeOutcome &&
                            (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME)) {
                            FrameTimeOutcome ftSnap = getCurrentFrameTimeSnapshot();
                            if (ftSnap.sample_count > 0 && ftSnap.frame_time_p95 > 0.0f) {
                                ftBeforeP95 = ftSnap.frame_time_p95;
                                ftBeforeAvg = ftSnap.frame_time_avg;
                                ftBeforeSamples = ftSnap.sample_count;
                            }
                        }
                        int currentCpuUtil = 0;
                        {
                            std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                            auto uit = g_cpuMetrics.find(ti.currentCpu);
                            if (uit != g_cpuMetrics.end()) currentCpuUtil = uit->second.util;
                        }
                        std::lock_guard<std::mutex> mlLock(g_mlMutex);
                        g_mlModel.recordPendingDecision(ti.tid, d.targetCpu, poolIdx,
                                                         d.mlFeatures, beforeRunTimeFresh, ti.nrSwitches,
                                                         currentCpuUtil,
                                                         beforeFreq, time(nullptr), ti.starttime,
                                                         ftBeforeP95, ftBeforeAvg, ftBeforeSamples);
                    } else if (!placeOk) {
                        std::lock_guard<std::mutex> mlLock(g_mlMutex);
                        g_mlModel.clearPendingDecision(ti.tid);
                    }
                    recordWebuiDecision(tid, ti.tname, role_names::getName((int)ti.role),
                        fromCpu, d.targetCpu, d.score, d.confidence, placeOk ? "MIGRATE" : "MIGRATE_FAILED");
                    if (placeOk) {
                        recordMigrationCount(tid);
                        abTestRecordMigration(abGroup);
                    }
                    migrationResults.push_back({tid, ti.pid, d.targetCpu, placeOk, ti.starttime});
                    if (placeOk) decisionCount++;
                }
                if (loopCfg.controlMode == "passive" && loopCfg.mlShadowSampling && loopCfg.mlLearningEnabled
                    && d.action != DecisionAction::MIGRATE
                    && (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME)
                    && !ti.needsHydration) {
                    static unsigned long long s_lastShadowSampleMs = 0;
                    static int s_shadowBudgetLeft = 0;
                    unsigned long long nowShadowMs = (unsigned long long)std::chrono::duration_cast<
                        std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (nowShadowMs - s_lastShadowSampleMs >= 2000) {
                        s_lastShadowSampleMs = nowShadowMs;
                        s_shadowBudgetLeft = 2;
                    }
                    if (s_shadowBudgetLeft > 0) {
                        unsigned long long beforeRunTimeFresh = ti.runTime;
                        {
                            std::ifstream bsf("/proc/" + std::to_string(ti.pid) + "/task/" + std::to_string(tid) + "/schedstat");
                            if (bsf) {
                                unsigned long long brt = 0, bwt = 0; unsigned long bns = 0;
                                bsf >> brt >> bwt >> bns;
                                if (brt > 0) beforeRunTimeFresh = brt;
                            }
                        }
                        int shadowPool = (int)getPoolForCpu(ti.currentCpu);
                        aatpe_ml::ThreadFeatures shadowFeats = extractMlFeatures(ti, ti.currentCpu);
                        if (aatpe_ml::LearningModel::isAllZeroFeatures(shadowFeats)) {
                        } else {
                            int currentCpuUtil = 0; int beforeFreq = 0;
                            {
                                std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                                auto uit = g_cpuMetrics.find(ti.currentCpu);
                                if (uit != g_cpuMetrics.end()) {
                                    currentCpuUtil = uit->second.util;
                                    beforeFreq = uit->second.curFreq;
                                }
                            }
                            std::lock_guard<std::mutex> mlLock(g_mlMutex);
                            g_mlModel.recordPendingDecision(ti.tid, ti.currentCpu, shadowPool,
                                                             shadowFeats, beforeRunTimeFresh, ti.nrSwitches,
                                                             currentCpuUtil, beforeFreq, time(nullptr), ti.starttime,
                                                             0.0f, 0.0f, 0);
                            g_mlModel.markPendingShadow(ti.tid);
                            s_shadowBudgetLeft--;
                        }
                    }
                }
                if (decisionCount >= effectiveMaxMigrations) break;
            }
        for (const auto& mr : migrationResults) {
            bool tidStillValid = false;
            {
                std::lock_guard<std::mutex> tlock(g_threadsMutex);
                auto it = g_threads.find(mr.tid);
                tidStillValid = (it != g_threads.end() && it->second.starttime == mr.starttime);
            }
            if (!tidStillValid) {
                continue;
            }
            int currentCpu = readThreadCurrentCpu(mr.pid, mr.tid);
            bool samePool = (getPoolForCpu(currentCpu) == getPoolForCpu(mr.targetCpu))
                          && (getPoolForCpu(currentCpu) != CpuPool::UNKNOWN);
            bool good = mr.placeOk && (samePool || currentCpu == mr.targetCpu);
            recordPlacementResult(mr.tid, mr.targetCpu, good);
        }
        protectCriticalThreads();

        time_t now2 = time(nullptr);
        if (loopCfg.mlLearningEnabled && (now2 - g_mlLastSave) >= loopCfg.mlSaveInterval) {
            const char* mp = mlModelPath();
            bool savedOk = false;
            int savedSamples = 0; double savedAlpha = 0.0;
            {
                std::lock_guard<std::mutex> mlLock(g_mlMutex);
                savedOk = g_mlModel.save(mp);
                savedSamples = g_mlModel.getTotalSamples();
                savedAlpha = g_mlModel.getAlpha();
            }
            if (savedOk) {
                g_mlSaveSuccess++;
                backupModelFile(mp);
                if (loopCfg.mlTopologyBindingEnabled) {
                    aatpe_ml::saveTopologySidecar(mp, aatpe_ml::computeDeviceTopologyHash());
                }
                struct stat st;
                long long fsize = -1;
                if (stat(mp, &st) == 0) fsize = (long long)st.st_size;
                LOG_INFO("ML model saved: path=%s bytes=%lld samples=%d alpha=%.4f",
                         mp, fsize, savedSamples, savedAlpha);
                updateMlSaveStateFiles(true, mp);
            } else {
                g_mlSaveFail++;
                LOG_WARN("ML model save FAILED: path=%s errno=%d (%s)",
                         mp, errno, aatpeErrStr(errno));
                updateMlSaveStateFiles(false, mp);
            }
            g_mlLastSave = now2;
        }
        {
            static time_t s_lastAbWinnerCheck = 0;
            if (now2 - s_lastAbWinnerCheck >= 60) {
                int winner = abTestDetermineWinner();
                if (winner >= 0) {
                    writeFile((std::string(STATE_DIR) + "/ab_test_winner").c_str(), winner);
                    LOG_INFO("A/B test winner: group %d (0=A, 1=B)", winner);
                }
                {
                    std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
                    g_abGroupAAvg = g_abStats.avgFrameTimeA;
                    g_abGroupBAvg = g_abStats.avgFrameTimeB;
                    g_abPValue = (winner >= 0) ? 0.0f : 1.0f;
                }
                s_lastAbWinnerCheck = now2;
            }
        }
        loopCount++;
        if (loopCount >= daemon::kHistoryDecayLoopCount) {
            decayHistory();
            loopCount = 0;
        }
        if (diagTick) {
        writeFile((std::string(STATE_DIR) + "/decision_count_current").c_str(), decisionCount);
        writeFile((std::string(STATE_DIR) + "/migration_rate_current").c_str(), g_migrationCount);
        writeFile((std::string(STATE_DIR) + "/loop_count").c_str(), loopCount);
        writeFile((std::string(STATE_DIR) + "/high_load_cycles").c_str(), (int)g_highLoadCycles.load());
        writeFile((std::string(STATE_DIR) + "/keep_protected").c_str(), (int)g_keepProtected.load());
        writeFile((std::string(STATE_DIR) + "/keep_passive").c_str(), (int)g_keepPassive.load());
        writeFile((std::string(STATE_DIR) + "/keep_no_cpu").c_str(), (int)g_keepNoCpu.load());
        writeFile((std::string(STATE_DIR) + "/keep_low_conf").c_str(), (int)g_keepLowConf.load());
        writeFile((std::string(STATE_DIR) + "/keep_high_cost").c_str(), (int)g_keepHighCost.load());
        writeFile((std::string(STATE_DIR) + "/keep_already_optimal").c_str(), (int)g_keepAlreadyOptimal.load());
        {
            long long rj_no_gain       = g_keepHighCost.load();
            long long rj_low_conf      = g_keepLowConf.load();
            long long rj_thermal      = g_keepThermalEmergency.load();
            long long rj_cpu_offline   = g_keepNoCpu.load();
            long long rj_protected     = g_keepProtected.load();
            long long rj_safe_mode     = g_keepSafeMode.load();
            long long rj_hysteresis    = g_settleHysteresisRejects.load();
            long long rj_cooldown      = g_settleCooldownRejects.load() + g_applyCooldownSkip.load();
            long long rj_cpusets_empty = g_applyCpusetSkip.load();
            long long rj_rate_limit    = g_applyRateLimited.load();
            long long rj_total = rj_no_gain + rj_low_conf + rj_thermal + rj_cpu_offline
                               + rj_protected + rj_safe_mode + rj_hysteresis + rj_cooldown
                               + rj_cpusets_empty + rj_rate_limit;
            writeFile((std::string(STATE_DIR) + "/migrate_reject_no_gain").c_str(), (int)rj_no_gain);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_low_confidence").c_str(), (int)rj_low_conf);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_thermal").c_str(), (int)rj_thermal);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_cpu_offline").c_str(), (int)rj_cpu_offline);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_thread_protected").c_str(), (int)rj_protected);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_safe_mode").c_str(), (int)rj_safe_mode);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_hysteresis").c_str(), (int)rj_hysteresis);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_cooldown").c_str(), (int)rj_cooldown);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_cpusets_empty").c_str(), (int)rj_cpusets_empty);
            writeFile((std::string(STATE_DIR) + "/migrate_reject_rate_limit").c_str(), (int)rj_rate_limit);
            writeFile((std::string(STATE_DIR) + "/migration_rejected").c_str(), (int)rj_total);
            {
                std::lock_guard<std::mutex> lk(g_lastCpusetSkipMutex);
                char samp[256];
                snprintf(samp, sizeof(samp), "{\"tid\":%lld,\"pid\":%d,\"targetCpu\":%d,\"maskCount\":%d,\"pname\":\"%s\"}",
                         (long long)g_lastCpusetSkip.tid, g_lastCpusetSkip.pid,
                         g_lastCpusetSkip.targetCpu, g_lastCpusetSkip.maskCount,
                         g_lastCpusetSkip.pname);
                writeFile((std::string(STATE_DIR) + "/apply_cpuset_skip_sample").c_str(), samp);
            }
        }
        writeFile((std::string(STATE_DIR) + "/migrate_actual").c_str(), (int)g_migrateActual.load());
        writeFile((std::string(STATE_DIR) + "/apply_success").c_str(), (int)g_applySuccess.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed").c_str(), (int)g_applyFailed.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_einval").c_str(), (int)g_applyFailedEinval.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_esrch").c_str(), (int)g_applyFailedEsrch.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_eperm").c_str(), (int)g_applyFailedEperm.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_other").c_str(), (int)g_applyFailedOther.load());
        {
            std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
            std::string topoJson = "{\"cpus\":" + std::to_string(g_cpuCount) +
                ",\"generation\":" + std::to_string(g_topologyGeneration.load()) +
                ",\"little\":[";
            for (size_t i = 0; i < g_littlePool.size(); i++) { if (i) topoJson += ","; topoJson += std::to_string(g_littlePool[i]); }
            topoJson += "],\"mid\":[";
            for (size_t i = 0; i < g_midPool.size(); i++) { if (i) topoJson += ","; topoJson += std::to_string(g_midPool[i]); }
            topoJson += "],\"big\":[";
            for (size_t i = 0; i < g_bigPool.size(); i++) { if (i) topoJson += ","; topoJson += std::to_string(g_bigPool[i]); }
            std::vector<int> primeSnap;
            for (const auto& cl : clusters()) {
                if (cl.tier == CpuTier::PRIME) primeSnap = cl.cpus;
            }
            topoJson += "],\"prime\":[";
            for (size_t i = 0; i < primeSnap.size(); i++) { if (i) topoJson += ","; topoJson += std::to_string(primeSnap[i]); }
            topoJson += "]}";
            writeFile((std::string(STATE_DIR) + "/cpu_topology").c_str(), topoJson);
        }
        writeFile((std::string(STATE_DIR) + "/prime_spillover_count").c_str(),
                  (long long)aatpe::placement::spilloverPromotionCount());
        writeFile((std::string(STATE_DIR) + "/prime_hosted_threads").c_str(),
                  aatpe::placement::spilloverActiveCount());
        writeFile((std::string(STATE_DIR) + "/apply_rate_limited").c_str(), (int)g_applyRateLimited.load());
        writeFile((std::string(STATE_DIR) + "/apply_passive").c_str(), (int)g_applyPassive.load());
        writeFile((std::string(STATE_DIR) + "/apply_cooldown_skip").c_str(), (int)g_applyCooldownSkip.load());
        writeFile((std::string(STATE_DIR) + "/apply_cpuset_skip").c_str(), (int)g_applyCpusetSkip.load());
        writeFile((std::string(STATE_DIR) + "/apply_interval_skip").c_str(), (int)g_applyIntervalSkip.load());
        }
        if (g_reloadConfigRequested.load()) {
            g_reloadConfigRequested.store(false);
            reloadConfig();
        }
        reloadConfigIfChanged();

        if (diagTick) {
        writeFile((std::string(STATE_DIR) + "/apply_kernel_skip").c_str(), (int)g_applyKernelSkip.load());
        writeFile((std::string(STATE_DIR) + "/apply_unmigratable_skip").c_str(), (int)g_applyUnmigratableSkip.load());
        {
            std::lock_guard<std::mutex> lock(g_unmigratableMutex);
            writeFile((std::string(STATE_DIR) + "/unmigratable_count").c_str(), (int)g_unmigratableThreads.size());
        }
        }

        if (diagTick) {
            using namespace aatpe::subsys;
            selfobs::sampleSelf();
            bool throttled = selfobs::evaluateThrottle();
            if (throttled) safemode::trigger(safemode::SELF_OVERLOAD, "self cpu over budget");
            if (g_thermalEmergency.load()) {
                safemode::trigger(safemode::THERMAL_EMERG, "thermal emergency");
            } else {
                safemode::clearReason(safemode::THERMAL_EMERG);
            }
            {
                int cl = readInt((std::string(STATE_DIR) + "/crash_loop_detected").c_str(), 0);
                if (cl == 1) safemode::trigger(safemode::CRASH_LOOP, "watchdog crash loop");
            }
            safemode::maybeRecover(std::time(nullptr));
            Registry::instance().writeStateFile(STATE_DIR);
            safemode::writeStateFiles(STATE_DIR);
            selfobs::writeStateFiles(STATE_DIR);
            (void)throttled;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loopStart).count();
        std::string fgApp = getCachedForegroundApp();
        bool hasFg = !fgApp.empty();
        bool jankNow = jankActiveNow;
        int sysLoadInt = systemLoadNow;
        float sysLoad = (float)sysLoadInt;
        int baseInterval = computeAdaptiveInterval(sysLoad, jankNow, hasFg, loopCfg.decisionInterval);
        if (g_currentAppConfig.valid && g_currentAppConfig.decisionIntervalMs > 0) {
            baseInterval = g_currentAppConfig.decisionIntervalMs;
        }
        {
            SystemState cadenceSt = getSystemState();
            double ld1 = 0.0;
            getloadavg(&ld1, 1);
            int numCores = g_cpuCount > 0 ? g_cpuCount : 8;
            bool hostOverloaded = (ld1 > (double)numCores * 3.0);
            bool lowOH = (cadenceSt == SystemState::EMERGENCY);
            g_lowOverheadMode.store(lowOH, std::memory_order_relaxed);
            if (cadenceSt == SystemState::EMERGENCY) {
                baseInterval = 80;
            } else if (hostOverloaded) {
                if (baseInterval < 60) baseInterval = 60;
            } else if (cadenceSt == SystemState::DEGRADED) {
                if (baseInterval < 10) baseInterval = 10;
            }
        }
        if (isGpuBottleneck(sysLoad)) {
            int gpuInterval = getGpuBottleneckDecisionInterval();
            if (gpuInterval > baseInterval) baseInterval = gpuInterval;
        }
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_runtimeDecisionIntervalMs = baseInterval;
        }
        int sleepMs = baseInterval - (int)elapsed;
        if (sleepMs < daemon::kMinSleepMs) {
            if (elapsed >= baseInterval / 2) {
                SystemState restSt = getSystemState();
                int minRestMs = (restSt == SystemState::EMERGENCY) ? 200 :
                                (restSt == SystemState::DEGRADED)  ? 150 : 50;
                sleepMs = minRestMs;
            } else {
                sleepMs = daemon::kMinSleepMs;
            }
        }

        if (diagTick) {
            writeFile((std::string(STATE_DIR) + "/ebpf_active").c_str(), g_ebpfActive.load(std::memory_order_acquire) ? "1" : "0");
            std::string ebpfLoaderSnap;
            {
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                ebpfLoaderSnap = g_ebpfLoader;
            }
            writeFile((std::string(STATE_DIR) + "/ebpf_loader").c_str(), ebpfLoaderSnap.c_str());
            writeFile((std::string(STATE_DIR) + "/ebpf_state").c_str(),
                      g_ebpfScheduler.getEbpfStateSevenString());
            writeFile((std::string(STATE_DIR) + "/ebpf_state_detailed").c_str(),
                      g_ebpfScheduler.getDetailedStateString());
            {
                std::string st = g_ebpfScheduler.getStateString();
                long long lost = g_ebpfScheduler.getWindowLost();
                std::string reason = "NONE";
                if (st == "ACTIVE" || st == "ACTIVE_PARTIAL") {
                    reason = (lost > 0) ? "RING_OVERFLOW" : "NONE";
                } else if (st == "DEGRADED") {
                    reason = "TRACEPOINT_MISSING";
                } else if (st == "LOAD_FAILED" || st == "UNINITIALIZED" || st == "LOAD_TIMEOUT") {
                    reason = "MAP_CREATE_FAILED";
                }
                writeFile((std::string(STATE_DIR) + "/ebpf_degraded_reason").c_str(), reason.c_str());
            }
            writeFile((std::string(STATE_DIR) + "/ebpf_ring_pages").c_str(),
                      g_ebpfScheduler.getCurrentRingPages());
            {
                std::string ebpfModeSnap;
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                ebpfModeSnap = g_ebpfMode;
                writeFile((std::string(STATE_DIR) + "/ebpf_mode").c_str(), ebpfModeSnap.c_str());
            }
            writeFile((std::string(STATE_DIR) + "/ebpf_events_total").c_str(),
                      (long long)g_ebpfScheduler.getTotalEvents());
            writeFile((std::string(STATE_DIR) + "/ebpf_events_fork").c_str(),
                      (long long)g_ebpfScheduler.getForkEvents());
            writeFile((std::string(STATE_DIR) + "/ebpf_events_exit").c_str(),
                      (long long)g_ebpfScheduler.getExitEvents());
            writeFile((std::string(STATE_DIR) + "/ebpf_events_switch").c_str(),
                      (long long)g_ebpfScheduler.getSwitchEvents());
            writeFile((std::string(STATE_DIR) + "/ebpf_ring_lost").c_str(),
                      (long long)g_ebpfScheduler.getWindowLost());
            writeFile((std::string(STATE_DIR) + "/ebpf_window_events").c_str(),
                      (long long)g_ebpfScheduler.getWindowEvents());
            {
                char rateBuf[32];
                snprintf(rateBuf, sizeof(rateBuf), "%.1f", g_ebpfScheduler.getEventRatePerSec());
                writeFile((std::string(STATE_DIR) + "/ebpf_event_rate").c_str(), rateBuf);
            }
            writeFile((std::string(STATE_DIR) + "/ebpf_queue_dropped").c_str(),
                      (long long)g_ebpfScheduler.getQueueDropped());
            writeFile((std::string(STATE_DIR) + "/ebpf_queue_size").c_str(),
                      (long long)g_ebpfScheduler.getQueueSize());
            {
                auto st = g_ebpfScheduler.getEventStats();
                writeFile((std::string(STATE_DIR) + "/ebpf_fallback_count").c_str(),
                          (long long)st.fallbackCount);
                writeFile((std::string(STATE_DIR) + "/ebpf_ring_overflow_cumulative").c_str(),
                          (long long)st.ringOverflow);
                writeFile((std::string(STATE_DIR) + "/ebpf_events_exec").c_str(),
                          (long long)g_ebpfScheduler.getExecEvents());
            }
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_filtered").c_str(),
                      (long long)g_ebpfScheduler.getOffsetFiltered());
            {
                std::lock_guard<std::mutex> hmacLk(g_hmacMutex);
                writeFile((std::string(STATE_DIR) + "/hmac_active").c_str(),
                          g_hmacActive ? "1" : "0");
                writeFile((std::string(STATE_DIR) + "/hmac_key_status").c_str(),
                          g_hmacKeyStatus);
            }
            writeFile((std::string(STATE_DIR) + "/hmac_session_active").c_str(),
                      aatpeSessionAnyActive() ? "1" : "0");
            {
                float gpuUtilSnap = g_gpuUtilization.load(std::memory_order_relaxed);
                const char* gpuTypeSnap = g_gpuType.load(std::memory_order_relaxed);
                writeFile((std::string(STATE_DIR) + "/gpu_utilization").c_str(),
                          (long long)gpuUtilSnap);
                writeFile((std::string(STATE_DIR) + "/gpu_type").c_str(),
                          gpuTypeSnap ? gpuTypeSnap : "unknown");
            }
            {
                bool audioModeSnap;
                int sceneSnap;
                {
                    std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
                    audioModeSnap = g_audioState.modeActive;
                    sceneSnap     = g_sceneState.currentScene;
                }
                writeFile((std::string(STATE_DIR) + "/audio_mode_active").c_str(),
                          audioModeSnap ? "1" : "0");
                writeFile((std::string(STATE_DIR) + "/current_game_scene").c_str(),
                          sceneToString((GameScene)sceneSnap));
            }
            writeFile((std::string(STATE_DIR) + "/ab_test_group_a_avg").c_str(), (int)g_abStats.avgFrameTimeA);
            writeFile((std::string(STATE_DIR) + "/ab_test_group_b_avg").c_str(), (int)g_abStats.avgFrameTimeB);
            writeFile((std::string(STATE_DIR) + "/ab_test_group_a_samples").c_str(), g_abStats.sampleCountA);
            writeFile((std::string(STATE_DIR) + "/ab_test_group_b_samples").c_str(), g_abStats.sampleCountB);
            writeFile((std::string(STATE_DIR) + "/battery_aging_enabled").c_str(), loopCfg.batteryAgingProtection ? "1" : "0");
            writeFile((std::string(STATE_DIR) + "/battery_aged").c_str(), g_batteryAged ? "1" : "0");
            writeFile((std::string(STATE_DIR) + "/battery_health").c_str(), g_batteryHealthPercent);
            writeFile((std::string(STATE_DIR) + "/battery_cycle_count").c_str(), g_batteryCycleCount);
            writeFile((std::string(STATE_DIR) + "/ml_cold_start_enabled").c_str(), loopCfg.mlColdStartProtection ? "1" : "0");
        }

        if (loopCfg.federatedEnabled) {
            long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs - g_lastFederatedExportMs > 60000) {
                g_lastFederatedExportMs = nowMs;
                std::vector<float> weights;
                float alpha = 0.0f;
                int sampleCount = 0;
                {
                    std::lock_guard<std::mutex> mlLock(g_mlMutex);
                    weights = g_mlModel.getWeights();
                    alpha = g_mlModel.getAlpha();
                    sampleCount = g_mlModel.getTotalSamples();
                }
                if (!weights.empty() && sampleCount >= 20) {
                    g_federatedLearner.setLocalModel(weights, alpha, sampleCount);
                    g_federatedLearner.exportLocalModel();
                }
            }
            if (nowMs - g_lastFederatedImportMs > 120000) {
                g_lastFederatedImportMs = nowMs;
                g_federatedLearner.checkAndImportModels();
                g_federatedLearner.runAggregation();
                const auto& aggModel = g_federatedLearner.getAggregatedModel();
                if (!aggModel.empty()) {
                    std::lock_guard<std::mutex> mlLock(g_mlMutex);
                    g_mlModel.setWeights(aggModel);
                }
            }
            const auto& fedStats = g_federatedLearner.getStats();
            g_federatedRounds.store(fedStats.rounds, std::memory_order_relaxed);
            g_federatedExports.store(fedStats.exports, std::memory_order_relaxed);
            g_federatedImports.store(fedStats.imports, std::memory_order_relaxed);
            g_federatedTrustedImports = fedStats.trustedImports;
            if (diagTick) {
                writeFile((std::string(STATE_DIR) + "/federated_rounds").c_str(), fedStats.rounds);
                writeFile((std::string(STATE_DIR) + "/federated_exports").c_str(), fedStats.exports);
                writeFile((std::string(STATE_DIR) + "/federated_imports").c_str(), fedStats.imports);
                writeFile((std::string(STATE_DIR) + "/federated_trusted_imports").c_str(), (long long)fedStats.trustedImports);
                writeFile((std::string(STATE_DIR) + "/ab_test_p_value").c_str(), (float)g_abTestLatestP);
            }
            checkHmacKeyRotation();
        }

        auto loopEnd = std::chrono::steady_clock::now();
        long long loopUs = std::chrono::duration_cast<std::chrono::microseconds>(loopEnd - loopStart).count();
        traceUpdate(g_traceDaemonLoop, loopUs);

        g_stateGeneration.fetch_add(1, std::memory_order_release);
        if (loopCount % 10 == 0) {
            writeStateJson();
        }

        {
            if (g_shouldExit.load(std::memory_order_relaxed)) break;
            const int idleIntervalMs = loopCfg.decisionIdleIntervalMs;
            const int minIntervalMs  = loopCfg.decisionMinIntervalMs;
            int pollTimeoutMs = sleepMs;

            {
                static int s_tickMode = 0;
                size_t liveThreadCount;
                {
                    std::lock_guard<std::mutex> tlock(g_threadsMutex);
                    liveThreadCount = g_threads.size();
                }
                bool isIdle = (liveThreadCount < daemon::kIdleTickMaxTrackedThreads) &&
                              !hasFg;
                if (isIdle) {
                    pollTimeoutMs = std::max(pollTimeoutMs, daemon::kIdleTickPollTimeoutMs);
                    s_tickMode = 1;
                } else {
                    s_tickMode = 0;
                }
                static time_t s_lastTickModeWrite = 0;
                time_t nowTickMode = time(nullptr);
                if (nowTickMode - s_lastTickModeWrite >= 1) {
                    writeFile((std::string(STATE_DIR) + "/tick_mode").c_str(),
                              s_tickMode == 0 ? "active" : "idle");
                    s_lastTickModeWrite = nowTickMode;
                }
            }

            if (pollTimeoutMs > daemon::kIdleTickPollTimeoutMs) pollTimeoutMs = daemon::kIdleTickPollTimeoutMs;
            if (pollTimeoutMs < daemon::kMinSleepMs) pollTimeoutMs = daemon::kMinSleepMs;

            bool skipSleep = g_skipNextSleep.exchange(false);
            if (skipSleep) {
                static long long s_lastWakeLogMs = 0;
                long long nowWakeLogMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (s_lastWakeLogMs == 0 || (nowWakeLogMs - s_lastWakeLogMs) >= 1000) {
                    s_lastWakeLogMs = nowWakeLogMs;
                    LOG_INFO("EVENT: waking for immediate full decision (medium-priority event)");
                }
                pollTimeoutMs = daemon::kMinSleepMs;
            } else {
                if (pollTimeoutMs > idleIntervalMs) pollTimeoutMs = idleIntervalMs;
            }

            bool wokenByEvent = false;

            if (g_lowOverheadMode.load(std::memory_order_relaxed)) {
                if (g_wakeupPipeReadFd >= 0) {
                    char drain[16];
                    while (read(g_wakeupPipeReadFd, drain, sizeof(drain)) > 0 || errno == EINTR) {}
                }
                if (g_controlInotifyFd >= 0) {
                    char evbuf[256];
                    while (read(g_controlInotifyFd, evbuf, sizeof(evbuf)) > 0 || errno == EINTR) {}
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                continue;
            }

            if (g_wakeupPipeReadFd >= 0) {
                struct pollfd pfds[2];
                int npfd = 0;
                pfds[npfd].fd = g_wakeupPipeReadFd;
                pfds[npfd].events = POLLIN;
                pfds[npfd].revents = 0;
                npfd++;
                if (g_controlInotifyFd >= 0) {
                    pfds[npfd].fd = g_controlInotifyFd;
                    pfds[npfd].events = POLLIN;
                    pfds[npfd].revents = 0;
                    npfd++;
                }
                int pret = poll(pfds, npfd, pollTimeoutMs);
                if (pret < 0) {
                    if (errno != EINTR) {
                        static std::atomic<int> s_pollErrLogged{0};
                        if (s_pollErrLogged.exchange(1, std::memory_order_relaxed) == 0) {
                            LOG_WARN("daemonLoop: wakeup-pipe poll error (%s) — continuing on timeout cadence",
                                     aatpeErrStr(errno));
                        }
                    }
                } else if (pret > 0) {
                    wokenByEvent = true;
                    if (pfds[0].revents & POLLIN) {
                        char drain[16];
                        while (read(g_wakeupPipeReadFd, drain, sizeof(drain)) > 0 || errno == EINTR) {}
                    }
                    if (npfd > 1 && (pfds[1].revents & POLLIN)) {
                        char evbuf[512];
                        bool controlEvent = false;
                        ssize_t n;
                        while ((n = read(g_controlInotifyFd, evbuf, sizeof(evbuf))) > 0 ||
                               (n < 0 && errno == EINTR)) {
                            size_t off = 0;
                            while (off + sizeof(struct inotify_event) <= (size_t)n) {
                                struct inotify_event* ev =
                                    (struct inotify_event*)(evbuf + off);
                                if (ev->len > 0) {
                                    std::string ename(ev->name, strnlen(ev->name, ev->len));
                                    if (ename == "control" || ename == "control_cmd") {
                                        controlEvent = true;
                                    }
                                }
                                off += sizeof(struct inotify_event) + ev->len;
                            }
                        }
                        if (controlEvent) {
                            wokenByEvent = true;
                        }
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(pollTimeoutMs));
            }

            if (wokenByEvent && !g_shouldExit.load(std::memory_order_relaxed)) {
                auto nowAfterPoll = std::chrono::steady_clock::now();
                long long sinceLastMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowAfterPoll - g_lastDecisionSteady).count();
                if (sinceLastMs < minIntervalMs) {
                    long long remainMs = minIntervalMs - sinceLastMs;
                    std::this_thread::sleep_for(std::chrono::milliseconds(remainMs));
                }
            }
        }
    }
    stopConfigHotReload();
    {
        Config exitCfg; snapshotConfig(exitCfg);
        if (exitCfg.mlLearningEnabled) {
        std::lock_guard<std::mutex> mlLock(g_mlMutex);
        const char* mp = mlModelPath();
        if (g_mlModel.save(mp)) {
            g_mlSaveSuccess++;
            backupModelFile(mp);
            struct stat st;
            long long fsize = -1;
            if (stat(mp, &st) == 0) fsize = (long long)st.st_size;
            LOG_INFO("ML model saved on exit: path=%s bytes=%lld samples=%d",
                     mp, fsize, g_mlModel.getTotalSamples());
            updateMlSaveStateFiles(true, mp);
        } else {
            g_mlSaveFail++;
            LOG_WARN("ML model save on exit FAILED: path=%s errno=%d (%s)",
                     mp, errno, aatpeErrStr(errno));
            updateMlSaveStateFiles(false, mp);
        }
        }
    }
    {
        std::lock_guard<std::mutex> hlock(g_hybridMutex);
        const char* mp = mlModelPath();
        std::string hybridPath = std::string(mp) + ".hybrid";
        if (aatpe_ml_hybrid::instance().save(hybridPath.c_str())) {
            LOG_INFO("[ML-HYBRID] saved on exit: path=%s", hybridPath.c_str());
        } else {
            LOG_WARN("[ML-HYBRID] save on exit FAILED: path=%s errno=%d (%s)",
                     hybridPath.c_str(), errno, aatpeErrStr(errno));
        }
    }
    LOG_INFO("Daemon stopped");
}

void cmdStatus() {
    printf("AATPE %s - Adaptive Android Task Placement Engine\n\n", AATPE_VERSION);
    printf("Mode:        %s\n", readFile((std::string(STATE_DIR) + "/control_mode").c_str()).c_str());
    printf("Profile:     %s\n", readFile((std::string(STATE_DIR) + "/current_profile").c_str()).c_str());
    printf("Daemon PID:  %d\n", readInt(PID_FILE, 0));
    printf("Uptime:      %ds\n", readInt((std::string(STATE_DIR) + "/daemon_uptime").c_str(), 0));
    printf("\nThermal:\n");
    printf("  CPU Max:   %dC\n", readInt((std::string(STATE_DIR) + "/thermal_cpu_max").c_str(), 0));
    printf("  Skin:      %dC\n", readInt((std::string(STATE_DIR) + "/thermal_skin").c_str(), 0));
    printf("  Throttle:  %d\n", readInt((std::string(STATE_DIR) + "/thermal_throttle").c_str(), 0));
    printf("\nFrame:\n");
    printf("  SF Latency: %dus\n", readInt((std::string(STATE_DIR) + "/sf_latency_avg").c_str(), 0));
    printf("  Layers:     %d\n", readInt((std::string(STATE_DIR) + "/layer_count").c_str(), 0));
    printf("  VSync:      %dHz\n", readInt((std::string(STATE_DIR) + "/vsync_rate").c_str(), 60));
    printf("\nMetrics:\n");
    printf("  Threads:    %d\n", readInt((std::string(STATE_DIR) + "/tracked_threads").c_str(), 0));
    printf("  Decisions:  %d\n", readInt((std::string(STATE_DIR) + "/decision_count").c_str(), 0));
    printf("  Migrations: %d\n", readInt((std::string(STATE_DIR) + "/migration_count").c_str(), 0));
    printf("\nKEEP Breakdown:\n");
    printf("  Passive:        %d\n", readInt((std::string(STATE_DIR) + "/keep_passive").c_str(), 0));
    printf("  Protected:      %d\n", readInt((std::string(STATE_DIR) + "/keep_protected").c_str(), 0));
    printf("  Low Confidence: %d\n", readInt((std::string(STATE_DIR) + "/keep_low_conf").c_str(), 0));
    printf("  High Cost:      %d\n", readInt((std::string(STATE_DIR) + "/keep_high_cost").c_str(), 0));
    printf("  Already Optimal:%d\n", readInt((std::string(STATE_DIR) + "/keep_already_optimal").c_str(), 0));
    printf("  No CPU:         %d\n", readInt((std::string(STATE_DIR) + "/keep_no_cpu").c_str(), 0));
    printf("  Actual Migrate: %d\n", readInt((std::string(STATE_DIR) + "/migrate_actual").c_str(), 0));
    printf("\nApply Placement:\n");
    printf("  Success:        %d\n", readInt((std::string(STATE_DIR) + "/apply_success").c_str(), 0));
    printf("  Failed:         %d\n", readInt((std::string(STATE_DIR) + "/apply_failed").c_str(), 0));
    printf("  Rate Limited:   %d\n", readInt((std::string(STATE_DIR) + "/apply_rate_limited").c_str(), 0));
    printf("  Passive Block:  %d\n", readInt((std::string(STATE_DIR) + "/apply_passive").c_str(), 0));
    printf("  Cooldown Skip:  %d\n", readInt((std::string(STATE_DIR) + "/apply_cooldown_skip").c_str(), 0));
    printf("  Cpuset Skip:    %d\n", readInt((std::string(STATE_DIR) + "/apply_cpuset_skip").c_str(), 0));
    printf("\nThermal Emergency: %d\n", readInt((std::string(STATE_DIR) + "/thermal_emergency").c_str(), 0));
}

void cmdThreads() {
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    printf("=== THREAD DATABASE (%zu threads) ===\n", g_threads.size());
    int count = 0;
    for (auto&_sb_item : g_threads) {
        auto& ti = _sb_item.second;
        if (count++ >= 50) { printf("... (truncated)\n"); break; }
        printf("[%d] %d/%d %s:%s role=%s prio=%d protect=%d\n",
            count-1, ti.pid, ti.tid, ti.pname.c_str(), ti.tname.c_str(),
            role_names::getName((int)ti.role), ti.priority, ti.protect ? 1 : 0);
    }
}

void cmdBenchmark() {
    printf("Running placement benchmark...\n");
    if (g_allCpus.empty()) {
        printf("CPU topology not initialized. Run detectCpuTopology first.\n");
        return;
    }
    auto start = std::chrono::steady_clock::now();
    int total = 0;
    {
        std::lock_guard<std::mutex> lock(g_threadsMutex);
        for (auto&_sb_item : g_threads) {
        auto& tid = _sb_item.first; auto& ti = _sb_item.second; (void)tid;
            if (total >= benchmark::kMaxOps) break;
            for (int cpuId : g_allCpus) {
                computeScore(cpuId, ti);
                total++;
                if (total >= benchmark::kMaxOps) break;
            }
        }
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    printf("%d score computations in %lldus (avg %.2fus)\n", total, (long long)elapsed, (float)elapsed / (total > 0 ? total : 1));
}

void cmdExplain(int targetTid) {
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    auto it = g_threads.find(targetTid);
    if (it == g_threads.end()) {
        printf("Thread %d not found\n", targetTid);
        return;
    }
    const ThreadInfo& ti = it->second;
    printf("=== EXPLAIN: %s:%s (PID=%d TID=%d) ===\n", ti.pname.c_str(), ti.tname.c_str(), ti.pid, ti.tid);
    printf("Role: %s  Priority: %d  Subtype: %s\n\n", role_names::getName((int)ti.role), ti.priority, ti.subtype.c_str());
    printf("CPU Scores:\n");
    for (int cpuId : g_allCpus) {
        int s = computeScore(cpuId, ti);
        printf("  CPU%-2d | TOTAL=%+4d\n", cpuId, s);
    }
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" ||
                      std::string(argv[1]) == "-v" ||
                      std::string(argv[1]) == "version")) {
        printf("%s\n", AATPE_VERSION);
        return 0;
    }
    logInit();
    if (argc < 2) {
        printf("AATPE %s - Adaptive Android Task Placement Engine\n", AATPE_VERSION);
        printf("Usage: %s <command> [args]\n", argv[0]);
        printf("\nCommands:\n");
        printf("  --daemon              Start daemon (background)\n");
        printf("  --init-topology       Initialize CPU topology and exit\n");
        printf("  status                Show full system status\n");
        printf("  threads               List tracked threads\n");
        printf("  explain <tid>         Explain placement for thread\n");
        printf("  benchmark             Run placement benchmark\n");
        printf("  stop                  Stop daemon\n");
        return 0;
    }
    std::string cmd = argv[1];
    if (cmd == "--daemon") {
        if (geteuid() != 0) {
            fprintf(stderr, "AATPE: --daemon must be run as root (euid=%d)\n", (int)geteuid());
            return 1;
        }
        {
            struct sigaction saEarly;
            memset(&saEarly, 0, sizeof(saEarly));
            saEarly.sa_handler = signalHandler;
            sigemptyset(&saEarly.sa_mask);
            sigaction(SIGTERM, &saEarly, nullptr);
            sigaction(SIGINT, &saEarly, nullptr);
        }
        if (::daemon(0, 0) != 0) {
            fprintf(stderr, "AATPE: daemon() failed: %s\n", aatpeErrStr(errno));
            return 1;
        }
        mkdirIfNotExist(RUN_DIR, 0700);
        logReinit();
        LOG_FORCE("=== AATPE Daemon %s Starting ===", AATPE_VERSION);
        mkdirIfNotExist(STATE_DIR, 0700);
        int lockFd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lockFd < 0) { LOG_ERROR("Cannot create lock file"); return 1; }
        if (lockf(lockFd, F_TLOCK, 0) < 0) {
            LOG_INFO("aatped already running (lock held); second instance exiting quietly");
            close(lockFd);
            _exit(0);
        }
        applyHttpServerState(true);
        std::string startupCrash;
        try {
            daemonInit();
        } catch (const std::exception& e) {
            startupCrash = e.what();
            LOG_ERROR("FATAL: daemonInit threw, continuing in DEGRADED mode: %s", e.what());
        } catch (...) {
            startupCrash = "unknown non-std exception";
            LOG_ERROR("FATAL: daemonInit threw unknown exception, continuing in DEGRADED mode");
        }
        if (!startupCrash.empty()) {
            atomicWriteFile((std::string(STATE_DIR) + "/last_crash_reason").c_str(), startupCrash.c_str());
            std::string cnt = readFile((std::string(STATE_DIR) + "/startup_crash_count").c_str());
            int n = 0; try { n = std::stoi(cnt); } catch (...) { n = 0; }
            atomicWriteFile((std::string(STATE_DIR) + "/startup_crash_count").c_str(), n + 1);
            applyHttpServerState(true);
        } else {
            atomicWriteFile((std::string(STATE_DIR) + "/last_crash_reason").c_str(), "");
            atomicWriteFile((std::string(STATE_DIR) + "/boot_ok_at").c_str(),
                            (long long)time(nullptr));
        }
        atomicWriteFile((std::string(STATE_DIR) + "/passive_mode").c_str(),
                         g_watchdogPassiveLocked.load() ? "1" : "0");
    try {
    if (!aatpe::event::EventMonitor::instance().init()) {
        LOG_WARN("EventMonitor init failed, falling back to polling mode");
    }
        aatpe::InputMonitor::instance().start();
        startThermalSampling();
    } catch (const std::exception& e) {
        LOG_ERROR("FATAL: post-init thread setup threw, continuing without it: %s", e.what());
    } catch (...) {
        LOG_ERROR("FATAL: post-init thread setup threw unknown, continuing");
    }
    try {
        daemonLoop();
    } catch (const std::exception& e) {
        LOG_ERROR("FATAL: daemonLoop threw, exiting cleanly: %s", e.what());
    } catch (...) {
        LOG_ERROR("FATAL: daemonLoop threw unknown, exiting cleanly");
    }
        {
            using namespace aatpe::subsys;
            auto& reg = Registry::instance();
            for (auto& nm : reg.listNames()) reg.setState(nm, SubsystemState::STOPPING);
            reg.writeStateFile(STATE_DIR);
        }
        aatpe::event::EventMonitor::instance().shutdown();
        aatpe::InputMonitor::instance().stop();
        stopThermalSampling();
        aatpe::placement::releaseAllSpilloverLocked();
        aatpe::aatpeGpuBoostForceZero("shutdown");
        aatpe::psi::shutdown();
        aatpe_mech::restoreAllCgroupUclamp();
        aatpe_recovery::recoveryClearAllPersisted();
        aatpe::frameaware::refreshRateEventStop();
        g_ebpfScheduler.stopPollThread();
        g_ebpfScheduler.unload();
        aatpe::webui_http::stopHttpServer();
        if (g_controlInotifyWd >= 0) {
            inotify_rm_watch(g_controlInotifyFd, g_controlInotifyWd);
            g_controlInotifyWd = -1;
        }
        if (g_controlInotifyFd >= 0) {
            close(g_controlInotifyFd);
            g_controlInotifyFd = -1;
        }
        if (g_wakeupPipeReadFd >= 0) { close(g_wakeupPipeReadFd); g_wakeupPipeReadFd = -1; }
        if (g_wakeupPipeWriteFd >= 0) { close(g_wakeupPipeWriteFd); g_wakeupPipeWriteFd = -1; }
        {
            using namespace aatpe::subsys;
            auto& reg = Registry::instance();
            for (auto& nm : reg.listNames()) reg.setState(nm, SubsystemState::STOPPED);
            reg.writeStateFile(STATE_DIR);
            safemode::writeStateFiles(STATE_DIR);
            selfobs::writeStateFiles(STATE_DIR);
        }
        logShutdown();
        close(lockFd);
        unlink(LOCK_FILE);
        unlink(PID_FILE);
        unlink((std::string(STATE_DIR) + "/daemon_pid").c_str());
        stateCacheNoteRemove((std::string(STATE_DIR) + "/daemon_pid").c_str());
        return 0;
    }
    if (cmd == "--init-topology") {
        if (geteuid() != 0) {
            fprintf(stderr, "AATPE: --init-topology must be run as root (euid=%d)\n", (int)geteuid());
            return 1;
        }
        mkdirIfNotExist(RUN_DIR, 0700);
        mkdirIfNotExist(STATE_DIR, 0700);
        loadConfig();
    {
        bool cgroupV1 = access("/sys/fs/cgroup/cpuset", F_OK) == 0 || access("/dev/cpuset", F_OK) == 0;
        bool cgroupV2 = access("/sys/fs/cgroup/cpuset.cpus.effective", F_OK) == 0;
        LOG_INFO("Capability: cgroup_v1=%d cgroup_v2=%d", cgroupV1, cgroupV2);
    }
    detectCpuTopology();
        printf("CPU topology initialized: %d CPUs\n", g_cpuCount);
        return 0;
    }
    if (cmd == "status") { cmdStatus(); return 0; }

    if (cmd == "topology") {
        loadConfig();
        detectCpuTopology();
        std::lock_guard<std::mutex> lk(g_topologyRwMutex);
        printf("=== CPU Topology (ROUND-005) ===\n");
        printf("cpu_count=%d clusters=%d\n", g_cpuCount, clusterCount());
        printf("--- clusters (weak -> strong) ---\n%s\n", clustersStateText().c_str());
        printf("--- legacy pools ---\n");
        auto printPool = [](const char* name, const std::vector<int>& v) {
            printf("%s:", name);
            for (int c : v) printf(" %d", c);
            printf("\n");
        };
        printPool("LITTLE", g_littlePool);
        printPool("MID", g_midPool);
        printPool("BIG", g_bigPool);
        printPool("PRIME", primePool());
        return 0;
    }

    auto applyIntegrityRootOverride = [&]() -> bool {
        std::string root;
        if (argc > 2 && argv[2] && argv[2][0]) root = argv[2];
        else if (const char* env = getenv("AATPE_MODULE_ROOT")) {
            if (env[0]) root = env;
        }
        if (root.empty()) return true;
        aatpe::integrity::setModuleRootOverride(root);
        struct stat st;
        std::string binDir = root + "/system/bin";
        if (stat(binDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "AATPE: explicit module root '%s' has no system/bin/; aborting integrity op\n",
                    root.c_str());
            return false;
        }
        return true;
    };
    if (cmd == "--verify-integrity") {
        mkdirIfNotExist(RUN_DIR, 0700);
        mkdirIfNotExist(STATE_DIR, 0700);
        if (!applyIntegrityRootOverride()) return 1;
        bool ok = aatpe::integrity::verifyModuleIntegrity();
        if (ok) {
            printf("INTEGRITY OK\n");
        } else {
            printf("INTEGRITY FAILED\n");
            std::string rep = aatpe::integrity::getIntegrityReport();
            if (!rep.empty()) printf("%s\n", rep.c_str());
        }
        return ok ? 0 : 1;
    }
    if (cmd == "--generate-manifest") {
        if (!applyIntegrityRootOverride()) return 1;
        bool ok = aatpe::integrity::generateAndStoreManifest();
        if (ok) {
            printf("MANIFEST GENERATED\n");
        } else {
            printf("MANIFEST GENERATION FAILED\n");
        }
        return ok ? 0 : 1;
    }
    if (cmd == "threads") { loadConfig(); detectCpuTopology(); scanThreads(); cmdThreads(); return 0; }
    if (cmd == "benchmark") { loadConfig(); detectCpuTopology(); scanThreads(); cmdBenchmark(); return 0; }
    if (cmd == "explain" && argc > 2) {
        int targetTid = 0;
        try { targetTid = std::stoi(argv[2]); }
        catch (...) { printf("Invalid tid: %s\n", argv[2]); return 1; }
        loadConfig(); detectCpuTopology(); scanThreads(); cmdExplain(targetTid); return 0;
    }
    if (cmd == "stop") {
        int pid = readInt(PID_FILE, 0);
        if (pid > 0) {
            if (!aatpe::pid_identity::verifyAatpedPid((pid_t)pid)) {
                printf("PID %d failed identity verification (not aatped, or reused PID), aborting\n",
                       pid);
                return 1;
            }
            kill(pid, SIGTERM);
            printf("Daemon stopped (PID=%d)\n", pid);
        }
        else printf("Daemon not running\n");
        return 0;
    }
    printf("Unknown command: %s\n", cmd.c_str());
    return 1;
}

float hybridScore(int cpuId, const ThreadInfo& ti) {
    Config cfg; snapshotConfig(cfg);
    if (!cfg.mlHybridEnabled) return 0.0f;
    auto& hybrid = aatpe_ml_hybrid::instance();
    if (!hybrid.isEnabled() || hybrid.getPoolCount() <= 0) return 0.0f;
    int poolIdx = (int)getPoolForCpu(cpuId);
    if (poolIdx < 0 || poolIdx >= hybrid.getPoolCount()) return 0.0f;
    float hf[15] = {0};
    hf[0] = 1.0f;
    unsigned long long avgRunUsProxy = (ti.nrSwitches > 0) ?
        (unsigned long long)((double)ti.runTime / (double)ti.nrSwitches / 1000.0) : 0;
    hf[1] = std::min(1.0f, (float)avgRunUsProxy / 50000.0f);
    hf[2] = std::min(1.0f, (float)ti.nrSwitches / 1000.0f);
    {
        {
            float utilLocal = 0.5f, tempLocal = 0.5f;
            std::lock_guard<std::mutex> rlock(g_cpuMetricsMutex);
            auto it = g_cpuMetrics.find(cpuId);
            if (it != g_cpuMetrics.end()) {
                utilLocal = std::min(1.0f, it->second.util / 100.0f);
                if (it->second.temp > 0)
                    tempLocal = std::min(1.0f, std::max(0.0f, (float)it->second.temp / 120.0f));
            }
            hf[3] = utilLocal;
            hf[7] = tempLocal;
        }
    }
    hf[4] = std::min(1.0f, std::max(0.0f, (float)ti.priority / 100.0f));
    hf[5] = std::min(1.0f, std::max(0.0f, ((float)ti.nice + 20.0f) / 39.0f));
    hf[6] = (ti.role == ROLE_S3_GAME || ti.role == ROLE_S2_RENDER) ? 1.0f : 0.0f;
    hf[8] = aatpe_ml_hybrid::poolFeatureForTier(poolIdx, mlDistinctClusterCount());
    hf[9] = (ti.currentCpu == cpuId) ? 1.0f : 0.0f;
    hf[10] = 0.0f;
    hf[11] = hf[1] * hf[8];
    hf[12] = hf[1] * hf[3];
    hf[13] = hf[8] * hf[7];
    hf[14] = hf[1] * hf[7];
    return hybrid.predict(poolIdx, hf);
}


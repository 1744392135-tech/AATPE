#include "aatped.h"
#include "aatpe_ml_hybrid.h"
#include "aatpe_config_schema.h"
#include "c4_freezer.h"
#include "aatped_ebpf_agg.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cmath>

using namespace aatpe;
using namespace aatpe_ml_hybrid;

extern std::mutex g_hybridMutex;
extern bool g_hmacRequired;

namespace aatpe { namespace config {
struct ReloadHookEntry { std::string name; std::function<void(const Config&)> fn; };
static std::vector<ReloadHookEntry> g_reloadHooks;
static std::mutex g_reloadHooksMutex;

void registerReloadHook(const std::string& name,
                        std::function<void(const Config&)> hook) {
    std::lock_guard<std::mutex> lk(g_reloadHooksMutex);
    for (auto& e : g_reloadHooks) {
        if (e.name == name) { e.fn = std::move(hook); return; }
    }
    g_reloadHooks.push_back({name, std::move(hook)});
}
}}

static void fireReloadHooks() {
    Config snap;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        snap = g_cfg;
    }
    std::vector<aatpe::config::ReloadHookEntry> hooks;
    {
        std::lock_guard<std::mutex> lk(aatpe::config::g_reloadHooksMutex);
        hooks = aatpe::config::g_reloadHooks;
    }
    for (const auto& h : hooks) {
        try {
            if (h.fn) h.fn(snap);
        } catch (const std::exception& e) {
            LOG_WARN("reload hook '%s' threw: %s (ignored)", h.name.c_str(), e.what());
        } catch (...) {
            LOG_WARN("reload hook '%s' threw unknown (ignored)", h.name.c_str());
        }
    }
}

bool validateConfigLocked() {
    bool valid = true;
    if (g_cfg.controlMode != "assist" && g_cfg.controlMode != "active" &&
        g_cfg.controlMode != "passive" && g_cfg.controlMode != "game" &&
        g_cfg.controlMode != "emergency") {
        LOG_WARN("validateConfig: unknown control_mode='%s', defaulting to 'assist'", g_cfg.controlMode.c_str());
        g_cfg.controlMode = "assist";
    }
    if (g_cfg.decisionInterval < 10) {
        LOG_WARN("validateConfig: decision_interval=%d too low, clamping to 10", g_cfg.decisionInterval);
        g_cfg.decisionInterval = 10;
        valid = false;
    }
    if (g_cfg.decisionInterval > 5000) {
        LOG_WARN("validateConfig: decision_interval=%d too high, clamping to 5000", g_cfg.decisionInterval);
        g_cfg.decisionInterval = 5000;
        valid = false;
    }
    if (g_cfg.decisionMinIntervalMs < 0) {
        LOG_WARN("validateConfig: decision_min_interval_ms=%d < 0, clamping to 0", g_cfg.decisionMinIntervalMs);
        g_cfg.decisionMinIntervalMs = 0;
        valid = false;
    }
    if (g_cfg.decisionMinIntervalMs > 1000) {
        LOG_WARN("validateConfig: decision_min_interval_ms=%d too high, clamping to 1000", g_cfg.decisionMinIntervalMs);
        g_cfg.decisionMinIntervalMs = 1000;
        valid = false;
    }
    if (g_cfg.decisionIdleIntervalMs < 10) {
        LOG_WARN("validateConfig: decision_idle_interval_ms=%d too low, clamping to 10", g_cfg.decisionIdleIntervalMs);
        g_cfg.decisionIdleIntervalMs = 10;
        valid = false;
    }
    if (g_cfg.decisionIdleIntervalMs > 5000) {
        LOG_WARN("validateConfig: decision_idle_interval_ms=%d too high, clamping to 5000", g_cfg.decisionIdleIntervalMs);
        g_cfg.decisionIdleIntervalMs = 5000;
        valid = false;
    }
    if (g_cfg.maxMigrationsPerCycle < 1) {
        LOG_WARN("validateConfig: max_migrations_per_cycle=%d < 1, clamping to 1", g_cfg.maxMigrationsPerCycle);
        g_cfg.maxMigrationsPerCycle = 1;
        valid = false;
    }
    if (g_cfg.maxMigrationsPerCycle > 64) {
        LOG_WARN("validateConfig: max_migrations_per_cycle=%d too high, clamping to 64", g_cfg.maxMigrationsPerCycle);
        g_cfg.maxMigrationsPerCycle = 64;
        valid = false;
    }
    if (g_cfg.migrationDwellSec < 1) {
        LOG_WARN("validateConfig: migrationDwellSec=%d < 1, clamping to 1", g_cfg.migrationDwellSec);
        g_cfg.migrationDwellSec = 1;
        valid = false;
    }
    if (g_cfg.migrationDwellSec > 60) {
        LOG_WARN("validateConfig: migrationDwellSec=%d > 60, clamping to 60", g_cfg.migrationDwellSec);
        g_cfg.migrationDwellSec = 60;
        valid = false;
    }
    if (g_cfg.targetFrameTime < 1000 || g_cfg.targetFrameTime > 100000) {
        LOG_WARN("validateConfig: target_frame_time=%d out of range [1000,100000], resetting to 16667", g_cfg.targetFrameTime);
        g_cfg.targetFrameTime = 16667;
        valid = false;
    }
    if (g_cfg.thermalLimitCpu < 0 || g_cfg.thermalLimitCpu > 200) {
        g_cfg.thermalLimitCpu = 120;
        valid = false;
    }
    if (g_cfg.thermalEmergency < 0 || g_cfg.thermalEmergency > 200) {
        g_cfg.thermalEmergency = 120;
        valid = false;
    }
    if (g_cfg.thermalWarningThreshold > 0 &&
        g_cfg.thermalCriticalThreshold > 0 &&
        g_cfg.thermalEmergencyThreshold > 0) {
        if (g_cfg.thermalCriticalThreshold >= g_cfg.thermalEmergencyThreshold) {
            LOG_WARN("validateConfig: thermal CRITICAL=%d >= EMERGENCY=%d, "
                     "pulling CRITICAL down to EMERGENCY-1",
                     g_cfg.thermalCriticalThreshold, g_cfg.thermalEmergencyThreshold);
            g_cfg.thermalCriticalThreshold = g_cfg.thermalEmergencyThreshold - 1;
            valid = false;
        }
        if (g_cfg.thermalWarningThreshold >= g_cfg.thermalCriticalThreshold) {
            LOG_WARN("validateConfig: thermal WARNING=%d >= CRITICAL=%d, "
                     "pulling WARNING down to CRITICAL-1",
                     g_cfg.thermalWarningThreshold, g_cfg.thermalCriticalThreshold);
            g_cfg.thermalWarningThreshold = g_cfg.thermalCriticalThreshold - 1;
            valid = false;
        }
    }
    if (valid) {
        LOG_INFO("validateConfig: configuration validated OK");
    }
    return valid;
}

bool validateConfig() {
    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
    return validateConfigLocked();
}

void updateMlSaveStateFiles(bool success, const char* path) {
    unsigned long saveSuccessSnap, saveFailSnap;
    {
        saveSuccessSnap = g_mlSaveSuccess.load(std::memory_order_relaxed);
        saveFailSnap   = g_mlSaveFail.load(std::memory_order_relaxed);
    }
    writeFile((std::string(STATE_DIR) + "/ml_model_path").c_str(), path ? path : "");
    writeFile((std::string(STATE_DIR) + "/ml_last_save_time").c_str(), (long long)time(nullptr));
    writeFile((std::string(STATE_DIR) + "/ml_save_success").c_str(), (long long)saveSuccessSnap);
    writeFile((std::string(STATE_DIR) + "/ml_save_fail").c_str(), (long long)saveFailSnap);
    if (success) {
        struct stat st;
        if (path && stat(path, &st) == 0) {
            writeFile((std::string(STATE_DIR) + "/ml_model_size").c_str(), (long long)st.st_size);
        }
    }
}
std::unordered_map<std::string, PerAppConfig> g_perAppConfigs;
PerAppConfig g_currentAppConfig;
std::mutex g_perAppMutex;

static FILE* openConfigReadOnlyNoFollow(const char* path) {
    int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return nullptr;
    FILE* fp = ::fdopen(fd, "r");
    if (!fp) { ::close(fd); return nullptr; }
    return fp;
}

void loadPerAppConfigs(const char* confPath) {
    std::lock_guard<std::mutex> paLock(g_perAppMutex);
    FILE* fp = openConfigReadOnlyNoFollow(confPath);
    if (!fp) return;
    g_perAppConfigs.clear();
    g_currentAppConfig = PerAppConfig();
    char line[4096];
    std::string currentApp;
    PerAppConfig cfg;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '[') {
            char* close = strchr(line, ']');
            if (close && strncmp(line, "[app:", 5) == 0) {
                if (!currentApp.empty() && cfg.valid) {
                    g_perAppConfigs[currentApp] = cfg;
                }
                currentApp = std::string(line + 5, close - line - 5);
                cfg = PerAppConfig();
                cfg.packageName = currentApp;
                cfg.valid = true;
            }
        } else if (!currentApp.empty()) {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char* key = line;
                char* val = eq + 1;
                while (*key == ' ' || *key == '\t') key++;
                if (*key) {
                    for (char* e = key + strlen(key) - 1; e >= key && (*e == ' ' || *e == '\t'); e--) *e = '\0';
                }
                while (*val == ' ' || *val == '\t') val++;
                char* nl = strchr(val, '\n'); if (nl) *nl = '\0';
                if (strcmp(key, "profile") == 0) cfg.profile = val;
                else if (strcmp(key, "decision_interval") == 0) cfg.decisionIntervalMs = parseIntSafe(val);
                else if (strcmp(key, "max_fps") == 0) cfg.maxFpsHint = parseIntSafe(val);
            }
        }
    }
    if (!currentApp.empty() && cfg.valid) g_perAppConfigs[currentApp] = cfg;
    fclose(fp);
}
void checkAndApplyPerAppConfig(const char* foregroundApp) {
    if (!foregroundApp) return;
    PerAppConfig matched;
    {
        std::lock_guard<std::mutex> paLock(g_perAppMutex);
        auto it = g_perAppConfigs.find(foregroundApp);
        if (it != g_perAppConfigs.end()) {
            matched = it->second;
            g_currentAppConfig = matched;
        } else {
            g_currentAppConfig = PerAppConfig();
        }
    }
    if (matched.valid && !matched.profile.empty()) {
        aatpe_profile::profileRequest(matched.profile,
                                      aatpe_profile::ProfileSource::APP);
    } else {
        aatpe_profile::profileRelease(aatpe_profile::ProfileSource::APP);
    }
}
void snapshotConfig(Config& out) {
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    out = g_cfg;
}
time_t g_configMtime = 0;
time_t g_configMtimeReal = 0;
static int g_configMtimeCheckCounter = 0;
static int g_configInotifyFd = -1;
static int g_configWatchFd = -1;
static int g_configWatchFdReal = -1;
static int g_profWatchFd = -1;
static int g_profWatchFdReal = -1;
static std::thread g_configInotifyThread;
static std::atomic<bool> g_configInotifyRunning{false};

static std::string realConfDir() {
    return getModuleDir() + "/system/etc/aatpe";
}
static std::string realConfPath() {
    return realConfDir() + "/aatpe.conf";
}

static std::string getConfigPath() {
    std::string modPath = realConfPath();
    struct stat st;
    if (stat(modPath.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        access(modPath.c_str(), R_OK) == 0) {
        return modPath;
    }
    return std::string(CONF_DIR) + "/aatpe.conf";
}

static void configInotifyLoop() {
    struct LoopGuard { ~LoopGuard() { g_configInotifyRunning.store(false); } } guard;
    g_configInotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_configInotifyFd < 0) return;
    g_configWatchFd = inotify_add_watch(g_configInotifyFd, CONF_DIR,
                                         IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE | IN_MODIFY);
    if (g_configWatchFd < 0) {
        LOG_WARN("config inotify: cannot watch %s (errno=%d), mount-point watch inactive",
                 CONF_DIR, errno);
    }
    {
        std::string realDir = realConfDir();
        g_configWatchFdReal = inotify_add_watch(g_configInotifyFd, realDir.c_str(),
                                                IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE | IN_MODIFY);
        if (g_configWatchFdReal < 0) {
            LOG_WARN("config inotify: cannot watch %s (errno=%d), real-path watch inactive",
                     realDir.c_str(), errno);
        } else {
            LOG_INFO("config inotify: watching %s AND %s", CONF_DIR, realDir.c_str());
        }
    }
    {
        std::string profDir = std::string(CONF_DIR) + "/profiles";
        g_profWatchFd = inotify_add_watch(g_configInotifyFd, profDir.c_str(),
                                          IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE | IN_MODIFY);
        if (g_profWatchFd < 0)
            LOG_WARN("config inotify: cannot watch %s (errno=%d)", profDir.c_str(), errno);
        std::string realProfDir = realConfDir() + "/profiles";
        g_profWatchFdReal = inotify_add_watch(g_configInotifyFd, realProfDir.c_str(),
                                              IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE | IN_MODIFY);
        if (g_profWatchFdReal < 0)
            LOG_WARN("config inotify: cannot watch %s (errno=%d)", realProfDir.c_str(), errno);
    }
    if (g_configWatchFd < 0 && g_configWatchFdReal < 0) {
        close(g_configInotifyFd); g_configInotifyFd = -1; return;
    }
    char buf[4096];
    while (g_configInotifyRunning.load()) {
        if (g_configInotifyFd < 0) {
            LOG_WARN("config inotify: fd=%d invalid, hot-reload watch exiting", g_configInotifyFd);
            break;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_configInotifyFd, &fds);
        struct timeval tv = {1, 0};
        int ret = select(g_configInotifyFd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("config inotify: select failed (%s), hot-reload watch exiting", aatpeErrStr(errno));
            break;
        }
        if (ret > 0 && FD_ISSET(g_configInotifyFd, &fds)) {
            ssize_t len = read(g_configInotifyFd, buf, sizeof(buf));
            if (len < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                } else {
                    LOG_WARN("config inotify: read failed (%s), hot-reload watch exiting", aatpeErrStr(errno));
                    break;
                }
            } else if (len > 0) {
                bool confChanged = false;
                bool profileChanged = false;
                for (char* ptr = buf; ptr < buf + len; ) {
                    const struct inotify_event* ev = (const struct inotify_event*)ptr;
                    if (ev->len > 0) {
                        std::string fname(ev->name);
                        if (fname == "aatpe.conf") { confChanged = true; }
                        else if (fname.size() > 5 &&
                                 fname.compare(fname.size() - 5, 5, ".conf") == 0) {
                            profileChanged = true;
                        }
                    }
                    ptr += sizeof(struct inotify_event) + ev->len;
                }
                if (confChanged) {
                    g_reloadConfigRequested.store(true);
                    LOG_INFO("Config file changed (inotify, watched paths), hot-reload requested");
                }
                if (profileChanged) {
                    g_reloadConfigRequested.store(true);
                    g_reloadProfile.store(1, std::memory_order_relaxed);
                    LOG_INFO("Profile file changed (inotify), reload + profile re-apply requested");
                }
            }
        }
    }
    if (g_configWatchFd >= 0) inotify_rm_watch(g_configInotifyFd, g_configWatchFd);
    if (g_configWatchFdReal >= 0) inotify_rm_watch(g_configInotifyFd, g_configWatchFdReal);
    if (g_profWatchFd >= 0) inotify_rm_watch(g_configInotifyFd, g_profWatchFd);
    if (g_profWatchFdReal >= 0) inotify_rm_watch(g_configInotifyFd, g_profWatchFdReal);
    if (g_configInotifyFd >= 0) close(g_configInotifyFd);
    g_configInotifyFd = -1;
    g_configWatchFd = -1;
    g_configWatchFdReal = -1;
}
void startConfigHotReload() {
    if (g_configInotifyRunning.load()) return;
    g_configInotifyRunning.store(true);
    g_configInotifyThread = std::thread(configInotifyLoop);
}

void stopConfigHotReload() {
    g_configInotifyRunning.store(false);
    if (g_configInotifyThread.joinable()) {
        g_configInotifyThread.join();
    }
    if (g_configInotifyFd >= 0) {
        close(g_configInotifyFd);
        g_configInotifyFd = -1;
    }
    g_configWatchFd = -1;
    g_configWatchFdReal = -1;
    g_profWatchFd = -1;
    g_profWatchFdReal = -1;
}
static int clampIntConfig(int val, int minVal, int maxVal, const char* key) {
    if (val < minVal) { LOG_WARN("Config %s=%d below min %d, clamped", key, val, minVal); return minVal; }
    if (val > maxVal) { LOG_WARN("Config %s=%d above max %d, clamped", key, val, maxVal); return maxVal; }
    return val;
}
static float clampFloatConfig(float val, float minVal, float maxVal, const char* key) {
    if (val < minVal) { LOG_WARN("Config %s=%.2f below min %.2f, clamped", key, val, minVal); return minVal; }
    if (val > maxVal) { LOG_WARN("Config %s=%.2f above max %.2f, clamped", key, val, maxVal); return maxVal; }
    return val;
}

static float parseFloatStrict(const std::string& val, float def, const char* key) {
    try {
 size_t consumed = 0;
        float v = std::stof(val, &consumed);
        if (!std::isfinite(v)) {
            LOG_WARN("Config %s='%s' is non-finite (nan/inf), keeping default %.4f", key, val.c_str(), def);
            return def;
        }
        return v;
    } catch (...) {
        LOG_WARN("Config %s='%s' not a float, keeping default %.4f", key, val.c_str(), def);
        return def;
    }
}

static double parseDoubleStrict(const std::string& val, double def, const char* key) {
    try {
        size_t consumed = 0;
        double v = std::stod(val, &consumed);
        if (!std::isfinite(v)) {
            LOG_WARN("Config %s='%s' is non-finite (nan/inf), keeping default %.6f", key, val.c_str(), def);
            return def;
        }
        return v;
    } catch (...) {
        LOG_WARN("Config %s='%s' not a double, keeping default %.6f", key, val.c_str(), def);
        return def;
    }
}
static double clampDoubleConfig(double val, double minVal, double maxVal, const char* key) {
    if (val < minVal) { LOG_WARN("Config %s=%.4f below min %.4f, clamped", key, val, minVal); return minVal; }
    if (val > maxVal) { LOG_WARN("Config %s=%.4f above max %.4f, clamped", key, val, maxVal); return maxVal; }
    return val;
}

static void validatePoolFormat(const std::string& val, const char* key) {
    if (val.empty()) return;
    for (char c : val) {
        if (!((c >= '0' && c <= '9') || c == ',' || c == '-' ||
              c == ' ' || c == '\t')) {
            LOG_WARN("Config %s='%s' contains unexpected characters (expected "
                     "comma-separated ids/ranges like '0-3,6,7')", key, val.c_str());
            return;
        }
    }
}

static int parseStrictBool(const std::string& val, int def, const char* key) {
    if (val == "0") return 0;
    if (val == "1") return 1;
    LOG_WARN("Config %s='%s' is not strict 0/1, keeping default %d", key, val.c_str(), def);
    return def;
}

static bool parseFlexBool(const std::string& val, bool def, const char* key) {
    std::string v;
    v.reserve(val.size());
    for (char c : val) v.push_back((char)tolower((unsigned char)c));
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    LOG_WARN("Config %s='%s' not a recognized bool, keeping default %d", key, val.c_str(), (int)def);
    return def;
}

void loadConfig() {
    std::string pendingGlobalProfile;
    std::string confPath = getConfigPath();
    FILE* cfgFp = openConfigReadOnlyNoFollow(confPath.c_str());
    if (!cfgFp) {
        LOG_WARN("Config not found (basename=%s, keeping previous g_cfg)",
                 confPath.c_str() + confPath.rfind('/') + 1);
        return;
    }
    {
        struct stat pst;
        if (::fstat(fileno(cfgFp), &pst) == 0) {
            bool insecure = (pst.st_mode & (S_IWGRP | S_IWOTH)) != 0;
            if (::geteuid() == 0 && pst.st_uid != 0) insecure = true;
            if (insecure) {
                LOG_WARN("Config (basename=%s) insecure (mode=%03o): refusing to load, keeping running g_cfg",
                         confPath.c_str() + confPath.rfind('/') + 1,
                         (unsigned)(pst.st_mode & 0777));
                ::fclose(cfgFp);
                return;
            }
        }
    }
    std::unique_lock<std::mutex> lock(g_cfgMutex);
    Config prevCfg = g_cfg;
    LOG_INFO("[CFG] loading config from %s (rebuild on previous running config)", confPath.c_str());
    std::string line;
    bool needsConfRewrite = false;
    int badKeyCount = 0;
    char linebuf[8192];
    while (std::fgets(linebuf, sizeof(linebuf), cfgFp)) {
        line = linebuf;
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        trim(key); trim(val);
        for (char& c : key) c = (char)toupper((unsigned char)c);
        if (!key.empty() && val.size() >= key.size() + 1 &&
            val.compare(0, key.size(), key) == 0 && val[key.size()] == '=') {
            LOG_WARN("Config self-heal: stripped doubled prefix '%s=' from value (was '%s')",
                     key.c_str(), val.c_str());
            val = val.substr(key.size() + 1);
            needsConfRewrite = true;
        }
        try {
            {
                const aatpe::ConfigField* sf = aatpe::configSchemaLookup(key);
                if (sf) {
                    std::string norm;
                    if (aatpe::configSchemaValidate(key, val, norm)) {
                        if      (key == "STATE_CACHE_TTL_HOURS")       g_cfg.stateCacheTtlHours = std::stoi(norm);
                        else if (key == "PLACEMENT_MIN_GAIN_PCT")      g_cfg.placementMinGainPercent = std::stoi(norm);
                        else if (key == "PLACEMENT_MIN_CONFIDENCE")    g_cfg.placementMinConfidence = std::stoi(norm);
                        else if (key == "MIN_USEFUL_CAPACITY_RATIO")   g_cfg.minUsefulCapacityRatio = parseFloatStrict(norm, g_cfg.minUsefulCapacityRatio, "MIN_USEFUL_CAPACITY_RATIO");
                        else if (key == "MIGRATION_HYSTERESIS_PCT" || key == "MIGRATION_HYSTERESIS_PERCENT") g_cfg.migrationHysteresisPercent = std::stoi(norm);
                        else if (key == "MIGRATION_COOLDOWN_MS")       g_cfg.migrationCooldownMs = std::stoi(norm);
                        else if (key == "MIGRATION_PENALTY_BIG_LITTLE") g_cfg.migrationPenaltyBigLittle = parseFloatStrict(norm, g_cfg.migrationPenaltyBigLittle, "MIGRATION_PENALTY_BIG_LITTLE");
                        else if (key == "THREAD_ROLE_CONFIDENCE_THRESHOLD") g_cfg.threadRoleConfidenceThreshold = std::stoi(norm);
                        else if (key == "ML_DRIFT_DETECTION_ENABLED" || key == "ML_DRIFT_DETECTION") g_cfg.mlDriftDetectionEnabled = (std::stoi(norm) != 0);
                        else if (key == "PRIME_POOL") g_cfg.primePoolCfg = norm;
                    } else {
                        LOG_WARN("[SCHEMA] %s='%s' failed type/range check, ignored (default kept)",
                                 key.c_str(), val.c_str());
                    }
                    continue;
                }
            }
            if (key == "CONTROL_MODE") {
                if (val == "assist" || val == "active" || val == "passive" ||
                    val == "game" || val == "emergency") {
                    g_cfg.controlMode = val;
                } else {
                    LOG_WARN("Config CONTROL_MODE invalid '%s' (expected assist/active/passive/game/emergency), keeping '%s'",
                             val.c_str(), g_cfg.controlMode.c_str());
                }
            }
            else if (key == "DECISION_INTERVAL") {
                g_cfg.decisionInterval = clampIntConfig(std::stoi(val), 10, 5000, "DECISION_INTERVAL");
            }
            else if (key == "DECISION_MIN_INTERVAL_MS")
                g_cfg.decisionMinIntervalMs = clampIntConfig(std::stoi(val), 0, 1000, "DECISION_MIN_INTERVAL_MS");
            else if (key == "DECISION_IDLE_INTERVAL_MS")
                g_cfg.decisionIdleIntervalMs = clampIntConfig(std::stoi(val), 10, 5000, "DECISION_IDLE_INTERVAL_MS");
            else if (key == "CONFIDENCE_THRESHOLD") g_cfg.confidenceThreshold = clampIntConfig(std::stoi(val), 0, 100, "CONFIDENCE_THRESHOLD");
            else if (key == "MIGRATION_COST_THRESHOLD") g_cfg.migrationCostThreshold = clampIntConfig(std::stoi(val), 0, 10000, "MIGRATION_COST_THRESHOLD");
            else if (key == "MAX_MIGRATIONS_PER_SEC") g_cfg.maxMigrationsPerSec = clampIntConfig(std::stoi(val), 1, 1000, "MAX_MIGRATIONS_PER_SEC");
            else if (key == "UCLAMP_PASSIVE_PCT") g_cfg.uclampPassivePct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_PASSIVE_PCT");
            else if (key == "UCLAMP_CRITICAL_MIN_PCT") g_cfg.uclampCriticalMinPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_CRITICAL_MIN_PCT");
            else if (key == "UCLAMP_CRITICAL_MAX_PCT") g_cfg.uclampCriticalMaxPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_CRITICAL_MAX_PCT");
            else if (key == "UCLAMP_ASSIST_MIN_PCT") g_cfg.uclampAssistMinPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_ASSIST_MIN_PCT");
            else if (key == "UCLAMP_SF_PCT") g_cfg.uclampSfPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_SF_PCT");
            else if (key == "INPUT_BOOST_MS_60") g_cfg.inputBoostMs60 = clampIntConfig(std::stoi(val), 10, 500, "INPUT_BOOST_MS_60");
            else if (key == "INPUT_BOOST_MS_120") g_cfg.inputBoostMs120 = clampIntConfig(std::stoi(val), 10, 500, "INPUT_BOOST_MS_120");
            else if (key == "INPUT_BOOST_MS_144") g_cfg.inputBoostMs144 = clampIntConfig(std::stoi(val), 10, 500, "INPUT_BOOST_MS_144");
            else if (key == "SCHEDTUNE_CRITICAL_BOOST") g_cfg.schedtuneCriticalBoost = clampIntConfig(std::stoi(val), 0, 100, "SCHEDTUNE_CRITICAL_BOOST");
            else if (key == "SCHEDTUNE_PASSIVE_BOOST") g_cfg.schedtunePassiveBoost = clampIntConfig(std::stoi(val), 0, 100, "SCHEDTUNE_PASSIVE_BOOST");
            else if (key == "SCHEDTUNE_ASSIST_BOOST") g_cfg.schedtuneAssistBoost = clampIntConfig(std::stoi(val), 0, 100, "SCHEDTUNE_ASSIST_BOOST");
            else if (key == "MAX_MIGRATIONS_PER_CYCLE") g_cfg.maxMigrationsPerCycle = clampIntConfig(std::stoi(val), 1, 64, "MAX_MIGRATIONS_PER_CYCLE");
            else if (key == "THERMAL_ENABLED") g_cfg.thermalEnabled = parseStrictBool(val, g_cfg.thermalEnabled ? 1 : 0, "THERMAL_ENABLED") != 0;
            else if (key == "C4_FREEZER_ENABLED") {
                bool en = parseFlexBool(val, g_cfg.c4FreezerEnabled, "C4_FREEZER_ENABLED");
                g_cfg.c4FreezerEnabled = en;
                aatpe::c4freezer::setC4FreezerEnabled(en);
            }
            else if (key == "EBPF_EVENT_DRIVEN") g_cfg.ebpfEnabled = parseStrictBool(val, g_cfg.ebpfEnabled ? 1 : 0, "EBPF_EVENT_DRIVEN") != 0;
            else if (key == "FEDERATED_ENABLED") g_cfg.federatedEnabled = parseFlexBool(val, g_cfg.federatedEnabled, "FEDERATED_ENABLED");
            else if (key == "THERMAL_LIMIT_CPU") g_cfg.thermalLimitCpu = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_LIMIT_CPU");
            else if (key == "THERMAL_LIMIT_SKIN") g_cfg.thermalLimitSkin = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_LIMIT_SKIN");
            else if (key == "THERMAL_LIMIT_BATTERY") g_cfg.thermalLimitBattery = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_LIMIT_BATTERY");
            else if (key == "THERMAL_EMERGENCY") {
                LOG_WARN("Config key THERMAL_EMERGENCY is deprecated; use THERMAL_EMERGENCY_THRESHOLD (new key takes precedence when > 0).");
                g_cfg.thermalEmergency = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_EMERGENCY");
            }
            else if (key == "FRAME_AWARE") g_cfg.frameAware = parseStrictBool(val, g_cfg.frameAware ? 1 : 0, "FRAME_AWARE") != 0;
            else if (key == "TARGET_FRAME_TIME") g_cfg.targetFrameTime = clampIntConfig(std::stoi(val), 1000, 100000, "TARGET_FRAME_TIME");
            else if (key == "FRAME_CRITICAL_THRESHOLD") g_cfg.frameCriticalThreshold = clampIntConfig(std::stoi(val), 0, 50000, "FRAME_CRITICAL_THRESHOLD");
            else if (key == "JANK_THRESHOLD_PERCENT") g_cfg.jankThresholdPercent = clampFloatConfig(parseFloatStrict(val, g_cfg.jankThresholdPercent, "JANK_THRESHOLD_PERCENT"), 0.0f, 100.0f, "JANK_THRESHOLD_PERCENT");
            else if (key == "LEARNING_ENABLED") g_cfg.learningEnabled = parseStrictBool(val, g_cfg.learningEnabled ? 1 : 0, "LEARNING_ENABLED") != 0;
            else if (key == "HISTORY_WEIGHT") g_cfg.historyWeight = clampIntConfig(std::stoi(val), 0, 100, "HISTORY_WEIGHT");
            else if (key == "LEARNING_DECAY") g_cfg.learningDecay = clampIntConfig(std::stoi(val), 0, 100, "LEARNING_DECAY");
            else if (key == "HISTORY_MIN_SAMPLES") g_cfg.minSamplesForLearning = clampIntConfig(std::stoi(val), 1, 10000, "HISTORY_MIN_SAMPLES");
            else if (key == "THREAD_SCAN_INTERVAL") g_cfg.threadScanInterval = clampIntConfig(std::stoi(val), 1, 10000, "THREAD_SCAN_INTERVAL");
            else if (key == "MAX_TRACKED_THREADS") g_cfg.maxTrackedThreads = clampIntConfig(std::stoi(val), 1, 10000, "MAX_TRACKED_THREADS");
            else if (key == "IGNORE_SYSTEM_THREADS") g_cfg.ignoreSystemThreads = parseStrictBool(val, g_cfg.ignoreSystemThreads ? 1 : 0, "IGNORE_SYSTEM_THREADS") != 0;
            else if (key == "WATCHDOG_ENABLED") g_cfg.watchdogEnabled = parseStrictBool(val, g_cfg.watchdogEnabled ? 1 : 0, "WATCHDOG_ENABLED") != 0;
            else if (key == "WATCHDOG_INTERVAL") g_cfg.watchdogInterval = clampIntConfig(std::stoi(val), 1, 600, "WATCHDOG_INTERVAL");
            else if (key == "WATCHDOG_MAX_RESTARTS") g_cfg.watchdogMaxRestarts = clampIntConfig(std::stoi(val), 1, 100, "WATCHDOG_MAX_RESTARTS");
            else if (key == "WATCHDOG_RESTART_WINDOW") g_cfg.watchdogRestartWindow = clampIntConfig(std::stoi(val), 1, 3600, "WATCHDOG_RESTART_WINDOW");
            else if (key == "FEDERATED_ENABLED") g_cfg.federatedEnabled = parseFlexBool(val, g_cfg.federatedEnabled, "FEDERATED_ENABLED");
            else if (key == "SAFETY_MIGRATION_LIMIT") g_cfg.safetyMigrationLimit = clampIntConfig(std::stoi(val), 1, 10000, "SAFETY_MIGRATION_LIMIT");
            else if (key == "SAFETY_LATENCY_LIMIT") g_cfg.safetyLatencyLimit = clampIntConfig(std::stoi(val), 1, 100000, "SAFETY_LATENCY_LIMIT");
            else if (key == "SAFETY_LOAD_LIMIT") g_cfg.safetyLoadLimit = clampIntConfig(std::stoi(val), 1, 1000, "SAFETY_LOAD_LIMIT");
            else if (key == "CPU_USAGE_LIMIT") g_cfg.cpuUsageLimit = clampIntConfig(std::stoi(val), 10, 100, "CPU_USAGE_LIMIT");
            else if (key == "LOG_LEVEL") { g_cfg.logLevel = clampIntConfig(std::stoi(val), 0, 5, "LOG_LEVEL"); g_logLevel.store(g_cfg.logLevel); }
            else if (key == "LOG_DECISIONS") g_cfg.logDecisions = clampIntConfig(std::stoi(val), 0, 1, "LOG_DECISIONS");
            else if (key == "LOG_THERMAL") g_cfg.logThermal = clampIntConfig(std::stoi(val), 0, 1, "LOG_THERMAL");
            else if (key == "LOG_FRAME") g_cfg.logFrame = clampIntConfig(std::stoi(val), 0, 1, "LOG_FRAME");
            else if (key == "PREBOOST_ENABLED") preboostEnabled.store(parseFlexBool(val, false, "PREBOOST_ENABLED"));
            else if (key == "THERMAL_SMOOTH_ENABLED") thermalSmoothEnabled.store(parseFlexBool(val, false, "THERMAL_SMOOTH_ENABLED"));
            else if (key == "SCENEINFER_ENABLED") sceneInferEnabled.store(parseFlexBool(val, false, "SCENEINFER_ENABLED"));
            else if (key == "C2_ENVELOPE_ENABLED") c2EnvelopeEnabled.store(parseFlexBool(val, false, "C2_ENVELOPE_ENABLED"));
            else if (key == "AUTO_PROFILE") g_cfg.autoProfile = parseStrictBool(val, 1, "AUTO_PROFILE");
            else if (key == "CURRENT_PROFILE") {
                if (val == "balanced" || val == "gaming" || val == "battery" || val == "thermal") {
                    pendingGlobalProfile = val;
                } else {
                    LOG_WARN("CURRENT_PROFILE invalid: '%s' (using default 'balanced')", val.c_str());
                    pendingGlobalProfile = "balanced";
                }
            }
            else if (key == "GAME_DETECTION") g_cfg.gameDetection = clampIntConfig(std::stoi(val), 0, 1, "GAME_DETECTION");
            else if (key == "REFRESH_RATE_MODE") {
                if (val == "peak" || val == "current" || val == "auto") g_cfg.refreshRateMode = val;
                else { LOG_WARN("REFRESH_RATE_MODE invalid: '%s' (using default 'peak')", val.c_str()); g_cfg.refreshRateMode = "peak"; }
            }
            else if (key == "BATTERY_SAVER_THRESHOLD") g_cfg.batterySaverThreshold = clampIntConfig(std::stoi(val), 0, 100, "BATTERY_SAVER_THRESHOLD");
            else if (key == "THERMAL_PROFILE_THRESHOLD") g_cfg.thermalProfileThreshold = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_PROFILE_THRESHOLD");
            else if (key == "THERMAL_WARNING_THRESHOLD")    g_cfg.thermalWarningThreshold    = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_WARNING_THRESHOLD");
            else if (key == "THERMAL_CRITICAL_THRESHOLD")   g_cfg.thermalCriticalThreshold   = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_CRITICAL_THRESHOLD");
            else if (key == "THERMAL_EMERGENCY_THRESHOLD")  g_cfg.thermalEmergencyThreshold  = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_EMERGENCY_THRESHOLD");
            else if (key == "LAYER_CRITICAL_THRESHOLD") g_cfg.layerCriticalThreshold = clampIntConfig(std::stoi(val), 0, 1000, "LAYER_CRITICAL_THRESHOLD");
            else if (key == "DAEMON_MEMORY_WARN_KB") g_cfg.daemonMemoryWarnKb = clampIntConfig(std::stoi(val), 1024, 1048576, "DAEMON_MEMORY_WARN_KB");
            else if (key == "LITTLE_POOL") { g_cfg.littlePool = val; validatePoolFormat(val, "LITTLE_POOL"); }
            else if (key == "MID_POOL")    { g_cfg.midPool    = val; validatePoolFormat(val, "MID_POOL"); }
            else if (key == "BIG_POOL")    { g_cfg.bigPool    = val; validatePoolFormat(val, "BIG_POOL"); }
            else if (key == "ML_ENABLED") g_cfg.mlLearningEnabled = parseStrictBool(val, g_cfg.mlLearningEnabled ? 1 : 0, "ML_ENABLED") != 0;
            else if (key == "ML_LEARNING_ENABLED") g_cfg.mlLearningEnabled = parseStrictBool(val, g_cfg.mlLearningEnabled ? 1 : 0, "ML_LEARNING_ENABLED") != 0;
            else if (key == "ML_SHADOW_SAMPLING") g_cfg.mlShadowSampling = parseStrictBool(val, g_cfg.mlShadowSampling ? 1 : 0, "ML_SHADOW_SAMPLING") != 0;
            else if (key == "ML_MAX_ALPHA") g_cfg.mlMaxAlpha = parseFloatStrict(val, g_cfg.mlMaxAlpha, "ML_MAX_ALPHA");
            else if (key == "ML_INITIAL_LR") g_cfg.mlInitialLR = parseFloatStrict(val, g_cfg.mlInitialLR, "ML_INITIAL_LR");
            else if (key == "ML_DECAY") g_cfg.mlDecay = parseFloatStrict(val, g_cfg.mlDecay, "ML_DECAY");
            else if (key == "ML_SAVE_INTERVAL") g_cfg.mlSaveInterval = clampIntConfig(std::stoi(val), 1, 3600, "ML_SAVE_INTERVAL");
            else if (key == "ML_WEIGHT_CLAMP") g_cfg.mlWeightClamp = parseFloatStrict(val, g_cfg.mlWeightClamp, "ML_WEIGHT_CLAMP");
            else if (key == "ML_HYBRID_ENABLED") g_cfg.mlHybridEnabled = parseStrictBool(val, g_cfg.mlHybridEnabled ? 1 : 0, "ML_HYBRID_ENABLED") != 0;
            else if (key == "HMAC_REQUIRED") g_cfg.hmacRequired = parseStrictBool(val, g_cfg.hmacRequired ? 1 : 0, "HMAC_REQUIRED") != 0;
            else if (key == "PACKAGE_WHITELIST") g_cfg.packageWhitelist = val;
            else if (key == "GAME_PACKAGES") {
                g_cfg.gamePackages.clear();
                std::stringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    trim(item);
                    if (!item.empty()) g_cfg.gamePackages.push_back(item);
                }
            }
            else if (key == "BATTERY_AGING_PROTECTION") g_cfg.batteryAgingProtection = parseFlexBool(val, g_cfg.batteryAgingProtection, "BATTERY_AGING_PROTECTION");
            else if (key == "ML_COLD_START_PROTECTION") g_cfg.mlColdStartProtection = parseFlexBool(val, g_cfg.mlColdStartProtection, "ML_COLD_START_PROTECTION");
            else if (key == "ML_MIN_SAMPLES") g_cfg.mlMinSamples = clampIntConfig(std::stoi(val), 1, 1000000, "ML_MIN_SAMPLES");
            else if (key == "ENABLE_HTTP_SERVER") g_cfg.enableHttpServer = parseFlexBool(val, g_cfg.enableHttpServer, "ENABLE_HTTP_SERVER");
            else if (key == "EBPF_ENABLED") g_cfg.ebpfEnabled = parseFlexBool(val, g_cfg.ebpfEnabled, "EBPF_ENABLED");
            else if (key == "EBPF_RING_PAGES") g_cfg.ebpfRingPages = clampIntConfig(std::stoi(val), 1, 256, "EBPF_RING_PAGES");
            else if (key == "EBPF_POLL_FALLBACK") g_cfg.ebpfPollFallback = parseFlexBool(val, g_cfg.ebpfPollFallback, "EBPF_POLL_FALLBACK");
            else if (key == "ML_FRAME_TIME_OUTCOME") g_cfg.mlFrameTimeOutcome = parseFlexBool(val, g_cfg.mlFrameTimeOutcome, "ML_FRAME_TIME_OUTCOME");
            else if (key == "ML_FRAME_TIME_WEIGHT") { float _w = parseFloatStrict(val, g_cfg.mlFrameTimeWeight, "ML_FRAME_TIME_WEIGHT"); g_cfg.mlFrameTimeWeight = (_w < 0.0f ? 0.0f : (_w > 1.0f ? 1.0f : _w)); }
            else if (key == "ML_OUTCOME_MAD_FILTER") g_cfg.mlOutcomeMadFilter = clampIntConfig(std::stoi(val), 0, 1, "ML_OUTCOME_MAD_FILTER");
            else if (key == "ML_DRIFT_RELEARN") g_cfg.mlDriftRelearn = clampIntConfig(std::stoi(val), 0, 1, "ML_DRIFT_RELEARN");
            else if (key == "ML_EPSILON_GREEDY") g_cfg.mlEpsilonGreedy = clampDoubleConfig(parseDoubleStrict(val, g_cfg.mlEpsilonGreedy, "ML_EPSILON_GREEDY"), 0.0, 0.05, "ML_EPSILON_GREEDY");
            else if (key == "PRIORITY_BOOST_ENABLED") g_cfg.priorityBoostEnabled = parseFlexBool(val, g_cfg.priorityBoostEnabled, "PRIORITY_BOOST_ENABLED");
            else if (key == "PRIORITY_BOOST_LEVEL") {
                int lvl = std::stoi(val);
                g_cfg.priorityBoostLevel = lvl < 1 ? 1 : (lvl > 3 ? 3 : lvl);
            }
            else if (key == "SMART_PLACEMENT_ENABLED") g_cfg.smartPlacementEnabled = parseFlexBool(val, g_cfg.smartPlacementEnabled, "SMART_PLACEMENT_ENABLED");
            else if (key == "CORE_AFFINITY_WEIGHT") g_cfg.coreAffinityWeight = std::max(0.0f, std::min(1.0f, parseFloatStrict(val, g_cfg.coreAffinityWeight, "CORE_AFFINITY_WEIGHT")));
            else if (key == "LOAD_BALANCE_WEIGHT") g_cfg.loadBalanceWeight = std::max(0.0f, std::min(1.0f, parseFloatStrict(val, g_cfg.loadBalanceWeight, "LOAD_BALANCE_WEIGHT")));
            else if (key == "TEMPERATURE_WEIGHT") g_cfg.temperatureWeight = std::max(0.0f, std::min(1.0f, parseFloatStrict(val, g_cfg.temperatureWeight, "TEMPERATURE_WEIGHT")));
            else if (key == "ADRC_ENABLED") g_cfg.adrcEnabled = parseFlexBool(val, g_cfg.adrcEnabled, "ADRC_ENABLED");
            else if (key == "ADRC_H") g_cfg.adrcH = std::max(0.005f, std::min(0.5f, parseFloatStrict(val, g_cfg.adrcH, "ADRC_H")));
            else if (key == "ADRC_R") g_cfg.adrcR = std::max(1.0f, std::min(500.0f, parseFloatStrict(val, g_cfg.adrcR, "ADRC_R")));
            else if (key == "ADRC_BETA01") g_cfg.adrcBeta01 = std::max(10.0f, std::min(1000.0f, parseFloatStrict(val, g_cfg.adrcBeta01, "ADRC_BETA01")));
            else if (key == "ADRC_BETA02") g_cfg.adrcBeta02 = std::max(30.0f, std::min(3000.0f, parseFloatStrict(val, g_cfg.adrcBeta02, "ADRC_BETA02")));
            else if (key == "ADRC_BETA03") g_cfg.adrcBeta03 = std::max(100.0f, std::min(10000.0f, parseFloatStrict(val, g_cfg.adrcBeta03, "ADRC_BETA03")));
            else if (key == "ADRC_BETA1") g_cfg.adrcBeta1 = std::max(0.1f, std::min(100.0f, parseFloatStrict(val, g_cfg.adrcBeta1, "ADRC_BETA1")));
            else if (key == "ADRC_BETA2") g_cfg.adrcBeta2 = std::max(0.01f, std::min(10.0f, parseFloatStrict(val, g_cfg.adrcBeta2, "ADRC_BETA2")));
            else if (key == "ADRC_ALPHA1") g_cfg.adrcAlpha1 = std::max(0.1f, std::min(2.0f, parseFloatStrict(val, g_cfg.adrcAlpha1, "ADRC_ALPHA1")));
            else if (key == "ADRC_ALPHA2") g_cfg.adrcAlpha2 = std::max(0.1f, std::min(2.0f, parseFloatStrict(val, g_cfg.adrcAlpha2, "ADRC_ALPHA2")));
            else if (key == "ADRC_DELTA") g_cfg.adrcDelta = std::max(0.001f, std::min(1.0f, parseFloatStrict(val, g_cfg.adrcDelta, "ADRC_DELTA")));
            else if (key == "ADRC_B0") g_cfg.adrcB0 = std::max(0.1f, std::min(100.0f, parseFloatStrict(val, g_cfg.adrcB0, "ADRC_B0")));
            else if (key == "ADRC_URGENCY_THRESHOLD") g_cfg.adrcUrgencyThreshold = std::max(0.1f, std::min(1.0f, parseFloatStrict(val, g_cfg.adrcUrgencyThreshold, "ADRC_URGENCY_THRESHOLD")));
            else if (key == "ADRC_RISK_THRESHOLD") g_cfg.adrcRiskThreshold = std::max(0.1f, std::min(1.0f, parseFloatStrict(val, g_cfg.adrcRiskThreshold, "ADRC_RISK_THRESHOLD")));
            else if (key == "ADRC_CLAMP_FRAME_RATIO") g_cfg.adrcClampFrameRatio = std::max(0.3f, std::min(0.9f, parseFloatStrict(val, g_cfg.adrcClampFrameRatio, "ADRC_CLAMP_FRAME_RATIO")));
            else if (key == "ADRC_WEIGHT_BASE") g_cfg.adrcWeightBase = std::max(0.0f, std::min(1.0f, parseFloatStrict(val, g_cfg.adrcWeightBase, "ADRC_WEIGHT_BASE")));
            else if (key == "ADRC_ML_WEIGHT_BASE") g_cfg.adrcMlWeightBase = std::max(0.0f, std::min(1.0f, parseFloatStrict(val, g_cfg.adrcMlWeightBase, "ADRC_ML_WEIGHT_BASE")));
            else if (key == "MIGRATION_HYSTERESIS_PERCENT")
                g_cfg.migrationHysteresisPercent = clampIntConfig(std::stoi(val), 0, 50, "MIGRATION_HYSTERESIS_PERCENT");
            else if (key == "MIGRATION_COOLDOWN_MS")
                g_cfg.migrationCooldownMs = clampIntConfig(std::stoi(val), 0, 60000, "MIGRATION_COOLDOWN_MS");
            else if (key == "MIGRATION_DWELL_SEC")
                g_cfg.migrationDwellSec = clampIntConfig(std::stoi(val), 1, 60, "MIGRATION_DWELL_SEC");
            else if (key == "MIGRATION_PENALTY_BIG_LITTLE") {
                float v = parseFloatStrict(val, g_cfg.migrationPenaltyBigLittle, "MIGRATION_PENALTY_BIG_LITTLE");
                g_cfg.migrationPenaltyBigLittle = (v < 0.1f ? 0.1f : (v > 10.0f ? 10.0f : v));
            }
            else if (key == "MIGRATION_PENALTY_SAME") {
                float v = parseFloatStrict(val, g_cfg.migrationPenaltySame, "MIGRATION_PENALTY_SAME");
                g_cfg.migrationPenaltySame = (v < 0.1f ? 0.1f : (v > 10.0f ? 10.0f : v));
            }
            else if (key == "THREAD_ROLE_CONFIDENCE_THRESHOLD")
                g_cfg.threadRoleConfidenceThreshold = clampIntConfig(std::stoi(val), 0, 100, "THREAD_ROLE_CONFIDENCE_THRESHOLD");
            else if (key == "ML_DRIFT_DETECTION") g_cfg.mlDriftDetectionEnabled = parseFlexBool(val, g_cfg.mlDriftDetectionEnabled, "ML_DRIFT_DETECTION");
            else if (key == "ML_DRIFT_WINDOW")
                g_cfg.mlDriftWindow = clampIntConfig(std::stoi(val), 3, 100, "ML_DRIFT_WINDOW");
            else if (key == "ML_TOPOLOGY_BINDING") g_cfg.mlTopologyBindingEnabled = parseFlexBool(val, g_cfg.mlTopologyBindingEnabled, "ML_TOPOLOGY_BINDING");
            else if (key == "REFRESH_RATE_EVENT_DRIVEN") g_cfg.refreshRateEventDriven = parseFlexBool(val, g_cfg.refreshRateEventDriven, "REFRESH_RATE_EVENT_DRIVEN");
            else if (key == "STATE_CACHE_TTL_HOURS")
                g_cfg.stateCacheTtlHours = clampIntConfig(std::stoi(val), 1, 720, "STATE_CACHE_TTL_HOURS");
            else if (key == "PLACEMENT_MIN_GAIN_PCT")
                g_cfg.placementMinGainPercent = clampIntConfig(std::stoi(val), 0, 100, "PLACEMENT_MIN_GAIN_PCT");
            else if (key == "PLACEMENT_MIN_CONFIDENCE")
                g_cfg.placementMinConfidence = clampIntConfig(std::stoi(val), 0, 100, "PLACEMENT_MIN_CONFIDENCE");
            else if (key == "MIN_USEFUL_CAPACITY_RATIO")
                g_cfg.minUsefulCapacityRatio = std::max(0.0f, std::min(1.0f,
                    parseFloatStrict(val, g_cfg.minUsefulCapacityRatio, "MIN_USEFUL_CAPACITY_RATIO")));
            else { LOG_WARN("Unknown config key in aatpe.conf: %s (ignored)", key.c_str()); }
        } catch (...) {
            ++badKeyCount;
            LOG_WARN("Config parse error: key=%s val='%s' (kept previous running value, not default)",
                     key.c_str(), val.c_str());
        }
    }
    ::fclose(cfgFp);
    if (badKeyCount > 0) {
        LOG_WARN("Config load: %d key(s) failed to parse and kept their previous values", badKeyCount);
    }
    if (needsConfRewrite) {
        std::ifstream fr(confPath);
        std::vector<std::string> clean;
        std::string cline;
        while (std::getline(fr, cline)) {
            std::string c = cline;
            trim(c);
            if (!c.empty() && c[0] != '#') {
                size_t cq = c.find('=');
                if (cq != std::string::npos) {
                    std::string ck = c.substr(0, cq);
                    std::string cv = c.substr(cq + 1);
                    trim(ck);
                    if (cv.size() >= ck.size() + 1 &&
                        cv.compare(0, ck.size(), ck) == 0 && cv[ck.size()] == '=') {
                        cv = cv.substr(ck.size() + 1);
                        c = ck + "=" + cv;
                    }
                }
            }
            clean.push_back(c);
        }
        fr.close();
        std::string healed;
        healed.reserve(clean.size() * 80);
        for (const auto& l : clean) healed += l + "\n";
        if (writeFileAtomic(confPath.c_str(), healed)) {
            LOG_INFO("Config self-heal: atomically rewrote aatpe.conf with doubled prefixes stripped");
        } else {
            LOG_WARN("Config self-heal: atomic rewrite of aatpe.conf FAILED (keeping on-disk copy as-is)");
        }
    }
    {
        std::lock_guard<std::mutex> hmacLk(g_hmacMutex);
        g_hmacRequired = g_cfg.hmacRequired;
        g_hmacActive = g_hmacRequired;
    }
    loadPerAppConfigs(confPath.c_str());
    g_maxTrackedThreadsAtomic.store(g_cfg.maxTrackedThreads, std::memory_order_relaxed);
    validateConfigLocked();
    LOG_INFO("Config loaded: mode=%s, per-app overrides=%zu", g_cfg.controlMode.c_str(),
             g_perAppConfigs.size());
    lock.unlock();
    if (!pendingGlobalProfile.empty()) {
        aatpe_profile::profileRequest(pendingGlobalProfile, aatpe_profile::ProfileSource::GLOBAL);
        LOG_INFO("[CFG] CURRENT_PROFILE=%s re-voted GLOBAL via Profile Manager",
                 pendingGlobalProfile.c_str());
    }
}

void reloadConfig(void) {
    Config cfgBefore;
    snapshotConfig(cfgBefore);
    const int oldTlimCpu  = cfgBefore.thermalLimitCpu;
    const int oldTlimSkin = cfgBefore.thermalLimitSkin;
    const int oldTlimBatt = cfgBefore.thermalLimitBattery;
    int newTlimCpu = 0, newTlimSkin = 0, newTlimBatt = 0;
    std::string newControlMode;
    LOG_INFO("Config hot-reload: reloading aatpe.conf");
    LOG_INFO("[CFG] config hot-reload from %s", getConfigPath().c_str());
    loadConfig();
    const std::string committedProfile = aatpe_profile::effectiveProfile();
    {
        std::lock_guard<std::mutex> slock(g_cfgMutex);
        atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(), g_cfg.controlMode.c_str());
        writeFile((std::string(STATE_DIR) + "/log_level").c_str(), g_logLevel.load());
        atomicWriteFile((std::string(STATE_DIR) + "/current_profile").c_str(), committedProfile.c_str());
        writeFile((std::string(STATE_DIR) + "/battery_aging_enabled").c_str(),
                  g_cfg.batteryAgingProtection ? "1" : "0");
        writeFile((std::string(STATE_DIR) + "/ml_cold_start_enabled").c_str(),
                  g_cfg.mlColdStartProtection ? "1" : "0");
        writeFile((std::string(STATE_DIR) + "/ml_learning_enabled").c_str(),
                  g_cfg.mlLearningEnabled ? "1" : "0");
        writeFile((std::string(STATE_DIR) + "/ml_shadow_sampling_enabled").c_str(),
                  g_cfg.mlShadowSampling ? "1" : "0");
        writeFile((std::string(STATE_DIR) + "/thermal_limit_cpu").c_str(), g_cfg.thermalLimitCpu);
        writeFile((std::string(STATE_DIR) + "/thermal_limit_skin").c_str(), g_cfg.thermalLimitSkin);
        writeFile((std::string(STATE_DIR) + "/thermal_limit_battery").c_str(), g_cfg.thermalLimitBattery);
        newTlimCpu = g_cfg.thermalLimitCpu;
        newTlimSkin = g_cfg.thermalLimitSkin;
        newTlimBatt = g_cfg.thermalLimitBattery;
        newControlMode = g_cfg.controlMode;
    }
    {
        std::lock_guard<std::mutex> slock(g_cfgMutex);
        std::lock_guard<std::mutex> lock(g_mlMutex);
        g_mlModel.setEnabled(g_cfg.mlLearningEnabled != 0);
        g_mlModel.setMaxAlpha(g_cfg.mlMaxAlpha);
        g_mlModel.setInitialLR(g_cfg.mlInitialLR);
        g_mlModel.setDecay(g_cfg.mlDecay);
        g_mlModel.setWeightClamp(g_cfg.mlWeightClamp);
        g_mlModel.setMinSamples(g_cfg.mlMinSamples);
    }
    {
        std::lock_guard<std::mutex> slock(g_cfgMutex);
        std::lock_guard<std::mutex> hlock(g_hybridMutex);
        aatpe_ml_hybrid::instance().setEnabled(g_cfg.mlHybridEnabled);
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_enabled").c_str(), g_cfg.mlHybridEnabled ? "1" : "0");
    }
    {
        struct stat st;
        if (stat((std::string(CONF_DIR) + "/aatpe.conf").c_str(), &st) == 0)
            g_configMtime = st.st_mtime;
        if (stat(realConfPath().c_str(), &st) == 0)
            g_configMtimeReal = st.st_mtime;
    }
    LOG_INFO("Config hot-reload: thermal_limit_cpu %d->%d, skin %d->%d, battery %d->%d",
             oldTlimCpu, newTlimCpu,
             oldTlimSkin, newTlimSkin,
             oldTlimBatt, newTlimBatt);
    LOG_INFO("Config hot-reload: complete (control_mode=%s, log_level=%d)",
             newControlMode.c_str(), g_logLevel.load());
    fireReloadHooks();
}

bool isPackageInGameList(const std::string& pkg) {
    if (pkg.empty()) return false;
    std::lock_guard<std::mutex> slock(g_cfgMutex);
    if (g_cfg.gamePackages.empty()) return false;
    std::string pkgLower = pkg;
    std::transform(pkgLower.begin(), pkgLower.end(), pkgLower.begin(), ::tolower);
    for (const auto& p : g_cfg.gamePackages) {
        std::string pLower = p;
        std::transform(pLower.begin(), pLower.end(), pLower.begin(), ::tolower);
        if (pLower == pkgLower) return true;
    }
    return false;
}

void reloadConfigIfChanged() {
    g_configMtimeCheckCounter++;
    if (g_configMtimeCheckCounter < 50) return;
    g_configMtimeCheckCounter = 0;

    bool changed = false;
    struct stat st;
    {
        std::string confPath = std::string(CONF_DIR) + "/aatpe.conf";
        if (stat(confPath.c_str(), &st) == 0) {
            if (g_configMtime != 0 && st.st_mtime != g_configMtime) { changed = true; }
            g_configMtime = st.st_mtime;
        }
    }
    {
        std::string realPath = realConfPath();
        if (stat(realPath.c_str(), &st) == 0) {
            if (g_configMtimeReal != 0 && st.st_mtime != g_configMtimeReal) { changed = true; }
            g_configMtimeReal = st.st_mtime;
        }
    }

    if (!changed) return;
    LOG_INFO("[CONFIG] reloaded due to file change (mtime polling, both paths)");
    reloadConfig();
}

void loadProfile(const std::string& profile) {
    if (profile.find('/') != std::string::npos || profile.find("..") != std::string::npos) {
        LOG_WARN("loadProfile: rejected unsafe profile name: %s", profile.c_str());
        return;
    }
    if (profile.find('\0') != std::string::npos) {
        LOG_WARN("loadProfile: rejected profile name containing embedded NUL byte");
        return;
    }
    std::unique_lock<std::mutex> lock(g_cfgMutex);
    std::string pfile = realConfDir() + "/profiles/" + profile + ".conf";
    std::ifstream f(pfile);
    if (!f) {
        pfile = std::string(CONF_DIR) + "/profiles/" + profile + ".conf";
        f.open(pfile);
    }
    if (!f) {
        LOG_WARN("Profile config not found: %s (applying profile name only)", pfile.c_str());
    } else {
        std::string line;
        while (std::getline(f, line)) {
            trim(line);
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key); trim(val);
            for (auto& c : key) c = (char)toupper((unsigned char)c);
            try {
                if (key == "CONTROL_MODE") {
                    LOG_INFO("Profile %s: ignoring CONTROL_MODE=%s (user control mode is preserved)",
                             profile.c_str(), val.c_str());
                }
                else if (key == "DECISION_INTERVAL")
                    g_cfg.decisionInterval = clampIntConfig(std::stoi(val), 10, 5000, "DECISION_INTERVAL");
                else if (key == "DECISION_MIN_INTERVAL_MS")
                    g_cfg.decisionMinIntervalMs = clampIntConfig(std::stoi(val), 0, 1000, "DECISION_MIN_INTERVAL_MS");
                else if (key == "DECISION_IDLE_INTERVAL_MS")
                    g_cfg.decisionIdleIntervalMs = clampIntConfig(std::stoi(val), 10, 5000, "DECISION_IDLE_INTERVAL_MS");
                else if (key == "CONFIDENCE_THRESHOLD") g_cfg.confidenceThreshold = clampIntConfig(std::stoi(val), 0, 100, "CONFIDENCE_THRESHOLD");
                else if (key == "MIGRATION_COST_THRESHOLD") g_cfg.migrationCostThreshold = clampIntConfig(std::stoi(val), 0, 10000, "MIGRATION_COST_THRESHOLD");
                else if (key == "MAX_MIGRATIONS_PER_SEC") g_cfg.maxMigrationsPerSec = clampIntConfig(std::stoi(val), 1, 1000, "MAX_MIGRATIONS_PER_SEC");
            else if (key == "UCLAMP_PASSIVE_PCT") g_cfg.uclampPassivePct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_PASSIVE_PCT");
            else if (key == "UCLAMP_CRITICAL_MIN_PCT") g_cfg.uclampCriticalMinPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_CRITICAL_MIN_PCT");
            else if (key == "UCLAMP_CRITICAL_MAX_PCT") g_cfg.uclampCriticalMaxPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_CRITICAL_MAX_PCT");
            else if (key == "UCLAMP_ASSIST_MIN_PCT") g_cfg.uclampAssistMinPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_ASSIST_MIN_PCT");
            else if (key == "UCLAMP_SF_PCT") g_cfg.uclampSfPct = clampIntConfig(std::stoi(val), 0, 100, "UCLAMP_SF_PCT");
            else if (key == "INPUT_BOOST_MS_60") g_cfg.inputBoostMs60 = clampIntConfig(std::stoi(val), 10, 500, "INPUT_BOOST_MS_60");
            else if (key == "INPUT_BOOST_MS_120") g_cfg.inputBoostMs120 = clampIntConfig(std::stoi(val), 10, 500, "INPUT_BOOST_MS_120");
            else if (key == "INPUT_BOOST_MS_144") g_cfg.inputBoostMs144 = clampIntConfig(std::stoi(val), 10, 500, "INPUT_BOOST_MS_144");
            else if (key == "SCHEDTUNE_CRITICAL_BOOST") g_cfg.schedtuneCriticalBoost = clampIntConfig(std::stoi(val), 0, 100, "SCHEDTUNE_CRITICAL_BOOST");
            else if (key == "SCHEDTUNE_PASSIVE_BOOST") g_cfg.schedtunePassiveBoost = clampIntConfig(std::stoi(val), 0, 100, "SCHEDTUNE_PASSIVE_BOOST");
            else if (key == "SCHEDTUNE_ASSIST_BOOST") g_cfg.schedtuneAssistBoost = clampIntConfig(std::stoi(val), 0, 100, "SCHEDTUNE_ASSIST_BOOST");
                else if (key == "MAX_MIGRATIONS_PER_CYCLE") g_cfg.maxMigrationsPerCycle = clampIntConfig(std::stoi(val), 1, 64, "MAX_MIGRATIONS_PER_CYCLE");
                else if (key == "THERMAL_ENABLED") g_cfg.thermalEnabled = parseStrictBool(val, g_cfg.thermalEnabled ? 1 : 0, "THERMAL_ENABLED") != 0;
                else if (key == "C4_FREEZER_ENABLED") g_cfg.c4FreezerEnabled = parseStrictBool(val, g_cfg.c4FreezerEnabled ? 1 : 0, "C4_FREEZER_ENABLED") != 0;
                else if (key == "EBPF_EVENT_DRIVEN") g_cfg.ebpfEnabled = parseStrictBool(val, g_cfg.ebpfEnabled ? 1 : 0, "EBPF_EVENT_DRIVEN") != 0;
                else if (key == "FEDERATED_ENABLED") g_cfg.federatedEnabled = parseStrictBool(val, g_cfg.federatedEnabled ? 1 : 0, "FEDERATED_ENABLED") != 0;
                else if (key == "THERMAL_LIMIT_CPU") g_cfg.thermalLimitCpu = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_LIMIT_CPU");
                else if (key == "THERMAL_LIMIT_SKIN") g_cfg.thermalLimitSkin = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_LIMIT_SKIN");
                else if (key == "THERMAL_LIMIT_BATTERY") g_cfg.thermalLimitBattery = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_LIMIT_BATTERY");
                else if (key == "THERMAL_EMERGENCY") {
                    LOG_WARN("Config key THERMAL_EMERGENCY is deprecated; use THERMAL_EMERGENCY_THRESHOLD (new key takes precedence when > 0).");
                    g_cfg.thermalEmergency = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_EMERGENCY");
                }
                else if (key == "FRAME_AWARE") g_cfg.frameAware = parseStrictBool(val, g_cfg.frameAware ? 1 : 0, "FRAME_AWARE") != 0;
                else if (key == "TARGET_FRAME_TIME") g_cfg.targetFrameTime = clampIntConfig(std::stoi(val), 1000, 100000, "TARGET_FRAME_TIME");
                else if (key == "FRAME_CRITICAL_THRESHOLD") g_cfg.frameCriticalThreshold = clampIntConfig(std::stoi(val), 0, 50000, "FRAME_CRITICAL_THRESHOLD");
                else if (key == "JANK_THRESHOLD_PERCENT") g_cfg.jankThresholdPercent = clampFloatConfig(parseFloatStrict(val, g_cfg.jankThresholdPercent, "JANK_THRESHOLD_PERCENT"), 0.0f, 100.0f, "JANK_THRESHOLD_PERCENT");
                else if (key == "LEARNING_ENABLED") g_cfg.learningEnabled = parseStrictBool(val, g_cfg.learningEnabled ? 1 : 0, "LEARNING_ENABLED") != 0;
                else if (key == "HISTORY_WEIGHT") g_cfg.historyWeight = clampIntConfig(std::stoi(val), 0, 100, "HISTORY_WEIGHT");
                else if (key == "LEARNING_DECAY") g_cfg.learningDecay = clampIntConfig(std::stoi(val), 0, 100, "LEARNING_DECAY");
                else if (key == "HISTORY_MIN_SAMPLES") g_cfg.minSamplesForLearning = clampIntConfig(std::stoi(val), 1, 10000, "HISTORY_MIN_SAMPLES");
                else if (key == "THREAD_SCAN_INTERVAL") g_cfg.threadScanInterval = clampIntConfig(std::stoi(val), 1, 10000, "THREAD_SCAN_INTERVAL");
                else if (key == "MAX_TRACKED_THREADS") g_cfg.maxTrackedThreads = clampIntConfig(std::stoi(val), 1, 10000, "MAX_TRACKED_THREADS");
                else if (key == "IGNORE_SYSTEM_THREADS") g_cfg.ignoreSystemThreads = parseStrictBool(val, g_cfg.ignoreSystemThreads ? 1 : 0, "IGNORE_SYSTEM_THREADS") != 0;
                else if (key == "WATCHDOG_ENABLED") g_cfg.watchdogEnabled = parseStrictBool(val, g_cfg.watchdogEnabled ? 1 : 0, "WATCHDOG_ENABLED") != 0;
                else if (key == "WATCHDOG_INTERVAL") g_cfg.watchdogInterval = clampIntConfig(std::stoi(val), 1, 600, "WATCHDOG_INTERVAL");
                else if (key == "SAFETY_MIGRATION_LIMIT") g_cfg.safetyMigrationLimit = clampIntConfig(std::stoi(val), 1, 10000, "SAFETY_MIGRATION_LIMIT");
                else if (key == "SAFETY_LATENCY_LIMIT") g_cfg.safetyLatencyLimit = clampIntConfig(std::stoi(val), 1, 100000, "SAFETY_LATENCY_LIMIT");
                else if (key == "SAFETY_LOAD_LIMIT") g_cfg.safetyLoadLimit = clampIntConfig(std::stoi(val), 1, 1000, "SAFETY_LOAD_LIMIT");
                else if (key == "LOG_LEVEL") { g_cfg.logLevel = clampIntConfig(std::stoi(val), 0, 5, "LOG_LEVEL"); g_logLevel.store(g_cfg.logLevel); }
                else if (key == "LOG_DECISIONS") g_cfg.logDecisions = clampIntConfig(std::stoi(val), 0, 1, "LOG_DECISIONS");
                else if (key == "LOG_THERMAL") g_cfg.logThermal = clampIntConfig(std::stoi(val), 0, 1, "LOG_THERMAL");
                else if (key == "LOG_FRAME") g_cfg.logFrame = clampIntConfig(std::stoi(val), 0, 1, "LOG_FRAME");
                else if (key == "AUTO_PROFILE") {
                    LOG_INFO("Profile %s: ignoring AUTO_PROFILE=%s (user autoProfile lock preserved)",
                             profile.c_str(), val.c_str());
                }
                else if (key == "GAME_DETECTION") g_cfg.gameDetection = clampIntConfig(std::stoi(val), 0, 1, "GAME_DETECTION");
                else if (key == "REFRESH_RATE_MODE") {
                    if (val == "peak" || val == "current" || val == "auto") g_cfg.refreshRateMode = val;
                    else { LOG_WARN("REFRESH_RATE_MODE invalid: '%s' (using default 'peak')", val.c_str()); g_cfg.refreshRateMode = "peak"; }
                }
                else if (key == "BATTERY_SAVER_THRESHOLD") g_cfg.batterySaverThreshold = clampIntConfig(std::stoi(val), 0, 100, "BATTERY_SAVER_THRESHOLD");
                else if (key == "THERMAL_PROFILE_THRESHOLD") g_cfg.thermalProfileThreshold = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_PROFILE_THRESHOLD");
                else if (key == "THERMAL_WARNING_THRESHOLD")    g_cfg.thermalWarningThreshold    = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_WARNING_THRESHOLD");
                else if (key == "THERMAL_CRITICAL_THRESHOLD")   g_cfg.thermalCriticalThreshold   = clampIntConfig(std::stoi(val), 0, 200, "THERMAL_CRITICAL_THRESHOLD");
                else if (key == "THERMAL_EMERGENCY_THRESHOLD") {
                    LOG_INFO("Profile %s: ignoring THERMAL_EMERGENCY_THRESHOLD=%s (global safety limit preserved at %d)",
                             profile.c_str(), val.c_str(), g_cfg.thermalEmergencyThreshold);
                }
                else if (key == "LAYER_CRITICAL_THRESHOLD") g_cfg.layerCriticalThreshold = clampIntConfig(std::stoi(val), 0, 1000, "LAYER_CRITICAL_THRESHOLD");
                else if (key == "DAEMON_MEMORY_WARN_KB") g_cfg.daemonMemoryWarnKb = clampIntConfig(std::stoi(val), 1024, 1048576, "DAEMON_MEMORY_WARN_KB");
                else if (key == "CPU_USAGE_LIMIT") g_cfg.cpuUsageLimit = clampIntConfig(std::stoi(val), 10, 100, "CPU_USAGE_LIMIT");
                else if (key == "LITTLE_POOL") g_cfg.littlePool = val;
                else if (key == "MID_POOL") g_cfg.midPool = val;
                else if (key == "BIG_POOL") g_cfg.bigPool = val;
                else if (key == "ML_ENABLED") g_cfg.mlLearningEnabled = parseFlexBool(val, g_cfg.mlLearningEnabled, "ML_ENABLED");
                else if (key == "ML_LEARNING_ENABLED") g_cfg.mlLearningEnabled = clampIntConfig(std::stoi(val), 0, 1, "ML_LEARNING_ENABLED");
                else if (key == "ML_SHADOW_SAMPLING") g_cfg.mlShadowSampling = parseStrictBool(val, g_cfg.mlShadowSampling ? 1 : 0, "ML_SHADOW_SAMPLING") != 0;
                else if (key == "ML_MAX_ALPHA") g_cfg.mlMaxAlpha = parseFloatStrict(val, g_cfg.mlMaxAlpha, "ML_MAX_ALPHA");
                else if (key == "ML_INITIAL_LR") g_cfg.mlInitialLR = parseFloatStrict(val, g_cfg.mlInitialLR, "ML_INITIAL_LR");
                else if (key == "ML_DECAY") g_cfg.mlDecay = parseFloatStrict(val, g_cfg.mlDecay, "ML_DECAY");
                else if (key == "ML_SAVE_INTERVAL") g_cfg.mlSaveInterval = clampIntConfig(std::stoi(val), 1, 3600, "ML_SAVE_INTERVAL");
                else if (key == "ML_WEIGHT_CLAMP") g_cfg.mlWeightClamp = parseFloatStrict(val, g_cfg.mlWeightClamp, "ML_WEIGHT_CLAMP");
                else if (key == "ML_HYBRID_ENABLED") g_cfg.mlHybridEnabled = parseFlexBool(val, g_cfg.mlHybridEnabled, "ML_HYBRID_ENABLED");
                else if (key == "ML_COLD_START_PROTECTION") g_cfg.mlColdStartProtection = parseFlexBool(val, g_cfg.mlColdStartProtection, "ML_COLD_START_PROTECTION");
                else if (key == "ML_MIN_SAMPLES") g_cfg.mlMinSamples = clampIntConfig(std::stoi(val), 1, 1000000, "ML_MIN_SAMPLES");
                else if (key == "ML_OUTCOME_MAD_FILTER") g_cfg.mlOutcomeMadFilter = clampIntConfig(std::stoi(val), 0, 1, "ML_OUTCOME_MAD_FILTER");
                else if (key == "ML_DRIFT_RELEARN") g_cfg.mlDriftRelearn = clampIntConfig(std::stoi(val), 0, 1, "ML_DRIFT_RELEARN");
                else if (key == "ML_EPSILON_GREEDY") g_cfg.mlEpsilonGreedy = clampDoubleConfig(parseDoubleStrict(val, g_cfg.mlEpsilonGreedy, "ML_EPSILON_GREEDY"), 0.0, 0.05, "ML_EPSILON_GREEDY");
                else { LOG_WARN("Unknown profile key in %s: %s (ignored)", pfile.c_str(), key.c_str()); }
            } catch (...) {
                LOG_WARN("Profile parse error: key=%s val='%s'", key.c_str(), val.c_str());
            }
        }
    }
    g_currentProfile = profile;
    if (g_cfg.refreshRateMode == "current") g_refreshRateModeEnum = RefreshRateMode::CURRENT;
    else if (g_cfg.refreshRateMode == "auto") g_refreshRateModeEnum = RefreshRateMode::AUTO;
    else g_refreshRateModeEnum = RefreshRateMode::PEAK;
    if (profile == "balanced") g_currentProfileEnum = ProfileType::BALANCED;
    else if (profile == "gaming") g_currentProfileEnum = ProfileType::GAMING;
    else if (profile == "battery") g_currentProfileEnum = ProfileType::BATTERY;
    else if (profile == "thermal") g_currentProfileEnum = ProfileType::THERMAL;
    else g_currentProfileEnum = ProfileType::UNKNOWN;
    atomicWriteFile((std::string(STATE_DIR) + "/current_profile").c_str(), profile);
    aatpe::c4freezer::setC4FreezerEnabled(g_cfg.c4FreezerEnabled);
    LOG_INFO("Profile loaded: %s", profile.c_str());
}

void persistConfigKeyValue(const char* key, const char* value) {
    if (!key || !*key || !value) return;
    for (const char* p = value; *p; ++p) {
        if (*p == '\n' || *p == '\r') {
            LOG_WARN("persistConfigKeyValue: rejecting value with embedded newline for key=%s", key);
            return;
        }
    }
    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
    std::string confPath = getConfigPath();
    std::ifstream f(confPath);
    std::vector<std::string> lines;
    bool found = false;
    std::string keyStr = key;
    for (auto& c : keyStr) c = (char)toupper((unsigned char)c);
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            std::string trimmed = line;
            trim(trimmed);
            if (!trimmed.empty() && trimmed[0] != '#') {
                size_t eq = trimmed.find('=');
                if (eq != std::string::npos) {
                    std::string existingKey = trimmed.substr(0, eq);
                    trim(existingKey);
                    for (auto& c : existingKey) c = (char)toupper((unsigned char)c);
                    if (existingKey == keyStr) {
                        size_t actualEq = line.find('=');
                        if (actualEq != std::string::npos) {
                            line = line.substr(0, actualEq + 1) + value;
                        } else {
                            line = keyStr + "=" + value;
                        }
                        found = true;
                    }
                }
            }
            lines.push_back(line);
        }
        f.close();
    }
    if (!found) {
        lines.push_back(std::string(key) + "=" + value);
    }
    std::string content;
    content.reserve(lines.size() * 64);
    for (const auto& l : lines) {
        content += l;
        content += '\n';
    }
    if (writeFileAtomic(confPath.c_str(), content)) {
        LOG_INFO("Config persisted: %s=%s", key, value);
    } else {
        LOG_WARN("persistConfigKeyValue: writeFileAtomic failed for %s (errno=%d, %s) — previous config preserved",
                 confPath.c_str(), errno, aatpeErrStr(errno));
    }
}

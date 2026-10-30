#include "aatped.h"
#include "aatpe_ml_hybrid.h"
#include "aatpe_command_dispatcher.h"
#include "aatpe_command_queue.h"
#include "c4_freezer.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <algorithm>

using namespace aatpe;
using namespace aatpe_ml_hybrid;

extern std::mutex g_hybridMutex;

std::mutex g_autoProfileMutex;

std::mutex g_userControlModeMutex;
std::string g_userControlMode = "assist";

std::string getUserControlMode() {
    std::lock_guard<std::mutex> lk(g_userControlModeMutex);
    return g_userControlMode;
}
void setUserControlMode(const std::string& m) {
    std::lock_guard<std::mutex> lk(g_userControlModeMutex);
    g_userControlMode = m;
}

void restoreUserControlMode() {
    std::string userMode = getUserControlMode();
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        g_cfg.controlMode = userMode;
    }
    atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(), userMode);
    atomicWriteFile((std::string(STATE_DIR) + "/control_mode_reason").c_str(),
                    std::string("restored_after_crash_window"));
    LOG_INFO("ROUND-007: control mode restored to user choice: %s", userMode.c_str());
}

void signalHandler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_shouldExit.store(1, std::memory_order_relaxed);
    } else if (sig == SIGUSR1) {
        g_reloadProfile.store(1, std::memory_order_relaxed);
    }
}

static void crashSignalHandler(int sig) {
    const char* sigName = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sigName = "SIGSEGV"; break;
        case SIGABRT: sigName = "SIGABRT"; break;
        case SIGFPE:  sigName = "SIGFPE";  break;
        case SIGILL:  sigName = "SIGILL";  break;
        case SIGBUS:  sigName = "SIGBUS";  break;
    }
    char pathBuf[160];
    int pi = 0;
    for (; STATE_DIR[pi] != '\0' && pi < (int)sizeof(pathBuf) - 1; pi++)
        pathBuf[pi] = STATE_DIR[pi];
    const char* suffix = "/crash_signal";
    for (int si = 0; suffix[si] != '\0' && pi < (int)sizeof(pathBuf) - 1; si++, pi++)
        pathBuf[pi] = suffix[si];
    pathBuf[pi] = '\0';
    int fd = open(pathBuf, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        const char* msg = sigName;
        size_t len = 0;
        while (msg[len]) len++;
        ssize_t w = write(fd, msg, len);
        (void)w;
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void installCrashHandlers() {
    umask(0077);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crashSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
}

static bool isAllDigits(const char* s) {
    if (!s || !*s) return false;
    size_t slen = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        slen++;
    }
    if (slen > 10) return false;
    return true;
}

bool isValidControlChars(const std::string& line) {
    if (line.empty() || line.size() > 256) return false;
    for (char c : line) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9') ||
              c == '_' || c == '=' || c == '.' || c == '-' || c == '&')) {
            return false;
        }
    }
    return true;
}

static bool parseNonNegChecked(const char* s, int& out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return false;
    if (v < 0 || v > INT_MAX) return false;
    out = (int)v;
    return true;
}

void processControlCommands() {
    {
        std::string cmdPath = std::string(RUN_DIR) + "/control_cmd";
        std::string staging = cmdPath + ".processing";
        if (::rename(cmdPath.c_str(), staging.c_str()) == 0) {
            constexpr std::streamoff kMaxControlCmd = std::streamoff(64) * 1024;
            std::string content;
            bool oversized = false;
            std::streamoff fileSize = 0;
            {
                std::ifstream cf(staging);
                if (cf) {
                    cf.seekg(0, std::ios::end);
                    fileSize = cf.tellg();
                    if (fileSize > kMaxControlCmd) {
                        oversized = true;
                    } else {
                        cf.seekg(0, std::ios::beg);
                        content.assign(std::istreambuf_iterator<char>(cf),
                                       std::istreambuf_iterator<char>());
                    }
                }
            }
            if (oversized) {
                LOG_WARN("CONTROL: control_cmd oversized (%lld bytes > 64KB), discarding",
                         (long long)fileSize);
                ::unlink(staging.c_str());
            } else if (!content.empty()) {
                std::ofstream out(std::string(RUN_DIR) + "/control", std::ios::app);
                if (out) {
                    out << content;
                    if (content.back() != '\n') out << '\n';
                    out.close();
                    ::unlink(staging.c_str());
                } else {
                    LOG_WARN("CONTROL: cannot append to /control (%s); requeueing control_cmd",
                             aatpeErrStr(errno));
                    ::rename(staging.c_str(), cmdPath.c_str());
                }
            } else {
                ::unlink(staging.c_str());
            }
        }
    }
    std::string ctrlPath = std::string(RUN_DIR) + "/control";
    std::string ctrlStaging = ctrlPath + ".processing";
    if (::rename(ctrlPath.c_str(), ctrlStaging.c_str()) != 0) {
        (void)aatpe_cmdq::commandQueueDrain();
        return;
    }
    std::ifstream f(ctrlStaging);
    if (!f) {
        ::unlink(ctrlStaging.c_str());
        (void)aatpe_cmdq::commandQueueDrain();
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty()) continue;
        if (line.size() > 256) {
            LOG_WARN("CONTROL: rejected (length=%zu > 256)", line.size());
            continue;
        }
        if (!isValidControlChars(line)) {
            LOG_WARN("CONTROL: rejected (invalid characters, only [a-z0-9_=.-&] allowed)");
            continue;
        }
        int eqCount = 0;
        for (char c : line) if (c == '=') eqCount++;
        if (eqCount > 3) {
            LOG_WARN("CONTROL: rejected (too many '=' separators: %d > 3)", eqCount);
            continue;
        }
        {
            std::string actualCmd = line;
            bool hasSig = false;
            std::string sig;
            size_t sigPos = line.rfind(".sig=");
            if (sigPos != std::string::npos && sigPos > 0) {
                hasSig = true;
                sig = line.substr(sigPos + 5);
                actualCmd = line.substr(0, sigPos);
            }
            if (hasSig) {
                if (!verifyCommandHmac(actualCmd.c_str(), sig.c_str())) {
                    LOG_WARN("CONTROL: HMAC verification FAILED for '%s' (sig len=%zu)",
                             actualCmd.c_str(), sig.size());
                    continue;
                }
                line = actualCmd;
            }
        }
        LOG_INFO("CONTROL: '%s'", line.c_str());
aatpe::CommandDispatcher::instance().dispatch(line, aatpe::DispatchAuth::ROOT);
    }
    f.close();
    ::unlink(ctrlStaging.c_str());

    (void)aatpe_cmdq::commandQueueDrain();
}

bool setRuntimeControlMode(const std::string& mode, const std::string& reason) {
    SystemState targetState;
    bool enterSafeLock;
    if (mode == "active") {
        targetState = SystemState::NORMAL;
        enterSafeLock = false;
    } else if (mode == "assist") {
        targetState = SystemState::NORMAL;
        enterSafeLock = false;
    } else if (mode == "passive") {
        targetState = SystemState::PASSIVE;
        enterSafeLock = true;
    } else if (mode == "emergency") {
        targetState = SystemState::EMERGENCY;
        enterSafeLock = true;
    } else {
        LOG_WARN("setRuntimeControlMode: invalid mode '%s' (must be active/assist/passive/emergency)",
                 mode.c_str());
        return false;
    }

    bool isEmergency = (mode == "emergency");
    if (!isEmergency &&
        targetState == SystemState::PASSIVE &&
        g_userModeLock.load(std::memory_order_relaxed)) {
        LOG_WARN("[P1-1] control_mode safety downgrade '%s' (%s) IGNORED: user manually locked mode (active/assist); thermal emergency still overrides",
                 mode.c_str(), reason.c_str());
        atomicWriteFile((std::string(STATE_DIR) + "/control_mode_reason").c_str(),
                        std::string("ignored_user_lock:") + (reason.empty() ? "unknown" : reason));
        return false;
    }

    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        g_cfg.controlMode = mode;
    }

    g_watchdogPassiveLocked.store(enterSafeLock, std::memory_order_relaxed);

    transitionToState(targetState, reason.c_str());

    std::string modeForFile;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        modeForFile = g_cfg.controlMode;
    }
    atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(), modeForFile);

    atomicWriteFile((std::string(STATE_DIR) + "/control_mode_reason").c_str(),
                    reason.empty() ? std::string("unknown") : reason);

    LOG_WARN("[P1-1] control_mode -> %s (reason=%s, watchdog_lock=%d, state=%s)",
             mode.c_str(), reason.c_str(),
             enterSafeLock ? 1 : 0,
             systemStateToString(getSystemState()));
    return true;
}

ControlResult executeControlLine(const std::string& line) {
        ControlResult res;
        if (line.rfind("set_mode=", 0) == 0) {
            res.success = true;
            std::string mode = line.substr(9);
            trim(mode);
            if (mode == "powersave" || mode == "passive") {
                g_userModeLock.store(false, std::memory_order_relaxed);
                transitionToState(SystemState::PASSIVE, "user_set_mode_passive");
            } else if (mode == "assist" || mode == "active" || mode == "normal") {
                g_watchdogPassiveLocked.store(false, std::memory_order_relaxed);
                g_userModeLock.store(true, std::memory_order_relaxed);
                bool ok = transitionToState(SystemState::NORMAL, "user_set_mode_assist");
                if (!ok) {
                    LOG_WARN("set_mode=normal: transition to NORMAL blocked (state=%d), recording controlMode=assist intent",
                             (int)getSystemState());
                }
            } else if (mode == "emergency") {
                res.success = false;
                res.error_code = ERR_INVALID_ARGUMENT;
                res.message = "emergency is internal-only (watchdog/thermal latch), not user-settable";
                LOG_WARN("set_mode=emergency rejected: internal-only, keeping current mode");
            } else {
                std::string curMode;
                { std::lock_guard<std::mutex> cl(g_cfgMutex); curMode = g_cfg.controlMode; }
                res.success = false;
                res.error_code = ERR_INVALID_ARGUMENT;
                res.message = "invalid control mode (user-selectable: passive/assist/active)";
                LOG_WARN("Invalid set_mode value: '%s', keeping current: %s", mode.c_str(), curMode.c_str());
            }
            if (res.success) {
                std::string mappedMode;
                if (mode == "powersave" || mode == "passive") mappedMode = "passive";
                else if (mode == "assist") mappedMode = "assist";
                else if (mode == "active") mappedMode = "active";
                else if (mode == "normal") mappedMode = "assist";
                if (!mappedMode.empty()) {
                    setUserControlMode(mappedMode);
                    bool inEmergency = (getSystemState() == SystemState::EMERGENCY);
                    std::lock_guard<std::mutex> cl(g_cfgMutex);
                    g_cfg.controlMode = mappedMode;
                    if (inEmergency) {
                        g_emergencyUserOverride.store(true, std::memory_order_relaxed);
                    }
                    if (!inEmergency) {
                        atomicWriteFile(
                            (std::string(STATE_DIR) + "/control_mode_effective").c_str(),
                            mappedMode);
                    }
                }
            }
            std::string finalControlMode;
            { std::lock_guard<std::mutex> cl(g_cfgMutex); finalControlMode = g_cfg.controlMode; }
            atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(), finalControlMode);
            persistConfigKeyValue("CONTROL_MODE", finalControlMode.c_str());
            LOG_INFO("[CFG] control_mode=%s (persisted to aatpe.conf)", finalControlMode.c_str());
        }
        else if (line.rfind("set_control_mode=", 0) == 0) {
            std::string payload = line.substr(17);
            std::string mode, reason;
            size_t amp = payload.find('&');
            if (amp != std::string::npos) {
                mode = payload.substr(0, amp);
                std::string rest = payload.substr(amp + 1);
                if (rest.rfind("reason=", 0) == 0) {
                    reason = rest.substr(7);
                }
            } else {
                mode = payload;
            }
            trim(mode);
            trim(reason);
            bool ok = setRuntimeControlMode(mode, reason);
            if (ok) {
                res.success = true;
                res.applied_value = mode;
            } else {
                res.success = false;
                res.error_code = ERR_INVALID_ARGUMENT;
                res.message = "invalid control mode (must be active/assist/passive/emergency)";
            }
        }
        else if (line == "unlock_watchdog") {
        res.success = true;
            bool wasLocked = isWatchdogPassiveLocked();
            g_watchdogPassiveLocked.store(false, std::memory_order_relaxed);
            LOG_WARN("WATCHDOG: manual unlock_watchdog command received, wasLocked=%d",
                     wasLocked ? 1 : 0);
            if (getSystemState() == SystemState::PASSIVE) {
                bool ok = transitionToState(SystemState::NORMAL, "manual_unlock_watchdog");
                if (ok) {
                    LOG_WARN("WATCHDOG: PASSIVE -> NORMAL transitioned after manual unlock");
                } else {
                    LOG_WARN("WATCHDOG: still cannot leave PASSIVE after unlock "
                             "(priority/other block), current state=%d",
                             (int)getSystemState());
                }
            }
            atomicWriteFile((std::string(STATE_DIR) + "/watchdog_passive_locked").c_str(),
                            "0");
        }
        else if (line.rfind("set_profile=", 0) == 0) {
        res.success = true;
            std::string prof = line.substr(12);
            trim(prof);
            if (prof == "balanced" || prof == "gaming" || prof == "battery" || prof == "thermal") {
                aatpe_profile::profileRequest(prof, aatpe_profile::ProfileSource::MANUAL);
                g_profileLock.store(true, std::memory_order_relaxed);
                aatpe_profile::profileRelease(aatpe_profile::ProfileSource::AUTO);
                {
                    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
                    g_cfg.autoProfile = 0;
                }
                atomicWriteFile((std::string(STATE_DIR) + "/profile_lock").c_str(), 1);
                atomicWriteFile((std::string(STATE_DIR) + "/auto_profile").c_str(), 0);
                persistConfigKeyValue("CURRENT_PROFILE", prof.c_str());
                persistConfigKeyValue("AUTO_PROFILE", "0");
            } else {
                res.success = false;
                LOG_WARN("Invalid set_profile value: '%s' (must be balanced/gaming/battery/thermal), keeping current: %s", prof.c_str(), aatpe_profile::effectiveProfile().c_str());
            }
        }
        else if (line.rfind("set_auto_profile=", 0) == 0) {
        res.success = true;
            const char* autoVal = line.c_str() + 17;
            if (!isAllDigits(autoVal)) {
                int curAuto;
                { std::lock_guard<std::mutex> cl(g_cfgMutex); curAuto = g_cfg.autoProfile; }
                res.success = false;
                LOG_WARN("Invalid set_auto_profile value: '%s' (must be integer 0 or 1), keeping current: %d", autoVal, curAuto);
            } else {
                int val = 0;
                if (!parseNonNegChecked(autoVal, val)) {
                    int curAuto;
                    { std::lock_guard<std::mutex> cl(g_cfgMutex); curAuto = g_cfg.autoProfile; }
                    res.success = false;
                    LOG_WARN("Invalid set_auto_profile value: '%s' (overflow / must be 0 or 1), keeping current: %d", autoVal, curAuto);
                } else if (val != 0 && val != 1) {
                    int curAuto;
                    { std::lock_guard<std::mutex> cl(g_cfgMutex); curAuto = g_cfg.autoProfile; }
                    res.success = false;
                    LOG_WARN("Invalid set_auto_profile value: '%s' (must be exactly 0 or 1), keeping current: %d", autoVal, curAuto);
                } else {
                    int writtenAuto;
                    {
                        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
                        g_cfg.autoProfile = (val != 0) ? 1 : 0;
                        writtenAuto = g_cfg.autoProfile;
                    }
                    g_profileLock.store((val == 0), std::memory_order_relaxed);
                    atomicWriteFile((std::string(STATE_DIR) + "/auto_profile").c_str(), writtenAuto);
                    atomicWriteFile((std::string(STATE_DIR) + "/profile_lock").c_str(),
                                    g_profileLock.load(std::memory_order_relaxed) ? 1 : 0);
                    if (writtenAuto) {
                        aatpe_profile::profileRelease(aatpe_profile::ProfileSource::MANUAL);
                    } else {
                        aatpe_profile::profileRelease(aatpe_profile::ProfileSource::AUTO);
                    }
                    persistConfigKeyValue("AUTO_PROFILE", writtenAuto ? "1" : "0");
                }
            }
        }
        else if (line.rfind("set_log_level=", 0) == 0) {
        res.success = true;
            const char* logVal = line.c_str() + 14;
            if (!isAllDigits(logVal)) {
                res.success = false;
                LOG_WARN("Invalid set_log_level value: '%s' (must be integer 0-5), keeping current: %d", logVal, g_logLevel.load());
            } else {
                int lv = 0;
                if (!parseNonNegChecked(logVal, lv)) {
                    res.success = false;
                    LOG_WARN("Invalid set_log_level value: '%s' (overflow, must be 0-5), keeping current: %d", logVal, g_logLevel.load());
                } else if (lv >= 0 && lv <= 5) {
                    g_logLevel.store(lv);
                    { std::lock_guard<std::mutex> cfgLock(g_cfgMutex); g_cfg.logLevel = lv; }
                    writeFile((std::string(STATE_DIR) + "/log_level").c_str(), g_logLevel.load());
                    persistConfigKeyValue("LOG_LEVEL", std::to_string(lv).c_str());
                } else {
                    res.success = false;
                    LOG_WARN("Invalid set_log_level value: %d (must be 0-5), keeping current: %d", lv, g_logLevel.load());
                }
            }
        }
        else if (line.rfind("game_add=", 0) == 0) {
            std::string pkg = line.substr(9);
            if (aatpe::gamelist::addManualGame(pkg)) {
                res.success = true;
                LOG_INFO("game_add: %s (manual override list updated)", pkg.c_str());
            } else {
                res.success = false;
                res.message = "invalid package name";
                LOG_WARN("game_add rejected: '%s'", pkg.c_str());
            }
        }
        else if (line.rfind("game_remove=", 0) == 0) {
            std::string pkg = line.substr(12);
            if (aatpe::gamelist::removeManualGame(pkg)) {
                res.success = true;
                LOG_INFO("game_remove: %s (manual override list updated)", pkg.c_str());
            } else {
                res.success = false;
                res.message = "invalid package name";
                LOG_WARN("game_remove rejected: '%s'", pkg.c_str());
            }
        }
        else if (line == "stop") {
            res.success = true;
            g_shouldExit.store(1, std::memory_order_relaxed);
        }
        else if (line == "restart") {
        res.success = true;
            loadConfig();
            aatpe_profile::profileReapplyWinner();
        }
        else if (line.rfind("model_reset=", 0) == 0) {
        res.success = true;
            std::lock_guard<std::mutex> mlLock(g_mlMutex);
            g_mlModel.reset();
            unlink(mlModelPath());
            unlink((std::string(mlModelPath()) + ".bak").c_str());
            LOG_INFO("ML model reset via control command");
        }
        else if (line.rfind("model_save=", 0) == 0) {
        res.success = true;
            const char* mp = mlModelPath();
            bool saved = false;
            int totalSamples = 0;
            {
                std::lock_guard<std::mutex> mlLock(g_mlMutex);
                saved = g_mlModel.save(mp);
                totalSamples = g_mlModel.getTotalSamples();
            }
            if (saved) {
                g_mlSaveSuccess++;
                backupModelFile(mp);
                struct stat st;
                long long fsize = -1;
                if (stat(mp, &st) == 0) fsize = (long long)st.st_size;
                LOG_INFO("ML model saved via control: path=%s bytes=%lld samples=%d",
                         mp, fsize, totalSamples);
                updateMlSaveStateFiles(true, mp);
            } else {
                g_mlSaveFail++;
                LOG_WARN("ML model save via control FAILED: path=%s errno=%d (%s)",
                         mp, errno, aatpeErrStr(errno));
                updateMlSaveStateFiles(false, mp);
            }
        }
        else if (line.rfind("shadow_dump=", 0) == 0) {
            res.success = true;
            std::string diagDir = std::string(STATE_DIR) + "/diagnostic";
            ::mkdir(diagDir.c_str(), 0700);
            std::string outPath = diagDir + "/shadow_samples.jsonl";
            std::vector<aatpe_ml::LearningModel::ShadowSampleView> samples;
            int drained = 0;
            {
                std::lock_guard<std::mutex> mlLock(g_mlMutex);
                samples = g_mlModel.drainShadowSamples(512, true);
                drained = (int)samples.size();
            }
            std::string jsonl;
            jsonl.reserve(samples.size() * 256);
            char line[1024];
            for (const auto& s : samples) {
                snprintf(line, sizeof(line),
                    "{\"pool\":%d,\"outcome\":%.6g,"
                    "\"f\":[%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g]}\n",
                    s.pool, s.outcome,
                    s.features.avg_run_us, s.features.nr_switches,
                    s.features.cpu_usage, s.features.priority,
                    s.features.nice, s.features.is_game,
                    s.features.thermal, s.features.capacity,
                    s.features.current_cpu_match, s.features.migration_count,
                    s.features.cross_run_cap, s.features.cross_run_usage,
                    s.features.cross_cap_thermal, s.features.cross_run_thermal);
                jsonl += line;
            }
            if (writeFileAtomic(outPath.c_str(), jsonl)) {
                LOG_INFO("shadow_dump: drained %d sample(s) -> %s", drained, outPath.c_str());
            } else {
                res.success = false;
                LOG_WARN("shadow_dump: atomic write FAILED: %s errno=%d (%s)",
                         outPath.c_str(), errno, aatpeErrStr(errno));
            }
        }
        else if (line.rfind("set_ml_hybrid=", 0) == 0) {
        res.success = true;
            const char* hybridVal = line.c_str() + 14;
            if (!isAllDigits(hybridVal)) {
                int curHybrid;
                { std::lock_guard<std::mutex> cl(g_cfgMutex); curHybrid = g_cfg.mlHybridEnabled ? 1 : 0; }
                res.success = false;
                LOG_WARN("Invalid set_ml_hybrid value: '%s' (must be 0 or 1), keeping current: %d",
                         hybridVal, curHybrid);
            } else {
                int val = 0;
                if (!parseNonNegChecked(hybridVal, val) || (val != 0 && val != 1)) {
                    int curHybrid;
                    { std::lock_guard<std::mutex> cl(g_cfgMutex); curHybrid = g_cfg.mlHybridEnabled ? 1 : 0; }
                    res.success = false;
                    LOG_WARN("Invalid set_ml_hybrid value: '%s' (must be 0 or 1), keeping current: %d",
                             hybridVal, curHybrid);
                } else {
                bool newState = (val != 0);
                bool stateChanged;
                {
                    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
                    stateChanged = (newState != g_cfg.mlHybridEnabled);
                    g_cfg.mlHybridEnabled = newState;
                    std::lock_guard<std::mutex> hlock(g_hybridMutex);
                    aatpe_ml_hybrid::instance().setEnabled(newState);
                    if (stateChanged) {
                        const char* mp = mlModelPath();
                        std::string hybridPath = std::string(mp) + ".hybrid";
                        aatpe_ml_hybrid::instance().save(hybridPath.c_str());
                    }
                }
                writeFile((std::string(STATE_DIR) + "/ml_hybrid_enabled").c_str(), newState ? "1" : "0");
                persistConfigKeyValue("ML_HYBRID_ENABLED", newState ? "1" : "0");
                LOG_INFO("[ML-HYBRID] dynamic toggle: %s (was %s)",
                         newState ? "ENABLED" : "DISABLED",
                         newState ? "enabled" : "disabled");
                }
            }
        }
        else if (line.rfind("set_ml_learning=", 0) == 0) {
        res.success = true;
            const char* mlVal = line.c_str() + 16;
            if (!isAllDigits(mlVal)) {
                int curMl;
                { std::lock_guard<std::mutex> cl(g_cfgMutex); curMl = g_cfg.mlLearningEnabled ? 1 : 0; }
                res.success = false;
                LOG_WARN("Invalid set_ml_learning value: '%s' (must be 0 or 1), keeping current: %d",
                         mlVal, curMl);
            } else {
                int val = 0;
                if (!parseNonNegChecked(mlVal, val) || (val != 0 && val != 1)) {
                    int curMl;
                    { std::lock_guard<std::mutex> cl(g_cfgMutex); curMl = g_cfg.mlLearningEnabled ? 1 : 0; }
                    res.success = false;
                    LOG_WARN("Invalid set_ml_learning value: '%s' (must be 0 or 1), keeping current: %d",
                             mlVal, curMl);
                } else {
                bool newState = (val != 0);
                bool stateChanged;
                {
                    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
                    stateChanged = (newState != g_cfg.mlLearningEnabled);
                    g_cfg.mlLearningEnabled = newState;
                    std::lock_guard<std::mutex> mlLock(g_mlMutex);
                    g_mlModel.setEnabled(newState);
                }
                writeFile((std::string(STATE_DIR) + "/ml_learning_enabled").c_str(),
                          newState ? "1" : "0");
                persistConfigKeyValue("ML_LEARNING_ENABLED", newState ? "1" : "0");
                if (stateChanged) {
                    LOG_INFO("[ML-LEARNING] dynamic toggle: %s (was %s)",
                             newState ? "ENABLED" : "DISABLED",
                             newState ? "disabled" : "enabled");
                }
                }
            }
        }
        else if (line.rfind("battery_aging=", 0) == 0) {
        res.success = true;
            const char* agingVal = line.c_str() + 14;
            if (!isAllDigits(agingVal)) {
                res.success = false;
                LOG_WARN("Invalid battery_aging value: '%s' (must be 0 or 1)", agingVal);
            } else {
                int val = 0;
                if (!parseNonNegChecked(agingVal, val) || (val != 0 && val != 1)) {
                    res.success = false;
                    LOG_WARN("Invalid battery_aging value: '%s' (must be 0 or 1)", agingVal);
                } else {
                {
                    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
                    g_cfg.batteryAgingProtection = (val != 0);
                }
                writeFile((std::string(STATE_DIR) + "/battery_aging_enabled").c_str(),
                          (val != 0) ? "1" : "0");
                persistConfigKeyValue("BATTERY_AGING_PROTECTION", (val != 0) ? "1" : "0");
                LOG_INFO("[BATTERY-AGING] dynamic toggle: %s",
                         (val != 0) ? "ENABLED" : "DISABLED");
                }
            }
        }
        else if (line.rfind("set_ml_cold_start=", 0) == 0) {
        res.success = true;
            const char* csVal = line.c_str() + 18;
            if (!isAllDigits(csVal)) {
                res.success = false;
                LOG_WARN("Invalid set_ml_cold_start value: '%s' (must be 0 or 1)", csVal);
            } else {
                int val = 0;
                if (!parseNonNegChecked(csVal, val) || (val != 0 && val != 1)) {
                    res.success = false;
                    LOG_WARN("Invalid set_ml_cold_start value: '%s' (must be 0 or 1)", csVal);
                } else {
                bool newState = (val != 0);
                bool stateChanged;
                {
                    std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
                    stateChanged = (newState != g_cfg.mlColdStartProtection);
                    if (stateChanged) g_cfg.mlColdStartProtection = newState;
                }
                if (stateChanged) {
                    LOG_INFO("[ML-COLD-START] dynamic toggle: %s", newState ? "ENABLED" : "DISABLED");
                }
                writeFile((std::string(STATE_DIR) + "/ml_cold_start_enabled").c_str(), newState ? "1" : "0");
                persistConfigKeyValue("ML_COLD_START_PROTECTION", newState ? "1" : "0");
                }
            }
        }
        else if (line.rfind("explain_tid=", 0) == 0) {
        res.success = true;
            const char* tidVal = line.c_str() + 12;
            int tid = 0;
            if (!isAllDigits(tidVal)) {
                res.success = false;
                LOG_WARN("Invalid explain_tid value: '%s' (must be non-negative integer), setting to 0 (disabled)", tidVal);
                tid = 0;
            } else if (!parseNonNegChecked(tidVal, tid)) {
                res.success = false;
                LOG_WARN("Invalid explain_tid value: '%s' (overflow, must be non-negative integer)", tidVal);
            }
            if (res.success) {
                g_explainTid.store(tid);
                LOG_INFO("WebUI explain tid set to %d", tid);
            }
        }
        else if (line == "clear_log") {
        res.success = true;
            logClear();
        }
        else if (line == "reload_config") {
        res.success = true;
            std::string before = aatpe_profile::effectiveProfile();
            loadConfig();
            aatpe_profile::profileReapplyWinner();
            LOG_INFO("Control command: reload_config — config reloaded, GLOBAL re-voted, winner=%s",
                     aatpe_profile::effectiveProfile().c_str());
        }
        else if (line.rfind("set_decision_interval=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 22;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_decision_interval: '%s' (must be integer 10-5000)", val);
            } else {
                int ms = 0;
                if (!parseNonNegChecked(val, ms)) {
                    res.success = false;
                    LOG_WARN("Invalid set_decision_interval: '%s' (overflow, must be integer 10-5000)", val);
                } else if (ms >= 10 && ms <= 5000) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.decisionInterval = ms; }
                    writeFile((std::string(STATE_DIR) + "/decision_interval").c_str(), ms);
                    persistConfigKeyValue("DECISION_INTERVAL", std::to_string(ms).c_str());
                    LOG_INFO("[CFG] decision_interval=%d ms (persisted to aatpe.conf)", ms);
                } else {
                    res.success = false;
                    LOG_WARN("set_decision_interval=%d out of range (10-5000)", ms);
                }
            }
        }
        else if (line.rfind("set_confidence_threshold=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 25;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_confidence_threshold: '%s' (must be integer 0-100)", val);
            } else {
                int t = 0;
                if (!parseNonNegChecked(val, t)) {
                    res.success = false;
                    LOG_WARN("Invalid set_confidence_threshold: '%s' (overflow, must be integer 0-100)", val);
                } else if (t >= 0 && t <= 100) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.confidenceThreshold = t; }
                    writeFile((std::string(STATE_DIR) + "/confidence_threshold").c_str(), t);
                    persistConfigKeyValue("CONFIDENCE_THRESHOLD", std::to_string(t).c_str());
                    LOG_INFO("[CFG] confidence_threshold=%d (persisted to aatpe.conf)", t);
                } else {
                    res.success = false;
                    LOG_WARN("set_confidence_threshold=%d out of range (0-100)", t);
                }
            }
        }
        else if (line.rfind("set_migration_cost_threshold=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 29;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_migration_cost_threshold: '%s' (must be integer 0-10000)", val);
            } else {
                int t = 0;
                if (!parseNonNegChecked(val, t)) {
                    res.success = false;
                    LOG_WARN("Invalid set_migration_cost_threshold: '%s' (overflow, must be integer 0-10000)", val);
                } else if (t >= 0 && t <= 10000) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.migrationCostThreshold = t; }
                    writeFile((std::string(STATE_DIR) + "/migration_cost_threshold").c_str(), t);
                    persistConfigKeyValue("MIGRATION_COST_THRESHOLD", std::to_string(t).c_str());
                    LOG_INFO("[CFG] migration_cost_threshold=%d (persisted to aatpe.conf)", t);
                } else {
                    res.success = false;
                    LOG_WARN("set_migration_cost_threshold=%d out of range (0-10000)", t);
                }
            }
        }
        else if (line.rfind("set_max_migrations=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 19;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_max_migrations: '%s' (must be integer 1-1000)", val);
            } else {
                int n = 0;
                if (!parseNonNegChecked(val, n)) {
                    res.success = false;
                    LOG_WARN("Invalid set_max_migrations: '%s' (overflow, must be integer 1-1000)", val);
                } else if (n >= 1 && n <= 1000) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.maxMigrationsPerSec = n; }
                    writeFile((std::string(STATE_DIR) + "/max_migrations_per_sec").c_str(), n);
                    persistConfigKeyValue("MAX_MIGRATIONS_PER_SEC", std::to_string(n).c_str());
                    LOG_INFO("[CFG] max_migrations_per_sec=%d (persisted to aatpe.conf)", n);
                } else {
                    res.success = false;
                    LOG_WARN("set_max_migrations=%d out of range (1-1000)", n);
                }
            }
        }
        else if (line.rfind("set_thermal_enabled=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 20;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_thermal_enabled: '%s' (must be 0 or 1)", val);
            } else if (strcmp(val, "0") != 0 && strcmp(val, "1") != 0) {
                res.success = false;
                LOG_WARN("Invalid set_thermal_enabled: '%s' (only 0=off, 1=on accepted)", val);
            } else {
                bool en = (val[0] == '1');
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalEnabled = en; }
                    writeFile((std::string(STATE_DIR) + "/thermal_enabled").c_str(), en ? "1" : "0");
                    persistConfigKeyValue("THERMAL_ENABLED", en ? "1" : "0");
                    LOG_INFO("[CFG] thermal_enabled=%d (persisted to aatpe.conf)", en ? 1 : 0);
            }
        }
        else if (line.rfind("set_c4_freezer=", 0) == 0) {
            res.success = true;
            const char* val = line.c_str() + 15;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_c4_freezer: '%s' (must be 0 or 1)", val);
            } else if (strcmp(val, "0") != 0 && strcmp(val, "1") != 0) {
                res.success = false;
                LOG_WARN("Invalid set_c4_freezer: '%s' (only 0=off, 1=on accepted)", val);
            } else {
                bool en = (val[0] == '1');
                { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.c4FreezerEnabled = en; }
                aatpe::c4freezer::setC4FreezerEnabled(en);
                writeFile((std::string(STATE_DIR) + "/c4_freezer_enabled").c_str(), en ? "1" : "0");
                persistConfigKeyValue("C4_FREEZER_ENABLED", en ? "1" : "0");
                LOG_INFO("[CFG] c4_freezer_enabled=%d", en ? 1 : 0);
            }
        }
        else if (line.rfind("set_ebpf=", 0) == 0) {
            res.success = true;
            const char* val = line.c_str() + 9;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_ebpf: '%s' (must be 0 or 1)", val);
            } else if (strcmp(val, "0") != 0 && strcmp(val, "1") != 0) {
                res.success = false;
                LOG_WARN("Invalid set_ebpf: '%s' (only 0=off, 1=on accepted)", val);
            } else {
                bool en = (val[0] == '1');
                { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.ebpfEnabled = en; }
                writeFile((std::string(STATE_DIR) + "/ebpf_event_driven").c_str(), en ? "1" : "0");
                const char* vstr = en ? "1" : "0";
                persistConfigKeyValue("EBPF_EVENT_DRIVEN", vstr);
                persistConfigKeyValue("EBPF_ENABLED", vstr);
                LOG_INFO("[CFG] ebpf_event_driven=%d (auto-detect/auto-downgrade still enforced; both EBPF_* keys synced)", en ? 1 : 0);
            }
        }
        else if (line.rfind("set_federated=", 0) == 0) {
            res.success = true;
            const char* val = line.c_str() + 14;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_federated: '%s' (must be 0 or 1)", val);
            } else if (strcmp(val, "0") != 0 && strcmp(val, "1") != 0) {
                res.success = false;
                LOG_WARN("Invalid set_federated: '%s' (only 0=off, 1=on accepted)", val);
            } else {
                bool en = (val[0] == '1');
                { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.federatedEnabled = en; }
                writeFile((std::string(STATE_DIR) + "/federated_enabled").c_str(), en ? "1" : "0");
                persistConfigKeyValue("FEDERATED_ENABLED", en ? "1" : "0");
                LOG_INFO("[CFG] federated_enabled=%d (local export/import gated by this flag)", en ? 1 : 0);
            }
        }
        else if (line.rfind("set_thermal_limit_cpu=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 22;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_thermal_limit_cpu: '%s' (must be integer 0-200)", val);
            } else {
                int t = 0;
                if (!parseNonNegChecked(val, t)) {
                    res.success = false;
                    LOG_WARN("Invalid set_thermal_limit_cpu: '%s' (overflow, must be integer 0-200)", val);
                } else if (t >= 0 && t <= 200) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalLimitCpu = t; }
                    writeFile((std::string(STATE_DIR) + "/thermal_limit_cpu").c_str(), t);
                    persistConfigKeyValue("THERMAL_LIMIT_CPU", std::to_string(t).c_str());
                    LOG_INFO("[CFG] thermal_limit_cpu=%d (persisted to aatpe.conf)", t);
                } else {
                    res.success = false;
                    LOG_WARN("set_thermal_limit_cpu=%d out of range (0-200)", t);
                }
            }
        }
        else if (line.rfind("set_thermal_limit_skin=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 23;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_thermal_limit_skin: '%s' (must be integer 0-200)", val);
            } else {
                int t = 0;
                if (!parseNonNegChecked(val, t)) {
                    res.success = false;
                    LOG_WARN("Invalid set_thermal_limit_skin: '%s' (overflow, must be integer 0-200)", val);
                } else if (t >= 0 && t <= 200) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalLimitSkin = t; }
                    writeFile((std::string(STATE_DIR) + "/thermal_limit_skin").c_str(), t);
                    persistConfigKeyValue("THERMAL_LIMIT_SKIN", std::to_string(t).c_str());
                    LOG_INFO("[CFG] thermal_limit_skin=%d (persisted to aatpe.conf)", t);
                } else {
                    res.success = false;
                    LOG_WARN("set_thermal_limit_skin=%d out of range (0-200)", t);
                }
            }
        }
        else if (line.rfind("set_thermal_limit_battery=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 26;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_thermal_limit_battery: '%s' (must be integer 0-200)", val);
            } else {
                int t = 0;
                if (!parseNonNegChecked(val, t)) {
                    res.success = false;
                    LOG_WARN("Invalid set_thermal_limit_battery: '%s' (overflow, must be integer 0-200)", val);
                } else if (t >= 0 && t <= 200) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalLimitBattery = t; }
                    writeFile((std::string(STATE_DIR) + "/thermal_limit_battery").c_str(), t);
                    persistConfigKeyValue("THERMAL_LIMIT_BATTERY", std::to_string(t).c_str());
                    LOG_INFO("[CFG] thermal_limit_battery=%d (persisted to aatpe.conf)", t);
                } else {
                    res.success = false;
                    LOG_WARN("set_thermal_limit_battery=%d out of range (0-200)", t);
                }
            }
        }
        else if (line.rfind("set_thermal_emergency=", 0) == 0) {
        res.success = true;
            const char* val = line.c_str() + 22;
            if (!isAllDigits(val)) {
                res.success = false;
                LOG_WARN("Invalid set_thermal_emergency: '%s' (must be integer 0-200)", val);
            } else {
                int t = 0;
                if (!parseNonNegChecked(val, t)) {
                    res.success = false;
                    LOG_WARN("Invalid set_thermal_emergency: '%s' (overflow, must be integer 0-200)", val);
                } else if (t >= 0 && t <= 200) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_cfg.thermalEmergency = t; }
                    persistConfigKeyValue("THERMAL_EMERGENCY", std::to_string(t).c_str());
                    LOG_INFO("[CFG] thermal_emergency=%d (persisted to aatpe.conf)", t);
                } else {
                    res.success = false;
                    LOG_WARN("set_thermal_emergency=%d out of range (0-200)", t);
                }
            }
        }
        else {
            res.success = false;
            res.error_code = aatpe::ERR_UNKNOWN_COMMAND;
            res.message = "no matching command";
            LOG_WARN("Unknown control command: '%s' (ignored)", line.c_str());
        }
        return res;
}

static bool isHelperApkInstalled() {
    static constexpr long long kProbeIntervalMs = 60LL * 1000LL;
    static std::mutex s_cacheMutex;
    static long long s_lastProbeMs = 0;
    static bool s_cachedInstalled = false;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        if (nowMs - s_lastProbeMs < kProbeIntervalMs) return s_cachedInstalled;
        s_lastProbeMs = nowMs;
    }
    std::string out = aatpeExec("pm list packages com.aatpe.helper 2>/dev/null", "HELPER_APK");
    bool installed = false;
    if (!out.empty()) {
        std::string lower = out;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        installed = lower.find("package:com.aatpe.helper") != std::string::npos;
    }
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        s_cachedInstalled = installed;
    }
    return installed;
}

void applyHelperApkDetection() {
    int cfgAutoProfile;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        cfgAutoProfile = g_cfg.autoProfile;
    }
    g_helperApkInstalled.store(isHelperApkInstalled());
    {
        std::lock_guard<std::mutex> apmLock(g_autoProfileMutex);
        if (g_helperApkInstalled) {
            g_helperAutoSuppressed.store(true);
            g_autoProfileMode = "apk_controlled";
        } else {
            g_helperAutoSuppressed.store(false);
            g_autoProfileMode = "module_controlled";
        }
    }
    if (g_helperApkInstalled) {
        LOG_INFO("Helper APK detected: auto_profile runtime-suppressed (APK controls switching; "
                 "user config AUTO_PROFILE=%d preserved)", cfgAutoProfile);
    } else {
        LOG_INFO("Helper APK not found: auto_profile %s (module controls switching)",
                 cfgAutoProfile ? "enabled" : "disabled");
    }
    writeFile((std::string(STATE_DIR) + "/helper_apk_installed").c_str(), g_helperApkInstalled.load() ? 1 : 0);
    std::string apmSnapshot;
    {
        std::lock_guard<std::mutex> apmLock(g_autoProfileMutex);
        apmSnapshot = g_autoProfileMode;
    }
    writeFile((std::string(STATE_DIR) + "/auto_profile_mode").c_str(), apmSnapshot);
    int effectiveAuto = cfgAutoProfile && !g_helperAutoSuppressed.load();
    atomicWriteFile((std::string(STATE_DIR) + "/auto_profile").c_str(), effectiveAuto);
}


#include "aatped.h"
#include "aatped_utils.h"

using namespace aatpe;

std::atomic<bool> g_audioModeActive{false};
std::atomic<int> g_audioIsolatedCpu{4};
static std::atomic<int> g_cachedAudioServerPid{-1};
static std::atomic<long long> g_lastAudioScanMs{0};
static std::atomic<bool> g_audioTracksActive{false};
static constexpr long long kProcScanNegCacheMs = 30000;
static constexpr long long kActiveTrackRescanMs = 5000;

static int parseAudioPidFromDumpsys(const std::string& out) {
    if (out.empty()) return -1;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        auto pos = line.find("AudioFlinger(");
        if (pos != std::string::npos) {
            int pid = atoi(line.c_str() + pos + 13);
            if (pid > 0) return pid;
        }
        auto colon = line.find("pid:");
        if (colon != std::string::npos) {
            int pid = atoi(line.c_str() + colon + 4);
            if (pid > 0) return pid;
        }
        auto colon2 = line.find("PID:");
        if (colon2 != std::string::npos) {
            int pid = atoi(line.c_str() + colon2 + 4);
            if (pid > 0) return pid;
        }
    }
    return -1;
}

static bool detectAudioPlaybackImpl(void);

bool aatpeAudioDumpShowsActiveTrack(const std::string& dump) {
    if (dump.empty()) return false;
    std::istringstream iss(dump);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("state: started") != std::string::npos ||
            line.find("state: Started") != std::string::npos ||
            line.find("State: started") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool detectAudioPlayback(void) {
    try {
        return detectAudioPlaybackImpl();
    } catch (...) {
        LOG_WARN("audio: unexpected exception in detectAudioPlayback, treating as inactive");
        g_audioModeActive = false;
        return false;
    }
}

static bool detectAudioPlaybackImpl(void) {
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    bool serverAlive = false;
    int cachedPid = g_cachedAudioServerPid.load(std::memory_order_relaxed);
    if (cachedPid > 0) {
        if (kill(cachedPid, 0) == 0) {
            char cmdPath[64];
            snprintf(cmdPath, sizeof(cmdPath), "/proc/%d/cmdline", cachedPid);
            std::string cmd = readFile(cmdPath);
            if (cmd.find("audioserver") != std::string::npos ||
                cmd.find("audioflinger") != std::string::npos) {
                serverAlive = true;
            }
        }
        if (!serverAlive) {
            g_cachedAudioServerPid.store(-1, std::memory_order_relaxed);
            g_audioTracksActive.store(false, std::memory_order_relaxed);
        }
    }

    {
        long long rescanEvery = g_audioTracksActive.load(std::memory_order_relaxed)
                                    ? kActiveTrackRescanMs
                                    : kProcScanNegCacheMs;
        long long lastScan = g_lastAudioScanMs.load(std::memory_order_relaxed);
        if (cachedPid <= 0 || nowMs - lastScan >= rescanEvery) {
        g_lastAudioScanMs.store(nowMs, std::memory_order_relaxed);

        std::string dumpsysOut = aatpeExecArgv(
            {"dumpsys", "media.audio_flinger"}, "audio-pid");
        int pidFromDumpsys = parseAudioPidFromDumpsys(dumpsysOut);
        if (pidFromDumpsys > 0) {
            g_cachedAudioServerPid.store(pidFromDumpsys, std::memory_order_relaxed);
            g_audioTracksActive.store(aatpeAudioDumpShowsActiveTrack(dumpsysOut),
                                      std::memory_order_relaxed);
        } else {
            DIR* proc = opendir("/proc");
            if (proc) {
                struct dirent* entry;
                while ((entry = readdir(proc)) != NULL) {
                    int pid = parseIntSafe(entry->d_name);
                    if (pid <= 0) continue;
                    char path[256];
                    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
                    FILE* fp = fopen(path, "r");
                    if (fp) {
                        char cmd[256] = {0};
                        fread(cmd, 1, sizeof(cmd)-1, fp);
                        fclose(fp);
                        if (strstr(cmd, "audioserver") || strstr(cmd, "audioflinger")) {
                            g_cachedAudioServerPid.store(pid, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
                closedir(proc);
            }
        }
        }
    }

    bool wantActive = serverAlive && g_audioTracksActive.load(std::memory_order_relaxed);

    if (wantActive) {
        std::vector<int> candidateCpus;
        {
            std::lock_guard<std::mutex> topoLk(g_topologyRwMutex);
            candidateCpus.insert(candidateCpus.end(), g_midPool.begin(), g_midPool.end());
            candidateCpus.insert(candidateCpus.end(), g_bigPool.begin(), g_bigPool.end());
        }
        int bestCpu = candidateCpus.empty() ? 4 : candidateCpus[0];
        float bestLoad = 100.0f;
        for (int c : candidateCpus) {
            float load = readCpuUtilizationSchedstat(c);
            if (load >= 0 && load < bestLoad) { bestLoad = load; bestCpu = c; }
        }
        g_audioIsolatedCpu.store(bestCpu, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_audioState.modeActive = true;
            g_audioState.isolatedCpu = bestCpu;
            g_audioState.serverPid = g_cachedAudioServerPid.load(std::memory_order_relaxed);
        }
        if (!writeFile((std::string(STATE_DIR) + "/audio_mode_active").c_str(), "1")) {
            LOG_WARN("audio: failed to write audio_mode_active state");
        }
        if (!writeFile((std::string(STATE_DIR) + "/audio_isolated_cpu").c_str(), bestCpu)) {
            LOG_WARN("audio: failed to write audio_isolated_cpu state");
        }
        g_audioModeActive = true;
        return true;
    }

    g_audioModeActive = false;
    {
        std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
        g_audioState.modeActive = false;
        g_audioState.isolatedCpu = -1;
        g_audioState.serverPid = -1;
    }
    return false;
}

bool isAudioThread(const char* threadName) {
    if (!threadName) return false;
    return (strstr(threadName, "AudioFlinger") || strstr(threadName, "AudioTrack") ||
            strstr(threadName, "audioserver") || strstr(threadName, "FastMixer") ||
            strstr(threadName, "AudioOut"));
}

int getAudioThreadMigrationPenalty(const char* threadName) {
    if (isAudioThread(threadName) && g_audioModeActive) return 200;
    return 0;
}

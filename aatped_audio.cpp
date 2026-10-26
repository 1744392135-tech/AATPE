// AATPE - Audio low-latency (extracted from aatped_main.cpp)
#include "aatped.h"
#include "aatped_utils.h"   // aatpeExecArgv (H11: hardened exec)

using namespace aatpe;

// Audio low-latency implementation
// P2-7: g_audioModeActive is read by getAudioThreadMigrationPenalty() from the
// placement thread and written by detectAudioPlayback() from the main loop.
// Plain bool is a data race; use atomic<bool>.
std::atomic<bool> g_audioModeActive{false};
// A2 (RCV): written by detectAudioPlayback() on the main loop, read by
// placement.cpp from the placement thread. Plain int was a data race; make it atomic.
std::atomic<int> g_audioIsolatedCpu{4}; // Isolate audio threads to CPU4 (mid core)
// MON-12: these were plain statics. detectAudioPlaybackImpl is documented as
// main-loop-only, but making them atomic removes the footgun if a future caller
// from another thread ever invokes detection.
static std::atomic<int> g_cachedAudioServerPid{-1};
static std::atomic<long long> g_lastAudioScanMs{0};
// C-006: positive-evidence active-track state. audioserver/audioflinger is a
// long-running system server; its mere existence does NOT mean audio is playing.
// The low-latency audio mode (isolation + 200x migration penalty) is entered ONLY
// when the server is alive AND we have recent positive evidence of an ACTIVE track.
// Refreshed by the throttled dumpsys scan on the main loop; read here.
static std::atomic<bool> g_audioTracksActive{false};
// H11: negative cache for /proc fallback scan. When dumpsys fast path fails and
// /proc scan finds nothing, wait 30s before retrying (was 5s). This avoids
// repeatedly scanning 300-500 processes when audio is genuinely off.
static constexpr long long kProcScanNegCacheMs = 30000;
// A2 (cluster2 #002): while we BELIEVE tracks are active, rescan on a much tighter
// cadence so that when the user stops playback we drop the isolation/200x penalty
// within ~5s instead of the full 30s window. When idle we keep the slow 30s scan
// (no extra dumpsys forks when audio is off). 5s chosen: a dumpsys media.audio_flinger
// is one hardened fork with a 3s deadline, once per 5s during active playback is cheap,
// and 5s bounds the over-isolation lag (was 30s, 6x reduction).
static constexpr long long kActiveTrackRescanMs = 5000;

// H11: parse PID from `dumpsys media.audio_flinger` output.
// Look for lines like "pid: 1234" or "PID 1234" or "Tid 1234".
// Returns -1 if not found.
static int parseAudioPidFromDumpsys(const std::string& out) {
    if (out.empty()) return -1;
    // Look for "pid=" or "PID " followed by digits.
    // The AudioFlinger dumps typically has lines like:
    //   "pid: 1234"  or  "PID 1234"  or  "AudioFlinger(1234)"
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        // Try "AudioFlinger(NNNN)" pattern.
        auto pos = line.find("AudioFlinger(");
        if (pos != std::string::npos) {
            int pid = atoi(line.c_str() + pos + 13);
            if (pid > 0) return pid;
        }
        // Try "pid: NNNN" or "PID: NNNN".
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

// P0 fail-soft: forward-declare impl so detectAudioPlayback can wrap it.
static bool detectAudioPlaybackImpl(void);

// C-006: parse `dumpsys media.audio_flinger` output for evidence of an ACTIVE
// playback track. Conservative + restorable: returns true ONLY on an explicit
// "track started" marker; an empty/unrecognized dump means "no active track", so
// we do NOT enter the audio mode (no isolation / 200x migration penalty without
// positive evidence). If a device's dump format is unrecognized, we err on the
// side of NOT penalizing rather than isolating idle FastMixer threads. Non-static
// for the host hermetic test.
bool aatpeAudioDumpShowsActiveTrack(const std::string& dump) {
    if (dump.empty()) return false;
    std::istringstream iss(dump);
    std::string line;
    while (std::getline(iss, line)) {
        // AudioTrack in the started/running state is concrete positive evidence of
        // active playback (vs. an idle mixer thread that merely exists).
        if (line.find("state: started") != std::string::npos ||
            line.find("state: Started") != std::string::npos ||
            line.find("State: started") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool detectAudioPlayback(void) {
    // P0 fail-soft: audio detection must never crash the main loop.
    try {
        return detectAudioPlaybackImpl();
    } catch (...) {
        LOG_WARN("audio: unexpected exception in detectAudioPlayback, treating as inactive");
        g_audioModeActive = false;
        return false;
    }
}

static bool detectAudioPlaybackImpl(void) {
    // P8: Cache audioserver PID, use kill(pid,0) for fast check
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // ---- Step 1: audioserver liveness (fast, every cycle) ----
    bool serverAlive = false;
    int cachedPid = g_cachedAudioServerPid.load(std::memory_order_relaxed);
    if (cachedPid > 0) {
        // Verify PID is still audioserver: kill(pid,0) only checks process
        // existence, not identity. If audioserver died and its PID was reused
        // by an unrelated process, kill() would falsely succeed.
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
            // C-007: audioserver died. Immediately drop the active-track belief AND
            // (in Step 3) g_audioModeActive, instead of lingering up to the 30s
            // negative-cache window. Eliminates the ~30s residual isolation/penalty.
            g_cachedAudioServerPid.store(-1, std::memory_order_relaxed);
            g_audioTracksActive.store(false, std::memory_order_relaxed);
        }
    }

    // ---- Step 2: throttled rescan: (re)discover pid + refresh active-track evidence
    // H11: rescan when we have no cached pid, OR the window elapsed. While tracks are
    // believed active we use the tighter kActiveTrackRescanMs (5s) so stop-playback
    // drops isolation quickly; when idle we keep the 30s negative cache.
    {
        long long rescanEvery = g_audioTracksActive.load(std::memory_order_relaxed)
                                    ? kActiveTrackRescanMs
                                    : kProcScanNegCacheMs;
        long long lastScan = g_lastAudioScanMs.load(std::memory_order_relaxed);
        if (cachedPid <= 0 || nowMs - lastScan >= rescanEvery) {
        g_lastAudioScanMs.store(nowMs, std::memory_order_relaxed);

        // Fast path: dumpsys media.audio_flinger (hardened, 3s deadline, silent fail).
        std::string dumpsysOut = aatpeExecArgv(
            {"dumpsys", "media.audio_flinger"}, "audio-pid");
        int pidFromDumpsys = parseAudioPidFromDumpsys(dumpsysOut);
        if (pidFromDumpsys > 0) {
            g_cachedAudioServerPid.store(pidFromDumpsys, std::memory_order_relaxed);
            // C-006: derive active-track evidence from the SAME dump (no extra fork).
            g_audioTracksActive.store(aatpeAudioDumpShowsActiveTrack(dumpsysOut),
                                      std::memory_order_relaxed);
        } else {
            // Fallback: /proc full scan (slow, only when dumpsys unavailable).
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
                            // C-006: /proc proves liveness, NOT active playback. We have
                            // no flinger dump here, so conservatively leave tracksActive
                            // unchanged (do NOT enter audio mode on liveness alone).
                            break;
                        }
                    }
                }
                closedir(proc);
            }
        }
        }  // rescan if
    }  // active-track rescan cadence block

    // ---- Step 3: audio mode = server alive AND positive active-track evidence ----
    // C-006: audioserver being resident is NECESSARY but not SUFFICIENT. Liveness
    // alone must not pin the isolation + 200x migration penalty on idle mixer threads.
    bool wantActive = serverAlive && g_audioTracksActive.load(std::memory_order_relaxed);

    if (wantActive) {
        // LED-091 (R7-006): g_midPool/g_bigPool are mutated by
        // detectCpuTopology() under g_topologyRwMutex; snapshot the pools
        // under that lock instead of copying iterators unlocked (which could
        // be invalidated by a concurrent realloc -> SIGSEGV).
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
        // LED-089 (R7-004): g_audioState.* is read by writeStateJson() under
        // g_stateDiagMutex. Publish the struct fields under the SAME lock so
        // the read side never sees a torn/half-updated audio snapshot.
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_audioState.modeActive = true;
            g_audioState.isolatedCpu = bestCpu;
            g_audioState.serverPid = g_cachedAudioServerPid.load(std::memory_order_relaxed);
        }
        // P3-35: check writeFile return value.
        if (!writeFile((std::string(STATE_DIR) + "/audio_mode_active").c_str(), "1")) {
            LOG_WARN("audio: failed to write audio_mode_active state");
        }
        if (!writeFile((std::string(STATE_DIR) + "/audio_isolated_cpu").c_str(), bestCpu)) {
            LOG_WARN("audio: failed to write audio_isolated_cpu state");
        }
        g_audioModeActive = true;
        return true;
    }

    // Inactive: no live server OR no active track. g_audioModeActive drops here
    // immediately (C-007: no lingering residual after the server dies).
    g_audioModeActive = false;
    // LED-089: publish the inactive state under g_stateDiagMutex.
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
    // Audio threads: double migration penalty to prevent jitter
    if (isAudioThread(threadName) && g_audioModeActive) return 200;
    return 0;
}

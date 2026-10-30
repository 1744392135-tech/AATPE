#ifndef AATPED_H
#define AATPED_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdarg>
#include <climits>
#include <limits>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <deque>
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

#include "aatped_types.h"

#include "aatped_utils.h"
#include "aatped_logging.h"
#include "aatped_config.h"
#include "aatped_profile.h"
#include "aatped_topology.h"
#include "aatped_state.h"
#include "aatped_control.h"
#include "aatped_hmac.h"
#include "aatped_gamescene.h"
#include "aatped_battery.h"
#include "aatped_gpu.h"
#include "aatped_audio.h"
#include "aatped_applaunch.h"
#include "aatped_uevent.h"
#include "aatped_abtest.h"
#include "aatped_ebpf_agg.h"

namespace aatpe { class EventMonitor; }
namespace aatpe_ml { class LearningModel; }

extern const char* RUN_DIR;
extern const char* STATE_DIR;
extern const char* METRICS_DIR;
extern const char* DECISION_DIR;
extern const char* WEBUI_DIR;
extern const char* LOG_FILE;
extern const char* PID_FILE;
extern const char* LOCK_FILE;
extern const char* CONF_DIR;
extern const char* ML_MODEL_PATH;

extern std::atomic<sig_atomic_t> g_shouldExit;
extern std::atomic<sig_atomic_t> g_reloadProfile;
extern std::atomic<uint64_t> g_stateGeneration;
void signalHandler(int sig);
void processControlCommands();
void daemonInit();
void daemonLoop();

void cmdStatus();
void cmdThreads();
void cmdBenchmark();
void cmdExplain(int targetTid);

float hybridScore(int cpuId, const ThreadInfo& ti);

extern aatpe_ml::GfxInfoFrameTimeProvider g_gfxInfoProvider;
extern aatpe_ml::SurfaceFlingerLatencyProvider g_sfLatencyProvider;
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot();
void initFrameTimeOutcomeCallbacks();

namespace aatpe {
namespace placement {
    void releaseForegroundDownpins(const std::string& newFg);
    void releaseAllSpilloverLocked();
    void healForegroundCgroupMembership(const std::string& fgPkg);
    bool healShouldLiftThread(const std::string& threadPkg, const std::string& fgPkg,
                             int threadRole, const std::string& liveCgroup,
                             const std::string& controlMode, bool thermalEmergency,
                             int thermalHalLevel, bool cpusetTopAppCap, bool screenOn);
}
namespace frameaware {
    void updateRefreshRate();
    int getRefreshRate();
    int getFrameIntervalMs();
    long long getDecisionCount();
    long long getMigrationCount();
    bool shouldTriggerFrameAware(long long nowMs);
    bool jankBoostVoteActive();
    void executeFrameAwareDecision();
    void writeStats(const std::string& stateDir);
    void refreshRateEventStart();
    void refreshRateEventStop();

    struct SfFrameSample {
        bool available = false;
        bool frameUnavailable = false;
        int samples = 0;
        int miss = 0;
        float hz = 0.0f;
        float intervalMs = 0.0f;
        double renderFps = 0.0;
        std::string chosenLayer;
        std::string reason;
        std::string candidates;
    };
    std::vector<std::string> sfListLayers(const std::string& listOut);
    std::string extractBracketedPkg(const std::string& layer);
    bool layerReferencesPkg(const std::string& core, const std::string& fgPkg);
    bool isGameEngineClass(const std::string& core);
    std::string selectGameLayer(const std::vector<std::string>& layers,
                                const std::string& fgPkg,
                                bool forceGame = false,
                                std::string* outReason = nullptr,
                                std::string* outCandidates = nullptr);
    SfFrameSample sampleGameLayer(const std::string& listOut, const std::string& fgPkg,
                                  const std::function<std::string(const std::string&)>& fetchLatency,
                                  bool forceGame = false);
    std::set<std::string> parseGameOverridesContent(const std::string& content);
    std::vector<std::string> parsePmPackageList(const std::string& pmOutput);
    struct TimestatsResult {
        bool available = false;
        std::string layer;
        int totalFrames = 0;
        double avgFps = 0.0;
        double missRate = 0.0;
        double p90Fps = 0.0;
        double p99Fps = 0.0;
        double targetFps = 0.0;
        int activeFrames = 0;
        double onCadenceRatio = 0.0;
        std::vector<std::pair<double,long long>> hist;
        std::string reason;
    };
    TimestatsResult parseTimestatsBlock(const std::string& dump,
                                        const std::string& fgPkg = "");
    bool isLayerNameSafe(const std::string& s);

    bool isTransientFocus(const std::string& prevFg, const std::string& newFg);
    bool sfLatencySamplesValid(const std::vector<long long>& us);
    long long frameBinderBackoffMs(int retryCount);
    struct FgDebouncer {
        std::string committed;
        std::string pending;
        long long pendingSinceMs = 0;
        static constexpr long long kCommitDelayMs = 3000;
        std::string onRawFg(const std::string& raw, long long nowMs);
    };
    struct TimestatsSession {
        bool enabled = false;
        long long enabledAtMs = 0;
        long long unavailableUntilMs = 0;
        long long lastDumpAtMs = 0;
        int forkCalls = 0;
        std::string lastReason;
        std::string lastDumpContent;
        std::string snapLayer;
        long long snapTotalFrames = -1;
        long long snapDumpMs = 0;
        TimestatsResult tick(long long nowMs, const std::string& fgPkg,
                             const std::function<std::string(const std::string&)>& fetch);
        void stop(const std::function<std::string(const std::string&)>& fetch);
    };
    double chooseForegroundFps(double latencyRenderFps, double latencyHz,
                              bool latencyAvail, double tsAvgFps, int tsActiveFrames,
                              int& outSource, bool& outSuspect);
}
namespace gamelist {
    bool isManualGame(const std::string& pkg);
    std::string currentManualGameList();
    std::vector<std::string> manualGameListSnapshot();
    bool addManualGame(const std::string& pkg);
    bool removeManualGame(const std::string& pkg);
    std::string detectManualGameForeground();
}
namespace apps {
    std::vector<std::string> listInstalledPackages();
}
}

void aatpeSetFrameJank(const char* source, int totalSamples, double missRate,
                      double avgFps, double p90Fps, double p99Fps);

struct AatpePerAppFrameStat {
    std::string pkg;
    double fps = 0.0;
    double ft_avg_ms = 0.0;
    double ft_p50 = 0.0;
    double ft_p95 = 0.0;
    double ft_p99 = 0.0;
    long jank = 0;
    long dropped = 0;
    long samples = 0;
    long total_frames = 0;
    long active_ms = 0;
    long long last_update_ms = 0;
    double target_fps = 0.0;
    std::string reason;
    std::string frameSource;
    bool isCurrent = false;
    long long lastSeenMs = 0;
    long long lastSeenEpochMs = 0;
    long missed_vsyncs = 0;
};
int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>& out,
                               std::string& currentPkg);

void frameSetAuthSnapshot(const std::string& fg, const std::string& source, const std::string& reason);
void frameGetAuthSnapshot(std::string& fg, std::string& source, std::string& reason);

inline bool aatpeThermalEmergencyHardWall(int anyCpuTempC, int emergencyWallC) {
    if (emergencyWallC <= 0) emergencyWallC = 120;
    return anyCpuTempC >= emergencyWallC;
}

#endif

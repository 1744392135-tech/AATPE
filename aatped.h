// AATPE - Adaptive Android Task Placement Engine
// Umbrella header for split compilation units.
// Type definitions in aatped_types.h; module declarations in sub-headers.
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

// Core type definitions (enums, structs)
#include "aatped_types.h"

// Sub-module headers
#include "aatped_utils.h"
#include "aatped_logging.h"
#include "aatped_config.h"
// Unified profile source state machine: all profile changes must vote
// through aatpe_profile::profileRequest/profileRelease instead of calling
// loadProfile() directly.
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

// ============================================================================
// Directory paths
// ============================================================================
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

// ============================================================================
// Daemon lifecycle
// ============================================================================
extern std::atomic<sig_atomic_t> g_shouldExit;
extern std::atomic<sig_atomic_t> g_reloadProfile;
extern std::atomic<uint64_t> g_stateGeneration;
void signalHandler(int sig);
void processControlCommands();
void daemonInit();
void daemonLoop();

// CLI commands
void cmdStatus();
void cmdThreads();
void cmdBenchmark();
void cmdExplain(int targetTid);

// hybridScore() returns 0.0f (hybrid learning fine-tuning placeholder)
float hybridScore(int cpuId, const ThreadInfo& ti);

// Frame-time outcome providers (global instances)
extern aatpe_ml::GfxInfoFrameTimeProvider g_gfxInfoProvider;
extern aatpe_ml::SurfaceFlingerLatencyProvider g_sfLatencyProvider;
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot();
void initFrameTimeOutcomeCallbacks();

// Frame-Aware Scheduling.
// Note: this is NOT real VSYNC synchronization. The daemon estimates the frame
// period from the observed/advertised refresh rate and paces placement decisions
// to that period; there is no kernel VSYNC event subscription. The C++ namespace
// and log strings use "frame-aware" to reflect that. On-disk state filenames keep
// the legacy "vsync_*" names because they are the stable WebUI/CLI contract.
namespace aatpe {
namespace placement {
    void releaseForegroundDownpins(const std::string& newFg);   // ROUND-012
    void releaseAllSpilloverLocked();                            // ROUND-014 P3
    // v4.4.1 P0: periodic (non-edge) cgroup readback heal for the current fg pkg's
    // critical threads. Lifts them out of a background cpuset (0-3) back into
    // top-app after a PiP/freeform/switch-back window, so placement can free-place
    // them onto the big cluster. Only touches pname==fgPkg critical-role threads.
    void healForegroundCgroupMembership(const std::string& fgPkg);
    // v4.4.1 P0: PURE per-thread lift decision (host-testable). All side-effecting
    // inputs passed in; returns true iff this thread should be moved to top-app.
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
    // ROUND-007c: bounded sustained-jank boost vote (true while foreground janks).
    bool jankBoostVoteActive();
    void executeFrameAwareDecision();
    void writeStats(const std::string& stateDir);
    // #28: inotify-based display-mode watcher (event-driven refresh-rate refresh).
    void refreshRateEventStart();
    void refreshRateEventStop();

    // WS-C2: game SurfaceView frame sampling (R3). Pure, hermetically testable.
    // Result of sampling a game SurfaceFlinger layer.
    struct SfFrameSample {
        bool available = false;          // >=1 valid frame row parsed
        bool frameUnavailable = false;    // fail-closed: no game layer / all invalid
        int samples = 0;                  // valid frame rows
        int miss = 0;                     // deadline-miss count (C-A > period)
        float hz = 0.0f;                  // refresh rate from layer first line (LTPO)
        float intervalMs = 0.0f;          // period in ms
        double renderFps = 0.0;          // v6.0.0 round4b: app's REAL present rate from the latency ring (not panel refresh)
        std::string chosenLayer;          // layer name we sampled (debug)
        // v4.2.1: fine-grained WHY the game layer could not be sampled. Empty when
        // available==true. One of:
        //   no_fg                 - foreground package unknown/empty
        //   sf_list_empty         - `dumpsys SurfaceFlinger --list` returned nothing
        //   no_layer_for_pkg      - layers exist but none references the fg pkg
        //   no_valid_frames       - layer selected but --latency had no valid rows
        // (legacy "no_game_layer" is mapped to no_layer_for_pkg by the caller.)
        std::string reason;
        // v4.2.1: up to 8 raw layer names that reference fgPkg (truncated, control
        // chars stripped), for on-device diagnosis via WebUI. "none" when empty.
        std::string candidates;
    };
    // Parse `dumpsys SurfaceFlinger --list` output into candidate layer names.
    std::vector<std::string> sfListLayers(const std::string& listOut);
    // Extract "pkg/pkg.Activity" from a layer's [...] bracket.
    std::string extractBracketedPkg(const std::string& layer);
    // v4.2.1: does the (already-normalized) layer core reference fgPkg?
    //   "pkg/act"        -> leading segment equals fgPkg
    //   "pkg"            -> equals fgPkg (bracket-only-package form)
    //   "pkg.Activity"   -> starts with fgPkg + "."
    bool layerReferencesPkg(const std::string& core, const std::string& fgPkg);
    // v4.2.1: is the activity/class substring a known game-engine class
    // (Unity/Unreal/etc.)? Strong ranking signal (not a hard gate anymore).
    bool isGameEngineClass(const std::string& core);
    // Pick the game render layer. v4.2.1 multi-format (bracket / "SurfaceView -
    // pkg/act" / bare "pkg/act#n" / bracket-only-package) + relaxed rule: a layer
    // whose package EXACTLY matches fgPkg is accepted even without an engine
    // keyword (covers 王者/Cocos/libGDX/自研). IME/camera/video/ad blacklist still
    // applies first; engine/surface hits rank ahead of bare matches.
    //   forceGame: manual-list hit -> treat as game unconditionally.
    //   outReason/outCandidates: optional diagnostic sinks (v4.2.1).
    std::string selectGameLayer(const std::vector<std::string>& layers,
                                const std::string& fgPkg,
                                bool forceGame = false,
                                std::string* outReason = nullptr,
                                std::string* outCandidates = nullptr);
    // Pure orchestration: select layer, fetch via callback, parse. `fetchLatency`
    // returns the raw `--latency "<layer>"` dump text.
    SfFrameSample sampleGameLayer(const std::string& listOut, const std::string& fgPkg,
                                  const std::function<std::string(const std::string&)>& fetchLatency,
                                  bool forceGame = false);
    // v4.2.1 manual game list (game_overrides.list) — PURE parsers (host-tested).
    // Parse a game_overrides.list body: one package per line, '#' comments, blank
    // lines ignored, whitespace trimmed, de-duplicated, only [A-Za-z0-9_.] with a
    // leading alpha accepted (illegal tokens dropped).
    std::set<std::string> parseGameOverridesContent(const std::string& content);
    // Parse `pm list packages[-3]` output ("package:com.foo\n...") into pkg list.
    std::vector<std::string> parsePmPackageList(const std::string& pmOutput);
    struct TimestatsResult {
        bool available = false;
        std::string layer;
        int totalFrames = 0;
        double avgFps = 0.0;
        double missRate = 0.0;
        double p90Fps = 0.0;
        double p99Fps = 0.0;
        double targetFps = 0.0;     // v4.2.6: inferred target frame rate from histogram mode
        int activeFrames = 0;       // v4.2.6: active (non-background) frames used for stats
        double onCadenceRatio = 0.0; // v4.2.6: active frames on-cadence (≤1.5×target) / active
        // v6.0.0 round4b: raw cumulative bucket histogram (bucketMs -> count) so the
        // caller can compute a DELTA between dumps (SurfaceFlinger timestats is a
        // cumulative histogram since layer creation, not a rolling window).
        std::vector<std::pair<double,long long>> hist;
        // v4.1.1 final-round: WHY the channel is unavailable (only meaningful when
        // available==false). One of: disabled | selinux_denied | parse_empty |
        // no_layer | unknown. Classified from the raw -dump stdout; empty when the
        // channel succeeded.
        std::string reason;
    };
    // Parse a `--timestats -dump` block; periodMs = detected refresh; fgPkg
    // selects only the layer block whose bracket package == fgPkg (F-01).
    TimestatsResult parseTimestatsBlock(const std::string& dump,
                                        const std::string& fgPkg = "");
    // T4: layer-name whitelist ([A-Za-z0-9_./()#@[] -]).
    bool isLayerNameSafe(const std::string& s);

    // v4.2.7: testable pure functions for frame metrics.
    bool isTransientFocus(const std::string& prevFg, const std::string& newFg);
    bool sfLatencySamplesValid(const std::vector<long long>& us);
    long long frameBinderBackoffMs(int retryCount);
    // FgDebouncer: foreground app debouncer (prevents transient focus switches).
    struct FgDebouncer {
        std::string committed;
        std::string pending;
        long long pendingSinceMs = 0;
        static constexpr long long kCommitDelayMs = 3000;
        std::string onRawFg(const std::string& raw, long long nowMs);
    };
    // Timestats session state machine (host-testable; fetch injects stdout).
    struct TimestatsSession {
        bool enabled = false;
        long long enabledAtMs = 0;
        long long unavailableUntilMs = 0;
        long long lastDumpAtMs = 0;
        int forkCalls = 0;
        // v4.1.1 final-round: last classified reason (survives the TTL short-circuit
        // so the panel can show WHY even when tick() early-returns an empty result).
        std::string lastReason;
        // v4.2.8: raw dump content from last successful dump (for multi-package parsing).
        std::string lastDumpContent;
        // v6.0.0 round4c: per-layer incremental snapshot (SurfaceFlinger timestats is a
        // CUMULATIVE histogram since enable; naive active-weighted-mean dilates idle
        // gaps with old fast frames -> fake low fps). We diff totalFrames against the
        // previous tick so fps = Δframes/Δwallsec reflects ONLY the current window.
        std::string snapLayer;
        long long snapTotalFrames = -1;
        long long snapDumpMs = 0;
        TimestatsResult tick(long long nowMs, const std::string& fgPkg,
                             const std::function<std::string(const std::string&)>& fetch);
        void stop(const std::function<std::string(const std::string&)>& fetch);
    };
    // v6.0.0 round5: choose the foreground fps. Latency (real present timestamps)
    // wins whenever it has valid frames; timestats is only a fallback + cross-check.
    // outSource: 0=latency, 1=timestats, -1=none. outSuspect=true when both agree
    // but disagree beyond tolerance (relGap>0.20 or absGap>15fps).
    double chooseForegroundFps(double latencyRenderFps, double latencyHz,
                              bool latencyAvail, double tsAvgFps, int tsActiveFrames,
                              int& outSource, bool& outSuspect);
} // namespace frameaware
namespace gamelist {
    // v4.2.1 runtime manual-list (mtime-cached hot reload; file in upgrade area).
    bool isManualGame(const std::string& pkg);
    std::string currentManualGameList();          // newline-joined, for WebUI
    std::vector<std::string> manualGameListSnapshot(); // v4.3.1: vector snapshot (诉求2)
    bool addManualGame(const std::string& pkg);    // validates + appends, returns ok
    bool removeManualGame(const std::string& pkg);
    // v4.2.6: fallback foreground detection — find which manual game is currently resumed.
    std::string detectManualGameForeground();
}
namespace apps {
    // v4.2.1 installed third-party packages (pm list packages -3, with fallback
    // chain). Cached briefly to avoid a fork on every WebUI poll.
    std::vector<std::string> listInstalledPackages();
}
}

// ROUND-014 P3 §2.1 — 帧/jank 统一收口（所有帧通道经此判票）。
void aatpeSetFrameJank(const char* source, int totalSamples, double missRate,
                      double avgFps, double p90Fps, double p99Fps);

// v4.2.1 T3: per-app rolling frame statistics. Keyed by foreground package.
// LRU-capped at 24 packages; backgrounded packages keep their last frozen window.
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
    long total_frames = 0;  // v4.2.6: total rendered frames in window (denominator for jank%)
    long active_ms = 0;
    long long last_update_ms = 0;
    double target_fps = 0.0;   // v4.2.6: inferred target refresh (0 = unknown -> UI shows --)
    // v4.3.1 (诉求2): every manual-list package appears here, even if never run.
    std::string reason;        // "" = fresh data; "background" = on list but >60s stale;
                               // "waiting_first_sample" = on list, never observed a frame yet.
    std::string frameSource;   // "timestats"/"gfxinfo"/"latency_*"/"" (inherit global source)/"none".
    bool isCurrent = false;    // this package == currentPkg.
    long long lastSeenMs = 0;   // wall-clock ms of last observed window; 0 = never seen.
    long long lastSeenEpochMs = 0;  // v4.3.5: real epoch ms (system_clock) for UI age display; 0 = never.
    // rel12 (Bug1): raw missed-vsync accumulator. "丢帧 dropped" is now the real
    // janky/missed FRAME count (<= total_frames); this preserves the vsync count
    // (one late frame can miss MANY vsyncs, so it may exceed total_frames) for the
    // detail panel, labelled "一帧可错过多个vsync".
    long missed_vsyncs = 0;
};
// Fills `out` with all tracked per-app stats (most-recently-updated first),
// sets `currentPkg` to the current foreground package name. Returns count.
int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>& out,
                               std::string& currentPkg);

// v4.2.8: authoritative frame subsystem snapshot (single source of truth for
// frame_debug_fg / frame_source / frame_unavailable_reason state files).
// Written every tick by the frame sampling loop; read by webui_json.
void frameSetAuthSnapshot(const std::string& fg, const std::string& source, const std::string& reason);
void frameGetAuthSnapshot(std::string& fg, std::string& source, std::string& reason);

// v6.0.0 round5b P2: 120°C emergency hard wall is FAIL-SAFE. It MUST fire
// regardless of the user's thermalEnabled switch (which only disables regular
// thermal-aware scheduling). Header-only inline so host tests can verify the
// invariant without linking the whole thermal.cpp TU. wall<=0 backstops to 120.
inline bool aatpeThermalEmergencyHardWall(int anyCpuTempC, int emergencyWallC) {
    if (emergencyWallC <= 0) emergencyWallC = 120;
    return anyCpuTempC >= emergencyWallC;
}

#endif // AATPED_H

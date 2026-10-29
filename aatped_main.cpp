// AATPE - Adaptive Android Task Placement Engine (fix84/3084 baseline)
// AATPE - Adaptive Android Task Placement Engine (core daemon - fix45 split)
#include "event_monitor.h"
#include "input_monitor.h"   // P1-6: touch immediate boost
#include "cold_start_rules.h" // P1-7: game cold-start preset
#include "gpu_boost.h"        // P2-5: CPU/GPU joint boost
#include "pre_boost_predict.h" // C1: predictive pre-boost (default-off)
#include "thermal_headroom.h"  // C3: thermal headroom smooth derating (default-off)
#include "scene_infer_wiring.h" // W5: scene inference slow-thread wiring (default-off)
#include "c4_freezer.h"          // v4.4.4 C4 cgroup v2 freezer (default-off)
#include "aatpe_federated.h"
#include "aatpe_ebpf.h"  // fix100: eBPF scheduler init
// This file contains: globals, logging, file utils, config, topology,
// readCpuMetrics, cpuset, checkSystemLoad, signal handlers, control commands,
// daemonInit, daemonLoop. Scoring/thermal/frame/thread_scan/placement/webui
// are in separate .cpp files (see aatped.h).
#include "aatped.h"
#include "placement.h"
#include "webui_http.h"
#include "integrity.h"
#include "adrc.h"  // ADRC feedforward prediction + ML fusion
#include "aatpe_state_ttl.h"     // #77 TTL sweep + #35 placement avg stats
#include "aatpe_helper_audit.h"  // #74 helper dir content audit
#include "aatpe_session_token.h" // #61 short-lived session bearer tokens
#include "aatpe_diag.h"          // ROUND-009: holistic PSI/GPU/throttle diagnostics
#include "aatpe_caps.h"          // ROUND-010: universal device capabilities probe
#include "aatpe_placement_mech.h" // ROUND-013: cgroup uclamp restore on shutdown
#include "aatped_psi.h"          // v4.3.6: PSI decision closed-loop wiring
#include "aatped_arbitrator.h"  // v4.3.7: multi-source arbitration (game_scene/device_profile/load_forecast/context)
#include "aatpe_recovery.h" // round-3: unified persistent recovery records
// ROUND-014 P3: G5 shutdown functions (defined in placement.cpp / gpu_boost.cpp).
namespace aatpe { namespace placement { void releaseAllSpilloverLocked(); } }
extern void aatpeGpuBoostForceZero(const char* reason);
namespace aatpe { int aatpeThermalHalLevel(); bool aatpeThermalDegraded();
bool aatpeThermalMayFreezeTraining(bool thermalEnabled); const char* aatpeThermalFreezeReason(); }   // thermal.cpp
#include <mutex>
#include <thread>
#include <chrono>
#include <deque>
#include <map>
#include <unordered_set>   // MAIN-01: live-tid snapshot set for dead-tid eviction
#include <regex>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>   // W-1: http_state port-busy classification probe
#include <netinet/in.h>   // W-1: sockaddr_in for the 8888 busy probe
#include <sys/statvfs.h>  // ROUND-014 P3: tmpfs budget
#include <sys/inotify.h> // fix93: config hot-reload
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/wait.h>
#include "aatpe-constants.h"
#include "aatpe_subsystem.h" // round-5: unified lifecycle/health/safe-mode/self-obs

using namespace aatpe;

#include "aatpe-version.h"
#include "aatpe_pid_identity.h"   // P1-2: unified PID triple-identity verification
#include "aatpe_ml.h"
#include "aatpe_ml_hybrid.h"  // v66: hybrid learning (experimental)
using namespace aatpe_ml;
using namespace aatpe_ml_hybrid;

// Phase3 Batch1: Unified system state machine static variables
// [split] state functions moved to aatped_state.cpp

// fix23 marker: strings aatped | grep AATPE_FIX23  (verifies binary contains fix23 build)
// fix49 marker: Android 10-17 full compatibility build (minSdk=29 / NDK r29)

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

// fix21: Global ML model manager (fix52: per-pool logistic regression + SGD)
LearningModel g_mlModel;
time_t g_mlLastSave = 0;
std::mutex g_mlMutex;
// fix39: ML persistence diagnostic counters.
// B2/H04: std::atomic (memory_order default seq-cst; relaxed would also suffice)
// because the periodic-save path (here) and the control model_save= path
// (aatped_control.cpp) both ++ these AFTER releasing g_mlMutex, while
// updateMlSaveStateFiles() snapshots them. A plain unsigned long RMW across two
// threads is C++ UB / lost updates on arm64. Atomic makes the ++ and the
// snapshot race-free without moving disk IO back under g_mlMutex.
std::atomic<unsigned long> g_mlSaveSuccess{0};
std::atomic<unsigned long> g_mlSaveFail{0};

// Frame-time outcome providers (global instances)
aatpe_ml::GfxInfoFrameTimeProvider g_gfxInfoProvider;
aatpe_ml::SurfaceFlingerLatencyProvider g_sfLatencyProvider;

// Get current frame-time snapshot (cached, TTL-aware). Returns sample_count=0 if unavailable.
aatpe_ml::FrameTimeOutcome getCurrentFrameTimeSnapshot() {
    // v94 P2 问题16: mlFrameTimeOutcome is a bool read by the ML outcome path
    // (which can run on the main loop AND from webui_json writeSystemJson).
    // Snapshot it under g_cfgMutex to avoid a torn read racing with loadConfig().
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

// SF latency callback wrapper (reuses frame.cpp's optimized detection)
static int sfLatencyCallbackWrapper(int refreshRate, int& outMaxLat, bool& outValid) {
    return aatpeDetectSFLatency(refreshRate, outMaxLat, outValid);
}

static std::string foregroundAppCallbackWrapper() {
    // v94 fix: read under g_foregroundMutex to avoid racing with onSwitch
    std::lock_guard<std::mutex> fgLock(g_foregroundMutex);
    return g_foregroundApp;
}

// Initialize frame-time outcome callbacks (called in daemonInit)
void initFrameTimeOutcomeCallbacks() {
    aatpe_ml::g_sfLatencyCallback = sfLatencyCallbackWrapper;
    aatpe_ml::g_foregroundAppCallback = foregroundAppCallbackWrapper;
    LOG_INFO("Frame-time outcome: callbacks initialized (ML_FRAME_TIME_OUTCOME=%d, weight=%.2f)",
             g_cfg.mlFrameTimeOutcome ? 1 : 0, (double)g_cfg.mlFrameTimeWeight);
}

// v66: Hybrid learning globals (experimental, default disabled)
std::mutex g_hybridMutex;
static aatpe_ebpf::ThreadCpuRuntimeAggregator g_threadCpuAggregator;
static long long g_hybridLastTickMs = 0;  // steady_clock milliseconds for 500ms tick
static std::atomic<uint64_t> g_hybridEbpfSamplesTotal{0};  // cumulative eBPF sample count

// ============================================================================
// fix50: ML model path resolution + persistence hardening
// ----------------------------------------------------------------------------
// PROBLEM (root cause of "relearn after reboot"):
//   ML_MODEL_PATH is hardcoded to /data/adb/modules/aatpe/model.dat. On KernelSU
//   the module actually lives in /data/adb/ksu/modules/aatpe/, and
//   /data/adb/modules is only a compat symlink that may not exist / not be
//   resolved at the moment the daemon boots. So the daemon can SAVE the model
//   at runtime (path writable) but then FAIL to LOAD it at next boot -> silent
//   cold start -> weights/samples/alpha all reset and the model "relearns from
//   scratch after every reboot".
//
// FIX:
//   1) mlModelPath(): resolve the real module dir at runtime by probing both
//      Magisk (/data/adb/modules/aatpe) and KernelSU (/data/adb/ksu/modules/aatpe)
//      layouts and using the first writable one. save() and load() therefore
//      always use the same physical file.
//   2) backupModelFile(): after every successful save also write model.dat.bak.
//   3) loadModelWithBackup(): at boot, if loading the primary file fails, fall
//      back to model.dat.bak so a transient failure can never wipe the learned
//      state; if both fail, the caller re-inits to a clean cold start and logs
//      the exact reason (errno + file stat) to /dev/aatpe/aatpe.log.
// ============================================================================
static std::string g_mlModelPathStr;
static std::once_flag g_mlModelPathOnce;
const char* mlModelPath() {
    std::call_once(g_mlModelPathOnce, []() {
        // fix-v96 #8: resolve the ML model path through the SINGLE canonical
        // module root (getModuleDir(): Magisk / KernelSU / APatch / pending
        // update), instead of a hardcoded Magisk/KSU list. This keeps save() and
        // load() on the same physical file across all manager layouts.
        std::string mod = getModuleDir();
        if (!mod.empty()) {
            g_mlModelPathStr = mod + "/model.dat";
            return;
        }
        g_mlModelPathStr = ML_MODEL_PATH;
    });
    return g_mlModelPathStr.c_str();
}

// fix101 P7: State structs defined in aatped_state.h
GpuState g_gpuState;
AudioState g_audioState;
SceneState g_sceneState;

void backupModelFile(const char* path) {
    if (!path || !path[0]) return;
    std::string bak = std::string(path) + ".bak";
    // RTC-01 (P2): the tmp name MUST be unique per call, not per process. The old
    // "<bak>.tmp.<getpid()>" meant the control thread (aatped_control.cpp model_save,
    // moved out of g_mlMutex) and the periodic-save thread (aatped_main.cpp ~4189,
    // already outside the lock) could both backup the SAME model.dat concurrently:
    // same pid => same tmp path, one O_TRUNC truncated the other's in-flight stream,
    // and rename() published a CRC-torn model.dat.bak (the primary model.dat itself
    // stays intact because model save is serialized by the model's internal mutex_;
    // only the rollback backup was corrupted). Mirror writeFileAtomic (aatped_utils.cpp):
    // mkstemp() creates a fresh "<bak>.tmp.XXXXXX" leaf (0600, O_EXCL) per call —
    // the leaf never pre-exists, so no trailing symlink can be followed (the old
    // O_NOFOLLOW defense is subsumed by mkstemp's atomic create).
    std::string tmpBak = bak + ".tmp.XXXXXX";
    int in = open(path, O_RDONLY);
    if (in < 0) return;
    // fix55 (问题51): Atomic backup write (tmp + fsync + rename),
    // matching the atomicity of the primary model save.
    int out = mkstemp(&tmpBak[0]);
    if (out < 0) {
        // P1-fix (issue 22): previously this path returned silently with no log,
        // making "why isn't model.dat.bak updating" impossible to diagnose.
        LOG_WARN("backupModelFile: mkstemp(%s) failed: %s", tmpBak.c_str(), aatpeErrStr(errno));
        close(in);
        return;
    }
    {
        char buf[4096];
        ssize_t n;
        bool ok = true;
        // P3-5: EINTR-hardened copy loop. Previously `while(read(...) > 0)` exited
        // the loop on EINTR (read returns -1) with ok still == true, so a signal
        // arriving mid-backup produced a silently truncated model.dat.bak that the
        // atomic rename then published. Retry on EINTR; any other read error fails
        // the backup (ok=false) so the rename is skipped and the old .bak survives.
        while (true) {
            n = read(in, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;  // EOF
            ssize_t totalWritten = 0;
            while (totalWritten < n) {
                ssize_t w = write(out, buf + totalWritten, (size_t)(n - totalWritten));
                // LED-004: the read loop was EINTR-hardened but the write loop was
                // not; retry on EINTR, fail on any other error.
                if (w < 0) {
                    if (errno == EINTR) continue;
                    ok = false;
                    break;
                }
                totalWritten += w;
            }
            if (!ok) break;
        }
        // LED-004: check fsync/rename return values and log on failure so a
        // durability/publish problem is visible instead of silently leaving the
        // old (or half-written) backup in place.
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
// Load model from `path`; if that fails and a .bak exists, try the backup.
// Returns true if a model was loaded. On total failure, leaves the model as the
// last load() attempt left it (caller re-inits to clean cold start).
static bool loadModelWithBackup(const char* path) {
    if (g_mlModel.load(path)) return true;
    std::string bak = std::string(path) + ".bak";
    if (access(bak.c_str(), F_OK) == 0 && g_mlModel.load(bak.c_str())) {
        LOG_WARN("ML model: primary %s load failed, restored from backup %s", path, bak.c_str());
        return true;
    }
    return false;
}

// [split] logging functions moved to aatped_logging.cpp

#define LOG_ERROR(...) logMsg(0, __VA_ARGS__)
#define LOG_WARN(...)  logMsg(1, __VA_ARGS__)
#define LOG_INFO(...)  logMsg(2, __VA_ARGS__)
// LOG_FORCE defined in aatped.h (shared across all translation units)
// LOG_DEBUG defined in aatped.h with 1% sampling (fix96)

// [split] utility functions moved to aatped_utils.cpp

// [split] config functions moved to aatped_config.cpp

// [split] utility functions moved to aatped_utils.cpp

// ============================================================================
// CONFIG
// ============================================================================

// fix94 Task4: Dynamic score weights (learnable)
DynamicScoreWeights g_dynamicScoreWeights;
std::mutex g_dynamicWeightsMutex;

// fix94 Task5: Softmax fusion with temperature parameter
float g_fusionTemperature = 1.0f;
float softmaxFuseScores(float modelScore, float heuristicScore, float temperature) {
    // v69 Bugfix: temperature boundary protection — negative → abs, zero/tiny → clamp to 0.01
    // Prevents division-by-zero NaN and exponent-sign inversion for negative temperatures.
    if (temperature < 0.0f) temperature = -temperature;
    if (temperature < 0.01f) temperature = 0.01f;
    // Clamp inputs to avoid exp overflow
    float m = modelScore / temperature;
    float h = heuristicScore / temperature;
    float maxVal = (m > h) ? m : h;
    m -= maxVal; h -= maxVal;
    float em = expf(m);
    float eh = expf(h);
    float sum = em + eh;
    if (sum < 1e-10f) return 0.5f * (modelScore + heuristicScore);
    // Weighted average by softmax probabilities
    return (modelScore * em + heuristicScore * eh) / sum;
}

// fix103: Federated learning stats
// HTTP-04: atomic<int> to eliminate TSan data race on cross-thread reads.
std::atomic<int> g_federatedRounds{0};
std::atomic<int> g_federatedExports{0};
std::atomic<int> g_federatedImports{0};
int g_federatedTrustedImports = 0;

// fix103: A/B test stats
float g_abGroupAAvg = 0.0f;
float g_abGroupBAvg = 0.0f;
float g_abPValue = 1.0f;

// fix103: ML prediction stats
float g_avgPredictionMAE = 0.0f;

// fix103: Thermal stats for state.json
// P1-13: atomic — written by thermal sampling thread, read by state writer and
// main loop concurrently.
std::atomic<int> g_thermalCpu{0};
std::atomic<int> g_thermalSkin{0};
std::atomic<int> g_thermalBattery{0};
// P2 Problem14: effective (runtime) decision interval in ms, written every loop
// by daemonLoop after computeAdaptiveInterval() + GPU-bottleneck override. This
// is the RUNTIME layer; g_cfg.decisionInterval is the CLAMPED requested layer.
// state.json exposes both so the WebUI can show requested vs effective.
int g_runtimeDecisionIntervalMs = 200;
std::string g_thermalCpuZonePath;

// fix103: GPU stats for state.json (A2: atomic pointer, see aatped_state.h)
std::atomic<const char*> g_gpuType{"unknown"};
// P1-6: atomic<float> — relaxed order (see aatped_state.h).
std::atomic<float> g_gpuUtilization{0.0f};

// fix103: eBPF loader info
std::string g_ebpfLoader = "none";
// P1-6: atomic<bool> — release/acquire (see aatped_state.h).
std::atomic<bool> g_ebpfActive{false};
// v73: eBPF event-driven mode and counters
std::string g_ebpfMode = "polling";
// P1-2 fix: leaf mutex guarding g_ebpfLoader/g_ebpfMode (std::string). See
// aatped_state.h for the lock-order note.
std::mutex g_ebpfInfoMutex;
std::atomic<uint64_t> g_ebpfEventsTotal{0};
std::atomic<uint64_t> g_ebpfEventsFork{0};
std::atomic<uint64_t> g_ebpfEventsExit{0};
std::atomic<uint64_t> g_ebpfEventsSwitch{0};
std::atomic<uint64_t> g_ebpfRingLost{0};
std::atomic<double> g_ebpfEventRate{0.0};

// fix103: Start time for uptime calculation
// G1-007: atomic so webui/control threads reading ForegroundManager::startTime()
// / uptime don't race the main loop's publish at daemon start.
std::atomic<time_t> g_startTime{0};

// [split] HMAC functions moved to aatped_hmac.cpp

// [split] game scene functions moved to aatped_gamescene.cpp

// [split] battery power functions moved to aatped_battery.cpp

// [split] A/B test functions moved to aatped_abtest.cpp

// [split] battery health functions moved to aatped_battery.cpp

// [split] GPU functions moved to aatped_gpu.cpp

// [split] audio functions moved to aatped_audio.cpp

// [split] app launch functions moved to aatped_applaunch.cpp

// [split] uevent fallback functions moved to aatped_uevent.cpp

// [split] foreground app cache moved to aatped_ebpf_agg.cpp

// ==================== fix95 Task3: lazy decision dir cleanup ====================
void lazyCleanupDecisionsDir(void) {
    static long long s_lastCleanupMs = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - s_lastCleanupMs < 10000) return; // check every 10s
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
        // batch delete oldest 50%
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

// ==================== v94 fix: dead incrementalThreadScan removed ====================
// Audit breakpoint 6: g_scannedThreadMap + incrementalThreadScan() were dead
// code. They wrote to a separate map that nobody read, and had zero callers.
// Real-time g_threads updates now come from onFork/onExec/onSwitch. The whole
// block (was ~70 lines of /proc walk writing to a throwaway map) is deleted.

// ==================== fix95 Task6: watchdog takeover scheduling ====================
void watchdogTakeoverScheduling(void) {
    LOG_INFO("fix95: watchdog takeover - binding critical threads to mid/big cores");
    // C-003: must hold g_topologyRwMutex when reading g_midPool/g_bigPool.
    //
    // P1-fix (issue 28): snapshot the pools under the lock, then RELEASE the lock
    // before walking /proc. The /proc walk does hundreds of opendir/fopen/sched_setaffinity
    // syscalls; holding g_topologyRwMutex across that walk blocked every other
    // topology reader (placement, metrics) for the duration of the scan.
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
    // LED-003 / LED-014 (R1-003/R1-014): the old code walked /proc/<pid>/comm
    // (which is ONLY the main-thread comm) and matched "RenderThread"/"binder:main"
    // — those are sub-thread names that never appear in a process's main comm, so
    // the comparison was dead and sched_setaffinity(TGID,...) only ever pinned the
    // main thread. It also stopped after readdir's first 10 numeric entries, making
    // which threads got bound non-deterministic.
    // Root-cause fix: walk each process's /proc/<pid>/task/ and read every tid's
    // comm, then sched_setaffinity() on the real tid so render/binder threads are
    // actually pinned. Bind ALL matching critical threads (no readdir-order cap).
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

// ==================== fix95 Task7: adaptive decision interval ====================
// v94 fix: configInterval (g_cfg.decisionInterval) is now the baseline.
// Adaptive logic only overrides in extreme cases (jank/high-load speed-up,
// idle no-foreground slow-down). Previously this function returned hardcoded
// 50/100/500ms and completely ignored DECISION_INTERVAL from aatpe.conf.
//
// v94-P0-fix (CPU占用高根因): 原逻辑 `if (jankDetected) return 50ms` 位于
// `systemLoad>80%` 判断之前, 构成短路 —— 一旦 jank 就恒返 50ms, 高负载减速
// 逻辑(L427)永远到不了。这导致高负载 + 丢帧场景下 daemon 以 50ms 快循环
// 狂奔, 自身 CPU 占用暴涨、加剧发热与卡顿。
// 修复: jank 加速只在系统未高负载时生效(systemLoad < 80%); 高负载时即使
// jank 也必须降到 200ms 基准(最低不低于 configInterval), 让系统喘息。
int computeAdaptiveInterval(float systemLoad, bool jankDetected, bool hasForeground, int configInterval) {
    // v95-OPT3 (CPU 压降): 更激进的自适应决策间隔。
    // 根因: 旧逻辑空闲时仍 200ms(5次/秒), 高负载 500ms, 中负载 200ms。
    // 新逻辑(按用户规格):
    //   低负载 (<30%)  : 1000ms (1次/秒, 系统空闲, 不需要高频决策)
    //   中负载 (30-70%): 500ms  (2次/秒, 平衡)
    //   高负载 (>70%)  : 1000ms (1次/秒, 少干活让系统喘口气, 避免 daemon 自身
    //                          CPU 叠加到已经高负载的系统上加剧卡顿/发热)
    //   jank           : 50ms (仅在中负载时 — 低负载无 jank 理据, 高负载加速雪上加霜)
    // 关键线程(UI/RENDER/GAME)始终 fresh 采样 + jank 上升沿走 emergencyMigrate,
    // 不受此间隔影响, 调度效果保留。
    (void)hasForeground;  // v95-OPT3: 新逻辑用负载分档, 不再单独看前台 app
    // P2fix: negative systemLoad means CPU read failure — don't treat as idle
    // (which would silently drop to 1Hz). Clamp to medium load default.
    if (systemLoad < 0.0f) systemLoad = 50.0f;
    if (systemLoad < 30.0f) return 1000;       // idle: 1s
    if (systemLoad > 70.0f) return 1000;      // high load: 1s (let system breathe)
    // medium load 30-70%
    // LED-005: if DECISION_INTERVAL was configured <=0, std::min(configInterval,50)
    // returned a non-positive interval; downstream sleepMs(base-elapsed) went
    // negative and only a later clamp saved it, losing the "jank speeds up to
    // configInterval" semantics. Floor the result at 1ms.
    if (jankDetected) return std::max(1, std::min(configInterval, 50));  // jank speed-up, cap 50ms
    return 500;                               // medium: 500ms
}

// ==================== fix95 Task9: thread role match cache ====================
std::unordered_map<std::string, int> g_threadRoleCache;
std::mutex g_threadRoleCacheMutex;  // C-007: protects g_threadRoleCache

int getCachedThreadRole(const std::string& threadName) {
    std::lock_guard<std::mutex> lock(g_threadRoleCacheMutex);
    auto it = g_threadRoleCache.find(threadName);
    if (it != g_threadRoleCache.end()) return it->second;
    return -1; // cache miss
}

void setCachedThreadRole(const std::string& threadName, int role) {
    std::lock_guard<std::mutex> lock(g_threadRoleCacheMutex);
    g_threadRoleCache[threadName] = role;
}

void invalidateThreadRoleCache(const std::string& threadName) {
    std::lock_guard<std::mutex> lock(g_threadRoleCacheMutex);
    g_threadRoleCache.erase(threadName);
}

// ==================== fix95 Task10: schedstat CPU utilization ====================
std::vector<SchedstatCpuData> g_schedstatData;

bool readSchedstatCpuUtil(void) {
    FILE* fp = fopen("/proc/schedstat", "r");
    if (!fp) return false;
    char line[512];
    g_schedstatData.clear();
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        SchedstatCpuData data;
        // format: cpuN <running_ns> <waiting_ns> <timeslices>
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

// ==================== fix95 Task11/12: enum conversion ====================
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

double g_abTestLatestP = 1.0; // fix101 P6: latest A/B test p-value

// fix100: Global eBPF scheduler instance (was never instantiated before)
static aatpe_ebpf::EbpfScheduler g_ebpfScheduler;

// [split] eBPF event consumer moved to aatped_ebpf_agg.cpp

// fix97 P3: Federated learning global instance
static aatpe_federated::FederatedLearner g_federatedLearner;
static long long g_lastFederatedExportMs = 0;
static long long g_lastFederatedImportMs = 0; // fix96: system load for adaptive fusion
Config g_cfg;
// D-05: Mutex to protect g_cfg from concurrent access
// v94 P2 问题16: see aatped.h for the full synchronization contract.

// CPU metrics & thread database (defined here, used by all split files)
std::map<int, CpuMetrics> g_cpuMetrics;
std::mutex g_cpuMetricsMutex;
// P0-opt2: unordered_map for O(1) tid lookup (was std::map O(log n) red-black tree).
std::unordered_map<int, ThreadInfo> g_threads;
std::mutex g_threadsMutex;

// ============================================================================
// v94-PLAN3: Event-driven scoring cache state (see aatped_utils.h).
// Single-threaded on the daemon main loop; onSwitch() runs on that same loop.
// ============================================================================
// v95-P1fix: g_scoreCacheMutex protects g_scoreCache from concurrent access
// between the eBPF poll thread (degraded path: onSwitch → markThreadScoreDirty)
// and the daemon main loop (operator[] / erase / iteration).
std::unordered_map<int, ThreadScoreEntry> g_scoreCache;
std::mutex g_scoreCacheMutex;
const long long kScoreTtlMs   = 500;   // cached decision valid for 500ms
const long long kInactiveMs   = 10000; // no sched_switch in 10s -> inactive/sleeping
// v95-OPT5: 全局重平衡兜底从 1.5s 收紧到 1s。事件驱动主循环下这一步开销小
// (只 invalidate cache, 不重算所有线程), 1s 粒度保证不静默过期。
const long long kRebalanceMs  = 1000;  // global full-sweep backstop every 1s
// v95-OPT5: dirty 线程阈值触发重平衡。当一帧内 dirty 线程数超过此阈值(大量
// sched_switch 同时涌入), 提前做一次全量重平衡, 避免脏数据堆积太久。
const int kDirtyRebalanceThreshold = 16;

void markThreadScoreDirty(int tid, long long nowMs) {
    std::lock_guard<std::mutex> lock(g_scoreCacheMutex);
    auto it = g_scoreCache.find(tid);
    if (it != g_scoreCache.end()) {
        it->second.dirty = true;
        it->second.lastSwitchMs = nowMs;
        // rel15 大小核颠倒 fix: a real sched_switch was observed for this tid.
        // If the thread was previously frozen by skipSchedstat (idleRefreshes>=6),
        // thread_scan consumes this hint on the next calibration scan and breaks
        // the freeze immediately instead of waiting up to 10 cycles for the
        // forced rescan. Harmless for threads that are already active.
        it->second.wakeHint = true;
    }
}

// rel15 大小核颠倒 fix: set the one-shot forceRescore bypass on EVERY cached
// score entry. Called by the main loop when a key control transition happens
// (notably EMERGENCY->NORMAL) so threads that were moved to little cores during
// emergency (or whose active signal was frozen) are re-scored once and promoted
// back, even though shouldRescoreThread's active gate would otherwise skip them.
// Each entry consumes its own forceRescore on the next decision cycle, so this
// does NOT become a per-second full sweep.
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

// v94 P2 问题16: Thread-safe config snapshot. Copies g_cfg into `out` under
// g_cfgMutex. The copy itself is a shallow struct copy (std::string members are
// copied), so it is safe to use `out` after releasing the lock.
// [split] config functions moved to aatped_config.cpp

// v94 P2 问题19: Monotonic state generation counter (see aatped.h).
// Definition lives in aatped_utils.cpp so unit tests that link aatped_utils.o get
// the symbol; this TU only references it via the extern in aatped.h.

// Snapshot for thermal emergency recovery (fix: don't full loadConfig on recovery)
Config g_preEmergencyCfg;
std::string g_preEmergencyProfile = "balanced";
bool g_preEmergencyAutoProfile = 1;

std::string g_currentProfile = "balanced";
RefreshRateMode g_refreshRateModeEnum = RefreshRateMode::PEAK;

// fix93: Adaptive decision interval
// fix93: DEBUG log sampling (1% sample rate)
// fix93: Config hot-reload
std::atomic<bool> g_reloadConfigRequested{false};
// v71: mtime-based config hot-reload (complements inotify; works on filesystems
// where inotify is unreliable, e.g. some FUSE/overlay mounts).
// [split] config functions moved to aatped_config.cpp

// fix93: Per-CPU utilization via schedstat
// P1-4: Two-sample delta formula with per-CPU last sample tracking.
// P2fix: cache last computed util per CPU so file-open failure returns the
// last known value instead of -1 (which was misinterpreted as low load).
//
// P1-fix (issue 21): these maps are reachable from DefaultCpuMonitor (WebUI/ML
// threads) as well as the main loop. Add a dedicated mutex to prevent data
// races on unordered_map insertion/lookup.
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
        return 50.0f;  // default medium load if no prior reading
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

// [split] config functions moved to aatped_config.cpp

// fix93: enum for hot path
ProfileType g_currentProfileEnum = ProfileType::BALANCED;
std::atomic<bool> g_thermalEmergency{false};
std::atomic<int> g_thermalMigrationPressure{0};
// C1/C3 exploration: thermal headroom (atomic, written by thermal.cpp, read by C1 gate + C3).
// Default -1 = invalid (before first thermal sample). Default switches = false (v4.4.1 byte-identical).
std::atomic<int> g_thermalHeadroomPct{-1};
std::atomic<float> g_thermalPlacementScale{1.0f};
std::atomic<bool> preboostEnabled{false};    // parsed from aatpe.conf PREBOOST_ENABLED
std::atomic<bool> thermalSmoothEnabled{false}; // parsed from aatpe.conf THERMAL_SMOOTH_ENABLED
std::atomic<bool> sceneInferEnabled{false};  // parsed from aatpe.conf SCENEINFER_ENABLED
std::atomic<bool> c2EnvelopeEnabled{false};  // parsed from aatpe.conf C2_ENVELOPE_ENABLED
std::atomic<bool> g_frameCritical{false};
// v95-fix: independent safety-latency gate (replaces the old controlMode="passive"
// override which never restored and stuck the daemon in passive mode).
std::atomic<bool> g_safetyLatencyActive{false};
std::atomic<bool> g_profileLock{false};
std::atomic<bool> g_skipNextSleep{false};  // fix84 P2-2: medium-priority event triggers immediate full decision
// (JSON 降频): live SSE /events connection count. webui_http.cpp
// bumps it on each handleSseConnection entry and RAII-decrements on exit. The
// daemon main loop reads it to decide whether the heavy threads/decisions/explain
// JSON serialization must stay on the 2s cadence (a browser is attached and
// streaming) or can be throttled to once per 5s (no EventSource reader). Core
// state files (decision_count/daemon_uptime/ebpf_event_rate, written under the
// 1s diagTick gate above) are UNCONDITIONAL and never gated by this counter.
std::atomic<int> g_sseClientCount{0};
// (33ms 节流): steady_clock timestamp of the START of the last full
// decision cycle. After an important-event wake (self-pipe) the main loop checks
// now - lastDecisionSteady; if < decisionMinIntervalMs (default 33ms) it sleeps
// the remainder so touch-event bursts / fork storms cannot fire a full decision
// more often than the configured minimum.
std::chrono::steady_clock::time_point g_lastDecisionSteady{};
// v95-OPT1 (事件驱动主循环): self-pipe 用于把 eBPF 关键事件 / control 文件变更
// 异步唤醒主循环的 poll()。写入端在 poll 线程(关键事件 hook)和 control inotify 回调中,
// 读取端在主循环末尾的 poll()。空闲时主循环阻塞在 poll 上直到有事件或超时, CPU≈0。
// pipe 在 daemonInit() 早期创建(早于 eBPF hook 安装), 主循环退出时不关闭(进程生命周期)。
int g_wakeupPipeReadFd = -1;
int g_wakeupPipeWriteFd = -1;
// control 文件 inotify watch: WebUI/CLI 写 /dev/aatpe/control 时立即唤醒主循环。
int g_controlInotifyFd = -1;
int g_controlInotifyWd = -1;
// 把任意事件转成一次 poll() 唤醒。async-signal-safe-ish(只 write 一个字节, 非阻塞)。
static inline void wakeMainLoopNow() {
    if (g_wakeupPipeWriteFd >= 0) {
        char b = 1;
        ssize_t r = write(g_wakeupPipeWriteFd, &b, 1);
        (void)r;  // EAGAIN 表示 pipe 已满(已有人待唤醒), 忽略即可
    }
}
int g_cpuCount = 0;
std::atomic<uint64_t> g_topologyGeneration{0};  // fix55 (问题59/60/61); P1-4 atomic
std::vector<int> g_allCpus;
std::vector<int> g_littlePool;
std::vector<int> g_midPool;
std::vector<int> g_bigPool;

// Global cumulative counters (fix: decision_count and migration_count were local/reset)
std::atomic<long long> g_totalDecisions{0};
std::atomic<long long> g_totalMigrations{0};
// fix25: 高负载降频循环计数器 (不再跳过决策, 仅降低频率和强度)
std::atomic<long long> g_highLoadCycles{0};
int g_migrationCount = 0;  // per-second rate limiter
time_t g_migrationWindowStart = 0;
// fix29: KEEP reason diagnostic counters (why decisions return KEEP instead of MIGRATE)
std::atomic<long long> g_keepProtected{0};
std::atomic<long long> g_keepPassive{0};
std::atomic<long long> g_keepThermalEmergency{0};
std::atomic<long long> g_keepSafetyLatency{0};
std::atomic<long long> g_keepNoCpu{0};
std::atomic<long long> g_keepLowConf{0};
std::atomic<long long> g_keepHighCost{0};
std::atomic<long long> g_keepSafeMode{0}; // ordinary migration blocked by Safe Mode
std::atomic<long long> g_keepAlreadyOptimal{0};
std::atomic<long long> g_migrateActual{0};
// fix29: applyPlacement diagnostic counters
std::atomic<long long> g_applySuccess{0};
std::atomic<long long> g_applyFailed{0};
// P1-18: sched_setaffinity error code classification (real original)
std::atomic<long long> g_applyFailedEinval{0};
std::atomic<long long> g_applyFailedEsrch{0};
std::atomic<long long> g_applyFailedEperm{0};
std::atomic<long long> g_applyFailedOther{0};
std::atomic<long long> g_applyRateLimited{0};
std::atomic<long long> g_applyPassive{0};
std::atomic<long long> g_applyThermalEmergency{0};
std::atomic<long long> g_applySafetyLatency{0};
std::atomic<long long> g_applyCooldownSkip{0};
// fix37: cpuset-aware scheduling — skip counter for target CPU outside thread's allowed cpuset
std::atomic<long long> g_applyCpusetSkip{0};
// v6.0.0 P1 (realdevice observability): most recent CPUSET_SKIP sample, so the
// panel can show WHICH thread was refused a big-core migration because it was
// parked in the background cpuset. Plain struct guarded by a small mutex; only
// overwritten on the skip path.
CpusetSkipSample g_lastCpusetSkip;
std::mutex g_lastCpusetSkipMutex;
// fix38: kernel thread skip counter (enhanced triple-detection)
std::atomic<long long> g_applyKernelSkip{0};
// fix38: unmigratable thread skip counter (errno=22 or 2+ failures auto-marked)
std::atomic<long long> g_applyUnmigratableSkip{0};
// fix40: per-thread 5-second minimum migration interval skip counter
std::atomic<long long> g_applyIntervalSkip{0};
// fix104 v34: ML learning funnel diagnostics
std::atomic<long long> g_mlPendingCreated{0};
std::atomic<long long> g_mlOutcomeMeasured{0};
std::atomic<long long> g_mlOutcomeValid{0};
// v4.5.0 shadow-sampling funnel: outcomes measured in observe-only (passive) mode
// and routed to the shadow buffer (never trained). Separate from g_mlOutcomeValid
// so formal-vs-shadow collection counts never mix.
std::atomic<long long> g_mlShadowOutcomeValid{0};
std::atomic<long long> g_mlOutcomeSkipSleeping{0};
std::atomic<long long> g_mlOutcomeSkipSaturated{0};
std::atomic<long long> g_mlOutcomeSkipReset{0};
std::atomic<long long> g_mlOutcomeSkipOther{0};
std::atomic<long long> g_mlPendingExpired{0};
std::atomic<long long> g_mlPendingTidReused{0};
std::atomic<long long> g_mlPendingThreadExited{0};
// fix40: APK smart detection state
// fix46: g_helperApkInstalled is now extern (declared in aatped.h) so
//        frame.cpp::aatpeDetectForegroundApp() can route to APK-controlled path.
// C-011: changed to atomic<bool> for thread-safe cross-thread access.
std::atomic<bool> g_helperApkInstalled{false};
std::string g_autoProfileMode = "module_controlled";
// round-3 (P0-7): runtime auto-profile suppression caused by the Helper
// APK being installed. This is SEPARATE from the user's configured AUTO_PROFILE
// (g_cfg.autoProfile). The effective auto-profile decision is
//   effectiveAuto = g_cfg.autoProfile && !g_helperAutoSuppressed.
// When the Helper APK is removed we clear this flag and the user's original
// AUTO_PROFILE setting takes effect again — we never overwrite the user config.
std::atomic<bool> g_helperAutoSuppressed{false};
// fix38: permanently unmigratable threads (errno=EINVAL from sched_setaffinity,
// or 2+ consecutive failures). Persists for daemon lifetime, reset on restart.
std::mutex g_unmigratableMutex;
std::set<int> g_unmigratableThreads;
// fix37: per-thread last migration timestamp (min interval enforcement)
std::mutex g_lastMigMutex;
std::unordered_map<int, time_t> g_lastMigrationTime;  // tid -> last successful migration time

// ============================================================================
// [split] utility functions moved to aatped_utils.cpp

// fix36: Migration failure cooldown (revised from fix35).
// fix35 was too aggressive: 60s cooldown + permanent block after 3 failures
// caused 96.9% of MIGRATE decisions to be skipped, freezing migration_count.
// fix36 changes:
//   1. Removed permanent blocking entirely — only temporary cooldown.
//   2. Cooldown reduced 60s -> 10s (thread state changes fast under load).
//   3. Cooldown only triggers after 2 consecutive failures (not 1).
//   4. Early cooldown release if thread's current CPU changed since failure
//      (indicates thread state / scheduling context has changed).
//   5. Floor guarantee: at least kMigCooldownFloorPerSec migrations per second
//      are allowed to bypass cooldown, preventing total migration freeze.
std::mutex g_migFailMutex;
std::unordered_map<int, time_t> g_migFailCooldown;   // tid -> earliest retry time
std::unordered_map<int, int> g_migFailCount;         // tid -> consecutive failures
std::unordered_map<int, int> g_migFailCpu;           // tid -> cpu at last failure (for state-change detection)
// P3-52: exponential backoff expiry (steady_clock ms). Authoritative retry throttle.
std::unordered_map<int, long long> g_migFailBackoffUntilMs;
// fix36: floor-bypass tracking (per-second window)
std::atomic<long long> g_cooldownBypassCount{0};
time_t g_cooldownFloorWindowStart = 0;

// [split] utility functions moved to aatped_utils.cpp

// [split] config functions moved to aatped_config.cpp

// [split] topology functions moved to aatped_topology.cpp

// ============================================================================
// THREAD CLASSIFICATION
// ============================================================================

// v94 fix56: g_prevCpuStat is deliberately UNPROTECTED by a mutex because it is
// only ever touched by readCpuMetrics(), which is itself only invoked from the
// main daemon loop (single call site, aatped.cpp main loop). Verified by grep:
// no WebUI, log-flush, config-inotify, or frame thread accesses this map.
// getCpuUsagePercent() does NOT use it either -- it keeps its own separate
// g_prevCpuBusy/g_prevCpuTotal/g_prevCpuValid statics for the aggregate load
// sample. Single-threaded access -> safe without a lock. If a second thread ever
// needs per-CPU deltas, introduce a mutex here before doing so.
std::map<int, std::pair<unsigned long long, unsigned long long>> g_prevCpuStat; // cpu -> {idle, total}

// v94-P7-fix: aggregate /proc/stat busy/total captured by readCpuMetrics()'s
// single /proc/stat open. checkSystemLoad()->getCpuUsagePercent() used to
// open /proc/stat AGAIN every loop (readProcStatCpu at ~L833) — same file,
// twice, open+read+close. Now the aggregate first line is captured here during
// the one read and handed to getCpuUsagePercent via these statics, eliminating
// the second open/close syscall pair per loop.
static unsigned long long g_aggProcStatBusy = 0;
static unsigned long long g_aggProcStatTotal = 0;
static bool g_aggProcStatValid = false;

// daemon self CPU usage tracking.
// Root cause: g_lastCpuUsagePercent is SYSTEM-wide CPU usage. When the daemon
// itself burns 90% of one core, system CPU reads ~66% (other cores idle), so
// the wake hook sees "under 80% load" and keeps the 30ms fast coalescing window
// -> 33 wakes/sec -> self-reinforcing CPU burn.
//
// We measure daemon self CPU as a percentage of ONE core (matching `top`'s
// per-process CPU display):
//   deltaSelfJiffies / (deltaTotalJiffies / numOnlineCpus) * 100
// Updated inside readCpuMetrics() on the same 2s /proc/stat cadence.
// g_selfCpuUsagePercent is non-static (read by aatped_state.cpp for state.json).
// P1-1: atomic<int> — written by readCpuMetrics thread, read by eBPF poll thread.
std::atomic<int> g_selfCpuUsagePercent{0};
// low-overhead mode. Activated under SystemState::EMERGENCY OR when the
// host is catastrophically overloaded (VMOS + game script + kswapd storm). In
// this mode the daemon throttles all non-essential work and keeps only
// event-triggered emergency migration. Non-static so stubs/tests can link.
std::atomic<bool> g_lowOverheadMode{false};
static unsigned long long g_prevSelfJiffies = 0;   // utime+stime from /proc/self/stat
static unsigned long long g_prevSelfTotalJiff = 0;  // aggregate total jiffies
static bool g_prevSelfValid = false;

void readCpuMetrics() {
    // v95-OPT4 (CPU 压降): 门控昂贵的 sysfs/procfs 读取。
    // 根因: 旧逻辑每轮(200ms)都 open /proc/stat + N×open scaling_cur_freq,
    //       8 核机器每轮 1 + 8×3 = 25 次 sysfs open/read/close, 纯开销。
    // 新逻辑:
    //   /proc/stat 读取门控到 2s (WebUI 2-5s poll, 2s 粒度足够)
    //   per-CPU freq (cur_freq/max_freq/cpu_capacity) 门控到 5s
    //   thermal zone 扫描保持 1s (readThermal 已有 1s TTL 缓存, 这里只复用)
    // 关键一致性: 门控 /proc/stat 时把 g_aggProcStatValid 置 false, 让
    // getCpuUsagePercent() 返回 g_lastCpuUsagePercent(上次真实值) 而不是
    // 用 stale busy/total 算出 0% delta — 避免 system_load 在 0 和真实值之间抖动。
    static unsigned long long s_lastProcStatMs = 0;
    static unsigned long long s_lastFreqReadMs = 0;
    static std::map<int, int> s_lastCpuUtil;  // 门控时复用上轮 per-CPU util
    unsigned long long nowMetricsMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool doProcStat  = (nowMetricsMs - s_lastProcStatMs >= 2000);
    bool doFreqRead  = (nowMetricsMs - s_lastFreqReadMs >= 5000);
    if (doProcStat) s_lastProcStatMs = nowMetricsMs;
    if (doFreqRead) s_lastFreqReadMs = nowMetricsMs;

    std::map<int, int> cpuUtil;
    if (doProcStat) {
        // Parse /proc/stat for per-CPU utilization
        std::ifstream statf("/proc/stat");
        if (statf) {
            std::string line;
            while (std::getline(statf, line)) {
                if (line.substr(0, 3) != "cpu") continue;
                size_t sp = line.find(' ');
                std::string cpuStr = line.substr(3, sp - 3);
                // Fields: user nice system idle iowait irq softirq steal guest guest_nice
                std::stringstream ss(line.substr(sp));
                unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
                ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
                unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;
                unsigned long long idleTotal = idle + iowait;
                if (cpuStr.empty()) {
                    // P7-fix: aggregate "cpu " line — capture busy/total for
                    // getCpuUsagePercent() so it does NOT re-open /proc/stat.
                    // busy matches readProcStatCpu(): user+nice+system+irq+softirq.
                    g_aggProcStatBusy  = user + nice + system + irq + softirq;
                    g_aggProcStatTotal = g_aggProcStatBusy + idle + iowait + steal;
                    g_aggProcStatValid = true;

                    // bugfix: self CPU %. The old implementation took the
                    // ratio of self-ticks to system-wide jiffies-per-core, which
                    // under extreme system load (VMOS + game script, kswapd storm)
                    // read back 0% even though top showed aatped at 42%. Compute it
                    // the robust way instead: diff /proc/self/stat utime+stime
                    // (clock ticks) over a measured wall interval and divide by
                    // (ticksPerSec * wallSec). This is % of ONE core and is
                    // independent of system-wide jiffies.
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
                    continue; // aggregate is not a per-CPU entry
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
        s_lastCpuUtil = cpuUtil;  // cache for gated loops
    } else {
        // 门控: 不读 /proc/stat。标记 aggregate 无效, 让 getCpuUsagePercent()
        // 返回 g_lastCpuUsagePercent 而不是 stale delta。复用上轮 cpuUtil。
        g_aggProcStatValid = false;
        cpuUtil = s_lastCpuUtil;
    }

    // v94-P1b-fix: DROP the per-loop thermal zone full rescan. readThermal()
    // already scans /sys/class/thermal with a 1000ms TTL cache and publishes
    // g_thermalCpu. Re-scanning every loop here (opendir + readdir + N×open
    // /type + /temp) was pure duplicated work. Reuse the cached value.
    // (g_thermalCpu is read under g_thermalMutex by getters; readCpuMetrics
    //  runs on the main loop after readThermal() each cycle, so the value is
    //  fresh within the 1s TTL.)
    int g_cachedCpuTemp = g_thermalCpu.load(std::memory_order_relaxed);

    // v94-P1a/P1c-fix: per-CPU sysfs STATE writes are display-only for WebUI
    // (which polls at 2-5s). Writing cur_freq/freq_pct/util/temp for every
    // CPU EVERY loop = 4 writes × N cpus each loop. Gate to 1s. The in-memory
    // g_cpuMetrics (read by scoring/placement under shared_lock) is ALWAYS
    // updated fresh — only the on-disk state files are throttled.
    // mkdirIfNotExist is now a one-time safety net (dirs normally created by
    // detectCpuTopology at init), gated by s_cpuStateDirsReady.
    static bool s_cpuStateDirsReady = false;
    static unsigned long long s_lastMetricsWriteMs = 0;
    bool doMetricsWrite = (!s_cpuStateDirsReady) || (nowMetricsMs - s_lastMetricsWriteMs >= 1000);
    if (doMetricsWrite) s_lastMetricsWriteMs = nowMetricsMs;

    // C-008: snapshot g_allCpus under g_topologyRwMutex before iterating.
    // g_allCpus is protected by g_topologyRwMutex, not g_cpuMetricsMutex.
    std::vector<int> cpusSnapshot;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        cpusSnapshot = g_allCpus;
    }

    // v68 P0: Write-side lock — protect g_cpuMetrics write loop with unique_lock.
    // Readers (scoring.cpp, placement.cpp) take shared_lock; this exclusive lock
    // prevents torn reads during the per-CPU metrics update.
    std::unique_lock<std::mutex> wlock(g_cpuMetricsMutex);
    // P1-fix (issue 29): reuse a char buffer for the per-CPU sysfs path instead
    // of allocating a std::string per cpuId per loop (8 cores × 5 loops/sec = 40
    // string allocations/sec just for path construction).
    char baseBuf[128];
    for (int cpuId : cpusSnapshot) {
        CpuMetrics m;
        snprintf(baseBuf, sizeof(baseBuf), "/sys/devices/system/cpu/cpu%d", cpuId);
        std::string base(baseBuf);
        // v95-OPT4: per-CPU freq 门控到 5s。门控时复用上轮 g_cpuMetrics 值,
        // 避免每轮 8×3 次 sysfs open。scoring 用 curFreq 做容量估计, 5s 粒度足够。
        auto prevIt = g_cpuMetrics.find(cpuId);
        if (doFreqRead) {
            m.curFreq = readInt((base + "/cpufreq/scaling_cur_freq").c_str(), 0);
            m.maxFreq = readInt((base + "/cpufreq/scaling_max_freq").c_str(), 1);
            // v88 CPU-09: 频率读取失败时输出WARN日志(核心online时), 不再静默返回0
            // 问题: 读取失败静默返回0, 无法区分"核心真0频率"和"读取失败"。
            // 修复: curFreq==0且核心online时输出WARN; 离线核心跳过不记录。
            if (m.curFreq == 0) {
                std::string onlinePath = base + "/online";
                int onlineVal = readInt(onlinePath.c_str(), 1);  // cpu0通常无online文件, 默认1
                if (onlineVal != 0) {
                    LOG_WARN("readCpuMetrics: CPU%d cur_freq read failed (returned 0, core online=%d)", cpuId, onlineVal);
                }
            }
            m.capacity = readInt((base + "/cpu_capacity").c_str(), topology::kDefaultCapacity);
        } else {
            // 门控: 复用上轮值(首次上轮为空则用默认)
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
        // Use real utilization from /proc/stat, fallback to freqPct
        auto uit = cpuUtil.find(cpuId);
        m.util = (uit != cpuUtil.end()) ? uit->second : m.freqPct;
        m.temp = g_cachedCpuTemp;
        g_cpuMetrics[cpuId] = m;
        recordCpuLoadSample(cpuId, m.util);  // fix84: record for predictive scheduling
        // fix84: Record prediction error for confidence calibration
        {
            float predicted = predictCpuLoad(cpuId);
            recordPredictionError(cpuId, predicted, (float)m.util);
        }
        // P1a: on-disk state writes throttled to 1s; in-memory g_cpuMetrics above
        // is always fresh. mkdir once only (P1c).
        if (doMetricsWrite) {
            std::string cpuStateDir = std::string(STATE_DIR) + "/cpu" + std::to_string(cpuId);
            if (!s_cpuStateDirsReady) mkdirIfNotExist(cpuStateDir.c_str(), 0700); // v94-fix#13
            writeFile((cpuStateDir + "/cur_freq").c_str(), m.curFreq);
            writeFile((cpuStateDir + "/freq_pct").c_str(), m.freqPct);
            writeFile((cpuStateDir + "/util").c_str(), m.util);
            writeFile((cpuStateDir + "/temp").c_str(), m.temp);
        }
    }
    s_cpuStateDirsReady = true;
}

// fix35: forward declaration - defined later in scanThreads section
bool isKernelThread(const std::string& tname, int tid);

// v69: CPU usage tracking (replaces normalized loadavg as primary load metric)
// /proc/stat first line aggregates all CPUs: user nice system idle iowait irq softirq steal guest guest_nice
// We compute delta busy time / delta total time between two reads.
static unsigned long long g_prevCpuBusy = 0;
static unsigned long long g_prevCpuTotal = 0;
static bool g_prevCpuValid = false;
static std::atomic<int> g_lastCpuUsagePercent{0};

// Read /proc/stat first line and return (busy, total) jiffies.
// Returns false on parse failure.
//
// v94-P7-fix: readCpuMetrics() already opens /proc/stat once per loop and
// captures the aggregate "cpu " line into g_aggProcStatBusy/g_aggProcStatTotal
// (see above). This function now hands out that cached pair instead of doing a
// SECOND open+fclose("/proc/stat") every loop. The incremental delta logic in
// getCpuUsagePercent() is unchanged — it only stops paying a redundant syscall.
// readCpuMetrics() runs before checkSystemLoad() every loop, so the cache is
// always populated when this is called.
static bool readProcStatCpu(unsigned long long& busyOut, unsigned long long& totalOut) {
    if (!g_aggProcStatValid) return false;
    busyOut  = g_aggProcStatBusy;
    totalOut = g_aggProcStatTotal;
    return true;
}

// Compute incremental CPU usage percent since last call.
// First call primes the baseline and returns 0.
// Returns 0-100 integer percent.
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

// SAFE GUARD
// ============================================================================
bool checkSystemLoad() {
    Config cfg; snapshotConfig(cfg);
    // v69: Primary metric = CPU usage (from /proc/stat delta), NOT normalized loadavg.
    // loadavg includes D-state (uninterruptible sleep) processes — Android has many
    // binder/IO/kernel threads in D state that inflate loadavg without actual CPU pressure.
    // CPU usage > cpuUsageLimit triggers DEGRADED. loadavg is kept as secondary reference
    // and written to state files for diagnostics.

    // 1. Compute CPU usage (incremental since last call)
    int cpuUsage = getCpuUsagePercent();

    // 2. Read loadavg as secondary reference (kept for diagnostics, not primary trigger)
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

    // 3. Write state files
    //    system_load: now stores CPU usage percent (primary metric WebUI should display)
    //    cpu_usage: also written explicitly for clarity
    //    loadavg_raw: raw 1-min loadavg for diagnostics
    //
    // v94-P5-fix: these three files were written EVERY loop (every 50-200ms),
    // i.e. 3 write syscalls/loop. They are WebUI/diagnostics display files that
    // nothing on the hot path reads sub-second. The RETURN value of this function
    // (highLoad bool) drives the degraded logic, NOT the files. Gate the writes to
    // 1s. The in-memory loadInt/cpuUsage are still computed fresh every loop.
    {
        static time_t s_lastLoadStateWrite = 0;
        time_t nowLsw = time(nullptr);
        if (nowLsw - s_lastLoadStateWrite >= 1) {
            writeFile((std::string(STATE_DIR) + "/system_load").c_str(), cpuUsage);
            writeFile((std::string(STATE_DIR) + "/cpu_usage").c_str(), cpuUsage);
            // expose daemon self CPU for diagnostic scripts.
            writeFile((std::string(STATE_DIR) + "/self_cpu_usage").c_str(),
                      g_selfCpuUsagePercent.load(std::memory_order_relaxed));
            // expose low-overhead mode flag for diagnostics.
            writeFile((std::string(STATE_DIR) + "/low_overhead_mode").c_str(),
                      g_lowOverheadMode.load(std::memory_order_relaxed) ? 1 : 0);
            char loadBuf[64];
            snprintf(loadBuf, sizeof(loadBuf), "%.2f", load1);
            writeFile((std::string(STATE_DIR) + "/loadavg_raw").c_str(), loadBuf);
            // ROUND-014 P3: tmpfs budget state key (kb).
            // Estimate tmpfs usage of /dev/aatpe (STATE_DIR parent).
            // Use statvfs to get total/free blocks and compute used.
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

    // 4. Trigger DEGRADED on CPU usage threshold (primary)
    //    Secondary: also trigger if loadavg is extremely high (safetyLoadLimit) as a
    //    backstop, but this requires BOTH conditions to avoid D-state false positives.
    bool cpuHigh = (cpuUsage >= cfg.cpuUsageLimit);
    bool loadExtreme = (loadInt >= cfg.safetyLoadLimit);
    bool backstop = loadExtreme && cpuUsage >= 80;
    // v96-fix-BUG2: if the user manually picked NORMAL ("正常"), do NOT auto
    // downgrade to DEGRADED on high CPU. Otherwise a sustained-load workload
    // (gaming) gets yanked back to battery-saver within 50-200ms and the manual
    // selection looks broken. We still return false (highLoad) so the loop lowers
    // the migration cap conservatively -- only the state transition is skipped.
    bool userLockedNormal = g_userModeLock.load(std::memory_order_relaxed);

    // v4.1.2-hotfix: stop NORMAL<->DEGRADED flapping on *expected foreground* heavy
    // load. On the user's box (D1100 game load, jank=0, PSI ~16%) cpuUsage sat at
    // 89-99% and the old code degraded on the very first over-threshold loop then
    // recovered after 3 clean cycles (~5s), ping-ponging within seconds. A high-load
    // DEGRADED transition now requires EITHER an abnormal stability signal (active
    // jank) OR a long sustained window; recovery also needs a much longer dwell.
    // Real instability paths (watchdog crash-loop, thermal emergency, memory
    // pressure) live OUTSIDE checkSystemLoad and are deliberately NOT relaxed.
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
    constexpr long long kHighLoadDwellMs = 10000;  // plain high load must persist 10s
    bool jankAbnormal = (readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0) != 0);
    bool allowDegrade = watchdog::highLoadShouldDegrade(jankAbnormal, dwellMs, kHighLoadDwellMs);

    // Primary trigger: CPU usage. With the dwell/gating above, a brief over-threshold
    // spike no longer degrades; only sustained load (10s) or active jank does.
    if (cpuHigh) {
        // v94-fix (卡顿修复): 高负载日志节流 — 最多每 5 秒打一次，避免每 50ms 一次 LOG_WARN
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
            // high_cpu_usage must NOT downgrade EMERGENCY. EMERGENCY
            // is the highest-priority thermal-safety state and is exited ONLY via
            // the documented thermal recovery path (thermal.cpp: EMERGENCY->
            // DEGRADED after temp<wall-hysteresis for 30s). Letting high_cpu_usage
            // yank EMERGENCY->DEGRADED is exactly what made the two monitors
            // ping-pong ~1/s and pin the daemon at ~85% CPU. We still return
            // false (highLoad) below so migration throttling stays active.
            if (getSystemState() != SystemState::EMERGENCY) {
                transitionToState(SystemState::DEGRADED, "high_cpu_usage");
            }
            g_degradedNormalCycles.store(0, std::memory_order_relaxed);
        }
        return false;
    }
    // Secondary backstop: only if BOTH loadavg extreme AND cpu_usage >= 80%.
    // was cpuUsage >= (cpuUsageLimit - 15) = 70% with default config,
    // which fired on I/O-wait-inflated loadavg at only 71% CPU. Now requires
    // genuine CPU pressure (80%+) in addition to extreme loadavg, AND the same
    // dwell/jank gating as the primary trigger.
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
            // same EMERGENCY-stickiness as the primary trigger above.
            if (getSystemState() != SystemState::EMERGENCY) {
                transitionToState(SystemState::DEGRADED, "high_load_backstop");
            }
            g_degradedNormalCycles.store(0, std::memory_order_relaxed);
        }
        return false;
    }

    // Load normal — count consecutive cycles for DEGRADED->NORMAL
    SystemState curState = getSystemState();
    if (curState == SystemState::DEGRADED) {
        int cycles = g_degradedNormalCycles.fetch_add(1, std::memory_order_relaxed) + 1;
        // v4.1.2-hotfix: require >=15 seconds dwell in DEGRADED before recovering
        // to NORMAL (was 5s). Longer hysteresis prevents the 5s NORMAL->DEGRADED
        // bounce when load dips just under threshold.
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

// fix: use sched_setaffinity instead of writing read-only affinity file
// ============================================================================
// APPLY PLACEMENT (fix: log sched_setaffinity failures)
// ============================================================================
// [split] control functions moved to aatped_control.cpp

// G6: resolve Android uid for a pid via /proc/<pid>/status "Uid:" line.
// Returns -1 if unavailable (early boot, restricted procfs); the caller then
// falls back to per-tid bucketing. Called only on TTL-throttled rescores, so
// the /proc read cost is negligible.
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

// ROUND-004: HTTP server live start/stop. Registered as a config reload hook so
// ENABLE_HTTP_SERVER flips take effect without a daemon restart.
static std::atomic<bool> g_httpServerRunning{false};
// W-1: HTTP channel observability. The WebUI top bar must distinguish
// "intentionally closed by config" from "startup genuinely broke", otherwise a
// deliberate ENABLE_HTTP_SERVER=0 renders as an alarming red fault. This writes
// a READ-ONLY state string to STATE_DIR/http_state; it changes no bind, auth or
// security semantics. Closed set of values:
//   on                 — listener bound, port lives in STATE_DIR/http_port
//   disabled_by_config — operator asked for no HTTP surface (ENABLE_HTTP_SERVER=0)
//   failed_token       — wanted up, but CSPRNG daemon-token mint failed (fail-closed)
//   ports_busy         — wanted up, but 127.0.0.1:8888-8899 all in use
static void writeHttpChannelState(const char* state) {
    atomicWriteFile((std::string(STATE_DIR) + "/http_state").c_str(), std::string(state));
}

// ROUND-004: non-static so the v94 main test can drive ENABLE_HTTP_SERVER flips.
void applyHttpServerState(bool wantStart) {
    if (wantStart) {
        if (g_httpServerRunning) { writeHttpChannelState("on"); return; }
        if (aatpe::webui_http::startHttpServer()) {
            g_httpServerRunning = true;
            LOG_INFO("v66-fix: HTTP server started on port %d", aatpe::webui_http::getHttpPort());
            // ROUND-005c: atomic write — WebUI reads this to know which port to connect.
            atomicWriteFile((std::string(STATE_DIR) + "/http_port").c_str(), aatpe::webui_http::getHttpPort());
            writeHttpChannelState("on");
        } else {
            // W-1: classify the failure WITHOUT touching webui_http.cpp. startHttpServer()
            // only fails for two reasons: CSPRNG token mint fail-closed, or every port in
            // 8888-8899 already bound. Probe 127.0.0.1:8888 (the first port the loop tries):
            // EADDRINUSE means ports_busy; a free port means the bind loop was never reached,
            // i.e. the token fail-closed path above. Probe socket is bound then closed at once.
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

// P0-crash-loop hardening (v4.2.3): per-stage startup isolation. Each independent
// init stage calls this when it throws; the daemon records the stage + what() and
// continues to the NEXT stage instead of aborting the whole startup. HTTP/control
// is forced up last (in main) regardless.
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

// included as the highest pool). ROUND-007d: the pool SLOT count must cover the
// highest tier INDEX present, not merely the cluster count. On D1100 the clusters
// are LITTLE/MID/PRIME (3 clusters) but the tier indices are 0/1/3, so the model
// needs slot index 3 -> 4 slots. Return maxTier+1 (clamped), so a PRIME cpu is a
// scorable target instead of pool=-1 (which scored 0 and was dropped from the
// per-CPU weights table).
static inline int mlPoolCount() {
    int maxTier = 0;
    bool any = false;
    // B-002 (P2 residue): lock-protected snapshot. This helper runs OUTSIDE
    // g_topologyRwMutex; a concurrent detectCpuTopology() rewrite must not
    // reallocate g_clusters mid-iteration. clustersSnapshot() copies under the
    // topology lock then releases it before we read the copy.
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

// v4.1.2-hotfix: NUMBER OF DISTINCT PHYSICAL CLUSTERS (compressed count), NOT
// maxTier+1. On sparse-tier machines (D1100: LITTLE=0/BIG=2/PRIME=3) maxTier+1=4
// but there are only 3 real clusters. The model still needs maxTier+1 SLOTS
// (mlPoolCount above, so PRIME tier=3 has a scorable slot), but the FEATURE
// normalizer poolFeatureForTier() must branch on the real cluster count:
//   <=3 clusters -> historical 0.3/0.6/1.0 (v4.1.0 byte-for-byte);
//   4 clusters   -> linear normalisation (the F3 PRIME fix).
// Passing maxTier+1 here is what made a 3-cluster box silently take the linear
// branch and change hf[8] vs v4.1.0.
static inline int mlDistinctClusterCount() {
    // B-002 (P2 residue): lock-protected snapshot (helper runs outside
    // g_topologyRwMutex). size() on the returned copy is safe even if a topology
    // write reallocates g_clusters concurrently.
    int n = (int)clustersSnapshot().size();
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

void daemonInit() {
    LOG_FORCE("daemonInit: entering initialization sequence");
    // ROUND-010: harden daemon against phantom process killer and lmkd.
    // setsid() detaches from the controlling terminal (prevents phantom
    // process kill when the parent shell dies). oom_score_adj=-1000 makes
    // the daemon the last process lmkd will kill under memory pressure.
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
    // ROUND-010: probe device capabilities (cgroup/uclamp/PSI/loader).
    // probe() is read-only (sysfs/proc walk) and may run before the runtime
    // dirs exist. writeStateFiles() MUST NOT run here: on tmpfs /dev/aatpe
    // the state dir does not exist yet below, so the atomic write silently
    // failed on every boot and the whole capabilities surface (uclamp knobs)
    // never hit disk. It is re-issued after the mkdir block further down.
    try {
        aatpe::caps::probe();
    } catch (const std::exception& e) { recordStartupCrash("caps_probe", e.what()); }
    catch (...) { recordStartupCrash("caps_probe", "?"); }
    // P0-opt2: pre-reserve g_threads bucket count to avoid rehashing during
    // the first scanThreads() burst (~200-500 threads on a typical game session).
    g_threads.reserve(1024);
    // v94-fix#13: runtime dirs hold pid/tid/pname/tname/role/priority/CPU and
    // placement info (and RUN_DIR also holds the daemon_token + control socket).
    // They are root-owned and read only by root (daemon, WebUI SSE, aatpe-cli),
    // so tighten to 0700. WEBUI_DIR is the static webroot (served by the in-
    // process HTTP server which reads files as root), so it stays 0755.
    mkdirIfNotExist(RUN_DIR, 0700);
    mkdirIfNotExist(STATE_DIR, 0700);
    mkdirIfNotExist(METRICS_DIR, 0700);
    mkdirIfNotExist(DECISION_DIR, 0700);
    mkdirIfNotExist(WEBUI_DIR, 0755);
    mkdirIfNotExist((std::string(RUN_DIR) + "/history").c_str(), 0700);
    // ML-10: before any new per-tid history file is written this boot, drop files
    // left by a previous daemon instance (tids are recycled; a stale foreign
    // placement_success_rate must not leak into this boot's smoothing).
    scoringHistorySweepStale();
    mkdirIfNotExist((std::string(RUN_DIR) + "/thread_db").c_str(), 0700);
    // Dir tree exists now — publish the packed capabilities line AND the
    // per-knob state files (task_uclamp / cgroup_uclamp_v1 / ...) that the
    // WebUI "机制与调试" panel aggregates by filename. Running this BEFORE the
    // mkdirs above was a silent no-op on tmpfs and left every uclamp row "—".
    try { aatpe::caps::writeStateFiles(STATE_DIR); }
    catch (const std::exception& e) { recordStartupCrash("caps_write", e.what()); }
    catch (...) { recordStartupCrash("caps_write", "?"); }
    // #74: audit helper directory for sensitive content.
    helperDirAudit();

    // round-5: register all long-lived subsystems in the unified registry.
    // They start in STARTING; the per-subsystem bring-up below flips them to
    // ACTIVE (or DEGRADED/FAILED on error). WebUI /state/all and
    // <STATE_DIR>/subsystem_health read from this registry.
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
    // v95-OPT1: 创建主循环事件驱动 self-pipe。必须在 eBPF hook 安装之前完成,
    // 因为关键事件 hook (line ~1180) 会调用 wakeMainLoopNow() 写该 pipe。
    {
        int pipeFds[2];
        if (pipe(pipeFds) == 0) {
            g_wakeupPipeReadFd = pipeFds[0];
            g_wakeupPipeWriteFd = pipeFds[1];
            // 读端非阻塞: 主循环 poll 唤醒后一次性 drain, 不会阻塞在 read。
            int fl = fcntl(g_wakeupPipeReadFd, F_GETFL, 0);
            if (fl >= 0) fcntl(g_wakeupPipeReadFd, F_SETFL, fl | O_NONBLOCK);
            // 写端非阻塞: 即使主循环没及时 drain, 写端也不会阻塞 poll 线程。
            fl = fcntl(g_wakeupPipeWriteFd, F_GETFL, 0);
            if (fl >= 0) fcntl(g_wakeupPipeWriteFd, F_SETFL, fl | O_NONBLOCK);
            // close-on-exec 防止 fork 子进程泄漏 fd。
            fcntl(g_wakeupPipeReadFd, F_SETFD, FD_CLOEXEC);
            fcntl(g_wakeupPipeWriteFd, F_SETFD, FD_CLOEXEC);
            LOG_FORCE("[OPT1] wakeup self-pipe created (read=%d write=%d)",
                      g_wakeupPipeReadFd, g_wakeupPipeWriteFd);
        } else {
            LOG_WARN("[OPT1] wakeup self-pipe FAILED: %s (event-driven fallback to timed poll)",
                     aatpeErrStr(errno));
        }
        // v95-OPT1: inotify 监视 control 文件, WebUI/CLI 写命令时立即唤醒主循环。
        // /control 由 WebUI HTTP 层 O_APPEND 写入; IN_MODIFY 触发即唤醒。
        // P1-2: watch the RUN DIRECTORY instead of the file itself. If the control
        // file is ever unlink()+recreated or rename()d onto (e.g. atomic-replace
        // handoff), a file-level watch follows the old inode and goes permanently
        // dead. A directory-level watch survives file replacement; we filter by
        // filename in the event-drain path.
        g_controlInotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_controlInotifyFd >= 0) {
            std::string ctrlPath = std::string(RUN_DIR) + "/control";
            // 先确保文件存在(避免 watch 一个不存在的路径)
            // P0-crash-fix: O_TRUNC instead of O_APPEND — clear any stale commands
            // from previous run (e.g. a leftover "stop" that would make g_shouldExit=1
            // on the first processControlCommands() call, causing instant daemon exit).
            // v6.0.0 S-02: O_NOFOLLOW|O_CLOEXEC — refuse a trailing symlink at the
            // control path and keep the fd out of posix_spawn children.
            int cf = open(ctrlPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (cf >= 0) close(cf);
            // Watch the directory, not the file. IN_CLOSE_WRITE catches writers
            // that open/write/close; IN_MODIFY catches O_APPEND writers.
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
    // SEC-05: align the PID-identity reader's state dir with the daemon's
    // global STATE_DIR BEFORE we publish daemon_starttime / daemon_exe below.
    // aatpe_pid_identity.h is self-contained (dropped into standalone
    // aatpe-cli / aatpe-watchdog TUs that do not link STATE_DIR) and defaults
    // to "/dev/aatpe/state"; this one call makes the reader follow STATE_DIR if
    // it ever moves, so writer and reader never drift apart.
    aatpe::pid_identity::setStateDir(STATE_DIR);
    // P1-2: publish our own starttime (clock ticks from /proc/self/stat field 21)
    // BEFORE writing daemon.pid, so any observer (watchdog / CLI / --stop) that
    // sees daemon.pid can immediately cross-check /proc/<pid>/stat starttime
    // against this record. This closes the PID-reuse window: a recycled PID
    // will have a different starttime and fail verifyAatpedPid().
    {
        long long selfSt = aatpe::pid_identity::readPidStarttime(getpid());
        if (selfSt >= 0) {
            // A1-004: atomic write (tmp+fsync+rename) to match daemon_pid /
            // daemon_start. A concurrent watchdog/CLI verifyAatpedPid() reader
            // must never observe a torn partial starttime string that would fall
            // back to bootstrap (exe-only) verification and transiently weaken
            // the PID triple-gate.
            atomicWriteFile((std::string(STATE_DIR) + "/daemon_starttime").c_str(), selfSt);
        }
    }
    // h6 D2 identity: symmetric to starttime, publish our own /proc/self/exe once
    // (this runs AFTER lockf success, so a ghost duplicate --daemon copy that lost
    // the lock _exit(0)'d before reaching here and never writes it). The watchdog
    // compares the live daemon's exe against this record instead of assuming it
    // lives at /system/bin/aatped — on a KernelSU/Magisk bind-mount device the real
    // exe is /data/adb/modules/aatpe/system/bin/aatped, and realpath(/system/bin)
    // does NOT resolve through the magic mount, so the old set-based check falsely
    // classified the live daemon as dead and spawned ghost copies forever.
    {
        char selfExe[PATH_MAX] = {0};
        ssize_t n = readlink("/proc/self/exe", selfExe, sizeof(selfExe) - 1);
        if (n > 0) {
            // A1-004: atomic write. Same rationale as daemon_starttime: a
            // torn exe record could momentarily make the watchdog see a
            // mismatched exe and reject the live daemon.
            atomicWriteFile((std::string(STATE_DIR) + "/daemon_exe").c_str(),
                            std::string(selfExe, n));
        }
    }
    writeFile(PID_FILE, getpid());
    // fix22: also write PID to STATE_DIR/daemon_pid for webui/index.html
    // (which reads state/daemon_pid, not RUN_DIR/daemon.pid)
    // ROUND-005c: atomic write — watchdog takeover reads this, a torn PID=0 would
    // trigger a false crash-restart loop.
    atomicWriteFile((std::string(STATE_DIR) + "/daemon_pid").c_str(), getpid());
    g_startTime.store(time(nullptr));
    // LED-012 (R1-012): time_t is 64-bit on ARM64; casting to int wrapped at 2038.
    // Use the long long overload so daemon_start round-trips the full timestamp.
    // ROUND-005c: atomic write for the same reason as daemon_pid.
    atomicWriteFile((std::string(STATE_DIR) + "/daemon_start").c_str(), (long long)g_startTime.load());
    // fix22: write initial uptime=0 so file exists even if first loop hits continue
    writeFile((std::string(STATE_DIR) + "/daemon_uptime").c_str(), 0);
    // fix24: initialize counters so WebUI shows 0 instead of "--" even when
    // checkSystemLoad() returns false and the loop hits `continue` on first cycle.
    writeFile((std::string(STATE_DIR) + "/decision_count").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/migration_count").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/decision_count_current").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/migration_rate_current").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/loop_count").c_str(), 0);
    // ROUND-007d: canonical counter files written at startup so WebUI shows 0
    // immediately (not "--" until first decision cycle / first migration).
    atomicWriteFile((std::string(STATE_DIR) + "/total_decisions").c_str(), (long long)0);
    atomicWriteFile((std::string(STATE_DIR) + "/total_migrations").c_str(), (long long)0);
    // WebUI "主动迁移次数" reads prediction_proactive_migrations (not total_migrations).
    // placement.cpp only writes it when migrated>0; seed it at 0 for fresh boot.
    atomicWriteFile((std::string(STATE_DIR) + "/prediction_proactive_migrations").c_str(), (long long)0);
    // ROUND-009: seed diagnostics files at startup so WebUI cards show immediately.
    try {
        aatpe::diag::collect();
        aatpe::diag::writeStateFiles(STATE_DIR);
    } catch (const std::exception& e) { recordStartupCrash("diag_collect", e.what()); }
    catch (...) { recordStartupCrash("diag_collect", "?"); }
    // ROUND-007: write version at startup so WebUI can show it immediately.
    atomicWriteFile((std::string(STATE_DIR) + "/version").c_str(),
                    std::string(AATPE_VERSION) + " (" + std::to_string(AATPE_VERSION_CODE) + ")");
    // fix29: initialize diagnostic counters so files exist from startup
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
    // fix40: APK detection state files (initialized to defaults, updated after detection)
    writeFile((std::string(STATE_DIR) + "/helper_apk_installed").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/auto_profile_mode").c_str(), "module_controlled");
    // fix46: foreground app detection source (apk / module / module_fallback)
    writeFile((std::string(STATE_DIR) + "/foreground_app_source").c_str(), "module");
    try {
        loadConfig();
        // round-3 (P0-2): rebuild in-memory recovery tracking from the on-disk
        // recovery/<tid>.rec files BEFORE the main loop starts scanning/mutating.
        aatpe_recovery::recoveryBootload();
        // ROUND-014 P3: boot-time sched restore.
        aatpe_recovery::recoveryRestoreSchedOnBoot();
    } catch (const std::exception& e) { recordStartupCrash("config_recovery", e.what()); }
    catch (...) { recordStartupCrash("config_recovery", "?"); }
    // fix101 P2: Initialize HMAC key (load or generate random)
    initHmacKey();
    // G3-001: wire integrity fail-hook -> runtime capability revocation.
    // When a (re)verify detects tampering, the daemon immediately rejects all
    // privileged /control writes (hmacForceReadonly) instead of only logging.
    aatpe::integrity::setIntegrityFailHook([](const char* reason) {
        hmacForceReadonly(reason);
    });
    // ROUND-003: symmetric release when integrity recovers to OK.
    aatpe::integrity::setIntegrityRestoreHook([](const char* reason) {
        hmacClearReadonly(reason);
    });
    // G3-001 boot gate + token materialization isolated: a boot-gate exception must
    // NOT skip later init (it stays fail-closed/read-only on its own).
    try {
        (void)aatpe::integrity::runBootGate();
        aatpe::webui_http::ensureDaemonToken();
    } catch (const std::exception& e) { recordStartupCrash("integrity_token", e.what()); }
    catch (...) { recordStartupCrash("integrity_token", "?"); }
    // fix72 P2-1: Write config-dependent state files AFTER loadConfig() (previously written with defaults before config load)
    // Phase3 Batch1: initialize unified state machine based on config controlMode
    // P1-7: use setStateEnterTimeNow() so both wall-clock and monotonic
    // enter timestamps are initialized together (was writing only wall clock,
    // leaving s_stateEnterSteadyMs=0 until the first transition).
    setStateEnterTimeNow();
    // HARDCORE: force default NORMAL/assist regardless of aatpe.conf
    g_systemState.store((int)SystemState::NORMAL, std::memory_order_release);
    strncpy(g_lastTransitionReason, "hardcore_default_normal", sizeof(g_lastTransitionReason) - 1);
    g_lastTransitionReason[sizeof(g_lastTransitionReason) - 1] = '\0';
    // v94 改进包B 任务3: key state-machine / durable-control files written
    // atomically (state.json readers + watchdog must never see a torn value).
    atomicWriteFile((std::string(STATE_DIR) + "/system_state").c_str(),
              systemStateToString(getSystemState()));
    atomicWriteFile((std::string(STATE_DIR) + "/last_transition_reason").c_str(),
              std::string(getLastTransitionReason()));
    atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(), g_cfg.controlMode);
    writeFile((std::string(STATE_DIR) + "/log_level").c_str(), g_logLevel.load());
    atomicWriteFile((std::string(STATE_DIR) + "/current_profile").c_str(), aatpe_profile::effectiveProfile());
    atomicWriteFile((std::string(STATE_DIR) + "/profile_lock").c_str(), g_profileLock ? 1 : 0);
    // v66: Battery aging protection initial state (default enabled from config)
    writeFile((std::string(STATE_DIR) + "/battery_aging_enabled").c_str(), g_cfg.batteryAgingProtection ? "1" : "0");
    atomicWriteFile((std::string(STATE_DIR) + "/battery_aged").c_str(), g_batteryAged ? "1" : "0");
    // v66: ML cold start protection initial state + model threshold sync
    writeFile((std::string(STATE_DIR) + "/ml_cold_start_enabled").c_str(), g_cfg.mlColdStartProtection ? "1" : "0");
    // v88 W2: ML学习开关初始状态 (之前漏写, 导致WebUI启动时显示--)
    writeFile((std::string(STATE_DIR) + "/ml_learning_enabled").c_str(), g_cfg.mlLearningEnabled ? "1" : "0");
    // v94-webui-fix: publish thermal limit thresholds from g_cfg to STATE_DIR at
    // startup. Previously thermal_limit_cpu/skin/battery were ONLY written when the
    // user changed them via set_thermal_limit_* control commands, so on a fresh
    // boot the files did not exist -> /state/all omitted the keys -> WebUI fell
    // back to its hardcoded 120 default. Now they always reflect aatpe.conf.
    writeFile((std::string(STATE_DIR) + "/thermal_limit_cpu").c_str(), g_cfg.thermalLimitCpu);
    writeFile((std::string(STATE_DIR) + "/thermal_limit_skin").c_str(), g_cfg.thermalLimitSkin);
    writeFile((std::string(STATE_DIR) + "/thermal_limit_battery").c_str(), g_cfg.thermalLimitBattery);
    // ROUND-005: publish thermal_enabled at startup (was only written on toggle,
    // leaving WebUI showing "--" on fresh boot). Also publish control_mode_effective
    // so WebUI can distinguish user mode from system-enforced mode immediately.
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
    // fix40: APK smart detection — if Helper APK installed, force auto_profile off
    applyHelperApkDetection();
    // fix55 (问题46): OEM capability discovery
    {
        bool cgroupV1 = access("/sys/fs/cgroup/cpuset", F_OK) == 0 || access("/dev/cpuset", F_OK) == 0;
        bool cgroupV2 = access("/sys/fs/cgroup/cpuset.cpus.effective", F_OK) == 0;
        LOG_INFO("Capability: cgroup_v1=%d cgroup_v2=%d", cgroupV1, cgroupV2);
    }
    try {
        detectCpuTopology();
    } catch (const std::exception& e) { recordStartupCrash("cpu_topology", e.what()); }
    catch (...) { recordStartupCrash("cpu_topology", "?"); }
    // P1-fix (issue 23): detectCpuTopology() returns void but sets g_cpuCount.
    // If topology detection failed (no CPUs discovered, all pool vectors empty),
    // every subsequent placement decision runs against an empty pool and the
    // daemon silently does nothing. Emit a prominent error so ops can see it.
    if (g_cpuCount <= 0 || g_allCpus.empty()) {
        LOG_ERROR("daemonInit: CPU topology detection FAILED (g_cpuCount=%d, "
                  "g_allCpus.size=%zu). Placement will be inert until restarted on "
                  "a supported device.", g_cpuCount, g_allCpus.size());
    }
    // v73: Initialize eBPF event-driven scheduler
    {
        // Configure from config
        g_ebpfScheduler.setRingPages(g_cfg.ebpfRingPages);
        g_ebpfScheduler.setPollFallbackEnabled(g_cfg.ebpfPollFallback);
        g_ebpfScheduler.setEventConsumer(&g_eventConsumer);
        // ROUND-004: register the eBPF config hot-reload hook so EBPF_ENABLED /
        // EBPF_RING_PAGES / EBPF_POLL_FALLBACK converge at runtime (was BOOT-only).
        g_ebpfScheduler.registerConfigReloadHook();
        // v95 issues 8/9: lifecycle events (FORK/EXEC/EXIT) arriving on the eBPF
        // poll thread must wake the daemon main loop out of its poll() sleep.
        // v95-OPT1: 除了保留 g_skipNextSleep(兼容旧路径), 现在直接写 self-pipe
        // 唤醒主循环 poll(), 实现真正的亚毫秒级唤醒(旧 sleep_for 无法被原子打断,
        // 最坏要等满一个 interval 才看到 flag)。SWITCH events 仍不触发(太频繁)。
        // CPU-BUG1: coalesce wake-ups. Android forks/execs dozens of
        // threads/sec; without a minimum gap each lifecycle event woke the main
        // loop -> forceRefresh foreground I/O -> poll(10ms) -> woken again,
        // pinning a core at 127%. Lifecycle events already queue in criticalQueue_
        // and are drained on the next loop iteration, so dropping redundant
        // wake-ups inside a 30ms window loses nothing and caps wake rate at ~33/s.
        //
// CPU-FIX2 (root cause of "waking for immediate full decision"
        // storm): the fixed 30ms window still yields ~33 wakeups/sec under a
        // live app storm. Each wake sets g_skipNextSleep -> pollTimeoutMs pinned
        // to kMinSleepMs(20ms) -> a full decision cycle (readCpuMetrics,
        // precompute, poll-based freq/temp scan, state JSON) runs on every wake.
        // On a thermal-throttled big core (659MHz, CPU=97%) one cycle takes
        // ~27ms, so 33 cycles/s ~= 89% main-thread CPU. The log itself was
        // already throttled to 1/s (line ~3051), which masked the true wake rate.
        //
        // Fix: load-aware coalescing. Under high load (CPU>=80%) or in
        // DEGRADED/EMERGENCY, stretch the coalescing window to 1000ms so the
        // main loop is woken at most once per second. Lifecycle events still
        // queue in criticalQueue_ and are consumed by the eBPF dispatch thread
        // (onFork/onExec/onExit update g_threads directly); the only thing
        // deferred is the placement-decision re-scan, which the 1s poll-timeout
        // backstop already covers.
        g_ebpfScheduler.setCriticalEventWakeHook([]() {
            static std::atomic<long long> s_lastWakeMs{0};
            long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            long long prev = s_lastWakeMs.load(std::memory_order_relaxed);

            // CPU-FIX2: pick coalescing window based on current load.
            // g_lastCpuUsagePercent is a plain int cached by the main loop's
            // readCpuMetrics(); a stale read here costs at most one extra wake.
            //
            // ALSO gate on daemon self CPU usage. Root cause:
            // when the daemon itself burns 90% of one core, system CPU reads
            // only ~66% (other cores idle), so cpuPct<80 keeps the 30ms window
            // -> 33 wakes/sec -> each wake runs a full 27ms decision cycle on
            // a throttled core -> self-reinforcing CPU burn.  g_selfCpuUsagePercent
            // measures daemon's own CPU as % of one core; >=50% means the daemon
            // is already spending half a core on itself, so stretch the window.
            int cpuPct = g_lastCpuUsagePercent.load(std::memory_order_relaxed);
            int selfPct = g_selfCpuUsagePercent.load(std::memory_order_relaxed);
            SystemState sysSt = getSystemState();
            bool underLoad = (cpuPct >= 80)
                          || (selfPct >= 50)
                          || (sysSt == SystemState::DEGRADED)
                          || (sysSt == SystemState::EMERGENCY)
                          || g_lowOverheadMode.load(std::memory_order_relaxed);
            const long long kCoalesceMs = underLoad ? 1000LL : 30LL;

            // bugfix: in low-overhead mode (EMERGENCY / host overloaded), do
            // NOT wake the main loop on eBPF lifecycle events at all. The critical
            // events are still queued and drained by the eBPF poll thread
            // (onFork/onExec/onExit update g_threads directly); deferring the
            // placement re-scan is fine because regular placement is skipped anyway.
            // Waking here was the actual source of the "waking for immediate full
            // decision" spam and the state=R main-thread busy-spin.
            if (g_lowOverheadMode.load(std::memory_order_relaxed)) {
                s_lastWakeMs.store(nowMs, std::memory_order_relaxed);
                return;
            }
            if (prev != 0 && (nowMs - prev) < kCoalesceMs) {
                return;   // coalesce: a wake within the window is pending
            }
            s_lastWakeMs.store(nowMs, std::memory_order_relaxed);
            g_skipNextSleep.store(true, std::memory_order_release);
            wakeMainLoopNow();  // v95-OPT1: 真正打断主循环 poll()
        });

        // Pre-flight diagnostics
        bool havePrecompiled = false;
        // fix-v96 #8: module root via getModuleDir() (Magisk/KSU/APatch).
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
        // P2-22: fixed absolute path check instead of "command -v bpftool" (which
        // relies on PATH lookup through /bin/sh). Known bpftool locations on Android.
        bool haveBpftool = (access("/system/bin/bpftool", X_OK) == 0 ||
                            access("/vendor/bin/bpftool", X_OK) == 0 ||
                            access("/data/adb/ksu/bin/bpftool", X_OK) == 0);
        LOG_FORCE("[EBPF] initializing, enabled=%d", g_cfg.ebpfEnabled ? 1 : 0);
        LOG_FORCE("eBPF pre-flight: enabled=%d precompiled_o=%d bpftool=%d ring_pages=%d",
                 g_cfg.ebpfEnabled ? 1 : 0, havePrecompiled ? 1 : 0,
                 haveBpftool ? 1 : 0, g_cfg.ebpfRingPages);

        if (g_cfg.ebpfEnabled) {
            LOG_FORCE("eBPF: calling g_ebpfScheduler.load()...");
            // G2 (v4.4.9): load() performs the attach (kept for its side effect)
            // but its return value is intentionally not used here — attach success
            // vs. non-privileged POLLING fallback is determined below via
            // isEventDrivenLive(); the old `bool ebpfOk = load()` was a dead
            // assignment that tripped -Werror on the release build.
            g_ebpfScheduler.load();
            // G2 (v4.4.9): load() returns `true` even when it cannot attach and
            // lands in the non-privileged POLLING fallback (a handled outcome,
            // not a crash; the main loop keeps running on /proc polling). The
            // daemon's g_ebpfActive must reflect whether event-driven ring
            // monitoring is ACTUALLY live, not merely that load() did not crash,
            // so a BPF-unavailable device (NO_CAP/ENOSYS/EPERM/no rings) does
            // not report eBPF as ACTIVE in the state file. Healthy attach
            // (EVENT_DRIVEN/HYBRID) still publishes true; pure polling fallback
            // publishes false.
            bool ebpfLive = g_ebpfScheduler.isEventDrivenLive();
            g_ebpfActive.store(ebpfLive, std::memory_order_release);
            // P1-2 fix: snapshot the loader/mode strings (value returns) then
            // publish the globals under g_ebpfInfoMutex so writeStateJson() can
            // read them without racing a std::string realloc.
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
            // v94 fix / 问题43/44/45: publish the runtime tracepoint-offset
            // verification verdict. Value is "true (...)", "false (...)", or
            // "unknown (debugfs unavailable)". Read-only diagnostic; does not
            // gate the load (verifyTracepointOffsets() already degrades gracefully).
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_verified").c_str(),
                      g_ebpfScheduler.getOffsetVerifyResult());
            LOG_FORCE("eBPF offset verify: %s", g_ebpfScheduler.getOffsetVerifyResult().c_str());
            // v94-r3 fix: per-tracepoint offset status tokens. These are the
            // machine-readable counterparts to the human-readable string above:
            // each file holds one of "verified" / "degraded" / "unknown" /
            // "no_ctx". The WebUI surfaces them so the user can see which
            // lifecycle events (fork/exec/exit) are being filtered vs. which
            // are healthy. switch is load-bearing: "degraded" there means the
            // whole eBPF subsystem has been downgraded to polling.
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
    // fix84 P2-4: Write online_cpus at daemon init so state file always exists.
    // Previously only written in refreshCpuTopology() on topology change, causing "file not found".
    {
        std::string ocList;
        for (size_t i = 0; i < g_allCpus.size(); i++) {
            if (i > 0) ocList += ",";
            ocList += std::to_string(g_allCpus[i]);
        }
        writeFile((std::string(STATE_DIR) + "/online_cpus").c_str(), ocList);
    }
    // fix55 (问题3/4): Pass real daemon CPU->Pool topology to ML migration.
    // ML must use daemon's actual mapping, not its own default 4+2+2.
    {
        std::vector<int> cpuToPool(g_cpuCount, -1);
        for (int cpu : g_littlePool) if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 0;
        for (int cpu : g_midPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 1;
        for (int cpu : g_bigPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 2;
        // ROUND-007d: PRIME cluster cpus (e.g. D1100 cpu7) were left at -1, so the
        // ML model returned score 0 for them and the per-CPU weights table omitted
        // them. Map PRIME to tier slot 3 (mlPoolCount now covers it).
        for (int cpu : primePool())  if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 3;
        g_mlModel.setCpuPoolAssignment(cpuToPool);
        // fix55 (问题5/6): topology fingerprint.
        // ROUND-010 D4-06: fingerprint keeps ONLY structural invariants (cpuCount,
        // poolCount, per-cpu pool). Per-cpu capacity/maxFreq are TRANSIENT (boot
        // early read 0, later 380/real) and were included, so each refresh changed
        // fp -> setTopologyFingerprint flagged stale -> repeated cold starts that
        // prevented samples from accumulating. Capacity/freq remain ML features but
        // must NOT enter the fingerprint.
        std::string fp = "cpu=" + std::to_string(g_cpuCount) + ";pools=" + std::to_string(mlPoolCount());
        for (int i = 0; i < g_cpuCount; i++) {
            fp += ";cpu" + std::to_string(i) + "=" + std::to_string((int)getPoolForCpu(i));
        }
        (void)0; // capacity/maxFreq intentionally excluded from fingerprint (D4-06)
        g_mlModel.setTopologyFingerprint(fp);
        LOG_INFO("Topology: cpuCount=%d assignment synced to ML (LITTLE=%zu MID=%zu BIG=%zu) fp_len=%zu",
                 g_cpuCount, g_littlePool.size(), g_midPool.size(), g_bigPool.size(), fp.size());
    }
    loadThreadRules();
    loadProfile(g_currentProfile);
    // unified profile machine: seed the GLOBAL base profile vote and sync
    // the "applied" bookkeeping to whatever the boot loadProfile() just applied.
    // All later per-app / auto / thermal / manual / emergency requests now arbitrate
    // against this baseline.
    aatpe_profile::profileInitGlobal(g_currentProfile, g_currentProfile);

    // fix21: Initialize ML model (fix52: per-POOL logistic regression, 3 pools)
    {
        std::lock_guard<std::mutex> lock(g_mlMutex);
        g_mlModel.setEnabled(g_cfg.mlLearningEnabled != 0);
        g_mlModel.setMaxAlpha(g_cfg.mlMaxAlpha);
        g_mlModel.setInitialLR(g_cfg.mlInitialLR);
        g_mlModel.setDecay(g_cfg.mlDecay);
        g_mlModel.setWeightClamp(g_cfg.mlWeightClamp);
        g_mlModel.init(mlPoolCount());  // fix52: 3 pools (LITTLE/MID/BIG), not g_cpuCount
        // Load persisted model (cold start if file doesn't exist)
        // fix50: resolve real module path (Magisk / KernelSU) + backup fallback
        const char* mp = mlModelPath();
        bool loaded = loadModelWithBackup(mp);
        // #39 topology binding: a sidecar records the device topology hash the
        // model was trained on. If it does not match THIS device (different ROM /
        // different SoC / pool reshuffle), discard the loaded weights and cold
        // start — a model trained on another device must not pollute ours.
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
            // load() may have partially mutated the model -> clean cold start
            g_mlModel.init(mlPoolCount());
            struct stat st;
            int err = errno;
            LOG_WARN("ML model: COLD START - load failed path=%s errno=%d(%s) exists=%d size=%lld",
                     mp, err, aatpeErrStr(err),
                     (stat(mp, &st) == 0) ? 1 : 0,
                     (stat(mp, &st) == 0) ? (long long)st.st_size : -1LL);
            // round-4 (#23): isolate a CORRUPT model file (one that exists
            // but failed magic/version/CRC/topology checks) so it cannot block
            // every subsequent restart. A missing file is the normal cold-start
            // case and is left alone. We rename (not delete) for forensics.
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

    // v66: Initialize hybrid learning (experimental, default disabled)
    {
        std::lock_guard<std::mutex> hlock(g_hybridMutex);
        auto& hybrid = aatpe_ml_hybrid::instance();
        hybrid.init(mlPoolCount(), mlDistinctClusterCount());  // slots=maxTier+1, feature-normalizer=real cluster count
        // Try load persisted delta weights (best-effort)
        const char* mp = mlModelPath();
        std::string hybridPath = std::string(mp) + ".hybrid";
        hybrid.load(hybridPath.c_str());
        // v66 regression-fix #10: persisted enabled state takes priority over
        // config-file default. This ensures a runtime toggle survives restart.
        g_cfg.mlHybridEnabled = hybrid.getLoadedEnabled();
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_enabled").c_str(),
                  g_cfg.mlHybridEnabled ? "1" : "0");
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_updates").c_str(), 0);
        writeFile((std::string(STATE_DIR) + "/ml_hybrid_ebpf_samples").c_str(), 0);
        LOG_INFO("[ML-HYBRID] initialized: enabled=%d, path=%s",
                 g_cfg.mlHybridEnabled ? 1 : 0, hybridPath.c_str());
    }

    // fix: write safety_migration_limit to state so watchdog can read it
    writeFile((std::string(STATE_DIR) + "/safety_migration_limit").c_str(), g_cfg.safetyMigrationLimit);
    // D-02: Use sigaction() instead of signal() for portable, reliable signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
    // fix27: SIGCHLD with SA_NOCLDWAIT — kernel auto-reaps child processes.
    // Prevents zombie accumulation from aatpeExec children that couldn't be
    // reaped via WNOHANG (e.g., D-state dumpsys grandchildren). Without this,
    // thousands of zombies could exhaust the PID namespace over time.
    struct sigaction saChld;
    memset(&saChld, 0, sizeof(saChld));
    saChld.sa_handler = SIG_DFL;
    saChld.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&saChld.sa_mask);
    sigaction(SIGCHLD, &saChld, nullptr);
    // P1-19: Install crash signal handlers (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS)
    installCrashHandlers();
    setenv("PATH", "/system/bin:/system/xbin:/vendor/bin:/sbin", 1);
    setenv("ANDROID_ROOT", "/system", 0);
    aatpeProbeEnvironment();
    LOG_INFO("Daemon initialized. Mode=%s Profile=%s", g_cfg.controlMode.c_str(), aatpe_profile::effectiveProfile().c_str());
    // fix96: Start config hot-reload
    startConfigHotReload();

    // fix97 P3: Initialize federated learning
    g_federatedLearner.init("/dev/aatpe/federated");
    // G7: honor FEDERATED_ENABLED config — opt-in, default off.
    g_federatedLearner.setEnabled(g_cfg.federatedEnabled);
    LOG_INFO("fix97: Federated learning initialized, exchange dir=/dev/aatpe/federated");

    // Initialize frame-time outcome callbacks (reuse frame.cpp SF latency)
    initFrameTimeOutcomeCallbacks();

    // #28: start the inotify display-mode watcher for event-driven refresh-rate
    // refresh. The 10s poll remains as the backstop; this only makes mode
    // switches react faster. Best-effort: no-op if nodes are unwatchable.
    if (g_cfg.refreshRateEventDriven) {
        aatpe::frameaware::refreshRateEventStart();
    }

    // Initialize ADRC feedforward controllers (per-role TD/ESO/NLSEF).
    // Must run before placement/scoring first calls getRoleOutput().
    if (g_cfg.adrcEnabled) {
        aatpe::adrc::init();
    } else {
        LOG_INFO("ADRC: disabled by config (ADRC_ENABLED=0)");
    }

    // v4.3.6: initialise the PSI decision closed-loop (default thresholds + state).
    // Cheap; idempotent. The per-tick consumption lives in loopForegroundTick().
    aatpe::psi::init();
    // v4.3.7: initialise the multi-source arbitration layer (loads device_profile
    // from STATE_DIR, fail-safe to defaults on missing/corrupt). Idempotent.
    aatpe::arb::init();

{
        bool shouldStartHttp = g_cfg.enableHttpServer;
        bool magiskAuto = false;
        if (!shouldStartHttp) {
            struct stat st;
            bool hasKsu = (stat("/data/adb/ksu", &st) == 0) ||
                          (stat("/data/adb/ksud", &st) == 0);
            bool hasMagisk = (stat("/data/adb/magisk", &st) == 0);
            // LED-002 (R1-002): previously a Magisk device auto-bound the root local
            // HTTP control surface (8888-8899) with NO explicit user opt-in. A root
            // long-running HTTP listener reachable by any local App is a high-value
            // attack surface; silent binding is the defect. Require explicit opt-in:
            // either ENABLE_HTTP_SERVER=1 (g_cfg.enableHttpServer) or a user-created
            // confirmation marker STATE_DIR/http_autostart. Otherwise refuse to bind
            // and log a prominent warning telling the operator how to enable it.
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

    // ROUND-004: register config reload hook so ENABLE_HTTP_SERVER flips take
    // effect live (1->0 stops, 0->1 starts). Fail-open: any exception in the
    // hook is swallowed by the registry and must not block the scheduler loop.
    aatpe::config::registerReloadHook("http_server", [](const Config& cfg) {
        applyHttpServerState(cfg.enableHttpServer);
    });


    // round-5: mark subsystems ACTIVE once bring-up has completed.
    // eBPF/frame/thermal/scheduler are driven continuously in the loop; their
    // STARTING->ACTIVE transition happens here. eBPF reload failures flip them
    // back to DEGRADED via g_ebpfScheduler error hooks.
    {
        using namespace aatpe::subsys;
        auto& reg = Registry::instance();
        reg.setState("scheduler", SubsystemState::ACTIVE);
        reg.setState("thermal", SubsystemState::ACTIVE);
        reg.setState("frame", SubsystemState::ACTIVE);
        reg.setState("ml", SubsystemState::ACTIVE);
        // eBPF: ACTIVE only if the scheduler reports it loaded; DEGRADED otherwise.
        reg.setState("ebpf", g_ebpfActive.load(std::memory_order_acquire) ? SubsystemState::ACTIVE
                                           : SubsystemState::DEGRADED);
        // WebUI: ACTIVE if HTTP server is up, DEGRADED if running in webroot-only.
        reg.setState("webui", g_cfg.enableHttpServer ? SubsystemState::ACTIVE
                                                      : SubsystemState::DEGRADED);
        reg.writeStateFile(STATE_DIR);
        safemode::writeStateFiles(STATE_DIR);
        selfobs::writeStateFiles(STATE_DIR);
        // round-5: resource budget ledger (verified against the compiled
        // constants so no abnormal state can grow unbounded).
        writeFile((std::string(STATE_DIR) + "/resource_budget").c_str(),
                  "rt_threads=32 cmd_queue=64 webui_sse_clients=16 "
                  "ebpf_ring_pages=64-128 log_max_bytes=8388608 self_cpu_budget_pct=50");
    }

}

// [split] state functions moved to aatped_state.cpp

// ============================================================================
// #97: daemonLoop subfunction split (code-organization only; behavior unchanged)
// Each extracted tick <= 100 lines. All function-local statics preserved.
// ============================================================================

// Thermal snapshot + combined migration pressure (was inline at top of loop).
static void loopThermalTick(const Config& loopCfg) {
    // ROUND-011: thermal sampling runs in its own thread; main loop only
    // computes freshness (detects stuck thread) and reads cached pressure.
    if (loopCfg.thermalEnabled) thermalFreshnessTick();
    // Combined thermal migration pressure (skin+battery) for placement tuning.
    g_thermalMigrationPressure.store(
        readInt((std::string(STATE_DIR) + "/thermal_migration_pressure").c_str(), 0),
        std::memory_order_relaxed);
}

// Throttled SurfaceFlinger frame-read (>=200ms) to avoid dumpsys/Binder storm.
static void loopSurfaceFlingerTick(const Config& loopCfg) {
    static long long s_lastSfReadMs = 0;
    long long nowSfMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (loopCfg.frameAware && (nowSfMs - s_lastSfReadMs) >= 200) {
        readSurfaceFlinger();
        s_lastSfReadMs = nowSfMs;
    }
}

// Jank rising-edge detection + high/medium priority event dispatch.
static void loopJankAndEvents(void) {
    // P-01 jank debounce state: require jank to SUSTAIN for >= kJankConfirmMs
    // before dispatching, and fire at most once per jank episode.
    static long long s_jankFirstSeenMs = 0;
    static long long s_jankClearSinceMs = 0;
    static bool s_emergencyArmed = true;
    constexpr long long kJankConfirmMs = 600;   // >=3 samples @200ms sampling
    constexpr long long kJankReleaseMs = 400;    // must clear before re-arm
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    int curJank = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0);
    bool curJankBool = (curJank != 0);
    // P-01: a single over-latency sample (often produced by a previous
    // sched_setaffinity's own cache-cold penalty) must NOT immediately fire an
    // emergency migration — that was the self-exciting loop:
    //   migrate -> cache cold -> jank rising edge -> emergency migrate -> cache cold.
    // Require jank to persist >= kJankConfirmMs, and after firing, disarm until
    // jank has actually cleared for kJankReleaseMs (so a lingering jank condition
    // does not re-trigger every loop tick).
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
        // 核心问题4: when Safe Mode is latched, ordinary periodic
        // migrations are suspended anyway (decidePlacement returns SAFE_MODE for
        // every thread). Waking for an immediate full decision here would only
        // re-scan threads and burn CPU with zero migrations — drain it silently.
        if (aatpe::subsys::safemode::isActive()) {
            // drained; no-op
        } else {
        SystemState medEvtState = getSystemState();
        bool medEvtHighCpu = (g_lastCpuUsagePercent.load(std::memory_order_relaxed) >= 80)
                          || (g_selfCpuUsagePercent.load(std::memory_order_relaxed) >= 50);
        // EMERGENCY-opt: medium-priority events are fully silent in
        // EMERGENCY (regular placement is skipped anyway; only high-priority
        // THERMAL_SPIKE/JANK events trigger emergency migration).
        if (medEvtState == SystemState::DEGRADED ||
            medEvtState == SystemState::EMERGENCY ||
            medEvtHighCpu) {
            // drain silently; normal poll cycle handles placement.
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

// Frame-synchronized lightweight decision (refresh-rate update + frame trigger).
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

// Poll-based fallback event detection: perf-core freq, CPU temp spike, hotplug.
static void loopPollEventDetection(const Config& loopCfg) {
    static int s_lastPerfMaxFreq = 0;
    static int s_lastCpuTemp = 0;
    static std::string s_lastOnlineCpus = "";
    static long long s_lastThermalEventMs = 0;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Perf-core max frequency (big pool + mid pool first core).
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

    // CPU temperature (smart-detected zone, fallback scan of all thermal zones).
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

    // CPU hotplug poll (online cpus).
    std::string curOnlineCpus = readFile("/sys/devices/system/cpu/online");
    if (!s_lastOnlineCpus.empty() && curOnlineCpus != s_lastOnlineCpus) {
        aatpe::event::EventMonitor::instance().triggerEvent(
            aatpe::event::EventType::HOTPLUG, "online:" + curOnlineCpus);
    }
    s_lastOnlineCpus = curOnlineCpus;
}

// Foreground app detection + APP_SWITCH event + per-app config apply.
static void loopForegroundTick(const Config& loopCfg) {
    static std::string s_prevForegroundApp = "";
    // ROUND-015/#A.4: transitional schedtune boost producer for the return-to-
    // foreground settle window (symmetric with touch/gpu producers). Released
    // after kFgBoostDwellMs so steady-state boost stays vote-driven.
    static long long s_fgBoostEdgeMs = 0;
    constexpr long long kFgBoostDwellMs = 2000;
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string curFg = aatpeDetectForegroundApp();
    // audit4 F2: gate the fg-DEPENDENT work (per-app config, APP_SWITCH, the
    // transitional fg boost arm) on a known foreground. The actuators below
    // (psi::tick / arb::tick / coldStartTick / gpuBoostTick + the thermal-freeze
    // gate) are fg-INDEPENDENT and now run outside this gate so they are driven
    // by live pressure instead of freezing at their last values.
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
            // P1-7: arm/disarm game cold-start preset on the switch edge.
            aatpe::coldStartOnForegroundSwitch(curFg);
            // ROUND-012: restore original affinity for the new FG's pinned threads.
            aatpe::placement::releaseForegroundDownpins(curFg);
            // ROUND-015/#A.3+#A.1: publish the freshly-detected fg package into the
            // placement-side cache (defeats the stale 3s-TTL window) and arm the
            // debounce-free re-pin sweep. MUST run before onInputEvent() below so
            // that tick consumes the fresh package and the armed sweep.
            aatpe::placement::onForegroundSwitchEdge(curFg);
            // ROUND-015/#A.4: foreground return raises top-app schedtune.boost for
            // the settle window instead of waiting for the first touch/GPU vote.
            aatpe::aatpeBoostSet("fg", 50);
            s_fgBoostEdgeMs = nowMs;
            // ROUND-007b: cold-start / foreground switch is a high-priority event
            // for spillover promotion. onInputEvent() self-guards (EMERGENCY /
            // no key threads → no-op) and consumes the re-pin sweep armed above.
            aatpe::placement::onInputEvent();
        }
        // ROUND-015/#A.4: release the transitional fg boost once the settle window
        // elapses (hand control back to the touch/gpu/jank vote max()).
        if (s_fgBoostEdgeMs != 0 && nowMs - s_fgBoostEdgeMs > kFgBoostDwellMs) {
            aatpe::aatpeBoostClear("fg");
            s_fgBoostEdgeMs = 0;
        }
    } else {
        // audit4 F2: fg unknown/empty (boot early, dumpsys cooldown, SF restart,
        // permission loss). The fg boost arm + fg-dependent work above are skipped,
        // but we MUST NOT leave the transitional "fg" schedtune producer stuck at
        // >=50. Release it unconditionally and forget the previous fg so the next
        // valid foreground re-arms APP_SWITCH cleanly. The PSI/arb/coldstart/gpu
        // actuators below now run OUTSIDE this gate, so they are driven by live
        // pressure instead of freezing at their last values; on a quiet sample
        // psi::tick releases the group-uclamp cut/boost and clears
        // g_pauseMigration/g_bigcoreDeprefer on its own.
        if (s_fgBoostEdgeMs != 0) {
            aatpe::aatpeBoostClear("fg");
            s_fgBoostEdgeMs = 0;
        }
        s_prevForegroundApp.clear();
    }

    // v4.4.1 P0 (RC-1a/RC-1b): periodic, NON-edge cgroup readback heal for the
    // current fg package's critical threads. Self-throttled to ~1s and self-gated
    // (passive mode / thermal wall / cpuset capability). Closes the "same package
    // enters/exits a small window -> no package-name edge -> stuck in background
    // cpuset (0-3)" hole. Must run every cycle regardless of whether a fg edge
    // fired above.
    if (fgValid) {
        aatpe::placement::healForegroundCgroupMembership(curFg);
    }

    // C1 exploration: predictive pre-boost (default-off, byte-identical when false).
    // Runs from loopForegroundTick but gated to 1s cadence (state machine designed
    // for 1s ticks: 15s hard cap, 3-arm/min rate limit, 3-strike early exit).
    if (preboostEnabled.load(std::memory_order_relaxed)) {
        static long long s_lastC1TickMs = 0;
        long long nowC1Ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // 1s cadence gate: skip if less than 1000ms since last C1 tick.
        if (s_lastC1TickMs > 0 && (nowC1Ms - s_lastC1TickMs) < 1000) {
            // skip
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
        // CPU load history: sample every 1s (this tick), keep ~6 samples for slope.
        static std::vector<int> s_cpuHist;
        static long long s_lastCpuSampleMs = 0;
        long long nowCpuMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        float sampleIntervalSec = 1.0f;  // default: 1s tick
        if (s_lastCpuSampleMs > 0) {
            sampleIntervalSec = (float)(nowCpuMs - s_lastCpuSampleMs) / 1000.0f;
        }
        s_lastCpuSampleMs = nowCpuMs;
        // Read current CPU load (cheap /proc/stat read)
        int cpuLoadPct = readInt((std::string(STATE_DIR) + "/cpu_util_pct").c_str(), -1);
        if (cpuLoadPct >= 0) {
            s_cpuHist.push_back(cpuLoadPct);
            if ((int)s_cpuHist.size() > 6) s_cpuHist.erase(s_cpuHist.begin());
        }
        int hrPct = g_thermalHeadroomPct.load(std::memory_order_relaxed);
        bool hrValid = (hrPct >= 0);
        float hr01 = hrValid ? (float)hrPct / 100.0f : 0.0f;
        int jankVal = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0);
        // MAIN-03 fix: g_cfg.controlMode is a std::string mutated by the config
        // inotify thread under g_cfgMutex. An unlocked (g_cfg.controlMode == "passive")
        // compare raced a concurrent std::string assignment (realloc -> read a
        // freed char* -> SIGSEGV). g_cfgMutex is the HIGHEST-level lock and may be
        // taken alone; snapshot the bool under it before building the features.
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
        // W5 §6: when the scene engine is ON and committed to LOADING, hold the C1
        // latency pre-boost back (loading must not arm it). Off path returns false
        // unconditionally, so preBoostTick runs exactly as v4.4.2 (byte-identical).
        if (!aatpe::scenewire::loadingActiveSuppressesPreboost()) {
            preBoostTick(nowCpuMs, pf);
        }
        } // end 1s cadence gate
    }

    // P1-7: re-assert presets every cycle while the window is open.
    // ROUND-014 P3: real gates. IO PSI "some" stall >20% -> downgrade (CPU boost
    // can't help an IO-bound launch). Thermal: g_thermalEmergency OR HAL level
    // >=3 (SEVERE) freeze RT presets; LIGHT/MODERATE (<3) still allowed.
    // v4.3.6: PSI decision closed-loop. Consumes the diag PSI snapshot + frame
    // timing, runs the verified psi_decision core (hysteresis + debounce), and
    // drives group-uclamp / cold-start-IO / placement seams. This now owns the
    // io-pressure gate previously inlined as "psiIoSomePct > 20". No-op on a
    // stale sample or when /proc/pressure is absent (fail-safe).
    aatpe::psi::tick();
    // v4.3.7: multi-source arbitration. Runs the four cores, merges into the
    // existing named boost bridge ("scene" producer), independent frame-cap +
    // efficiency files. Hard-wall absolute; context soft-only. No-op safe.
    aatpe::arb::tick();
    // v4.2.3 P0: authoritative ML training freeze gate.
    // - thermal_enabled=0 → never freeze (user disabled thermal = no constraints).
    // - Sysfs CPU temp >= 85°C → freeze (primary evidence).
    // - HAL SEVERE + sysfs confirms hot → freeze (secondary).
    // - HAL alone with invalid data → does NOT freeze.
    // - Hysteresis: resume at 80°C (5°C below pause).
    // g_thermalEmergency still triggers freeze (hardware protection path).
    // MAIN-06 fix: use the per-loop loopCfg snapshot instead of reading
    // g_cfg.thermalEnabled directly (inotify hot-reload writes g_cfg under
    // g_cfgMutex; a bool scalar tear is benign in practice but the loop body
    // rule established at :2997 is "no direct g_cfg reads").
    bool mlThermalFreeze = aatpe::aatpeThermalMayFreezeTraining(loopCfg.thermalEnabled);
    // g_thermalEmergency is the hardware-protection emergency (P0, always active).
    bool thermalFreeze = g_thermalEmergency.load(std::memory_order_relaxed) || mlThermalFreeze;
    aatpe::coldStartSetThermalFreeze(thermalFreeze);
    // F-10 (v4.1.2-hotfix, per-outcome refinement): the classic schedstat
    // run-ratio outcome stays meaningful even under MODERATE throttling
    // (it is a *relative* within-window measure), so we no longer freeze ALL
    // training at HAL>=2. Instead:
    //   * SEVERE(3)+ / thermal emergency -> thermalFreeze, full pause (unchanged).
    //   * MODERATE(2) only -> keep training the classic path, but suppress the
    //     frame-time/jank-dominated fusion (thermal-inflated p95 would
    //     otherwise be trained as a placement effect). The outcome loop drops
    //     the frame weight to 0 when g_mlFrameOutcomeSuppressedByThermal is set.
    int mlHalLevel = aatpe::aatpeThermalHalLevel();
    g_mlTrainingPausedByThermal.store(thermalFreeze, std::memory_order_relaxed);
    g_mlFrameOutcomeSuppressedByThermal.store(mlHalLevel >= 2 && mlHalLevel < 3,
                                              std::memory_order_relaxed);
    // v4.2.4: publish the ML-freeze gate decision + thresholds to STATE_DIR
    // every cycle (auto-exposed via /state/all).
    writeFile((std::string(STATE_DIR) + "/thermal_ml_freeze_reason").c_str(),
              aatpe::aatpeThermalFreezeReason());
    writeFile((std::string(STATE_DIR) + "/thermal_ml_freeze_pause_c").c_str(), "85");
    writeFile((std::string(STATE_DIR) + "/thermal_ml_freeze_resume_c").c_str(), "80");
    aatpe::coldStartTick();
    // P2-5: CPU/GPU joint boost while GPU is pegged in-game.
    aatpe::gpuBoostTick();
    // ROUND-014 P3: GPU throttle detection tick (writes gpu_hard_throttle / gpu_thermal_cap).
    // Throttled to every 3 cycles to avoid excessive sysfs reads.
    {
        static int s_throttleTickDiv = 0;
        if (++s_throttleTickDiv >= 3) {
            s_throttleTickDiv = 0;
            aatpeGpuThrottleTick();
        }
    }
}

// Staggered auxiliary scan: battery / GPU / audio / game-scene / preload (one per 5s tick).
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
        // v6.0.0 RT-A A1: g_cpuMetrics is a std::map mutated on the collection
        // thread (under g_cpuMetricsMutex). Iterating it lock-free here was a data
        // race (concurrent read/write on std::map = UB). Snapshot the per-CPU util
        // values under the mutex, then release before doing anything else. This
        // slot takes no other lock, so no nesting/ABBA risk.
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
        // W5: gate the scene detector. On -> the weighted W5 engine replaces
        // detectGameScene (and drives C2 when c2EnvelopeEnabled). Off -> legacy
        // detectGameScene path, byte-identical to v4.4.2.
        if (aatpe::scenewire::chooseRoute(sceneInferEnabled.load(std::memory_order_relaxed))
                == aatpe::scenewire::SceneRoute::NEW_SCENEFUSE) {
            aatpe::scenewire::auxTickSlot3(avgCpuLoad);
        } else {
            detectGameScene(avgCpuLoad, jankVal ? 5.0f : 0.0f, 0.0f);
        }
        // v4.4.4 C4 cgroup v2 freezer. Gated by c4FreezerEnabled() (default off);
        // OFF -> this call returns immediately, byte-identical to v4.4.3.
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

// eBPF event drain + degrade/recover/ring-adjust lifecycle.
static void loopEbpfDrain(void) {
    // P3-6: snapshot g_ebpfMode (std::string) under g_ebpfInfoMutex before the
    // equality compare. The eBPF poll thread publishes g_ebpfMode elsewhere under
    // the same mutex; an unsynchronized std::string read races that write and can
    // observe a half-updated internal pointer -> SIGSEGV.
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
        // P1-2 fix: compare + publish g_ebpfMode under g_ebpfInfoMutex. The
        // eBPF poll thread writes here while the main loop reads g_ebpfMode in
        // writeStateJson()/state files; an unsynchronized std::string R/W can
        // publish a half-updated internal pointer -> SIGSEGV.
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

// ROUND-010 / MAIN-01: evict dead-tid entries from the unbounded per-tid side
// maps to prevent long-uptime memory growth.
//
// Locking contract (this is the whole point of MAIN-01):
//   g_threads is an unordered_map<int,ThreadInfo> mutated by the eBPF poll thread
//   (onFork/onExec/onSwitch/onExit) under g_threadsMutex; insert/erase can rehash
//   the bucket array. The side maps (g_lastMigrationTime / g_migFail* /
//   g_schedstatLast*) each have their OWN inner mutex. We must NOT call
//   g_threads.count() while holding only an inner lock (that was the data race:
//   follow a stale bucket pointer on concurrent rehash -> UB/crash). Instead:
//     1. take g_threadsMutex, snapshot the live tid set into a local
//        unordered_set, RELEASE g_threadsMutex;
//     2. then take each inner map lock in turn and erase entries whose tid is
//        absent from the snapshot.
//   This never nests g_threadsMutex under an inner lock, preserving the documented
//   g_threadsMutex -> g_scoreCacheMutex global order. The TOCTOU window (a tid
//   dying between snapshot and erase) only leaves a stale entry until the next
//   calibration scan — same semantics as before the fix.
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
    time_t lastWebuiFastUpdate = 0;  // fix23: threads/decisions/explain/system (2s)
    time_t lastWebuiModelUpdate = 0; // fix23: model.json (5s)
    // steady_clock timestamp of the last heavy threads/decisions/explain
    // JSON write. When g_sseClientCount == 0 (no EventSource attached) these three
    // expensive serializations (over ~500 threads) are throttled to at most once per
    // 5s instead of every 2s. Empty epoch -> first loop always writes.
    std::chrono::steady_clock::time_point lastHeavyJsonSteady{};
    time_t lastApkDetect = 0;         // fix65: re-detect Helper APK install state every 30s
    // #97: lastAuxScan / auxSlot moved into loopAuxScan() (function-local statics).
    // P2 Problem8: 1s cadence for non-critical diagnostic state writes. The
    // adaptive loop runs every ~50-100ms, and ~50+ diagnostic files used to be
    // rewritten EVERY loop, thrashing /proc I/O. They now gate on diagTick once
    // per second. heartbeat stays every loop (watchdog liveness). control_mode /
    // current_profile / thermal_emergency are written ON-CHANGE (real time) by
    // the command/thermal paths and are NOT throttled here.
    time_t lastDiagWrite = 0;
    bool diagTick = false;
    int loopCount = 0;
    // #8: hourly integrity re-verification (3600s cadence).
    time_t lastIntegrityCheck = 0;
    // fix59: Track ML maturity state changes for logging
    bool s_prevMlMature = false;
    while (!g_shouldExit.load(std::memory_order_relaxed)) {
        auto loopStart = std::chrono::steady_clock::now();
        // stamp the start of this decision cycle. The poll block at
        // the end of the loop uses this to enforce the minimum decision interval
        // (decisionMinIntervalMs, default 33ms) after an important-event wake.
        g_lastDecisionSteady = loopStart;
        // fix27: heartbeat written as the VERY FIRST operation in the loop,
        // before processControlCommands() or any file reads. This ensures
        // the watchdog sees a fresh heartbeat even if a later operation blocks.
        // Previously heartbeat was after processControlCommands() + uptime write,
        // meaning a block in those early calls would freeze heartbeat too.
        //
        // v94-P5-fix: heartbeat used to be written EVERY loop (every 50-200ms),
        // which is a sysfs open/write/close per loop. The watchdog liveness
        // threshold is watchdogInterval=3s, so 1s granularity is more than
        // enough and cuts one write syscall per loop. Gate to 1s.
        {
            // W-004: stamp the heartbeat with CLOCK_BOOTTIME (monotonic, INCLUDES
            // time spent suspended) instead of wall-clock time(nullptr). On Android
            // the display turns off / the device suspends; wall-clock stops advancing
            // during suspend, so a wall-clock heartbeat that was fresh before suspend
            // looks "stale" when the device wakes up and the watchdog SIGKILLs the
            // healthy daemon. CLOCK_BOOTTIME keeps counting across suspend, so the
            // age the watchdog computes is the real running-time age. The watchdog
            // reads the file and computes age with the SAME clock.
            static time_t s_lastHeartbeatWrite = 0;
            struct timespec hbTs;
            clock_gettime(CLOCK_BOOTTIME, &hbTs);
            time_t nowHb = (time_t)hbTs.tv_sec;
            if (nowHb - s_lastHeartbeatWrite >= 1) {
                writeFile((std::string(STATE_DIR) + "/heartbeat").c_str(), (int)nowHb);
                // ROUND-014 P3: wakeup rebalance detection.
                // If BOOTTIME jumped > 60s since last tick, we just resumed from suspend.
                // Force a full rebalance on next iteration.
                static time_t s_prevBootTime = 0;
                if (s_prevBootTime != 0 && (nowHb - s_prevBootTime) > 60) {
                    LOG_INFO("Wakeup: BOOTTIME jumped %llds since last tick, forcing rebalance",
                             (long long)(nowHb - s_prevBootTime));
                    g_skipNextSleep.store(true);
                    // Clear all cached thread state to avoid stale decisions.
                    // (thread_scan will re-read everything on next iteration)
                }
                s_prevBootTime = nowHb;
            }
        }
        // P2 Problem8: latch the 1s diag-write tick ONCE per loop. All gated
        // diagnostic blocks below read diagTick; the tick flips exactly when the
        // wall-clock second advances, so every gated block writes in the same loop.
        {
            time_t nowDiag = time(nullptr);
            diagTick = (nowDiag - lastDiagWrite >= 1);
            if (diagTick) lastDiagWrite = nowDiag;
        }
        // Frame-time outcome snapshot (cached, TTL-aware; non-blocking on cache hit)
        // LED-007 (R1-007): removed the per-loop `(void)getCurrentFrameTimeSnapshot();`
        // no-op. It only acquired g_cfgMutex and discarded the result "to warm the
        // cache"; getSnapshot() is TTL-cached and is already called on demand by the
        // ML outcome path (ftAfter/ftSnap below) and by webui_json when /api/frame_time
        // is polled, so the discarded call bought nothing but lock traffic.
        processControlCommands();
        // v88 W3: 已删除旧的battery_aging_toggle/ml_cold_start_toggle文件机制。
        // 前端已不再使用, 统一通过control命令(battery_aging=/set_ml_cold_start=)处理,
        // 这些命令会更新内存+写state+持久化到aatpe.conf。
        // #8: hourly module integrity re-verification.
        {
            time_t nowInt = time(nullptr);
            if (nowInt - lastIntegrityCheck >= 3600) {
                lastIntegrityCheck = nowInt;
                bool intOk = aatpe::integrity::verifyModuleIntegrity();
                if (!intOk) {
                    // G3-001: capability already revoked via fail-hook (writes rejected).
                    LOG_ERROR("INTEGRITY: hourly re-verification FAILED");
                    std::string rep = aatpe::integrity::getIntegrityReport();
                    if (!rep.empty()) LOG_ERROR("INTEGRITY report:\n%s", rep.c_str());
                } else {
                    LOG_INFO("INTEGRITY: hourly re-verification OK");
                }
            }
        }
        // fix22: update uptime BEFORE checkSystemLoad, so it is written
        // even when system load is critical and the loop hits `continue`.
        // Previously uptime was at loop end; sustained high load caused
        // daemon_uptime to never be created, making WebUI show "--".
        // fix27: heartbeat moved to loop top (before processControlCommands).
        // P2 Problem8: daemon_uptime is 1Hz-granularity; gate on diagTick. It is
        // still written BEFORE checkSystemLoad (early in the loop) so a high-load
        // `continue` cannot starve it.
        if (diagTick) {
            int uptime = (int)(time(nullptr) - g_startTime.load());
            writeFile((std::string(STATE_DIR) + "/daemon_uptime").c_str(), uptime);
        }
        // fix: handle SIGUSR1 profile reload request
        if (g_reloadProfile.load(std::memory_order_relaxed)) {
            g_reloadProfile.store(0, std::memory_order_relaxed);
            // item 8: do not call loadProfile() directly — go through the Profile
            // Manager so the current winner (respecting MANUAL/APP/EMERGENCY locks)
            // is re-applied with the generation guard.
            LOG_INFO("SIGUSR1 received: re-applying current profile winner");
            aatpe_profile::profileReapplyWinner();
        }
        // P1-1: snapshot g_cfg under g_cfgMutex once per loop iteration. The
        // config inotify thread writes g_cfg under g_cfgMutex; the main loop
        // previously read g_cfg.thermalEnabled / g_cfg.frameAware / etc. directly
        // without the lock, producing torn reads on std::string / int fields
        // during a hot-reload. Direct g_cfg.xxx reads in the loop body now use
        // this per-loop snapshot (cfg).
        Config loopCfg;
        snapshotConfig(loopCfg);
        loopThermalTick(loopCfg);
        loopSurfaceFlingerTick(loopCfg);
        loopJankAndEvents();
        loopFrameAwareTick();
        readCpuMetrics();
        // LED-006 (R1-006): system_load is mirrored to STATE_DIR by readCpuMetrics()
        // just above, and jank_detected is written by frame.cpp; neither is rewritten
        // mid-iteration. Previously the same files were open+read+closed again inside
        // the ML covariate block, the rebalance block, the jank-active gate and the
        // adaptive-interval block — 2-3 redundant open/read/close syscalls per loop.
        // Snapshot once here and reuse for the rest of this iteration.
        const int systemLoadNow = readInt((std::string(STATE_DIR) + "/system_load").c_str(), 0);
        const bool jankActiveNow = readInt((std::string(STATE_DIR) + "/jank_detected").c_str(), 0) != 0;
        precomputeCpuBaseScores();  // fix88: precompute per-CPU base score before decisions
        loopPollEventDetection(loopCfg);
        // fix84 Feature 3: Proactive migration check (prediction-driven)
        // Predicts future CPU overload and migrates critical threads early.
        // Requires high prediction confidence (>0.5) and ML positive gain.
        proactiveMigrationCheck();
        // fix52 new-C: Refresh CPU topology in case of hotplug (online/offline cores)
        refreshCpuTopology();
        // ROUND-005: startup evidence backoff retry (1s/3s/10s/30s/60s) until
        // capacity+cpufreq nodes are ready. Shares the same detect path.
        if (topologyRetryDue((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count())) {
            LOG_INFO("topology: scheduled evidence retry");
            detectCpuTopology();
        }
        // ROUND-004: if hotplug changed the detected cluster count, the ML model's
        // pool dimension no longer matches topology. Re-init safely (LOG_WARN) so
        // we never index out of range in the scoring path.
        {
            int wantPools = mlPoolCount();
            if (g_mlModel.getPoolCount() != wantPools) {
                LOG_WARN("ML pool dimension drift: model=%d topology=%d, migrating (preserve weights)",
                         g_mlModel.getPoolCount(), wantPools);
                std::lock_guard<std::mutex> lock(g_mlMutex);
                // ROUND-010 D4-07: incremental migration, NOT init() — a late-coming
                // PRIME cpu7 (3->4) keeps knowledge learned on pools 0..2.
                g_mlModel.migratePoolCount(wantPools);
                std::vector<int> cpuToPool(g_cpuCount, -1);
                for (int cpu : g_littlePool) if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 0;
                for (int cpu : g_midPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 1;
                for (int cpu : g_bigPool)    if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 2;
                for (int cpu : primePool())  if (cpu >= 0 && cpu < g_cpuCount) cpuToPool[cpu] = 3;
                g_mlModel.setCpuPoolAssignment(cpuToPool);
            }
        }
        // fix52 P1-9: Periodically re-probe unmigratable threads (auto-unlock)
        reprobeUnmigratableThreads();
        loopForegroundTick(loopCfg);
        // P0-7: effective auto-profile = user configured AND not runtime-suppressed
        // by the Helper APK. We never mutate g_cfg.autoProfile on Helper install.
        bool effectiveAutoProfile = loopCfg.autoProfile && !g_helperAutoSuppressed.load();
        if (effectiveAutoProfile) autoSelectProfile();
        time_t now = time(nullptr);
        loopAuxScan(now);
        // fix65: Periodically re-detect Helper APK install state (every 30s).
        // Previously only detected once at daemonInit; if APK was installed after
        // daemon start, or uninstalled/reinstalled, the state would be stale.
        // This handles dynamic install/uninstall and also recovers from transient
        // `pm list packages` failures during boot.
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
        // v73: eBPF event-driven — consume events every decision cycle
        // (v94 P1-2: single-consumer model; see loopEbpfDrain for full notes)
        loopEbpfDrain();
        // v73: Dual-mode thread scanning
        //
        // v94 P1-4: explicit responsibility division (eBPF incremental vs full scan):
        //   - eBPF events (fork/exec/exit/sched_switch) own REAL-TIME, INCREMENTAL
        //     updates: new-thread placeholder insertion, immediate role
        //     classification on exec, tname fill-in, currentCpu tracking, and
        //     thread removal on exit. These cover the live needs of the scheduler
        //     on every decision cycle.
        //   - The periodic full /proc scanThreads() exists ONLY as a
        //     CALIBRATION / fault-tolerance mechanism: it reconciles threads the
        //     event stream missed (lost ring events, exec before attach, races),
        //     refines role via the schedstat heuristic (which needs accumulated
        //     runtime a freshly-exec'd thread lacks), and re-reads starttime /
        //     nice / runTime / nrSwitches. It is NOT the primary discovery path.
        //   Because incremental updates now cover the bulk of the work, the
        //   calibration scan interval is relaxed from 30s to 60s in event_driven
        //   mode to cut /proc walk overhead. The full scan is NEVER removed (it
        //   remains the safety-net/calibration path). Pure polling mode keeps the
        //   configured threadScanInterval unchanged.
        {
            int scanInterval = loopCfg.threadScanInterval;
            // P3-7: g_ebpfMode (std::string) is published under g_ebpfInfoMutex by
            // the eBPF poll thread; snapshot it here to avoid an unsynchronized
            // string compare racing that write.
            std::string ebpfModeSnap;
            {
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                ebpfModeSnap = g_ebpfMode;
            }
            if (g_ebpfActive.load(std::memory_order_acquire) && ebpfModeSnap == "event_driven") {
                scanInterval = 60;  // v94 P1-4: 60s calibration scan (was 30s)
            }
            if (now - lastThreadScan >= scanInterval) {
                scanThreads();
                lastThreadScan = now;
                g_eventConsumer.clearPending();
                // ROUND-010 / MAIN-01: evict dead-tid entries from the unbounded
                // per-tid side maps. Extracted to aatpeEvictDeadTidEntries() so the
                // locking contract (snapshot live tids under g_threadsMutex, then
                // erase under the per-map inner locks) is exercised by a host
                // concurrency test. See that function for the rationale.
                aatpeEvictDeadTidEntries();
            }
            // v94 fix (audit breakpoint 5): the old `else if
            // (g_eventConsumer.hasPendingEvents()...)` "incremental scan" block
            // was dead code — localThreads/count were declared then (void)
            // discarded, and it never touched g_threads. With onFork/onExec/
            // onSwitch now updating g_threads in real time, no targeted rescan
            // is needed between full scans. The pendingFork/Exit/Exec counters
            // are kept as statistics only.
        }
        // fix: cleanup decisions directory every 60 seconds
        if (now - lastDecisionCleanup >= daemon::kDecisionCleanupIntervalSec) {
            cleanupDecisionsDir();
            // #77: state/cache TTL sweep — prune stale history, settle, score cache.
            {
                Config cfgSnap;
                snapshotConfig(cfgSnap);
                stateTtlSweep(now, cfgSnap.stateCacheTtlHours);
            }
            // #35: write placement avg gain/confidence diagnostic files.
            writePlacementAvgStats();
            lastDecisionCleanup = now;
        }

        // fix24: Write cumulative counters and WebUI JSON BEFORE checkSystemLoad().
        // Previously these were at loop end; when checkSystemLoad() returned false
        // (sustained high load >= 95%), the `continue` skipped them entirely, so
        // decision_count / migration_count / *.json were never created, and WebUI
        // showed "--" for all four panels. Same root cause as the fix22 daemon_uptime bug.
        // P2 Problem8: cumulative monotonic counters — gate to 1s (diagTick). Still written
        // BEFORE checkSystemLoad so a high-load `continue` cannot starve them.
        if (diagTick) {
            // LED-009 (R1-009): g_totalDecisions/g_totalMigrations are
            // atomic<long long>; casting to int wrapped past 2^31. Use the long long
            // overload so the cumulative counters do not wrap after long uptimes.
            writeFile((std::string(STATE_DIR) + "/decision_count").c_str(), (long long)g_totalDecisions.load());
            writeFile((std::string(STATE_DIR) + "/migration_count").c_str(), (long long)g_totalMigrations.load());
            // ROUND-007: also write the canonical total_decisions/total_migrations
            // filenames (WebUI /state/all expects these as standalone state files).
            atomicWriteFile((std::string(STATE_DIR) + "/total_decisions").c_str(),
                            (long long)g_totalDecisions.load());
            atomicWriteFile((std::string(STATE_DIR) + "/total_migrations").c_str(),
                            (long long)g_totalMigrations.load());
        }
        {
            time_t nowWebui = time(nullptr);
            if (nowWebui - lastWebuiFastUpdate >= 2) {
            // P0-fix: UNCONDITIONAL write of threads/decisions/explain JSON.
                // The previous hasActiveClients() gate assumed only the built-in HTTP
                // WebUI needs these files, but third-party frontends (Vibin UI, etc.)
                // read /dev/aatpe/*.json directly without opening an SSE/HTTP
                // connection. The gate caused those frontends to see stale/empty
                // data forever. writeThreadsJson is already ScoringContext-optimized
                // (one context build + per-tid 500ms score cache), so the 2s
                // serialization cost is acceptable on 8-core devices.
                //
// (JSON 降频): when NO SSE /events stream is attached
                // (g_sseClientCount == 0), the three heaviest writers
                // (threads/decisions/explain — over ~500 threads each) are
                // additionally throttled to at most once per 5s (steady_clock).
                // This preserves the P0-fix guarantee for third-party
                // frontends (they still see data every 5s, never stale forever)
                // while cutting wasted serialization when nobody is streaming.
                // Core state files (decision_count/daemon_uptime/ebpf_event_rate,
                // written under diagTick above) are UNCONDITIONAL and unaffected.
                int sseClients = g_sseClientCount.load(std::memory_order_acquire);
                auto nowHeavy = std::chrono::steady_clock::now();
                bool heavyJsonDue = (sseClients > 0) ||
                    (lastHeavyJsonSteady == std::chrono::steady_clock::time_point{}) ||
                    (std::chrono::duration_cast<std::chrono::seconds>(
                        nowHeavy - lastHeavyJsonSteady).count() >= 5);
                if (heavyJsonDue) {
                    writeThreadsJson();
                    writeDecisionsJson();
                    writeC4Json();   // v4.4.6: read-only C4 freezer status
                    writeExplainJson();
                    lastHeavyJsonSteady = nowHeavy;
                }
                writeSystemJson();
                writePerCpuJson();
                writeLogsJson();  // v66 P2-2: real log output for WebUI
                writeConfigJson();  // fix65: current config JSON
                // P2-5: Write prediction diagnostics state files
                // S2: when NO prediction error samples exist yet, do NOT write a
                // fabricated 0.000000 (WebUI read that as "perfect zero error").
                // Publish an EMPTY file instead: webui_json emits JSON null and the
                // frontend renders "--". getAveragePredictionMAE() still returns 0
                // in that case, so we gate explicitly on hasAnyPredictionErrorSamples().
                if (hasAnyPredictionErrorSamples()) {
                    writeFile((std::string(STATE_DIR) + "/prediction_mae").c_str(), getAveragePredictionMAE());
                } else {
                    writeFile((std::string(STATE_DIR) + "/prediction_mae").c_str(), "");
                }
                writeFile((std::string(STATE_DIR) + "/prediction_confidence").c_str(), getPredictionConfidence(0));
                // P2-5: Write event monitor and frame sync stats
                aatpe::event::EventMonitor::instance().writeStats(STATE_DIR);
                aatpe::frameaware::writeStats(STATE_DIR);
                // ROUND-009: holistic diagnostics (PSI, GPU devfreq, thermal
                // throttle state, rq-wait accessor). Read-only; safe in any
                // system state (EMERGENCY/DEGRADED/etc). Runs on the same 2s
                // cadence as the other WebUI state writers.
                aatpe::diag::collect();
                aatpe::diag::writeStateFiles(STATE_DIR);
                lastWebuiFastUpdate = nowWebui;
            }
            if (nowWebui - lastWebuiModelUpdate >= 5) {
                writeModelJson();
                lastWebuiModelUpdate = nowWebui;
                // fix56 P1-5: Write ml_samples / ml_alpha state files.
                // Previously these files were never written, so CLI/WebUI consumers
                // reading state/ml_samples or state/ml_alpha always got empty/default.
                {
                    std::lock_guard<std::mutex> mlLock(g_mlMutex);
                    writeFile((std::string(STATE_DIR) + "/ml_samples").c_str(), g_mlModel.getTotalSamples());
                    // #38: expose the drift latch to WebUI/CLI.
                    writeFile((std::string(STATE_DIR) + "/ml_drift_detected").c_str(),
                              aatpe_ml::g_mlDriftDetected.load() ? 1 : 0);
                    // fix65: store alpha as decimal string (e.g. "0.0138"), not integer*1000.
                    // Previously stored (int)(alpha*1000)=14, which was ambiguous/misleading.
                    {
                        char alphaBuf[32];
                        snprintf(alphaBuf, sizeof(alphaBuf), "%.4f", (double)g_mlModel.getAlpha());
                        writeFile((std::string(STATE_DIR) + "/ml_alpha").c_str(), alphaBuf);
                    }
                    // fix59: Write ml_mature state file (0=immature, 1=mature).
                    // Confidence gate is enabled only when mature.
                    bool nowMature = g_mlModel.isMature();
                    writeFile((std::string(STATE_DIR) + "/ml_mature").c_str(), nowMature ? 1 : 0);
                    // fix59: Log maturity state transitions
                    if (nowMature && !s_prevMlMature) {
                        LOG_INFO("ML matured (samples=%d, alpha=%.3f), confidence gate ENABLED",
                                 g_mlModel.getTotalSamples(), (double)g_mlModel.getAlpha());
                    } else if (!nowMature && s_prevMlMature) {
                        LOG_INFO("ML reset to immature (samples=%d, alpha=%.3f), confidence gate DISABLED",
                                 g_mlModel.getTotalSamples(), (double)g_mlModel.getAlpha());
                    }
                    s_prevMlMature = nowMature;
                    // fix104 v34: ML learning funnel diagnostics state files
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
                    // v4.2.1 T1-fix: update() internal-gate rejection state files.
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_thermal").c_str(), (long long)aatpe_ml::g_mlUpdateSkipThermal.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_disabled").c_str(), (long long)aatpe_ml::g_mlUpdateSkipDisabled.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_pool").c_str(), (long long)aatpe_ml::g_droppedInvalidTargetPool.load());
                    writeFile((std::string(STATE_DIR) + "/ml_update_skip_outcome").c_str(), (long long)aatpe_ml::g_invalidOutcomeSkipped.load());
                    writeFile((std::string(STATE_DIR) + "/ml_training_paused_by_thermal").c_str(), aatpe_ml::g_mlTrainingPausedByThermal.load() ? 1 : 0);
                    writeFile((std::string(STATE_DIR) + "/ml_model_anomaly_latched").c_str(), g_mlModel.isAnomalyLatched() ? 1 : 0);
                    // v4.5.0 shadow-sampling funnel (observe-only mode). Separate
                    // counters so formal vs shadow collection never mix.
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_outcome_valid").c_str(), (long long)g_mlShadowOutcomeValid.load());
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_samples_collected").c_str(), (long long)aatpe_ml::g_mlShadowSamplesCollected.load());
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_samples_dropped").c_str(), (long long)aatpe_ml::g_mlShadowSamplesDropped.load());
                    writeFile((std::string(STATE_DIR) + "/ml_shadow_sampling_enabled").c_str(), loopCfg.mlShadowSampling ? 1 : 0);
                }
            }
        }

        // P2-fix: Periodic exposure of decision fusion weights, frame-time outcome
        // data, and aggregated snapshot.json. Runs every 5s so WebUI panels
        // (/api/fusion, /api/frame_time, snapshot.json) have fresh data.
        {
            static time_t s_lastExposureWrite = 0;
            if (now - s_lastExposureWrite >= 5) {
                writeFusionWeightsState();
                writeFrameTimeOutcomeState();
                writeSnapshotJson();
                s_lastExposureWrite = now;
            }
        }

        // fix25: 高负载时不再 `continue` 跳过整个决策循环.
        // 原逻辑: checkSystemLoad()==false → sleep+continue → 决策循环永远不执行
        //         → g_totalDecisions/g_totalMigrations恒为0, g_decisionRing永远为空.
        // 新逻辑: 高负载进入降频模式(degraded), 仍执行决策但降低每轮迁移上限,
        //         并在循环末尾加倍sleep间隔. 确保任何负载下都有决策产出.
        bool highLoad = !checkSystemLoad();
        // Phase3 Batch1: DEGRADED state also halves migration cap (persists across
        // cycles until 3 consecutive normal-load cycles trigger NORMAL transition).
        SystemState sysState = getSystemState();
        bool degradedActive = (sysState == SystemState::DEGRADED);
        int effectiveMaxMigrations = loopCfg.maxMigrationsPerCycle;
        if (highLoad || degradedActive) {
            if (highLoad) g_highLoadCycles++;
            // DEGRADED state uses /4 (was /2 for all high-load).
            // Temporary high-load spikes keep /2; sustained DEGRADED halves
            // again to reduce migration CPU cost during overheat/overload.
            if (degradedActive) {
                effectiveMaxMigrations = std::max(1, loopCfg.maxMigrationsPerCycle / 4);
            } else {
                effectiveMaxMigrations = std::max(1, loopCfg.maxMigrationsPerCycle / 2);
            }
            // v94-fix (卡顿修复): 高负载日志节流 — 原逻辑每 50ms 循环都打 LOG_WARN，
            // 每秒 20 次日志输出本身就是开销。改为最多每 5 秒打一次。
            static time_t s_lastHighLoadLog = 0;
            time_t nowLog = time(nullptr);
            if (nowLog - s_lastHighLoadLog >= 5) {
                LOG_WARN("System load high / DEGRADED state: max_migrations_per_cycle=%d (was %d), state=%s",
                         effectiveMaxMigrations, loopCfg.maxMigrationsPerCycle,
                         systemStateToString(sysState));
                s_lastHighLoadLog = nowLog;
            }
        }

        // fix65: Write tracked_threads state file (was never written, CLI read default 0)
        {
            std::lock_guard<std::mutex> lock(g_threadsMutex);
            writeFile((std::string(STATE_DIR) + "/tracked_threads").c_str(), (int)g_threads.size());
        }
        // fix21: ML Online Learning - process pending decisions from previous cycle
        // fix45 Problem 2 & 10: Uses measureOutcomeV2 with freq normalization,
        // load covariate, and incremental baseline comparison.
        if (loopCfg.mlLearningEnabled) {
            std::lock_guard<std::mutex> threadsLock(g_threadsMutex);
            std::lock_guard<std::mutex> mlLock(g_mlMutex);
            // v4.5.0: push the observe-only shadow-sampling switch into the model.
            // Independent of formal training gating; shadow recording is pure
            // telemetry and never mutates the live model.
            g_mlModel.setShadowSamplingEnabled(loopCfg.mlShadowSampling);
            g_mlPendingExpired += g_mlModel.clearExpiredPendingDecisions(60);  // fix104 v34: count expired
            g_mlModel.cleanExpiredBaselines(300);
            // Read system load for covariate (Problem 2) — LED-006: reuse the
            // per-loop snapshot instead of re-opening the state file.
            int loadInt = systemLoadNow;
            // v94-P3-fix: gate the per-pending schedstat outcome batch to 500ms.
            // Root cost: each pending decision opens /proc/<pid>/task/<tid>/schedstat
            // (open+fscanf+close). Running that every 50-200ms loop = N open/close
            // pairs per loop. The existing exponential backoff throttles sleeping
            // threads; this adds a hard 500ms floor on the WHOLE outcome batch so
            // active pending threads' schedstat isn't re-read every loop. The cheap
            // clearExpiredPendingDecisions / cleanExpiredBaselines above still run
            // every loop. Outcome latency budget is unchanged (measured at most
            // 500ms later — irrelevant for learning).
            static unsigned long long s_lastOutcomeBatchMs = 0;
            unsigned long long nowOutcomeMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            bool runOutcomeBatch = (nowOutcomeMs - s_lastOutcomeBatchMs >= 500);
            if (runOutcomeBatch) s_lastOutcomeBatchMs = nowOutcomeMs;
            if (runOutcomeBatch) {
            // P0-2: Iterate ALL pending decisions (includes exited threads),
            // not just threads currently in g_threads. This ensures pending
            // decisions for threads that exited between cycles are still
            // measured/expired rather than silently leaked.
            std::vector<PendingDecision> allPending = g_mlModel.getAllPendingDecisions();
            for (const auto& pd : allPending) {
                int tid = pd.tid;
                // Look up thread info; may be null if thread exited
                auto tit = g_threads.find(tid);
                bool threadAlive = (tit != g_threads.end());
                if (!threadAlive) {
                    // Thread exited: count and discard pending (can't measure outcome)
                    g_mlPendingThreadExited++;
                    g_mlModel.clearPendingDecision(tid);
                    continue;
                }
                const ThreadInfo& ti = tit->second;
                // fix56 P1-4: TID-reuse protection. If the thread's starttime differs
                // from the pending decision's recorded starttime, the TID was recycled
                // (old thread exited, new thread created with same TID). Learning from
                // this stale pending would pollute the model with mismatched features/outcomes.
                // Discard the pending and skip learning for this cycle.
                if (pd.starttime != 0 && ti.starttime != 0 && pd.starttime != ti.starttime) {
                    LOG_DEBUG("ML: tid=%d TID reused (starttime %llu -> %llu), discarding stale pending",
                              tid, (unsigned long long)pd.starttime, (unsigned long long)ti.starttime);
                    g_mlPendingTidReused++;  // fix104 v34: count TID-reuse discards
                    g_mlModel.clearPendingDecision(tid);
                    continue;
                }
                // fix104 v39: Minimum measurement interval. Skip outcome measurement
                // until 500ms after migration. Root cause of ml_outcome_valid=0:
                // measuring at 50ms loop interval when thread is still running
                // continuously → run_ratio > 0.99 → saturation filter skips all samples.
                // v70-fix: nowMs hoisted to outer scope for sleep-backoff check below.
                unsigned long long nowMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (pd.create_time_ms > 0) {
                    // v66-fix: >= instead of > (same-millisecond edge case)
                    if (nowMs >= pd.create_time_ms &&
                        (nowMs - pd.create_time_ms) < aatpe_ml::MIN_MEASURE_INTERVAL_MS) {
                        continue;  // too soon, retry next cycle
                    }
                }
                // v70-fix: Sleep-pending exponential backoff.
                // Without backoff, a sleeping thread is re-measured every 50ms,
                // causing 99.5% of measurements to be the same sleeping thread.
                // Backoff = min(100 * 2^(consecutive_sleeps-1), 5000) ms.
                // Sequence: 100, 200, 400, 800, 1600, 3200, 5000 (capped).
                if (pd.consecutive_sleeps > 0 && pd.last_attempt_ms > 0) {
                    int shift = pd.consecutive_sleeps - 1;
                    if (shift > 6) shift = 6; // 2^6=6400 > cap, higher unnecessary
                    unsigned long long backoffMs = 100ULL * (1ULL << shift);
                    if (backoffMs > 5000) backoffMs = 5000;
                    if ((nowMs - pd.last_attempt_ms) < backoffMs) {
                        continue;  // in backoff, retry later
                    }
                }
                // Measure outcome using V2 incremental method
                // v66-fix (ML funnel root cause): ti.runTime is only refreshed
                // every 5s by scanThreads() (full rebuild). Using stale ti.runTime
                // here made delta_run==0 for every measurement between scans, so
                // ALL samples were classified as "sleeping" and skipped — only 1
                // sample in 2.5min. Read fresh schedstat per pending decision,
                // identical to the hybrid-learning tick (aatped.cpp ~3859).
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
                // v6.0.0 RT-A A1: g_cpuMetrics is a std::map mutated on the
                // collection thread. The previous lock-free .find() here (and the
                // maxFreq reads below) raced with that writer = UB. Snapshot the
                // needed fields under a short g_cpuMetricsMutex critical section,
                // then release. Lock order is threads+ml -> cpuMetrics (all other
                // holders take cpuMetrics without re-acquiring threads/ml, so no
                // ABBA). A2: the schedstat read above uses only snapshotted pid/tid.
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
                // fix55 (问题7/8): base freq uses ACTUAL runtime CPU's maxFreq,
                // not target CPU's maxFreq. In pool affinity, thread may run on
                // any CPU in the target pool, so normalization must use actual CPU.
                int baseFreq = 1;
                if (curCpuMaxFreq > 0) {
                    baseFreq = curCpuMaxFreq;
                } else if (tgtCpuMaxFreq > 0) {
                    // Fallback to target CPU if actual CPU metrics unavailable
                    baseFreq = tgtCpuMaxFreq;
                }
                // Get pre-migration baseline (Problem 10)
                float baseline = g_mlModel.getBaselineRunRatio(tid);
                time_t after_ts = time(nullptr);
                unsigned long long after_mono_ns = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                g_mlOutcomeMeasured++;  // fix104 v34: count outcome measurement attempts
                // fix104 v34: classify skip reason before calling measureOutcomeV2
                bool skipReset = (afterRunTime < pd.before_run_time);
                unsigned long long delta_run = skipReset ? 0 : (afterRunTime - pd.before_run_time);
                bool skipSleeping = (!skipReset && delta_run == 0);
                // v66-issue16: Get frame-time after snapshot for UI/Render/Game threads.
                float ftAfterP95 = 0.0f;
                int ftAfterSamples = 0;
                bool isUiRenderThread = (ti.role == ROLE_S1_UI || ti.role == ROLE_S2_RENDER || ti.role == ROLE_S3_GAME);
                // v4.1.2-hotfix: under MODERATE thermal throttle the frame-time
                // fusion is suppressed (thermal-inflated p95); force the outcome to
                // the pure schedstat run-ratio path by zeroing the frame weight AND
                // skipping the after-frame snapshot. SEVERE/emergency already paused
                // training entirely via g_mlTrainingPausedByThermal.
                bool ftSuppressed = g_mlFrameOutcomeSuppressedByThermal.load(std::memory_order_relaxed);
                if (loopCfg.mlFrameTimeOutcome && !ftSuppressed && isUiRenderThread && pd.frame_time_before_samples > 0) {
                    // A1-001: do NOT call getCurrentFrameTimeSnapshot() here. That
                    // helper re-reads g_cfg.mlFrameTimeOutcome under g_cfgMutex,
                    // which would nest g_mlMutex -> g_cfgMutex and violate the
                    // documented lock order (aatped_config.h:28). We already hold
                    // the per-loop loopCfg snapshot (taken before g_mlMutex at
                    // line ~2699) and gate on loopCfg.mlFrameTimeOutcome above, so
                    // it is safe to read the provider's cached snapshot directly.
                    // GfxInfoFrameTimeProvider::getSnapshot() takes only leaf
                    // locks (g_foregroundMutex via callback), never g_cfgMutex.
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
                // measureOutcomeV2 returns -1.0f for samples that should be skipped
                // (anomaly: delta_run==0, saturated, counter reset, etc.)
                if (outcome >= 0.0f) {
                    // ROUND-014 P3: boost-period sample de-weighting. A thread promoted
                    // within the last 2s is still benefiting from the transient boost,
                    // not from the pool itself — shrink its outcome toward neutral 0.5 so
                    // the model does not learn "big core is always better" (positive feedback).
                    long long ageSec = (long long)after_ts - (long long)pd.before_timestamp;
                    if (ageSec >= 0 && ageSec < 2) {
                        outcome = 0.5f + (outcome - 0.5f) * 0.5f;
                    }
                    // ROUND-014 P3: fuse the G4 frame jank vote (read-only, single source).
                    // A fresh-jank frame window pushes the outcome strongly negative.
                    //
                    // F1.4 (v4.3.8): bound the release-tail over-penalty. The frame-side
                    // hysteresis (frame.cpp aatpeFusionJankVote) keeps g_jankBoostVote
                    // armed for kJankBoostReleaseConsecutive clean frame cycles AFTER jank
                    // has already cleared (the release debounce tail). The old code
                    // punished EVERY outcome measured while the vote was armed, so a
                    // window measured entirely inside that tail (already smooth) was
                    // still pushed toward "bad". We now attribute the jank label penalty
                    // ONLY to windows that coincide with a fresh off->on arm edge, and
                    // auto-expire the penalty after a bounded hold. The placement-side
                    // vote (g_jankBoostVote) is UNCHANGED — this only gates the ML label
                    // injection, so placement hysteresis/thrash protection is untouched.
                    //   - off->on edge  -> open a kJankLabelPunishHoldMs punish window.
                    //   - vote releases -> immediately kill any tail punishment.
                    //   - beyond the hold (sustained arming without a fresh edge, or the
                    //     post-release tail) -> no label rewrite.
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
                    // v4.2.1 T1-fix: g_mlOutcomeValid++ is MOVED below to AFTER
                    // update() returns TRAINED. Previously it fired here (before the
                    // call), so a finite outcome that update() later rejected
                    // (thermal pause / disabled-anomaly / pool / negative) was
                    // still displayed as "valid learned" — the source of the
                    // "outcome_valid=25 but total_samples=0" contradiction.
                    // fix55 (问题6/49): Outcome attributed to ACTUAL runtime pool,
                    // not target pool. Read thread's current CPU → map to its pool.
                    // This closes the causal loop: decision→execution→actual runtime→outcome→learning.
                    int actualCpu = ti.currentCpu;
                    // ROUND-009a: attribute the sample using the MODEL's own
                    // cpu->pool assignment (set in daemonInit: LITTLE=0/MID=1/BIG=2/
                    // PRIME=3), NOT getPoolForCpu(), which collapses PRIME into BIG(2)
                    // for legacy callers. That collapse sent every cpu7 sample into
                    // pool 2 and left the PRIME pool 3 at sample_count==0 forever.
                    int actualPool = -1;
                    {
                        std::vector<int> pa = g_mlModel.getCpuPoolAssignment();
                        if (actualCpu >= 0 && actualCpu < (int)pa.size())
                            actualPool = pa[actualCpu];
                    }
                    if (actualPool < 0 || actualPool >= g_mlModel.getPoolCount()) {
                        // Fallback to target pool if actual CPU pool is unknown
                        actualPool = pd.target_pool;
                        if (actualPool < 0 || actualPool >= g_mlModel.getPoolCount()) {
                            std::vector<int> pa2 = g_mlModel.getCpuPoolAssignment();
                            actualPool = (pd.target_cpu >= 0 && pd.target_cpu < (int)pa2.size())
                                       ? pa2[pd.target_cpu] : -1;
                            if (actualPool < 0) actualPool = 0;
                        }
                    }
                    // v4.2 Batch B1/B2: push the v4.2 ML switches into the model.
                    // All three default OFF (loopCfg.mlOutcomeMadFilter / mlDriftRelearn
                    // / mlEpsilonGreedy == 0), so with defaults the update() call is
                    // bit-identical to v4.1.1.
                    // A1-001: use the loopCfg snapshot (taken before g_mlMutex)
                    // instead of reading g_cfg directly under g_mlMutex. The direct
                    // g_cfg read was an unsynchronized race with loadConfig() and
                    // also risked nesting g_ml -> g_cfg. loopCfg already carries
                    // these fields via snapshotConfig().
                    g_mlModel.setMadFilterEnabled(loopCfg.mlOutcomeMadFilter != 0);
                    g_mlModel.setDriftRelearnEnabled(loopCfg.mlDriftRelearn != 0);
                    // v4.2 Batch B3: paired-difference label. A control tick (boost
                    // deliberately suppressed) that measured a GOOD outcome must teach
                    // the model this placement does NOT need a boost -> mirror the
                    // outcome about 0.5.
                    float learnOutcome = outcome;
                    if (pd.counterfactualControl)
                        learnOutcome = LearningModel::counterfactualControlLabel(outcome);
                    if (pd.shadow) {
                        // v4.5.0 observe-only shadow sample. In passive/auxiliary
                        // mode no migration was applied (control semantics unchanged);
                        // we still measure the thread's actual run-ratio and record the
                        // (features -> outcome) pair into the separate SHADOW buffer.
                        // It MUST NOT train the live model (update), MUST NOT feed the
                        // drift detector, and MUST NOT nudge weights -- otherwise
                        // observe-only mode would silently change online decisions.
                        g_mlModel.recordShadowOutcome(actualPool, pd.features, learnOutcome);
                        g_mlShadowOutcomeValid++;
                    } else {
                    // v4.2.1 T1-fix: update() now returns WHY it did/did not train.
                    // Only count g_mlOutcomeValid when the model actually accepted
                    // the sample (TRAINED). Rejections by thermal/disabled/pool/
                    // negative are counted inside update() via dedicated atomics so
                    // the funnel shows exactly where samples are dropped.
                    UpdateResult ur = g_mlModel.update(actualPool, pd.features, learnOutcome);
                    if (ur == UpdateResult::TRAINED) g_mlOutcomeValid++;
                    // #38: feed the closed loop into the drift detector. When
                    // outcomes keep degrading the detector latches g_mlDriftDetected
                    // (read by the fusion path to shrink alpha / fall back).
                    {
                        static aatpe_ml::DriftDetector s_drift;
                        // LED-008 (R1-008): the window was applied only on the first
                        // outcome (s_driftWindowSet latch), so a config hot-reload of
                        // mlDriftWindow never took effect. Re-apply whenever the value
                        // actually changes.
                        static int s_driftLastWindow = 0;
                        if (loopCfg.mlDriftWindow != s_driftLastWindow) {
                            s_drift.setWindow(loopCfg.mlDriftWindow);
                            s_driftLastWindow = loopCfg.mlDriftWindow;
                        }
                        s_drift.recordOutcome(outcome, actualPool);
                        // v4.2 Batch B2: on a drift latch, pull THIS pool's weights
                        // half-way toward neutral 0.5 (never a full reset). Gated by
                        // mlDriftRelearn; off = no-op (v4.1.1).
                        // A1-001: use loopCfg snapshot, not direct g_cfg read.
                        if (loopCfg.mlDriftRelearn != 0 && s_drift.isDriftDetected()) {
                            g_mlModel.nudgeTowardNeutral(actualPool);
                        }
                    }
                    }  // end formal (non-shadow) training path
                    LOG_DEBUG("ML update V2: tid=%d actualPool=%d actualCpu=%d targetPool=%d outcome=%.3f baseline=%.3f load=%d samples=%d alpha=%.4f%s",
                              tid, actualPool, actualCpu, pd.target_pool, outcome, baseline, loadInt,
                              g_mlModel.getTotalSamples(), g_mlModel.getAlpha(),
                              pd.shadow ? " [SHADOW]" : "");
                    // fix56 P2-6: Only clear pending after a successful learning update.
                    // Previously invalid outcomes (-1) also cleared pending, permanently
                    // losing the sample. Now retry next cycle (bounded by 30s expiry).
                    // v70-fix: Reset sleep backoff on successful measurement.
                    g_mlModel.setPendingSleepBackoff(tid, 0, 0);
                    g_mlModel.clearPendingDecision(tid);
                } else {
                    // fix104 v34: classify skip reason for diagnostics
                    if (skipReset) g_mlOutcomeSkipReset++;
                    else if (skipSleeping) {
                        g_mlOutcomeSkipSleeping++;
                        // v70-fix: Exponential backoff for sleeping threads.
                        // Increment consecutive sleep count and record attempt time.
                        // Non-sleep skips (saturated, reset) do NOT increment.
                        g_mlModel.setPendingSleepBackoff(tid, pd.consecutive_sleeps + 1, nowMs);
                    } else {
                        // Check saturated: run_ratio > 0.99
                        unsigned long long dt_ns = (after_ts > pd.before_timestamp) ?
                            (unsigned long long)(after_ts - pd.before_timestamp) * 1000000000ULL : 1;
                        float rr = (dt_ns > 0) ? (float)delta_run / (float)dt_ns : 0.0f;
                        if (rr > 0.99f) g_mlOutcomeSkipSaturated++;
                        else g_mlOutcomeSkipOther++;
                    }
                    // fix56 P2-6: Invalid outcome (anomaly: thread sleeping, saturated,
                    // counter reset, etc.). Keep pending active so the next cycle can
                    // retry measurement. clearExpiredPendingDecisions(60) bounds lifetime.
                    LOG_DEBUG("ML update V2: tid=%d SKIPPED (anomaly/outlier), pending kept for retry", tid);
                }
            }
            // fix45 Problem 10: Record baseline for threads NOT being migrated this cycle.
            // This builds the pre-migration reference for future outcome measurements.
            //
            // F1.1 (v4.3.8): the old baseline was avgRunNs/10ms (mean timeslice
            // duration / 10ms), a TIMESLICE proxy, while measureOutcomeV2's nrr is a
            // normalized DUTY CYCLE (delta_run/delta_time / freq_factor). Those are
            // different physical quantities, so the "relative improvement" subtraction
            // in measureOutcomeV2 was dimensionally invalid whenever a baseline existed.
            // We now feed recordBaselineDuty() a FRESH schedstat runTime paired with a
            // monotonic timestamp taken at the SAME instant; the model does two-period
            // tracking and emits a baseline_nrr in the SAME dimension as outcome nrr.
            // First period (no prior sample) records nothing -> outcome uses 1.0-nrr.
            //
            // Cost gate: reading schedstat for every tracked thread under g_threadsMutex
            // every 500ms would block the eBPF poll thread (the v94-P3-fix rationale).
            // The duty baseline is a slow-moving reference, so this fresh-read pass runs
            // on its own 2s cadence (4x the outcome batch) while the cheap expiry sweeps
            // above still run every batch.
            static unsigned long long s_lastBaselineDutyMs = 0;
            bool runBaselineDuty = (nowOutcomeMs - s_lastBaselineDutyMs >= 2000);
            if (runBaselineDuty) {
                s_lastBaselineDutyMs = nowOutcomeMs;
                unsigned long long nowBaseNs = (unsigned long long)std::chrono::duration_cast<
                    std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                for (auto&_sb_item : g_threads) {
                    auto& tid = _sb_item.first; auto& ti = _sb_item.second; (void)tid;
                    if (g_mlModel.findPendingDecision(tid) >= 0) continue;  // has pending decision (v66 regression-fix #1: -1 is truthy, must check >=0)
                    if (ti.nrSwitches == 0) continue;
                    // Fresh schedstat runTime, paired with nowBaseNs (same instant) so
                    // the duty window [prev, now] is dimensionally correct. Mirrors the
                    // outcome-path fresh read at ~3189.
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
                    // Freq for normalization: actual runtime CPU's curFreq/maxFreq, the
                    // same inputs measureOutcomeV2 uses for freq_factor.
                    // v6.0.0 RT-A A1: snapshot under g_cpuMetricsMutex (was lock-free
                    // find racing with the collection writer = UB).
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
            } // v94-P3-fix: end runOutcomeBatch 500ms gate
            // v66-fix: Learning rate decay moved INTO LearningModel::update()
            // (per-model, tied to sample count). No longer time-based here.
            // Old code: g_mlModel.decayLearningRate(); — removed.
        }

        // v66: Hybrid learning real-time fine-tuning tick (every 500ms)
        // Reads per-thread per-CPU runtime + eBPF per-CPU stats, performs
        // small-step SGD on independent delta weights (20% influence).
        // Zero overhead when disabled (early return inside tick()).
        {
            long long nowHybridMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (loopCfg.mlHybridEnabled && (nowHybridMs - g_hybridLastTickMs) >= 500) {
                g_hybridLastTickMs = nowHybridMs;
                // Collect per-thread snapshots
                std::vector<aatpe_ml_hybrid::ThreadCpuSnapshot> snapshots;
                // P2-#7: first pass — under g_threadsMutex, collect only the
                // (pid, tid, cpu, role, budget) list. Do NOT open /proc files
                // while holding the lock (hundreds of syscalls block the eBPF
                // poll thread). Release the lock, then read schedstat.
                struct ThreadRef {
                    pid_t pid; int tid; int cpu; int role;
                    int priority; int nice; uint64_t cachedRunTime;
                    uint64_t nrSwitches;  // R005c: verbatim ThreadInfo.nrSwitches for hybrid f[2]
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
                // Second pass — NO g_threadsMutex held. Open /proc schedstat here.
                uint64_t nowNs = (uint64_t)nowHybridMs * 1000000ULL;
                snapshots.reserve(refs.size());
                // ROUND-009a: use the model's cpu->pool assignment (PRIME=3) instead
                // of getPoolForCpu() which folds PRIME into BIG(2). Otherwise hybrid
                // deltas for cpu7 threads land in pool 2.
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
                // Read eBPF per-CPU runtime stats + REAL sched_switch event count
                std::vector<uint64_t> cpuRuntime;
                static uint64_t s_lastEbpfTotalEvents = 0;
                uint64_t ebpfEventsThisTick = 0;
                if (g_ebpfActive.load(std::memory_order_acquire)) {
                    std::vector<uint64_t> counts;
                    if (g_ebpfScheduler.readEmbeddedCpuCounts(counts)) {
                        cpuRuntime = counts;
                        // Sum per-CPU sched_switch event counts = real eBPF events
                        uint64_t totalNow = 0;
                        for (uint64_t c : counts) totalNow += c;
                        if (s_lastEbpfTotalEvents > 0 && totalNow >= s_lastEbpfTotalEvents) {
                            ebpfEventsThisTick = totalNow - s_lastEbpfTotalEvents;
                        }
                        s_lastEbpfTotalEvents = totalNow;
                    }
                }
                // Fallback: use schedstat if eBPF unavailable
                if (cpuRuntime.empty()) {
                    std::vector<aatpe_ebpf::CpuSchedStats> stats;
                    if (aatpe_ebpf::EbpfScheduler::readFallbackSchedstat(stats)) {
                        cpuRuntime.reserve(stats.size());
                        for (const auto& s : stats) cpuRuntime.push_back(s.totalRuntimeNs);
                    }
                }
                int cpuCount = (int)cpuRuntime.size();
                if (cpuCount == 0) cpuCount = (int)g_allCpus.size();
                // v94 fix (问题18): per-CPU thermal feature vector. Read under the
                // same lock used by readCpuMetrics; temp is in millidegrees/degC
                // per CpuMetrics, normalized to [0,1] by /120 (120C ceiling).
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
                // Execute hybrid tick (small-step SGD on delta weights)
                {
                    std::lock_guard<std::mutex> hlock(g_hybridMutex);
                    aatpe_ml_hybrid::instance().tick(snapshots, cpuRuntime, cpuCount,
                                                      cpuThermalNorm);
                }
                // Accumulate REAL eBPF sched_switch event count (cumulative)
                g_hybridEbpfSamplesTotal += ebpfEventsThisTick;
                // Write hybrid state files for WebUI
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
        // v94 fix29: carry starttime alongside the migration result so the
        // post-loop outcome recording can verify the TID was not recycled in
        // the window after g_threadsMutex was dropped.
        struct MigrationResult { int tid; int pid; int targetCpu; bool placeOk; unsigned long long starttime; };
        std::vector<MigrationResult> migrationResults;

        // ====================================================================
        // v94-PLAN3 (方案3 事件驱动架构): global rebalance backstop gate.
        // The event-driven path re-scores only dirty/expired/active threads.
        // As a SAFETY NET (so we never silently go stale), force a full
        // invalidate every kRebalanceMs (1s), or when temperature changed
        // >5°C or global load changed >20%, or when dirty threads exceed
        // kDirtyRebalanceThreshold (burst of sched_switch).
        // v95-OPT5: 周期从 1.5s 收紧到 1s; 新增 dirty 阈值提前触发。
        // ====================================================================
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
            // v95-OPT5: 统计 dirty 线程数。一帧内大量 sched_switch 同时涌入时,
            // 提前做一次全量重平衡, 避免脏数据堆积到下个 1s 周期。
            int dirtyCount = 0;
            {
                std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                for (const auto& kv : g_scoreCache) {
                    if (kv.second.dirty) dirtyCount++;
                }
            }
            if (dirtyCount >= kDirtyRebalanceThreshold) {
                forceRebalance = true;
                s_lastRebalanceMs = nowDecisionMs;  // 重置周期, 避免刚触发又触发
            }
            int curTemp = g_thermalCpu.load(std::memory_order_relaxed);
            int curLoad = systemLoadNow;  // LED-006: reuse per-loop snapshot
            if (s_lastRebalanceTemp >= 0 && abs(curTemp - s_lastRebalanceTemp) >= 5) forceRebalance = true;
            if (s_lastRebalanceLoad  >= 0 && abs(curLoad  - s_lastRebalanceLoad)  >= 20) forceRebalance = true;
            s_lastRebalanceTemp = curTemp;
            s_lastRebalanceLoad  = curLoad;
            if (forceRebalance) {
                invalidateAllScores();
                // Prune cache entries whose threads have exited between scans.
                // P0-3 fix: enforce global lock order g_threadsMutex -> g_scoreCacheMutex.
                // onSwitch() (ebpf_agg.cpp:120-126) holds g_threadsMutex then calls
                // markThreadScoreDirty() which takes g_scoreCacheMutex. Previously this
                // prune block held g_scoreCacheMutex and reached into g_threads, i.e. the
                // reverse order -> classic AB-BA deadlock in fallback/degraded mode where
                // onSwitch runs on the eBPF poll thread concurrently with the main loop.
                // Taking g_threadsMutex first here matches the onSwitch ordering and also
                // makes the g_threads.find() lookup race-free.
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

        // v95-P1fix: snapshot g_threads under g_threadsMutex before iterating.
        // In degraded mode (queueEnabled_=false), onSwitch() runs on the eBPF
        // poll thread and modifies g_threads under g_threadsMutex. Iterating
        // g_threads directly without holding the lock causes iterator races.
        // We copy to a local vector under the lock, release the lock, then
        // iterate the copy. applyPlacement() takes const ThreadInfo& and
        // looks up the real entry internally under its own lock.
        std::vector<std::pair<int, ThreadInfo>> threadsSnapshot;
        {
            std::lock_guard<std::mutex> tlock(g_threadsMutex);
            threadsSnapshot.reserve(g_threads.size());
            for (const auto& kv : g_threads) threadsSnapshot.push_back(kv);
        }
        // opt#6: when jank is active the loop runs at the fast 50ms cadence.
        // In that window only critical threads (S1_UI/S2_RENDER/S3_GAME) are scored +
        // migrated; non-critical threads (S4_THROUGHPUT/S5_BACKGROUND) are deferred to
        // the next normal decision cycle. Their score-cache entries stay valid and
        // their dirty bits are left set, so they are picked up normally once jank
        // clears. Critical-thread responsiveness (and thus jank relief) is unchanged.
        bool jankActive = jankActiveNow;  // LED-006: reuse per-loop snapshot
        // rel15 大小核颠倒 fix: key control-transition detector. When the system
        // ENTERS NORMAL from a higher-priority state (EMERGENCY/DEGRADED/PASSIVE),
        // force a one-shot bypass rescore of every cached thread. Rationale: during
        // EMERGENCY the governor moved heavy workers to little cores to cool; when
        // thermal recovers those workers must be promoted back. In rel15 the active
        // gate skipped them because a low-switch-rate worker's schedstat view was
        // frozen by skipSchedstat and its eBPF mark aged past kInactiveMs, so it
        // was judged active=false and never re-scored (stuck on little cores until
        // cycle ~11). forceRescoreAllScores() makes the very first NORMAL cycle
        // re-score them regardless of the active signal; each entry consumes its
        // own bypass, so this is a single sweep, not a per-second regression.
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
        // EMERGENCY-opt: under SystemState::EMERGENCY the thermal governor
        // already rejected ~25% of regular placements, so running the full
        // decidePlacement() computation per thread is pure CPU waste. Skip the
        // regular periodic placement loop entirely; only event-triggered emergency
        // migration (THERMAL_SPIKE/JANK/FPS_DROP) still runs on its own path.
        // Dirty score-cache bits are left set and flushed once we return to NORMAL.
        bool emergencySystem = (getSystemState() == SystemState::EMERGENCY);
        // low-overhead: when the host is catastrophically overloaded
        // (VMOS/game/kswapd storm), skip regular placement too — only event-driven
        // emergency migration keeps running.
        bool lowOverhead = g_lowOverheadMode.load(std::memory_order_relaxed);
        // round9: EMERGENCY no longer fully skips regular placement — it
        // now runs placement once every 5 loops (~every 400ms at the 80ms EMERGENCY
        // interval). Fully skipping left thread distribution static while hot, and
        // on a VMOS box we still need to move heavy threads to little cores to cool.
        // Host-load low_overhead (non-thermal) still fully skips.
        static int s_emgPlaceCounter = 0;
        bool emgDoPlacement = true;
        if (emergencySystem && !lowOverhead) {
            s_emgPlaceCounter++;
            emgDoPlacement = (s_emgPlaceCounter % 5 == 0);
        }
        // round10: low_overhead (host overloaded) also no longer fully
        // skips placement — it runs once every 10 loops (~1.5s at 150ms interval).
        // Fully skipping froze decision_count/migration_count even though the box
        // still needed scheduling. Emergency migration remains event-driven.
        static int s_loPlaceCounter = 0;
        bool loDoPlacement = true;
        if (lowOverhead) {
            s_loPlaceCounter++;
            loDoPlacement = (s_loPlaceCounter % 10 == 0);
        }
        for (auto&_sb_item : threadsSnapshot) {
        auto& tid = _sb_item.first; auto& ti = _sb_item.second; (void)tid;
                if ((emergencySystem && !emgDoPlacement) || (lowOverhead && !loDoPlacement)) continue;  // EMERGENCY/low-OH throttled
                if (ti.role == ROLE_S0_SYSTEM) continue;
                // opt#6: jank fast path — defer non-critical threads.
                if (jankActive && ti.role > ROLE_S3_GAME) continue;
                // v94-PLAN3: event-driven scoring cache. Rescore a thread ONLY if:
                //   (a) dirty  — an eBPF sched_switch touched it (onSwitch marked it), OR
                //   (b) never scored yet, OR
                //   (c) cache expired (>=kScoreTtlMs) AND the thread is still active
                //       (had a sched_switch within kInactiveMs).
                // Sleeping/inactive threads with a fresh cache are SKIPPED — their
                // last decision is still valid, and re-scoring them every 200ms was
                // the root CPU hog. This collapses ~200 full scores/loop down to only
                // the threads that actually moved. Batching across the whole loop
                // (rather than migrating per-event) avoids thrash.
                {
                    std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                    auto& entry = g_scoreCache[tid];
                    // v600-fix2: extracted decision (see aatped_utils.h
                    // shouldRescoreThread). Passes g_ebpfActive so eBPF recency
                    // is only trusted when event-driven is live; otherwise the
                    // /proc schedstat signal (ti.switchesPerSec/idleRefreshes,
                    // maintained by thread_scan.cpp) is the active fallback.
                    bool ebpfLive = g_ebpfActive.load(std::memory_order_acquire);
                    if (!shouldRescoreThread(entry, ti, nowDecisionMs,
                                             kScoreTtlMs, kInactiveMs, ebpfLive)) continue;
                }
                // G6: stable bucketing by Android uid (restart-stable); fall back
                // to per-tid bucketing only when uid cannot be resolved.
                int abGroup;
                int resolvedUid = resolveUidForPid(ti.pid);
                if (resolvedUid >= 0) {
                    abGroup = abTestAssignByUid(resolvedUid) + 1;
                } else {
                    abGroup = abTestAssignThread(tid) + 1;
                }
                Decision d = decidePlacement(ti, loopCfg);
                g_totalDecisions++;
                // v94-PLAN3: record the fresh score in the cache.
                {
                    std::lock_guard<std::mutex> sclock(g_scoreCacheMutex);
                    auto& entry = g_scoreCache[tid];
                    entry.lastDecision = d;
                    entry.cachedAtMs = nowDecisionMs;
                    if (entry.lastSwitchMs == 0) entry.lastSwitchMs = nowDecisionMs;
                }
                if (d.action == DecisionAction::MIGRATE) {
                    int fromCpu = ti.currentCpu;
                    // v4.2 Batch B3 (F-1): epsilon-greedy counterfactual exploration.
                    // g_cfg.mlEpsilonGreedy defaults 0.0 -> maybeExplore() always
                    // returns false, so the v4.1.1 applyPlacement path below runs
                    // untouched (bit-identical). Only when ε>0 do we occasionally
                    // SUPPRESS a boost on an edge decision as a control sample.
                    bool b3Suppress = false;
                    // MAIN-06 fix: use loopCfg.mlEpsilonGreedy (per-loop snapshot)
                    // instead of reading g_cfg directly (inotify hot-reload).
                    if (loopCfg.mlEpsilonGreedy > 0.0) {
                        g_mlModel.setEpsilonGreedy(loopCfg.mlEpsilonGreedy);
                        g_mlModel.setExploreJankBlocked(aatpe::frameaware::jankBoostVoteActive());
                        b3Suppress = g_mlModel.maybeExplore(tid, d.confidence * 0.01f);
                    }
                    if (b3Suppress) {
                        // Control tick: deliberately keep the thread on its current
                        // CPU (do NOT migrate). Record a control pending so the
                        // outcome loop measures the not-boosted outcome and labels it
                        // with the paired-difference transform.
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
                        continue;   // do not migrate this tick
                    }
                    // P0-1: pass the per-iteration loopCfg (already snapshotted at
                    // loop top) so applyPlacement no longer re-snapshots Config.
                    bool placeOk = applyPlacement(ti, d.targetCpu, d.reason, loopCfg);
                    if (placeOk && d.mlActive) {
                        g_mlPendingCreated++;  // fix104 v34: count pending decision creation
                        // LED-011 (R1-011): gather all per-thread inputs OUTSIDE
                        // g_mlMutex — the blocking /proc/<pid>/task/<tid>/schedstat
                        // open/read, the frame-time snapshot and the g_cpuMetrics
                        // lookups. Only g_mlModel.recordPendingDecision() needs the
                        // lock; previously the /proc read held g_mlMutex open,
                        // lengthening ML lock hold time for webui model.json readers.
                        int beforeFreq = 0;
                        // v6.0.0 RT-A A1: take g_cpuMetricsMutex for the lock-free
                        // find() (was racing the collection writer = UB). This block
                        // is already outside g_mlMutex (LED-011), so the short lock
                        // here nests nothing.
                        {
                            std::lock_guard<std::mutex> cmLock(g_cpuMetricsMutex);
                            auto bfmit = g_cpuMetrics.find(ti.currentCpu);
                            if (bfmit != g_cpuMetrics.end()) beforeFreq = bfmit->second.curFreq;
                        }
                        int poolIdx = (int)d.targetPool;
                        if (poolIdx < 0) poolIdx = (int)getPoolForCpu(d.targetCpu);
                        // v66-fix (ML funnel): read fresh schedstat for before_run_time
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
                        // v66-issue16: Capture frame-time before snapshot for UI/Render/Game threads.
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
                        // C-002: use .find() under g_cpuMetricsMutex (was count()?operator[]
                        // which is both a TOCTOU race and operator[] can implicitly insert).
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
                // v4.5.0 observe-only (passive) shadow sampling.
                // In passive mode decidePlacement returns KEEP/PASSIVE, so no migration
                // is applied (control semantics unchanged: no boost/freeze/sysfs write).
                // When shadow sampling is on, we STILL bookkeeping-record a shadow pending
                // for a throttled sample of critical threads so the existing outcome loop
                // measures their ACTUAL run-ratio into the separate SHADOW buffer. This
                // never calls applyPlacement and never trains the live model.
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
                        s_shadowBudgetLeft = 2;  // at most 2 shadow pendings per 2s window
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
                        // v6.0.0 P1 fix: in passive mode decidePlacement() hits the
                        // PASSIVE gate and returns KEEP early (placement.cpp), so the
                        // sole d.mlFeatures assignment is NEVER reached — d.mlFeatures
                        // was the value-initialized all-zero ThreadFeatures. Every
                        // shadow sample recorded an all-zero feature vector, making the
                        // shadow ring a useless dataset. Build the counterfactual
                        // observation ("stay in current pool") with the SAME pure
                        // extractor the formal scoring loop uses. extractMlFeatures is
                        // strictly read-only: g_cpuMetrics short lock + ti fields +
                        // migration-count lookup; it performs NO boost/freeze/
                        // sched_setaffinity/sysfs write, so observe-only control
                        // semantics are unchanged. targetCpu = ti.currentCpu, aligned
                        // with shadowPool.
                        aatpe_ml::ThreadFeatures shadowFeats = extractMlFeatures(ti, ti.currentCpu);
                        // Reuse the formal cold-start all-zero filter: if the extracted
                        // vector is all-zero (metrics not ready yet at cold start), do
                        // NOT bookkeep a shadow pending — recording it would poison the
                        // shadow dataset with all-zero samples. Budget is NOT consumed
                        // in that case; the 2s window throttle retries once metrics are
                        // populated.
                        if (aatpe_ml::LearningModel::isAllZeroFeatures(shadowFeats)) {
                            // cold start: skip this round, no pending, no budget spend.
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
            // v94 fix29: TID-reuse double-check. readThreadCurrentCpu() runs here
            // AFTER the decision loop. Between then and now the OS thread
            // may have exited and its TID recycled by an unrelated thread, which
            // would make the /proc read below report a DIFFERENT thread's CPU and
            // poison the placement-outcome history. Re-acquire the lock and confirm
            // the entry is still present with the SAME starttime captured at decision
            // time. If it is gone or starttime mismatches, skip this outcome.
            bool tidStillValid = false;
            {
                std::lock_guard<std::mutex> tlock(g_threadsMutex);
                auto it = g_threads.find(mr.tid);
                tidStillValid = (it != g_threads.end() && it->second.starttime == mr.starttime);
            }
            if (!tidStillValid) {
                // Thread exited / TID reused: do not record a bogus outcome.
                continue;
            }
            int currentCpu = readThreadCurrentCpu(mr.pid, mr.tid);
            bool samePool = (getPoolForCpu(currentCpu) == getPoolForCpu(mr.targetCpu))
                          && (getPoolForCpu(currentCpu) != CpuPool::UNKNOWN);
            bool good = mr.placeOk && (samePool || currentCpu == mr.targetCpu);
            recordPlacementResult(mr.tid, mr.targetCpu, good);
        }
        protectCriticalThreads();

        // fix21: ML model periodic save (every LEARNING_SAVE_INTERVAL seconds)
        // fix39: Log actual path + byte count, check errno on failure, update state files
        time_t now2 = time(nullptr);
        if (loopCfg.mlLearningEnabled && (now2 - g_mlLastSave) >= loopCfg.mlSaveInterval) {
            const char* mp = mlModelPath();
            // LED-011 (R1-011): serialize g_mlModel.save() (and snapshot the log
            // fields) under g_mlMutex, but run backupModelFile() — a full-file
            // read+write+fsync+rename of model.dat — OUTSIDE the lock. backupModelFile
            // only copies the just-written on-disk model.dat and never touches ML
            // state; holding g_mlMutex across that disk IO blocked webui model.json
            // readers on every periodic save.
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
                // #39: persist the topology hash sidecar alongside model.dat so a
                // later load on THIS device recognizes the model as compatible.
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
        // fix104 P1-1 batch 4: Periodic A/B test winner determination (every 60s)
        {
            static time_t s_lastAbWinnerCheck = 0;
            if (now2 - s_lastAbWinnerCheck >= 60) {
                int winner = abTestDetermineWinner();
                if (winner >= 0) {
                    writeFile((std::string(STATE_DIR) + "/ab_test_winner").c_str(), winner);
                    LOG_INFO("A/B test winner: group %d (0=A, 1=B)", winner);
                }
                // LED-100 (R7-015): state.json exposes g_abGroupAAvg/g_abGroupBAvg/
                // g_abPValue but they were written nowhere, so state.json always
                // showed 0.000/0.000/1.0000. Mirror the live AB counters here (same
                // direct-access pattern as the ab_test_group_a_avg/b_avg writes just
                // below; both producer and consumer run on the main loop). When a
                // winner exists p<0.05 by definition, else p is non-significant.
                // MAIN-07 fix: aatped_state.h:39-42 declares g_ab* as guarded by
                // g_stateDiagMutex (writeStateJson reads them under that lock).
                // Take the same leaf lock on the write side so the contract is
                // symmetric; currently same-thread, but the lock must not be
                // re-opened if these stats ever migrate off the main loop.
                {
                    std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
                    g_abGroupAAvg = g_abStats.avgFrameTimeA;
                    g_abGroupBAvg = g_abStats.avgFrameTimeB;
                    g_abPValue = (winner >= 0) ? 0.0f : 1.0f;
                }
                s_lastAbWinnerCheck = now2;
            }
        }
        loopCount++; // callCount % 10 lazy deletion of old state entries
        if (loopCount >= daemon::kHistoryDecayLoopCount) {
            decayHistory();
            loopCount = 0;
        }
        // fix24: cumulative counters (decision_count, migration_count) and all
        // JSON writes moved BEFORE checkSystemLoad() so they survive the
        // `continue` on high-load cycles. Only per-cycle counters remain here.
        // P2 Problem8: per-cycle/diagnostic counters + cpu_topology gated on
        // diagTick (1s). They are monotonic or slowly-varying; WebUI polls at
        // 2-5s so 1s staleness is invisible. Config reload below stays every loop.
        if (diagTick) {
        writeFile((std::string(STATE_DIR) + "/decision_count_current").c_str(), decisionCount);
        writeFile((std::string(STATE_DIR) + "/migration_rate_current").c_str(), g_migrationCount);
        writeFile((std::string(STATE_DIR) + "/loop_count").c_str(), loopCount);
        // fix25: write high-load degraded cycle counter for diagnostics
        writeFile((std::string(STATE_DIR) + "/high_load_cycles").c_str(), (int)g_highLoadCycles.load());
        // fix29: KEEP reason diagnostic counters (why migrations stop)
        writeFile((std::string(STATE_DIR) + "/keep_protected").c_str(), (int)g_keepProtected.load());
        writeFile((std::string(STATE_DIR) + "/keep_passive").c_str(), (int)g_keepPassive.load());
        writeFile((std::string(STATE_DIR) + "/keep_no_cpu").c_str(), (int)g_keepNoCpu.load());
        writeFile((std::string(STATE_DIR) + "/keep_low_conf").c_str(), (int)g_keepLowConf.load());
        writeFile((std::string(STATE_DIR) + "/keep_high_cost").c_str(), (int)g_keepHighCost.load());
        writeFile((std::string(STATE_DIR) + "/keep_already_optimal").c_str(), (int)g_keepAlreadyOptimal.load());
        // 核心问题3: migrate_reject_* per-reason counters. Every reason an
        // ordinary migration decision was rejected gets its own state file, plus a
        // migration_rejected total. These were previously counted internally but
        // never exposed on disk, so on-device diagnosis showed empty reject files.
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
            // v6.0.0 P1: expose the most recent CPUSET_SKIP sample (tid/pid/targetCpu/
            // maskCount/pname) as a compact state file so the panel can show WHICH
            // thread was refused a big-core migration because it was parked in a
            // narrower cpuset (the realdevice "locked little core" diagnosis).
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
        // fix29: applyPlacement diagnostic counters
        writeFile((std::string(STATE_DIR) + "/apply_success").c_str(), (int)g_applySuccess.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed").c_str(), (int)g_applyFailed.load());
        // P1-18: sched_setaffinity error code classification state files
        writeFile((std::string(STATE_DIR) + "/apply_failed_einval").c_str(), (int)g_applyFailedEinval.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_esrch").c_str(), (int)g_applyFailedEsrch.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_eperm").c_str(), (int)g_applyFailedEperm.load());
        writeFile((std::string(STATE_DIR) + "/apply_failed_other").c_str(), (int)g_applyFailedOther.load());
        // P2: CPU topology state file (JSON format for WebUI JSON.parse)
        // C-004: hold g_topologyRwMutex while reading g_littlePool/g_midPool/g_bigPool.
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
            // P1-1: emit the real PRIME pool (was hardcoded []). Derive from
            // clusters() while we already hold g_topologyRwMutex, matching
            // aatped_state.cpp snapshot.json so cpu_topology and snapshot.json agree.
            std::vector<int> primeSnap;
            for (const auto& cl : clusters()) {
                if (cl.tier == CpuTier::PRIME) primeSnap = cl.cpus;
            }
            topoJson += "],\"prime\":[";
            for (size_t i = 0; i < primeSnap.size(); i++) { if (i) topoJson += ","; topoJson += std::to_string(primeSnap[i]); }
            topoJson += "]}";
            writeFile((std::string(STATE_DIR) + "/cpu_topology").c_str(), topoJson);
        }
        // ROUND-004: G5 spillover promotion counters (WebUI topology panel).
        // prime_spillover_count = cumulative PRIME-hosted promotions;
        // prime_hosted_threads = current number of threads promoted to PRIME.
        writeFile((std::string(STATE_DIR) + "/prime_spillover_count").c_str(),
                  (long long)aatpe::placement::spilloverPromotionCount());
        writeFile((std::string(STATE_DIR) + "/prime_hosted_threads").c_str(),
                  aatpe::placement::spilloverActiveCount());
        // NOTE: event_uevent_active is written by EventMonitor::writeStateFiles()
        // with real UeventMonitor::isActive() value. Do NOT overwrite here.
        writeFile((std::string(STATE_DIR) + "/apply_rate_limited").c_str(), (int)g_applyRateLimited.load());
        writeFile((std::string(STATE_DIR) + "/apply_passive").c_str(), (int)g_applyPassive.load());
        writeFile((std::string(STATE_DIR) + "/apply_cooldown_skip").c_str(), (int)g_applyCooldownSkip.load());
        writeFile((std::string(STATE_DIR) + "/apply_cpuset_skip").c_str(), (int)g_applyCpusetSkip.load());
        writeFile((std::string(STATE_DIR) + "/apply_interval_skip").c_str(), (int)g_applyIntervalSkip.load());
        } // P2 Problem8: end diagTick block A
        // fix96: Check config file inotify events and reload if requested
        // fix96: config inotify runs in dedicated thread (fix93), no need to poll here
        if (g_reloadConfigRequested.load()) {
            // fix100 P1: Actually perform config hot-reload (was empty no-op)
            g_reloadConfigRequested.store(false);
            reloadConfig();
        }
        // v71: mtime-based config hot-reload (complements inotify, ~10s poll)
        reloadConfigIfChanged();

        // fix38: new diagnostic counters
        // P2 Problem8: gated on diagTick (1s) with the other diagnostic counters.
        if (diagTick) {
        writeFile((std::string(STATE_DIR) + "/apply_kernel_skip").c_str(), (int)g_applyKernelSkip.load());
        writeFile((std::string(STATE_DIR) + "/apply_unmigratable_skip").c_str(), (int)g_applyUnmigratableSkip.load());
        {
            std::lock_guard<std::mutex> lock(g_unmigratableMutex);
            writeFile((std::string(STATE_DIR) + "/unmigratable_count").c_str(), (int)g_unmigratableThreads.size());
        }
        } // P2 Problem8: end diagTick block B

        // round-5: self-observation + safe-mode lifecycle (1Hz).
        if (diagTick) {
            using namespace aatpe::subsys;
            selfobs::sampleSelf();
            bool throttled = selfobs::evaluateThrottle();
            // A sustained self-overload itself is a safe-mode trigger.
            if (throttled) safemode::trigger(safemode::SELF_OVERLOAD, "self cpu over budget");
            // Thermal emergency -> safe mode latch (hard trigger).
            if (g_thermalEmergency.load()) {
                safemode::trigger(safemode::THERMAL_EMERG, "thermal emergency");
            } else {
                // fix: temperature recovered — drop the THERMAL_EMERG
                // latch bit so safe_mode can release. Previously this bit was set
                // but never cleared, leaving safe_mode latched at NORMAL and
                // rejecting ~984 migrations.
                safemode::clearReason(safemode::THERMAL_EMERG);
            }
            // Crash loop detected by watchdog -> latch monitor-only.
            {
                int cl = readInt((std::string(STATE_DIR) + "/crash_loop_detected").c_str(), 0);
                if (cl == 1) safemode::trigger(safemode::CRASH_LOOP, "watchdog crash loop");
            }
            safemode::maybeRecover(std::time(nullptr));
            // Write all derived state files.
            Registry::instance().writeStateFile(STATE_DIR);
            safemode::writeStateFiles(STATE_DIR);
            selfobs::writeStateFiles(STATE_DIR);
            (void)throttled;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loopStart).count();
        // fix96: Adaptive decision interval based on system load, jank, and foreground.
        // High load >80% -> 50ms, medium 40-80% -> 100ms, idle no-fg -> 500ms,
        // jank detected -> forced 50ms. Replaces fixed highLoad*2 logic.
        std::string fgApp = getCachedForegroundApp();
        bool hasFg = !fgApp.empty();
        // LED-006: jank state + system load are the per-loop snapshots taken after
        // readCpuMetrics(); reuse them instead of re-opening both state files here.
        bool jankNow = jankActiveNow;
        // System load: reuse the per-loop snapshot (written by readCpuMetrics above).
        int sysLoadInt = systemLoadNow;
        float sysLoad = (float)sysLoadInt;
        int baseInterval = computeAdaptiveInterval(sysLoad, jankNow, hasFg, loopCfg.decisionInterval);
        // 问题4 fix: per-app decisionIntervalMs override. When the foreground
        // package has a [app:pkg] section with decision_interval=N, use it as the
        // base interval instead of the global config. This lets a heavy game get
        // faster placement decisions while a background app keeps the relaxed
        // default. g_currentAppConfig is refreshed by checkAndApplyPerAppConfig()
        // earlier in the same loop iteration.
        if (g_currentAppConfig.valid && g_currentAppConfig.decisionIntervalMs > 0) {
            baseInterval = g_currentAppConfig.decisionIntervalMs;
        }
        // EMERGENCY-opt: adaptive decision cadence by system state.
        //   NORMAL: keep the computed 1-2ms adaptive interval (unchanged).
        //   DEGRADED: slow to 10ms (less CPU, ring gets drained).
        //   EMERGENCY: slow to 80ms (within 50-100ms) — regular placement is
        //     skipped anyway, so we only need a light heartbeat; event-triggered
        //     emergency migration still fires immediately via the event path.
        // low-overhead: if the HOST is catastrophically overloaded
        // (loadavg > 3.0 on this 8-core box = ~375%, or kswapd thrashing), drop to
        // 150ms regardless of state. aatped must not fight the VMOS/game/kswapd
        // storm for CPU.
        {
            SystemState cadenceSt = getSystemState();
            double ld1 = 0.0;
            getloadavg(&ld1, 1);
            int numCores = g_cpuCount > 0 ? g_cpuCount : 8;
            bool hostOverloaded = (ld1 > (double)numCores * 3.0);  // >300% normalized
            // fix (round 8): low-overhead mode (which SKIPS regular
            // placement and sleeps on sleep_for) must ONLY engage on thermal
            // EMERGENCY. It must NOT be gated on host loadavg — a VMOS+game
            // scenario keeps loadavg at 38+ forever, which previously latched
            // low_overhead on permanently (never < 2x cores), freezing
            // decision_count / migration_count at NORMAL thermal. Host overload
            // now only LENGTHENS the decision interval (see below); it never
            // suppresses placement, because those high-load scenarios are exactly
            // when scheduling matters most.
            bool lowOH = (cadenceSt == SystemState::EMERGENCY);
            g_lowOverheadMode.store(lowOH, std::memory_order_relaxed);
            if (cadenceSt == SystemState::EMERGENCY) {
                baseInterval = 80;
            } else if (hostOverloaded) {
                // High host load: slow down to ~60ms but STILL decide each cycle.
                if (baseInterval < 60) baseInterval = 60;
            } else if (cadenceSt == SystemState::DEGRADED) {
                if (baseInterval < 10) baseInterval = 10;
            }
        }
        // fix104 P1-1 batch 5: GPU bottleneck detection — when GPU-bound and CPU
        // is idle, reduce decision frequency to avoid unnecessary CPU overhead.
        if (isGpuBottleneck(sysLoad)) {
            int gpuInterval = getGpuBottleneckDecisionInterval();
            if (gpuInterval > baseInterval) baseInterval = gpuInterval;
        }
        // P2 Problem14: expose the post-adaptive/post-GPU-bottleneck effective
        // decision interval for state.json (requested=cfgSnap.decisionInterval).
        // MAIN-05 fix: aatped_state.h:31-34 declares g_runtimeDecisionIntervalMs
        // as guarded by g_stateDiagMutex; the read side in writeStateJson()
        // already takes that lock. Take it here on the write side so the lock
        // contract is symmetric (currently same-thread, but the contract must
        // not be re-opened if the adaptive cadence ever moves off the main loop).
        {
            std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
            g_runtimeDecisionIntervalMs = baseInterval;
        }
        int sleepMs = baseInterval - (int)elapsed;
        // minimum rest after long work cycles.
        // When the loop itself took most/all of the interval (e.g., 980ms of a
        // 1000ms budget on a thermally throttled CPU), the old code clamped to
        // kMinSleepMs(20ms) — the loop immediately starts another heavy cycle,
        // yielding ~98% duty cycle = 100% main-thread CPU. This starves the
        // eBPF poll thread → ring buffer overflow → ring_lost storm (15998/78s).
        // Fix: if work consumed >= 50% of the interval, enforce a minimum REST
        // period (not just the busy-wait guard). Under EMERGENCY/DEGRADED, rest
        // longer so the poll thread gets scheduled and drains the ring.
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

        // fix104 EBPF-FIX (updated by P2 Problem8): eBPF/HMAC/GPU/Audio/Scene/AB/
        // battery/ml_cold_start are WebUI DISPLAY files. They used to be rewritten
        // EVERY loop; now gated on diagTick (1s). They are NOT on-change critical
        // state (control_mode/current_profile/thermal_emergency are), so 1s
        // staleness is invisible to the 2-5s WebUI poll and cuts ~18 writes/loop.
        if (diagTick) {
            writeFile((std::string(STATE_DIR) + "/ebpf_active").c_str(), g_ebpfActive.load(std::memory_order_acquire) ? "1" : "0");
            // P1-2 fix: snapshot g_ebpfLoader under g_ebpfInfoMutex before writing
            // the state file (eBPF poll thread may be mutating it concurrently).
            std::string ebpfLoaderSnap;
            {
                std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
                ebpfLoaderSnap = g_ebpfLoader;
            }
            writeFile((std::string(STATE_DIR) + "/ebpf_loader").c_str(), ebpfLoaderSnap.c_str());
            // v94 fix / 问题51: expose the raw eBPF state as the failure reason.
            // getStateString() yields ACTIVE / DEGRADED / LOAD_TIMEOUT /
            // LOAD_FAILED / UNINITIALIZED — together with ebpf_mode
            // (active/event_driven/polling/hybrid) this tells WebUI whether we
            // fell back and why. The loader already logs each failed step at WARN.
            // ROUND-014 P3: ebpf_state uses seven-state accessor (G5).
            writeFile((std::string(STATE_DIR) + "/ebpf_state").c_str(),
                      g_ebpfScheduler.getEbpfStateSevenString());
            // detailed state string — "ACTIVE_PARTIAL" when some
            // tracepoints are degraded but eBPF is otherwise active.
            writeFile((std::string(STATE_DIR) + "/ebpf_state_detailed").c_str(),
                      g_ebpfScheduler.getDetailedStateString());
            // round-4 (#12): canonical degraded-reason enum derived from
            // the existing state + ring-loss signals. WebUI shows this directly.
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
            // current ring buffer page count (dynamic sizing)
            writeFile((std::string(STATE_DIR) + "/ebpf_ring_pages").c_str(),
                      g_ebpfScheduler.getCurrentRingPages());
            // v73: comprehensive eBPF event-driven state files
            // P1-2 fix: snapshot g_ebpfMode under g_ebpfInfoMutex (eBPF poll
            // thread degrades/recovers it concurrently).
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
            // v91 fix(P1): ebpf_ring_lost now reports the current 60s window
            // lost count (getWindowLost) instead of cumulative getRingLost,
            // so WebUI loss rate reflects recent activity. Cumulative counter
            // is retained for diagnostics only.
            writeFile((std::string(STATE_DIR) + "/ebpf_ring_lost").c_str(),
                      (long long)g_ebpfScheduler.getWindowLost());
            // v91 fix(P1): new state file — events processed in current window
            writeFile((std::string(STATE_DIR) + "/ebpf_window_events").c_str(),
                      (long long)g_ebpfScheduler.getWindowEvents());
            {
                char rateBuf[32];
                snprintf(rateBuf, sizeof(rateBuf), "%.1f", g_ebpfScheduler.getEventRatePerSec());
                writeFile((std::string(STATE_DIR) + "/ebpf_event_rate").c_str(), rateBuf);
            }
            // v94 P1-2/P1-5: bounded queue back-pressure counters. queue_dropped
            // counts events the poll thread had to drop because the daemon main
            // loop drained the queue too slowly (oldest-first eviction). Together
            // with ebpf_ring_lost (kernel ring overflow) this gives a complete
            // end-to-end loss picture. queue_size is the live queue depth.
            writeFile((std::string(STATE_DIR) + "/ebpf_queue_dropped").c_str(),
                      (long long)g_ebpfScheduler.getQueueDropped());
            writeFile((std::string(STATE_DIR) + "/ebpf_queue_size").c_str(),
                      (long long)g_ebpfScheduler.getQueueSize());
            // v94 fix / 问题46: consolidated eBPF event stats. fallback_count is
            // how often the loader stepped down a mode (or entered polling);
            // ring_overflow_cumulative is the lifetime kernel-ring loss (the
            // rolling-window value is ebpf_ring_lost above). Pulled in one call
            // from getEventStats() so the counters stay consistent.
            {
                auto st = g_ebpfScheduler.getEventStats();
                writeFile((std::string(STATE_DIR) + "/ebpf_fallback_count").c_str(),
                          (long long)st.fallbackCount);
                writeFile((std::string(STATE_DIR) + "/ebpf_ring_overflow_cumulative").c_str(),
                          (long long)st.ringOverflow);
                writeFile((std::string(STATE_DIR) + "/ebpf_events_exec").c_str(),
                          (long long)g_ebpfScheduler.getExecEvents());
            }
            // v94-r3 fix: events dropped by deliver*Event() because their
            // tracepoint's hardcoded ctx offsets were actively detected as
            // wrong on this kernel. Non-zero means the user should look at
            // ebpf_offset_status_{switch,fork,exec,exit} to see which
            // tracepoints are degraded.
            writeFile((std::string(STATE_DIR) + "/ebpf_offset_filtered").c_str(),
                      (long long)g_ebpfScheduler.getOffsetFiltered());
            // v94 P2 问题18: HMAC active/status single source of truth = g_hmacActive
            // and g_hmacKeyStatus (set by initHmacKey/checkHmacKeyRotation).
            // Previously hmac_active was hardcoded to "1" and key_status was derived
            // from a separate g_hmacKeyLoaded/g_hmacKeyGeneratedAt heuristic, which
            // could disagree with state.json's "hmac.active" field. Both now read
            // the same runtime variables.
            {
                std::lock_guard<std::mutex> hmacLk(g_hmacMutex);
                writeFile((std::string(STATE_DIR) + "/hmac_active").c_str(),
                          g_hmacActive ? "1" : "0");
                writeFile((std::string(STATE_DIR) + "/hmac_key_status").c_str(),
                          g_hmacKeyStatus);
            }
            // #61 / SEC-08: SOLE owner of STATE_DIR/hmac_session_active. We
            // recompute aatpeSessionAnyActive() fresh here (it takes
            // g_sessionMutex and lazily purges expired slots), so this file
            // always reflects current truth. The webui issue path no longer
            // writes this file itself (that was a second writer that could
            // race a stale "0" over a fresh "1"); it waits for this tick.
            writeFile((std::string(STATE_DIR) + "/hmac_session_active").c_str(),
                      aatpeSessionAnyActive() ? "1" : "0");
            // MAIN-04 fix: this flat publish block previously read
            // g_gpuState.utilization/type (int + std::string) and g_audioState.modeActive
            // / g_sceneState.currentScene with NO lock, bypassing the lock contract that
            // writeStateJson() observes (g_gpuDualMutex for GPU, g_stateDiagMutex for
            // audio/scene). Even though the writers (readGpuUtilization / detectAudioPlayback
            // / detectGameScene) currently run on this same main loop via loopAuxScan, the
            // contract must not be re-opened: readGpuUtilization() already publishes the
            // values through lock-free atomics (g_gpuUtilization / g_gpuType), and the
            // audio/scene structs are written under g_stateDiagMutex. Read from those
            // safe sources so a future move of readGpuUtilization to a sampling thread
            // cannot turn this into UAF/SIGSEGV (the P1-2 fix in state.cpp already did
            // this for the JSON path).
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
                // ML-01: current_game_scene is published ONLY here (single owner-writer);
                // the two detectors only update in-memory state, they do not write the file.
                writeFile((std::string(STATE_DIR) + "/current_game_scene").c_str(),
                          sceneToString((GameScene)sceneSnap));
            }
            writeFile((std::string(STATE_DIR) + "/ab_test_group_a_avg").c_str(), (int)g_abStats.avgFrameTimeA);
            writeFile((std::string(STATE_DIR) + "/ab_test_group_b_avg").c_str(), (int)g_abStats.avgFrameTimeB);
            writeFile((std::string(STATE_DIR) + "/ab_test_group_a_samples").c_str(), g_abStats.sampleCountA);
            writeFile((std::string(STATE_DIR) + "/ab_test_group_b_samples").c_str(), g_abStats.sampleCountB);
            // v66: Battery aging protection state for WebUI
            writeFile((std::string(STATE_DIR) + "/battery_aging_enabled").c_str(), loopCfg.batteryAgingProtection ? "1" : "0");
            writeFile((std::string(STATE_DIR) + "/battery_aged").c_str(), g_batteryAged ? "1" : "0");
            // v69: Write battery health / cycle count (was missing — WebUI always read default)
            // -1 = unknown (read failed); WebUI shows "--" instead of 100%
            writeFile((std::string(STATE_DIR) + "/battery_health").c_str(), g_batteryHealthPercent);
            writeFile((std::string(STATE_DIR) + "/battery_cycle_count").c_str(), g_batteryCycleCount);
            // v66: ML cold start protection state for WebUI
            writeFile((std::string(STATE_DIR) + "/ml_cold_start_enabled").c_str(), loopCfg.mlColdStartProtection ? "1" : "0");
        }

        // fix97 P3: Federated learning periodic tasks (P0 fix: moved outside if block)
        // G7: gate the whole per-cycle federated block on FEDERATED_ENABLED.
        // MAIN-06 fix: use loopCfg.federatedEnabled (per-loop snapshot) instead of
        // reading g_cfg directly (inotify hot-reload).
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
            // final-review fix (dead telemetry): g_federatedTrustedImports is
            // read by aatped_state.cpp writeStateJson(), but NOTHING in
            // production ever incremented it -> state JSON always published
            // "trusted_imports":0. The FederatedLearner stats are the single
            // source of truth (trustedImports is bumped ONLY for peer models
            // that pass the HMAC signature + replay checks inside
            // checkAndImportModels()). Mirror the absolute counters into the
            // exported globals immediately after each import/aggregation cycle.
            // (g_federatedRounds/Exports/Imports were equally dead; the state
            // JSON emits all four together, so they are mirrored consistently.)
            g_federatedRounds.store(fedStats.rounds, std::memory_order_relaxed);
            g_federatedExports.store(fedStats.exports, std::memory_order_relaxed);
            g_federatedImports.store(fedStats.imports, std::memory_order_relaxed);
            g_federatedTrustedImports = fedStats.trustedImports;
            // P2 Problem8: federated stats + ab p-value are slow-changing display
            // values; gate on diagTick. checkHmacKeyRotation() below stays every loop.
            if (diagTick) {
                writeFile((std::string(STATE_DIR) + "/federated_rounds").c_str(), fedStats.rounds);
                writeFile((std::string(STATE_DIR) + "/federated_exports").c_str(), fedStats.exports);
                writeFile((std::string(STATE_DIR) + "/federated_imports").c_str(), fedStats.imports);
                writeFile((std::string(STATE_DIR) + "/federated_trusted_imports").c_str(), (long long)fedStats.trustedImports);
                writeFile((std::string(STATE_DIR) + "/ab_test_p_value").c_str(), (float)g_abTestLatestP);
            }
            checkHmacKeyRotation();
        }

        // fix103 P1-6.3: Update daemon loop tracing (P0 fix: moved outside if block)
        auto loopEnd = std::chrono::steady_clock::now();
        long long loopUs = std::chrono::duration_cast<std::chrono::microseconds>(loopEnd - loopStart).count();
        traceUpdate(g_traceDaemonLoop, loopUs);

        // fix103 P1-6.1: Write structured state JSON every 10 loops
        // P0 fix: uses single loopCount (incremented earlier for decayHistory), duplicate removed
        // v94 P2 问题19: bump the state generation counter at the end of every
        // decision cycle so state.json / snapshot.json / webui *.json all embed
        // a monotonically increasing generation that WebUI can use to detect
        // whether data came from the same decision cycle.
        g_stateGeneration.fetch_add(1, std::memory_order_release);
        if (loopCount % 10 == 0) {
            writeStateJson();
        }

        // P2-2: if medium-priority event triggered, skip sleep for immediate full decision
        // fix84 P3-1: still enforce minimum sleep to prevent busy-wait

        // ====================================================================
        // v95-OPT1 (事件驱动主循环): 替换无条件 sleep_for(sleepMs)。
        // 旧逻辑: 每轮固定睡 sleepMs(默认200ms), 即使没有任何事件也要醒来跑一遍,
        //         空闲时 5 次/秒空转 -> CPU 占用高。
        // 新逻辑: poll() 同时等待
        //   (a) self-pipe read fd — eBPF 关键事件 hook / control inotify 写入即唤醒
        //   (b) timeout = min(adaptive interval, 1000ms) — 全局重平衡兜底 1s
        // 空闲时主循环阻塞在 poll() 上, CPU 占用≈0; 有事件立即唤醒(亚毫秒级)。
        // eBPF 不可用(polling 模式)时, self-pipe 仍由 control inotify 驱动,
        // 1s 超时兜底保证定时重平衡不丢失 — 天然退化, 无需特殊分支。
        // ====================================================================
        {
            // 退出短路: stop 命令/信号已置 g_shouldExit, 不再进入 poll 等待,
            // 直接退出 while 循环(否则会多等一个 poll timeout 周期)。
            if (g_shouldExit.load(std::memory_order_relaxed)) break;
            // configurable cadence. idleIntervalMs is the poll timeout
            // when no important event is pending (default 200ms); minIntervalMs is
            // the minimum gap between two full decisions after a wake (default 33ms).
            const int idleIntervalMs = loopCfg.decisionIdleIntervalMs;
            const int minIntervalMs  = loopCfg.decisionMinIntervalMs;
            int pollTimeoutMs = sleepMs;

            // ROUND-014 P3: dynamic tick three-state.
            // active (game foreground) → 200ms
            // idle (screen off / no foreground game) → 5000ms
            // event → immediately (skipSleep path below)
            // Check if screen is on and we have a foreground game.
            // For simplicity: if g_currentAppConfig indicates a game profile and
            // recent activity, use active mode. Otherwise idle mode.
            // This is a conservative implementation - we don't want to break
            // existing behavior, just extend the max idle timeout.
            {
                static int s_tickMode = 0;  // 0=active, 1=idle
                // Simple heuristic: if no recent decisions and no foreground game,
                // extend poll timeout to 5s for power saving.
                // We don't have a direct screen-on/off API here; use the
                // existing inactive detection (kInactiveMs = 10s no sched_switch).
                // If the system has been inactive for > 10s, we're idle.
                // For now, just extend the max cap from 1000ms to 5000ms when
                // we detect no foreground game.
                // MAIN-02 fix: g_threads is mutated by the eBPF poll thread under
                // g_threadsMutex. Reading .size() unlocked was a data race on the
                // container (UB; TSan-flagged). Snapshot under the lock.
                size_t liveThreadCount;
                {
                    std::lock_guard<std::mutex> tlock(g_threadsMutex);
                    liveThreadCount = g_threads.size();
                }
                // MAIN-08 fix: the magic number 10 is now named
                // daemon::kIdleTickMaxTrackedThreads (see aatpe-constants.h). We
                // ALSO require hasFg == false before declaring idle — otherwise a
                // foreground-active session on a lightly-threaded workload (e.g. a
                // simple UI) would be misclassified as idle and its poll timeout
                // stretched to 5s, delaying event response. The thread-count test
                // alone was over-eager.
                bool isIdle = (liveThreadCount < daemon::kIdleTickMaxTrackedThreads) &&
                              !hasFg;
                if (isIdle) {
                    pollTimeoutMs = std::max(pollTimeoutMs, daemon::kIdleTickPollTimeoutMs);
                    s_tickMode = 1;
                } else {
                    s_tickMode = 0;
                }
                // Write tick mode state key once per second.
                static time_t s_lastTickModeWrite = 0;
                time_t nowTickMode = time(nullptr);
                if (nowTickMode - s_lastTickModeWrite >= 1) {
                    writeFile((std::string(STATE_DIR) + "/tick_mode").c_str(),
                              s_tickMode == 0 ? "active" : "idle");
                    s_lastTickModeWrite = nowTickMode;
                }
            }

            // 全局重平衡兜底: 即使 adaptive interval 更长, 也最多睡 1s 醒来做一次
            // 轻量巡检(thermal/metrics/全局重平衡门控)。事件驱动模式下这一步开销小。
            // ROUND-014 P3: idle mode allows up to 5s (power saving).
            if (pollTimeoutMs > daemon::kIdleTickPollTimeoutMs) pollTimeoutMs = daemon::kIdleTickPollTimeoutMs;
            if (pollTimeoutMs < daemon::kMinSleepMs) pollTimeoutMs = daemon::kMinSleepMs;

            // 关键事件已到(旧 g_skipNextSleep 路径): 仍然走 poll(), 但 timeout 压到
            // kMinSleepMs 级, 等价于"立即跑下一轮"。poll() 若先被 pipe 唤醒则更快。
            bool skipSleep = g_skipNextSleep.exchange(false);
            if (skipSleep) {
                // CPU-FIX: throttle this log to once per second. It used
                // to fire on every 10ms poll wake during event storms, flooding
                // logcat and consuming measurable CPU in the logging path.
                static long long s_lastWakeLogMs = 0;
                long long nowWakeLogMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (s_lastWakeLogMs == 0 || (nowWakeLogMs - s_lastWakeLogMs) >= 1000) {
                    s_lastWakeLogMs = nowWakeLogMs;
                    LOG_INFO("EVENT: waking for immediate full decision (medium-priority event)");
                }
                pollTimeoutMs = daemon::kMinSleepMs;
            } else {
                // idle baseline. The adaptive sleepMs already shortens
                // the cadence under EMERGENCY/DEGRADED/GPU-bottleneck load (we keep
                // those faster values when they are < idleIntervalMs). When the
                // system is quiet (adaptive idle no-fg returns up to 500ms), clamp
                // the poll timeout to the configured idle interval (default 200ms)
                // so the loop does not sleep longer than the user-configured idle.
                if (pollTimeoutMs > idleIntervalMs) pollTimeoutMs = idleIntervalMs;
            }

            // set when poll() returns because the self-pipe / control
            // inotify became readable (an important event arrived), rather than
            // timing out. Drives the 33ms minimum-decision-interval throttle below.
            bool wokenByEvent = false;

            // critical-fix: in low-overhead mode (EMERGENCY / host
            // overloaded), NEVER enter poll() on the wakeup pipe / control
            // inotify. Under a VMOS+game+kswapd storm the lifecycle events flood
            // the pipe and/or the control inotify fires (IN_ATTRIB from WebUI
            // stat), so poll() returns immediately every iteration and the main
            // thread burns 97% CPU in do_sys_poll (state=R, no sleep). Instead:
            // drain any pending pipe/inotify bytes once, then sleep_for() the
            // full interval. Regular placement is skipped anyway; a queued
            // control command is picked up by the next ~150ms tick.
            if (g_lowOverheadMode.load(std::memory_order_relaxed)) {
                if (g_wakeupPipeReadFd >= 0) {
                    char drain[16];
                    // P3-11: EINTR on read() must not prematurely stop the drain
                    // (leftover pipe bytes would make poll() fire immediately next
                    // cycle). Retry on EINTR; any other error ends the loop.
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
                // poll 多个 fd: self-pipe(eBPF 关键事件) + control inotify(命令到达)。
                // 任一就绪即唤醒主循环; 都未就绪则阻塞到 timeout。
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
                // P2-3: EINTR (signal) re-polls silently. A real poll error
                // (EBADF/EFAULT) on the wakeup pipe would otherwise spin every
                // pollTimeoutMs; log it ONCE and keep the timeout-cadence loop.
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
                    // drain self-pipe (非阻塞, 循环读到 EAGAIN)
                    if (pfds[0].revents & POLLIN) {
                        char drain[16];
                        // P3-11: retry on EINTR so a signal mid-drain does not leave
                        // bytes pending (which would make poll() return immediately
                        // on the next iteration and spin the loop).
                        while (read(g_wakeupPipeReadFd, drain, sizeof(drain)) > 0 || errno == EINTR) {}
                    }
                    // drain control inotify (非阻塞, 丢弃事件体 — 主循环顶部
                    // processControlCommands() 实际读 /control 文件内容).
                    // P1-2: we now watch the directory, so filter by filename —
                    // only wake for 'control' / 'control_cmd' writes, not for
                    // every heartbeat/state file the daemon itself writes.
                    if (npfd > 1 && (pfds[1].revents & POLLIN)) {
                        char evbuf[512];
                        bool controlEvent = false;
                        ssize_t n;
                        // P3-11: continue the inotify drain on EINTR; a short read
                        // interrupted by a signal must not abandon pending events.
                        while ((n = read(g_controlInotifyFd, evbuf, sizeof(evbuf))) > 0 ||
                               (n < 0 && errno == EINTR)) {
                            // Walk all inotify_event structs in the buffer.
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
                // pipe 创建失败(极端情况): 退化回旧 sleep_for, 保证调度不死锁。
                std::this_thread::sleep_for(std::chrono::milliseconds(pollTimeoutMs));
            }

            // (33ms 节流): if an important event woke us but the last
            // full decision cycle started < minIntervalMs ago, sleep the remainder
            // before looping into the next decision. This coalesces touch-event
            // bursts / fork storms: events still queue in criticalQueue_ and are
            // drained on the next cycle, but the heavy placement re-scan cannot
            // fire more often than minIntervalMs (default 33ms).
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
    // concurrency fix: stop the config hot-reload thread BEFORE saving
    // ML/hybrid state. This prevents the inotify thread from triggering a
    // reloadConfig() while we're trying to save the model (which would take
    // g_cfgMutex / g_mlMutex / g_hybridMutex and possibly deadlock with our
    // shutdown locks). The thread is joined here — no detached-thread leak.
    stopConfigHotReload();
    // fix21: Save ML model on daemon exit
    // fix39: Check return value, log path+bytes or errno
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
    // v66 regression-fix #3: persist hybrid learner delta weights + enabled flag on exit
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

// ============================================================================
// CLI COMMANDS (fix: PID path, g_allCpus init for explain/benchmark)
// ============================================================================
void cmdStatus() {
    printf("AATPE %s - Adaptive Android Task Placement Engine\n\n", AATPE_VERSION);
    printf("Mode:        %s\n", readFile((std::string(STATE_DIR) + "/control_mode").c_str()).c_str());
    printf("Profile:     %s\n", readFile((std::string(STATE_DIR) + "/current_profile").c_str()).c_str());
    // fix: read PID from correct path (RUN_DIR/daemon.pid, not STATE_DIR/daemon_pid)
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
    // fix29: KEEP reason breakdown (diagnostics for frozen migration_count)
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
        auto& ti = _sb_item.second;  // v94 fix54: unused `tid` binding + duplicate (void)tid removed
        if (count++ >= 50) { printf("... (truncated)\n"); break; }
        printf("[%d] %d/%d %s:%s role=%s prio=%d protect=%d\n",
            count-1, ti.pid, ti.tid, ti.pname.c_str(), ti.tname.c_str(),
            role_names::getName((int)ti.role), ti.priority, ti.protect ? 1 : 0);
    }
}

// fix: real benchmark that calls computeScore
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

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    // P2-32: --version / -v / version prints the embedded version and exits
    // BEFORE any root-only init (logInit / /dev/aatpe / state dirs). This makes
    // `aatped --version` return vX.Y.Z cleanly on-device; the cross-build host
    // cannot exec aarch64 ELFs, so the release gate confirms the same string via
    // `strings` against this AATPE_VERSION macro.
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
        // LED-010 (R1-010): the daemon writes sysfs/procfs, binds cpusets, reads
        // per-task schedstat and long-running as root. Previously a non-root
        // --daemon run fell all the way through to sysfs/procfs write failures
        // before erroring. Fail fast with a clear message.
        if (geteuid() != 0) {
            fprintf(stderr, "AATPE: --daemon must be run as root (euid=%d)\n", (int)geteuid());
            return 1;
        }
        // G1-016: install the SIGTERM/SIGINT handler BEFORE daemon(). Previously
        // the handler was installed inside daemonInit(), leaving a window after
        // daemon() where SIGTERM took the default action and killed us without
        // g_shouldExit / crash_signal visibility. daemonInit() re-installs the
        // full handler table (SA_RESTART etc.); this is a minimal early guard.
        {
            struct sigaction saEarly;
            memset(&saEarly, 0, sizeof(saEarly));
            saEarly.sa_handler = signalHandler;
            sigemptyset(&saEarly.sa_mask);
            sigaction(SIGTERM, &saEarly, nullptr);
            sigaction(SIGINT, &saEarly, nullptr);
        }
        // D-06: Proper daemonize using daemon() function
        if (::daemon(0, 0) != 0) {
            fprintf(stderr, "AATPE: daemon() failed: %s\n", aatpeErrStr(errno));
            return 1;
        }
        // v74-kprobe FIX: Create RUN_DIR BEFORE logReinit(). On a fresh boot
        // /dev/aatpe may not exist yet; fopen() would fail with ENOENT and the
        // error would vanish into stderr (redirected to /dev/null by daemon()).
        mkdirIfNotExist(RUN_DIR, 0700); // v94-fix#13: root-only
        // v74-kprobe FIX: Re-open log file AFTER daemon(). On Android/Bionic,
        // daemon() closes all fds > 2, invalidating g_logFp opened by logInit().
        // Without this, ALL daemon logs (including eBPF init) silently disappear.
        logReinit();
        // Forced startup banner — bypasses log level check so it always appears
        LOG_FORCE("=== AATPE Daemon %s Starting ===", AATPE_VERSION);
        // LED-157 (SEC-004): every other init path creates STATE_DIR 0700 (root-only).
        // This --daemon path used 0755; because mkdirIfNotExist() re-chmods to the
        // requested mode on EEXIST, whichever path ran last decided the final mode,
        // so STATE_DIR could end up world-readable. Unify on 0700.
        mkdirIfNotExist(STATE_DIR, 0700);
        // A1-005: O_CLOEXEC so the lock fd is not inherited by any child process
        // spawned via aatpeExec (pm list packages, etc.). The parent still holds
        // the lock; this just keeps the fd table clean in long-lived children.
        int lockFd = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lockFd < 0) { LOG_ERROR("Cannot create lock file"); return 1; }
        if (lockf(lockFd, F_TLOCK, 0) < 0) {
            // P0 (double-instance tombstone fix): another healthy instance already
            // holds the lock. DO NOT return from main() / call exit() here — that
            // runs __cxa_finalize + static/atexit destructors, and on this build one
            // of those dtors throws during exit -> std::terminate -> SIGABRT, which
            // the watchdog then reads as a hard crash (crash_signal=SIGABRT) and
            // latches rapid_crash_loop, respawning us every ~129s (the observed
            // self-sustaining abort loop). _exit(0) skips atexit/static dtors and
            // never produces a tombstone. No HTTP bind / eBPF attach / side effect
            // has happened yet (lockf is the first gate after daemonize).
            LOG_INFO("aatped already running (lock held); second instance exiting quietly");
            close(lockFd);
            _exit(0);
        }
        // P0 (WebUI black-screen fix): bind HTTP EARLY — right after daemonize +
        // single-instance lock, BEFORE the (slow, multi-stage) daemonInit. This way
        // /health answers 200 even while eBPF/integrity/ML init is still running or
        // throws. Auth stays fail-closed: checkDaemonTokenAuth() returns false until
        // the daemon token exists, so /state/all and every /api/* return 401/403;
        // only the open /health + /version liveness probes answer. applyHttpServerState
        // is idempotent (g_httpServerRunning guard), so the later calls inside
        // daemonInit are no-ops and never double-bind.
        applyHttpServerState(true);
        // P0-crash-loop hardening (v4.2.3): NEVER let a subsystem init throw escape
        // to std::terminate -> SIGABRT. On an upgrade with leftover/corrupt v4.2.1
        // state/config/.rec/model files, a single uncaught std::stoi/out_of_range
        // used to kill the whole daemon on every boot -> watchdog crash_loop ->
        // HTTP never comes up. Now: isolate each stage, record last_crash_reason,
        // and STILL bring up HTTP/control + enter the loop in degraded mode.
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
            // Expose for WebUI/watchdog: why we came up degraded + a restart count.
            atomicWriteFile((std::string(STATE_DIR) + "/last_crash_reason").c_str(), startupCrash.c_str());
            // best-effort bump startup_crash_count (read-modify-write under lock-free;
            // a lost increment across a restart is acceptable telemetry).
            std::string cnt = readFile((std::string(STATE_DIR) + "/startup_crash_count").c_str());
            int n = 0; try { n = std::stoi(cnt); } catch (...) { n = 0; }
            atomicWriteFile((std::string(STATE_DIR) + "/startup_crash_count").c_str(), n + 1);
            // FORCE HTTP/control up even if daemonInit threw before its own
            // ensureHttpServerState() call — the user must still be able to see
            // status / unlock / recover, never a black screen.
            applyHttpServerState(true);
        } else {
            atomicWriteFile((std::string(STATE_DIR) + "/last_crash_reason").c_str(), "");
            // P0: clean startup — stamp boot_ok_at so the WebUI can tell "up and
            // healthy" from "still booting" (last_crash_reason cleared above).
            atomicWriteFile((std::string(STATE_DIR) + "/boot_ok_at").c_str(),
                            (long long)time(nullptr));
        }
        // P0: expose watchdog passive mode as a stable bool key for WebUI.
        atomicWriteFile((std::string(STATE_DIR) + "/passive_mode").c_str(),
                         g_watchdogPassiveLocked.load() ? "1" : "0");
    // fix84: Initialize event monitor (inotify multi-source listener)
    try {
    if (!aatpe::event::EventMonitor::instance().init()) {
        LOG_WARN("EventMonitor init failed, falling back to polling mode");
    }
        // P1-6: start touch immediate boost (best-effort; inert without touch hw).
        aatpe::InputMonitor::instance().start();
        // ROUND-011: start thermal sampling thread (decoupled sysfs reads).
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
        // unified shutdown sequence.
        // Order: stop producers first, then consumers, then close resources.
        // This ensures no thread touches shared state after cleanup begins.
        //
        //   1. EventMonitor — stops uevent/inotify event production (joins thread)
        //   2. Config hot-reload — stops inotify thread that writes g_cfg (joins)
        //   3. eBPF poll thread — stops ring pumping (joins thread, closes pipe)
        //   4. eBPF unload — detaches tracepoints, unmaps rings
        //   5. HTTP server — drains in-flight connections, joins accept loop
        //   6. Control inotify fd — close (wakeup mechanism for main loop, now exited)
        //   7. Wakeup self-pipe — close both ends
        //   8. Log flush — joins log thread, flushes buffer
        //   9. Lock/PID files — close and unlink
        //
        // Persistent state files in STATE_DIR are intentionally NOT removed
        // (they are the daemon's durable output for WebUI/CLI/watchdog).
        // round-5: flip every registered subsystem to STOPPING before we
        // begin joining workers, so the on-disk subsystem_health reflects the
        // orderly shutdown rather than a mid-flight ACTIVE.
        {
            using namespace aatpe::subsys;
            auto& reg = Registry::instance();
            for (auto& nm : reg.listNames()) reg.setState(nm, SubsystemState::STOPPING);
            reg.writeStateFile(STATE_DIR);
        }
        aatpe::event::EventMonitor::instance().shutdown();
        aatpe::InputMonitor::instance().stop();   // P1-6: release touch boost
        stopThermalSampling();  // ROUND-011: join thermal sampling thread
        // ROUND-014 P3: shutdown sequence (§2.4).
        // releaseAllSpilloverLocked: demote all spillover-promoted threads.
        // aatpeGpuBoostForceZero: force GPU boost to 0.
        aatpe::placement::releaseAllSpilloverLocked();
        aatpe::aatpeGpuBoostForceZero("shutdown");
        // audit4 F3: release PSI's group-uclamp holds (background cut / top-app
        // boost) and clear the placement seams. Must run BEFORE
        // restoreAllCgroupUclamp() so the shared ledger's refcount drops to zero and
        // the recorded original is written back; previously a clean shutdown (or a
        // watchdog SIGKILL) left the PSI group-uclamp cut/boost frozen on the node.
        aatpe::psi::shutdown();
        // ROUND-013: bulk-restore all cgroup uclamp floors on shutdown.
        aatpe_mech::restoreAllCgroupUclamp();
        // D-P3-2: clear all persisted recovery records on clean shutdown.
        // Boot restore (recoveryRestoreSchedOnBoot) only handles crash recovery.
        aatpe_recovery::recoveryClearAllPersisted();
        // LED-001 (R1-001, P1): refreshRateEventDriven defaults to true, so startup
        // spawns a JOINABLE background thread (refreshRateEventStart). This shutdown
        // sequence stopped every other producer but never called refreshRateEventStop(),
        // leaving the static std::thread joinable at main() return; its destructor then
        // calls std::terminate() -> SIGABRT, so `aatped stop` always ended in an
        // abnormal exit. Stop + join it now (internal no-op if never started).
        aatpe::frameaware::refreshRateEventStop();
        // LED-015 (R1-015): stopConfigHotReload() is intentionally NOT called here.
        // daemonLoop() already calls it (before the on-exit ML/hybrid save) to stop
        // the inotify reload thread and avoid a g_cfgMutex/g_mlMutex deadlock while
        // saving; that call joins the thread and clears g_configInotifyRunning.
        // Calling it again here was a redundant idempotent no-op; the single
        // load-bearing call inside daemonLoop's exit path is retained.
        g_ebpfScheduler.stopPollThread();
        g_ebpfScheduler.unload();
        aatpe::webui_http::stopHttpServer();
        // close control inotify fd and watch
        if (g_controlInotifyWd >= 0) {
            inotify_rm_watch(g_controlInotifyFd, g_controlInotifyWd);
            g_controlInotifyWd = -1;
        }
        if (g_controlInotifyFd >= 0) {
            close(g_controlInotifyFd);
            g_controlInotifyFd = -1;
        }
        // close wakeup self-pipe
        if (g_wakeupPipeReadFd >= 0) { close(g_wakeupPipeReadFd); g_wakeupPipeReadFd = -1; }
        if (g_wakeupPipeWriteFd >= 0) { close(g_wakeupPipeWriteFd); g_wakeupPipeWriteFd = -1; }
        // round-5: all workers joined, fds closed, logs flushed. Mark every
        // subsystem STOPPED and write the final health snapshot before exit.
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
        // v94-fix#12: keep the in-memory state cache coherent with the on-disk
        // deletion (daemon_pid is a flat STATE_DIR file mirrored by the cache).
        stateCacheNoteRemove((std::string(STATE_DIR) + "/daemon_pid").c_str());
        return 0;
    }
    // fix: --init-topology command (post-fs-data.sh calls this)
    if (cmd == "--init-topology") {
        // P1-fix (issue 24): require root. detectCpuTopology() reads/writes
        // /sys/devices/system/cpu/* and /dev/cpuset/* which are root-only;
        // running as non-root silently produces an empty topology.
        if (geteuid() != 0) {
            fprintf(stderr, "AATPE: --init-topology must be run as root (euid=%d)\n", (int)geteuid());
            return 1;
        }
        mkdirIfNotExist(RUN_DIR, 0700);    // v94-fix#13
        mkdirIfNotExist(STATE_DIR, 0700);  // v94-fix#13
        loadConfig();
        // fix55 (问题46): OEM capability discovery
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

    // ROUND-005: CLI `topology` -- print per-core evidence + final cluster split.
    // Mirrors STATE_DIR/topology_diagnostics for on-device回传.
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

    // #8: integrity verification CLI.
    //   --verify-integrity   recompute hash chain, compare to stored manifest,
    //                        write state/integrity_status, exit 0 on OK.
    //   --generate-manifest  force-regenerate manifest.sha256 on disk.
    // ROUND-002 (addendum): both accept an optional explicit module-root positional
    // arg, and honor AATPE_MODULE_ROOT. Priority: argv[2] > env > auto-probe.
    // During a Magisk/KSU upgrade the new tree is in modules_update while the old
    // one is still in modules/; without an explicit root we would anchor the OLD
    // tree and the swap-in would then fail verify. If an explicit root is given
    // but lacks system/bin we error out non-zero and DO NOT fall back to probing.
    auto applyIntegrityRootOverride = [&]() -> bool {
        std::string root;
        if (argc > 2 && argv[2] && argv[2][0]) root = argv[2];
        else if (const char* env = getenv("AATPE_MODULE_ROOT")) {
            if (env[0]) root = env;
        }
        if (root.empty()) return true; // auto-probe
        aatpe::integrity::setModuleRootOverride(root);
        // Validate: must contain system/bin, else refuse (no silent other-tree anchor).
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
    // fix: for threads/benchmark/explain, init CPU topology first
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
            // P1-2 (hardened): triple-identity verification before signaling.
            // The old code only did strstr(linkBuf, "aatped") — a substring match
            // that accepted "aatped_fake" etc. and had NO starttime check. We now
            // use the same unified verifyAatpedPid() as the watchdog and CLI:
            // kill(pid,0) + exact exe realpath == "/system/bin/aatped" + starttime
            // match against STATE_DIR/daemon_starttime.
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

// v68: hybridScore() — real hybrid learning prediction for four-way decision fusion.
// Returns normalized [0,1] score when mlHybridEnabled; 0.0f otherwise (disabled path).
float hybridScore(int cpuId, const ThreadInfo& ti) {
    Config cfg; snapshotConfig(cfg);
    if (!cfg.mlHybridEnabled) return 0.0f;
    auto& hybrid = aatpe_ml_hybrid::instance();
    if (!hybrid.isEnabled() || hybrid.getPoolCount() <= 0) return 0.0f;
    int poolIdx = (int)getPoolForCpu(cpuId);
    if (poolIdx < 0 || poolIdx >= hybrid.getPoolCount()) return 0.0f;
    // Build 15-feature proxy vector matching hybrid tick() training distribution.
    float hf[15] = {0};
    hf[0] = 1.0f;  // bias
    unsigned long long avgRunUsProxy = (ti.nrSwitches > 0) ?
        (unsigned long long)((double)ti.runTime / (double)ti.nrSwitches / 1000.0) : 0;
    hf[1] = std::min(1.0f, (float)avgRunUsProxy / 50000.0f);
    hf[2] = std::min(1.0f, (float)ti.nrSwitches / 1000.0f);
    {
        // v94 fix30 (TOCTOU): read the util value UNDER g_cpuMetricsMutex and
        // copy it into the local feature vector hf[3] BEFORE releasing the lock.
        // v94 fix (问题18): also read the per-CPU temp here so hf[7] matches the
        // training-time thermal feature delivered via cpuThermalNorm[] in tick().
        // All later feature construction (hf[12]=hf[1]*hf[3], hf[14], etc.) uses
        // only these local copies, so we never dereference g_cpuMetrics after the
        // lock is dropped. This is the "copy under lock, release, use local"
        // pattern; no g_cpuMetrics access occurs outside this critical section.
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
    // v94 fix (问题18): real scheduling features, matching tick() training distribution.
    hf[4] = std::min(1.0f, std::max(0.0f, (float)ti.priority / 100.0f));
    hf[5] = std::min(1.0f, std::max(0.0f, ((float)ti.nice + 20.0f) / 39.0f));
    hf[6] = (ti.role == ROLE_S3_GAME || ti.role == ROLE_S2_RENDER) ? 1.0f : 0.0f;
    // #F3-alignment: shared tier->feature mapping. Use the DISTINCT CLUSTER count
    // (not the model slot count) so 3-cluster boxes keep the historical branch.
    hf[8] = aatpe_ml_hybrid::poolFeatureForTier(poolIdx, mlDistinctClusterCount());
    hf[9] = (ti.currentCpu == cpuId) ? 1.0f : 0.0f;
    // TODO(问题18): migration_count is derived in tick() by diffing consecutive
    // snapshots; at serving time we do not have the previous-CPU history handy,
    // so keep it neutral (0.0). This feature's weight has low impact.
    hf[10] = 0.0f;
    hf[11] = hf[1] * hf[8];
    hf[12] = hf[1] * hf[3];
    hf[13] = hf[8] * hf[7];
    hf[14] = hf[1] * hf[7];
    return hybrid.predict(poolIdx, hf);
}
// v94 fix55: Removed 4 dead eBPF stub functions (ebpfLoadObject /
// ebpfAttachSchedSwitch / ebpfDetachAll / ebpfStateDegradation). They were
// empty no-op shells that always returned false; no production call site
// references them (verified by grep). Real eBPF setup/teardown is owned by
// g_ebpfScheduler. Declarations in aatped.h were removed alongside.


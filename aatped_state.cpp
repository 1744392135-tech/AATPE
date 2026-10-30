#include "aatped.h"
#include "placement.h"
std::string aatpe_buildFrameAppsJson();
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <deque>
#include <mutex>
#include <atomic>
#include <algorithm>

using namespace aatpe;

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char uc = (unsigned char)c;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uc < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", uc);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::atomic<int> g_systemState{(int)SystemState::NORMAL};
std::atomic<time_t> g_stateEnterTime{0};
char g_lastTransitionReason[128] = "initial_state";
std::atomic<int> g_degradedNormalCycles{0};
std::mutex g_stateMutex;
std::deque<StateHistoryEntry> g_stateHistory;
std::atomic<bool> g_watchdogPassiveLocked{false};
std::atomic<bool> g_userModeLock{false};
std::atomic<bool> g_emergencyUserOverride{false};

static std::atomic<long long> s_lastCommitMs{0};
static std::atomic<long long> s_dwellMsOverride{-1};
static std::atomic<long long> s_stateEnterSteadyMs{0};

#ifdef AATPE_HOST_TEST
extern "C" void aatpe_state_setDwellMsForTest(long long ms) {
    s_dwellMsOverride.store(ms, std::memory_order_release);
}
#include <string>
extern "C" void aatpe_state_jsonEscapeForTest(const char* in, char* out, int outCap) {
    if (!in || !out || outCap <= 0) { if (out && outCap>0) out[0]=0; return; }
    std::string e = jsonEscape(in);
    snprintf(out, outCap, "%s", e.c_str());
}
#endif

static inline long long effectiveDwellMs() {
    long long ov = s_dwellMsOverride.load(std::memory_order_acquire);
    return ov >= 0 ? ov : (long long)kStateDwellMs;
}

static inline long long nowSteadyMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
TracingStats g_traceDaemonLoop, g_traceDecidePlacement, g_traceComputeScore, g_traceApplyPlacement, g_traceEventResponse;
static std::mutex g_traceMutex;
std::mutex g_stateDiagMutex;
void traceUpdate(TracingStats& s, long long us) {
    std::lock_guard<std::mutex> lk(g_traceMutex);
    s.count++;
    s.sumUs += us;
    if (us < s.minUs) s.minUs = us;
    if (us > s.maxUs) s.maxUs = us;
    s.samples.push_back(us);
    if (s.samples.size() > 256) s.samples.pop_front();
}
static long long tracePercentile(const TracingStats& s, int pct) {
    std::lock_guard<std::mutex> lk(g_traceMutex);
    if (s.samples.empty()) return 0;
    std::vector<long long> sorted(s.samples.begin(), s.samples.end());
    std::sort(sorted.begin(), sorted.end());
    size_t idx = (size_t)((pct / 100.0) * (sorted.size() - 1));
    return sorted[idx];
}

struct TraceScalarSnap { long long count, minUs, maxUs; };
static TraceScalarSnap traceScalars(const TracingStats& s) {
    std::lock_guard<std::mutex> lk(g_traceMutex);
    return { s.count, s.minUs, s.maxUs };
}

void writeStateJson() {
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::string json = "{";
    json += "\"version\":\"" + std::string(AATPE_VERSION) + "\",";
    json += "\"version_code\":" + std::to_string(AATPE_VERSION_CODE) + ",";
    json += "\"release_id\":\"" + std::string(AATPE_RELEASE_ID) + "\",";
    json += "\"build_date\":\"" + std::string(AATPE_BUILD_DATE) + "\",";
    json += "\"timestamp\":" + std::to_string((long long)time(nullptr)) + ",";
    json += "\"generation\":" + std::to_string(gen) + ",";
    time_t start = g_startTime.load();
    long long uptimeSec = start > 0 ? (long long)(time(nullptr) - start) : 0;
    json += "\"uptime\":" + std::to_string(uptimeSec) + ",";
    int cpuCountSnap;
    {
        std::lock_guard<std::mutex> topoCpuLk(g_topologyRwMutex);
        cpuCountSnap = g_cpuCount;
    }
    json += "\"cpu_count\":" + std::to_string(cpuCountSnap) + ",";
    json += "\"control_mode\":\"" + cfgSnap.controlMode + "\",";
    {
        SystemState ss = getSystemState();
        json += "\"system_state\":{";
        json += "\"current\":\"" + std::string(systemStateToString(ss)) + "\",";
        json += "\"duration_sec\":" + std::to_string((long long)getStateDurationSec()) + ",";
        json += "\"last_transition_reason\":\"" + jsonEscape(std::string(getLastTransitionReason())) + "\",";
        json += "\"watchdog_passive_locked\":" + std::string(isWatchdogPassiveLocked() ? "true" : "false") + ",";
        json += "\"history\":[";
        std::vector<StateHistoryEntry> hist = getStateHistory();
        for (size_t i = 0; i < hist.size(); i++) {
            if (i > 0) json += ",";
            json += "{\"from\":\"" + std::string(systemStateToString(hist[i].fromState))
                 + "\",\"to\":\"" + std::string(systemStateToString(hist[i].toState))
                 + "\",\"timestamp\":" + std::to_string((long long)hist[i].timestamp)
                 + ",\"reason\":\"" + jsonEscape(std::string(hist[i].reason)) + "\"}";
        }
        json += "]";
        json += "},";
    }
    json += "\"current_profile\":\"" + aatpe_profile::effectiveProfile() + "\",";
    {
      std::lock_guard<std::mutex> fgLock(g_foregroundMutex);
      json += "\"foreground_app\":\"" + g_foregroundApp + "\",";
    }
    json += "\"decision_interval\":" + std::to_string(cfgSnap.decisionInterval) + ",";
    int runtimeIntervalSnap;
    {
        std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
        runtimeIntervalSnap = g_runtimeDecisionIntervalMs;
    }
    json += "\"decision_interval_runtime_ms\":" + std::to_string(runtimeIntervalSnap) + ",";
    json += "\"thermal\":{\"cpu\":" + std::to_string(g_thermalCpu.load(std::memory_order_relaxed)) + ",\"skin\":" + std::to_string(g_thermalSkin.load(std::memory_order_relaxed)) + ",\"battery\":" + std::to_string(g_thermalBattery.load(std::memory_order_relaxed)) + "},";
    json += "\"self_cpu_usage\":" + std::to_string(g_selfCpuUsagePercent.load(std::memory_order_relaxed)) + ",";
    {
        std::string gpuTypeSnap;
        int gpuUtilSnap;
        {
            std::lock_guard<std::mutex> gpuLk(g_gpuDualMutex);
            gpuTypeSnap = g_gpuState.type;
            gpuUtilSnap = g_gpuState.utilization;
        }
        json += "\"gpu\":{\"type\":\"" + gpuTypeSnap + "\",\"utilization\":" + std::to_string(gpuUtilSnap) + "},";
    }
    bool audioModeSnap; int audioCpuSnap; int sceneSnap;
    {
        std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
        audioModeSnap = g_audioState.modeActive;
        audioCpuSnap  = g_audioState.isolatedCpu;
        sceneSnap     = g_sceneState.currentScene;
    }
    json += "\"audio\":{\"mode_active\":" + std::string(audioModeSnap ? "true" : "false") + ",\"isolated_cpu\":" + std::to_string(audioCpuSnap) + "},";
    json += "\"game_scene\":\"" + std::string(sceneToString((GameScene)sceneSnap)) + "\",";
    {
        std::string ebpfLoaderSnap;
        {
            std::lock_guard<std::mutex> ebpfInfoLk(g_ebpfInfoMutex);
            ebpfLoaderSnap = g_ebpfLoader;
        }
        json += "\"ebpf\":{\"active\":" + std::string(g_ebpfActive.load(std::memory_order_acquire) ? "true" : "false") + ",\"loader\":\"" + ebpfLoaderSnap + "\"},";
    }
    {
        bool hmacActiveSnap;
        std::string hmacKeyStatusSnap;
        {
            std::lock_guard<std::mutex> hmacLk(g_hmacMutex);
            hmacActiveSnap = g_hmacActive;
            hmacKeyStatusSnap = g_hmacKeyStatus;
        }
        json += "\"hmac\":{\"active\":" + std::string(hmacActiveSnap ? "true" : "false") + ",\"key_status\":\"" + hmacKeyStatusSnap + "\"},";
    }
    int fedRounds, fedExports, fedImports, fedTrusted;
    float abA, abB, abP, avgMaeSnap;
    {
        std::lock_guard<std::mutex> diagLk(g_stateDiagMutex);
        fedRounds   = g_federatedRounds.load(std::memory_order_relaxed);
        fedExports  = g_federatedExports.load(std::memory_order_relaxed);
        fedImports  = g_federatedImports.load(std::memory_order_relaxed);
        fedTrusted  = g_federatedTrustedImports;
        abA         = g_abGroupAAvg;
        abB         = g_abGroupBAvg;
        abP         = g_abPValue;
        avgMaeSnap  = g_avgPredictionMAE;
    }
    json += "\"federated\":{\"rounds\":" + std::to_string(fedRounds) + ",\"exports\":" + std::to_string(fedExports) + ",\"imports\":" + std::to_string(fedImports) + ",\"trusted_imports\":" + std::to_string(fedTrusted) + "},";
    json += "\"ab_test\":{\"group_a_avg\":" + std::to_string(abA) + ",\"group_b_avg\":" + std::to_string(abB) + ",\"p_value\":" + std::to_string(abP) + "},";
    int mlWtDegraded = readInt((std::string(STATE_DIR)+"/ml_weight_degraded_count").c_str(), 0);
    int coldRejected = readInt((std::string(STATE_DIR)+"/cold_start_rejected").c_str(), 0);
    int sfThreadCnt  = readInt((std::string(STATE_DIR)+"/sf_thread_count").c_str(), 0);
    int stuneBoost   = readInt((std::string(STATE_DIR)+"/schedtune_boost_current").c_str(), 0);
    int autocalX1000 = readInt((std::string(STATE_DIR)+"/autocal_ratio").c_str(), 0);
    long long mlShadowCollected = readInt64((std::string(STATE_DIR)+"/ml_shadow_samples_collected").c_str(), 0);
    long long mlShadowDropped   = readInt64((std::string(STATE_DIR)+"/ml_shadow_samples_dropped").c_str(), 0);
    long long mlShadowValid     = readInt64((std::string(STATE_DIR)+"/ml_shadow_outcome_valid").c_str(), 0);
    int mlShadowOn        = readInt((std::string(STATE_DIR)+"/ml_shadow_sampling_enabled").c_str(), 0);
    char autocalBuf[32];
    snprintf(autocalBuf, sizeof(autocalBuf), "%.3f", (double)autocalX1000 / 1000.0);
    json += "\"ml\":{\"learning_enabled\":" + std::string(cfgSnap.mlLearningEnabled ? "true" : "false") + ",\"avg_mae\":" + std::to_string(avgMaeSnap)
          + ",\"weight_degraded_count\":" + std::to_string(mlWtDegraded)
          + ",\"shadow_sampling_enabled\":" + std::string(mlShadowOn ? "true" : "false")
          + ",\"shadow_outcome_valid\":" + std::to_string(mlShadowValid)
          + ",\"shadow_samples_collected\":" + std::to_string(mlShadowCollected)
          + ",\"shadow_samples_dropped\":" + std::to_string(mlShadowDropped) + "},";
    json += "\"coldstart\":{\"cold_start_rejected\":" + std::to_string(coldRejected) + "},";
    json += "\"mech\":{\"sf_thread_count\":" + std::to_string(sfThreadCnt)
          + ",\"schedtune_boost_current\":" + std::to_string(stuneBoost)
          + ",\"autocal_ratio\":" + std::string(autocalBuf) + "},";
    int preloadCountSnap;
    {
        std::lock_guard<std::mutex> alLk(g_appLaunchHistoryMutex);
        preloadCountSnap = (int)g_appLaunchHistory.size();
    }
    json += "\"preload_count\":" + std::to_string(preloadCountSnap) + ",";
    TraceScalarSnap tLoop = traceScalars(g_traceDaemonLoop);
    TraceScalarSnap tDecide = traceScalars(g_traceDecidePlacement);
    TraceScalarSnap tScore = traceScalars(g_traceComputeScore);
    TraceScalarSnap tApply = traceScalars(g_traceApplyPlacement);
    TraceScalarSnap tEvent = traceScalars(g_traceEventResponse);
    json += "\"tracing\":{";
    json += "\"daemon_loop\":{\"count\":" + std::to_string(tLoop.count) + ",\"p50\":" + std::to_string(tracePercentile(g_traceDaemonLoop, 50)) + ",\"p95\":" + std::to_string(tracePercentile(g_traceDaemonLoop, 95)) + ",\"p99\":" + std::to_string(tracePercentile(g_traceDaemonLoop, 99)) + "},";
    json += "\"decide_placement\":{\"count\":" + std::to_string(tDecide.count) + ",\"p50\":" + std::to_string(tracePercentile(g_traceDecidePlacement, 50)) + ",\"p95\":" + std::to_string(tracePercentile(g_traceDecidePlacement, 95)) + ",\"p99\":" + std::to_string(tracePercentile(g_traceDecidePlacement, 99)) + "},";
    json += "\"compute_score\":{\"count\":" + std::to_string(tScore.count) + ",\"p50\":" + std::to_string(tracePercentile(g_traceComputeScore, 50)) + ",\"p95\":" + std::to_string(tracePercentile(g_traceComputeScore, 95)) + ",\"p99\":" + std::to_string(tracePercentile(g_traceComputeScore, 99)) + "},";
    json += "\"apply_placement\":{\"count\":" + std::to_string(tApply.count) + ",\"p50\":" + std::to_string(tracePercentile(g_traceApplyPlacement, 50)) + ",\"p95\":" + std::to_string(tracePercentile(g_traceApplyPlacement, 95)) + ",\"p99\":" + std::to_string(tracePercentile(g_traceApplyPlacement, 99)) + "},";
    json += "\"event_response\":{\"count\":" + std::to_string(tEvent.count) + ",\"p50\":" + std::to_string(tracePercentile(g_traceEventResponse, 50)) + ",\"p95\":" + std::to_string(tracePercentile(g_traceEventResponse, 95)) + ",\"p99\":" + std::to_string(tracePercentile(g_traceEventResponse, 99)) + "}";
    json += "}";
    json += "}";
    if (!atomicWriteFile((std::string(STATE_DIR) + "/state.json").c_str(), json))
        LOG_WARN("writeStateJson: failed to atomically publish state.json (WebUI will see stale state)");
    if (!atomicWriteFile((std::string(STATE_DIR) + "/frame_apps.json").c_str(),
                    aatpe_buildFrameAppsJson()))
        LOG_DEBUG("writeStateJson: frame_apps.json publish failed (non-fatal)");
}

void writeSnapshotJson() {
    Config cfgSnap;
    snapshotConfig(cfgSnap);
    uint64_t gen = g_stateGeneration.load(std::memory_order_relaxed);
    std::string json = "{";
    json += "\"timestamp\":" + std::to_string((long long)time(nullptr)) + ",";
    json += "\"generation\":" + std::to_string(gen) + ",";
    {
        time_t s0 = g_startTime.load();
        long long u0 = s0 > 0 ? (long long)(time(nullptr) - s0) : 0;
        json += "\"uptime\":" + std::to_string(u0) + ",";
    }
    SystemState ss = getSystemState();
    json += "\"system_state\":\"" + std::string(systemStateToString(ss)) + "\",";
    json += "\"control_mode\":\"" + cfgSnap.controlMode + "\",";
    {
        std::string eff = (ss == SystemState::EMERGENCY) ? std::string("emergency") : cfgSnap.controlMode;
        json += "\"control_mode_effective\":\"" + eff + "\",";
    }
    json += "\"thermal_enabled\":" + std::string(cfgSnap.thermalEnabled ? "true" : "false") + ",";
    json += "\"prime_spillover_count\":" + std::to_string((long long)aatpe::placement::spilloverPromotionCount()) + ",";
    json += "\"prime_hosted_threads\":" + std::to_string(aatpe::placement::spilloverActiveCount()) + ",";
    {
        std::string sc = readFile((std::string(STATE_DIR) + "/scheduler_capabilities").c_str());
        json += "\"scheduler_capabilities\":\"" + jsonEscape(sc) + "\",";
    }
    {
        std::string td = readFile((std::string(STATE_DIR) + "/topology_diagnostics").c_str());
        std::string esc;
        esc.reserve(td.size() + 8);
        for (char c : td) {
            if (c == '"') esc += "\\\"";
            else if (c == '\n') esc += "\\n";
            else if (c == '\r') esc += "\\r";
            else if (c == '\\') esc += "\\\\";
            else esc += c;
        }
        json += "\"topology_diagnostics\":\"" + esc + "\",";
    }
    {
        auto readMultiline = [](const char* path) -> std::string {
            std::string s = readFile(path);
            std::string esc;
            esc.reserve(s.size() + 8);
            for (char c : s) {
                if (c == '"') esc += "\\\"";
                else if (c == '\n') esc += "\\n";
                else if (c == '\r') esc += "\\r";
                else if (c == '\\') esc += "\\\\";
                else esc += c;
            }
            return esc;
        };
        std::string clustersEsc = readMultiline((std::string(STATE_DIR) + "/clusters").c_str());
        json += "\"clusters\":\"" + clustersEsc + "\",";
        std::string clustersRawEsc = readMultiline((std::string(STATE_DIR) + "/clusters_raw_debug").c_str());
        json += "\"clusters_raw_debug\":\"" + clustersRawEsc + "\",";
    }
    json += "\"current_profile\":\"" + aatpe_profile::effectiveProfile() + "\",";
    {
      std::lock_guard<std::mutex> fgLock(g_foregroundMutex);
      json += "\"foreground_app\":\"" + g_foregroundApp + "\",";
    }
    json += "\"system_load\":" + std::to_string(readInt((std::string(STATE_DIR)+"/system_load").c_str(),0)) + ",";
    json += "\"cpu_usage\":" + std::to_string(readInt((std::string(STATE_DIR)+"/cpu_usage").c_str(),0)) + ",";
    {
        std::string rawLoad = readFile((std::string(STATE_DIR)+"/loadavg_raw").c_str());
        if (rawLoad.empty()) rawLoad = "0.00";
        json += "\"loadavg_raw\":\"" + jsonEscape(rawLoad) + "\",";
    }
    json += "\"thermal_cpu\":" + std::to_string(g_thermalCpu.load(std::memory_order_relaxed)) + ",";
    json += "\"thermal_skin\":" + std::to_string(g_thermalSkin.load(std::memory_order_relaxed)) + ",";
    json += "\"total_decisions\":" + std::to_string((long long)g_totalDecisions.load()) + ",";
    json += "\"total_migrations\":" + std::to_string((long long)g_totalMigrations.load()) + ",";
    {
        std::lock_guard<std::mutex> tlock(g_threadsMutex);
        json += "\"tracked_threads\":" + std::to_string((int)g_threads.size()) + ",";
    }
    {
        float wh, wm, why, whs;
        long long cnt = g_fusionWeightAccum.getAverageWeights(wh, wm, why, whs);
        char fbuf[256];
        snprintf(fbuf, sizeof(fbuf),
            "\"decision_fusion\":{\"count\":%lld,\"heuristic\":%.4f,\"ml\":%.4f,\"hybrid\":%.4f,\"history\":%.4f},",
            cnt, (double)wh, (double)wm, (double)why, (double)whs);
        json += fbuf;
    }
    {
        std::lock_guard<std::mutex> mlLock(g_mlMutex);
        json += "\"ml\":{";
        json += "\"enabled\":" + std::string(g_mlModel.isEnabled()?"true":"false") + ",";
        json += "\"samples\":" + std::to_string(g_mlModel.getTotalSamples()) + ",";
        json += "\"mature\":" + std::string(g_mlModel.isMature()?"true":"false") + ",";
        char abuf[32]; snprintf(abuf, sizeof(abuf), "%.4f", (double)g_mlModel.getAlpha());
        json += "\"alpha\":" + std::string(abuf) + ",";
        json += "\"pool_count\":" + std::to_string(g_mlModel.getPoolCount());
        json += "},";
    }
    {
        json += "\"frame_time_outcome\":{";
        json += "\"samples\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeSamples.load()) + ",";
        json += "\"improved\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeImproved.load()) + ",";
        json += "\"degraded\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeDegraded.load()) + ",";
        json += "\"fallback\":" + std::to_string((long long)aatpe_ml::g_ftOutcomeFallback.load()) + ",";
        json += "\"gfxinfo_calls\":" + std::to_string((long long)aatpe_ml::g_ftGfxinfoCalls.load()) + ",";
        json += "\"gfxinfo_cache_hits\":" + std::to_string((long long)aatpe_ml::g_ftGfxinfoCacheHits.load());
        json += "},";
        aatpe_ml::FrameTimeOutcome ft = getCurrentFrameTimeSnapshot();
        char ftbuf[256];
        snprintf(ftbuf, sizeof(ftbuf),
            "\"frame_time_snapshot\":{\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"p99_ms\":%.2f,\"samples\":%d},",
            (double)ft.frame_time_avg, (double)ft.frame_time_p95, (double)ft.frame_time_p99, ft.sample_count);
        json += ftbuf;
    }
    {
        int healthSnap, cycleSnap; bool agedSnap;
        {
            std::lock_guard<std::mutex> batLk(g_batteryStateMutex);
            healthSnap = g_batteryHealthPercent;
            cycleSnap  = g_batteryCycleCount;
            agedSnap   = g_batteryAged;
        }
        json += "\"battery_aging\":{\"enabled\":" + std::string(cfgSnap.batteryAgingProtection?"true":"false")
              + ",\"aged\":" + std::string(agedSnap?"true":"false")
              + ",\"health\":" + std::to_string(healthSnap)
              + ",\"cycle_count\":" + std::to_string(cycleSnap) + "},";
    }
    json += "\"ebpf\":{\"active\":" + std::string(g_ebpfActive.load(std::memory_order_acquire)?"true":"false") + "},";
    std::vector<int> littleSnap, midSnap, bigSnap, primeSnap;
    {
        std::lock_guard<std::mutex> topoLock(g_topologyRwMutex);
        littleSnap = g_littlePool;
        midSnap    = g_midPool;
        bigSnap    = g_bigPool;
        for (const auto& cl : clusters()) {
            if (cl.tier == CpuTier::PRIME) primeSnap = cl.cpus;
        }
    }
    json += "\"cpu_topology\":{\"little\":[";
    for (size_t i=0;i<littleSnap.size();i++){if(i>0)json+=",";json+=std::to_string(littleSnap[i]);}
    json += "],\"mid\":[";
    for (size_t i=0;i<midSnap.size();i++){if(i>0)json+=",";json+=std::to_string(midSnap[i]);}
    json += "],\"big\":[";
    for (size_t i=0;i<bigSnap.size();i++){if(i>0)json+=",";json+=std::to_string(bigSnap[i]);}
    json += "],\"prime\":[";
    for (size_t i=0;i<primeSnap.size();i++){if(i>0)json+=",";json+=std::to_string(primeSnap[i]);}
    json += "]}";
    json += "}";
    if (!atomicWriteFile((std::string(STATE_DIR) + "/snapshot.json").c_str(), json))
        LOG_DEBUG("writeSnapshotJson: snapshot.json publish failed (non-fatal)");
}

const char* systemStateToString(SystemState s) {
    switch (s) {
        case SystemState::NORMAL:    return "NORMAL";
        case SystemState::DEGRADED:  return "DEGRADED";
        case SystemState::PASSIVE:   return "PASSIVE";
        case SystemState::EMERGENCY: return "EMERGENCY";
    }
    return "UNKNOWN";
}

int systemStatePriority(SystemState s) {
    switch (s) {
        case SystemState::EMERGENCY: return 4;
        case SystemState::PASSIVE:   return 3;
        case SystemState::DEGRADED:  return 2;
        case SystemState::NORMAL:    return 1;
    }
    return 0;
}

SystemState getSystemState() {
    return (SystemState)g_systemState.load(std::memory_order_acquire);
}

long getStateDurationSec() {
    long long enter = s_stateEnterSteadyMs.load(std::memory_order_relaxed);
    if (enter == 0) return 0;
    long long nowMs = nowSteadyMs();
    if (nowMs < enter) return 0;
    return (long)((nowMs - enter) / 1000);
}

void setStateEnterTimeNow() {
    g_stateEnterTime.store(time(nullptr), std::memory_order_relaxed);
    s_stateEnterSteadyMs.store(nowSteadyMs(), std::memory_order_relaxed);
}

std::string getLastTransitionReason() {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    return std::string(g_lastTransitionReason);
}

bool isWatchdogPassiveLocked() {
    return g_watchdogPassiveLocked.load(std::memory_order_relaxed);
}

void setWatchdogPassiveLocked(bool locked) {
    g_watchdogPassiveLocked.store(locked, std::memory_order_relaxed);
}

std::vector<StateHistoryEntry> getStateHistory() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    std::vector<StateHistoryEntry> result;
    for (auto it = g_stateHistory.rbegin(); it != g_stateHistory.rend(); ++it) {
        result.push_back(*it);
    }
    return result;
}

bool transitionToState(SystemState newState, const char* reason) {
    SystemState current = getSystemState();
    if (current == newState) return false;

    int curPrio = systemStatePriority(current);
    int newPrio = systemStatePriority(newState);
    if (newPrio < curPrio) {
        if (!(current == SystemState::EMERGENCY && newState == SystemState::DEGRADED)) {
            if (!(current == SystemState::DEGRADED && newState == SystemState::NORMAL)) {
                if (!(current == SystemState::PASSIVE && newState == SystemState::NORMAL
                      && !isWatchdogPassiveLocked())) {
                    LOG_WARN("STATE: transition %s -> %s BLOCKED by priority (reason=%s)",
                             systemStateToString(current), systemStateToString(newState),
                             reason ? reason : "unknown");
                    return false;
                }
            }
        }
    }

    if (current == SystemState::PASSIVE && isWatchdogPassiveLocked() &&
        newState != SystemState::EMERGENCY) {
        LOG_WARN("STATE: cannot leave PASSIVE — watchdog lock active (reason=%s)",
                 reason ? reason : "unknown");
        return false;
    }

    {
        bool emergencyEscalation = (newState == SystemState::EMERGENCY);
        long long lastCommitMs = s_lastCommitMs.load(std::memory_order_acquire);
        long long dwellMs = nowSteadyMs() - lastCommitMs;
        if (!emergencyEscalation && lastCommitMs != 0 && dwellMs < effectiveDwellMs()) {
            static std::atomic<long long> s_lastDeferLogMs{0};
            long long nowDeferMs = nowSteadyMs();
            long long prevDefer = s_lastDeferLogMs.load(std::memory_order_relaxed);
            if (prevDefer == 0 || (nowDeferMs - prevDefer) >= 5000) {
                s_lastDeferLogMs.store(nowDeferMs, std::memory_order_relaxed);
                LOG_INFO("STATE: %s -> %s DEFERRED by dwell (%lldms < %lldms, reason=%s)",
                         systemStateToString(current), systemStateToString(newState),
                         (long long)dwellMs, (long long)effectiveDwellMs(),
                         reason ? reason : "unknown");
            }
            return false;
        }
    }

    {
        int expected = (int)current;
        if (!g_systemState.compare_exchange_strong(expected, (int)newState,
                                                   std::memory_order_acq_rel)) {
            LOG_INFO("STATE: %s -> %s transition lost race (now=%d), reason=%s",
                     systemStateToString(current), systemStateToString(newState),
                     expected, reason ? reason : "unknown");
            return false;
        }
    }

    time_t now = time(nullptr);
    long long nowSteady = nowSteadyMs();
    long long prevEnterSteady = 0;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        prevEnterSteady = s_stateEnterSteadyMs.load(std::memory_order_relaxed);
        StateHistoryEntry entry;
        entry.fromState = current;
        entry.toState = newState;
        entry.timestamp = now;
        strncpy(entry.reason, reason ? reason : "unknown", sizeof(entry.reason) - 1);
        entry.reason[sizeof(entry.reason) - 1] = '\0';
        g_stateHistory.push_back(entry);
        if ((int)g_stateHistory.size() > kMaxStateHistory) {
            g_stateHistory.pop_front();
        }
        strncpy(g_lastTransitionReason, reason ? reason : "unknown",
                sizeof(g_lastTransitionReason) - 1);
        g_lastTransitionReason[sizeof(g_lastTransitionReason) - 1] = '\0';
    }

    g_stateEnterTime.store(now, std::memory_order_relaxed);
    s_stateEnterSteadyMs.store(nowSteady, std::memory_order_relaxed);
    s_lastCommitMs.store(nowSteady, std::memory_order_release);
    long long prevDurationSec = (nowSteady > prevEnterSteady)
                                    ? (nowSteady - prevEnterSteady) / 1000 : 0;

    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        switch (newState) {
            case SystemState::NORMAL:
                break;
            case SystemState::DEGRADED:
                break;
            case SystemState::PASSIVE:
                break;
            case SystemState::EMERGENCY:
                g_preEmergencyCfg = g_cfg;
                g_preEmergencyAutoProfile = g_cfg.autoProfile;
                g_emergencyUserOverride.store(false, std::memory_order_relaxed);
                break;
        }
    }
    switch (newState) {
        case SystemState::NORMAL:
            g_thermalEmergency.store(false, std::memory_order_relaxed);
            g_watchdogPassiveLocked.store(false, std::memory_order_relaxed);
            g_degradedNormalCycles.store(0, std::memory_order_relaxed);
            aatpe_profile::profileRelease(aatpe_profile::ProfileSource::EMERGENCY);
            break;
        case SystemState::DEGRADED:
            g_thermalEmergency.store(false, std::memory_order_relaxed);
            g_degradedNormalCycles.store(0, std::memory_order_relaxed);
            aatpe_profile::profileRelease(aatpe_profile::ProfileSource::EMERGENCY);
            break;
        case SystemState::PASSIVE:
            g_thermalEmergency.store(false, std::memory_order_relaxed);
            aatpe_profile::profileRelease(aatpe_profile::ProfileSource::EMERGENCY);
            break;
        case SystemState::EMERGENCY:
            g_preEmergencyProfile = aatpe_profile::effectiveProfile();
            g_thermalEmergency.store(true, std::memory_order_relaxed);
            aatpe_profile::profileRequest("thermal", aatpe_profile::ProfileSource::EMERGENCY);
            break;
    }

    std::string controlModeForFile;
    {
        std::lock_guard<std::mutex> cfgLock(g_cfgMutex);
        controlModeForFile = g_cfg.controlMode;
    }
    if (!atomicWriteFile((std::string(STATE_DIR) + "/thermal_emergency").c_str(),
              g_thermalEmergency.load() ? "1" : "0"))
        LOG_WARN("state-transition: failed to write thermal_emergency file");
    if (!atomicWriteFile((std::string(STATE_DIR) + "/control_mode").c_str(),
              controlModeForFile))
        LOG_WARN("state-transition: failed to write control_mode file");
    {
        std::string effective = (newState == SystemState::EMERGENCY)
                                ? std::string("emergency")
                                : controlModeForFile;
        if (!atomicWriteFile((std::string(STATE_DIR) + "/control_mode_effective").c_str(),
                  effective))
            LOG_WARN("state-transition: failed to write control_mode_effective file");
    }
    if (!atomicWriteFile((std::string(STATE_DIR) + "/state_duration_sec").c_str(),
              (long long)prevDurationSec))
        LOG_WARN("state-transition: failed to write state_duration_sec file");
    if (!atomicWriteFile((std::string(STATE_DIR) + "/last_transition_reason").c_str(),
              std::string(reason ? reason : "unknown")))
        LOG_WARN("state-transition: failed to write last_transition_reason file");
    if (!atomicWriteFile((std::string(STATE_DIR) + "/system_state").c_str(),
              systemStateToString(newState)))
        LOG_WARN("state-transition: failed to write system_state file");

    LOG_WARN("STATE: %s -> %s (reason=%s, duration_in_prev=%llds)",
             systemStateToString(current), systemStateToString(newState),
             reason ? reason : "unknown",
             (long long)prevDurationSec);
    return true;
}


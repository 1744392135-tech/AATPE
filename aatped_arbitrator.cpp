
#include "aatped_arbitrator.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace aatpe {
namespace arb {

namespace {

inline float clampf(float v, float lo, float hi) {
    if (!(v > lo)) v = lo;
    if (v > hi) v = hi;
    return v;
}
inline bool finite(float v) { return std::isfinite(v); }

}

void arbDeriveFrameSignals(float ft_avg, float ft_p95, int ft_valid,
                           int refreshHz,
                           float* outActual, float* outTarget, float* outJitter) {
    if (outActual)   *outActual   = 0.0f;
    if (outTarget)   *outTarget   = 0.0f;
    if (outJitter)   *outJitter   = 0.0f;
    if (!outActual || !outTarget || !outJitter) return;
    if (!ft_valid || !std::isfinite(ft_avg) || ft_avg <= 0.0f) return;

    float actual = ft_avg > 0.0f ? ft_avg : 0.0f;
    float target = (refreshHz > 0) ? (1000.0f / (float)refreshHz) : 0.0f;
    float p95 = (std::isfinite(ft_p95) && ft_p95 > 0.0f) ? ft_p95 : actual;
    float jitter = (target > 0.0f) ? (p95 - target) : (p95 - actual);
    if (jitter < 0.0f) jitter = 0.0f;

    *outActual = actual;
    *outTarget = target;
    *outJitter = jitter;
}

float arbHoursOfDay(time_t t) {
    if (t <= 0) return -1.0f;
    struct tm lt;
    if (!localtime_r(&t, &lt)) return -1.0f;
    return (float)lt.tm_hour + (float)lt.tm_min / 60.0f + (float)lt.tm_sec / 3600.0f;
}

int arbScreenOnFromBlank(int blankNodeValue) {
    return (blankNodeValue == 0) ? 1 : 0;
}

float arbTouchEventsPerTick(uint64_t totalNow, uint64_t* prevTotal) {
    if (!prevTotal) return 0.0f;
    uint64_t prev = *prevTotal;
    *prevTotal = totalNow;
    if (totalNow < prev) return 0.0f;
    uint64_t d = totalNow - prev;
    if (d > 100000u) d = 100000u;
    return (float)d;
}

float arbDtMs(int64_t nowSteadyMs, int64_t* prevSteadyMs) {
    if (!prevSteadyMs) return 50.0f;
    if (*prevSteadyMs == 0) { *prevSteadyMs = nowSteadyMs; return 50.0f; }
    int64_t d = nowSteadyMs - *prevSteadyMs;
    *prevSteadyMs = nowSteadyMs;
    if (d < 1) d = 1;
    if (d > 60000) d = 60000;
    return (float)d;
}

int arbLoadProfileFile(const char* path, DeviceProfile* out,
                       uint64_t expectedFingerprint) {
    if (!path || !out) return -2;
    int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT) ? -1 : -2;

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return -2; }
    if (::getuid() == 0 && (st.st_uid != 0 || st.st_gid != 0)) {
        ::close(fd); return -2;
    }

    uint8_t buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    ::close(fd);
    if (n <= 0) return -2;

    DeviceProfile loaded;
    device_profile_init(&loaded);
    if (device_profile_deserialize(&loaded, buf, (uint32_t)n) != (int)sizeof(DeviceProfile) ||
        !device_profile_is_valid(&loaded)) {
        return -2;
    }
    if (expectedFingerprint != 0 && loaded.topology_fingerprint != expectedFingerprint) {
        return -3;
    }
    *out = loaded;
    return 0;
}

int arbSaveProfileFileAtomic(const char* path, const DeviceProfile* dp) {
    if (!path || !dp) return -1;
    uint8_t buf[256];
    int n = device_profile_serialize(dp, buf, (uint32_t)sizeof(buf));
    if (n <= 0) return -1;

    std::string tmp = std::string(path) + ".tmp.XXXXXX";
    int fd = ::mkstemp(&tmp[0]);
    if (fd < 0) return -1;
    if (::fchmod(fd, 0600) != 0) { ::close(fd); ::unlink(tmp.c_str()); return -1; }
    ssize_t w = ::write(fd, buf, (size_t)n);
    bool ok = (w == (ssize_t)n);
    if (::fsync(fd) != 0) ok = false;
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); return -1; }
    if (::rename(tmp.c_str(), path) != 0) { ::unlink(tmp.c_str()); return -1; }
    return 0;
}

void arb_runtime_init(ArbRuntime* rt) {
    std::memset(rt, 0, sizeof(*rt));
    game_scene_state_init(&rt->gs);
    load_forecast_state_init(&rt->lf);
    ctx_state_init(&rt->ctx);
    device_profile_init(&rt->dp);
    device_profile_calibrate(&rt->dp, &rt->thr);
    rt->cap_armed = 0;
    rt->ticks = 0;
}

void arb_compute(const ArbSignals* sig, ArbRuntime* rt, ArbOutput* out) {
    std::memset(out, 0, sizeof(*out));

    if (!sig || !sig->signals_valid) {
        out->active = 0;
        out->boost_vote_pct = 0;
        out->frame_cap_hz = 0;
        out->efficiency_hint = 0.0f;
        out->scene = (int)GAME_UNKNOWN;
        out->scene_confidence = 0.0f;
        rt->cap_armed = 0;
        return;
    }

    GameSceneSample gsSample;
    std::memset(&gsSample, 0, sizeof(gsSample));
    gsSample.frame_actual_ms   = finite(sig->frame_actual_ms) ? sig->frame_actual_ms : 0.0f;
    gsSample.frame_target_ms   = finite(sig->frame_target_ms) ? sig->frame_target_ms : 0.0f;
    gsSample.frame_jitter_ms   = finite(sig->frame_jitter_ms) ? sig->frame_jitter_ms : 0.0f;
    gsSample.touch_events_per_tick = finite(sig->touch_events_per_tick) ? sig->touch_events_per_tick : 0.0f;
    gsSample.cpu_util_pct      = clampf(finite(sig->cpu_util_pct) ? sig->cpu_util_pct : 0.0f, 0.0f, 100.0f);
    gsSample.gpu_util_pct      = clampf(finite(sig->gpu_util_pct) ? sig->gpu_util_pct : 0.0f, 0.0f, 100.0f);
    gsSample.io_pressure_pct   = clampf(finite(sig->io_pressure_pct) ? sig->io_pressure_pct : 0.0f, 0.0f, 100.0f);
    gsSample.tick_dt_ms        = finite(sig->tick_dt_ms) && sig->tick_dt_ms > 0.0f ? sig->tick_dt_ms : 50.0f;

    GameSceneConfig gsCfg; game_scene_config_default(&gsCfg);
    GameSceneDecision gsDec;
    game_scene_decide(&gsSample, &gsCfg, &rt->gs, &gsDec);

    DevObservation dpObs;
    std::memset(&dpObs, 0, sizeof(dpObs));
    dpObs.temp_c          = finite(sig->temp_c) ? sig->temp_c : 0.0f;
    dpObs.thermal_trip_c = finite(sig->thermal_trip_c) ? sig->thermal_trip_c : 0.0f;
    dpObs.charging        = sig->charging;
    dpObs.battery_pct     = clampf(finite(sig->battery_pct) ? sig->battery_pct : 0.0f, 0.0f, 100.0f);
    dpObs.refresh_rate_hz = finite(sig->refresh_rate_hz) ? sig->refresh_rate_hz : 0.0f;
    dpObs.cpu_util_pct    = gsSample.cpu_util_pct;
    dpObs.gpu_util_pct    = gsSample.gpu_util_pct;
    device_profile_observe(&rt->dp, &dpObs);
    device_profile_calibrate(&rt->dp, &rt->thr);

    LoadObservation lfObs;
    std::memset(&lfObs, 0, sizeof(lfObs));
    lfObs.cpu_util_pct    = gsSample.cpu_util_pct;
    lfObs.gpu_util_pct    = gsSample.gpu_util_pct;
    lfObs.frame_time_ms   = gsSample.frame_actual_ms;
    lfObs.psi_cpu_avg10   = clampf(finite(sig->psi_cpu_avg10) ? sig->psi_cpu_avg10 : 0.0f, 0.0f, 100.0f);
    LoadForecastConfig lfCfg; load_forecast_config_default(&lfCfg);
    LoadForecast fcst;
    load_forecast_step(&lfObs, &lfCfg, &rt->lf, &fcst);

    CtxSample ctxSample;
    std::memset(&ctxSample, 0, sizeof(ctxSample));
    ctxSample.hours_of_day            = finite(sig->hours_of_day) ? sig->hours_of_day : 12.0f;
    ctxSample.screen_on               = sig->screen_on;
    ctxSample.battery_pct             = dpObs.battery_pct;
    ctxSample.charging                = sig->charging;
    ctxSample.last_interaction_ago_ms = finite(sig->last_interaction_ago_ms) ? sig->last_interaction_ago_ms : 100000.0f;
    ctxSample.tick_dt_ms              = gsSample.tick_dt_ms;
    CtxConfig ctxCfg; ctx_config_default(&ctxCfg);
    CtxDecision ctxDec;
    ctx_decide(&ctxSample, &ctxCfg, &rt->ctx, &ctxDec);

    float scene_vote = 0.0f;
    if (finite(gsDec.boost_tendency) && gsDec.boost_tendency > 0.0f && gsDec.confidence > 0.3f) {
        scene_vote = clampf(gsDec.boost_tendency, 0.0f, 1.0f) * rt->thr.ingame_boost_level;
    }
    float fcst_vote = 0.0f;
    if (fcst.boost_hint && finite(fcst.confidence) && fcst.confidence > 0.0f) {
        fcst_vote = fcst.confidence * rt->thr.default_boost_level;
    }
    float raw_perf = scene_vote > fcst_vote ? scene_vote : fcst_vote;
    out->raw_perf_vote = raw_perf;

    float perf = raw_perf;
    if (finite(ctxDec.boost_bias)) {
        perf = raw_perf * clampf(1.0f + ctxDec.boost_bias, 0.5f, 1.0f);
    }

    if (sig->thermal_hard_wall) {
        perf = 0.0f;
    }

    out->boost_vote_pct = (int)std::lround(clampf(perf, 0.0f, 100.0f));

    int cap_hz = 0;
    float tend = finite(gsDec.frame_cap_tendency) ? gsDec.frame_cap_tendency : 0.0f;
    if (sig->thermal_hard_wall) {
        cap_hz = (int)rt->thr.cutscene_frame_cap_hz;
        if (cap_hz <= 0) cap_hz = 30;
        rt->cap_armed = 1;
    } else if (rt->cap_armed) {
        if (tend < 0.2f || gsDec.scene == GAME_IN_GAME) {
            rt->cap_armed = 0;
            cap_hz = 0;
        } else {
            cap_hz = (int)rt->thr.cutscene_frame_cap_hz;
            if (cap_hz <= 0) cap_hz = 30;
        }
    } else {
        if (tend >= 0.4f && gsDec.confidence > 0.3f) {
            rt->cap_armed = 1;
            cap_hz = (int)rt->thr.cutscene_frame_cap_hz;
            if (cap_hz <= 0) cap_hz = 30;
        } else {
            cap_hz = 0;
        }
    }
    out->frame_cap_hz = cap_hz;

    float eff = 0.0f;
    if (finite(gsDec.boost_tendency) && gsDec.boost_tendency < 0.0f)
        eff += -gsDec.boost_tendency;
    if (fcst.deboost_hint)
        eff += 0.5f;
    if (finite(ctxDec.efficiency_bias) && ctxDec.efficiency_bias < 0.0f)
        eff += -ctxDec.efficiency_bias;
    out->efficiency_hint = clampf(eff, 0.0f, 1.0f);

    out->active = 1;
    out->scene = (int)gsDec.scene;
    out->scene_confidence = clampf(gsDec.confidence, 0.0f, 1.0f);
    rt->ticks++;
}

}
}

#ifdef AATPED_ARBITRATOR_DAEMON

#include "aatped.h"
#include "gpu_boost.h"
#include "input_monitor.h"
#include "aatpe_diag.h"

extern std::atomic<int> g_thermalTripC;

namespace aatpe { int aatpeThermalHalLevel(); }

namespace aatpe {
namespace arb {

namespace {
ArbRuntime g_rt;
bool g_loaded = false;

std::string g_profilePath;
bool        g_profilePersistent = false;

int64_t  g_lastTickSteadyMs = 0;
uint64_t g_prevTouchEvents = 0;

bool     g_boostWasOn = false;
float    g_boostFrameBefore = 0.0f;
int64_t  g_boostOnMs = 0;
bool     g_boostSampled = false;

int64_t  g_lastSaveMs = 0;
uint32_t g_saveCount = 0;
float    g_lastSavedConf = -1.0f;
uint32_t g_lastSavedObs = 0;
float    g_lastSavedEff = -1.0f;

constexpr int64_t kMinSaveIntervalMs  = 30000;
constexpr uint32_t kMaxSavesPerBoot   = 200;
constexpr int64_t kBoostSampleAfterMs = 800;

uint64_t currentTopologyFingerprint() {
    std::vector<CpuCluster> cs = clustersSnapshot();
    if (cs.empty()) return 0;
    DevCluster dc[DEV_MAX_CLUSTERS];
    uint32_t n = 0;
    for (const auto& c : cs) {
        if (n >= DEV_MAX_CLUSTERS) break;
        std::memset(&dc[n], 0, sizeof(dc[n]));
        for (int cpu : c.cpus) if (cpu >= 0 && cpu < 32) dc[n].cpu_mask |= (1u << cpu);
        dc[n].min_freq_khz = (uint32_t)c.minKhz;
        dc[n].max_freq_khz = (uint32_t)c.maxKhz;
        n++;
    }
    return device_profile_fingerprint(dc, n);
}

void applyTopologyOnce() {
    if (g_rt.dp.num_clusters > 0) return;
    std::vector<CpuCluster> cs = clustersSnapshot();
    if (cs.empty()) return;
    DevCluster dc[DEV_MAX_CLUSTERS];
    uint32_t n = 0;
    for (const auto& c : cs) {
        if (n >= DEV_MAX_CLUSTERS) break;
        std::memset(&dc[n], 0, sizeof(dc[n]));
        for (int cpu : c.cpus) if (cpu >= 0 && cpu < 32) dc[n].cpu_mask |= (1u << cpu);
        dc[n].min_freq_khz = (uint32_t)c.minKhz;
        dc[n].max_freq_khz = (uint32_t)c.maxKhz;
        n++;
    }
    if (n > 0) {
        device_profile_set_topology(&g_rt.dp, dc, n);
        LOG_INFO("arb: device_profile topology bound (%u clusters, fp=0x%016llx)",
                 n, (unsigned long long)g_rt.dp.topology_fingerprint);
    }
}

void resolveProfilePath() {
    const char* flashDir = "/data/adb/aatpe";
    std::string flashPath = std::string(flashDir) + "/device_profile.bin";
    std::string ramPath   = std::string(STATE_DIR) + "/device_profile.bin";

    bool flashOk = (::access(flashDir, W_OK) == 0);
    if (!flashOk) {
        if (::mkdir(flashDir, 0700) == 0) flashOk = true;
    }
    if (flashOk) {
        g_profilePath = flashPath;
        g_profilePersistent = true;
        LOG_INFO("arb: device_profile persistence -> flash %s", g_profilePath.c_str());
    } else {
        g_profilePath = ramPath;
        g_profilePersistent = false;
        LOG_WARN("arb: flash profile dir not writable -> %s (SINGLE-BOOT learning only)",
                 g_profilePath.c_str());
    }
}

static int readNodeNoFollow(const char* path) {
    int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return -1; }
    char buf[32] = {0};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return (int)strtol(buf, nullptr, 10);
}

int readScreenOn() {
    int v = readNodeNoFollow("/sys/class/graphics/fb0/blank");
    if (v >= 0) return arbScreenOnFromBlank(v);
    const char* bl[] = {
        "/sys/class/backlight/panel0-backlight/bl_power",
        "/sys/class/backlight/panel-backlight/bl_power",
        "/sys/class/leds/lcd-backlight/bl_power"
    };
    for (const char* p : bl) {
        int bv = readNodeNoFollow(p);
        if (bv >= 0) return bv == 0 ? 1 : 0;
    }
    const char* br[] = {
        "/sys/class/backlight/panel0-backlight/brightness",
        "/sys/class/leds/lcd-backlight/brightness"
    };
    for (const char* p : br) {
        long bv = readNodeNoFollow(p);
        if (bv >= 0) return bv > 0 ? 1 : 0;
    }
    return 1;
}

int64_t nowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void saveProfileThrottled() {
    if (!g_profilePersistent && g_profilePath.empty()) return;
    if (g_saveCount >= kMaxSavesPerBoot) return;

    int64_t now = nowSteadyMs();
    if (g_lastSaveMs != 0 && (now - g_lastSaveMs) < kMinSaveIntervalMs) return;

    float conf = g_rt.dp.calibration_confidence;
    uint32_t obs = g_rt.dp.total_observation_count;
    float eff = g_rt.dp.boost_effectiveness;
    bool significant = (g_lastSaveMs == 0)
        || (conf - g_lastSavedConf > 0.02f)
        || ((float)(obs - g_lastSavedObs) >= 25.0f)
        || (eff - g_lastSavedEff > 0.05f);
    if (!significant) return;

    if (arbSaveProfileFileAtomic(g_profilePath.c_str(), &g_rt.dp) == 0) {
        g_lastSaveMs = now;
        g_saveCount++;
        g_lastSavedConf = conf;
        g_lastSavedObs = obs;
        g_lastSavedEff = eff;
    }
}
}

void init() {
    arb_runtime_init(&g_rt);
    g_loaded = false;
    g_lastTickSteadyMs = 0;
    g_prevTouchEvents = 0;
    g_boostWasOn = false;
    g_lastSaveMs = 0;
    g_saveCount = 0;
    g_lastSavedConf = -1.0f;
    g_lastSavedObs = 0;
    g_lastSavedEff = -1.0f;

    resolveProfilePath();
    applyTopologyOnce();
    uint64_t fp = currentTopologyFingerprint();

    DeviceProfile loaded;
    device_profile_init(&loaded);
    int rc = arbLoadProfileFile(g_profilePath.c_str(), &loaded, fp);
    if (rc == 0) {
        g_rt.dp = loaded;
        device_profile_calibrate(&g_rt.dp, &g_rt.thr);
        g_loaded = true;
        LOG_INFO("arb: device_profile loaded (%d obs, conf=%.2f, fp=0x%016llx)",
                 loaded.total_observation_count, (double)loaded.calibration_confidence,
                 (unsigned long long)loaded.topology_fingerprint);
    } else if (rc == -3) {
        LOG_WARN("arb: device_profile topology fingerprint mismatch -> discarded (fail-safe defaults)");
    } else if (rc == -2) {
        LOG_WARN("arb: device_profile corrupt/invalid -> fail-safe defaults");
    } else {
        LOG_INFO("arb: no device_profile yet -> defaults (fail-safe)");
    }
}

#ifdef AATPE_HOST_TEST
void resetForTest() {
    arb_runtime_init(&g_rt);
    g_loaded = false;
    aatpeBoostClear("scene");
    writeFile((std::string(STATE_DIR) + "/arbitrator_frame_cap_hz").c_str(), 0);
    writeFile((std::string(STATE_DIR) + "/arbitrator_efficiency_hint").c_str(), 0.0f);
    writeFile((std::string(STATE_DIR) + "/arbitrator_scene").c_str(), (int)GAME_UNKNOWN);
    if (!g_profilePath.empty()) ::unlink(g_profilePath.c_str());
}
#endif

void tick() {
    ArbSignals sig;
    std::memset(&sig, 0, sizeof(sig));

    int64_t now = nowSteadyMs();

    applyTopologyOnce();
    if (!g_loaded && g_rt.dp.num_clusters > 0 && g_rt.dp.topology_fingerprint != 0) {
        DeviceProfile loaded; device_profile_init(&loaded);
        if (arbLoadProfileFile(g_profilePath.c_str(), &loaded,
                               g_rt.dp.topology_fingerprint) == 0) {
            g_rt.dp = loaded;
            device_profile_calibrate(&g_rt.dp, &g_rt.thr);
            g_loaded = true;
            LOG_INFO("arb: device_profile late-loaded after topology (%d obs)",
                     loaded.total_observation_count);
        }
    }

    sig.gpu_util_pct = g_gpuUtilization.load(std::memory_order_relaxed);
    float cpuSum = 0.0f; int cpuN = 0;
    {
        std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
        for (auto& kv : g_cpuMetrics) {
            if (kv.second.util >= 0) { cpuSum += kv.second.util; cpuN++; }
        }
    }
    sig.cpu_util_pct = cpuN > 0 ? cpuSum / (float)cpuN : 0.0f;

    aatpe_ml::FrameTimeOutcome ft = getCurrentFrameTimeSnapshot();
    int refreshHz = aatpe::frameaware::getRefreshRate();
    sig.refresh_rate_hz = (refreshHz > 0) ? (float)refreshHz : 0.0f;
    arbDeriveFrameSignals(ft.frame_time_avg, ft.frame_time_p95, ft.valid ? 1 : 0,
                          refreshHz,
                          &sig.frame_actual_ms, &sig.frame_target_ms, &sig.frame_jitter_ms);

    auto& im = aatpe::InputMonitor::instance();
    uint64_t tot = im.touchEventsTotal();
    sig.touch_events_per_tick = arbTouchEventsPerTick(tot, &g_prevTouchEvents);
    float lastAgo = (float)im.msSinceLastTouch();
    sig.last_interaction_ago_ms = std::isfinite(lastAgo) ? lastAgo : 100000.0f;

    readArbBatteryState();
    int capPct = -1, charging = 0;
    getArbBatterySnapshot(capPct, charging);
    sig.battery_pct  = (capPct >= 0) ? (float)capPct : 0.0f;
    sig.charging     = charging;

    time_t t = time(nullptr);
    float hr = arbHoursOfDay(t);
    sig.hours_of_day = (hr >= 0.0f && hr < 24.0f) ? hr : 12.0f;
    sig.screen_on    = readScreenOn();

    int cpuTemp = g_thermalCpu.load(std::memory_order_relaxed);
    sig.temp_c        = (cpuTemp > 0) ? (float)cpuTemp : 0.0f;
    int tripC = g_thermalTripC.load(std::memory_order_relaxed);
    sig.thermal_trip_c = (tripC > 0) ? (float)tripC : 0.0f;
    sig.thermal_hard_wall = (g_thermalEmergency.load(std::memory_order_relaxed) ||
                             aatpeThermalHalLevel() >= 3) ? 1 : 0;

    auto sp = aatpe::diag::getSystemPressureSnapshot();
    sig.psi_cpu_avg10  = sp.psiAvailable ? sp.psiCpuSomePct : 0.0f;
    sig.io_pressure_pct = sp.psiAvailable ? sp.psiIoSomePct : 0.0f;

    sig.tick_dt_ms = arbDtMs(now, &g_lastTickSteadyMs);

    sig.signals_valid = 1;

    ArbOutput out;
    arb_compute(&sig, &g_rt, &out);

    if (!out.active || out.boost_vote_pct <= 0) {
        aatpeBoostClear("scene");
    } else {
        aatpeBoostSet("scene", out.boost_vote_pct);
    }

    if (out.active && out.boost_vote_pct > 0 && sig.frame_actual_ms > 0.0f) {
        if (!g_boostWasOn) {
            g_boostWasOn = true;
            g_boostFrameBefore = sig.frame_actual_ms;
            g_boostOnMs = now;
            g_boostSampled = false;
        } else if (!g_boostSampled && (now - g_boostOnMs) >= kBoostSampleAfterMs) {
            DevBoostObservation bobs;
            std::memset(&bobs, 0, sizeof(bobs));
            bobs.frame_time_before_ms = g_boostFrameBefore;
            bobs.frame_time_after_ms  = sig.frame_actual_ms;
            bobs.boost_level = (float)out.boost_vote_pct;
            device_profile_record_boost(&g_rt.dp, &bobs);
            g_boostSampled = true;
        }
    } else {
        g_boostWasOn = false;
    }

    atomicWriteFile((std::string(STATE_DIR) + "/arbitrator_frame_cap_hz").c_str(),
                    (int)out.frame_cap_hz);
    char effBuf[32];
    snprintf(effBuf, sizeof(effBuf), "%.3f", out.efficiency_hint);
    atomicWriteFile((std::string(STATE_DIR) + "/arbitrator_efficiency_hint").c_str(),
                    std::string(effBuf));
    atomicWriteFile((std::string(STATE_DIR) + "/arbitrator_scene").c_str(),
                    (int)out.scene);

    saveProfileThrottled();
}

}
}

#endif

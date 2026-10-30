#ifndef AATPED_ARBITRATOR_H
#define AATPED_ARBITRATOR_H

#include <stdint.h>
#include <time.h>

#include "mech/include/game_scene.h"
#include "mech/include/device_profile.h"
#include "mech/include/load_forecast.h"
#include "mech/include/context.h"

namespace aatpe {
namespace arb {

struct ArbSignals {
    float cpu_util_pct;
    float gpu_util_pct;
    float frame_actual_ms;
    float frame_target_ms;
    float frame_jitter_ms;
    float touch_events_per_tick;
    float io_pressure_pct;
    float psi_cpu_avg10;

    float temp_c;
    float thermal_trip_c;
    int   charging;
    float battery_pct;
    float refresh_rate_hz;

    float hours_of_day;
    int   screen_on;
    float last_interaction_ago_ms;
    float tick_dt_ms;

    int   thermal_hard_wall;
    int   signals_valid;
};

struct ArbOutput {
    int   active;
    int   boost_vote_pct;
    int   frame_cap_hz;
    float efficiency_hint;
    int   scene;
    float scene_confidence;
    float raw_perf_vote;
};

struct ArbRuntime {
    GameSceneState    gs;
    LoadForecastState  lf;
    CtxState           ctx;
    DeviceProfile      dp;
    CalibratedThresholds thr;
    int   gs_cfg_ready;
    int   lf_cfg_ready;
    int   ctx_cfg_ready;
    int   cap_armed;
    uint32_t ticks;
};

void arb_runtime_init(ArbRuntime* rt);

void arb_compute(const ArbSignals* sig, ArbRuntime* rt, ArbOutput* out);

void arbDeriveFrameSignals(float ft_avg, float ft_p95, int ft_valid, int refreshHz,
                           float* outActual, float* outTarget, float* outJitter);
float arbHoursOfDay(time_t t);
int   arbScreenOnFromBlank(int blankNodeValue);
float arbTouchEventsPerTick(uint64_t totalNow, uint64_t* prevTotal);
float arbDtMs(int64_t nowSteadyMs, int64_t* prevSteadyMs);

int arbLoadProfileFile(const char* path, DeviceProfile* out,
                       uint64_t expectedFingerprint);
int arbSaveProfileFileAtomic(const char* path, const DeviceProfile* dp);

void init();

void tick();

#ifdef AATPE_HOST_TEST
void resetForTest();
#endif

}
}

#endif

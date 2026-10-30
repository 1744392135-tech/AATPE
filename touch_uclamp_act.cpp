#include "touch_uclamp_act.h"

#include <cmath>

namespace aatpe {

unsigned int touchUclampPctToKernel(float pct)
{
    if (!std::isfinite(pct)) return 0u;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    unsigned k = (unsigned)std::lroundf(pct * 10.24f);
    if (k > 1024u) k = 1024u;
    return k;
}

TouchUclampController::TouchUclampController()
{
    touch_config_default(&cfg_);
    touch_state_init(&st_);
    lastGlobalFloor_ = (int)std::lroundf(cfg_.global_idle_boost);
}

void TouchUclampController::configure(const TouchConfig& cfg)
{
    cfg_ = cfg;
}

void TouchUclampController::tick(uint32_t nowMs, bool touchActive,
                                const FgThread* threads, uint32_t nThreads,
                                float refreshHz, float frameActualMs,
                                float frameTargetMs)
{
    TouchDecision d;
    touch_decide(&st_, &cfg_, nowMs, touchActive ? 1 : 0,
                 threads, nThreads, refreshHz, frameActualMs, frameTargetMs, &d);
    apply_(d);
}

void TouchUclampController::apply_(const TouchDecision& d)
{
    for (int i = 0; i < d.action_count; ++i) {
        const BoostAction& a = d.actions[i];
        if (a.tid < 0) continue;
        if (!act_.setTaskUclampMin) continue;
        unsigned int util = (a.action == TOUCH_ACT_SET)
                                ? touchUclampPctToKernel(a.uclamp_min_pct)
                                : 0u;
        (void)act_.setTaskUclampMin(a.tid, util, act_.ctx);
    }

    float g = d.global_schedtune_boost_pct;
    if (!std::isfinite(g)) g = cfg_.global_idle_boost;
    if (g < 0.0f) g = 0.0f;
    if (g > 100.0f) g = 100.0f;
    int lvl = (int)std::lroundf(g);
    lastGlobalFloor_ = lvl;
    live_ = (d.touch_boost_active != 0);
    if (act_.setGlobalBoostFloor) {
        act_.setGlobalBoostFloor(lvl, act_.ctx);
    }
}

void TouchUclampController::releaseAll(uint32_t nowMs, bool clearGlobal)
{
    (void)nowMs;
    for (uint32_t i = 0; i < st_.n_boosted; ++i) {
        if (st_.tid[i] >= 0 && act_.setTaskUclampMin) {
            (void)act_.setTaskUclampMin(st_.tid[i], 0u, act_.ctx);
        }
    }
    touch_state_init(&st_);
    live_ = false;
    if (clearGlobal) {
        lastGlobalFloor_ = 0;
        if (act_.setGlobalBoostFloor) act_.setGlobalBoostFloor(0, act_.ctx);
    } else {
        lastGlobalFloor_ = (int)std::lroundf(cfg_.global_idle_boost);
        if (act_.setGlobalBoostFloor) act_.setGlobalBoostFloor(lastGlobalFloor_, act_.ctx);
    }
}

}

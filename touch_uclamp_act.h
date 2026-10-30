#ifndef AATPE_TOUCH_UCLAMP_ACT_H
#define AATPE_TOUCH_UCLAMP_ACT_H

#include "touch_uclamp.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aatpe {

struct TouchUclampActuator {
    int  (*setTaskUclampMin)(int tid, unsigned int utilMin, void* ctx) = nullptr;
    void (*setGlobalBoostFloor)(int level0_100, void* ctx) = nullptr;
    void* ctx = nullptr;
};

unsigned int touchUclampPctToKernel(float pct);

class TouchUclampController {
public:
    TouchUclampController();

    void configure(const TouchConfig& cfg);
    const TouchConfig& config() const { return cfg_; }

    void setActuator(const TouchUclampActuator& act) { act_ = act; }

    void tick(uint32_t nowMs, bool touchActive,
              const FgThread* threads, uint32_t nThreads,
              float refreshHz, float frameActualMs, float frameTargetMs);

    bool isBoostLive() const { return live_; }
    int globalFloorLevel() const { return lastGlobalFloor_; }
    uint32_t boostedCount() const { return st_.n_boosted; }

    void releaseAll(uint32_t nowMs, bool clearGlobal);

private:
    void apply_(const TouchDecision& d);

    TouchConfig cfg_;
    TouchState  st_;
    TouchUclampActuator act_;
    bool   live_ = false;
    int    lastGlobalFloor_ = 40;
};

uint32_t touchUclampCollectForegroundThreads(const std::string& fgPackage,
                                             FgThread* out, uint32_t cap);

}

#endif

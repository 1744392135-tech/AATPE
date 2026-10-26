// touch_uclamp_act.h — daemon-side actuator wiring for the verified
// `touch_uclamp` decision core (v4.3.6, mech_proto §3).
//
// The core (touch_uclamp.h/.c) is PURE logic: it emits a TouchDecision of
// per-thread SET/RELEASE actions plus a suggested global schedtune floor.
// This layer is the ONLY place that maps that decision onto real side effects:
//
//   - SET     -> aatpe_mech::aatpeUclampSetMin(tid, pct*10.24)  (per-thread uclamp.min)
//   - RELEASE -> aatpe_mech::aatpeUclampSetMin(tid, 0)         (drop the floor)
//   - global floor -> aatpeBoostSet/Clear("touch", idle40/active50)
//
// It owns NO production globals itself: the caller feeds it the foreground
// thread set, refresh rate and frame timing every tick, and injects the
// actuator callbacks. On the host the callbacks are faked, so the whole
// decide->apply pipeline is hermetically unit-testable (ASan/TSan clean).
//
// Boundaries (hard acceptance):
//   - Only UI_MAIN / RENDER / high-frame-contribution threads are ever SET.
//   - TOUCH_UCLAMP_ROLE_BG is never boosted (the core already skips it; we never
//     promote a BG thread here).
//   - Empty thread set -> no per-thread action, only the global low floor.
//   - Non-finite refresh/frame values -> core degrades to base window / no
//     overrun bonus; we never feed NaN to the syscall.
//   - Group-level (cgroup) uclamp is owned by the PSI path; this layer only
//     touches PER-THREAD sched_setattr uclamp.min and the global schedtune.
#ifndef AATPE_TOUCH_UCLAMP_ACT_H
#define AATPE_TOUCH_UCLAMP_ACT_H

#include "touch_uclamp.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aatpe {

// Actuator seam. All side effects go through these two hooks so the controller
// is host-testable. `ctx` is opaque to the controller.
//   setTaskUclampMin : 0..1024 util_min; 0 = drop the floor. Returns 0 on ok.
//   setGlobalBoostFloor : 0..100 schedtune.boost floor; 0 = clear the producer.
struct TouchUclampActuator {
    int  (*setTaskUclampMin)(int tid, unsigned int utilMin, void* ctx) = nullptr;
    void (*setGlobalBoostFloor)(int level0_100, void* ctx) = nullptr;
    void* ctx = nullptr;
};

// 0..100 percentage -> kernel 0..SCHED_CAPACITY_SCALE(=1024). round(pct*10.24).
// Clamped to [0,1024]. Non-finite pct -> 0 (fail-safe, never a NaN syscall arg).
unsigned int touchUclampPctToKernel(float pct);

// Owns the TouchState/TouchConfig for the single input-monitor epoll thread.
// Single-threaded by contract (the epoll loop calls tick() and nothing else);
// the actuator callbacks may be thread-safe producers but are only invoked
// from this thread. No internal mutex -> TSan-clean by construction.
class TouchUclampController {
public:
    TouchUclampController();

    // Override defaults (e.g. from config). Must be called before start of ticks.
    void configure(const TouchConfig& cfg);
    const TouchConfig& config() const { return cfg_; }

    // Install the actuator. Until installed, apply() is a silent no-op (decision
    // is still computed and isBoostLive()/globalFloorLevel() stay correct).
    void setActuator(const TouchUclampActuator& act) { act_ = act; }

    // One tick. nowMs = monotonic ms. touchActive = a touch event arrived this
    // tick. threads/nThreads: foreground thread set (may be empty/0).
    // refreshHz: display refresh (0/NaN -> core uses base window).
    // frameActualMs/frameTargetMs: <=0 or NaN -> core skips the overrun check.
    void tick(uint32_t nowMs, bool touchActive,
              const FgThread* threads, uint32_t nThreads,
              float refreshHz, float frameActualMs, float frameTargetMs);

    // True while a per-thread boost window is live (drives the adaptive epoll
    // timeout and the touchActive_ atomic).
    bool isBoostLive() const { return live_; }
    // Last applied global schedtune floor (0..100).
    int globalFloorLevel() const { return lastGlobalFloor_; }
    // Number of tids currently being boosted.
    uint32_t boostedCount() const { return st_.n_boosted; }

    // Tear-down: emit RELEASE for every currently-boosted tid and drop the
    // global floor to idle (or clear it entirely if clearGlobal is set).
    void releaseAll(uint32_t nowMs, bool clearGlobal);

private:
    void apply_(const TouchDecision& d);

    TouchConfig cfg_;
    TouchState  st_;
    TouchUclampActuator act_;
    bool   live_ = false;
    int    lastGlobalFloor_ = 40;
};

// ---------------------------------------------------------------------------
// Production-only foreground thread collector (input_monitor thread).
//
// Snapshot g_threads under g_threadsMutex, keep threads whose owning process
// name == fgPackage, and map them onto the core's FgThread role enum.
//   - tid == pid                                   -> TOUCH_UCLAMP_ROLE_UI_MAIN
//   - tname contains "RenderThread" / ROLE_S2_RENDER -> TOUCH_UCLAMP_ROLE_RENDER
//   - ROLE_S5_BACKGROUND                           -> TOUCH_UCLAMP_ROLE_BG (never boosted)
//   - ROLE_S1_UI                                   -> TOUCH_UCLAMP_ROLE_UI_MAIN
//   - anything else                                -> TOUCH_UCLAMP_ROLE_UNKNOWN
// frame_contribution is left -1 (unknown): without a real per-thread frame
// attribution we must NOT fabricate a share, so the high-contrib branch only
// fires when a caller supplies a real contribution (host tests do).
//
// Writes at most `cap` entries into `out`; returns the count written.
// fgPackage empty -> returns 0 (no foreground -> no per-thread action).
// Declared here, defined in touch_uclamp_act.cpp; pulls in g_threads only on
// the production link (not in the host unit test).
uint32_t touchUclampCollectForegroundThreads(const std::string& fgPackage,
                                             FgThread* out, uint32_t cap);

}  // namespace aatpe

#endif  // AATPE_TOUCH_UCLAMP_ACT_H

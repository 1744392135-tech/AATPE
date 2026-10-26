/*
 * touch_uclamp.h - per-thread uclamp selection for touch boosts.
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 *
 * Replaces the blanket global schedtune.boost=80 (input_monitor.cpp:219) with a
 * targeted plan: on touch, boost only the foreground UI main thread, Render
 * thread, and (optionally) other high frame-contribution threads, via per-thread
 * uclamp.min, for a short refresh-rate-scaled window. The global schedtune
 * boost is held at a low floor instead of maxed.
 *
 * This module only OUTPUTS a plan. The daemon actuator maps BoostAction ->
 * sched_setattr(SCHED_FLAG_UCLAMP_MIN) / cgroup uclamp writes and timers.
 */
#ifndef AATPE_TOUCH_UCLAMP_H
#define AATPE_TOUCH_UCLAMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define TOUCH_FF_MAX_THREADS 32u
#define TOUCH_FF_MAX_ACTIONS 16u

typedef enum {
    TOUCH_UCLAMP_ROLE_UNKNOWN = 0,
    TOUCH_UCLAMP_ROLE_UI_MAIN = 1,   /* tid == pid; the app main/UI thread */
    TOUCH_UCLAMP_ROLE_RENDER = 2,    /* RenderThread */
    TOUCH_UCLAMP_ROLE_BG = 3         /* known background/worker; never boosted */
} TouchUclampRole;

typedef struct {
    int32_t tid;                /* <0 => invalid, skipped */
    TouchUclampRole role;
    float frame_contribution;   /* 0..1 estimated share of frame work; <0 if unknown */
} FgThread;

typedef enum {
    TOUCH_ACT_RELEASE = 0,
    TOUCH_ACT_SET = 1
} TouchAction;

typedef struct {
    int32_t tid;
    TouchAction action;
    float uclamp_min_pct;  /* 0..100 (when SET) */
    uint32_t duration_ms;  /* boost window (when SET) */
} BoostAction;

typedef struct {
    BoostAction actions[TOUCH_FF_MAX_ACTIONS];
    int action_count;
    float global_schedtune_boost_pct; /* suggested global floor (not maxed) */
    int touch_boost_active;           /* bool: a touch boost window is live */
} TouchDecision;

typedef struct {
    float uclamp_ui_pct;        /* default 80 */
    float uclamp_render_pct;    /* default 80 */
    float uclamp_highcontrib_pct; /* default 60 */
    float uclamp_on_overrun_bonus;/* +this pct when frames overrun. default 10 */
    float global_idle_boost;    /* default 40 (was 80) */
    float global_active_boost; /* default 50 */
    uint32_t base_release_ms;   /* at 60 Hz. default 80 */
    uint32_t release_min_ms;    /* default 30 */
    uint32_t release_max_ms;    /* default 120 */
    float frame_overrun_margin; /* default 0.1 */
    float high_contrib_threshold;/* frame_contribution >= this boosts. default 0.5 */
} TouchConfig;

/* Persistent state: which tids are currently boosted and their expiry. */
typedef struct {
    int32_t  tid[TOUCH_FF_MAX_THREADS];
    uint32_t expiry_ms[TOUCH_FF_MAX_THREADS];
    uint32_t n_boosted;
    uint32_t last_touch_ms;
    int      ever_touched;
} TouchState;

void touch_config_default(TouchConfig *cfg);
void touch_state_init(TouchState *st);

/*
 * One decision per tick / touch event.
 *  now_ms    : monotonic ms.
 *  touch_active : 1 if a touch event arrived / is held this tick.
 *  threads   : foreground process thread set (may be NULL/empty).
 *  n_threads: number of valid entries in threads.
 *  refresh_hz: display refresh (0 => use base_release_ms).
 *  frame_actual_ms / frame_target_ms : frame timing (target<=0 => no check).
 */
void touch_decide(TouchState *st, const TouchConfig *cfg,
                  uint32_t now_ms, int touch_active,
                  const FgThread *threads, uint32_t n_threads,
                  float refresh_hz, float frame_actual_ms, float frame_target_ms,
                  TouchDecision *out);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_TOUCH_UCLAMP_H */

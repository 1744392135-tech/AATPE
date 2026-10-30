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
    TOUCH_UCLAMP_ROLE_UI_MAIN = 1,
    TOUCH_UCLAMP_ROLE_RENDER = 2,
    TOUCH_UCLAMP_ROLE_BG = 3
} TouchUclampRole;

typedef struct {
    int32_t tid;
    TouchUclampRole role;
    float frame_contribution;
} FgThread;

typedef enum {
    TOUCH_ACT_RELEASE = 0,
    TOUCH_ACT_SET = 1
} TouchAction;

typedef struct {
    int32_t tid;
    TouchAction action;
    float uclamp_min_pct;
    uint32_t duration_ms;
} BoostAction;

typedef struct {
    BoostAction actions[TOUCH_FF_MAX_ACTIONS];
    int action_count;
    float global_schedtune_boost_pct;
    int touch_boost_active;
} TouchDecision;

typedef struct {
    float uclamp_ui_pct;
    float uclamp_render_pct;
    float uclamp_highcontrib_pct;
    float uclamp_on_overrun_bonus;
    float global_idle_boost;
    float global_active_boost;
    uint32_t base_release_ms;
    uint32_t release_min_ms;
    uint32_t release_max_ms;
    float frame_overrun_margin;
    float high_contrib_threshold;
} TouchConfig;

typedef struct {
    int32_t  tid[TOUCH_FF_MAX_THREADS];
    uint32_t expiry_ms[TOUCH_FF_MAX_THREADS];
    uint32_t n_boosted;
    uint32_t last_touch_ms;
    int      ever_touched;
} TouchState;

void touch_config_default(TouchConfig *cfg);
void touch_state_init(TouchState *st);

void touch_decide(TouchState *st, const TouchConfig *cfg,
                  uint32_t now_ms, int touch_active,
                  const FgThread *threads, uint32_t n_threads,
                  float refresh_hz, float frame_actual_ms, float frame_target_ms,
                  TouchDecision *out);

#ifdef __cplusplus
}
#endif

#endif

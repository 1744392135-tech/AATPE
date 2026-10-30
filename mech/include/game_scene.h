#ifndef AATPE_GAME_SCENE_H
#define AATPE_GAME_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {

    float frame_actual_ms;
    float frame_target_ms;
    float frame_jitter_ms;

    float touch_events_per_tick;

    float cpu_util_pct;
    float gpu_util_pct;

    float io_pressure_pct;

    float tick_dt_ms;
} GameSceneSample;

typedef enum {
    GAME_UNKNOWN   = 0,
    GAME_LOADING   = 1,
    GAME_MENU      = 2,
    GAME_IN_GAME   = 3,
    GAME_CUTSCENE  = 4
} MechGameScene;

typedef struct {
    MechGameScene scene;
    float confidence;

    float boost_tendency;
    float frame_cap_tendency;

    uint32_t dwell_ticks;

    int just_changed;
} GameSceneDecision;

typedef struct {

    float loading_io_min;
    float loading_gpu_max;
    float loading_touch_max;

    float ingame_cpu_min;
    float ingame_gpu_min;
    float ingame_touch_min;

    float cutscene_gpu_min;
    float cutscene_touch_max;
    float cutscene_jitter_max;

    float menu_cpu_min;
    float menu_gpu_max;
    float menu_touch_min;

    float hysteresis_band_pct;
    uint32_t min_dwell_ticks;
    uint32_t debounce_ticks;

    float confidence_ewma_alpha;
} GameSceneConfig;

typedef struct {
    MechGameScene committed;
    MechGameScene candidate;
    uint32_t candidate_count;
    uint32_t dwell;
    float confidence;
} GameSceneState;

void game_scene_config_default(GameSceneConfig *cfg);
void game_scene_state_init(GameSceneState *st);

void game_scene_decide(const GameSceneSample *s, const GameSceneConfig *cfg,
                       GameSceneState *st, GameSceneDecision *out);

const char *game_scene_name(MechGameScene sc);

#ifdef __cplusplus
}
#endif

#endif

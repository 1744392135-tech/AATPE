/*
 * game_scene.h - Game scene classification state machine (pure logic).
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 * Caller owns all state; one tick per scheduler decision cycle.
 *
 * Scenes:
 *   GAME_UNKNOWN  - initial / insufficient evidence
 *   GAME_LOADING  - loading screen: high IO pressure, low GPU, no touch
 *   GAME_MENU     - menus/UI: moderate CPU, low GPU, touch present
 *   GAME_IN_GAME  - actual gameplay: high CPU+GPU, high touch rate
 *   GAME_CUTSCENE - cutscene/animatic: high GPU, no touch, smooth frames
 *
 * Each transition has:
 *   - entry / exit hysteresis band (avoid flapping at threshold)
 *   - min dwell ticks (cannot leave a state before N ticks)
 *   - debounce (candidate must persist M ticks before committing)
 *
 * Output includes per-scene boost/frame-cap tendency for actuator mapping.
 * This module NEVER writes a node; it only recommends.
 *
 * Design basis:
 *   - Blueprint 短板-5: game scene detection was coarse (4 threshold buckets,
 *     no engine-level differentiation, no confidence).
 *   - Reference: Apple ProMotion scene-based ramping; iQOO game-space profiles.
 *   - Loading detection via IO pressure (PSI io) + low GPU (disk-bound load).
 *   - Cutscene detection via high GPU but zero touch + stable frame pacing.
 */
#ifndef AATPE_GAME_SCENE_H
#define AATPE_GAME_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ------- input signals (one sample per tick) ------- */
typedef struct {
    /* Frame timing (ms). target <= 0 disables frame-based classification. */
    float frame_actual_ms;
    float frame_target_ms;
    float frame_jitter_ms;   /* recent frame-time std-dev or max-min (ms) */

    /* Touch: number of touch events observed in this tick window. */
    float touch_events_per_tick;

    /* Utilisation percentages [0..100]. */
    float cpu_util_pct;
    float gpu_util_pct;

    /* IO pressure [0..100] (PSI io some avg10). High => loading / disk-bound. */
    float io_pressure_pct;

    /* Tick wall-time delta in milliseconds. Must be > 0. */
    float tick_dt_ms;
} GameSceneSample;

/* ------- scene enum ------- */
typedef enum {
    GAME_UNKNOWN   = 0,
    GAME_LOADING   = 1,
    GAME_MENU      = 2,
    GAME_IN_GAME   = 3,
    GAME_CUTSCENE  = 4
} MechGameScene;

/* ------- output decision ------- */
typedef struct {
    MechGameScene scene;
    float confidence;          /* 0..1, how sure the classifier is */

    /* Actuator hints (normalised -1..+1):
     *   boost_tendency:    +1 = want max boost, 0 = neutral, -1 = de-boost
     *   frame_cap_tendency:+1 = cap framerate low (save power), 0 = neutral,
     *                       -1 = uncap / raise framerate ceiling
     */
    float boost_tendency;
    float frame_cap_tendency;

    /* Diagnostics: how many ticks we've been in the current committed state. */
    uint32_t dwell_ticks;

    /* True if the scene changed on this tick. */
    int just_changed;
} GameSceneDecision;

/* ------- tunables (all thresholds; defaults follow blueprint + references) ------- */
typedef struct {
    /* --- Loading detection --- */
    float loading_io_min;       /* IO pressure >= this => loading candidate. default 15.0 */
    float loading_gpu_max;       /* GPU util <= this while IO high => loading. default 30.0 */
    float loading_touch_max;     /* touch <= this => no interaction. default 0.5 */

    /* --- In-game detection --- */
    float ingame_cpu_min;       /* CPU >= this AND GPU >= this => in-game candidate. default 55.0 */
    float ingame_gpu_min;
    float ingame_touch_min;     /* touch rate >= this => active play. default 1.0 */

    /* --- Cutscene detection --- */
    float cutscene_gpu_min;     /* GPU >= this with no touch => cutscene. default 45.0 */
    float cutscene_touch_max;   /* touch <= this. default 0.2 */
    float cutscene_jitter_max;  /* jitter <= this => smooth cinematic. default 2.0 */

    /* --- Menu detection --- */
    float menu_cpu_min;         /* CPU >= this with touch and low GPU => menu. default 20.0 */
    float menu_gpu_max;         /* GPU <= this. default 35.0 */
    float menu_touch_min;       /* touch >= this. default 0.5 */

    /* --- Hysteresis & debounce --- */
    float hysteresis_band_pct;  /* exit threshold = enter - band (%). default 10.0 */
    uint32_t min_dwell_ticks;    /* cannot leave a state before N ticks. default 3 */
    uint32_t debounce_ticks;     /* candidate must persist M ticks. default 2 */

    /* --- Confidence smoothing --- */
    float confidence_ewma_alpha; /* EWMA alpha for confidence smoothing. default 0.3 */
} GameSceneConfig;

/* ------- persistent state (caller owns, zero-init or *_init once) ------- */
typedef struct {
    MechGameScene committed;        /* the state we've officially committed to */
    MechGameScene candidate;        /* the scene we're accumulating evidence for */
    uint32_t candidate_count;    /* consecutive ticks candidate has held */
    uint32_t dwell;              /* ticks in committed state */
    float confidence;            /* smoothed confidence [0..1] */
} GameSceneState;

/* ------- API ------- */
void game_scene_config_default(GameSceneConfig *cfg);
void game_scene_state_init(GameSceneState *st);

/*
 * Pure decision step. One call per scheduler tick.
 *   s   : current signal snapshot (must be non-NULL).
 *   cfg : thresholds (must be non-NULL, defaulted/overridden).
 *   st  : persistent state (must be non-NULL, initialised once).
 *   out : decision written here (must be non-NULL).
 *
 * All numeric inputs are sanitised: NaN/inf/out-of-range => treated as
 * "no signal" (0 for utilisation/touch, safe defaults for timing).
 * The module NEVER writes a node; the daemon's actuator maps the decision.
 */
void game_scene_decide(const GameSceneSample *s, const GameSceneConfig *cfg,
                       GameSceneState *st, GameSceneDecision *out);

/* Helper: human-readable scene name (static string). */
const char *game_scene_name(MechGameScene sc);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_GAME_SCENE_H */

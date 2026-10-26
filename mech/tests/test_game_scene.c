#include "test_harness.h"
#include "game_scene.h"

#include <math.h>

/* Helper: fill a "neutral" sample. */
static void neutral_sample(GameSceneSample *s)
{
    s->frame_actual_ms   = 16.67f;
    s->frame_target_ms   = 16.67f;
    s->frame_jitter_ms   = 1.0f;
    s->touch_events_per_tick = 0.0f;
    s->cpu_util_pct      = 10.0f;
    s->gpu_util_pct      = 5.0f;
    s->io_pressure_pct   = 0.0f;
    s->tick_dt_ms        = 50.0f;
}

/* ---- normal: loading -> in-game transition ---- */
static int test_normal_transition(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    /* Phase 1: loading (high IO, low GPU, no touch) */
    GameSceneSample s;
    neutral_sample(&s);
    s.io_pressure_pct = 40.0f;
    s.gpu_util_pct     = 10.0f;
    s.cpu_util_pct     = 20.0f;
    s.touch_events_per_tick = 0.0f;

    /* Feed enough ticks to pass debounce + first commit. */
    for (int i = 0; i < 5; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_LOADING);
    CHECK(dec.confidence > 0.0f);
    CHECK(dec.boost_tendency > 0.0f);

    /* Phase 2: switch to in-game (high CPU+GPU, high touch). */
    neutral_sample(&s);
    s.cpu_util_pct = 80.0f;
    s.gpu_util_pct = 75.0f;
    s.touch_events_per_tick = 3.0f;
    s.io_pressure_pct = 2.0f;

    /* Need min_dwell_ticks + debounce ticks before transition commits. */
    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_IN_GAME);
    CHECK(dec.boost_tendency > 0.5f);   /* gameplay => strong boost */
    CHECK(dec.frame_cap_tendency == 0.0f); /* no frame cap in game */

    return test_summary("game_scene normal transition");
}

/* ---- boundary: threshold edge values ---- */
static int test_boundary_thresholds(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);

    /* Exactly at loading IO threshold (15.0): should NOT trigger loading
     * because score requires io >= loading_io_min, and we set it exactly.
     * With default loading_io_min=15, 15.0 should be >= threshold. */
    s.io_pressure_pct = 15.0f;
    s.gpu_util_pct    = 25.0f;
    s.touch_events_per_tick = 0.0f;
    s.cpu_util_pct    = 15.0f;

    for (int i = 0; i < 5; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    /* At exactly threshold, score should be > 0 but low. */
    CHECK(dec.scene == GAME_LOADING);  /* 15 >= 15 triggers */
    CHECK(dec.confidence >= 0.0f);

    /* Just below threshold: should not trigger loading. */
    game_scene_state_init(&st);
    s.io_pressure_pct = 14.9f;
    for (int i = 0; i < 5; i++)
        game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene != GAME_LOADING);

    return test_summary("game_scene boundary thresholds");
}

/* ---- fault injection: NaN / inf / out-of-range ---- */
static int test_fault_injection(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);

    /* NaN everywhere */
    s.cpu_util_pct    = NAN;
    s.gpu_util_pct    = NAN;
    s.io_pressure_pct = NAN;
    s.touch_events_per_tick = NAN;
    s.frame_jitter_ms = NAN;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    /* All NaN => no scene should commit. */
    CHECK(dec.scene == GAME_UNKNOWN);
    CHECK(dec.confidence == 0.0f);
    CHECK(dec.boost_tendency == 0.0f);

    /* Inf values */
    game_scene_state_init(&st);
    neutral_sample(&s);
    s.cpu_util_pct    = INFINITY;
    s.gpu_util_pct    = INFINITY;
    s.io_pressure_pct = INFINITY;
    s.touch_events_per_tick = INFINITY;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    /* Inf utilisation should be clamped to 100, not crash. */
    CHECK(isfinite(dec.confidence));
    CHECK(dec.confidence >= 0.0f && dec.confidence <= 1.0f);

    /* Negative values */
    game_scene_state_init(&st);
    neutral_sample(&s);
    s.cpu_util_pct    = -50.0f;
    s.gpu_util_pct    = -20.0f;
    s.io_pressure_pct = -10.0f;
    s.touch_events_per_tick = -1.0f;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_UNKNOWN);
    CHECK(dec.confidence == 0.0f);

    /* NULL pointers should not crash. */
    game_scene_decide(NULL, &cfg, &st, &dec);
    game_scene_decide(&s, NULL, &st, &dec);
    game_scene_decide(&s, &cfg, NULL, &dec);
    game_scene_decide(&s, &cfg, &st, NULL);

    CHECK(1); /* if we got here, no crash */

    return test_summary("game_scene fault injection");
}

/* ---- hysteresis: scene should not flap at threshold ---- */
static int test_hysteresis_debounce(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    cfg.debounce_ticks = 3;
    cfg.min_dwell_ticks = 5;
    game_scene_state_init(&st);

    /* Start with clear in-game signal. */
    GameSceneSample s;
    neutral_sample(&s);
    s.cpu_util_pct = 80.0f;
    s.gpu_util_pct = 75.0f;
    s.touch_events_per_tick = 3.0f;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene == GAME_IN_GAME);

    /* Now drop CPU just below threshold: should NOT immediately leave in-game
     * because min_dwell hasn't passed AND debounce needs 3 ticks of new signal. */
    s.cpu_util_pct = 30.0f;
    s.gpu_util_pct = 10.0f;
    s.touch_events_per_tick = 0.0f;

    /* Only 1 tick of new signal: still in-game. */
    game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene == GAME_IN_GAME);

    /* 2 more ticks (total 3): debounce reached but min_dwell since last change
     * may not have passed. Actually dwell resets on change, and we need 5 ticks
     * of dwelling. Since we just changed at tick 10, dwell=0 there. Let's
     * verify that after 3 ticks of low signal, we're still holding. */
    game_scene_decide(&s, &cfg, &st, &dec);
    game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene == GAME_IN_GAME);

    return test_summary("game_scene hysteresis/debounce");
}

/* ---- long sequence: confidence bounded, no overflow ---- */
static int test_long_sequence_bounded(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);

    /* Alternate between loading and in-game every tick for 1000 ticks.
     * Confidence should stay bounded [0,1], no NaN, no crash. */
    for (int i = 0; i < 1000; i++) {
        if (i % 2 == 0) {
            s.io_pressure_pct = 40.0f;
            s.gpu_util_pct = 10.0f;
            s.cpu_util_pct = 20.0f;
            s.touch_events_per_tick = 0.0f;
        } else {
            s.cpu_util_pct = 80.0f;
            s.gpu_util_pct = 75.0f;
            s.touch_events_per_tick = 3.0f;
            s.io_pressure_pct = 2.0f;
        }
        game_scene_decide(&s, &cfg, &st, &dec);
        CHECK(isfinite(dec.confidence));
        CHECK(dec.confidence >= 0.0f && dec.confidence <= 1.0f);
        CHECK(dec.boost_tendency >= -1.0f && dec.boost_tendency <= 1.0f);
    }

    return test_summary("game_scene long sequence bounded");
}

/* ---- cutscene detection ---- */
static int test_cutscene_detection(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);
    s.gpu_util_pct = 70.0f;
    s.cpu_util_pct = 30.0f;
    s.touch_events_per_tick = 0.0f;
    s.frame_jitter_ms = 0.5f;
    s.io_pressure_pct = 1.0f;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_CUTSCENE);
    CHECK(dec.frame_cap_tendency > 0.0f); /* cutscene => suggest frame cap */

    return test_summary("game_scene cutscene detection");
}

int main(void)
{
    int fails = 0;
    fails += test_normal_transition();
    fails += test_boundary_thresholds();
    fails += test_fault_injection();
    fails += test_hysteresis_debounce();
    fails += test_long_sequence_bounded();
    fails += test_cutscene_detection();
    printf("== game_scene ALL: %s ==\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

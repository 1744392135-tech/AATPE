#include "test_harness.h"
#include "game_scene.h"

#include <math.h>

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

static int test_normal_transition(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);
    s.io_pressure_pct = 40.0f;
    s.gpu_util_pct     = 10.0f;
    s.cpu_util_pct     = 20.0f;
    s.touch_events_per_tick = 0.0f;

    for (int i = 0; i < 5; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_LOADING);
    CHECK(dec.confidence > 0.0f);
    CHECK(dec.boost_tendency > 0.0f);

    neutral_sample(&s);
    s.cpu_util_pct = 80.0f;
    s.gpu_util_pct = 75.0f;
    s.touch_events_per_tick = 3.0f;
    s.io_pressure_pct = 2.0f;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_IN_GAME);
    CHECK(dec.boost_tendency > 0.5f);
    CHECK(dec.frame_cap_tendency == 0.0f);

    return test_summary("game_scene normal transition");
}

static int test_boundary_thresholds(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);

    s.io_pressure_pct = 15.0f;
    s.gpu_util_pct    = 25.0f;
    s.touch_events_per_tick = 0.0f;
    s.cpu_util_pct    = 15.0f;

    for (int i = 0; i < 5; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_LOADING);
    CHECK(dec.confidence >= 0.0f);

    game_scene_state_init(&st);
    s.io_pressure_pct = 14.9f;
    for (int i = 0; i < 5; i++)
        game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene != GAME_LOADING);

    return test_summary("game_scene boundary thresholds");
}

static int test_fault_injection(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);

    s.cpu_util_pct    = NAN;
    s.gpu_util_pct    = NAN;
    s.io_pressure_pct = NAN;
    s.touch_events_per_tick = NAN;
    s.frame_jitter_ms = NAN;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(dec.scene == GAME_UNKNOWN);
    CHECK(dec.confidence == 0.0f);
    CHECK(dec.boost_tendency == 0.0f);

    game_scene_state_init(&st);
    neutral_sample(&s);
    s.cpu_util_pct    = INFINITY;
    s.gpu_util_pct    = INFINITY;
    s.io_pressure_pct = INFINITY;
    s.touch_events_per_tick = INFINITY;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);

    CHECK(isfinite(dec.confidence));
    CHECK(dec.confidence >= 0.0f && dec.confidence <= 1.0f);

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

    game_scene_decide(NULL, &cfg, &st, &dec);
    game_scene_decide(&s, NULL, &st, &dec);
    game_scene_decide(&s, &cfg, NULL, &dec);
    game_scene_decide(&s, &cfg, &st, NULL);

    CHECK(1);

    return test_summary("game_scene fault injection");
}

static int test_hysteresis_debounce(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    cfg.debounce_ticks = 3;
    cfg.min_dwell_ticks = 5;
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);
    s.cpu_util_pct = 80.0f;
    s.gpu_util_pct = 75.0f;
    s.touch_events_per_tick = 3.0f;

    for (int i = 0; i < 10; i++)
        game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene == GAME_IN_GAME);

    s.cpu_util_pct = 30.0f;
    s.gpu_util_pct = 10.0f;
    s.touch_events_per_tick = 0.0f;

    game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene == GAME_IN_GAME);

    game_scene_decide(&s, &cfg, &st, &dec);
    game_scene_decide(&s, &cfg, &st, &dec);
    CHECK(dec.scene == GAME_IN_GAME);

    return test_summary("game_scene hysteresis/debounce");
}

static int test_long_sequence_bounded(void)
{
    GameSceneConfig cfg;
    GameSceneState  st;
    GameSceneDecision dec;
    game_scene_config_default(&cfg);
    game_scene_state_init(&st);

    GameSceneSample s;
    neutral_sample(&s);

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
    CHECK(dec.frame_cap_tendency > 0.0f);

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

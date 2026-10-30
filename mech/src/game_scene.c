#include "game_scene.h"

#include <math.h>

static float sanf_pct(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

static float sanf_touch(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    return v;
}

static float sanf_ms(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    return v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void game_scene_config_default(GameSceneConfig *cfg)
{
    if (!cfg) return;
    cfg->loading_io_min      = 15.0f;
    cfg->loading_gpu_max     = 30.0f;
    cfg->loading_touch_max   = 0.5f;

    cfg->ingame_cpu_min      = 55.0f;
    cfg->ingame_gpu_min      = 50.0f;
    cfg->ingame_touch_min    = 1.0f;

    cfg->cutscene_gpu_min    = 45.0f;
    cfg->cutscene_touch_max  = 0.2f;
    cfg->cutscene_jitter_max = 2.0f;

    cfg->menu_cpu_min        = 20.0f;
    cfg->menu_gpu_max        = 35.0f;
    cfg->menu_touch_min      = 0.5f;

    cfg->hysteresis_band_pct = 10.0f;
    cfg->min_dwell_ticks     = 3;
    cfg->debounce_ticks      = 2;
    cfg->confidence_ewma_alpha = 0.3f;
}

void game_scene_state_init(GameSceneState *st)
{
    if (!st) return;
    st->committed   = GAME_UNKNOWN;
    st->candidate   = GAME_UNKNOWN;
    st->candidate_count = 0;
    st->dwell       = 0;
    st->confidence  = 0.0f;
}

static float score_loading(const GameSceneSample *s, const GameSceneConfig *c)
{
    float io   = sanf_pct(s->io_pressure_pct);
    float gpu  = sanf_pct(s->gpu_util_pct);
    float tch  = sanf_touch(s->touch_events_per_tick);

    if (io < c->loading_io_min) return 0.0f;
    if (gpu > c->loading_gpu_max) return 0.0f;
    if (tch > c->loading_touch_max) return 0.0f;

    float io_score  = clampf((io - c->loading_io_min) / 30.0f, 0.0f, 1.0f);
    float gpu_score = clampf((c->loading_gpu_max - gpu) / 20.0f, 0.0f, 1.0f);
    return clampf(0.4f + 0.3f * io_score + 0.3f * gpu_score, 0.0f, 1.0f);
}

static float score_ingame(const GameSceneSample *s, const GameSceneConfig *c)
{
    float cpu  = sanf_pct(s->cpu_util_pct);
    float gpu  = sanf_pct(s->gpu_util_pct);
    float tch  = sanf_touch(s->touch_events_per_tick);

    float cpu_thr  = c->ingame_cpu_min;
    float gpu_thr  = c->ingame_gpu_min;
    float tch_thr  = c->ingame_touch_min;

    if (cpu < cpu_thr) return 0.0f;
    if (gpu < gpu_thr) return 0.0f;
    if (tch < tch_thr) return 0.0f;

    float cpu_s = clampf((cpu - cpu_thr) / 30.0f, 0.0f, 1.0f);
    float gpu_s = clampf((gpu - gpu_thr) / 30.0f, 0.0f, 1.0f);
    float tch_s = clampf(tch / 5.0f, 0.0f, 1.0f);
    return clampf(0.4f + 0.25f * cpu_s + 0.25f * gpu_s + 0.1f * tch_s, 0.0f, 1.0f);
}

static float score_cutscene(const GameSceneSample *s, const GameSceneConfig *c)
{
    float gpu  = sanf_pct(s->gpu_util_pct);
    float tch  = sanf_touch(s->touch_events_per_tick);
    float jit  = sanf_ms(s->frame_jitter_ms);

    if (gpu < c->cutscene_gpu_min) return 0.0f;
    if (tch > c->cutscene_touch_max) return 0.0f;
    if (jit > c->cutscene_jitter_max) return 0.0f;

    float gpu_s = clampf((gpu - c->cutscene_gpu_min) / 30.0f, 0.0f, 1.0f);
    return clampf(0.4f + 0.4f * gpu_s, 0.0f, 1.0f);
}

static float score_menu(const GameSceneSample *s, const GameSceneConfig *c)
{
    float cpu  = sanf_pct(s->cpu_util_pct);
    float gpu  = sanf_pct(s->gpu_util_pct);
    float tch  = sanf_touch(s->touch_events_per_tick);

    if (cpu < c->menu_cpu_min) return 0.0f;
    if (gpu > c->menu_gpu_max) return 0.0f;
    if (tch < c->menu_touch_min) return 0.0f;

    float cpu_s = clampf((cpu - c->menu_cpu_min) / 30.0f, 0.0f, 1.0f);
    return clampf(0.4f + 0.3f * cpu_s, 0.0f, 1.0f);
}

static void scene_tendency(MechGameScene sc, float *boost, float *frame_cap)
{
    switch (sc) {
    case GAME_LOADING:

        *boost     = 0.3f;
        *frame_cap = 0.0f;
        break;
    case GAME_MENU:

        *boost     = 0.2f;
        *frame_cap = 0.0f;
        break;
    case GAME_IN_GAME:

        *boost     = 1.0f;
        *frame_cap = 0.0f;
        break;
    case GAME_CUTSCENE:

        *boost     = 0.1f;
        *frame_cap = 0.6f;
        break;
    case GAME_UNKNOWN:
    default:
        *boost     = 0.0f;
        *frame_cap = 0.0f;
        break;
    }
}

const char *game_scene_name(MechGameScene sc)
{
    switch (sc) {
    case GAME_LOADING:  return "LOADING";
    case GAME_MENU:     return "MENU";
    case GAME_IN_GAME:  return "IN_GAME";
    case GAME_CUTSCENE: return "CUTSCENE";
    case GAME_UNKNOWN:
    default:            return "UNKNOWN";
    }
}

void game_scene_decide(const GameSceneSample *s, const GameSceneConfig *cfg,
                       GameSceneState *st, GameSceneDecision *out)
{

    if (out) {
        out->scene           = GAME_UNKNOWN;
        out->confidence      = 0.0f;
        out->boost_tendency  = 0.0f;
        out->frame_cap_tendency = 0.0f;
        out->dwell_ticks      = 0;
        out->just_changed    = 0;
    }
    if (!s || !cfg || !st || !out) return;

    float s_load = score_loading(s, cfg);
    float s_menu = score_menu(s, cfg);
    float s_game = score_ingame(s, cfg);
    float s_cut  = score_cutscene(s, cfg);

    MechGameScene best = GAME_UNKNOWN;
    float best_score = 0.0f;

    if (s_load > best_score) { best = GAME_LOADING; best_score = s_load; }
    if (s_game > best_score) { best = GAME_IN_GAME; best_score = s_game; }
    if (s_cut  > best_score) { best = GAME_CUTSCENE; best_score = s_cut; }
    if (s_menu > best_score) { best = GAME_MENU; best_score = s_menu; }

    if (best == st->candidate) {
        st->candidate_count++;
    } else {
        st->candidate = best;
        st->candidate_count = 1;
    }

    st->dwell++;

    float band_pct = cfg->hysteresis_band_pct;
    if (band_pct < 0.0f) band_pct = 0.0f;
    if (band_pct > 50.0f) band_pct = 50.0f;

    int should_change = 0;
    if (best != GAME_UNKNOWN && st->candidate_count >= cfg->debounce_ticks) {
        if (st->committed == GAME_UNKNOWN) {

            should_change = 1;
        } else if (st->dwell >= cfg->min_dwell_ticks) {

            if (best != st->committed) {
                float committed_score = 0.0f;
                switch (st->committed) {
                    case GAME_LOADING:  committed_score = s_load; break;
                    case GAME_IN_GAME:   committed_score = s_game; break;
                    case GAME_CUTSCENE:  committed_score = s_cut; break;
                    case GAME_MENU:      committed_score = s_menu; break;
                    default: break;
                }
                float band = committed_score * (band_pct / 100.0f);
                if (best_score >= committed_score + band) {
                    should_change = 1;
                }
            }
        }
    }

    if (should_change) {
        st->committed = best;
        st->dwell = 0;
        out->just_changed = 1;
    }

    float alpha = cfg->confidence_ewma_alpha;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    st->confidence = st->confidence + alpha * (best_score - st->confidence);

    out->scene      = st->committed;
    out->confidence = clampf(st->confidence, 0.0f, 1.0f);
    out->dwell_ticks = st->dwell;
    scene_tendency(st->committed, &out->boost_tendency, &out->frame_cap_tendency);
}

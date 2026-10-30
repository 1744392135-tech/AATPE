#include "touch_uclamp.h"

#include <math.h>

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int boosted_index(const TouchState *st, int32_t tid)
{
    for (uint32_t i = 0; i < st->n_boosted; i++)
        if (st->tid[i] == tid) return (int)i;
    return -1;
}

void touch_config_default(TouchConfig *cfg)
{
    if (!cfg) return;
    cfg->uclamp_ui_pct = 80.0f;
    cfg->uclamp_render_pct = 80.0f;
    cfg->uclamp_highcontrib_pct = 60.0f;
    cfg->uclamp_on_overrun_bonus = 10.0f;
    cfg->global_idle_boost = 40.0f;
    cfg->global_active_boost = 50.0f;
    cfg->base_release_ms = 80u;
    cfg->release_min_ms = 30u;
    cfg->release_max_ms = 120u;
    cfg->frame_overrun_margin = 0.10f;
    cfg->high_contrib_threshold = 0.5f;
}

void touch_state_init(TouchState *st)
{
    if (!st) return;
    for (uint32_t i = 0; i < TOUCH_FF_MAX_THREADS; i++) {
        st->tid[i] = -1;
        st->expiry_ms[i] = 0u;
    }
    st->n_boosted = 0;
    st->last_touch_ms = 0u;
    st->ever_touched = 0;
}

static uint32_t release_window_ms(const TouchConfig *cfg, float refresh_hz)
{
    uint32_t base = cfg->base_release_ms;
    if (!isfinite(refresh_hz) || refresh_hz <= 0.0f) return base;
    float rel = (float)base * (60.0f / refresh_hz);
    if (rel < (float)cfg->release_min_ms) rel = (float)cfg->release_min_ms;
    if (rel > (float)cfg->release_max_ms) rel = (float)cfg->release_max_ms;
    return (uint32_t)rel;
}

static void add_action(TouchDecision *out, BoostAction a)
{
    if (out->action_count < (int)TOUCH_FF_MAX_ACTIONS)
        out->actions[out->action_count++] = a;
}

void touch_decide(TouchState *st, const TouchConfig *cfg,
                  uint32_t now_ms, int touch_active,
                  const FgThread *threads, uint32_t n_threads,
                  float refresh_hz, float frame_actual_ms, float frame_target_ms,
                  TouchDecision *out)
{
    if (out) {
        out->action_count = 0;
        out->global_schedtune_boost_pct = cfg ? cfg->global_idle_boost : 40.0f;
        out->touch_boost_active = 0;
    }
    if (!st || !cfg || !out) return;

    for (uint32_t i = 0; i < st->n_boosted;) {
        if (now_ms >= st->expiry_ms[i]) {
            BoostAction a;
            a.tid = st->tid[i];
            a.action = TOUCH_ACT_RELEASE;
            a.uclamp_min_pct = 0.0f;
            a.duration_ms = 0u;
            add_action(out, a);

            st->tid[i] = st->tid[st->n_boosted - 1u];
            st->expiry_ms[i] = st->expiry_ms[st->n_boosted - 1u];
            st->n_boosted--;
        } else {
            i++;
        }
    }

    if (touch_active && touch_active != 0) {
        st->last_touch_ms = now_ms;
        st->ever_touched = 1;

        int overrun = 0;
        if (isfinite(frame_actual_ms) && isfinite(frame_target_ms) &&
            frame_target_ms > 0.0f &&
            frame_actual_ms > frame_target_ms * (1.0f + cfg->frame_overrun_margin))
            overrun = 1;

        uint32_t win = release_window_ms(cfg, refresh_hz);
        uint32_t expiry = now_ms + win;

        if (threads && n_threads > 0u) {
            for (uint32_t i = 0; i < n_threads && i < TOUCH_FF_MAX_THREADS; i++) {
                const FgThread *t = &threads[i];
                if (t->tid < 0) continue;

                float lvl = -1.0f;
                if (t->role == TOUCH_UCLAMP_ROLE_UI_MAIN)
                    lvl = cfg->uclamp_ui_pct;
                else if (t->role == TOUCH_UCLAMP_ROLE_RENDER)
                    lvl = cfg->uclamp_render_pct;
                else if (t->role == TOUCH_UCLAMP_ROLE_UNKNOWN &&
                         isfinite(t->frame_contribution) &&
                         t->frame_contribution >= cfg->high_contrib_threshold)
                    lvl = cfg->uclamp_highcontrib_pct;
                else
                    continue;

                if (overrun) lvl += cfg->uclamp_on_overrun_bonus;
                lvl = clampf(lvl, 0.0f, 100.0f);

                int idx = boosted_index(st, t->tid);
                if (idx < 0) {
                    if (st->n_boosted >= TOUCH_FF_MAX_THREADS) continue;
                    st->tid[st->n_boosted] = t->tid;
                    st->expiry_ms[st->n_boosted] = expiry;
                    st->n_boosted++;
                } else {
                    st->expiry_ms[idx] = expiry;
                }

                BoostAction a;
                a.tid = t->tid;
                a.action = TOUCH_ACT_SET;
                a.uclamp_min_pct = lvl;
                a.duration_ms = win;
                add_action(out, a);
            }
        }
    }

    int live = (st->n_boosted > 0u) || (touch_active && touch_active != 0);
    out->touch_boost_active = live ? 1 : 0;
    out->global_schedtune_boost_pct =
        live ? cfg->global_active_boost : cfg->global_idle_boost;
}

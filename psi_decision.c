#include "psi_decision.h"

#include <math.h>

/* Clamp a raw PSI percent / frame input to a safe finite range.
 * Non-finite (NaN/inf) and out-of-range values become "no signal":
 * negatives -> 0, >100 -> 100, NaN/inf -> 0.  We must never treat a bad
 * sample as overload. */
static float sanf_pct(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 100.0f) return 100.0f;
    return v;
}

static float sanf_ms(float v)
{
    if (!isfinite(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    return v;
}

static void push_reason(PsiDecision *d, PsiReason r)
{
    if (d->reason_count < PSI_MAX_REASONS)
        d->reasons[d->reason_count++] = r;
}

void psi_config_default(PsiConfig *cfg)
{
    if (!cfg) return;
    cfg->cpu_mild_avg10 = 10.0f;
    cfg->cpu_severe_avg10 = 25.0f;
    cfg->io_pressure_avg10 = 20.0f;
    cfg->mem_pressure_avg10 = 5.0f;
    cfg->hysteresis_band = 4.0f;
    cfg->frame_overrun_margin = 0.10f;
    cfg->trend_margin = 3.0f;
    cfg->enter_debounce = 2;
    cfg->exit_debounce = 4;
}

void psi_state_init(PsiState *st)
{
    if (!st) return;
    st->level = PSI_OK;
    st->consec_above = 0;
    st->consec_below = 0;
}

void psi_decide(const PsiSample *s, const PsiConfig *cfg,
                PsiState *st, PsiDecision *out)
{
    /* Zero the output; callers may rely on a clean struct even on bad input. */
    if (out) {
        out->overload = PSI_OK;
        out->boost = PSI_BOOST_NONE;
        out->downgrade_background = 0;
        out->pause_noncritical_migration = 0;
        out->io_pressure_downgrade = 0;
        out->mem_bigcore_deprefer = 0;
        out->bg_uclamp_reduction_pct = 0.0f;
        out->critical_uclamp_boost_pct = 0.0f;
        out->reason_count = 0;
    }
    if (!s || !cfg || !st || !out) return;

    /* --- sanitise every input --- */
    float cpu10  = sanf_pct(s->cpu_some_avg10);
    float cpu60  = sanf_pct(s->cpu_some_avg60);
    float cpu300 = sanf_pct(s->cpu_some_avg300);
    float io_s10 = sanf_pct(s->io_some_avg10);
    float io_f10 = sanf_pct(s->io_full_avg10);
    float io_s60 = sanf_pct(s->io_some_avg60);
    float mem_s10 = sanf_pct(s->mem_some_avg10);
    float mem_f10 = sanf_pct(s->mem_full_avg10);
    float fact = sanf_ms(s->frame_actual_ms);
    float ftgt = sanf_ms(s->frame_target_ms);
    (void)cpu300; /* reserved: long-window confirmation; not used in v1 logic */
    (void)io_s60; /* reserved */
    (void)mem_f10; /* reserved: full is a stronger mem signal, some drives it */

    /* --- primary scheduler-overload signal: cpu some avg10 --- */
    float mild_in   = cfg->cpu_mild_avg10;
    float severe_in = cfg->cpu_severe_avg10;
    float mild_out  = mild_in - cfg->hysteresis_band;
    float severe_out = severe_in - cfg->hysteresis_band;
    if (mild_out < 0.0f) mild_out = 0.0f;
    if (severe_out < 0.0f) severe_out = 0.0f;

    PsiOverload prev = st->level;

    /* --- hysteresis state machine with direction-debounce --- */
    switch (st->level) {
    case PSI_OK:
        if (cpu10 >= severe_in) {
            st->consec_above++; st->consec_below = 0;
            if (st->consec_above >= cfg->enter_debounce) st->level = PSI_OVERLOAD_SEVERE;
        } else if (cpu10 >= mild_in) {
            st->consec_above++; st->consec_below = 0;
            if (st->consec_above >= cfg->enter_debounce) st->level = PSI_OVERLOAD_MILD;
        } else {
            st->consec_above = 0;
        }
        break;
    case PSI_OVERLOAD_MILD:
        if (cpu10 >= severe_in) {
            st->consec_above++; st->consec_below = 0;
            if (st->consec_above >= cfg->enter_debounce) st->level = PSI_OVERLOAD_SEVERE;
        } else if (cpu10 < mild_out) {
            st->consec_below++; st->consec_above = 0;
            if (st->consec_below >= cfg->exit_debounce) st->level = PSI_OK;
        } else {
            /* holding inside the mild hysteresis band */
            st->consec_above = 0; st->consec_below = 0;
        }
        break;
    case PSI_OVERLOAD_SEVERE:
        if (cpu10 < severe_out) {
            st->consec_below++; st->consec_above = 0;
            if (st->consec_below >= cfg->exit_debounce)
                st->level = (cpu10 >= mild_out) ? PSI_OVERLOAD_MILD : PSI_OK;
        } else {
            st->consec_below = 0;
        }
        break;
    }

    PsiOverload lvl = st->level;
    out->overload = lvl;

    /* --- frame overrun (user-visible) --- */
    int frame_overrun = 0;
    if (ftgt > 0.0f && fact > ftgt * (1.0f + cfg->frame_overrun_margin))
        frame_overrun = 1;

    /* --- boost / downgrade mapping --- */
    if (lvl == PSI_OVERLOAD_SEVERE) {
        out->boost = PSI_BOOST_AGGRESSIVE;
        out->critical_uclamp_boost_pct = 80.0f;
        out->bg_uclamp_reduction_pct = 50.0f;
        out->downgrade_background = 1;
        out->pause_noncritical_migration = 1;
    } else if (lvl == PSI_OVERLOAD_MILD) {
        out->downgrade_background = 1;
        out->bg_uclamp_reduction_pct = 20.0f;
        if (frame_overrun) {
            out->boost = PSI_BOOST_MODERATE;
            out->critical_uclamp_boost_pct = 60.0f;
        } else {
            out->boost = PSI_BOOST_NONE;
        }
    } else {
        out->boost = PSI_BOOST_NONE;
    }

    /* --- independent IO / memory pressure hooks --- */
    float io_pressure = io_s10 > io_f10 ? io_s10 : io_f10;
    if (io_pressure >= cfg->io_pressure_avg10) out->io_pressure_downgrade = 1;
    if (mem_s10 >= cfg->mem_pressure_avg10) out->mem_bigcore_deprefer = 1;

    /* --- explainable reasons --- */
    int rising = (cpu10 > cpu60 + cfg->trend_margin) ? 1 : 0;

    if (lvl == PSI_OVERLOAD_SEVERE)
        push_reason(out, PSI_REASON_CPU_STALL_SEVERE);
    else if (lvl == PSI_OVERLOAD_MILD)
        push_reason(out, PSI_REASON_CPU_STALL);

    if (rising && lvl != PSI_OK)
        push_reason(out, PSI_REASON_CPU_STALL_RISING);
    if (frame_overrun && lvl != PSI_OK)
        push_reason(out, PSI_REASON_FRAME_OVERRUN);
    if (out->io_pressure_downgrade)
        push_reason(out, PSI_REASON_IO_PRESSURE);
    if (out->mem_bigcore_deprefer)
        push_reason(out, PSI_REASON_MEM_PRESSURE);

    if (lvl == prev) {
        /* no (de)escalation this tick */
        if (lvl != PSI_OK) push_reason(out, PSI_REASON_HYSTERESIS_HOLD);
    } else if (lvl < prev) {
        push_reason(out, PSI_REASON_RELEASE);
    }
    /* escalation: reasons already name the new level */
}

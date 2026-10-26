/*
 * psi_decision.h - PSI (Pressure Stall Information) decision closed-loop core.
 *
 * PURE LOGIC. No product dependency, no sysfs, no global state. Host-testable.
 *
 * Input : PSI cpu/io/mem stall windows (some/full avg10/60/300) + frame
 *         actual/target period.
 * Output: an explainable overload / boost / background-downgrade suggestion.
 *
 * Design basis (see docs/INTEGRATION.md for provenance):
 *   - Blueprint P0-1: cpu some avg10>10% -> scheduler overload (lower bg uclamp,
 *     pause non-critical migration); io some avg10>20% -> cold-start IO
 *     downgrade hook; mem some avg10>5% -> deprefer big-core migration.
 *   - /proc/pressure/cpu exposes "some" only; io & memory expose "some"+"full".
 *
 * Hysteresis + debounce live in PsiState so the decision does not flap at the
 * threshold. All numeric inputs are sanitised (NaN/inf/out-of-range => treated
 * as "no signal", never as overload). The module NEVER writes a node; the
 * daemon's actuator maps PsiDecision -> affinity/uclamp/schedtune.
 */
#ifndef AATPE_PSI_DECISION_H
#define AATPE_PSI_DECISION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* All pressure values are percent in [0,100]. Frame times are milliseconds. */
typedef struct {
    /* /proc/pressure/cpu  -> "some" line (cpu has no "full" line) */
    float cpu_some_avg10;
    float cpu_some_avg60;
    float cpu_some_avg300;

    /* /proc/pressure/io -> "some" and "full" lines */
    float io_some_avg10;
    float io_full_avg10;
    float io_some_avg60;

    /* /proc/pressure/memory -> "some" and "full" lines */
    float mem_some_avg10;
    float mem_full_avg10;

    /* Frame timing. frame_target_ms<=0 disables the frame-overrun check. */
    float frame_actual_ms;
    float frame_target_ms;
} PsiSample;

typedef enum {
    PSI_OK = 0,            /* no scheduler overload */
    PSI_OVERLOAD_MILD = 1, /* short runqueue stall; free capacity for critical */
    PSI_OVERLOAD_SEVERE = 2 /* sustained stall; aggressively shed non-critical work */
} PsiOverload;

typedef enum {
    PSI_BOOST_NONE = 0,
    PSI_BOOST_MODERATE = 1, /* raise critical-thread uclamp */
    PSI_BOOST_AGGRESSIVE = 2 /* raise critical uclamp hard + shed background */
} PsiBoost;

typedef enum {
    PSI_REASON_NONE = 0,
    PSI_REASON_CPU_STALL,        /* cpu some avg10 above mild threshold */
    PSI_REASON_CPU_STALL_SEVERE, /* cpu some avg10 above severe threshold */
    PSI_REASON_CPU_STALL_RISING, /* avg10 > avg60+margin: stall building */
    PSI_REASON_FRAME_OVERRUN,    /* actual frame time exceeds target*(1+m) */
    PSI_REASON_IO_PRESSURE,      /* io some/full above threshold */
    PSI_REASON_MEM_PRESSURE,     /* mem some above threshold */
    PSI_REASON_HYSTERESIS_HOLD,  /* currently holding previous (de)escalation */
    PSI_REASON_RELEASE           /* hysteresis band released back to OK */
} PsiReason;

#define PSI_MAX_REASONS 8

typedef struct {
    PsiOverload overload;
    PsiBoost    boost;

    int downgrade_background;          /* bool: lower background uclamp */
    int pause_noncritical_migration;     /* bool: stop shuffling non-critical tids */
    int io_pressure_downgrade;          /* bool: feed cold-start IO-pressure hook */
    int mem_bigcore_deprefer;          /* bool: avoid big-core placement under mem pressure */

    float bg_uclamp_reduction_pct;      /* 0..100 suggested bg uclamp cut */
    float critical_uclamp_boost_pct;  /* 0..100 suggested critical uclamp raise */

    PsiReason reasons[PSI_MAX_REASONS];
    int reason_count;
} PsiDecision;

/* Tunables. Defaults follow the blueprint; the daemon may override per device. */
typedef struct {
    float cpu_mild_avg10;      /* default 10.0 (%) */
    float cpu_severe_avg10;    /* default 25.0 (%) */
    float io_pressure_avg10;   /* default 20.0 (%) */
    float mem_pressure_avg10;  /* default 5.0 (%) */

    float hysteresis_band;     /* enter at thr, exit at thr-band (%). default 4.0 */
    float frame_overrun_margin;/* actual > target*(1+margin) => overrun. default 0.10 */
    float trend_margin;        /* avg10-avg60 > this => "rising". default 3.0 (%) */

    uint32_t enter_debounce;    /* consecutive samples needed to (de)escalate up */
    uint32_t exit_debounce;     /* consecutive samples needed to relax down */
} PsiConfig;

/* Opaque-ish persistent state across decision ticks. Caller owns it. */
typedef struct {
    PsiOverload level;
    uint32_t consec_above; /* samples at/above the current entry threshold */
    uint32_t consec_below; /* samples below the exit threshold */
} PsiState;

void psi_config_default(PsiConfig *cfg);
void psi_state_init(PsiState *st);

/*
 * Pure decision step. One call per scheduler tick.
 *  s   : current signal snapshot (must be non-NULL).
 *  cfg : thresholds (must be non-NULL, already defaulted/overridden).
 *  st  : persistent state (must be non-NULL, initialised once).
 *  out : decision written here (must be non-NULL).
 */
void psi_decide(const PsiSample *s, const PsiConfig *cfg,
                PsiState *st, PsiDecision *out);

#ifdef __cplusplus
}
#endif

#endif /* AATPE_PSI_DECISION_H */

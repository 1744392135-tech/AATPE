#ifndef AATPE_PSI_DECISION_H
#define AATPE_PSI_DECISION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {

    float cpu_some_avg10;
    float cpu_some_avg60;
    float cpu_some_avg300;

    float io_some_avg10;
    float io_full_avg10;
    float io_some_avg60;

    float mem_some_avg10;
    float mem_full_avg10;

    float frame_actual_ms;
    float frame_target_ms;
} PsiSample;

typedef enum {
    PSI_OK = 0,
    PSI_OVERLOAD_MILD = 1,
    PSI_OVERLOAD_SEVERE = 2
} PsiOverload;

typedef enum {
    PSI_BOOST_NONE = 0,
    PSI_BOOST_MODERATE = 1,
    PSI_BOOST_AGGRESSIVE = 2
} PsiBoost;

typedef enum {
    PSI_REASON_NONE = 0,
    PSI_REASON_CPU_STALL,
    PSI_REASON_CPU_STALL_SEVERE,
    PSI_REASON_CPU_STALL_RISING,
    PSI_REASON_FRAME_OVERRUN,
    PSI_REASON_IO_PRESSURE,
    PSI_REASON_MEM_PRESSURE,
    PSI_REASON_HYSTERESIS_HOLD,
    PSI_REASON_RELEASE
} PsiReason;

#define PSI_MAX_REASONS 8

typedef struct {
    PsiOverload overload;
    PsiBoost    boost;

    int downgrade_background;
    int pause_noncritical_migration;
    int io_pressure_downgrade;
    int mem_bigcore_deprefer;

    float bg_uclamp_reduction_pct;
    float critical_uclamp_boost_pct;

    PsiReason reasons[PSI_MAX_REASONS];
    int reason_count;
} PsiDecision;

typedef struct {
    float cpu_mild_avg10;
    float cpu_severe_avg10;
    float io_pressure_avg10;
    float mem_pressure_avg10;

    float hysteresis_band;
    float frame_overrun_margin;
    float trend_margin;

    uint32_t enter_debounce;
    uint32_t exit_debounce;
} PsiConfig;

typedef struct {
    PsiOverload level;
    uint32_t consec_above;
    uint32_t consec_below;
} PsiState;

void psi_config_default(PsiConfig *cfg);
void psi_state_init(PsiState *st);

void psi_decide(const PsiSample *s, const PsiConfig *cfg,
                PsiState *st, PsiDecision *out);

#ifdef __cplusplus
}
#endif

#endif

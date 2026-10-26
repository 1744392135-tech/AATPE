// adrc.h - Active Disturbance Rejection Control with ML fusion
// Implements TD (Tracking Differentiator), ESO (Extended State Observer),
// NLSEF (Nonlinear State Error Feedback), and disturbance compensation.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace aatpe {
namespace adrc {

// ADRC tuning parameters (all configurable via aatpe.conf)
struct AdrcParams {
    double h;           // sample period (seconds), default 0.05 (50ms)
    double r;           // tracking speed factor, default 50
    double beta01;      // ESO bandwidth 1, default 100
    double beta02;      // ESO bandwidth 2, default 300
    double beta03;      // ESO bandwidth 3, default 1000
    double beta1;       // NLSEF gain 1, default 10
    double beta2;       // NLSEF gain 2, default 0.5
    double alpha1;      // NLSEF nonlinear power 1, default 0.75
    double alpha2;      // NLSEF nonlinear power 2, default 1.25
    double delta;       // linear interval threshold, default 0.01
    double b0;          // control gain, default 1.0
    // fusion weights
    double adrcWeightBase;  // base ADRC weight in fusion, default 0.4
    double mlWeightBase;    // base ML weight in fusion, default 0.6
    // thresholds for boolean outputs
    double urgencyThreshold;    // default 0.5
    double riskScoreThreshold;  // default 0.6
    double clampFrameRatio;     // frameTime < target * ratio -> clamp, default 0.7
};

// Default ADRC parameters (inline constexpr: one shared instance across TUs,
// no per-TU ODR-possible copy).
inline constexpr AdrcParams kDefaultAdrcParams{
    /*h=*/           0.05,
    /*r=*/           50.0,
    /*beta01=*/      100.0,
    /*beta02=*/      300.0,
    /*beta03=*/      1000.0,
    /*beta1=*/       10.0,
    /*beta2=*/       0.5,
    /*alpha1=*/      0.75,
    /*alpha2=*/      1.25,
    /*delta=*/       0.01,
    /*b0=*/          1.0,
    /*adrcWeight=*/  0.4,
    /*mlWeight=*/     0.6,
    /*urgencyThr=*/  0.5,
    /*riskThr=*/     0.6,
    /*clampRatio=*/  0.7
};

// Per-thread-role ADRC instance roles
enum class AdrcRole {
    UI = 0,
    RENDER = 1,
    GAME = 2,
    COUNT = 3
};

// ADRC input: frame time observation
struct AdrcInput {
    double frameTimeUs;         // current measured frame time (microseconds)
    double targetFrameTimeUs;   // target frame time (microseconds)
    double cpuLoad;             // current CPU load (0-1)
    double thermalLevel;        // thermal level (0-1, 0=cool, 1=critical)
    double mlSignal;            // ML feedback signal (0-1, higher = more boost needed)
    double mlConfidence;        // ML confidence (0-1)
};

// ADRC output: control decisions
struct AdrcOutput {
    double urgency;             // 0-1, how far current frame time is from target
    double riskScore;            // 0-1, predicted jank risk based on ESO disturbance
    bool   shouldBoost;         // boost performance (migrate to big cores)
    bool   shouldClamp;         // clamp performance (migrate to little cores, save power)
    double predictedFrameTime;   // predicted next frame time (microseconds)
    double fusedSignal;         // ADRC + ML fused decision signal (0-1)
    double adrcWeight;          // actual ADRC weight used in fusion
    double mlWeight;            // actual ML weight used in fusion
    double disturbance;         // estimated total disturbance z3
    double trackingError;       // tracking error e1
    double errorDerivative;     // error derivative e2
};

// Tracking Differentiator state (TD)
struct TdState {
    double v1;  // tracked signal
    double v2;  // differential of tracked signal
    void reset() { v1 = 0.0; v2 = 0.0; }
};

// Extended State Observer state (ESO)
struct EsoState {
    double z1;  // estimated frame time
    double z2;  // estimated frame time derivative
    double z3;  // estimated total disturbance
    void reset() { z1 = 0.0; z2 = 0.0; z3 = 0.0; }
};

// Single ADRC controller instance
class AdrcController {
public:
    AdrcController();
    explicit AdrcController(const AdrcParams& params);

    void setParams(const AdrcParams& params);
    // P2: return by value under lock — callers previously got an unlocked
    // reference to m_params and could tear-read it racing setParams().
    AdrcParams getParams() const { std::lock_guard<std::mutex> lock(m_mutex); return m_params; }

    void reset();
    AdrcOutput update(const AdrcInput& input);

    // thread-safe snapshot of last output
    AdrcOutput getLastOutput() const;

private:
    // Tracking Differentiator (Han's discrete TD with fastest synthesis)
    void tdStep(double target, double h);

    // Nonlinear state error feedback function fal()
    // fal(e, alpha, delta) = |e|^alpha * sign(e)  when |e| > delta
    //                        = e / delta^(1-alpha)   when |e| <= delta
    double fal(double e, double alpha, double delta) const;

    // Extended State Observer (second-order nonlinear ESO)
    void esoStep(double measured, double control, double h);

    // Nonlinear State Error Feedback
    double nlsefStep(double e1, double e2) const;

    // Compute dynamic fusion weights based on confidence
    void computeFusionWeights(double adrcConfidence, double mlConfidence,
                               double& outAdrcWeight, double& outMlWeight) const;

    // G5-005: shared divide-by-zero / instability clamp used by BOTH the
    // (params) constructor and setParams(), so a caller passing b0=0 / h=0 / r=0
    // via the constructor can never reach update() with unclamped values.
    static AdrcParams clampParams(const AdrcParams& in, const AdrcParams& fallback);

    AdrcParams m_params;
    TdState m_td;
    EsoState m_eso;
    bool m_initialized;

    mutable std::mutex m_mutex;
    AdrcOutput m_lastOutput;
};

// Global ADRC subsystem API
void init();
void reset();
void configure(const AdrcParams& params);
AdrcParams getConfig();

// Update ADRC for a specific thread role (called from frame loop / main loop)
AdrcOutput updateRole(AdrcRole role, const AdrcInput& input);

// Get last ADRC output for a role (thread-safe, called from placement/scoring)
AdrcOutput getRoleOutput(AdrcRole role);

// Get global risk score aggregate (max across all active roles)
double getAggregateRiskScore();

// Check if any role needs boost
bool anyShouldBoost();

// Check if all roles can clamp (system has headroom)
bool allCanClamp();

// Get predicted frame time (average across roles)
double getPredictedFrameTimeUs();

// Map thread role to ADRC role
AdrcRole mapThreadRoleToAdrc(int threadRole);

} // namespace adrc
} // namespace aatpe

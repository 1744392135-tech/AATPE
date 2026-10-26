// adrc.cpp - Active Disturbance Rejection Control implementation
// TD (Tracking Differentiator) + ESO (Extended State Observer) + NLSEF
// + disturbance compensation + ML fusion decision.
#include "adrc.h"
#include "aatped_logging.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace aatpe {
namespace adrc {

// ============================================================================
// fal() - Nonlinear power function (Han's nonlinear function)
// fal(e, alpha, delta) = |e|^alpha * sign(e)   when |e| > delta
//                        e / delta^(1-alpha)     when |e| <= delta
// The linear interval near zero prevents high-gain oscillation near origin.
// ============================================================================
double AdrcController::fal(double e, double alpha, double delta) const {
    // P3: NaN input would propagate through pow()/abs() and poison ESO states.
    if (std::isnan(e)) return 0.0;
    double ae = std::abs(e);
    if (ae > delta) {
        return std::pow(ae, alpha) * (e > 0.0 ? 1.0 : -1.0);
    } else {
        // linear region: e / delta^(1-alpha) = e * delta^(alpha-1)
        if (delta < 1e-12) return e;
        return e * std::pow(delta, alpha - 1.0);
    }
}

// ============================================================================
// Tracking Differentiator (TD) - Han's fastest discrete TD
// Input: target signal v(k), sample period h
// Output: v1 (smooth tracking), v2 (differential of v1)
// Discrete form:
//   fh = fhan(v1 - v, v2, r, h)
//   v1 = v1 + h * v2
//   v2 = v2 + h * fh
// ============================================================================

// fhan function - fastest control synthesis function
// fhan(x1, x2, r, h) = -r * fsg(x1 + h*x2, r*h)  when ...
// Standard implementation:
static inline double fsign(double x) { return (x > 0.0) ? 1.0 : ((x < 0.0) ? -1.0 : 0.0); }
// P3: fsg() was dead code (never called); removed. fsign() above is still used
// by tdStep().

void AdrcController::tdStep(double target, double h) {
    if (h < 1e-6) h = m_params.h;
    double r = m_params.r;
    if (r < 1e-6) r = 1.0;  // P3: prevent d=0 divide-by-zero at -r*a/d
    double d = r * h;
    double d0 = d * h;
    double x = m_td.v1 - target + h * m_td.v2;
    double ax2 = std::sqrt(d * d + 8.0 * r * std::abs(x));

    double a;
    if (std::abs(x) > d0) {
        a = m_td.v2 + fsign(x) * (ax2 - d) * 0.5;
    } else {
        a = m_td.v2 + x / h;
    }

    double fh;
    if (std::abs(a) <= d) {
        fh = -r * a / d;
    } else {
        fh = -r * fsign(a);
    }

    m_td.v1 += h * m_td.v2;
    m_td.v2 += h * fh;
}

// ============================================================================
// Extended State Observer (ESO) - Second-order nonlinear ESO
// Input: measured signal y, control u, sample period h
// State: z1 (estimated frame time), z2 (derivative), z3 (total disturbance)
// Discrete ESO (Han's nonlinear ESO):
//   e = z1 - y
//   z1 = z1 + h * (z2 - beta01 * e)
//   z2 = z2 + h * (z3 - beta02 * fal(e, alpha1, delta) + b0 * u)
//   z3 = z3 + h * (-beta03 * fal(e, alpha2, delta))
// ============================================================================
void AdrcController::esoStep(double measured, double control, double h) {
    if (h < 1e-6) h = m_params.h;

    double e = m_eso.z1 - measured;

    // Nonlinear error feedback terms
    double fe1 = fal(e, 0.5, m_params.delta);  // fal(e, 0.5, delta) for z1 update
    double fe2 = fal(e, 0.25, m_params.delta); // fal(e, 0.25, delta) for z2/z3 update

    // ESO state update (Euler discretization)
    m_eso.z1 += h * (m_eso.z2 - m_params.beta01 * e);
    m_eso.z2 += h * (m_eso.z3 - m_params.beta02 * fe1 + m_params.b0 * control);
    m_eso.z3 += h * (-m_params.beta03 * fe2);

    // Clamp disturbance to reasonable range to prevent runaway
    if (m_eso.z3 > 100000.0) m_eso.z3 = 100000.0;
    if (m_eso.z3 < -100000.0) m_eso.z3 = -100000.0;
}

// ============================================================================
// Nonlinear State Error Feedback (NLSEF)
// u0 = beta1 * fal(e1, alpha1, delta) + beta2 * fal(e2, alpha2, delta)
// where e1 = v1 - z1 (tracking error), e2 = v2 - z2 (error derivative)
// ============================================================================
double AdrcController::nlsefStep(double e1, double e2) const {
    double u0 = m_params.beta1 * fal(e1, m_params.alpha1, m_params.delta)
              + m_params.beta2 * fal(e2, m_params.alpha2, m_params.delta);
    return u0;
}

// ============================================================================
// Compute dynamic fusion weights: ADRC vs ML
// ADRC confidence high when disturbance is stable (small |z3| relative to signal)
// ML confidence high when historical data is sufficient
// Final weights normalized to sum = 1
// ============================================================================
void AdrcController::computeFusionWeights(double adrcConfidence, double mlConfidence,
                                           double& outAdrcWeight, double& outMlWeight) const {
    // Base weights
    double wAdrc = m_params.adrcWeightBase;
    double wMl = m_params.mlWeightBase;

    // Adjust by confidence (confidence 0-1)
    wAdrc *= adrcConfidence;
    wMl *= mlConfidence;

    // If both are near zero, fall back to equal split
    double total = wAdrc + wMl;
    if (total < 0.01) {
        outAdrcWeight = 0.5;
        outMlWeight = 0.5;
        return;
    }

    outAdrcWeight = wAdrc / total;
    outMlWeight = wMl / total;
}

// ============================================================================
// AdrcController implementation
// ============================================================================
AdrcController::AdrcController()
    : m_params(kDefaultAdrcParams)
    , m_initialized(false) {
    m_td.reset();
    m_eso.reset();
    m_lastOutput = AdrcOutput{};
}

AdrcController::AdrcController(const AdrcParams& params)
    : m_params(clampParams(params, kDefaultAdrcParams))
    , m_initialized(false) {
    m_td.reset();
    m_eso.reset();
    m_lastOutput = AdrcOutput{};
}

// G5-005: single source of truth for numeric safety. Used by the (params) ctor
// (which previously stored raw params unclamped) and by setParams().
AdrcParams AdrcController::clampParams(const AdrcParams& in, const AdrcParams& fb) {
    AdrcParams p = in;
    // Guard against divide-by-zero downstream (tdStep uses x/h, u = u0 - z3/b0).
    if (p.h < 1e-6) p.h = fb.h > 1e-6 ? fb.h : 0.005;
    if (p.b0 < 1e-9) p.b0 = fb.b0 > 1e-9 ? fb.b0 : 1.0;
    // r=0 makes d=r*h=0, then tdStep divides by d at -r*a/d.
    if (p.r < 1e-6) p.r = fb.r > 1e-6 ? fb.r : 1.0;
    // Forward-Euler ESO stability: keep beta_k * h <= 2.
    const double kMaxEulerGain = 2.0;
    double beta01Max = kMaxEulerGain / p.h;
    double beta02Max = kMaxEulerGain / p.h;
    double beta03Max = kMaxEulerGain / p.h;
    if (p.beta01 > beta01Max) p.beta01 = beta01Max;
    if (p.beta02 > beta02Max) p.beta02 = beta02Max;
    if (p.beta03 > beta03Max) p.beta03 = beta03Max;
    return p;
}

void AdrcController::setParams(const AdrcParams& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_params = clampParams(params, m_params);
}

void AdrcController::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_td.reset();
    m_eso.reset();
    m_initialized = false;
    m_lastOutput = AdrcOutput{};
}

AdrcOutput AdrcController::update(const AdrcInput& input) {
    std::lock_guard<std::mutex> lock(m_mutex);

    AdrcOutput out{};

    // Validate input: skip update if frame time is invalid
    if (input.frameTimeUs <= 0.0 || input.targetFrameTimeUs <= 0.0) {
        out = m_lastOutput;
        out.urgency = 0.0;
        out.riskScore = 0.0;
        out.shouldBoost = false;
        out.shouldClamp = false;
        return out;
    }

    // On first valid sample, initialize ESO state to measured value
    if (!m_initialized) {
        m_td.v1 = input.targetFrameTimeUs;
        m_td.v2 = 0.0;
        m_eso.z1 = input.frameTimeUs;
        m_eso.z2 = 0.0;
        m_eso.z3 = 0.0;
        m_initialized = true;
    }

    double h = m_params.h;

    // Step 1: Tracking Differentiator on target frame time
    tdStep(input.targetFrameTimeUs, h);

    // Step 2: Compute errors between TD output and ESO state
    double e1 = m_td.v1 - m_eso.z1;   // tracking error (frame time deviation)
    double e2 = m_td.v2 - m_eso.z2;   // error derivative (rate of change)

    // Step 3: NLSEF - nonlinear state error feedback
    double u0 = nlsefStep(e1, e2);

    // Step 4: Disturbance compensation
    // u = u0 - z3 / b0  (cancel estimated total disturbance)
    double u = u0 - m_eso.z3 / m_params.b0;

    // Step 5: ESO update (observe the system using measured output and control)
    esoStep(input.frameTimeUs, u, h);

    // ========================================================================
    // Compute output signals
    // ========================================================================

    // urgency: normalized deviation of current frame time from target
    // Use TD-smoothed tracking error for stability
    double deviation = std::abs(m_eso.z1 - input.targetFrameTimeUs);
    double maxDeviation = input.targetFrameTimeUs * 0.5;  // 50% over target = max urgency
    out.urgency = std::min(1.0, deviation / maxDeviation);
    // Clamp: if frame time is BELOW target (faster), urgency is lower
    if (m_eso.z1 < input.targetFrameTimeUs) {
        out.urgency *= 0.3;  // being fast is less urgent than being slow
    }

    // riskScore: based on ESO total disturbance and its rate of change
    // High |z3| means large unmodeled disturbance -> higher jank risk
    double disturbanceNorm = std::abs(m_eso.z3) / (input.targetFrameTimeUs * 2.0);
    double derivativeNorm = std::abs(m_eso.z2) / (input.targetFrameTimeUs / m_params.h);
    // Combine disturbance magnitude and rate for risk
    out.riskScore = std::min(1.0, 0.6 * disturbanceNorm + 0.4 * derivativeNorm);

    // shouldBoost: urgency high AND risk high
    out.shouldBoost = (out.urgency > m_params.urgencyThreshold)
                   && (out.riskScore > m_params.riskScoreThreshold)
                   && (input.thermalLevel < 0.8);  // thermal safety gate

    // shouldClamp: frame time well below target AND thermal safe AND risk low
    double frameRatio = m_eso.z1 / input.targetFrameTimeUs;
    out.shouldClamp = (frameRatio < m_params.clampFrameRatio)
                   && (input.thermalLevel < 0.5)
                   && (out.riskScore < 0.3)
                   && (out.urgency < 0.2);

    // predictedFrameTime: one-step prediction using ESO state
    // x(k+1) ≈ z1 + h * z2 + h^2/2 * z3 (second-order Taylor prediction)
    out.predictedFrameTime = m_eso.z1 + h * m_eso.z2 + 0.5 * h * h * m_eso.z3;
    if (out.predictedFrameTime < 0.0) out.predictedFrameTime = 0.0;

    // ========================================================================
    // ML Fusion: ADRC feedforward + ML feedback
    // fused = w_adrc * adrc_signal + w_ml * ml_signal
    // ========================================================================
    // ADRC signal: based on urgency and risk (0-1, higher = more boost needed)
    double adrcSignal = 0.5 * out.urgency + 0.5 * out.riskScore;

    // ADRC confidence: higher when disturbance is stable (z3 changes slowly)
    // Estimate by normalized disturbance magnitude (smaller = more confident)
    double adrcConf = 1.0 - std::min(1.0, std::abs(m_eso.z3) / (input.targetFrameTimeUs * 3.0));
    // Also: TD tracking quality (e1 small = good tracking)
    double trackQuality = 1.0 - std::min(1.0, std::abs(e1) / (input.targetFrameTimeUs * 0.3));
    adrcConf = 0.5 * adrcConf + 0.5 * trackQuality;

    // ML confidence from input
    double mlConf = std::max(0.0, std::min(1.0, input.mlConfidence));

    // Compute dynamic weights
    double wAdrc, wMl;
    computeFusionWeights(adrcConf, mlConf, wAdrc, wMl);

    out.adrcWeight = wAdrc;
    out.mlWeight = wMl;

    // Fused signal
    out.fusedSignal = wAdrc * adrcSignal + wMl * input.mlSignal;
    out.fusedSignal = std::max(0.0, std::min(1.0, out.fusedSignal));

    // Disturbance and error diagnostics
    out.disturbance = m_eso.z3;
    out.trackingError = e1;
    out.errorDerivative = e2;

    m_lastOutput = out;
    return out;
}

AdrcOutput AdrcController::getLastOutput() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastOutput;
}

// ============================================================================
// Global ADRC subsystem
// ============================================================================
static AdrcController g_adrcControllers[(int)AdrcRole::COUNT];
static AdrcParams g_adrcParams = kDefaultAdrcParams;
// P2: g_adrcParams is written by configure() (config reload thread) and read by
// getConfig() (any thread) without a lock. Guard both with a dedicated mutex.
static std::mutex g_adrcParamsMutex;
static std::atomic<bool> g_adrcInitialized{false};

void init() {
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        g_adrcControllers[i].reset();
        g_adrcControllers[i].setParams(g_adrcParams);
    }
    g_adrcInitialized.store(true);
    LOG_INFO("ADRC: subsystem initialized with %d roles, h=%.3f r=%.1f",
              (int)AdrcRole::COUNT, g_adrcParams.h, g_adrcParams.r);
}

void reset() {
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        g_adrcControllers[i].reset();
    }
    LOG_INFO("ADRC: all controllers reset");
}

void configure(const AdrcParams& params) {
    {
        std::lock_guard<std::mutex> lock(g_adrcParamsMutex);
        g_adrcParams = params;
    }
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        g_adrcControllers[i].setParams(params);
    }
    LOG_INFO("ADRC: configured h=%.3f r=%.1f beta01=%.1f beta02=%.1f beta03=%.1f "
             "beta1=%.1f beta2=%.2f alpha1=%.2f alpha2=%.2f delta=%.3f b0=%.2f",
             params.h, params.r, params.beta01, params.beta02, params.beta03,
             params.beta1, params.beta2, params.alpha1, params.alpha2,
             params.delta, params.b0);
}

AdrcParams getConfig() {
    std::lock_guard<std::mutex> lock(g_adrcParamsMutex);
    return g_adrcParams;
}

AdrcOutput updateRole(AdrcRole role, const AdrcInput& input) {
    int idx = (int)role;
    if (idx < 0 || idx >= (int)AdrcRole::COUNT) {
        return AdrcOutput{};
    }
    return g_adrcControllers[idx].update(input);
}

AdrcOutput getRoleOutput(AdrcRole role) {
    int idx = (int)role;
    if (idx < 0 || idx >= (int)AdrcRole::COUNT) {
        return AdrcOutput{};
    }
    return g_adrcControllers[idx].getLastOutput();
}

double getAggregateRiskScore() {
    double maxRisk = 0.0;
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        AdrcOutput out = g_adrcControllers[i].getLastOutput();
        if (out.riskScore > maxRisk) maxRisk = out.riskScore;
    }
    return maxRisk;
}

bool anyShouldBoost() {
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        AdrcOutput out = g_adrcControllers[i].getLastOutput();
        if (out.shouldBoost) return true;
    }
    return false;
}

bool allCanClamp() {
    int count = 0;
    int clampCount = 0;
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        AdrcOutput out = g_adrcControllers[i].getLastOutput();
        // Only consider roles that have been active (non-zero urgency/risk means initialized)
        if (out.urgency > 0.001 || out.riskScore > 0.001) {
            count++;
            if (out.shouldClamp) clampCount++;
        }
    }
    return (count > 0 && clampCount == count);
}

double getPredictedFrameTimeUs() {
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < (int)AdrcRole::COUNT; i++) {
        AdrcOutput out = g_adrcControllers[i].getLastOutput();
        if (out.predictedFrameTime > 0.0) {
            sum += out.predictedFrameTime;
            count++;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

AdrcRole mapThreadRoleToAdrc(int threadRole) {
    // These values must match enum ThreadRole in aatped_types.h:
    //   ROLE_S0_SYSTEM=0, ROLE_S1_UI=1, ROLE_S2_RENDER=2, ROLE_S3_GAME=3.
    // We use raw ints rather than including aatped_types.h here because that
    // header pulls in aatpe_ml::ThreadFeatures (ML subsystem) which adrc.cpp
    // must not depend on.
    static constexpr int kRoleUi     = 1;
    static constexpr int kRoleRender = 2;
    static constexpr int kRoleGame   = 3;
    switch (threadRole) {
        case kRoleUi:     return AdrcRole::UI;
        case kRoleRender: return AdrcRole::RENDER;
        case kRoleGame:   return AdrcRole::GAME;
        default:          return AdrcRole::UI;  // fallback
    }
}

} // namespace adrc
} // namespace aatpe

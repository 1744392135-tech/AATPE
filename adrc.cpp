#include "adrc.h"
#include "aatped_logging.h"

#include <cmath>
#include <algorithm>
#include <cstring>

namespace aatpe {
namespace adrc {

double AdrcController::fal(double e, double alpha, double delta) const {
    if (std::isnan(e)) return 0.0;
    double ae = std::abs(e);
    if (ae > delta) {
        return std::pow(ae, alpha) * (e > 0.0 ? 1.0 : -1.0);
    } else {
        if (delta < 1e-12) return e;
        return e * std::pow(delta, alpha - 1.0);
    }
}

static inline double fsign(double x) { return (x > 0.0) ? 1.0 : ((x < 0.0) ? -1.0 : 0.0); }

void AdrcController::tdStep(double target, double h) {
    if (h < 1e-6) h = m_params.h;
    double r = m_params.r;
    if (r < 1e-6) r = 1.0;
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

void AdrcController::esoStep(double measured, double control, double h) {
    if (h < 1e-6) h = m_params.h;

    double e = m_eso.z1 - measured;

    double fe1 = fal(e, 0.5, m_params.delta);
    double fe2 = fal(e, 0.25, m_params.delta);

    m_eso.z1 += h * (m_eso.z2 - m_params.beta01 * e);
    m_eso.z2 += h * (m_eso.z3 - m_params.beta02 * fe1 + m_params.b0 * control);
    m_eso.z3 += h * (-m_params.beta03 * fe2);

    if (m_eso.z3 > 100000.0) m_eso.z3 = 100000.0;
    if (m_eso.z3 < -100000.0) m_eso.z3 = -100000.0;
}

double AdrcController::nlsefStep(double e1, double e2) const {
    double u0 = m_params.beta1 * fal(e1, m_params.alpha1, m_params.delta)
              + m_params.beta2 * fal(e2, m_params.alpha2, m_params.delta);
    return u0;
}

void AdrcController::computeFusionWeights(double adrcConfidence, double mlConfidence,
                                           double& outAdrcWeight, double& outMlWeight) const {
    double wAdrc = m_params.adrcWeightBase;
    double wMl = m_params.mlWeightBase;

    wAdrc *= adrcConfidence;
    wMl *= mlConfidence;

    double total = wAdrc + wMl;
    if (total < 0.01) {
        outAdrcWeight = 0.5;
        outMlWeight = 0.5;
        return;
    }

    outAdrcWeight = wAdrc / total;
    outMlWeight = wMl / total;
}

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

AdrcParams AdrcController::clampParams(const AdrcParams& in, const AdrcParams& fb) {
    AdrcParams p = in;
    if (p.h < 1e-6) p.h = fb.h > 1e-6 ? fb.h : 0.005;
    if (p.b0 < 1e-9) p.b0 = fb.b0 > 1e-9 ? fb.b0 : 1.0;
    if (p.r < 1e-6) p.r = fb.r > 1e-6 ? fb.r : 1.0;
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

    if (input.frameTimeUs <= 0.0 || input.targetFrameTimeUs <= 0.0) {
        out = m_lastOutput;
        out.urgency = 0.0;
        out.riskScore = 0.0;
        out.shouldBoost = false;
        out.shouldClamp = false;
        return out;
    }

    if (!m_initialized) {
        m_td.v1 = input.targetFrameTimeUs;
        m_td.v2 = 0.0;
        m_eso.z1 = input.frameTimeUs;
        m_eso.z2 = 0.0;
        m_eso.z3 = 0.0;
        m_initialized = true;
    }

    double h = m_params.h;

    tdStep(input.targetFrameTimeUs, h);

    double e1 = m_td.v1 - m_eso.z1;
    double e2 = m_td.v2 - m_eso.z2;

    double u0 = nlsefStep(e1, e2);

    double u = u0 - m_eso.z3 / m_params.b0;

    esoStep(input.frameTimeUs, u, h);

    double deviation = std::abs(m_eso.z1 - input.targetFrameTimeUs);
    double maxDeviation = input.targetFrameTimeUs * 0.5;
    out.urgency = std::min(1.0, deviation / maxDeviation);
    if (m_eso.z1 < input.targetFrameTimeUs) {
        out.urgency *= 0.3;
    }

    double disturbanceNorm = std::abs(m_eso.z3) / (input.targetFrameTimeUs * 2.0);
    double derivativeNorm = std::abs(m_eso.z2) / (input.targetFrameTimeUs / m_params.h);
    out.riskScore = std::min(1.0, 0.6 * disturbanceNorm + 0.4 * derivativeNorm);

    out.shouldBoost = (out.urgency > m_params.urgencyThreshold)
                   && (out.riskScore > m_params.riskScoreThreshold)
                   && (input.thermalLevel < 0.8);

    double frameRatio = m_eso.z1 / input.targetFrameTimeUs;
    out.shouldClamp = (frameRatio < m_params.clampFrameRatio)
                   && (input.thermalLevel < 0.5)
                   && (out.riskScore < 0.3)
                   && (out.urgency < 0.2);

    out.predictedFrameTime = m_eso.z1 + h * m_eso.z2 + 0.5 * h * h * m_eso.z3;
    if (out.predictedFrameTime < 0.0) out.predictedFrameTime = 0.0;

    double adrcSignal = 0.5 * out.urgency + 0.5 * out.riskScore;

    double adrcConf = 1.0 - std::min(1.0, std::abs(m_eso.z3) / (input.targetFrameTimeUs * 3.0));
    double trackQuality = 1.0 - std::min(1.0, std::abs(e1) / (input.targetFrameTimeUs * 0.3));
    adrcConf = 0.5 * adrcConf + 0.5 * trackQuality;

    double mlConf = std::max(0.0, std::min(1.0, input.mlConfidence));

    double wAdrc, wMl;
    computeFusionWeights(adrcConf, mlConf, wAdrc, wMl);

    out.adrcWeight = wAdrc;
    out.mlWeight = wMl;

    out.fusedSignal = wAdrc * adrcSignal + wMl * input.mlSignal;
    out.fusedSignal = std::max(0.0, std::min(1.0, out.fusedSignal));

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

static AdrcController g_adrcControllers[(int)AdrcRole::COUNT];
static AdrcParams g_adrcParams = kDefaultAdrcParams;
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
    static constexpr int kRoleUi     = 1;
    static constexpr int kRoleRender = 2;
    static constexpr int kRoleGame   = 3;
    switch (threadRole) {
        case kRoleUi:     return AdrcRole::UI;
        case kRoleRender: return AdrcRole::RENDER;
        case kRoleGame:   return AdrcRole::GAME;
        default:          return AdrcRole::UI;
    }
}

}
}

#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace aatpe {
namespace adrc {

struct AdrcParams {
    double h;
    double r;
    double beta01;
    double beta02;
    double beta03;
    double beta1;
    double beta2;
    double alpha1;
    double alpha2;
    double delta;
    double b0;
    double adrcWeightBase;
    double mlWeightBase;
    double urgencyThreshold;
    double riskScoreThreshold;
    double clampFrameRatio;
};

inline constexpr AdrcParams kDefaultAdrcParams{
           0.05,
           50.0,
      100.0,
      300.0,
      1000.0,
       10.0,
       0.5,
      0.75,
      1.25,
       0.01,
          1.0,
  0.4,
     0.6,
  0.5,
     0.6,
  0.7
};

enum class AdrcRole {
    UI = 0,
    RENDER = 1,
    GAME = 2,
    COUNT = 3
};

struct AdrcInput {
    double frameTimeUs;
    double targetFrameTimeUs;
    double cpuLoad;
    double thermalLevel;
    double mlSignal;
    double mlConfidence;
};

struct AdrcOutput {
    double urgency;
    double riskScore;
    bool   shouldBoost;
    bool   shouldClamp;
    double predictedFrameTime;
    double fusedSignal;
    double adrcWeight;
    double mlWeight;
    double disturbance;
    double trackingError;
    double errorDerivative;
};

struct TdState {
    double v1;
    double v2;
    void reset() { v1 = 0.0; v2 = 0.0; }
};

struct EsoState {
    double z1;
    double z2;
    double z3;
    void reset() { z1 = 0.0; z2 = 0.0; z3 = 0.0; }
};

class AdrcController {
public:
    AdrcController();
    explicit AdrcController(const AdrcParams& params);

    void setParams(const AdrcParams& params);
    AdrcParams getParams() const { std::lock_guard<std::mutex> lock(m_mutex); return m_params; }

    void reset();
    AdrcOutput update(const AdrcInput& input);

    AdrcOutput getLastOutput() const;

private:
    void tdStep(double target, double h);

    double fal(double e, double alpha, double delta) const;

    void esoStep(double measured, double control, double h);

    double nlsefStep(double e1, double e2) const;

    void computeFusionWeights(double adrcConfidence, double mlConfidence,
                               double& outAdrcWeight, double& outMlWeight) const;

    static AdrcParams clampParams(const AdrcParams& in, const AdrcParams& fallback);

    AdrcParams m_params;
    TdState m_td;
    EsoState m_eso;
    bool m_initialized;

    mutable std::mutex m_mutex;
    AdrcOutput m_lastOutput;
};

void init();
void reset();
void configure(const AdrcParams& params);
AdrcParams getConfig();

AdrcOutput updateRole(AdrcRole role, const AdrcInput& input);

AdrcOutput getRoleOutput(AdrcRole role);

double getAggregateRiskScore();

bool anyShouldBoost();

bool allCanClamp();

double getPredictedFrameTimeUs();

AdrcRole mapThreadRoleToAdrc(int threadRole);

}
}

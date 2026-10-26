// aatpe_interfaces.h - 接口抽象层 (fix103/3104 P3接口接入)
#ifndef AATPE_INTERFACES_H
#define AATPE_INTERFACES_H

#include <string>
#include <vector>
#include <cstdint>

struct ThreadInfo;
struct Decision;
struct Config;

// ==================== 接口定义 ====================

class IScorer {
public:
    virtual ~IScorer() = default;
    virtual int computeScore(int cpuId, const ThreadInfo& ti) = 0;
    virtual std::string name() const = 0;
};

class IPlacementDecider {
public:
    virtual ~IPlacementDecider() = default;
    virtual Decision decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency = false) = 0;
    virtual std::string name() const = 0;
};

class IEventSource {
public:
    virtual ~IEventSource() = default;
    virtual bool init() = 0;
    virtual bool poll(int timeoutMs) = 0;
    virtual void cleanup() = 0;
    virtual std::string name() const = 0;
};

class IMlModel {
public:
    virtual ~IMlModel() = default;
    // P1-4: Per-pool ML interface. poolIdx selects the compute pool
    // (0=LITTLE, 1=MID, 2=BIG) so callers can train/predict per-pool instead
    // of being locked to pool 0.
    virtual float predict(int poolIdx, const std::vector<float>& features) = 0;
    virtual void update(int poolIdx, const std::vector<float>& features, float target) = 0;
    virtual std::vector<float> getWeights(int poolIdx) const = 0;
    virtual void setWeights(int poolIdx, const std::vector<float>& w) = 0;
    virtual int getPoolCount() const = 0;
    virtual std::string name() const = 0;
};

class ICpuMonitor {
public:
    virtual ~ICpuMonitor() = default;
    virtual int getCpuCount() = 0;
    virtual float getCpuUtilization(int cpuId) = 0;
    virtual int getCpuFrequency(int cpuId) = 0;
    virtual int getTemperature() = 0;
    virtual std::string name() const = 0;
};

// ==================== 生产实现类 (fix104 P3) ====================

class DefaultScorer : public IScorer {
public:
    int computeScore(int cpuId, const ThreadInfo& ti) override;
    std::string name() const override { return "default_scorer"; }
};

class DefaultPlacementDecider : public IPlacementDecider {
public:
    Decision decidePlacement(const ThreadInfo& ti, const Config& cfg, bool emergency = false) override;
    std::string name() const override { return "default_decider"; }
};

class DefaultCpuMonitor : public ICpuMonitor {
public:
    int getCpuCount() override;
    float getCpuUtilization(int cpuId) override;
    int getCpuFrequency(int cpuId) override;
    int getTemperature() override;
    std::string name() const override { return "default_cpu_monitor"; }
};

class PollingEventSource : public IEventSource {
public:
    bool init() override { return true; }
    bool poll(int timeoutMs) override;
    void cleanup() override {}
    std::string name() const override { return "polling_event"; }
};

class OnlineLearningModel : public IMlModel {
public:
    float predict(int poolIdx, const std::vector<float>& features) override;
    void update(int poolIdx, const std::vector<float>& features, float target) override;
    std::vector<float> getWeights(int poolIdx) const override;
    void setWeights(int poolIdx, const std::vector<float>& w) override;
    int getPoolCount() const override;
    std::string name() const override { return "online_learning"; }
};

// ==================== Mock实现 (测试用) ====================
class MockScorer : public IScorer {
public:
    int computeScore(int, const ThreadInfo&) override { return 50; }
    std::string name() const override { return "mock_scorer"; }
};

class MockPlacementDecider : public IPlacementDecider {
public:
    Decision decidePlacement(const ThreadInfo&, const Config&, bool emergency = false) override;
    std::string name() const override { return "mock_decider"; }
};

class MockEventSource : public IEventSource {
public:
    bool init() override { return true; }
    bool poll(int) override { return false; }
    void cleanup() override {}
    std::string name() const override { return "mock_event"; }
};

class MockMlModel : public IMlModel {
public:
    float predict(int, const std::vector<float>&) override { return 0.0f; }
    void update(int, const std::vector<float>&, float) override {}
    std::vector<float> getWeights(int) const override { return {}; }
    void setWeights(int, const std::vector<float>&) override {}
    int getPoolCount() const override { return 3; }
    std::string name() const override { return "mock_ml"; }
};

class MockCpuMonitor : public ICpuMonitor {
public:
    int getCpuCount() override { return 8; }
    float getCpuUtilization(int) override { return 50.0f; }
    int getCpuFrequency(int) override { return 2000000; }
    int getTemperature() override { return 40; }
    std::string name() const override { return "mock_cpu"; }
};

extern IScorer* g_scorer;
extern IPlacementDecider* g_placementDecider;
extern ICpuMonitor* g_cpuMonitor;
extern IEventSource* g_eventSource;
extern IMlModel* g_mlModelInterface;

void initInterfaces();
void cleanupInterfaces();

#endif // AATPE_INTERFACES_H

#ifndef AATPE_MANAGERS_H
#define AATPE_MANAGERS_H

#include <string>
#include <atomic>
#include <mutex>
#include <ctime>

struct CpuMetrics;

namespace aatpe {

class ThermalManager {
public:
    static ThermalManager& instance();

    bool isEmergency() const;
    void setEmergency(bool e);

    int migrationPressure() const;
    void setMigrationPressure(int p);

    int cpuTemp() const;
    int skinTemp() const;
    int batteryTemp() const;
    void setCpuTemp(int t);
    void setSkinTemp(int t);
    void setBatteryTemp(int t);

private:
    ThermalManager() = default;
    ThermalManager(const ThermalManager&) = delete;
    ThermalManager& operator=(const ThermalManager&) = delete;
};

class ForegroundManager {
public:
    static ForegroundManager& instance();

    std::string getPackage() const;
    void setPackage(const std::string& pkg);

    time_t startTime() const;

private:
    ForegroundManager() = default;
    ForegroundManager(const ForegroundManager&) = delete;
    ForegroundManager& operator=(const ForegroundManager&) = delete;
};

class CpuMetricsManager {
public:
    static CpuMetricsManager& instance();

    int capacity(int cpuId) const;

    int util(int cpuId) const;

private:
    CpuMetricsManager() = default;
    CpuMetricsManager(const CpuMetricsManager&) = delete;
    CpuMetricsManager& operator=(const CpuMetricsManager&) = delete;
};

}

#endif

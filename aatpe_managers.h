// AATPE - Global variable manager encapsulation (#96, partial)
// Thin manager classes that wrap existing global variables. This is the first
// step toward full encapsulation; the globals remain as a compatibility layer.
#ifndef AATPE_MANAGERS_H
#define AATPE_MANAGERS_H

#include <string>
#include <atomic>
#include <mutex>
#include <ctime>

// Forward declarations
struct CpuMetrics;

namespace aatpe {

// ============================================================================
// ThermalManager: wraps g_thermalEmergency, g_thermalMigrationPressure,
// g_thermalCpu, g_thermalSkin, g_thermalBattery. Provides a single access
// point for thermal state reads/writes, preserving the existing global
// variables as a compatibility layer.
// ============================================================================
class ThermalManager {
public:
    static ThermalManager& instance();

    // Thermal emergency flag
    bool isEmergency() const;
    void setEmergency(bool e);

    // Migration pressure (0-100, percent extra cost threshold)
    int migrationPressure() const;
    void setMigrationPressure(int p);

    // Temperature readings (°C)
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

// ============================================================================
// ForegroundManager: wraps g_foregroundApp, g_startTime. Provides thread-safe
// access to the foreground package name.
// ============================================================================
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

// ============================================================================
// CpuMetricsManager: wraps g_cpuMetrics + g_cpuMetricsMutex. Provides a
// snapshot interface for reading CPU metrics without holding the lock long.
// ============================================================================
class CpuMetricsManager {
public:
    static CpuMetricsManager& instance();

    // Get capacity for a specific CPU (thread-safe)
    int capacity(int cpuId) const;

    // Get utilization for a specific CPU (thread-safe)
    int util(int cpuId) const;

private:
    CpuMetricsManager() = default;
    CpuMetricsManager(const CpuMetricsManager&) = delete;
    CpuMetricsManager& operator=(const CpuMetricsManager&) = delete;
};

} // namespace aatpe

#endif // AATPE_MANAGERS_H

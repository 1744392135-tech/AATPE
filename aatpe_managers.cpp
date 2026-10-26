// AATPE - Global variable manager encapsulation (#96, partial)
#include "aatped.h"
#include "aatpe_managers.h"
#include <mutex>

namespace aatpe {

// ---- ThermalManager ----
ThermalManager& ThermalManager::instance() {
    static ThermalManager inst;
    return inst;
}

bool ThermalManager::isEmergency() const {
    return g_thermalEmergency.load(std::memory_order_relaxed);
}

void ThermalManager::setEmergency(bool e) {
    g_thermalEmergency.store(e, std::memory_order_relaxed);
}

int ThermalManager::migrationPressure() const {
    return g_thermalMigrationPressure.load(std::memory_order_relaxed);
}

void ThermalManager::setMigrationPressure(int p) {
    g_thermalMigrationPressure.store(p, std::memory_order_relaxed);
}

int ThermalManager::cpuTemp() const { return g_thermalCpu.load(std::memory_order_relaxed); }
int ThermalManager::skinTemp() const { return g_thermalSkin.load(std::memory_order_relaxed); }
int ThermalManager::batteryTemp() const { return g_thermalBattery.load(std::memory_order_relaxed); }

void ThermalManager::setCpuTemp(int t) { g_thermalCpu.store(t, std::memory_order_relaxed); }
void ThermalManager::setSkinTemp(int t) { g_thermalSkin.store(t, std::memory_order_relaxed); }
void ThermalManager::setBatteryTemp(int t) { g_thermalBattery.store(t, std::memory_order_relaxed); }

// ---- ForegroundManager ----
ForegroundManager& ForegroundManager::instance() {
    static ForegroundManager inst;
    return inst;
}

std::string ForegroundManager::getPackage() const {
    // P0-3: g_foregroundApp is a std::string written by the foreground detection
    // thread and read here. Returning it without g_foregroundMutex races a writer
    // reallocating the string's internal buffer -> SIGSEGV. Snapshot under lock.
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    return g_foregroundApp;
}

void ForegroundManager::setPackage(const std::string& pkg) {
    // P0-3: writers must hold the same lock as readers.
    std::lock_guard<std::mutex> lk(g_foregroundMutex);
    g_foregroundApp = pkg;
}

time_t ForegroundManager::startTime() const {
    return g_startTime.load();
}

// ---- CpuMetricsManager ----
CpuMetricsManager& CpuMetricsManager::instance() {
    static CpuMetricsManager inst;
    return inst;
}

int CpuMetricsManager::capacity(int cpuId) const {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    auto it = g_cpuMetrics.find(cpuId);
    return (it != g_cpuMetrics.end()) ? it->second.capacity : 0;
}

int CpuMetricsManager::util(int cpuId) const {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    auto it = g_cpuMetrics.find(cpuId);
    return (it != g_cpuMetrics.end()) ? it->second.util : 0;
}

} // namespace aatpe

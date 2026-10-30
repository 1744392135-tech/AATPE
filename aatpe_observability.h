#ifndef AATPE_OBSERVABILITY_H
#define AATPE_OBSERVABILITY_H

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <atomic>

namespace aatpe {
namespace observability {

enum class HealthStatus {
    HEALTHY = 0,
    DEGRADED = 1,
    CRITICAL = 2,
    UNKNOWN = 3
};

enum class AlertSeverity {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    CRITICAL = 3
};

struct AlertRecord {
    AlertSeverity severity;
    std::string source;
    std::string message;
    long long timestampMs;
    int count;
};

struct MetricDef {
    std::string name;
    std::string unit;
    std::string description;
    double value;
    double min;
    double max;
    double warningThreshold;
    double criticalThreshold;
    bool hasWarning;
    bool hasCritical;
};

extern const size_t g_alertHistorySize;

HealthStatus getHealthStatus();
void setHealthStatus(HealthStatus status);
const char* healthStatusToString(HealthStatus status);

void raiseAlert(AlertSeverity severity, const std::string& source, const std::string& message);
std::vector<AlertRecord> getRecentAlerts(size_t max = 50);
void clearAlerts();
int getAlertCount(AlertSeverity severity);

void initMetrics(const std::vector<MetricDef>& metrics);
void updateMetric(const std::string& name, double value);
double getMetricValue(const std::string& name);
std::vector<MetricDef> getAllMetrics();
bool checkMetricThresholds(const std::string& name, AlertSeverity& outSeverity);

bool performHealthCheck();
std::string getHealthSummary();

std::string exportMetricsJson();
std::string exportAlertsJson();

extern std::mutex g_healthMutex;
extern std::mutex g_alertsMutex;
extern std::mutex g_metricsMutex;

}
}

#endif

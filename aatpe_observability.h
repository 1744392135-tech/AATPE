// AATPE Observability - health metrics, alerts, and monitoring
// header contains only declarations; implementations moved to .cpp
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

// Health status enum
enum class HealthStatus {
    HEALTHY = 0,
    DEGRADED = 1,
    CRITICAL = 2,
    UNKNOWN = 3
};

// Alert severity
enum class AlertSeverity {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    CRITICAL = 3
};

// Alert record
struct AlertRecord {
    AlertSeverity severity;
    std::string source;
    std::string message;
    long long timestampMs;
    int count;  // duplicate suppression count
};

// Metric definition
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

// extern declaration, defined in .cpp
extern const size_t g_alertHistorySize;

// Health management
HealthStatus getHealthStatus();
void setHealthStatus(HealthStatus status);
const char* healthStatusToString(HealthStatus status);

// Alert management
void raiseAlert(AlertSeverity severity, const std::string& source, const std::string& message);
std::vector<AlertRecord> getRecentAlerts(size_t max = 50);
void clearAlerts();
int getAlertCount(AlertSeverity severity);

// Metric management
void initMetrics(const std::vector<MetricDef>& metrics);
void updateMetric(const std::string& name, double value);
double getMetricValue(const std::string& name);
std::vector<MetricDef> getAllMetrics();
bool checkMetricThresholds(const std::string& name, AlertSeverity& outSeverity);

// System health check
bool performHealthCheck();
std::string getHealthSummary();

// JSON export for web UI
std::string exportMetricsJson();
std::string exportAlertsJson();

// mutexes protecting global state (declared extern, defined in .cpp)
extern std::mutex g_healthMutex;
extern std::mutex g_alertsMutex;
extern std::mutex g_metricsMutex;

} // namespace observability
} // namespace aatpe

#endif // AATPE_OBSERVABILITY_H

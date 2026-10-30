#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "aatpe_observability.h"

using namespace aatpe::observability;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

int main() {
    printf("=== Observability: health status ===\n");
    CHECK(getHealthStatus() == HealthStatus::UNKNOWN, "initial health is UNKNOWN");
    setHealthStatus(HealthStatus::HEALTHY);
    CHECK(getHealthStatus() == HealthStatus::HEALTHY, "setHealthStatus HEALTHY roundtrip");
    setHealthStatus(HealthStatus::CRITICAL);
    CHECK(getHealthStatus() == HealthStatus::CRITICAL, "setHealthStatus CRITICAL roundtrip");
    CHECK(strcmp(healthStatusToString(HealthStatus::HEALTHY), "HEALTHY") == 0, "to string HEALTHY");
    CHECK(strcmp(healthStatusToString(HealthStatus::DEGRADED), "DEGRADED") == 0, "to string DEGRADED");
    CHECK(strcmp(healthStatusToString(HealthStatus::CRITICAL), "CRITICAL") == 0, "to string CRITICAL");
    CHECK(strcmp(healthStatusToString((HealthStatus)99), "UNKNOWN") == 0, "to string unknown default");

    printf("=== Observability: alerts with dedup ===\n");
    clearAlerts();
    CHECK(getAlertCount(AlertSeverity::WARNING) == 0, "alerts empty after clear");
    raiseAlert(AlertSeverity::WARNING, "srcA", "disk low");
    raiseAlert(AlertSeverity::WARNING, "srcA", "disk low");
    raiseAlert(AlertSeverity::WARNING, "srcA", "disk low");
    raiseAlert(AlertSeverity::ERROR, "srcB", "cpu hot");
    CHECK(getAlertCount(AlertSeverity::WARNING) == 1, "dedup keeps one WARNING record");
    CHECK(getAlertCount(AlertSeverity::ERROR) == 1, "distinct ERROR recorded");
    std::vector<AlertRecord> recent = getRecentAlerts(50);
    CHECK(recent.size() == 2, "two distinct alerts total");
    int dedupCount = 0;
    for (auto& a : recent) if (a.source == "srcA") dedupCount = a.count;
    CHECK(dedupCount == 3, "duplicate-suppression count == 3");
    CHECK(getRecentAlerts(1).size() == 1, "getRecentAlerts(max=1) returns 1");
    clearAlerts();
    CHECK(getRecentAlerts(50).empty(), "alerts cleared");

    printf("=== Observability: metrics O(1) index ===\n");
    std::vector<MetricDef> defs = {
        {"cpu_usage", "%", "cpu", 0.0, 0.0, 100.0, 80.0, 95.0, true, true},
        {"temp_c", "C", "temp", 20.0, 0.0, 120.0, 70.0, 90.0, true, true},
        {"latency_ms", "ms", "lat", 5.0, 0.0, 100.0, 0.0, 0.0, false, false}
    };
    initMetrics(defs);
    CHECK(getMetricValue("cpu_usage") == 0.0, "initial metric value 0");
    updateMetric("cpu_usage", 42.5);
    CHECK(getMetricValue("cpu_usage") == 42.5, "update/getMetricValue roundtrip");
    CHECK(getMetricValue("nonexistent") == 0.0, "unknown metric returns 0");
    updateMetric("cpu_usage", 10.0);
    CHECK(getMetricValue("cpu_usage") == 10.0, "re-update via index works");
    CHECK(getAllMetrics().size() == 3, "getAllMetrics count");

    printf("=== Observability: threshold checks ===\n");
    AlertSeverity sev = AlertSeverity::INFO;
    CHECK(checkMetricThresholds("latency_ms", sev) == false, "no thresholds -> false");
    updateMetric("temp_c", 50.0);
    CHECK(checkMetricThresholds("temp_c", sev) == false, "below warning -> no alert");
    updateMetric("temp_c", 75.0);
    CHECK(checkMetricThresholds("temp_c", sev) == true && sev == AlertSeverity::WARNING, ">=warning -> WARNING");
    updateMetric("temp_c", 95.0);
    CHECK(checkMetricThresholds("temp_c", sev) == true && sev == AlertSeverity::CRITICAL, ">=critical -> CRITICAL");
    CHECK(checkMetricThresholds("ghost", sev) == false, "unknown metric threshold -> false");

    printf("=== Observability: health check state machine ===\n");
    clearAlerts();
    updateMetric("temp_c", 25.0);
    CHECK(performHealthCheck() == true, "healthy when all metrics normal");
    CHECK(getHealthStatus() == HealthStatus::HEALTHY, "healthcheck sets HEALTHY");
    updateMetric("temp_c", 99.0);
    CHECK(performHealthCheck() == false, "critical metric -> unhealthy");
    CHECK(getHealthStatus() == HealthStatus::CRITICAL, "healthcheck sets CRITICAL");
    CHECK(getAlertCount(AlertSeverity::CRITICAL) >= 1, "critical alert raised");
    std::string summary = getHealthSummary();
    CHECK(summary.find("HEALTHY") != std::string::npos || summary.find("CRITICAL") != std::string::npos,
          "health summary contains status");
    CHECK(summary.find("temp_c") != std::string::npos, "health summary lists metric");

    printf("=== Observability: JSON export + escaping ===\n");
    updateMetric("cpu_usage", 33.3);
    std::string mj = exportMetricsJson();
    CHECK(mj.find("\"metrics\"") != std::string::npos, "metrics json has metrics key");
    CHECK(mj.find("\"cpu_usage\"") != std::string::npos, "metrics json names cpu_usage");
    CHECK(mj.find("\"value\":33.3") != std::string::npos, "metrics json carries value");
    clearAlerts();
    raiseAlert(AlertSeverity::ERROR, "srv", "bad \"value\"\\here");
    std::string aj = exportAlertsJson();
    CHECK(aj.find("\"alerts\"") != std::string::npos, "alerts json has alerts key");
    CHECK(aj.find("\\\"") != std::string::npos, "double-quote escaped in JSON");
    CHECK(aj.find("\\\\") != std::string::npos, "backslash escaped in JSON");
    CHECK(aj.find("\"severity\":\"ERROR\"") != std::string::npos, "severity string serialized");

    printf("\n========================================\n");
    printf("Observability results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

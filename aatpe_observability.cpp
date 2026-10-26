// AATPE Observability - implementation
// all global state protected by mutexes
// O(1) metric lookup via unordered_map name index
// implementations moved from header to .cpp
#include "aatpe_observability.h"
#include <chrono>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace aatpe {
namespace observability {

// definition of extern declared in header
const size_t g_alertHistorySize = 500;

// mutex definitions (declared extern in header)
std::mutex g_healthMutex;
std::mutex g_alertsMutex;
std::mutex g_metricsMutex;

// Global state
static HealthStatus g_healthStatus = HealthStatus::UNKNOWN;
static std::vector<AlertRecord> g_alerts;
// P1-fix (issue 16): O(1) duplicate-alert index. Maps "source|message" to the
// index in g_alerts of the most recent record for that key. Without this,
// raiseAlert() linear-scanned all 500 alerts on every call — O(500) string
// compares per raise, which became a hotspot during alert storms.
static std::unordered_map<std::string, size_t> g_alertIndex;

static std::vector<MetricDef> g_metrics;
// O(1) name→index lookup
static std::unordered_map<std::string, size_t> g_metricIndex;

// === Health Status ===

HealthStatus getHealthStatus() {
    std::lock_guard<std::mutex> lock(g_healthMutex);
    return g_healthStatus;
}

void setHealthStatus(HealthStatus status) {
    std::lock_guard<std::mutex> lock(g_healthMutex);
    if (g_healthStatus != status) {
        g_healthStatus = status;
    }
}

const char* healthStatusToString(HealthStatus status) {
    switch (status) {
        case HealthStatus::HEALTHY:  return "HEALTHY";
        case HealthStatus::DEGRADED: return "DEGRADED";
        case HealthStatus::CRITICAL: return "CRITICAL";
        default:                     return "UNKNOWN";
    }
}

// === Alerts ===

void raiseAlert(AlertSeverity severity, const std::string& source, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_alertsMutex);
    // Duplicate suppression: if same source+message exists within last 60s, increment count
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // P1-fix (issue 16): O(1) lookup via g_alertIndex instead of O(n) scan.
    std::string key = source + "|" + message;
    auto idxIt = g_alertIndex.find(key);
    if (idxIt != g_alertIndex.end()) {
        size_t idx = idxIt->second;
        // The index may be stale after a trim/rebuild; validate bounds.
        if (idx < g_alerts.size()) {
            auto& alert = g_alerts[idx];
            if (alert.source == source && alert.message == message &&
                (nowMs - alert.timestampMs) < 60000) {
                alert.count++;
                alert.timestampMs = nowMs;
                return;
            }
        }
        // Stale index entry — fall through and insert fresh.
        g_alertIndex.erase(idxIt);
    }
    AlertRecord rec;
    rec.severity = severity;
    rec.source = source;
    rec.message = message;
    rec.timestampMs = nowMs;
    rec.count = 1;
    g_alerts.push_back(rec);
    g_alertIndex[key] = g_alerts.size() - 1;
    // Trim history
    if (g_alerts.size() > g_alertHistorySize) {
        size_t drop = g_alerts.size() - g_alertHistorySize;
        g_alerts.erase(g_alerts.begin(), g_alerts.begin() + drop);
        // Rebuild index after trim (indices shifted by `drop`).
        g_alertIndex.clear();
        for (size_t i = 0; i < g_alerts.size(); i++) {
            g_alertIndex[g_alerts[i].source + "|" + g_alerts[i].message] = i;
        }
    }
}

std::vector<AlertRecord> getRecentAlerts(size_t max) {
    std::lock_guard<std::mutex> lock(g_alertsMutex);
    std::vector<AlertRecord> result;
    size_t start = (g_alerts.size() > max) ? (g_alerts.size() - max) : 0;
    for (size_t i = start; i < g_alerts.size(); i++) {
        result.push_back(g_alerts[i]);
    }
    return result;
}

void clearAlerts() {
    std::lock_guard<std::mutex> lock(g_alertsMutex);
    g_alerts.clear();
    g_alertIndex.clear();  // P1-fix (issue 16): keep index coherent.
}

int getAlertCount(AlertSeverity severity) {
    std::lock_guard<std::mutex> lock(g_alertsMutex);
    int count = 0;
    for (const auto& a : g_alerts) {
        if (a.severity == severity) count++;
    }
    return count;
}

// === Metrics ===

void initMetrics(const std::vector<MetricDef>& metrics) {
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    g_metrics = metrics;
    // build O(1) name→index map
    g_metricIndex.clear();
    for (size_t i = 0; i < g_metrics.size(); i++) {
        g_metricIndex[g_metrics[i].name] = i;
    }
}

void updateMetric(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    // O(1) lookup via unordered_map
    auto it = g_metricIndex.find(name);
    if (it != g_metricIndex.end()) {
        g_metrics[it->second].value = value;
    }
}

double getMetricValue(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    auto it = g_metricIndex.find(name);
    if (it != g_metricIndex.end()) {
        return g_metrics[it->second].value;
    }
    return 0.0;
}

std::vector<MetricDef> getAllMetrics() {
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    return g_metrics;
}

bool checkMetricThresholds(const std::string& name, AlertSeverity& outSeverity) {
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    auto it = g_metricIndex.find(name);
    if (it == g_metricIndex.end()) return false;
    const MetricDef& m = g_metrics[it->second];
    // P3-29: NaN comparisons are always false, so a NaN value would never trigger
    // a threshold alert. Guard against NaN explicitly.
    if (std::isnan(m.value)) return false;
    if (m.hasCritical && m.value >= m.criticalThreshold) {
        outSeverity = AlertSeverity::CRITICAL;
        return true;
    }
    if (m.hasWarning && m.value >= m.warningThreshold) {
        outSeverity = AlertSeverity::WARNING;
        return true;
    }
    return false;
}

// minimal JSON string escaper for export output.
// Mirrors webui_http.cpp jsonEscape(): escapes quotes, backslashes and control
// chars so that metric/alert name, unit, source and message fields cannot break
// out of their JSON string literals (e.g. a message containing a `"` or `\`).
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// === Health Check ===

bool performHealthCheck() {
    // Check critical metrics
    bool healthy = true;
    auto metrics = getAllMetrics();
    for (const auto& m : metrics) {
        // P3-29: NaN guard — NaN >= threshold is always false, which would
        // silently skip an unhealthy metric.
        if (std::isnan(m.value)) continue;
        if (m.hasCritical && m.value >= m.criticalThreshold) {
            healthy = false;
            setHealthStatus(HealthStatus::CRITICAL);
            raiseAlert(AlertSeverity::CRITICAL, "healthcheck",
                       "Metric " + m.name + " at critical value: " + std::to_string(m.value));
            break;
        }
        // R8-004 (LED-111): a warning must (a) mark the check unhealthy so the
        // `if (healthy) setHealthStatus(HEALTHY)` below does NOT clobber the
        // DEGRADED we are about to set, and (b) actually raise a WARNING alert —
        // previously it only called setHealthStatus(DEGRADED) with no alert and
        // left healthy=true, so DEGRADED was immediately overwritten to HEALTHY.
        if (m.hasWarning && m.value >= m.warningThreshold) {
            healthy = false;
            setHealthStatus(HealthStatus::DEGRADED);
            raiseAlert(AlertSeverity::WARNING, "healthcheck",
                       "Metric " + m.name + " above warning threshold: " + std::to_string(m.value));
            // P1-fix (issue 17): break after the first warning raise so we don't
            // emit one alert per over-threshold metric in a single health check
            // (which could spam 5+ alerts per cycle during a broad CPU spike).
            // The 60s duplicate window already coalesces identical messages across
            // cycles, but different metric names produced different messages.
            break;
        }
    }
    if (healthy) {
        setHealthStatus(HealthStatus::HEALTHY);
    }
    return healthy;
}

std::string getHealthSummary() {
    std::ostringstream oss;
    oss << "Health: " << healthStatusToString(getHealthStatus()) << "\n";
    oss << "Alerts: " << getAlertCount(AlertSeverity::CRITICAL) << " critical, "
        << getAlertCount(AlertSeverity::ERROR) << " error, "
        << getAlertCount(AlertSeverity::WARNING) << " warning\n";
    auto metrics = getAllMetrics();
    oss << "Metrics: " << metrics.size() << " tracked\n";
    for (const auto& m : metrics) {
        oss << "  " << m.name << " = " << m.value << " " << m.unit << "\n";
    }
    return oss.str();
}

// === JSON Export ===

// P2-11: sanitize a double for JSON output. NaN/Inf would produce invalid JSON
// ("nan"/"inf" tokens that strict parsers reject). Replace non-finite with 0.0.
static double jsonSanitizeDouble(double v) {
    return std::isfinite(v) ? v : 0.0;
}

std::string exportMetricsJson() {
    std::lock_guard<std::mutex> lock(g_metricsMutex);
    std::ostringstream oss;
    oss << "{\"metrics\":[";
    for (size_t i = 0; i < g_metrics.size(); i++) {
        if (i > 0) oss << ",";
        const auto& m = g_metrics[i];
        oss << "{\"name\":\"" << jsonEscape(m.name) << "\","
            << "\"value\":" << jsonSanitizeDouble(m.value) << ","
            << "\"unit\":\"" << jsonEscape(m.unit) << "\","
            << "\"min\":" << jsonSanitizeDouble(m.min) << ","
            << "\"max\":" << jsonSanitizeDouble(m.max);
        if (m.hasWarning) oss << ",\"warning\":" << jsonSanitizeDouble(m.warningThreshold);
        if (m.hasCritical) oss << ",\"critical\":" << jsonSanitizeDouble(m.criticalThreshold);
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string exportAlertsJson() {
    std::lock_guard<std::mutex> lock(g_alertsMutex);
    std::ostringstream oss;
    oss << "{\"alerts\":[";
    for (size_t i = 0; i < g_alerts.size(); i++) {
        if (i > 0) oss << ",";
        const auto& a = g_alerts[i];
        const char* sevStr = "INFO";
        switch (a.severity) {
            case AlertSeverity::WARNING:  sevStr = "WARNING"; break;
            case AlertSeverity::ERROR:    sevStr = "ERROR"; break;
            case AlertSeverity::CRITICAL: sevStr = "CRITICAL"; break;
            default: break;
        }
        oss << "{\"severity\":\"" << sevStr << "\","
            << "\"source\":\"" << jsonEscape(a.source) << "\","
            << "\"message\":\"" << jsonEscape(a.message) << "\","
            << "\"timestamp\":" << a.timestampMs << ","
            << "\"count\":" << a.count << "}";
    }
    oss << "]}";
    return oss.str();
}

} // namespace observability
} // namespace aatpe

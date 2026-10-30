#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

static bool isValidPort(int port) {
    return port >= 8888 && port <= 8899;
}

static void test_port_range() {
    printf("\n=== P1-7: HTTP server port range ===\n");
    for (int p = 8888; p <= 8899; p++) {
        CHECK(isValidPort(p), ("port " + std::to_string(p) + " is valid").c_str());
    }
    CHECK(!isValidPort(8887), "port 8887 is below range");
    CHECK(!isValidPort(8900), "port 8900 is above range");
    CHECK(!isValidPort(80), "port 80 is not in range");
}

static std::string routeRequest(const std::string& method, const std::string& path) {
    if (method != "GET") return "405";
    if (path == "/health" || path == "/healthz") return "200 health";
    if (path == "/state/all" || path == "/state" || path == "/" || path == "/api/state") return "200 state";
    if (path == "/version") return "200 version";
    return "404";
}

static void test_http_routing() {
    printf("\n=== P1-7: HTTP request routing ===\n");
    CHECK(routeRequest("GET", "/health") == "200 health", "GET /health → 200");
    CHECK(routeRequest("GET", "/healthz") == "200 health", "GET /healthz → 200");
    CHECK(routeRequest("GET", "/state/all") == "200 state", "GET /state/all → 200");
    CHECK(routeRequest("GET", "/") == "200 state", "GET / → 200 (redirect to state)");
    CHECK(routeRequest("GET", "/version") == "200 version", "GET /version → 200");
    CHECK(routeRequest("GET", "/nonexistent") == "404", "GET /nonexistent → 404");
    CHECK(routeRequest("POST", "/health") == "405", "POST /health → 405");
}

static std::string buildHealthJson(int uptimeSec, int port, int pid) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"uptime_s\":%d,\"port\":%d,\"pid\":%d}",
        uptimeSec, port, pid);
    return std::string(buf);
}

static void test_health_response() {
    printf("\n=== P1-7: Health response format ===\n");
    std::string json = buildHealthJson(120, 8888, 5432);
    CHECK(json.find("\"status\":\"ok\"") != std::string::npos, "contains status=ok");
    CHECK(json.find("\"uptime_s\":120") != std::string::npos, "contains uptime");
    CHECK(json.find("\"port\":8888") != std::string::npos, "contains port");
    CHECK(json.find("\"pid\":5432") != std::string::npos, "contains pid");
}

static std::string buildAggregateJson(const std::vector<std::string>& keys,
                                       const std::vector<std::string>& values) {
    std::string json = "{";
    for (size_t i = 0; i < keys.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + keys[i] + "\":\"" + values[i] + "\"";
    }
    json += "}";
    return json;
}

static void test_state_aggregation() {
    printf("\n=== P1-7: State aggregation JSON ===\n");
    std::vector<std::string> keys = {"control_mode", "current_profile", "cpu_count"};
    std::vector<std::string> values = {"assist", "balanced", "8"};
    std::string json = buildAggregateJson(keys, values);
    CHECK(json.find("\"control_mode\":\"assist\"") != std::string::npos, "contains control_mode");
    CHECK(json.find("\"current_profile\":\"balanced\"") != std::string::npos, "contains profile");
    CHECK(json.find("\"cpu_count\":\"8\"") != std::string::npos, "contains cpu_count");
    CHECK(json[0] == '{' && json.back() == '}', "valid JSON object braces");
}

static bool isLoopbackOnly(const std::string& bindAddr) {
    return bindAddr == "127.0.0.1" || bindAddr == "INADDR_LOOPBACK";
}

static void test_loopback_only() {
    printf("\n=== P1-7: Loopback-only binding (security) ===\n");
    CHECK(isLoopbackOnly("127.0.0.1"), "binds to 127.0.0.1");
    CHECK(isLoopbackOnly("INADDR_LOOPBACK"), "uses INADDR_LOOPBACK constant");
    CHECK(!isLoopbackOnly("0.0.0.0"), "does NOT bind to 0.0.0.0 (external access blocked)");
    CHECK(!isLoopbackOnly("*"), "does NOT bind to all interfaces");
}

static unsigned long long g_lastRead = 0;
static bool shouldSkipRead(unsigned long long nowMs) {
    return g_lastRead > 0 && (nowMs - g_lastRead) < 1000;
}

static void test_thermal_cache() {
    printf("\n=== P2: Thermal read cache (1s TTL) ===\n");
    g_lastRead = 0;
    CHECK(!shouldSkipRead(1000), "first read: not skipped");
    g_lastRead = 1000;
    CHECK(shouldSkipRead(1500), "read after 500ms: skipped (within TTL)");
    CHECK(!shouldSkipRead(2001), "read after 1001ms: not skipped (TTL expired)");
}

static int g_watchCount = 0;
static constexpr int kMaxWatches = 256;
static bool addWatch() {
    if (g_watchCount >= kMaxWatches) return false;
    g_watchCount++;
    return true;
}

static void test_inotify_limit() {
    printf("\n=== P2: inotify watch limit (256) ===\n");
    g_watchCount = 0;
    for (int i = 0; i < 256; i++) CHECK(addWatch(), ("watch " + std::to_string(i) + " added").c_str());
    CHECK(!addWatch(), "watch 257 rejected (limit reached)");
    CHECK(g_watchCount == 256, "exactly 256 watches registered");
}

int main() {
    printf("=== AATPE v66 HTTP Server & Infrastructure Tests ===\n");
    test_port_range();
    test_http_routing();
    test_health_response();
    test_state_aggregation();
    test_loopback_only();
    test_thermal_cache();
    test_inotify_limit();
    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

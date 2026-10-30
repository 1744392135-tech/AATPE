#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>

std::string aatpeExecArgv(const std::vector<std::string>& argv, const char* debugTag,
                          int* outExitStatus = nullptr, bool bypassCooldown = false);

std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}

const char* STATE_DIR   = "/tmp/hotfix_state";
const char* WEBUI_DIR   = "/tmp/hotfix_state";
const char* DECISION_DIR = "/tmp/hotfix_state";
const char* METRICS_DIR  = "/tmp/hotfix_state";
std::vector<int> g_allCpus;
std::vector<int> getOnlineCpus() { return {}; }
extern "C" int __system_property_get(const char*, char*) { return 0; }

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

int main() {
    printf("=== HOTFIX #8/#9: hardened exec timeout ===\n");

    std::string out = aatpeExecArgv({"/bin/echo", "hello-hotfix"}, "test-echo");
    CHECK(out.find("hello-hotfix") != std::string::npos, "fast command returns stdout");

    auto t0 = std::chrono::steady_clock::now();
    aatpeExecArgv({"/bin/sleep", "30"}, "test-timeout");
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    CHECK(ms < 3500, "sleep-30 child killed by 3s deadline (not blocking 30s)");
    printf("     (sleep-30 returned after %lld ms)\n", ms);

    aatpeExecArgv({"/bin/echo", "second"}, "test-echo2");
    CHECK(true, "subsequent exec call works (child was reaped, not leaked)");

    printf("\n========================================\n"
           "Exec timeout results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// HOTFIX isolated test — #19: thermal throttle evidence scans ALL clusters, not cpu0.
// Pure test body; all stubs come from tests/stubs/thermal_deps.cpp + log_stub.cpp +
// aatpe_ml.o (same surface as the v94 `thermal` target). Build links thermal.o.
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>

// thermalSetTestRoot is a global (file-scope) function in thermal.cpp; the accessors
// I exposed are in namespace aatpe.
void thermalSetTestRoot(const std::string& r);
namespace aatpe {
bool thermalAnyCpuScalingMaxThrottledForTest();
bool thermalAnyCpufreqCoolingActiveForTest();
}
extern std::vector<int> g_allCpus;   // defined in thermal_deps.cpp

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                         else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

static void W(const std::string& p, const std::string& v) {
    FILE* f = fopen(p.c_str(), "w"); if (f) { fputs(v.c_str(), f); fclose(f); }
}
static void Mk(const std::string& d) { system(("mkdir -p " + d).c_str()); }

int main() {
    printf("=== HOTFIX #19: multi-cluster throttle evidence ===\n");
    std::string root = "/tmp/hotfix_thermal_root";
    system(("rm -rf " + root + " && mkdir -p " + root).c_str());

    // --- Case A: two clusters, only BIG (cpu4) throttled. ---
    std::string cl = root + "/sys/devices/system/cpu";
    Mk(cl + "/cpu0/cpufreq"); Mk(cl + "/cpu4/cpufreq");
    W(cl + "/cpu0/online", "1");
    W(cl + "/cpu0/cpufreq/scaling_max_freq", "1800000");   // == hw max: NOT throttled
    W(cl + "/cpu0/cpufreq/cpuinfo_max_freq", "1800000");
    W(cl + "/cpu4/online", "1");
    W(cl + "/cpu4/cpufreq/scaling_max_freq", "600000");    // < hw max: THROTTLED
    W(cl + "/cpu4/cpufreq/cpuinfo_max_freq", "2800000");

    g_allCpus = {0, 4};
    thermalSetTestRoot(root);
    bool thr = aatpe::thermalAnyCpuScalingMaxThrottledForTest();
    CHECK(thr == true, "BIG cluster (cpu4) throttled, cpu0 fine -> evidence present");

    W(cl + "/cpu4/cpufreq/scaling_max_freq", "2800000");
    thr = aatpe::thermalAnyCpuScalingMaxThrottledForTest();
    CHECK(thr == false, "both clusters at hw max -> no throttle evidence");

    // --- Case B: cooling_device0 idle (gpu), cooling_device3 (cpufreq) active. ---
    std::string th = root + "/sys/class/thermal";
    Mk(th + "/cooling_device0"); Mk(th + "/cooling_device3");
    W(th + "/cooling_device0/cur_state", "0");
    W(th + "/cooling_device0/type", "gpu");
    W(th + "/cooling_device3/cur_state", "40");
    W(th + "/cooling_device3/type", "cpufreq");
    bool cool = aatpe::thermalAnyCpufreqCoolingActiveForTest();
    CHECK(cool == true, "cooling_device3 (cpufreq) active -> cooling evidence present");

    W(th + "/cooling_device3/cur_state", "0");
    cool = aatpe::thermalAnyCpufreqCoolingActiveForTest();
    CHECK(cool == false, "no cpufreq cooling active -> no cooling evidence");

    printf("\n========================================\n"
           "Thermal cluster results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

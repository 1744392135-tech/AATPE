// test_webui_percpu.cpp — #98 regression.
//
// writePerCpuJson() must ALWAYS emit /webui/percpu.json:
//   T-PERCPU-A: with topology + per-CPU metrics present, the file carries one
//               object per CPU (cpu/type/util/cur_freq/freq_pct/temp/capacity),
//               "count" == number of CPUs, "degraded" == "ok".
//   T-PERCPU-B: topology known but g_cpuMetrics empty -> the file is STILL
//               written, "cpus":[] , "count":0, "degraded":"no-cpu-metrics"
//               (explicit reason, not a silent empty object).
//   T-PERCPU-C: topology itself missing (g_allCpus empty) -> "degraded":"no-topology".
//
// Host build (-DAATPE_HOST_TEST): WEBUI_DIR points at a temp dir via the stub.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "aatped.h"

extern std::vector<int> g_allCpus;
extern std::vector<int> g_littlePool;
extern std::vector<int> g_midPool;
extern std::vector<int> g_bigPool;
extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::mutex g_topologyRwMutex;
extern std::mutex g_cpuMetricsMutex;

void writePerCpuJson();

static std::string slurp(const std::string& p) {
    std::ifstream f(p.c_str());
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static void seedTopology(const std::vector<int>& all) {
    std::lock_guard<std::mutex> l(g_topologyRwMutex);
    g_littlePool.clear(); g_midPool.clear(); g_bigPool.clear();
    for (int c : all) {
        if (c < 4) g_littlePool.push_back(c); else g_bigPool.push_back(c);
    }
    g_allCpus = all;
}
static void seedMetrics(const std::vector<int>& cpus) {
    std::lock_guard<std::mutex> l(g_cpuMetricsMutex);
    g_cpuMetrics.clear();
    for (int c : cpus) {
        CpuMetrics m; m.util = 10 + c; m.capacity = 50 + c; m.freqPct = 20 + c;
        m.curFreq = 1800000 + c*100000; m.maxFreq = 2800000; m.temp = 40;
        g_cpuMetrics[c] = m;
    }
}

int main() {
    mkdir(WEBUI_DIR, 0755);
    const std::string path = std::string(WEBUI_DIR) + "/percpu.json";

    printf("=== #98: writePerCpuJson always emits, explicit degraded codes ===\n");

    // T-PERCPU-A: full data.
    seedTopology({0,1,2,3,4,5,6,7});
    seedMetrics({0,1,2,3,4,5,6,7});
    writePerCpuJson();
    std::string a = slurp(path);
    printf("  A: %s\n", a.c_str());
    CHECK(a.find("\"degraded\":\"ok\"") != std::string::npos, "A: degraded=ok with data");
    CHECK(a.find("\"count\":8") != std::string::npos, "A: count=8");
    CHECK(a.find("\"cpu\":0") != std::string::npos, "A: cpu0 entry present");
    CHECK(a.find("\"cpu\":7") != std::string::npos, "A: cpu7 entry present");
    CHECK(a.find("\"type\":\"little\"") != std::string::npos, "A: little type tagged");
    CHECK(a.find("\"type\":\"big\"") != std::string::npos, "A: big type tagged");

    // T-PERCPU-B: topology known, metrics empty.
    seedTopology({0,1,2,3,4,5,6,7});
    { std::lock_guard<std::mutex> l(g_cpuMetricsMutex); g_cpuMetrics.clear(); }
    writePerCpuJson();
    std::string b = slurp(path);
    printf("  B: %s\n", b.c_str());
    CHECK(b.find("\"cpus\":[]") != std::string::npos, "B: cpus empty array");
    CHECK(b.find("\"count\":0") != std::string::npos, "B: count=0");
    CHECK(b.find("\"degraded\":\"no-cpu-metrics\"") != std::string::npos,
          "B: explicit degraded=no-cpu-metrics (not silent)");

    // T-PERCPU-C: topology itself missing.
    seedTopology({});
    { std::lock_guard<std::mutex> l(g_cpuMetricsMutex); g_cpuMetrics.clear(); }
    writePerCpuJson();
    std::string c = slurp(path);
    printf("  C: %s\n", c.c_str());
    CHECK(c.find("\"degraded\":\"no-topology\"") != std::string::npos,
          "C: explicit degraded=no-topology");

    printf("\n=== percpu result: pass=%d fail=%d ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}

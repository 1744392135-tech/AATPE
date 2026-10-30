#include "c4_freezer.h"
#include <chrono>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>

using namespace aatpe::c4freezer;

static double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0.0;
    size_t idx = (size_t)std::llround(p * (v.size() - 1));
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

template <typename F>
static std::vector<double> timeit(F&& f, int iters) {
    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return samples;
}

int main() {
    const int N = 200000;
    int sink = 0;

    C4DecisionInputs in{};
    in.featureEnabled = true; in.isForeground = false; in.alreadyFrozen = false;
    in.freezerAvailable = true; in.audioActive = false;
    auto dDecide = timeit([&]{
        C4Decision d = decideC4Freeze(in);
        sink += (int)d.action;
    }, N);

    auto dTarget = timeit([&]{
        std::string r = c4ChooseTarget("com.game", "com.android.launcher", false);
        sink += (int)r.size();
    }, N);

    auto dJson = timeit([&]{
        std::string r = c4StateJson(true, "com.game", true, "/sys/fs/cgroup/uid_1/cg_1",
                                    "UNFREEZE(foreground)", false, true, "");
        sink += (int)r.size();
    }, N);

    auto report = [&](const char* name, const std::vector<double>& s) {
        double p50 = percentile(s, 0.50), p95 = percentile(s, 0.95), p99 = percentile(s, 0.99);
        double mean = 0; for (double x : s) mean += x; mean /= s.size();
        double ops = 1e9 / mean;
        std::printf("%-22s P50=%8.1fns P95=%9.1fns P99=%9.1fns mean=%8.1fns  ops/s=%.0f\n",
                    name, p50, p95, p99, mean, ops);
        if (!(p50 <= p95 && p95 <= p99)) { std::printf("BENCH-ASSERT percentile order FAIL\n"); return 1; }
        if (p99 > 100000.0) { std::printf("BENCH-ASSERT p99>100us FAIL (%s)\n", name); return 1; }
        return 0;
    };

    std::printf("=== AATPE v4.4.6 decision hot-path benchmark (N=%d, 1 core) ===\n", N);
    int rc = 0;
    rc |= report("decideC4Freeze", dDecide);
    rc |= report("c4ChooseTarget", dTarget);
    rc |= report("c4StateJson", dJson);
    std::printf("sink=%d\n", sink);
    std::printf(rc == 0 ? "BENCH OK\n" : "BENCH FAIL\n");
    return rc;
}

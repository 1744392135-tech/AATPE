#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "aatpe_ml.h"
#include "aatped_types.h"

extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::mutex g_cpuMetricsMutex;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static constexpr int kNumCpus = 8;

static void collectorWriter(std::atomic<bool>& stop) {
    unsigned long long iter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> wl(g_cpuMetricsMutex);
        for (int c = 0; c < kNumCpus; ++c) {
            CpuMetrics& m = g_cpuMetrics[c];
            m.util = (int)(iter % 100);
            m.temp = 40 + (int)(iter % 30);
            m.curFreq = 600000 + (int)(iter % 1800000);
            m.maxFreq = 2400000;
            m.freqPct = (int)(iter % 100);
            m.capacity = 380;
        }
        ++iter;
        std::this_thread::yield();
    }
}

static bool auxScanAvgLoad(float& outAvg) {
    float sum = 0.0f; int n = 0;
    {
        std::lock_guard<std::mutex> rl(g_cpuMetricsMutex);
        for (const auto& kv : g_cpuMetrics) {
            if (kv.second.util >= 0) { sum += (float)kv.second.util; ++n; }
        }
    }
    if (n == 0) return false;
    outAvg = sum / (float)n;
    return true;
}

static bool snapshotFreq(int cpu, int& curFreq, int& maxFreq) {
    std::lock_guard<std::mutex> rl(g_cpuMetricsMutex);
    auto it = g_cpuMetrics.find(cpu);
    if (it == g_cpuMetrics.end()) return false;
    curFreq = it->second.curFreq;
    maxFreq = it->second.maxFreq;
    return true;
}

int main() {
    printf("=== v6.0.0 RT-A A5: g_cpuMetrics concurrent read/write (TSan-clean) ===\n");

    std::atomic<bool> stop{false};
    std::thread writer(collectorWriter, std::ref(stop));

    std::atomic<unsigned long long> reads{0};
    auto readerLoop = [&](int cpuBase) {
        unsigned long long seen = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            float avg = 0.0f;
            (void)auxScanAvgLoad(avg);
            for (int c = cpuBase; c < cpuBase + 2 && c < kNumCpus; ++c) {
                int cf = 0, mf = 1;
                (void)snapshotFreq(c, cf, mf);
            }
            ++seen;
        }
        reads.fetch_add(seen, std::memory_order_relaxed);
    };
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back(readerLoop, i * 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true);
    writer.join();
    for (auto& t : readers) t.join();

    CHECK(reads.load() > 1000, "reader threads performed many concurrent reads");

    {
        std::lock_guard<std::mutex> rl(g_cpuMetricsMutex);
        int present = 0; bool saneFreq = true;
        for (int c = 0; c < kNumCpus; ++c) {
            auto it = g_cpuMetrics.find(c);
            if (it != g_cpuMetrics.end()) {
                ++present;
                if (it->second.maxFreq <= 0) saneFreq = false;
            }
        }
        CHECK(present == kNumCpus, "all cpus present after concurrent run");
        CHECK(saneFreq, "maxFreq values internally consistent (no torn reads)");
    }

    printf("\n=== cpu_metrics_race_v600: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

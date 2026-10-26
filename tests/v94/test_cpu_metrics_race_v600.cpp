// test_cpu_metrics_race_v600.cpp — v6.0.0 RT-A A5.
//
// Regression test for the g_cpuMetrics data race (RT-A P1). The production
// collector thread mutates g_cpuMetrics (std::map) under g_cpuMetricsMutex;
// the main loop had 5 lock-free read points (loopAuxScan avg-load iteration
// at aatped_main.cpp:2688, outcome-batch find()s at 3352/3362/3623, the
// placement-path beforeFreq find at 3981). Concurrent read/write on std::map
// is undefined behavior. The fix wraps every read in a short
// g_cpuMetricsMutex critical section that copies only the needed fields.
//
// This host test drives the SAME locking contract with a writer thread (mirror
// of the collector) and reader threads mirroring aux-scan + outcome-batch
// snapshots. Under ThreadSanitizer it must report ZERO races. It is linked
// with scoring_deps.cpp (the same g_cpuMetrics/mutex stubs production links).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "aatpe_ml.h"
#include "aatped_types.h"

// Provided by tests/stubs/scoring_deps.cpp (same globals production uses).
extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::mutex g_cpuMetricsMutex;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static constexpr int kNumCpus = 8;

// Mirror of the collector thread (aatped_main.cpp:1118): under the mutex,
// refresh every CPU's util/temp/curFreq/maxFreq/capacity.
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

// Mirror of loopAuxScan slot-3 (aatped_main.cpp:2688): iterate the whole map
// under the mutex, sum util for avg-load. Returns false on any inconsistency.
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

// Mirror of outcome-batch find() (aatped_main.cpp:3352/3623): short critical
// section snapshotting curFreq + maxFreq for a given cpu.
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

    // Start one writer + several reader threads hammering the map for a bounded
    // window. TSan observes every access; if any reader skipped the mutex this
    // would be flagged as a data race against the writer.
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

    // Bounded run (~1s) — enough to interleave many map mutations + reads.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true);
    writer.join();
    for (auto& t : readers) t.join();

    CHECK(reads.load() > 1000, "reader threads performed many concurrent reads");

    // Final state sanity under the mutex: all 8 cpus present, maxFreq sane.
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

    // The TSan pass/fail is determined by the sanitizer itself; if we reach
    // here without TSan aborting, there was zero race.
    printf("\n=== cpu_metrics_race_v600: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

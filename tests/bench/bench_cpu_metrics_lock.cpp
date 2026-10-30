#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

struct CpuMetrics {
    int capacity = 4;
    int curFreq = 0;
    int maxFreq = 1;
    int freqPct = 0;
    int util = 0;
    int temp = 0;
};

static constexpr int NCPU = 8;

static std::unordered_map<int, CpuMetrics> g_cpuMetrics;
static std::mutex g_cpuMetricsMutex;

static inline int read_R1_avgLoad() {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    long long sum = 0; int n = 0;
    for (auto& kv : g_cpuMetrics) { if (kv.second.util >= 0) { sum += kv.second.util; ++n; } }
    return n ? (int)(sum / n) : 0;
}
static inline int read_R2_outcomeFreq(int cur, int tgt) {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    int afterFreq = 0, base = 0;
    auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) { afterFreq = it->second.curFreq; base = it->second.maxFreq; }
    auto jt = g_cpuMetrics.find(tgt);
    if (jt != g_cpuMetrics.end() && jt->second.maxFreq > base) base = jt->second.maxFreq;
    return afterFreq + base;
}
static inline int read_R3_baselineDuty(int cur) {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    int cf = 0, mf = 1;
    auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) { cf = it->second.curFreq; if (it->second.maxFreq > 0) mf = it->second.maxFreq; }
    return cf + mf;
}
static inline int read_R4_beforeFreq(int cur) {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    int bf = 0;
    auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) bf = it->second.curFreq;
    return bf;
}
static inline int read_R5_curUtil(int cur) {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    int u = 0;
    auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) u = it->second.util;
    return u;
}

static inline int read_R1_avgLoad_nolock() {
    long long sum = 0; int n = 0;
    for (auto& kv : g_cpuMetrics) { if (kv.second.util >= 0) { sum += kv.second.util; ++n; } }
    return n ? (int)(sum / n) : 0;
}
static inline int read_R2_nolock(int cur, int tgt) {
    int afterFreq = 0, base = 0;
    auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) { afterFreq = it->second.curFreq; base = it->second.maxFreq; }
    auto jt = g_cpuMetrics.find(tgt);
    if (jt != g_cpuMetrics.end() && jt->second.maxFreq > base) base = jt->second.maxFreq;
    return afterFreq + base;
}
static inline int read_R3_nolock(int cur) {
    int cf = 0, mf = 1;
    auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) { cf = it->second.curFreq; if (it->second.maxFreq > 0) mf = it->second.maxFreq; }
    return cf + mf;
}
static inline int read_R4_nolock(int cur) {
    int bf = 0; auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) bf = it->second.curFreq;
    return bf;
}
static inline int read_R5_nolock(int cur) {
    int u = 0; auto it = g_cpuMetrics.find(cur);
    if (it != g_cpuMetrics.end()) u = it->second.util;
    return u;
}

using clk = std::chrono::steady_clock;
static inline double ns_since(clk::time_point t) {
    return std::chrono::duration<double, std::nano>(clk::now() - t).count();
}
static double percentile(std::vector<double>& v, double p) {
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0;
    size_t idx = (size_t)std::llround(p * (v.size() - 1));
    return v[std::min(idx, v.size() - 1)];
}

static long g_sink = 0;

int main() {
    for (int c = 0; c < NCPU; ++c) {
        CpuMetrics m; m.curFreq = 800 + c * 100; m.maxFreq = 2400; m.util = 30 + c; m.temp = 40;
        g_cpuMetrics[c] = m;
    }

    std::printf("=== g_cpuMetrics short-lock microbench (NCPU=%d, O3, pinned) ===\n", NCPU);

    const int N = 2000000;
    struct Sec { const char* name; std::vector<double> locked, nolock; };
    Sec secs[5] = {
        {"R1 loopAuxScan avgLoad", {}, {}},
        {"R2 outcomeFreq(2find)",  {}, {}},
        {"R3 baselineDuty",        {}, {}},
        {"R4 beforeFreq",          {}, {}},
        {"R5 curUtil",              {}, {}},
    };
    auto bench = [&](int idx) {
        for (int i = 0; i < 20000; ++i) {
            switch (idx) {
                case 0: g_sink += read_R1_avgLoad(); break;
                case 1: g_sink += read_R2_outcomeFreq(i % NCPU, (i+3) % NCPU); break;
                case 2: g_sink += read_R3_baselineDuty(i % NCPU); break;
                case 3: g_sink += read_R4_beforeFreq(i % NCPU); break;
                case 4: g_sink += read_R5_curUtil(i % NCPU); break;
            }
        }
        for (int i = 0; i < N; ++i) {
            auto t0 = clk::now();
            int r = 0;
            switch (idx) {
                case 0: r = read_R1_avgLoad(); break;
                case 1: r = read_R2_outcomeFreq(i % NCPU, (i+3) % NCPU); break;
                case 2: r = read_R3_baselineDuty(i % NCPU); break;
                case 3: r = read_R4_beforeFreq(i % NCPU); break;
                case 4: r = read_R5_curUtil(i % NCPU); break;
            }
            secs[idx].locked.push_back(ns_since(t0));
            g_sink += r;
        }
        for (int i = 0; i < 20000; ++i) {
            switch (idx) {
                case 0: g_sink += read_R1_avgLoad_nolock(); break;
                case 1: g_sink += read_R2_nolock(i % NCPU, (i+3) % NCPU); break;
                case 2: g_sink += read_R3_nolock(i % NCPU); break;
                case 3: g_sink += read_R4_nolock(i % NCPU); break;
                case 4: g_sink += read_R5_nolock(i % NCPU); break;
            }
        }
        for (int i = 0; i < N; ++i) {
            auto t0 = clk::now();
            int r = 0;
            switch (idx) {
                case 0: r = read_R1_avgLoad_nolock(); break;
                case 1: r = read_R2_nolock(i % NCPU, (i+3) % NCPU); break;
                case 2: r = read_R3_nolock(i % NCPU); break;
                case 3: r = read_R4_nolock(i % NCPU); break;
                case 4: r = read_R5_nolock(i % NCPU); break;
            }
            secs[idx].nolock.push_back(ns_since(t0));
            g_sink += r;
        }
    };
    std::printf("\n--- PART1: uncontended critical-section time (single thread, ns) ---\n");
    std::printf("%-24s | %10s | %10s | %10s | %10s | %10s\n",
                "section", "lock P50", "lock P99", "nolock P50", "delta P50", "delta P99");
    double lock_p50_sum = 0, delta_p50_sum = 0;
    for (int s = 0; s < 5; ++s) {
        bench(s);
        double lp50 = percentile(secs[s].locked, 0.50);
        double lp99 = percentile(secs[s].locked, 0.99);
        double up50 = percentile(secs[s].nolock, 0.50);
        double dp50 = lp50 - up50;
        double dp99 = lp99 - percentile(secs[s].nolock, 0.99);
        lock_p50_sum += lp50; delta_p50_sum += dp50;
        std::printf("%-24s | %10.1f | %10.1f | %10.1f | %10.1f | %10.1f\n",
                    secs[s].name, lp50, lp99, up50, dp50, dp99);
    }
    std::printf("%-24s | %10.1f | %10.1f | %10.1f | %10.1f | %10.1f\n",
                "SUM (5 locks/cycle)", lock_p50_sum, 0.0, 0.0, delta_p50_sum, 0.0);

    std::printf("\n--- PART2: 1 writer + 4 readers concurrency ---\n");
    std::atomic<bool> stop{false};
    std::atomic<long long> cycles{0};
    static std::vector<std::vector<double>> waitSamples(4);

    auto writer = [&]() {
        int tick = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(g_cpuMetricsMutex);
                for (int c = 0; c < NCPU; ++c) {
                    g_cpuMetrics[c].util = (tick + c * 7) % 100;
                    g_cpuMetrics[c].curFreq = 800 + ((tick + c) % 16) * 100;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            ++tick;
        }
    };
    auto reader = [&](int idx) {
        long long local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (int k = 0; k < 5; ++k) {
                auto t0 = clk::now();
                g_cpuMetricsMutex.lock();
                double wait = ns_since(t0);
                waitSamples[idx].push_back(wait);
                int cur = k % NCPU;
                auto it = g_cpuMetrics.find(cur);
                int v = (it != g_cpuMetrics.end()) ? it->second.util : 0;
                g_cpuMetricsMutex.unlock();
                local += v;
            }
            ++cycles;
        }
        g_sink += local;
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back(reader, i);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    auto tw0 = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    double elapsed_s = ns_since(tw0) / 1e9;
    stop.store(true);
    w.join();
    for (auto& r : readers) r.join();

    long long cyc = cycles.load();
    double cycles_per_sec = cyc / elapsed_s;
    std::printf("decision cycles completed (4 readers, 2.0s window): %lld\n", cyc);
    std::printf("throughput: %.0f decision cycles/s across 4 readers (%.0f reader-locks/s)\n",
                cycles_per_sec, cycles_per_sec * 5);

    std::vector<double> allWait;
    for (auto& v : waitSamples) allWait.insert(allWait.end(), v.begin(), v.end());
    if (allWait.empty()) {
        std::printf("reader acquire-wait: no samples?!\n");
    } else {
        double wp50 = percentile(allWait, 0.50);
        double wp95 = percentile(allWait, 0.95);
        double wp99 = percentile(allWait, 0.99);
        double wmax = *std::max_element(allWait.begin(), allWait.end());
        double mean = std::accumulate(allWait.begin(), allWait.end(), 0.0) / allWait.size();
        std::printf("reader acquire-wait (contention) n=%zu: mean=%.1fns P50=%.1fns P95=%.1fns P99=%.1fns max=%.1fns\n",
                    allWait.size(), mean, wp50, wp95, wp99, wmax);
    }

    std::printf("\n--- PART3: lock cost vs 500ms decision cycle ---\n");
    double per_cycle_ns = delta_p50_sum;
    double cycle_period_ns = 500.0 * 1e6;
    double frac = per_cycle_ns / cycle_period_ns * 100.0;
    std::printf("added lock overhead per 500ms decision cycle: %.1f ns = %.3f us\n",
                per_cycle_ns, per_cycle_ns / 1e3);
    std::printf("as fraction of the 500ms cycle: %.6f%%\n", frac);
    std::printf("(full critical-section time incl. find+copy, summed P50: %.1f ns = %.3f us)\n",
                lock_p50_sum, lock_p50_sum / 1e3);

    std::printf("\nsink=%ld (prevents elision)\nBENCH DONE\n", g_sink);
    return 0;
}

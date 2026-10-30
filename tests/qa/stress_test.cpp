#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include <chrono>

#include "aatpe_ebpf.h"

using aatpe_ebpf::SpscBoundedQueue;

static int g_fail = 0;
#define CHECK(c, m) do { if (c) { printf("  [PASS] %s\n", m); } \
    else { printf("  [FAIL] %s\n", m); g_fail++; } } while(0)

struct Ev { uint64_t seq; uint32_t cpu; uint64_t ts; };

static void test_spsc_ordered(uint64_t total) {
    printf("=== SPSC ordered delivery (%llu events, ring=64) ===\n",
           (unsigned long long)total);
    SpscBoundedQueue<Ev> q(64);
    std::atomic<bool> prod_done{false};
    uint64_t received = 0;
    uint64_t max_seq_seen = 0;

    std::thread prod([&] {
        for (uint64_t i = 0; i < total; ++i) {
            while (!q.push(Ev{i, (uint32_t)(i % 8), 1000 + i})) {
                std::this_thread::yield();
            }
        }
        prod_done.store(true);
    });

    std::vector<Ev> out;
    while (!prod_done.load() || q.sizeApprox() > 0) {
        out.clear();
        size_t n = q.popBatch(out, 32);
        for (size_t k = 0; k < n; ++k) {
            if (out[k].seq != received) { g_fail++;
                printf("  [FAIL] seq mismatch: got %llu expected %llu\n",
                       (unsigned long long)out[k].seq,
                       (unsigned long long)received); }
            received++;
        }
    }
    prod.join();
    CHECK(received == total, "all non-dropped events received (no loss beyond drops)");
    printf("  dropped=%llu (expected 0, ring was drained)\n",
           (unsigned long long)q.droppedCount());
}

static void test_ring_full() {
    printf("=== ring-full drop-newest (ring=16, producer faster than consumer) ===\n");
    SpscBoundedQueue<Ev> q(16);
    const uint64_t total = 200000;
    std::atomic<bool> prod_done{false};
    std::thread prod([&] {
        for (uint64_t i = 0; i < total; ++i) q.push(Ev{i, 0, 0});
        prod_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<Ev> out;
    uint64_t got = 0;
    while (!prod_done.load() || q.sizeApprox() > 0) {
        out.clear();
        q.popBatch(out, 0);
        got += out.size();
        if (!prod_done.load()) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    prod.join();
    out.clear(); q.popBatch(out, 0); got += out.size();
    uint64_t dropped = q.droppedCount();
    printf("  sent=%llu got=%llu dropped=%llu (got+dropped=%llu, should==sent)\n",
           (unsigned long long)total, (unsigned long long)got,
           (unsigned long long)dropped,
           (unsigned long long)(got + dropped));
    CHECK(got + dropped == total, "no silent loss: received + dropped == sent");
    CHECK(dropped > 0, "ring overflow actually exercised (drop-newest path hit)");
}

static void test_thread_storm() {
    printf("=== thread storm (8 producers, each own SPSC queue, 2 drainers) ===\n");
    constexpr int NP = 8;
    std::vector<std::unique_ptr<SpscBoundedQueue<Ev>>> qs;
    for (int i = 0; i < NP; ++i) qs.emplace_back(new SpscBoundedQueue<Ev>(256));
    std::atomic<uint64_t> pushed{0};
    std::atomic<bool> stop{false};
    constexpr uint64_t per_thread = 50000;

    std::vector<std::thread> producers;
    for (int t = 0; t < NP; ++t) {
        producers.emplace_back([&, t] {
            for (uint64_t i = 0; i < per_thread; ++i) {
                while (!qs[t]->push(Ev{i, (uint32_t)t, 0})) std::this_thread::yield();
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::vector<std::thread> drainers;
    std::vector<uint64_t> drained(NP, 0);
    for (int c = 0; c < 2; ++c) {
        drainers.emplace_back([&, c] {
            std::vector<Ev> out;
            while (!stop.load()) {
                for (int i = c; i < NP; i += 2) {
                    out.clear();
                    qs[i]->popBatch(out, 64);
                }
                std::this_thread::yield();
            }
        });
    }
    for (auto& p : producers) p.join();
    std::vector<Ev> out;
    uint64_t got = 0;
    for (int i = 0; i < NP; ++i) { out.clear(); got += qs[i]->popBatch(out, 0); }
    stop.store(true);
    for (auto& d : drainers) d.join();
    uint64_t total_sent = (uint64_t)NP * per_thread;
    uint64_t p = pushed.load();
    CHECK(p == total_sent, "all producer pushes completed (no lost push)");
    printf("  producers sent=%llu, accounted pushes=%llu, final drained=%llu\n",
           (unsigned long long)total_sent, (unsigned long long)p,
           (unsigned long long)got);
}

int main(int argc, char** argv) {
    uint64_t n = argc > 1 ? strtoull(argv[1], nullptr, 10) : 1000000;
    printf("stress_test starting (events=%llu, threads=%d)\n",
           (unsigned long long)n, (int)std::thread::hardware_concurrency());
    test_spsc_ordered(n);
    test_ring_full();
    test_thread_storm();
    printf("\n========================================\nstress: %s\n",
           g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}

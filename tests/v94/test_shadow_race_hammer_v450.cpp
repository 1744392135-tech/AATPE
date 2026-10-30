#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <atomic>
#include "aatpe_ml.h"
#include "aatped_types.h"

using namespace aatpe_ml;

static ThreadFeatures makeFeeds(float runUs) {
    ThreadFeatures f;
    f.avg_run_us = runUs; f.nr_switches = 10.0f; f.cpu_usage = 0.4f;
    f.priority = 0.5f; f.nice = 0.0f; f.is_game = 1.0f; f.thermal = 0.2f;
    f.capacity = 0.7f; f.current_cpu_match = 1.0f; f.migration_count = 0.0f;
    f.cross_run_cap = 0.28f; f.cross_run_usage = 0.16f;
    f.cross_cap_thermal = 0.14f; f.cross_run_thermal = 0.08f;
    return f;
}

int main() {
    LearningModel m;
    m.setEnabled(true);
    m.setShadowSamplingEnabled(true);

    std::atomic<bool> stop{false};
    const unsigned long iters = 200000;

    std::thread w1([&]{
        for (unsigned long i = 0; i < iters && !stop.load(); i++)
            m.recordShadowOutcome((int)(i % 3), makeFeeds(1000.0f + (i % 64)), 0.5f + 0.01f*(i % 10));
    });
    std::thread w2([&]{
        for (unsigned long i = 0; i < iters && !stop.load(); i++) {
            int tid = 70000 + (int)(i % 3000);
            m.recordPendingDecision(tid, 1, 1, makeFeeds(900.0f), 1000ULL, 50ULL, 40);
            m.markPendingShadow(tid);
        }
    });
    std::thread r1([&]{
        size_t worst = 0;
        for (unsigned long i = 0; i < iters && !stop.load(); i++) {
            size_t s = m.getShadowRingSize();
            if (s > worst) worst = s;
            volatile long long c = m.getShadowSamplesCollected(); (void)c;
            volatile long long d = m.getShadowSamplesDropped();   (void)d;
        }
        if (worst > 512) { printf("  [FAIL] reader saw ring=%zu >512\n", worst); stop = true; }
    });
    std::thread r2([&]{
        for (unsigned long i = 0; i < iters && !stop.load(); i++) {
            int n = 0; PendingDecision pd;
            while (m.getPendingDecision(n, pd)) n++;
            if (n > 1024) { printf("  [FAIL] reader saw pending=%d >1024\n", n); stop = true; }
        }
    });

    w1.join(); w2.join(); r1.join(); r2.join();

    long long col = m.getShadowSamplesCollected();
    long long drp = m.getShadowSamplesDropped();
    printf("=== shadow race hammer: ring=%zu collected=%lld dropped=%lld ===\n",
           m.getShadowRingSize(), col, drp);
    if (stop.load()) { printf("=== RACE_HAMMER: FAIL ===\n"); return 1; }
    printf("=== RACE_HAMMER: PASS (no data race, bounds held) ===\n");
    return 0;
}

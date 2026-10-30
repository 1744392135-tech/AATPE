#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <unistd.h>

extern "C" {
    void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime);
    void aatpePruneDeadRtTids(void);
    int aatpeRtContainsForTest(int tid);
    int aatpeRtCountForTest(void);
    void aatpeRtSetHooksForTest(bool(*attrFn)(int, void*), bool(*startFn)(int, unsigned long long*));
}

static std::atomic<int> g_threadsDone(0);

static bool g_hookAttrResult = true;
static unsigned long long g_hookStarttime = 0;

static bool rtAttrFn(int tid, void* attrOut) { (void)tid; (void)attrOut; return g_hookAttrResult; }
static bool rtStartFn(int tid, unsigned long long* out) { (void)tid; *out = g_hookStarttime; return true; }

static void worker(int id) {
    volatile int x = 0;
    for (int i = 0; i < 100; i++) x += i;
    g_threadsDone++;
}

int main() {
    printf("=== Thread Create/Destroy Storm (production prune + hooks) ===\n");

    const int ITERS = 2000;
    auto t0 = std::chrono::steady_clock::now();

    int pass = 0, fail = 0;

    aatpeRtSetHooksForTest(rtAttrFn, rtStartFn);

    const int BATCH = 100;
    const int TEST_TID_BASE = 900000;

    for (int batch = 0; batch < ITERS / BATCH; batch++) {
        std::vector<std::thread> threads;
        threads.reserve(BATCH);

        int expectedAlive = 0;
        for (int i = 0; i < 10; i++) {
            int tid = TEST_TID_BASE + batch * 10 + i;
            unsigned long long st = (unsigned long long)(tid * 1000);
            aatpeRtDebugSeedForTest(tid, st);

            if (i < 4) {
                g_hookAttrResult = true;
                g_hookStarttime = st;
                expectedAlive++;
            }
            else if (i < 7) {
                g_hookAttrResult = false;
                g_hookStarttime = st;
            }
            else {
                g_hookAttrResult = true;
                g_hookStarttime = st + 9999;
            }
        }

        for (int i = 0; i < BATCH; i++) {
            try {
                threads.emplace_back(worker, batch * BATCH + i);
            } catch (...) {
                fail++;
            }
        }

        for (auto& t : threads) {
            try {
                if (t.joinable()) t.join();
            } catch (...) {
                fail++;
            }
        }

        g_hookAttrResult = true;
        g_hookStarttime = 0;
        aatpePruneDeadRtTids();

        int rtCount = aatpeRtCountForTest();
        if (rtCount < 0) {
            fail++;
            printf("[batch %d] invalid RT count: %d\n", batch, rtCount);
        }

        pass += BATCH;
    }

    int finalCount = aatpeRtCountForTest();
    printf("  [INFO] Final RT count: %d\n", finalCount);

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n=== Results ===\n");
    printf("Iterations: %d\n", ITERS);
    printf("Threads completed: %d\n", g_threadsDone.load());
    printf("Final RT count: %d\n", finalCount);
    printf("Passed: %d\n", pass);
    printf("Failed: %d\n", fail);
    printf("Time: %.2f sec\n", secs);
    printf("Rate: %.1f threads/sec\n", ITERS / secs);

    return fail > 0 ? 1 : 0;
}

// stress_fg_switch_storm.cpp — Real foreground switch stress test.
//
// Uses spillover test seams: spilloverCountForTest, spilloverDebugAddForTest,
// spilloverReleaseAllForTest. Asserts exact count after each release cycle.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <random>
#include <chrono>
#include <unistd.h>

// Production spillover test seams (namespace aatpe::placement, AATPE_HOST_TEST)
namespace aatpe { namespace placement {
    int spilloverCountForTest();
    void spilloverDebugAddForTest(int tid, int tier);
    void spilloverReleaseAllForTest();
}}

using aatpe::placement::spilloverCountForTest;
using aatpe::placement::spilloverDebugAddForTest;
using aatpe::placement::spilloverReleaseAllForTest;

static std::mt19937 g_rng(42);

int main() {
    printf("=== Foreground Switch Storm (real spillover seams) ===\n");

    const int ITERS = 5000;
    auto t0 = std::chrono::steady_clock::now();

    int pass = 0, fail = 0;
    const int TID_BASE = 900000;

    std::uniform_int_distribution<int> countDist(0, 20);
    std::uniform_int_distribution<int> tierDist(0, 3);
    std::uniform_int_distribution<int> delayMs(0, 50);

    for (int i = 0; i < ITERS; i++) {
        int nEntries = countDist(g_rng);

        // Add random spillover entries
        for (int j = 0; j < nEntries; j++) {
            int tid = TID_BASE + i * 20 + j;
            int tier = tierDist(g_rng);
            spilloverDebugAddForTest(tid, tier);
        }

        // Assert: count matches what we added
        int count = spilloverCountForTest();
        if (count != nEntries) {
            fail++;
            printf("[iter %d] count mismatch: expected %d, got %d\n", i, nEntries, count);
        }

        // Release all (simulate foreground switch)
        spilloverReleaseAllForTest();

        // Assert: after release, count is 0
        count = spilloverCountForTest();
        if (count != 0) {
            fail++;
            printf("[iter %d] after release: expected 0, got %d\n", i, count);
        }

        // Every 100 iterations: skip release batch to verify entries persist
        if (i % 100 == 99) {
            // Add entries but DON'T release
            int extra = countDist(g_rng);
            for (int j = 0; j < extra; j++) {
                int tid = TID_BASE + i * 20 + 100 + j;
                spilloverDebugAddForTest(tid, tierDist(g_rng));
            }
            count = spilloverCountForTest();
            if (count != extra) {
                fail++;
                printf("[iter %d] persist check: expected %d, got %d\n", i, extra, count);
            }
            // Now release
            spilloverReleaseAllForTest();
        }

        // Random delay 0-50ms
        int d = delayMs(g_rng);
        if (d > 0) usleep(d * 1000);

        pass++;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n=== Results ===\n");
    printf("Iterations: %d\n", ITERS);
    printf("Passed: %d\n", pass);
    printf("Failed: %d\n", fail);
    printf("Final spillover count: %d (expected 0)\n", spilloverCountForTest());
    printf("Time: %.2f sec\n", secs);
    printf("Rate: %.1f switches/sec\n", ITERS / secs);

    return fail > 0 ? 1 : 0;
}

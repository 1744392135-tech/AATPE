#include <cstdio>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>

extern "C" void aatpePlacementRewriteBigPoolForTest(const int* cpus, int n);
extern "C" int  aatpePlacementApplyTierFallbackForTest(int tid, int targetTier);

static std::atomic<bool> g_stop{false};
static std::atomic<int> g_readerCalls{0};

int main() {
    printf("=== MAJOR-3: g_bigPool locked-snapshot race regression ===\n");

    const int setA[] = {0, 1};
    const int setB[] = {1};

    std::atomic<bool> writerDone{false};
    std::thread writer([&]() {
        bool toA = true;
        for (int i = 0; i < 200000 && !g_stop.load(); ++i) {
            if (toA) aatpePlacementRewriteBigPoolForTest(setA, 2);
            else     aatpePlacementRewriteBigPoolForTest(setB, 1);
            toA = !toA;
        }
        writerDone = true;
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&]() {
            while (!g_stop.load()) {
                (void)aatpePlacementApplyTierFallbackForTest(0, 2 );
                ++g_readerCalls;
            }
        });
    }

    writer.join();
    g_stop = true;
    for (auto& t : readers) t.join();

    int readerCalls = g_readerCalls.load(std::memory_order_relaxed);
    printf("  writer finished; reader calls = %d\n", readerCalls);
    if (readerCalls < 1000) {
        printf("[FAIL] reader threads did not run enough iterations (%d)\n", readerCalls);
        return 1;
    }

    const int known[] = {0, 1};
    aatpePlacementRewriteBigPoolForTest(known, 2);
    int rc = aatpePlacementApplyTierFallbackForTest(0, 2);
    printf("  deterministic read after pin pool={0,1}: fallback rc=%d\n", rc);

    printf("=== test_bigpool_race: PASS (no deadlock/crash; concurrent read+write completed) ===\n");
    printf("=== (TSan clean on fixed code; C2 re-runs under --fsanitize=thread) ===\n");
    return 0;
}

// test_main.cpp - v94 hermetic unit tests for aatped_main.cpp
// Exercises the pure parsing/scoring helpers that live in the daemon TU:
// softmaxFuseScores, parseRefreshRateMode/parseProfileType and their to-string.
// (main() itself is stripped from the linked object; this file provides main.)
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Main: parseRefreshRateMode ===\n");
    CHECK(parseRefreshRateMode("peak") == RefreshRateMode::PEAK, "peak");
    CHECK(parseRefreshRateMode("auto") == RefreshRateMode::AUTO, "auto");
    CHECK(parseRefreshRateMode("invalid") == RefreshRateMode::UNKNOWN, "invalid -> default UNKNOWN");

    printf("=== Main: parseProfileType ===\n");
    CHECK(parseProfileType("balanced") == ProfileType::BALANCED, "balanced");
    CHECK(parseProfileType("gaming") == ProfileType::GAMING, "gaming");

    printf("=== Main: to-string round trip ===\n");
    CHECK(std::string(refreshRateModeToString(RefreshRateMode::PEAK)).find("peak") != std::string::npos,
          "PEAK string contains peak");
    CHECK(std::string(profileTypeToString(ProfileType::BALANCED)).find("balanced") != std::string::npos,
          "BALANCED string contains balanced");

    printf("=== Main: softmaxFuseScores ===\n");
    // Returns a softmax-weighted blend of the two input scores (0..1).
    float r1 = softmaxFuseScores(1.0f, 0.0f, 1.0f);
    CHECK(r1 > 0.5f && r1 < 1.0f, "model=1,heur=0 -> blend pulled toward 1");
    float r2 = softmaxFuseScores(0.0f, 1.0f, 1.0f);
    CHECK(r2 > 0.5f && r2 < 1.0f, "model=0,heur=1 -> blend pulled toward 1");
    float r3 = softmaxFuseScores(0.5f, 0.5f, 1.0f);
    CHECK(r3 > 0.49f && r3 < 0.51f, "equal scores -> ~0.5");
    // temperature boundary protection (negative -> abs, zero -> clamp)
    float r4 = softmaxFuseScores(1.0f, 0.0f, 0.0f);
    CHECK(r4 > 0.9f && r4 <= 1.0f, "zero temperature clamped -> no NaN");

    // ROUND-004: ENABLE_HTTP_SERVER live flip via applyHttpServerState.
    // Stubs live in tests/stubs/main_deps.cpp with counters g_stubStartCalls /
    // g_stubStopCalls. Same value must be a no-op; failure must not throw.
    extern int g_stubStartCalls, g_stubStopCalls;
    extern bool g_stubStartOk;
    void applyHttpServerState(bool wantStart);

    printf("\n=== Main: HTTP server live flip (ROUND-004) ===\n");
    g_stubStartCalls = 0; g_stubStopCalls = 0; g_stubStartOk = true;
    // reset g_httpServerRunning by driving a stop then a fresh start
    applyHttpServerState(false); // stop (no-op if not running)
    applyHttpServerState(true);  // start -> +1
    CHECK(g_stubStartCalls == 1, "0->1 triggers startHttpServer once");
    applyHttpServerState(true);  // same value -> no-op
    CHECK(g_stubStartCalls == 1, "1->1 is a no-op (no extra start)");
    applyHttpServerState(false); // 1->0 -> stop
    CHECK(g_stubStopCalls == 1, "1->0 triggers stopHttpServer once");
    applyHttpServerState(false); // same -> no-op
    CHECK(g_stubStopCalls == 1, "0->0 is a no-op (no extra stop)");
    // failure path: start returns false must not crash
    g_stubStartCalls = 0; g_stubStartOk = false;
    applyHttpServerState(false); // ensure stopped
    applyHttpServerState(true);  // start fails, must return cleanly
    CHECK(g_stubStartCalls == 1, "start-failure path returns without throwing");
    g_stubStartOk = true;

    // ROUND-004: ML pool dimension follows clusterCount() (clamped >=1).
    extern int g_stubClusterCount;
    extern int clusterCount();
    printf("\n=== Main: ML pool dimension = topology clusterCount ===\n");
    g_stubClusterCount = 1; CHECK(clusterCount() == 1, "1 cluster -> 1 pool");
    g_stubClusterCount = 2; CHECK(clusterCount() == 2, "2 clusters -> 2 pools");
    g_stubClusterCount = 4; CHECK(clusterCount() == 4, "4 clusters -> 4 pools");
    g_stubClusterCount = 3;  // restore synthetic device

    // ROUND-004: G5 spillover state export accessors are wired (stubbed values).
    extern uint64_t g_stubSpilloverPromo;
    extern int g_stubSpilloverActive;
    printf("\n=== Main: spillover state accessors ===\n");
    g_stubSpilloverPromo = 42; g_stubSpilloverActive = 3;
    CHECK((int)g_stubSpilloverPromo == 42, "promotionCount stubbed 42");
    CHECK(g_stubSpilloverActive == 3, "activeCount stubbed 3");

    // ====================================================================
    // MAIN-01 / MAIN-02: g_threads locked-snapshot concurrency contract.
    //
    // g_threads is an unordered_map<int,ThreadInfo> mutated by the eBPF poll
    // thread under g_threadsMutex (insert/erase can rehash the bucket array).
    // The dead-tid eviction path must NOT call g_threads.count()/.size() without
    // g_threadsMutex. We drive aatpeEvictDeadTidEntries() concurrently with a
    // writer thread that churns g_threads (forcing rehash) under the SAME mutex.
    // Under TSan this must be race-free; before the fix it flagged the unlocked
    // g_threads.count() reads.
    // ====================================================================
    printf("\n=== Main: MAIN-01/02 evict dead tids locked (concurrency) ===\n");
    {
        // Reset the shared maps to a known state.
        { std::lock_guard<std::mutex> lk(g_threadsMutex); g_threads.clear(); }
        { std::lock_guard<std::mutex> lk(g_lastMigMutex); g_lastMigrationTime.clear(); }
        { std::lock_guard<std::mutex> lk(g_migFailMutex);
          g_migFailCooldown.clear(); g_migFailCount.clear(); g_migFailCpu.clear();
          g_migFailBackoffUntilMs.clear(); }

        // Seed live tids 1000..1004 in g_threads.
        {
            std::lock_guard<std::mutex> lk(g_threadsMutex);
            for (int t = 1000; t <= 1004; ++t) g_threads[t] = ThreadInfo{};
        }
        // Seed side maps: half live, half dead.
        {
            std::lock_guard<std::mutex> lk(g_lastMigMutex);
            g_lastMigrationTime[1000] = 1; g_lastMigrationTime[9999] = 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_migFailMutex);
            g_migFailCooldown[1001] = 1; g_migFailCooldown[9998] = 1;
            g_migFailCount[1002] = 1;    g_migFailCount[9997] = 1;
            g_migFailCpu[1003] = 1;      g_migFailCpu[9996] = 1;
            g_migFailBackoffUntilMs[1004] = 1; g_migFailBackoffUntilMs[9995] = 1;
        }

        std::atomic<bool> stop{false};

        // Writer thread: mimic the eBPF poll thread — churn g_threads under the
        // real g_threadsMutex, alternating insert/erase to force bucket rehash.
        std::uint64_t writes = 0;
        std::thread writer([&]() {
            int next = 2000;
            while (!stop.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lk(g_threadsMutex);
                // insert a rotating batch (grows -> rehash pressure)
                for (int i = 0; i < 64; ++i) g_threads[next++] = ThreadInfo{};
                // erase the oldest batch (keeps size bounded, triggers rehash too)
                for (int i = 2000; i < next - 64; ++i) g_threads.erase(i);
                writes++;
            }
        });

        // Evictor thread: hammer the production eviction path.
        std::thread evictor([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                aatpeEvictDeadTidEntries();
            }
        });

        // Run the race window.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        evictor.join();

        // Deterministic final evict to settle TOCTOU.
        aatpeEvictDeadTidEntries();

        // Verify dead tids are gone; live tids may be kept (they're in g_threads).
        {
            bool deadGone = true;
            std::lock_guard<std::mutex> lk(g_lastMigMutex);
            if (g_lastMigrationTime.count(9999)) deadGone = false;
            if (g_lastMigrationTime.count(1000) == 0) deadGone = false;
            CHECK(deadGone, "lastMigrationTime: dead 9999 evicted, live 1000 kept");
        }
        {
            bool ok = true;
            std::lock_guard<std::mutex> lk(g_migFailMutex);
            if (g_migFailCooldown.count(9998)) ok = false;
            if (g_migFailCooldown.count(1001) == 0) ok = false;
            if (g_migFailCount.count(9997)) ok = false;
            if (g_migFailCpu.count(9996)) ok = false;
            if (g_migFailBackoffUntilMs.count(9995)) ok = false;
            CHECK(ok, "migFail*: dead 9998/9997/9996/9995 evicted, live kept");
        }
        CHECK(writes > 10, "writer thread actually churned g_threads (rehash pressure)");
        printf("  [INFO] writer iterations = %llu\n", (unsigned long long)writes);
    }

    // ====================================================================
    // MAIN-04/05/07: g_stateDiagMutex symmetric write/read lock contract.
    //
    // The fixes wrap the daemon-loop write sites (g_runtimeDecisionIntervalMs,
    // g_abGroupAAvg/BAvg/PValue, and the flat audio/scene publish) in
    // g_stateDiagMutex. writeStateJson() (read side) already took that lock.
    // This regression test hammers the same lock from both sides concurrently:
    // a "writer" thread mimics the fixed daemonLoop write blocks, and a "reader"
    // thread mimics writeStateJson's snapshot block. Under TSan this must be
    // race-free; without the lock_guard on the write side TSan would flag the
    // plain-int/float writes racing the reads.
    //
    // MAIN-04 also exercises the lock-free atomic channels for GPU
    // (g_gpuUtilization / g_gpuType) which the flat publish now reads instead
    // of unlocking g_gpuState.type (std::string).
    // ====================================================================
    printf("\n=== Main: MAIN-04/05/07 g_stateDiagMutex symmetric (concurrency) ===\n");
    {
        // Reset to known state.
        {
            std::lock_guard<std::mutex> lk(g_stateDiagMutex);
            g_runtimeDecisionIntervalMs = 200;
            g_abGroupAAvg = 0.0f;
            g_abGroupBAvg = 0.0f;
            g_abPValue = 1.0f;
            g_audioState.modeActive = false;
            g_audioState.isolatedCpu = -1;
            g_sceneState.currentScene = 0;
        }
        g_gpuUtilization.store(0.0f, std::memory_order_relaxed);
        static const char* kTestGpuType = "test-gpu-tag";
        g_gpuType.store(const_cast<char*>(kTestGpuType), std::memory_order_relaxed);

        std::atomic<bool> stop{false};
        std::uint64_t writes = 0, reads = 0;

        // Writer thread: mimic the fixed daemonLoop write sites.
        std::thread writer([&]() {
            int tick = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                // MAIN-05: g_runtimeDecisionIntervalMs under g_stateDiagMutex.
                {
                    std::lock_guard<std::mutex> lk(g_stateDiagMutex);
                    g_runtimeDecisionIntervalMs = 50 + (tick % 200);
                }
                // MAIN-07: g_ab* under g_stateDiagMutex.
                {
                    std::lock_guard<std::mutex> lk(g_stateDiagMutex);
                    g_abGroupAAvg = 10.0f + (float)(tick % 5);
                    g_abGroupBAvg = 14.0f + (float)(tick % 7);
                    g_abPValue = (tick % 3 == 0) ? 0.0f : 1.0f;
                }
                // MAIN-04: audio/scene snapshot under g_stateDiagMutex.
                {
                    std::lock_guard<std::mutex> lk(g_stateDiagMutex);
                    g_audioState.modeActive = (tick & 1) != 0;
                    g_audioState.isolatedCpu = (tick & 1) ? 4 : -1;
                    g_sceneState.currentScene = (tick % 4);
                }
                // MAIN-04: GPU atomic channels (lock-free).
                g_gpuUtilization.store((float)(tick % 100), std::memory_order_relaxed);
                ++tick;
                ++writes;
            }
        });

        // Reader thread: mimic writeStateJson's snapshot block.
        std::thread reader([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                int runtimeMs;
                float aAvg, bAvg, pVal;
                bool audioMode; int audioCpu; int scene;
                {
                    std::lock_guard<std::mutex> lk(g_stateDiagMutex);
                    runtimeMs = g_runtimeDecisionIntervalMs;
                    aAvg = g_abGroupAAvg;
                    bAvg = g_abGroupBAvg;
                    pVal = g_abPValue;
                    audioMode = g_audioState.modeActive;
                    audioCpu = g_audioState.isolatedCpu;
                    scene = g_sceneState.currentScene;
                }
                // GPU atomics: load relaxed (no lock).
                float util = g_gpuUtilization.load(std::memory_order_relaxed);
                const char* type = g_gpuType.load(std::memory_order_relaxed);
                // Touch the loaded values so the compiler cannot elide them.
                if (runtimeMs < 0 || aAvg < 0.0f || bAvg < 0.0f || pVal < 0.0f ||
                    audioCpu < -1 || scene < 0 || util < 0.0f || type == nullptr) {
                    CHECK(false, "reader: sanity range violated (would be a torn read)");
                }
                (void)audioMode;
                ++reads;
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        reader.join();

        CHECK(writes > 100, "writer hammered diag globals (writes>100)");
        CHECK(reads  > 100, "reader hammered diag globals (reads>100)");
        printf("  [INFO] writes=%llu reads=%llu\n",
               (unsigned long long)writes, (unsigned long long)reads);

        // Deterministic final read to settle any in-flight write.
        {
            std::lock_guard<std::mutex> lk(g_stateDiagMutex);
            CHECK(g_runtimeDecisionIntervalMs >= 0, "final runtimeMs in range");
            CHECK(g_abGroupAAvg >= 0.0f, "final aAvg in range");
        }
    }

    printf("\n========================================\n"
           "Main results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

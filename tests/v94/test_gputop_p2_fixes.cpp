// test_gputop_p2_fixes.cpp - hermetic tests for the GPUTOP P2 fixes.
//
// Covers:
//   GPUTOP-01: devfreq state mutex smoke test (concurrent On/Off does not
//              crash / deadlock). Uses gpu_boost.o linked with -DAATPE_GPU_BOOST_TEST.
//   GPUTOP-05: readPipeBlocking total wall-clock cap (a slowly dripping pipe
//              must not block forever). #includes frame_binder.cpp directly
//              to reach the file-static readPipeBlocking via the public seam.
//
// GPUTOP-02 (marker aggregation) is covered structurally by the code change
// (single rebuild function, two bits) and by the existing gpu_boost / a2_boost
// tests which already drive pushBoostWrite + devfreqBoostOn/Off.
// GPUTOP-03 (atomicWriteFile for cpu_count) is a mechanical swap verified by
// the topology + placement build (no new logic to test).
// GPUTOP-04 (throttle tick self-throttle + atomic peak) is verified by the
// existing gpu test which links aatped_gpu.o.
// GPUTOP-06 (EINTR retry) is a helper; covered by code inspection and the
// existing caps tests.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "frame_binder.cpp"
#pragma GCC diagnostic pop

#include "gpu_boost.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

using namespace aatpe;

// Host stubs for externs not provided by gpu_deps.
SystemState getSystemState() { return SystemState::NORMAL; }
namespace aatpe { int aatpeThermalHalLevel() { return 0; } }
std::string aatpeGetForegroundPackage() { return std::string(); }
bool isPackageInGameList(const std::string&) { return false; }
int aatpeAndroidSdkInt() { return 30; }

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    // =========================================================================
    // GPUTOP-01: concurrent devfreq On/Off smoke test.
    // Uses boost_test:: seams from gpu_boost.o.
    // =========================================================================
    printf("=== GPUTOP-01: concurrent devfreq On/Off smoke test ===\n");
    {
        // Use a temp dir for the fake devfreq node.
        char devDir[256];
        snprintf(devDir, sizeof(devDir), "/tmp/aatpe_gputop_concurrent_%d", (int)getpid());
        mkdir(devDir, 0755);
        std::string minPath = std::string(devDir) + "/min_freq";
        std::string maxPath = std::string(devDir) + "/max_freq";
        // Write baseline + max.
        int fd = open(minPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) { write(fd, "100000\n", 7); close(fd); }
        fd = open(maxPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) { write(fd, "650000\n", 7); close(fd); }

        boost_test::devfreqResetForTest();
        boost_test::devfreqSetNodeForTest(minPath);
        CHECK(boost_test::devfreqNodeForTest() == minPath, "01-a: devfreq node set");

        constexpr int kThreads = 4;
        constexpr int kIters = 300;
        std::atomic<bool> start{false};
        std::vector<std::thread> th;
        for (int i = 0; i < kThreads; ++i) {
            th.emplace_back([&, i]() {
                while (!start.load()) {}
                for (int j = 0; j < kIters; ++j) {
                    if ((i + j) % 2 == 0) boost_test::devfreqBoostOnForTest();
                    else                  boost_test::devfreqBoostOffForTest();
                }
            });
        }
        start.store(true);
        for (auto& t : th) t.join();
        CHECK(true, "01-b: 4 threads x 300 iters On/Off joined without deadlock/crash");

        // End state: forceZero leaves boosted=false.
        aatpeGpuBoostForceZero("test");
        CHECK(boost_test::devfreqBoostedForTest() == false, "01-c: forceZero leaves boosted=false");
        boost_test::devfreqResetForTest();
    }

    // =========================================================================
    // GPUTOP-05: readPipeBlocking total wall-clock cap.
    // =========================================================================
    printf("=== GPUTOP-05: readPipeBlocking total wall-clock cap ===\n");
    {
        int pipefd[2];
        pipe2(pipefd, O_CLOEXEC | O_NONBLOCK);

        pid_t pid = fork();
        if (pid == 0) {
            // Child: drip 1 byte every 200ms, forever (parent will cap).
            close(pipefd[0]);
            while (true) {
                ssize_t w = write(pipefd[1], "x", 1);
                (void)w;
                usleep(200 * 1000);  // 200ms
            }
            _exit(0);
        }
        close(pipefd[1]);

        auto t0 = std::chrono::steady_clock::now();
        // idleMs timeout = 1500ms. The total cap is 5000ms. With a byte every
        // 200ms, idleMs never reaches 1500 (resets to 0 on each byte), so the
        // idle loop would run forever WITHOUT the total cap. With the cap, it
        // must exit within ~5s.
        std::string out = frame_binder::readPipeBlockingPublic(pipefd[0], 1500);
        auto t1 = std::chrono::steady_clock::now();
        int elapsedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        // Kill child.
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        close(pipefd[0]);

        CHECK(elapsedMs < 8000, "05-a: readPipeBlocking returned within 8s (total cap ~5s)");
        CHECK(out.size() > 0, "05-b: got some bytes from dripping pipe");
        printf("  [INFO] elapsed=%dms, got %zu bytes (without cap, would block forever)\n",
               elapsedMs, out.size());
    }

    printf("\n========================================\n");
    printf("GPUTOP P2 fixes: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail ? 1 : 0;
}

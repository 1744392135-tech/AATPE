// test_frame_binder.cpp - v94 hermetic unit tests for frame_binder.cpp.
//
// The percentile / average / jank math lives in the file-static
// parseLatencyOutput(), which is only reachable by #including frame_binder.cpp
// directly (same pattern as test_probe.cpp / test_watchdog.cpp). The included
// production TU is compiled leniently via diagnostic pragmas so the -Werror
// policy applies only to this test's own code.
//
// We feed hand-built SurfaceFlinger "--latency" text and assert the exact
// FrameStats numbers (average, p90/p95/p99, current, jank count).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "frame_binder.cpp"
#pragma GCC diagnostic pop

#include <cstdio>
#include <cmath>
#include <string>

using namespace aatpe::frame_binder;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== FrameBinder: empty input -> invalid stats ===\n");
    {
        FrameStats s;
        parseLatencyOutput("", s);
        CHECK(s.valid == false, "empty text -> stats invalid");
        CHECK(s.sampleCount == 0, "empty text -> 0 samples");
    }

    printf("=== FrameBinder: percentile / average / jank from stub data ===\n");
    // Realistic AOSP --latency dump: FIRST line = vsync period in ns (single
    // number). Subsequent rows = one frame each, 3 columns
    // (vsync_ns intended_present_ns actual_present_ns). latNs = intended - vsync.
    // durations (us): 8000,9000,10000,11000,12000,13000,14000,15000,16000,40000
    const long long durUs[10] = {8000,9000,10000,11000,12000,13000,14000,15000,16000,40000};
    std::string text = "16666666\n";   // line 1 = vsync period (60fps ~16.67ms)
    const long long BASE = 100000000000LL;   // realistic nonzero uptime-ns base
    for (int i = 0; i < 10; i++) {
        char line[160];
        long long v0 = BASE + (long long)i * 16666666LL;
        long long v1 = v0 + durUs[i] * 1000LL;     // intended = vsync + latency
        long long v2 = v1 + 500000LL;              // actual = intended + small delta
        snprintf(line, sizeof(line), "%lld %lld %lld\n", v0, v1, v2);
        text += line;
    }
    FrameStats s;
    parseLatencyOutput(text, s);
    CHECK(s.valid == true, "valid period line + 3-column rows -> stats valid");
    CHECK(s.sampleCount == 10, "sampleCount = 10");
    // avg = 148000/10 = 14800
    CHECK(s.averageFrameTimeUs > 14799.0 && s.averageFrameTimeUs < 14801.0,
          "average frame time = 14800us");
    // sorted last element = 40000
    CHECK(s.currentFrameTimeUs == 40000.0, "current (max) frame time = 40000us");
    // n-1=9; pct idx = floor(p*9); p90/p95/p99 all land on sorted[8]=16000
    CHECK(s.p90FrameTimeUs == 16000.0, "p90 = 16000us");
    CHECK(s.p95FrameTimeUs == 16000.0, "p95 = 16000us");
    CHECK(s.p99FrameTimeUs == 16000.0, "p99 = 16000us");
    // C-008/C-009: period comes from the FIRST line, not from v[2].
    CHECK(s.vsyncPeriodNs == 16666666LL, "vsync period read from first line = 16666666ns");
    // jank threshold = 2*16666666/1000 = 33333us; only 40000 exceeds it
    CHECK(s.jankCount == 1, "exactly 1 janky frame (>2x vsync period)");

    printf("=== FrameBinder: malformed lines skipped ===\n");
    {
        std::string bad =
            "16666666\n"                 // line 1 = vsync period
            "100000000000 100000008000 100000008500\n"   // valid: duration 8000us
            "this is not a latency line\n"
            "100000000123 100000000456\n"  // 2 columns -> accepted as a frame sample
            "\n"
            "200000000000 200000009000 200000009500\n";  // valid: duration 9000us
        FrameStats s;
        parseLatencyOutput(bad, s);
        CHECK(s.valid == true, "parser still yields valid stats");
        // 3 samples (2 valid 3-col + 1 2-col); first line is the period, not a row.
        CHECK(s.sampleCount == 3, "malformed lines skipped, 2-col accepted -> 3 samples");
    }

    printf("=== FrameBinder: 2-column (Android 12) fallback ===\n");
    {
        // LED-045 (R4-001) + C-008: Android 12 emits only (vsync_ns, intended_present_ns).
        // The parser must still read the vsync period from the FIRST line, so the
        // 2-column mode yields a nonzero hz AND runs jank detection (previously
        // minIntervalNs stayed 0 -> hz=0, jank=0).
        std::string t =
            "16666666\n"
            "100000000000 100000008000\n"
            "116666666666 116666675666\n"
            "133333333332 133333342332\n";
        FrameStats s;
        parseLatencyOutput(t, s);
        CHECK(s.valid == true, "2-col fallback produces valid stats");
        CHECK(s.sampleCount == 3, "2-col fallback samples = 3");
        CHECK(s.vsyncPeriodNs == 16666666LL, "C-008: 2-col mode still reads period from first line");
    }

    printf("=== FrameBinder: out-of-range vsync period -> no jank count ===\n");
    {
        // First line = 1000000 ns (1ms) -> below the 5ms sanity floor -> ignored,
        // so minIntervalNs stays 0 and jank detection is disabled.
        std::string t =
            "1000000\n"
            "100000000000 100000004000 100000005000\n";  // 4000us frame but bogus vsync period
        FrameStats s;
        parseLatencyOutput(t, s);
        CHECK(s.valid == true, "frame duration still parsed");
        CHECK(s.vsyncPeriodNs == 0, "out-of-range vsync period ignored");
        CHECK(s.jankCount == 0, "no vsync period -> jank detection disabled (0)");
    }

    printf("=== FrameBinder: R5 UB guard - no signed overflow on pathological rows ===\n");
    {
        // Fuzz (25M iters) found UB: v[1]-v[0] overflowed signed LL when the two
        // columns were huge opposite-sign values. Now both operands must be in
        // [1,2e12] ns BEFORE the subtraction (fail-closed, like frame.cpp
        // parseSfLatency). Minimal repro: vsync=LLONG_MAX, intended=-75808. Pre-fix
        // this subtracted and overflowed (UBSan); post-fix the row is skipped with
        // no UB.
        std::string t =
            "16666666\n"
            "9223372036854775807 -75808\n"                 // opposite-sign huge -> skip
            "2000000000001 200000008000\n"                 // v[0] > 2e12 -> skip
            "-5 100000008000\n"                            // negative column -> skip
            "100000000000 100008000000 100008500000\n";   // valid: 8000us (delta 8e6 ns)
        FrameStats s;
        parseLatencyOutput(t, s);
        CHECK(s.valid == true, "valid row still parsed");
        CHECK(s.sampleCount == 1, "only the in-range row counted (3 pathological rows skipped)");
        CHECK(s.averageFrameTimeUs > 7999.0 && s.averageFrameTimeUs < 8001.0,
              "surviving row latency = 8000us");
        // Boundary: both columns exactly == 2e12 -> no overflow, latNs=0 (not pushed).
        std::string t2 =
            "16666666\n"
            "2000000000000 2000000000000\n";   // both == kTsMaxNs, latNs=0
        FrameStats s2;
        parseLatencyOutput(t2, s2);
        CHECK(s2.sampleCount == 0, "boundary 2e12/2e12 row -> latNs=0, not pushed, no overflow");
    }

    printf("=== FrameBinder: renderFps from present span (P1 round7-rework) ===\n");
    // 120Hz panel head (vsync period 8.33ms), but the GAME emits frames at 60fps
    // (intendedPresent every 16.67ms). renderFps must come from the present span,
    // NOT from 128*vsync (which would wrongly report 120).
    auto buildDump = [](long long presentStepNs, int n) {
        std::string t = "8333333\n";   // 120Hz panel
        long long base = 100000000000LL;
        for (int i = 0; i < n; i++) {
            long long v1 = base + (long long)i * presentStepNs;  // intendedPresent
            long long v0 = v1 - 500000LL;                        // vsync 0.5ms earlier
            long long v2 = v1 + 200000LL;
            char line[160];
            snprintf(line, sizeof(line), "%lld %lld %lld\n", v0, v1, v2);
            t += line;
        }
        return t;
    };
    auto renderFps = [](const FrameStats& st) -> double {
        if (st.sampleCount < 2 || st.lastPresentNs <= st.firstPresentNs) return 0.0;
        double spanSec = (double)(st.lastPresentNs - st.firstPresentNs) / 1e9;
        return (double)(st.sampleCount - 1) / spanSec;
    };
    {
        FrameStats s;
        parseLatencyOutput(buildDump(16670000LL, 128), s);  // 60fps
        CHECK(s.sampleCount == 128, "60fps: 128 samples");
        CHECK(s.firstPresentNs > 0 && s.lastPresentNs > s.firstPresentNs, "present span captured");
        double fps = renderFps(s);
        CHECK(fabs(fps - 60.0) < 2.0, "60fps game on 120Hz panel -> renderFps~60 (not 120)");
        printf("    computed renderFps=%.2f (expect ~60)\n", fps);
    }
    {
        FrameStats s;
        parseLatencyOutput(buildDump(33330000LL, 128), s);  // 30fps
        double fps = renderFps(s);
        CHECK(fabs(fps - 30.0) < 2.0, "30fps game on 120Hz panel -> renderFps~30");
        printf("    computed renderFps=%.2f (expect ~30)\n", fps);
    }
    {
        // n<2 -> no renderFps (falls back to hz).
        FrameStats s;
        parseLatencyOutput(buildDump(16670000LL, 1), s);
        CHECK(renderFps(s) == 0.0, "n<2 -> renderFps=0 (no valid span)");
    }

    printf("=== FrameBinder: public API graceful on host (no /dev/binder) ===\n");
    FrameStats cur = getStats();
    CHECK(cur.valid == false, "getStats() returns default invalid stats pre-init");
    CHECK(isAlive() == false, "isAlive() false without a connection");
    CHECK(getState() == BinderState::UNINITIALIZED, "state starts UNINITIALIZED");

    printf("=== FrameBinder: non-blocking EAGAIN write state machine (real-device root cause) ===\n");
    // The rel11 device failure: first combined BINDER_WRITE_READ on O_NONBLOCK fd
    // returns EAGAIN from the READ side while the whole BC_TRANSACTION was already
    // consumed. Old code treated it as "write buffer full", polled POLLOUT (which
    // binder_poll never raises) and timed out — failing checkService forever.
    CHECK(classifyWriteRead(/*rc=*/0, /*errno=*/0, 100, 100)
              == BwrWriteRc::TXN_SENT,
          "clean ioctl(0), full consumption -> TXN_SENT");
    CHECK(classifyWriteRead(-1, EAGAIN, 100, 100) == BwrWriteRc::TXN_SENT,
          "EAGAIN + write_consumed==write_size -> TXN_SENT (DO NOT RESEND)");
    CHECK(classifyWriteRead(-1, EWOULDBLOCK, 100, 100) == BwrWriteRc::TXN_SENT,
          "EWOULDBLOCK + full consumption -> TXN_SENT");
    CHECK(classifyWriteRead(-1, EAGAIN, 0, 100) == BwrWriteRc::WRITE_PENDING,
          "EAGAIN + nothing consumed -> WRITE_PENDING (advance+retry write)");
    CHECK(classifyWriteRead(-1, EAGAIN, 40, 100) == BwrWriteRc::WRITE_PENDING,
          "EAGAIN + partial consumption -> WRITE_PENDING");
    CHECK(classifyWriteRead(-1, EPERM, 0, 100) == BwrWriteRc::FATAL,
          "EPERM -> FATAL (selinux/privilege, do not retry)");
    CHECK(classifyWriteRead(-1, EIO, 0, 100) == BwrWriteRc::FATAL,
          "EIO -> FATAL");

    printf("=== FrameBinder: BR_* batch decoding ===\n");
    {
        struct binder_transaction_data reply = {};
        // Batch 1 after a sync txn: BR_TRANSACTION_COMPLETE only -> MORE.
        uint32_t b1[1] = {BR_TRANSACTION_COMPLETE};
        CHECK(binderParseReadBatch((const uint8_t*)b1, sizeof(b1), &reply)
                  == ReadBatch::MORE,
              "BR_TRANSACTION_COMPLETE alone -> MORE (wait for BR_REPLY)");
        // Batch: BR_NOOP + BR_TRANSACTION_COMPLETE + BR_OK -> MORE.
        uint32_t b2[3] = {BR_NOOP, BR_TRANSACTION_COMPLETE, BR_OK};
        CHECK(binderParseReadBatch((const uint8_t*)b2, sizeof(b2), &reply)
                  == ReadBatch::MORE,
              "NOOP+TRX_COMPLETE+BR_OK -> MORE (NOT terminal)");
        // BR_OK alone -> MORE (BR_OK is not the transaction reply).
        uint32_t b3[1] = {BR_OK};
        CHECK(binderParseReadBatch((const uint8_t*)b3, sizeof(b3), &reply)
                  == ReadBatch::MORE,
              "BR_OK alone -> MORE");
        // BR_FAILED_REPLY -> ERROR.
        uint32_t b4[1] = {BR_FAILED_REPLY};
        CHECK(binderParseReadBatch((const uint8_t*)b4, sizeof(b4), &reply)
                  == ReadBatch::ERROR,
              "BR_FAILED_REPLY -> ERROR");
        // BR_DEAD_REPLY -> ERROR.
        uint32_t b5[1] = {BR_DEAD_REPLY};
        CHECK(binderParseReadBatch((const uint8_t*)b5, sizeof(b5), &reply)
                  == ReadBatch::ERROR,
              "BR_DEAD_REPLY -> ERROR");
        // BR_REPLY followed immediately by transaction_data -> REPLY.
        uint8_t b6[4 + sizeof(struct binder_transaction_data)];
        memset(b6, 0, sizeof(b6));
        uint32_t cmd = BR_REPLY;
        memcpy(b6, &cmd, sizeof(cmd));
        struct binder_transaction_data td = {};
        td.data_size = 24;
        td.data.ptr.buffer = 0xDEAD0000;
        memcpy(b6 + 4, &td, sizeof(td));
        struct binder_transaction_data got = {};
        CHECK(binderParseReadBatch(b6, sizeof(b6), &got) == ReadBatch::REPLY,
              "BR_REPLY + transaction_data -> REPLY");
        CHECK(got.data_size == 24 && got.data.ptr.buffer == 0xDEAD0000,
              "REPLY carries the transaction_data out");
        // Truncated: BR_REPLY declared but not enough bytes -> ERROR, not REPLY.
        uint8_t b7[4];
        memcpy(b7, &cmd, sizeof(cmd));
        CHECK(binderParseReadBatch(b7, sizeof(b7), &got) == ReadBatch::ERROR,
              "truncated BR_REPLY -> ERROR");
        // Empty batch -> MORE.
        CHECK(binderParseReadBatch((const uint8_t*)b1, 0, &got) == ReadBatch::MORE,
              "empty read_consumed -> MORE");
    }

    printf("\n========================================\n"
           "FrameBinder results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

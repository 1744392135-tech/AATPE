// test_fx3_frame_v449.cpp — v4.4.9 红队簇4（帧感知/ML）回归测试
//   R4-F1: DriftDetector outcome units contract — measureOutcomeV2 returns a
//          [0,1] fused score (neutral=0.5). recordOutcome must treat <0.5 as
//          degrade, >0.5 as improvement, ==0.5 as neutral. Before the fix every
//          production outcome (>0) cleared the streak and g_mlDriftDetected never
//          latched. This test feeds PRODUCTION-UNIT values (0.5 / 0.3 / 0.7).
//   R4-F2: parseSfLatency (shell-degraded path) accepts both 2-column (Android12+
//          OEM) and 3-column classic SF latency rows, like the binder path.
//   R4-F3: GfxInfoFrameTimeProvider does NOT synthesize p50=16.67ms and mark
//          valid=true when gfxinfo has only "Total frames rendered" and no real
//          percentile — that poisoned ML samples.
//
// Links the real frame.o/frame_binder.o/aatpe_ml.o (no mocks). Host-test seam
// parseGfxinfoForTest is AATPE_HOST_TEST-guarded (absent from release nm).
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include "aatpe_ml.h"
#include "aatped.h"

using namespace aatpe_ml;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

using namespace aatpe::frameaware;

int main() {
    // ======================================================================
    // R4-F1: DriftDetector in PRODUCTION units ([0,1] fused score, neutral=0.5)
    // ======================================================================
    printf("=== R4-F1: DriftDetector [0,1] production units ===\n");

    // (1) Neutral (==0.5) must neither accumulate a degrade streak nor clear one.
    {
        DriftDetector d;
        d.setWindow(5);
        // Seed a few below-neutral outcomes to build a partial streak, then feed
        // neutral repeatedly: the streak must be FROZEN (not incremented, not
        // cleared).
        for (int i = 0; i < 3; i++) d.recordOutcome(0.3f, 0);  // 3 < window(5)
        CHECK(!d.isDriftDetected(), "F1: 3 degrades <5 -> not latched yet");
        CHECK(d.getConsecutiveDegrades() == 3, "F1: streak=3 after 3 below-neutral");
        for (int i = 0; i < 10; i++) d.recordOutcome(0.5f, 0);  // neutral
        CHECK(!d.isDriftDetected(), "F1: 10 neutral outcomes do NOT latch");
        CHECK(d.getConsecutiveDegrades() == 3, "F1: neutral leaves streak untouched (==3)");
    }

    // (2) Below-neutral values accumulate the streak and LATCH at window.
    {
        DriftDetector d;
        d.setWindow(5);
        g_mlDriftDetected.store(false);
        for (int i = 0; i < 5; i++) d.recordOutcome(0.3f, 2);  // all <0.5 = degrade
        CHECK(d.isDriftDetected(), "F1: 5 below-neutral outcomes latch drift");
        CHECK(g_mlDriftDetected.load(), "F1: global g_mlDriftDetected exposed");
    }

    // (3) Above-neutral (>0.5) outcomes clear the streak and self-heal the latch.
    {
        DriftDetector d;
        d.setWindow(3);
        g_mlDriftDetected.store(false);
        for (int i = 0; i < 3; i++) d.recordOutcome(0.2f, 1);  // degrade, latch
        CHECK(d.isDriftDetected(), "F1: latched at window=3");
        d.recordOutcome(0.8f, 1);  // >0.5 improvement -> heal
        CHECK(!d.isDriftDetected(), "F1: above-neutral improvement self-heals");
    }

    // (4) The OLD bug repro: feeding production-range [0,1] values where the
    // "outcome" is always >0 (as production does) must STILL be able to latch a
    // real degrade (a score BELOW neutral 0.3). Before the fix, >0 always cleared
    // the streak so this never latched.
    {
        DriftDetector d;
        d.setWindow(5);
        g_mlDriftDetected.store(false);
        for (int i = 0; i < 10; i++) d.recordOutcome(0.4f, 0);  // <0.5 = real degrade
        CHECK(d.isDriftDetected(), "F1: sustained below-neutral (0.4) latches (old bug)");
    }

    g_mlDriftDetected.store(false);

    // ======================================================================
    // R4-F2: parseSfLatency accepts 2-column AND 3-column SF latency rows
    // ======================================================================
    printf("\n=== R4-F2: parseSfLatency 2/3 column acceptance ===\n");
    const char* listOut =
        "StatusBar\n"
        "SurfaceView[com.mygame/com.mygame.UnityPlayerActivity]@0(BLAST)#1\n";

    // 3-column classic rows: period 16.67ms. One on-time, one miss.
    auto fetch3 = [](const std::string&) -> std::string {
        return std::string("16666667\n"
               "10000000 11000000 12000000\n"
               "11000000 12000000 42000000\n");  // (c-a)=30ms > period -> miss
    };
    SfFrameSample s3 = sampleGameLayer(listOut, "com.mygame", fetch3);
    CHECK(s3.available, "F2: 3-column rows -> available");
    CHECK(s3.samples == 2, "F2: 3-column -> 2 samples");
    CHECK(s3.miss == 1, "F2: 3-column -> 1 miss");

    // 2-column (Android 12+ OEM) rows: [vsync, intendedPresent].
    auto fetch2 = [](const std::string&) -> std::string {
        return std::string("16666667\n"
               "10000000 12000000\n"
               "11000000 15000000\n");
    };
    SfFrameSample s2 = sampleGameLayer(listOut, "com.mygame", fetch2);
    CHECK(s2.available, "F2: 2-column rows now accepted (was: all skipped)");
    CHECK(s2.samples == 2, "F2: 2-column -> 2 samples");
    CHECK(s2.miss == 0, "F2: 2-column on-time rows -> 0 miss");

    // 2-column rows within period (on-time).
    auto fetch2miss = [](const std::string&) -> std::string {
        return std::string("16666667\n"
               "10000000 12000000\n"
               "11000000 16000000\n");
    };
    SfFrameSample s2m = sampleGameLayer(listOut, "com.mygame", fetch2miss);
    CHECK(s2m.available && s2m.samples == 2 && s2m.miss == 0,
          "F2: 2-column near-period rows counted correctly");

    // Broken/garbage rows still skipped (1-column, text, invalid markers).
    auto fetchBroken = [](const std::string&) -> std::string {
        return std::string("16666667\n"
               "garbage line\n"
               "10000000\n"
               "-32768 -32768\n"
               "10000000 11000000\n");  // valid 2-col
    };
    SfFrameSample sB = sampleGameLayer(listOut, "com.mygame", fetchBroken);
    CHECK(sB.available && sB.samples == 1, "F2: broken rows skipped, 1 valid 2-col kept");

    // ======================================================================
    // R4-F3: GfxInfo no-percentile -> NOT a valid sample (no synthetic 16.67ms)
    // ======================================================================
    printf("\n=== R4-F3: GfxInfo no-percentile does not fake a valid sample ===\n");
    {
        GfxInfoFrameTimeProvider prov;
        // gfxinfo with "Total frames rendered" but NO percentile summary.
        const char* noPct =
            "Stats since: 123456\n"
            "Total frames rendered: 512\n"
            "Janky frames: 12 (2.34%)\n";
        FrameTimeOutcome r = prov.parseGfxinfoForTest(noPct);
        CHECK(!r.valid, "F3: no percentile -> valid==false (was true)");
        CHECK(r.sample_count == 0, "F3: no percentile -> sample_count==0 (was totalFrames)");
        CHECK(r.frame_time_p95 <= 0.0f, "F3: no percentile -> no synthetic p95");
        CHECK(!prov.isAvailable(), "F3: provider not marked available on no-percentile");
    }
    {
        // Control: a REAL percentile row DOES produce a valid sample.
        GfxInfoFrameTimeProvider prov;
        const char* withPct =
            "Total frames rendered: 512\n"
            "Janky frames: 12\n"
            "50th percentile: 8.0ms\n"
            "95th percentile: 24.0ms\n"
            "99th percentile: 40.0ms\n";
        FrameTimeOutcome r = prov.parseGfxinfoForTest(withPct);
        CHECK(r.valid, "F3: real percentile -> valid==true");
        CHECK(r.sample_count == 512, "F3: real percentile -> sample_count=totalFrames");
        CHECK(r.frame_time_p95 > 0.0f, "F3: real percentile -> p95 parsed");
        CHECK(prov.isAvailable(), "F3: real percentile -> available");
    }
    {
        // Empty output -> invalid (unchanged).
        GfxInfoFrameTimeProvider prov;
        FrameTimeOutcome r = prov.parseGfxinfoForTest("");
        CHECK(!r.valid && r.sample_count == 0, "F3: empty output -> invalid");
    }

    printf("\n========================================\n");
    printf("fx3_frame_v449 results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

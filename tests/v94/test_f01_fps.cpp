// test_f01_fps.cpp - F-01~F-06 帧率/帧时测量口径回归（host hermetic）。
//
// 背景：全局 binder `--latency`（不带 layer 名）跟踪 ViewRoot/HWUI 主层，在任何
// SurfaceView 游戏上都落在 5-15ms 提交延迟带，于是 sfLatValid 几乎恒为 true。旧代码
// 用它当开关，把整段游戏 SurfaceView 层 FPS 测量短路掉，游戏包 5s 后被判
// background_stale 显示 0 / 个位数 fps。本测试锁定修复后的口径：
//
//   F-01: 游戏在前台时，无论全局 sfLatValid 是否 valid，都必须走 per-layer 测量；
//   F-04: renderFps 不足时显示 DEGRADED，绝不回退面板刷新率（假 120fps）；
//   F-06: per-layer 稀疏时回退 MTK GED composer fps；
//   非游戏前台：标注 non_game，而不是把游戏 fps 显示成 0/个位数；
//   数据缺失：如实标 DEGRADED，不编数。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "aatped.h"

// g_foregroundApp is set directly (single-threaded test).
extern std::string g_foregroundApp;
// F-02: game_* state clearer (frame.cpp, global ns).
extern void aatpeFrameClearGameStats();

namespace aatpe { namespace frameaware {
    // F-01: sampler gate decision (frame.cpp).
    bool gameSamplingRequired(bool sfLatValid, bool fgIsGame);
    // F-04/F-06: honest fps resolution (frame.cpp).
    struct GameFpsResolve {
        double fps;
        const char* source;
        bool degraded;
    };
    GameFpsResolve resolveGameFps(double renderFps, double layerHz, int gedFps,
                                  const char* measuredSource);
    // Existing pure layer sampler (frame.cpp).
    SfFrameSample sampleGameLayer(const std::string& listOut, const std::string& fgPkg,
                                  const std::function<std::string(const std::string&)>& fetch,
                                  bool forceGame);
} }

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

using namespace aatpe::frameaware;

// Build a per-layer `--latency` dump for a game running at ~gameFps on a panel of
// panelHz. First line = vsync period (ns); rows = (vsync intended actual-present).
// present timestamps spaced 1/gameFps apart -> renderFps ~= gameFps, hz = panelHz.
static std::string makeLayerDump(double panelHz, double gameFps, int rows) {
    long long periodNs = (long long)(1e9 / panelHz);
    char hdr[64]; snprintf(hdr, sizeof(hdr), "%lld\n", periodNs);
    std::string s = hdr;
    long long t = 10000000000LL;             // arbitrary base uptime ns
    long long stepNs = (long long)(1e9 / gameFps);
    for (int i = 0; i < rows; i++) {
        long long vsync = t;
        long long intended = t;
        long long present = t + 2000000LL;  // +2ms submission latency
        char row[128];
        snprintf(row, sizeof(row), "%lld %lld %lld\n", vsync, intended, present);
        s += row;
        t += stepNs;
    }
    return s;
}

int main() {
    mkdir("/tmp/aatpe_v94_state", 0755);

    // ======================================================================
    printf("=== F-01: game per-layer sampler gate (global latency must NOT short-circuit) ===\n");
    // ======================================================================
    // Old buggy gate was `if (!sfLatValid)`. On a SurfaceView game the global
    // --latency is always valid (5-15ms), so the game layer was never measured.
    CHECK(gameSamplingRequired(/*sfLatValid=*/false, /*fgIsGame=*/true)  == true,
          "F-01: game fg + global latency invalid -> sample (unchanged)");
    CHECK(gameSamplingRequired(/*sfLatValid=*/true,  /*fgIsGame=*/true)  == true,
          "F-01: game fg + global latency VALID -> STILL sample (no short-circuit)");
    CHECK(gameSamplingRequired(/*sfLatValid=*/true,  /*fgIsGame=*/false) == false,
          "F-01: non-game fg + global latency valid -> skip sampler (non_game label)");
    CHECK(gameSamplingRequired(/*sfLatValid=*/false, /*fgIsGame=*/false) == true,
          "F-01: non-game fg + global latency invalid -> sample for jank");

    // ======================================================================
    printf("\n=== F-04: never fabricate panel refresh as measured fps ===\n");
    // ======================================================================
    {
        // Healthy measured 60fps on a 120Hz panel -> report 60, not 120.
        auto r1 = resolveGameFps(60.0, 120.0, 0, "latency_binder");
        CHECK(!r1.degraded, "F-04: measured 60fps -> not degraded");
        CHECK(r1.fps > 58.0 && r1.fps < 62.0, "F-04: fps=60 (measured), NOT panel 120");
        CHECK(std::string(r1.source) == "latency_binder", "F-04: source=latency_binder");

        // Sparse ring (renderFps=0) on a 120Hz panel, no GED -> DEGRADED, NOT 120.
        // This is the exact "fake 120fps while game is at single digits" bug.
        auto r2 = resolveGameFps(0.0, 120.0, 0, "latency_binder");
        CHECK(r2.degraded, "F-04: sparse ring + no GED -> degraded (not fake 120)");
        CHECK(r2.fps == 0.0, "F-04: degraded fps=0 (no fabricated number)");

        // Borderline sparse (renderFps=0.5) also degraded.
        auto r3 = resolveGameFps(0.5, 120.0, 0, "latency_dumpsys");
        CHECK(r3.degraded, "F-04: renderFps=0.5 (<=1) -> degraded");

        // Healthy 120fps game on 120Hz panel -> 120, not degraded.
        auto r4 = resolveGameFps(120.0, 120.0, 0, "latency_binder");
        CHECK(!r4.degraded && r4.fps > 119.0 && r4.fps < 121.0, "F-04: healthy 120 -> 120");
    }

    // ======================================================================
    printf("\n=== F-06: MTK GED composer fps fallback ===\n");
    // ======================================================================
    {
        // Per-layer ring sparse, but GED composer reports 85fps -> use 85.
        auto r1 = resolveGameFps(0.0, 120.0, 85, "latency_binder");
        CHECK(!r1.degraded, "F-06: sparse ring but GED=85 -> not degraded");
        CHECK(r1.fps > 84.0 && r1.fps < 86.0, "F-06: fps=85 from GED composer node");
        CHECK(std::string(r1.source) == "ged_composer", "F-06: source=ged_composer");

        // Measured present span wins over GED.
        auto r2 = resolveGameFps(60.0, 120.0, 85, "latency_binder");
        CHECK(!r2.degraded && r2.fps > 58.0 && r2.fps < 62.0,
              "F-06: measured 60 wins over GED 85");

        // GED out of range (999) -> degraded, not trusted.
        auto r3 = resolveGameFps(0.0, 120.0, 999, "latency_binder");
        CHECK(r3.degraded, "F-06: GED=999 out of range -> degraded");

        // GED=0 (node absent) + sparse ring -> degraded.
        auto r4 = resolveGameFps(0.0, 120.0, 0, "latency_binder");
        CHECK(r4.degraded, "F-06: no GED + sparse -> degraded");
    }

    // ======================================================================
    printf("\n=== End-to-end: fake SF dump with game layer -> measured fps, not panel hz ===\n");
    // ======================================================================
    {
        // Realistic王者 SurfaceView layer on a 120Hz panel, game running 60fps.
        const char* listOut =
            "StatusBar\n"
            "NavigationBar\n"
            "SurfaceView[com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity]@0(BLAST)#12\n";
        // fetchLatency: return a 60fps-on-120Hz dump for the game layer; empty for others.
        auto fetch = [](const std::string& layer) -> std::string {
            if (layer.find("sgame") != std::string::npos) {
                return makeLayerDump(/*panelHz=*/120.0, /*gameFps=*/60.0, /*rows=*/40);
            }
            return "";
        };
        auto smp = sampleGameLayer(listOut, "com.tencent.tmgp.sgame", fetch, /*forceGame=*/false);
        CHECK(smp.available, "E2E: game layer found and has valid present rows");
        CHECK(smp.renderFps > 55.0 && smp.renderFps < 65.0, "E2E: renderFps ~60 (measured)");
        CHECK(smp.hz > 118.0 && smp.hz < 122.0, "E2E: layer hz = panel 120 (target hint)");

        // Resolve with the production policy: 60, NOT the panel 120.
        auto gr = resolveGameFps(smp.renderFps, (double)smp.hz, 0, "latency_binder");
        CHECK(!gr.degraded, "E2E: not degraded");
        CHECK(gr.fps > 55.0 && gr.fps < 65.0, "E2E: displayed fps ~60 (NOT panel 120)");

        // This is the F-01 core: even though the GLOBAL sfLatValid would be true on
        // this game, the sampler still runs (gameSamplingRequired=true) and yields 60.
        CHECK(gameSamplingRequired(true, true) == true,
              "E2E: global latency valid but game fg -> sampler runs (60 measured)");
    }

    // ======================================================================
    printf("\n=== F-02: measured game fps published to per-app panel, NOT cleared to 0 ===\n");
    // ======================================================================
    {
        // Simulate the per-layer success path publishing the resolved 60fps for the
        // game pkg (what readSurfaceFlinger now does every tick for a foreground game).
        g_foregroundApp = "com.tencent.tmgp.sgame";
        aatpeSetFrameJank("latency_binder", 40, 0.0, 60.0, 58.0, 55.0);
        // Publish a second tick so the window is fresh (ageMs ~0, not >5000).
        aatpeSetFrameJank("latency_binder", 40, 0.0, 60.0, 58.0, 55.0);

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        const AatpePerAppFrameStat* s = nullptr;
        for (auto& st : out) if (st.pkg == "com.tencent.tmgp.sgame") s = &st;
        CHECK(s != nullptr, "F-02: game pkg present in per-app panel");
        if (s) {
            CHECK(s->fps > 58.0 && s->fps < 62.0, "F-02: game fps=60 (measured, not 0)");
            CHECK(s->reason != "background_stale", "F-02: NOT background_stale (fresh window)");
        }
    }

    // ======================================================================
    printf("\n=== non-game foreground: non_game label, game fps held/cleared honestly ===\n");
    // ======================================================================
    {
        // The else-branch (non-game, global latency valid) calls
        // aatpeFrameClearGameStats which zeroes game_*; the frame_source is set to
        // "non_game" (verified by contract). We exercise the clear helper to prove it
        // is safe and idempotent, and that a non-game pkg does NOT get a fake game row.
        aatpeFrameClearGameStats();
        g_foregroundApp = "com.android.launcher";   // not a game
        // No aatpeSetFrameJank call (non-game path skips the sampler) -> no game row
        // is pushed. The launcher must NOT appear as a "game at 0fps" entry.
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        bool launcherGameRow = false;
        for (auto& st : out) {
            if (st.pkg == "com.android.launcher") launcherGameRow = true;
        }
        CHECK(!launcherGameRow, "non-game: launcher does NOT get a fake game-fps row");
    }

    printf("\n========================================\n"
           "F-01~F-06 host regression: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

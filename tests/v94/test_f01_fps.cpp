#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "aatped.h"

extern std::string g_foregroundApp;
extern void aatpeFrameClearGameStats();

namespace aatpe { namespace frameaware {
    bool gameSamplingRequired(bool sfLatValid, bool fgIsGame);
    struct GameFpsResolve {
        double fps;
        const char* source;
        bool degraded;
    };
    GameFpsResolve resolveGameFps(double renderFps, double layerHz, int gedFps,
                                  const char* measuredSource);
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

static std::string makeLayerDump(double panelHz, double gameFps, int rows) {
    long long periodNs = (long long)(1e9 / panelHz);
    char hdr[64]; snprintf(hdr, sizeof(hdr), "%lld\n", periodNs);
    std::string s = hdr;
    long long t = 10000000000LL;
    long long stepNs = (long long)(1e9 / gameFps);
    for (int i = 0; i < rows; i++) {
        long long vsync = t;
        long long intended = t;
        long long present = t + 2000000LL;
        char row[128];
        snprintf(row, sizeof(row), "%lld %lld %lld\n", vsync, intended, present);
        s += row;
        t += stepNs;
    }
    return s;
}

int main() {
    mkdir("/tmp/aatpe_v94_state", 0755);

    printf("=== F-01: game per-layer sampler gate (global latency must NOT short-circuit) ===\n");
    CHECK(gameSamplingRequired( false, true)  == true,
          "F-01: game fg + global latency invalid -> sample (unchanged)");
    CHECK(gameSamplingRequired( true, true)  == true,
          "F-01: game fg + global latency VALID -> STILL sample (no short-circuit)");
    CHECK(gameSamplingRequired( true, false) == false,
          "F-01: non-game fg + global latency valid -> skip sampler (non_game label)");
    CHECK(gameSamplingRequired( false, false) == true,
          "F-01: non-game fg + global latency invalid -> sample for jank");

    printf("\n=== F-04: never fabricate panel refresh as measured fps ===\n");
    {
        auto r1 = resolveGameFps(60.0, 120.0, 0, "latency_binder");
        CHECK(!r1.degraded, "F-04: measured 60fps -> not degraded");
        CHECK(r1.fps > 58.0 && r1.fps < 62.0, "F-04: fps=60 (measured), NOT panel 120");
        CHECK(std::string(r1.source) == "latency_binder", "F-04: source=latency_binder");

        auto r2 = resolveGameFps(0.0, 120.0, 0, "latency_binder");
        CHECK(r2.degraded, "F-04: sparse ring + no GED -> degraded (not fake 120)");
        CHECK(r2.fps == 0.0, "F-04: degraded fps=0 (no fabricated number)");

        auto r3 = resolveGameFps(0.5, 120.0, 0, "latency_dumpsys");
        CHECK(r3.degraded, "F-04: renderFps=0.5 (<=1) -> degraded");

        auto r4 = resolveGameFps(120.0, 120.0, 0, "latency_binder");
        CHECK(!r4.degraded && r4.fps > 119.0 && r4.fps < 121.0, "F-04: healthy 120 -> 120");
    }

    printf("\n=== F-06: MTK GED composer fps fallback ===\n");
    {
        auto r1 = resolveGameFps(0.0, 120.0, 85, "latency_binder");
        CHECK(!r1.degraded, "F-06: sparse ring but GED=85 -> not degraded");
        CHECK(r1.fps > 84.0 && r1.fps < 86.0, "F-06: fps=85 from GED composer node");
        CHECK(std::string(r1.source) == "ged_composer", "F-06: source=ged_composer");

        auto r2 = resolveGameFps(60.0, 120.0, 85, "latency_binder");
        CHECK(!r2.degraded && r2.fps > 58.0 && r2.fps < 62.0,
              "F-06: measured 60 wins over GED 85");

        auto r3 = resolveGameFps(0.0, 120.0, 999, "latency_binder");
        CHECK(r3.degraded, "F-06: GED=999 out of range -> degraded");

        auto r4 = resolveGameFps(0.0, 120.0, 0, "latency_binder");
        CHECK(r4.degraded, "F-06: no GED + sparse -> degraded");
    }

    printf("\n=== End-to-end: fake SF dump with game layer -> measured fps, not panel hz ===\n");
    {
        const char* listOut =
            "StatusBar\n"
            "NavigationBar\n"
            "SurfaceView[com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity]@0(BLAST)#12\n";
        auto fetch = [](const std::string& layer) -> std::string {
            if (layer.find("sgame") != std::string::npos) {
                return makeLayerDump( 120.0, 60.0, 40);
            }
            return "";
        };
        auto smp = sampleGameLayer(listOut, "com.tencent.tmgp.sgame", fetch, false);
        CHECK(smp.available, "E2E: game layer found and has valid present rows");
        CHECK(smp.renderFps > 55.0 && smp.renderFps < 65.0, "E2E: renderFps ~60 (measured)");
        CHECK(smp.hz > 118.0 && smp.hz < 122.0, "E2E: layer hz = panel 120 (target hint)");

        auto gr = resolveGameFps(smp.renderFps, (double)smp.hz, 0, "latency_binder");
        CHECK(!gr.degraded, "E2E: not degraded");
        CHECK(gr.fps > 55.0 && gr.fps < 65.0, "E2E: displayed fps ~60 (NOT panel 120)");

        CHECK(gameSamplingRequired(true, true) == true,
              "E2E: global latency valid but game fg -> sampler runs (60 measured)");
    }

    printf("\n=== F-02: measured game fps published to per-app panel, NOT cleared to 0 ===\n");
    {
        g_foregroundApp = "com.tencent.tmgp.sgame";
        aatpeSetFrameJank("latency_binder", 40, 0.0, 60.0, 58.0, 55.0);
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

    printf("\n=== non-game foreground: non_game label, game fps held/cleared honestly ===\n");
    {
        aatpeFrameClearGameStats();
        g_foregroundApp = "com.android.launcher";
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

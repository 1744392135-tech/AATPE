#include <cstdio>
#include <string>
#include <vector>
#include <set>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

using namespace aatpe::frameaware;

static std::string fetch60(const std::string& ) {
    return std::string("16666667\n") +
           "10000000 11000000 12000000\n"
           "11000000 12000000 13000000\n"
           "12000000 18000000 42000000\n";
}

int main() {
    printf("=== v4.2.1: multi-format game layer recognition (MIUI/MTK) ===\n");

    {
        const char* listOut =
            "StatusBar\n"
            "SurfaceView[com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity]@0(BLAST)#12\n";
        auto layers = sfListLayers(listOut);
        std::string r, cand;
        std::string picked = selectGameLayer(layers, "com.tencent.tmgp.sgame", false, &r, &cand);
        CHECK(picked.find("SGameActivity") != std::string::npos,
              "(a) bracket SGameActivity selected (王者, no engine keyword)");
        CHECK(r.empty(), "(a) no reason on success");
        CHECK(cand.find("SGameActivity") != std::string::npos, "(a) candidate recorded");
    }

    {
        const char* listOut =
            "NavigationBar\n"
            "SurfaceView - com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity#0\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.tencent.tmgp.sgame");
        CHECK(picked.find("SGameActivity") != std::string::npos,
              "(b) dash-form 'SurfaceView - pkg/act#n' selected");
    }

    {
        const char* listOut =
            "StatusBar\n"
            "com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity#0\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.tencent.tmgp.sgame");
        CHECK(picked.find("SGameActivity") != std::string::npos,
              "(c) bare 'pkg/act#n' selected (MTK)");
    }

    {
        const char* listOut =
            "SurfaceView[com.tencent.tmgp.sgame]@0(BLAST)#5\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.tencent.tmgp.sgame");
        CHECK(picked.find("com.tencent.tmgp.sgame") != std::string::npos,
              "(d) bracket-only-package layer selected");
    }

    {
        const char* listOut =
            "SurfaceView[com.unity.game/com.unity.game.UnityPlayerActivity]@0(BLAST)#1\n"
            "SurfaceView[org.cocos2dx.game/org.cocos2dx.game.AppActivity]@0(BLAST)#2\n"
            "SurfaceView[com.gdx.game/com.gdx.game.GdxActivity]@0(BLAST)#3\n"
            "SurfaceView[com.self.game/com.self.game.MyGLSurfaceView]@0(BLAST)#4\n";
        auto layers = sfListLayers(listOut);
        CHECK(selectGameLayer(layers, "com.unity.game").find("UnityPlayerActivity") != std::string::npos,
              "Unity layer selected");
        CHECK(selectGameLayer(layers, "org.cocos2dx.game").find("cocos2dx") != std::string::npos,
              "Cocos2dx layer selected");
        CHECK(selectGameLayer(layers, "com.gdx.game").find("GdxActivity") != std::string::npos,
              "libGDX GdxActivity selected");
        CHECK(selectGameLayer(layers, "com.self.game").find("GLSurfaceView") != std::string::npos,
              "自研 GLSurfaceView selected");
    }

    printf("\n=== v4.2.1: anti-mis-selection (blacklist) + ranking ===\n");

    {
        const char* listOut =
            "SurfaceView[com.mygame/com.mygame.InputMethod]@0(BLAST)#1\n"
            "SurfaceView[com.mygame/com.mygame.CameraView]@0(BLAST)#2\n"
            "SurfaceView[com.mygame/com.mygame.AdView]@0(BLAST)#3\n"
            "SurfaceView[com.mygame/com.mygame.ExoPlayer]@0(BLAST)#4\n"
            "SurfaceView[com.mygame/com.mygame.GameRender]@0(BLAST)#5\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.mygame");
        CHECK(picked.find("GameRender") != std::string::npos,
              "blacklisted IME/camera/ad/video skipped -> real render layer wins");
        CHECK(picked.find("InputMethod") == std::string::npos &&
              picked.find("Camera") == std::string::npos &&
              picked.find("AdView") == std::string::npos,
              "no blacklisted layer chosen");
    }

    {
        const char* listOut =
            "com.mygame/com.mygame.MainWindow#0\n"
            "SurfaceView[com.mygame/com.mygame.UnityPlayerActivity]@0(BLAST)#9\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.mygame");
        CHECK(picked.find("UnityPlayerActivity") != std::string::npos,
              "engine layer ranked above bare window layer");
    }

    {
        const char* listOut =
            "SurfaceView[com.other/com.other.GameActivity]@0(BLAST)#1\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.mygame");
        CHECK(picked.empty(), "wrong-package layer not selected");
    }

    printf("\n=== v4.2.1: reason diagnostics ===\n");
    {
        SfFrameSample s = sampleGameLayer("StatusBar\n", "", fetch60);
        CHECK(s.frameUnavailable && s.reason == "no_fg", "empty fg -> reason no_fg");
    }
    {
        SfFrameSample s = sampleGameLayer("", "com.mygame", fetch60);
        CHECK(s.frameUnavailable && s.reason == "sf_list_empty", "empty list -> reason sf_list_empty");
        CHECK(s.candidates == "none", "empty list -> candidates none");
    }
    {
        std::string r, cand;
        auto layers = sfListLayers("StatusBar\nNavigationBar\n");
        std::string picked = selectGameLayer(layers, "com.mygame", false, &r, &cand);
        CHECK(picked.empty() && r == "no_layer_for_pkg",
              "no layer references fg pkg -> reason no_layer_for_pkg");
    }
    {
        const char* listOut = "SurfaceView[com.mygame/com.mygame.GameRender]@0(BLAST)#1\n";
        auto fetchEmpty = [](const std::string&) { return std::string(""); };
        SfFrameSample s = sampleGameLayer(listOut, "com.mygame", fetchEmpty);
        CHECK(s.frameUnavailable && s.reason == "no_valid_frames",
              "selected layer, empty latency -> no_valid_frames");
    }

    printf("\n=== v4.2.1: --latency FPS / miss counting (relaxed selection path) ===\n");
    {
        const char* listOut =
            "SurfaceView[com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.SGameActivity]@0(BLAST)#7\n";
        SfFrameSample s = sampleGameLayer(listOut, "com.tencent.tmgp.sgame", fetch60);
        CHECK(s.available, "王者 bracket: available");
        CHECK(!s.frameUnavailable, "王者 bracket: not unavailable");
        CHECK(s.samples == 3, "王者 bracket: 3 valid rows");
        CHECK(s.miss == 1, "王者 bracket: 1 miss");
        CHECK(s.hz > 59.0f && s.hz < 61.0f, "王者 bracket: hz ~= 60");
        CHECK(s.chosenLayer.find("SGameActivity") != std::string::npos,
              "王者 bracket: verbatim layer passed to --latency (#n/(BLAST) kept)");
    }

    printf("\n=== v4.2.1: manual game list (forceGame) ===\n");
    {
        const char* listOut = "StatusBar\nNavigationBar\n";
        auto layers = sfListLayers(listOut);
        std::string r, cand;
        std::string picked = selectGameLayer(layers, "com.listed.app", true , &r, &cand);
        CHECK(picked.empty() && r == "no_layer_for_pkg",
              "forceGame pkg with no SurfaceView -> no_layer_for_pkg");
    }
    {
        const char* listOut = "com.listed.app/com.listed.app.MainView#0\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.listed.app", true );
        CHECK(picked.find("MainView") != std::string::npos,
              "forceGame: bare layer of listed pkg selected");
    }
    {
        const char* listOut = "SurfaceView[com.listed.app/com.listed.app.InputMethod]@0(BLAST)#1\n";
        auto layers = sfListLayers(listOut);
        std::string picked = selectGameLayer(layers, "com.listed.app", true);
        CHECK(picked.empty(), "forceGame: still rejects blacklisted IME SurfaceView");
    }

    printf("\n=== v4.2.1: pm list parsing ===\n");
    {
        const char* pm =
            "package:com.tencent.tmgp.sgame\n"
            "package:com.miHoYo.GenshinImpact\n"
            "  package:com.example.withspace  \n"
            "Malformed line without prefix\n"
            "package:invalid..pkg\n"
            "package:_leading.underscore\n";
        auto pkgs = parsePmPackageList(pm);
        bool hasSgame = false, hasGenshin = false, hasSpace = false;
        bool rejectedBad = true;
        for (auto& p : pkgs) {
            if (p == "com.tencent.tmgp.sgame") hasSgame = true;
            if (p == "com.miHoYo.GenshinImpact") hasGenshin = true;
            if (p == "com.example.withspace") hasSpace = true;
            if (p == "invalid..pkg" || p == "_leading.underscore") rejectedBad = false;
        }
        CHECK(hasSgame && hasGenshin, "pm: valid third-party packages parsed");
        CHECK(hasSpace, "pm: trailing whitespace trimmed");
        CHECK(rejectedBad, "pm: malformed/invalid pkg rejected (no 'package:' prefix, '..', leading _)");
    }

    printf("\n=== v4.2.1: game_overrides.list parsing ===\n");
    {
        const char* content =
            "# comment line\n"
            "\n"
            "com.tencent.tmgp.sgame\n"
            "com.tencent.tmgp.sgame\n"
            "  com.miHoYo.GenshinImpact  \n"
            "com.bad;rm -rf\n"
            "..not.a.pkg\n"
            "# com.should.ignore.commented\n";
        std::set<std::string> s = parseGameOverridesContent(content);
        CHECK(s.count("com.tencent.tmgp.sgame") == 1, "list: valid pkg accepted, dup deduped");
        CHECK(s.count("com.miHoYo.GenshinImpact") == 1, "list: whitespace trimmed");
        CHECK(s.count("com.bad;rm -rf") == 0, "list: shell-metachar pkg rejected");
        CHECK(s.count("..not.a.pkg") == 0, "list: invalid '..' pkg rejected");
        CHECK(s.count("com.should.ignore.commented") == 0, "list: commented line ignored");
        CHECK(s.size() == 2, "list: exactly 2 valid entries");
    }

    printf("\n===================================================\n");
    printf("v4.2.1 hotfix host tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

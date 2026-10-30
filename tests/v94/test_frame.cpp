#include <cstdio>
#include <cmath>
#include "aatped.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

using namespace aatpe::frameaware;

namespace aatpe { namespace frameaware {
    struct FrameLatencyAlarm { bool latCritical; bool jank; };
    FrameLatencyAlarm aatpeFrameLatencyAlarm(bool sfLatValid, int avgLatUs, int maxLatUs,
                                             int effectiveThresholdUs, int frameTimeUs,
                                             float jankThresholdPct, bool alarmAllowed);
} }

extern std::string g_foregroundApp;
namespace aatpe { namespace gamelist {
    void setManualListForTest(const std::vector<std::string>& pkgs);
} }
namespace aatpe { namespace frameaware {
    long long aatpeDebugTimestatsParsePasses();
    void aatpeDebugTimestatsResetParseCounter();
} }
extern void aatpeDebugProcessManualLayersForTest(const std::string& dump,
                                                  const std::string& fgPkg,
                                                  long long contentTsMs);
extern void aatpeDebugFsMetricsSet(const std::string& p);

static std::string driveToReason(const std::string& dumpBody, const std::string& fgPkg) {
    TimestatsSession s;
    auto fetch = [&](const std::string& cmd) -> std::string {
        if (cmd.find("-dump") != std::string::npos) return dumpBody;
        return std::string();
    };
    s.tick(0, fgPkg, fetch);
    TimestatsResult r = s.tick(1500, fgPkg, fetch);
    return r.reason;
}

int main() {
    printf("=== Frame: getRefreshRate / getFrameIntervalMs defaults ===\n");
    int rr = getRefreshRate();
    CHECK(rr >= 0, "getRefreshRate returns non-negative");
    int fi = getFrameIntervalMs();
    CHECK(fi >= 0, "getFrameIntervalMs returns non-negative");

    printf("=== Frame: shouldTriggerFrameAware (static-internal state) ===\n");
    bool a = shouldTriggerFrameAware(1000);
    bool b = shouldTriggerFrameAware(1000);
    CHECK(a == b, "same nowMs -> same decision (pure predicate)");
    bool farFuture = shouldTriggerFrameAware(999999999);
    CHECK(farFuture == true, "shouldTriggerFrameAware returns true for a far-future timestamp");

    printf("\n=== WS-C2: game SurfaceView layer selection + latency parse ===\n");

    const char* listOut =
        "StatusBar\n"
        "NavigationBar\n"
        "SurfaceView[com.mygame/com.mygame.UnityPlayerActivity]@0(BLAST)#1\n"
        "SurfaceView[com.mygame/com.mygame.AdViewActivity]@0(BLAST)#2\n"
        "SurfaceView[com.other/com.other.GameActivity]@0(BLAST)#3\n"
        "0xdeadbeef (ignored)\n";
    std::vector<std::string> layers = sfListLayers(listOut);
    bool hasGame = false, hasStatusBar = false, skippedHex = true;
    for (auto& l : layers) {
        if (l.find("UnityPlayerActivity") != std::string::npos) hasGame = true;
        if (l == "StatusBar") hasStatusBar = true;
        if (l.find("0x") != std::string::npos) skippedHex = false;
    }
    CHECK(hasGame && hasStatusBar, "sfListLayers keeps real layers (game + statusbar)");
    CHECK(skippedHex, "sfListLayers skips hex id lines");

    CHECK(extractBracketedPkg("SurfaceView[com.mygame/com.mygame.UnityPlayerActivity]@0(BLAST)#1")
              == "com.mygame/com.mygame.UnityPlayerActivity",
          "extractBracketedPkg pulls the bracket content");
    CHECK(extractBracketedPkg("StatusBar").empty(), "no bracket -> empty pkg");

    std::string picked = selectGameLayer(layers, "com.mygame");
    CHECK(picked.find("UnityPlayerActivity") != std::string::npos,
          "selectGameLayer picks bracketed pkg==fgPkg + engine activity");
    CHECK(picked.find("AdView") == std::string::npos,
          "selectGameLayer skips blacklisted ad-SDK SurfaceView");
    CHECK(picked.find("com.other") == std::string::npos,
          "selectGameLayer skips wrong-package game layer");

    auto fetch60 = [](const std::string&) -> std::string {
        return std::string("16666667\n") +
               "10000000 11000000 12000000\n"
               "11000000 12000000 13000000\n"
               "-32768 -32768 -32768\n"
               "12000000 18000000 42000000\n"
               "0 0 0\n";
    };
    SfFrameSample s60 = sampleGameLayer(listOut, "com.mygame", fetch60);
    CHECK(s60.available, "60Hz game layer: sample marked available");
    CHECK(!s60.frameUnavailable, "60Hz game layer: not frame_unavailable");
    CHECK(s60.samples == 3, "60Hz: exactly 3 valid frame rows");
    CHECK(s60.miss == 1, "60Hz: exactly 1 deadline miss");
    CHECK(s60.hz > 59.0f && s60.hz < 61.0f, "60Hz: hz ~= 60");

    auto fetch120 = [](const std::string&) -> std::string {
        return std::string("8333333\n") +
               "10000000 10500000 11000000\n"
               "11000000 11500000 12000000\n";
    };
    SfFrameSample s120 = sampleGameLayer(listOut, "com.mygame", fetch120);
    CHECK(s120.hz > 119.0f && s120.hz < 121.0f, "120Hz: hz ~= 120");
    CHECK(s120.miss == 0, "120Hz: no miss on on-time frames");

    auto fetchBadPeriod = [](const std::string&) -> std::string {
        return std::string("10000000000\n") +
               "10000000 11000000 12000000\n";
    };
    SfFrameSample sBadP = sampleGameLayer(listOut, "com.mygame", fetchBadPeriod);
    CHECK(!sBadP.available && sBadP.frameUnavailable,
          "T2: out-of-bound period rejected -> frame_unavailable");

    const char* listNoGame = "StatusBar\nNavigationBar\nSurfaceView[com.launcher/com.launcher.Launcher]@0(BLAST)#1\n";
    SfFrameSample sNone = sampleGameLayer(listNoGame, "com.unknown.game", fetch60);
    CHECK(!sNone.available && sNone.frameUnavailable,
          "no matching game layer -> frame_unavailable (fail-closed)");

    auto fetchEmpty = [](const std::string&) -> std::string {
        return std::string("16666667\n-32768 -32768 -32768\n");
    };
    SfFrameSample sBad = sampleGameLayer(listOut, "com.mygame", fetchEmpty);
    CHECK(!sBad.available && sBad.frameUnavailable,
          "all-invalid rows -> frame_unavailable (no jank vote on missing data)");

    auto fetchOverflow = [](const std::string&) -> std::string {
        return std::string("16666667\n") +
               "8782103423042627808 1 -6493876427050512817\n";
    };
    SfFrameSample sOv = sampleGameLayer(listOut, "com.mygame", fetchOverflow);
    CHECK(!sOv.available && sOv.frameUnavailable,
          "overflow-magnitude row skipped -> frame_unavailable (no overflow, no vote)");

    auto fetchNormal = [](const std::string&) -> std::string {
        return std::string("16666667\n") +
               "10000000 11000000 12000000\n";
    };
    SfFrameSample sN = sampleGameLayer(listOut, "com.mygame", fetchNormal);
    CHECK(sN.available && sN.samples == 1 && sN.miss == 0,
          "normal positive timestamps still counted (control)");

    printf("\n=== round4b: latency renderFps / all-zero container skip ===\n");
    auto fetchContent = [](const std::string& layer) -> std::string {
        if (layer.find("ActivityRecord") != std::string::npos) {
            return std::string("8333333\n0 0 0\n0 0 0\n0 0 0\n");
        }
        std::string out = "8333333\n";
        long long base = 1000000000000LL;
        for (int i = 0; i < 10; i++) {
            long long a = base + (long long)(i * 8333333);
            long long b = (i == 9) ? 9223372036854775807LL : a + 27000000LL;
            long long c = a + 42000000LL;
            char row[96];
            snprintf(row, sizeof(row), "%lld %lld %lld\n", a, b, c);
            out += row;
        }
        return out;
    };
    const char* listTwo =
        "ActivityRecord{1_u0_com.game_.Main_t1}#0\n"
        "SurfaceView[com.game/Main]@0(BLAST)#0\n";
    SfFrameSample sR = sampleGameLayer(listTwo, "com.game", fetchContent);
    CHECK(sR.available, "round4b: picked the content layer (all-zero container skipped)");
    CHECK(sR.samples >= 9, "round4b: >=9 present rows counted (INT64_MAX row still counted via present col)");
    CHECK(sR.renderFps > 90.0 && sR.renderFps < 150.0,
          "round4b: renderFps ~120 from present ring (not panel-only)");
    auto fetchOnlyZero = [](const std::string&) -> std::string {
        return std::string("8333333\n0 0 0\n0 0 0\n");
    };
    SfFrameSample sZ = sampleGameLayer(listTwo, "com.game", fetchOnlyZero);
    CHECK(!sZ.available && sZ.frameUnavailable,
          "round4b: all-zero container layer -> frame_unavailable (no fake fps)");

    printf("\n=== round5: chooseForegroundFps (latency wins, ts fallback, suspect) ===\n");
    {
        int src; bool suspect;
        double f1 = chooseForegroundFps(101.0, 120.0, true, 12.4, 242, src, suspect);
        CHECK(fabs(f1 - 101.0) < 0.5, "round5: latency wins over contradicting timestats");
        CHECK(src == 0 && suspect, "round5: source=latency, timestats flagged suspect");
        double f2 = chooseForegroundFps(0.0, 0.0, false, 58.0, 20, src, suspect);
        CHECK(fabs(f2 - 58.0) < 0.5, "round5: timestats used when latency unavailable");
        CHECK(src == 1 && !suspect, "round5: source=timestats, not suspect");
        double f3 = chooseForegroundFps(100.0, 120.0, true, 105.0, 30, src, suspect);
        CHECK(fabs(f3 - 100.0) < 0.5 && src == 0 && !suspect,
              "round5: channels agree -> latency, no suspect flag");
        double f4 = chooseForegroundFps(0.0, 0.0, false, 0.0, 0, src, suspect);
        CHECK(src == -1 && f4 == 0.0, "round5: no channel -> source=none");
    }

    printf("\n=== Timestats: parseTimestatsBlock ===\n");

    const char* tsNormal =
        "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n"
        "  totalFrames = 120\n"
        "  averageFPS = 59.8\n"
        "  presentToPresent histogram:\n"
        "    8ms: 100\n"
        "    16ms: 18\n"
        "    33ms: 2\n";
    TimestatsResult tn = parseTimestatsBlock(tsNormal, "com.mygame");
    CHECK(tn.available, "ts normal: available");
    CHECK(tn.totalFrames == 120, "ts normal: totalFrames=120");
    CHECK(tn.missRate > 0.1 && tn.missRate < 0.2,
          "ts normal: missRate ~= 20/120 (histogram-weighted threshold)");
    CHECK(tn.layer.find("SurfaceView") != std::string::npos, "ts normal: layer captured");

    const char* tsJank =
        "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n"
        "  totalFrames = 120\n"
        "  averageFPS = 60.0\n"
        "  presentToPresent histogram:\n"
        "    9ms: 80\n"
        "    19ms: 40\n";
    TimestatsResult tj = parseTimestatsBlock(tsJank, "com.mygame");
    CHECK(tj.available, "ts jank: available");
    CHECK(tj.targetFps > 110.0 && tj.targetFps < 130.0, "ts jank: target ≈120");
    CHECK(tj.missRate > 0.25 && tj.missRate < 0.45, "ts jank: missRate ~0.33 -> arms vote");
    CHECK(tj.p99Fps > 0.0, "ts jank: p90/p99 fps computed");

    TimestatsResult te = parseTimestatsBlock("", "com.mygame");
    CHECK(!te.available, "ts empty: not available (fail-closed)");

    const char* tsNoFrames =
        "  totalFrames = 0\n"
        "  presentToPresent histogram:\n"
        "    8ms: 0\n";
    TimestatsResult tnf = parseTimestatsBlock(tsNoFrames, "com.mygame");
    CHECK(!tnf.available, "ts zero frames: not available");

    const char* tsMulti =
        "Layer Name's timestats: SurfaceView[com.other/Back]@0(BLAST)#9\n"
        "  totalFrames = 100\n"
        "  averageFPS = 60.0\n"
        "  presentToPresent histogram:\n"
        "    33ms: 100\n"
        "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n"
        "  totalFrames = 100\n"
        "  averageFPS = 60.0\n"
        "  presentToPresent histogram:\n"
        "    8ms: 100\n";
    TimestatsResult tm = parseTimestatsBlock(tsMulti, "com.mygame");
    CHECK(tm.available, "F-01: multi-block available");
    CHECK(tm.layer.find("com.mygame") != std::string::npos, "F-01: fg block selected");
    CHECK(tm.missRate < 0.01, "F-01: background slow block NOT mixed (missRate~0)");

    const char* tsLtpo =
        "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n"
        "  totalFrames = 60\n"
        "  averageFPS = 60.0\n"
        "  presentToPresent histogram:\n"
        "    16ms: 60\n";
    TimestatsResult tl = parseTimestatsBlock(tsLtpo, "com.mygame");
    CHECK(tl.available && tl.missRate == 0.0, "F-04: LTPO 60fps layer not falsely flagged");

    const char* tsOver =
        "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n"
        "  totalFrames = 100\n"
        "  averageFPS = 60.0\n"
        "  presentToPresent histogram:\n"
        "    33ms: 200\n";
    TimestatsResult to = parseTimestatsBlock(tsOver, "com.mygame");
    CHECK(to.available && to.missRate <= 1.0 + 1e-9, "F-07: missRate clamped <=1.0");

    {
        std::vector<std::string> layers = {
            "StatusBar",
            "SurfaceView[com.mygame.UnityPlayerActivity]@0(BLAST)#7",
        };
        std::string picked = selectGameLayer(layers, "com.mygame");
        CHECK(picked.find("UnityPlayerActivity") != std::string::npos,
              "F-06: single-component bracket layer selected");
    }
    {
        std::vector<std::string> layers = {
            "SurfaceView[com.other.UnityPlayerActivity]@0(BLAST)#7",
        };
        std::string picked = selectGameLayer(layers, "com.mygame");
        CHECK(picked.empty(), "F-06: wrong-pkg single-bracket rejected");
    }

    printf("\n=== TimestatsSession: lifecycle ===\n");
    std::vector<std::string> cmds;
    auto mockFetch = [&](const std::string& c) -> std::string {
        cmds.push_back(c);
        if (c.find("-dump") != std::string::npos) {
            return std::string("Layer Name's timestats: SurfaceView[com.mygame/c]@0(BLAST)#1\n")
                 + "  totalFrames = 120\n  averageFPS = 59.8\n"
                 + "  presentToPresent histogram:\n    8ms: 118\n    33ms: 2\n";
        }
        return "";
    };
    TimestatsSession sess;
    TimestatsResult r0 = sess.tick(0, "com.mygame", mockFetch);
    CHECK(!r0.available && sess.forkCalls == 1, "t0: enable fork, warmup no data");
    TimestatsResult r1 = sess.tick(500, "com.mygame", mockFetch);
    CHECK(!r1.available && sess.forkCalls == 1, "t500: warmup no fork");
    TimestatsResult r2 = sess.tick(1500, "com.mygame", mockFetch);
    CHECK(r2.available && sess.forkCalls == 2, "t1500: dump fork, data available");

    TimestatsSession sess2;
    std::vector<std::string> cmds2;
    auto emptyFetch = [&](const std::string& c) -> std::string { cmds2.push_back(c); return ""; };
    sess2.tick(0, "com.mygame", emptyFetch);
    TimestatsResult fe = sess2.tick(2000, "com.mygame", emptyFetch);
    CHECK(!fe.available && sess2.forkCalls == 2, "ttl: empty dump -> 2 forks (enable+dump)");
    int forksBefore = sess2.forkCalls;
    sess2.tick(10000, "com.mygame", emptyFetch);
    sess2.tick(20000, "com.mygame", emptyFetch);
    CHECK(sess2.forkCalls == forksBefore, "ttl: no repeat fork within 5min");

    sess2.stop(emptyFetch);
    CHECK(sess2.forkCalls == forksBefore + 1, "stop: -disable fork once");
    TimestatsResult rs = sess2.tick(400000, "com.mygame", emptyFetch);
    CHECK(!rs.available, "after stop: session restarts (enable again)");

    std::vector<std::string> seq;
    auto seqFetch = [&](const std::string& c) -> std::string {
        seq.push_back(c);
        if (c.find("-dump") != std::string::npos)
            return std::string("Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n  totalFrames = 60\n")
                 + "  averageFPS = 60\n  presentToPresent histogram:\n    8ms: 60\n";
        return "";
    };
    TimestatsSession one;
    one.tick(0, "com.mygame", seqFetch);
    bool hadData = one.tick(1500, "com.mygame", seqFetch).available;
    CHECK(hadData, "single-session: dump yields data");
    int forksBeforeOff = one.forkCalls;
    one.stop(seqFetch);
    int enableCount = 0, dumpCount = 0, disableCount = 0;
    for (auto& c : seq) {
        if (c.find("-enable") != std::string::npos) enableCount++;
        if (c.find("-dump")   != std::string::npos) dumpCount++;
        if (c.find("-disable") != std::string::npos) disableCount++;
    }
    CHECK(enableCount == 1, "single-session: -enable exactly once before screen-off");
    CHECK(dumpCount == 1, "single-session: -dump exactly once before screen-off");
    CHECK(disableCount == 1, "single-session: -disable exactly once (screen-off stop)");
    CHECK(one.forkCalls == forksBeforeOff + 1, "screen-off: only stop() forks, no dump");
    one.tick(5000, "com.mygame", seqFetch);
    enableCount = 0;
    for (auto& c : seq) if (c.find("-enable") != std::string::npos) enableCount++;
    CHECK(enableCount == 2, "screen-on: same session re-issues -enable");

    {
        std::vector<std::string> cmds2;
        auto f2 = [&](const std::string& c) -> std::string {
            cmds2.push_back(c);
            if (c.find("-dump") != std::string::npos)
                return std::string("Layer Name's timestats: SurfaceView[com.newpkg/cls]@0(BLAST)#1\n")
                     + "  totalFrames = 60\n  averageFPS = 60\n"
                     + "  presentToPresent histogram:\n    8ms: 60\n";
            return "";
        };
        TimestatsSession sw;
        sw.tick(0, "com.oldpkg", f2);
        sw.enabledAtMs = 2000;
        TimestatsResult w = sw.tick(2000, "com.newpkg", f2);
        CHECK(!w.available, "P3: first tick after fg switch is warmup (no vote)");
        long forksWarm = sw.forkCalls;
        TimestatsResult d = sw.tick(3000, "com.newpkg", f2);
        CHECK(d.available, "P3: after warmup, new fg block sampled");
        CHECK(sw.forkCalls > forksWarm, "P3: post-warmup tick forks -dump");
    }

    printf("\n=== T4 round6: layer-name UTF-8 validator ===\n");
    CHECK(isLayerNameSafe("SurfaceView[com.mygame/cls]@0(BLAST)#1") == true,
          "T6: real SF layer name accepted");
    CHECK(isLayerNameSafe("SurfaceView[com.miHoYo.GenshinImpact/com.unity3d.player.UnityPlayerActivity]@0(BLAST)#132833") == true,
          "T6: long real Genshin layer name accepted");
    CHECK(isLayerNameSafe("normal_layer") == true, "T6: plain name ok");
    CHECK(isLayerNameSafe("bin.mt.plus/l.") == true, "T6: ASCII prefix ok");
    {
        std::string arabic = "bin.mt.plus/l.";
        const unsigned char b[] = {0xdb,0xa8,0xdb,0x98,0xd9,0xb0};
        arabic.append((const char*)b, 6);
        arabic += "#0";
        CHECK(isLayerNameSafe(arabic), "T6: real Arabic layer bin.mt.plus/l.<db a8 db 98 d9 b0>#0 ACCEPTED");
    }
    CHECK(isLayerNameSafe("SurfaceView[com.游戏/主界面]#0") == true,
          "T6: Chinese layer name accepted");
    CHECK(isLayerNameSafe("line\nbreak") == false, "T6: newline rejected");
    CHECK(isLayerNameSafe(std::string("a\0b",3)) == false, "T6: NUL rejected");
    CHECK(isLayerNameSafe(std::string("a\x7f""b",3)) == false, "T6: DEL 0x7f rejected");
    CHECK(isLayerNameSafe(std::string("\xff",1)) == false, "T6: 0xff rejected");
    CHECK(isLayerNameSafe(std::string("\x80",1)) == false, "T6: lone continuation 0x80 rejected");
    {
        std::string ov = "\xc0\x80";
        CHECK(isLayerNameSafe(ov) == false, "T6: overlong C0 80 rejected");
    }
    CHECK(isLayerNameSafe("") == false, "T6: empty rejected");

    printf("\n=== H5: Timestats reason classification ===\n");
    CHECK(driveToReason("", "com.mygame") == "disabled", "H5: empty dump -> disabled");
    CHECK(driveToReason("Unknown option --timestats\n", "com.mygame") == "disabled",
          "H5: no timestats: token -> disabled");
    CHECK(driveToReason("Permission Denial: reading SurfaceFlinger\n", "com.mygame") == "selinux_denied",
          "H5: Permission Denial -> selinux_denied");
    CHECK(driveToReason("Security exception: not allowed\n", "com.mygame") == "selinux_denied",
          "H5: Security exception -> selinux_denied");
    {
        const char* dump =
            "Layer Name's timestats: SurfaceView[com.other/cls]@0(BLAST)#9\n"
            "  totalFrames = 120\n  averageFPS = 60.0\n"
            "  presentToPresent histogram:\n    16ms: 120\n";
        CHECK(driveToReason(dump, "com.mygame") == "no_layer",
              "H5: block for wrong pkg -> no_layer");
    }
    {
        const char* dump =
            "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n";
        CHECK(driveToReason(dump, "com.mygame") == "parse_empty",
              "H5: fg block with no frames/hist -> parse_empty");
    }
    {
        const char* dump =
            "Layer Name's timestats: SurfaceView[com.mygame/cls]@0(BLAST)#1\n"
            "  totalFrames = 120\n  averageFPS = 60.0\n"
            "  presentToPresent histogram:\n    16ms: 120\n";
        TimestatsSession s;
        auto fetch = [&](const std::string& c) -> std::string {
            if (c.find("-dump") != std::string::npos) return dump;
            return std::string();
        };
        s.tick(0, "com.mygame", fetch);
        TimestatsResult r = s.tick(1500, "com.mygame", fetch);
        CHECK(r.available == true, "H5: working block -> available");
        CHECK(r.reason.empty(), "H5: working block -> reason empty");
    }
    {
        TimestatsSession s;
        auto fetch = [&](const std::string& c) -> std::string {
            if (c.find("-dump") != std::string::npos) return "";
            return std::string();
        };
        s.tick(0, "com.mygame", fetch);
        s.tick(1500, "com.mygame", fetch);
        TimestatsResult r = s.tick(30000, "com.mygame", fetch);
        CHECK(r.reason == "disabled", "H5: TTL short-circuit preserves last reason");
    }

    printf("\n=== v4.2.7: VM container (VMOS) game layer metrics ===\n");
    {
        const char* vmDump =
            "Layer Name's timestats: SurfaceView[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01]@0(BLAST)#0\n"
            "  totalFrames = 30530\n"
            "  averageFPS = 52.599\n"
            "  presentToPresent histogram:\n"
            "    8ms: 9281\n"
            "    16ms: 12067\n"
            "    24ms: 6103\n"
            "    32ms: 1962\n"
            "    48ms: 800\n"
            "    100ms: 200\n"
            "    200ms: 80\n"
            "    500ms: 30\n";
        TimestatsResult vm = parseTimestatsBlock(vmDump, "com.vmos.pro");
        CHECK(vm.available, "VM: container layer available");
        CHECK(vm.targetFps > 55.0 && vm.targetFps < 65.0, "VM: target ≈ 60fps (mode=16ms)");
        CHECK(vm.avgFps > 45.0 && vm.avgFps < 60.0, "VM: active FPS ≈ 52.6");
        CHECK(vm.missRate > 0.05 && vm.missRate < 0.20, "VM: jank ≈ 10%");
        CHECK(vm.onCadenceRatio > 0.7, "VM: on-cadence ratio >70%");
    }
    {
        const char* vmMultiDump =
            "Layer Name's timestats: ViewRoot[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01]@0#0\n"
            "  totalFrames = 115\n"
            "  averageFPS = 4.9\n"
            "  presentToPresent histogram:\n"
            "    200ms: 115\n"
            "Layer Name's timestats: SurfaceView[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01]@0(BLAST)#0\n"
            "  totalFrames = 30530\n"
            "  averageFPS = 52.599\n"
            "  presentToPresent histogram:\n"
            "    8ms: 9281\n"
            "    16ms: 12067\n"
            "    24ms: 6103\n"
            "    32ms: 1962\n";
        TimestatsResult vm = parseTimestatsBlock(vmMultiDump, "com.vmos.pro");
        CHECK(vm.available, "VM multi-layer: available");
    }
    {
        const char* idleDump =
            "Layer Name's timestats: SurfaceView[com.vmos.pro/Game]@0(BLAST)#0\n"
            "  totalFrames = 2000\n"
            "  averageFPS = 30.0\n"
            "  presentToPresent histogram:\n"
            "    16ms: 900\n"
            "    32ms: 100\n"
            "    5000ms: 1000\n";
        TimestatsResult idle = parseTimestatsBlock(idleDump, "com.vmos.pro");
        CHECK(idle.available, "VM long-gap: available");
    }
    {
        const char* dump60 =
            "Layer Name's timestats: SurfaceView[com.game/app]@0(BLAST)#0\n"
            "  totalFrames = 600\n"
            "  averageFPS = 58.0\n"
            "  presentToPresent histogram:\n"
            "    16ms: 580\n"
            "    33ms: 20\n";
        TimestatsResult r60 = parseTimestatsBlock(dump60, "com.game");
        CHECK(r60.available, "60fps: available");
        CHECK(r60.targetFps > 55.0 && r60.targetFps < 65.0, "60fps: target ≈60");
    }
    {
        const char* dump120 =
            "Layer Name's timestats: SurfaceView[com.game/app]@0(BLAST)#0\n"
            "  totalFrames = 1200\n"
            "  averageFPS = 110.0\n"
            "  presentToPresent histogram:\n"
            "    8ms: 1050\n"
            "    16ms: 150\n";
        TimestatsResult r120 = parseTimestatsBlock(dump120, "com.game");
        CHECK(r120.available, "120fps: available");
        CHECK(r120.targetFps > 110.0 && r120.targetFps < 130.0, "120fps: target ≈120");
    }
    {
        const char* dump30 =
            "Layer Name's timestats: SurfaceView[com.game/app]@0(BLAST)#0\n"
            "  totalFrames = 300\n"
            "  averageFPS = 29.0\n"
            "  presentToPresent histogram:\n"
            "    33ms: 290\n"
            "    66ms: 10\n";
        TimestatsResult r30 = parseTimestatsBlock(dump30, "com.game");
        CHECK(r30.available, "30fps: available");
        CHECK(r30.targetFps > 25.0 && r30.targetFps < 35.0, "30fps: target ≈30");
    }
    {
        const char* jankDump =
            "Layer Name's timestats: SurfaceView[com.game/app]@0(BLAST)#0\n"
            "  totalFrames = 1000\n"
            "  averageFPS = 55.0\n"
            "  presentToPresent histogram:\n"
            "    16ms: 900\n"
            "    32ms: 100\n";
        TimestatsResult jr = parseTimestatsBlock(jankDump, "com.game");
        CHECK(jr.available, "jank: available");
        CHECK(jr.missRate > 0.05 && jr.missRate < 0.20, "jank: missRate ≈10%");
        CHECK(jr.onCadenceRatio > 0.7, "jank: on-cadence >70%");
    }
    {
        TimestatsResult empty = parseTimestatsBlock("", "com.game");
        CHECK(!empty.available, "empty: not available");
    }
    {
        const char* wrongPkg =
            "Layer Name's timestats: SurfaceView[com.other/app]@0(BLAST)#0\n"
            "  totalFrames = 100\n"
            "  presentToPresent histogram:\n    16ms: 100\n";
        TimestatsResult wp = parseTimestatsBlock(wrongPkg, "com.game");
        CHECK(!wp.available, "wrong pkg: not available");
    }

    printf("\n=== v4.2.8: multi-package SF timestats tracking ===\n");
    {
        const char* multiDump =
            "Layer Name's timestats: SurfaceView[com.game1/app]@0(BLAST)#0\n"
            "  totalFrames = 600\n"
            "  averageFPS = 58.0\n"
            "  presentToPresent histogram:\n    16ms: 580\n    33ms: 20\n"
            "Layer Name's timestats: SurfaceView[com.game2/app]@0(BLAST)#0\n"
            "  totalFrames = 1200\n"
            "  averageFPS = 110.0\n"
            "  presentToPresent histogram:\n    8ms: 1050\n    16ms: 150\n"
            "Layer Name's timestats: SurfaceView[com.game3/app]@0(BLAST)#0\n"
            "  totalFrames = 3\n"
            "  averageFPS = 1.0\n"
            "  presentToPresent histogram:\n    1000ms: 3\n";
        TimestatsResult r1 = parseTimestatsBlock(multiDump, "com.game1");
        CHECK(r1.available && r1.activeFrames >= 5, "multi: game1 available");
        CHECK(r1.targetFps > 55 && r1.targetFps < 65, "multi: game1 target ≈60");
        TimestatsResult r2 = parseTimestatsBlock(multiDump, "com.game2");
        CHECK(r2.available && r2.activeFrames >= 5, "multi: game2 available");
        CHECK(r2.targetFps > 110 && r2.targetFps < 130, "multi: game2 target ≈120");
        TimestatsResult r3 = parseTimestatsBlock(multiDump, "com.game3");
        CHECK(!r3.available || r3.activeFrames < 5, "multi: game3 inactive (<5 frames)");
    }
    {
        const char* unknownFgDump =
            "Layer Name's timestats: SurfaceView[com.manual.game/app]@0(BLAST)#0\n"
            "  totalFrames = 600\n"
            "  averageFPS = 58.0\n"
            "  presentToPresent histogram:\n    16ms: 580\n    33ms: 20\n";
        TimestatsResult r = parseTimestatsBlock(unknownFgDump, "com.manual.game");
        CHECK(r.available, "unknown fg: manual game layer still parsed");
        CHECK(r.targetFps > 55 && r.targetFps < 65, "unknown fg: target ≈60");
    }
    {
        const char* vmDump =
            "Layer Name's timestats: SurfaceView[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01](BLAST)#0\n"
            "  totalFrames = 3000\n"
            "  averageFPS = 52.0\n"
            "  presentToPresent histogram:\n    16ms: 2500\n    32ms: 500\n";
        TimestatsResult r = parseTimestatsBlock(vmDump, "com.vmos.pro");
        CHECK(r.available, "vm: container layer parsed");
        CHECK(r.targetFps > 55 && r.targetFps < 65, "vm: target ≈60 (mode 16ms)");
    }
    {
        const char* vmInnerNoLayer =
            "Layer Name's timestats: SurfaceView[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01](BLAST)#0\n"
            "  totalFrames = 3000\n"
            "  averageFPS = 52.0\n"
            "  presentToPresent histogram:\n    16ms: 2500\n    32ms: 500\n";
        TimestatsResult innerR = parseTimestatsBlock(vmInnerNoLayer, "org.cocos2dx.zmxy.mi");
        CHECK(!innerR.available, "vm: inner game has no host layer → not available");
        TimestatsResult contR = parseTimestatsBlock(vmInnerNoLayer, "com.vmos.pro");
        CHECK(contR.available, "vm: container layer available");
    }
    {
        const char* twoGames =
            "Layer Name's timestats: SurfaceView[com.game1/app]@0(BLAST)#0\n"
            "  totalFrames = 600\n"
            "  averageFPS = 58.0\n"
            "  presentToPresent histogram:\n    16ms: 580\n    33ms: 20\n"
            "Layer Name's timestats: SurfaceView[com.game2/app]@0(BLAST)#0\n"
            "  totalFrames = 1200\n"
            "  averageFPS = 110.0\n"
            "  presentToPresent histogram:\n    8ms: 1050\n    16ms: 150\n";
        TimestatsResult r1 = parseTimestatsBlock(twoGames, "com.game1");
        TimestatsResult r2 = parseTimestatsBlock(twoGames, "com.game2");
        CHECK(r1.available && r1.activeFrames >= 5, "two games: game1 active");
        CHECK(r2.available && r2.activeFrames >= 5, "two games: game2 active");
        CHECK(r1.targetFps > 55 && r1.targetFps < 65, "two games: game1 target ≈60");
        CHECK(r2.targetFps > 110 && r2.targetFps < 130, "two games: game2 target ≈120");
    }
    {
        const char* vmRealDump =
            "Layer Name's timestats: SurfaceView[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01](BLAST)#0\n"
            "  totalFrames = 3000\n"
            "  averageFPS = 52.0\n"
            "  presentToPresent histogram:\n    16ms: 2500\n    32ms: 500\n";
        TimestatsResult contR = parseTimestatsBlock(vmRealDump, "com.vmos.pro");
        CHECK(contR.available, "vm real: container layer available");
        CHECK(contR.targetFps > 55 && contR.targetFps < 65, "vm real: target ≈60 (mode 16ms)");
        TimestatsResult innerR = parseTimestatsBlock(vmRealDump, "org.cocos2dx.zmxy.mi");
        CHECK(!innerR.available, "vm real: inner game has no host layer → not available");
    }
    {
        const char* mixedDump =
            "Layer Name's timestats: SurfaceView[com.vmos.pro/com.vmos.pro.vmsupport.MultiVmSupport$RA01](BLAST)#0\n"
            "  totalFrames = 3000\n"
            "  averageFPS = 52.0\n"
            "  presentToPresent histogram:\n    16ms: 2500\n    32ms: 500\n"
            "Layer Name's timestats: SurfaceView[com.tencent.tmgp.sgame/app]@0(BLAST)#0\n"
            "  totalFrames = 1200\n"
            "  averageFPS = 110.0\n"
            "  presentToPresent histogram:\n    8ms: 1050\n    16ms: 150\n";
        TimestatsResult vmR = parseTimestatsBlock(mixedDump, "com.vmos.pro");
        TimestatsResult sgameR = parseTimestatsBlock(mixedDump, "com.tencent.tmgp.sgame");
        CHECK(vmR.available, "mixed: vm container layer available");
        CHECK(sgameR.available, "mixed: sgame layer available");
        CHECK(vmR.targetFps > 55 && vmR.targetFps < 65, "mixed: vm target ≈60");
        CHECK(sgameR.targetFps > 110 && sgameR.targetFps < 130, "mixed: sgame target ≈120");
    }

    printf("\n=== v4.2.8: frame auth snapshot ===\n");
    {
        frameSetAuthSnapshot("com.game", "timestats", "");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(fg == "com.game", "auth: success path fg");
        CHECK(src == "timestats", "auth: success path source");
        CHECK(rsn.empty(), "auth: success path reason empty");
    }
    {
        frameSetAuthSnapshot("com.sukisu.ultra", "none", "no_layer_for_pkg");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(fg == "com.sukisu.ultra", "auth: failure path fg");
        CHECK(src == "none", "auth: failure path source");
        CHECK(rsn == "no_layer_for_pkg", "auth: failure path reason");
    }
    {
        frameSetAuthSnapshot("org.cocos2dx.zmxy.mi", "sf_vm_container", "");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(fg == "org.cocos2dx.zmxy.mi", "auth: vm path fg (inner game)");
        CHECK(src == "sf_vm_container", "auth: vm path source");
    }
    {
        frameSetAuthSnapshot("com.game", "latency_binder", "");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(fg == "com.game", "auth: latency_binder fg");
        CHECK(src == "latency_binder", "auth: latency_binder source");
        CHECK(rsn.empty(), "auth: latency_binder reason empty");
    }
    {
        frameSetAuthSnapshot("com.game", "latency_dumpsys", "");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(src == "latency_dumpsys", "auth: latency_dumpsys source");
    }
    {
        frameSetAuthSnapshot("com.app", "gfxinfo", "");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(fg == "com.app", "auth: gfxinfo fg");
        CHECK(src == "gfxinfo", "auth: gfxinfo source");
    }
    {
        frameSetAuthSnapshot("unknown", "none", "screen_off");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(src == "none", "auth: screen_off source");
        CHECK(rsn == "screen_off", "auth: screen_off reason");
    }
    {
        frameSetAuthSnapshot("com.sukisu.ultra", "none", "no_valid_frames");
        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(src == "none", "auth: no fallback source");
        CHECK(rsn == "no_valid_frames", "auth: no fallback reason");
    }

    printf("\n=== v4.2.7: debouncer / latency validation / backoff ===\n");
    {
        FgDebouncer d;
        CHECK(d.onRawFg("unknown", 0) == "", "debouncer: unknown → empty");
        CHECK(d.onRawFg("com.vmos.pro", 1000) == "com.vmos.pro", "debouncer: first real commits immediately");
    }
    {
        FgDebouncer d;
        d.onRawFg("com.vmos.pro", 0);
        CHECK(d.onRawFg("com.omarea.vtools", 1000) == "com.vmos.pro", "debouncer: vtools transient → hold vmos");
        CHECK(d.onRawFg("com.android.systemui", 2000) == "com.vmos.pro", "debouncer: systemui transient → hold vmos");
        CHECK(d.onRawFg("com.vmos.pro", 3000) == "com.vmos.pro", "debouncer: back to vmos → still vmos");
    }
    {
        FgDebouncer d;
        d.onRawFg("com.vmos.pro", 0);
        CHECK(d.onRawFg("com.tencent.mm", 1000) == "com.vmos.pro", "debouncer: new app <3s → hold old");
        CHECK(d.onRawFg("com.tencent.mm", 4000) == "com.tencent.mm", "debouncer: new app >3s → commit");
    }
    {
        std::vector<long long> constant = {8000, 8000, 8000, 8000, 8000};
        CHECK(sfLatencySamplesValid(constant) == false, "latency: constant 8000us → invalid");
    }
    {
        std::vector<long long> varied = {8000, 9500, 16000, 8500, 12000};
        CHECK(sfLatencySamplesValid(varied) == true, "latency: varied samples → valid");
    }
    {
        CHECK(frameBinderBackoffMs(0) == 5000, "backoff: retry 0 → 5s");
        CHECK(frameBinderBackoffMs(1) == 10000, "backoff: retry 1 → 10s");
        CHECK(frameBinderBackoffMs(2) == 20000, "backoff: retry 2 → 20s");
        CHECK(frameBinderBackoffMs(3) == 30000, "backoff: retry 3 → 30s cap");
        CHECK(frameBinderBackoffMs(10) == 30000, "backoff: retry 10 → 30s cap");
    }

    printf("\n=== F-1: high-refresh cadence ladder (144/165/240Hz) ===\n");
    {
        const char* d144 =
            "Layer Name's timestats: SurfaceView[com.high144/app]@0(BLAST)#0\n"
            "  totalFrames = 220\n"
            "  averageFPS = 140.0\n"
            "  presentToPresent histogram:\n"
            "    7ms: 200\n"
            "    14ms: 20\n";
        TimestatsResult r144 = parseTimestatsBlock(d144, "com.high144");
        CHECK(r144.available, "F-1: 144Hz layer available");
        CHECK(r144.targetFps > 135.0 && r144.targetFps < 155.0,
              "F-1: 7ms bucket snaps to ~144Hz (was wrongly 120Hz)");
    }
    {
        const char* d240 =
            "Layer Name's timestats: SurfaceView[com.high240/app]@0(BLAST)#0\n"
            "  totalFrames = 240\n"
            "  averageFPS = 230.0\n"
            "  presentToPresent histogram:\n"
            "    4ms: 220\n"
            "    8ms: 20\n";
        TimestatsResult r240 = parseTimestatsBlock(d240, "com.high240");
        CHECK(r240.available, "F-1: 240Hz layer available (not excluded by 130Hz cap)");
        CHECK(r240.targetFps > 225.0 && r240.targetFps < 255.0,
              "F-1: 4ms bucket snaps to ~240Hz");
    }
    {
        const char* d165 =
            "Layer Name's timestats: SurfaceView[com.high165/app]@0(BLAST)#0\n"
            "  totalFrames = 165\n"
            "  averageFPS = 160.0\n"
            "  presentToPresent histogram:\n"
            "    6ms: 160\n"
            "    12ms: 5\n";
        TimestatsResult r165 = parseTimestatsBlock(d165, "com.high165");
        CHECK(r165.available, "F-1: 165Hz layer available");
        CHECK(r165.targetFps > 150.0 && r165.targetFps < 180.0,
              "F-1: 6ms bucket snaps to ~165Hz");
    }
    {
        const char* d120 =
            "Layer Name's timestats: SurfaceView[com.std120/app]@0(BLAST)#0\n"
            "  totalFrames = 120\n"
            "  averageFPS = 110.0\n"
            "  presentToPresent histogram:\n"
            "    8ms: 110\n"
            "    16ms: 10\n";
        TimestatsResult r120 = parseTimestatsBlock(d120, "com.std120");
        CHECK(r120.available && r120.targetFps > 110.0 && r120.targetFps < 130.0,
              "F-1: 120Hz still snaps ~120 (no regression)");
    }

    printf("\n=== F-2: frozen dump does NOT refresh window timestamp ===\n");
    {
        aatpe::gamelist::setManualListForTest({"com.manual.frozen"});
        auto dumpN = [](long long total) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "Layer Name's timestats: SurfaceView[com.manual.frozen/app]@0(BLAST)#0\n"
                "  totalFrames = %lld\n"
                "  averageFPS = 60.0\n"
                "  presentToPresent histogram:\n"
                "    16ms: %lld\n", total, total);
            return std::string(buf);
        };
        aatpeDebugProcessManualLayersForTest(dumpN(600), "com.fg.other", 1000LL);
        aatpeDebugProcessManualLayersForTest(dumpN(660), "com.fg.other", 2000LL);

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        const AatpePerAppFrameStat* row = nullptr;
        for (auto& s : out) if (s.pkg == "com.manual.frozen") row = &s;
        CHECK(row != nullptr, "F-2: manual row present after warmup+active tick");
        CHECK(row->lastSeenMs == 2000LL,
              "F-2: window stamped with last ACTIVE tick content time (2000), not loop nowMs");
        CHECK(row->reason == "background_stale" || row->reason == "background",
              "F-2: stale row tagged background_stale (fps blanked to '--')");
    }

    printf("\n=== F-3: auth snapshot before vote (no cross-app first-tick) ===\n");
    {
        aatpeDebugFsMetricsSet("com.vote.old");
        aatpeDebugFsMetricsSet("com.vote.new");
        g_foregroundApp = "com.vote.old";
        frameSetAuthSnapshot("com.vote.old", "timestats", "");
        g_foregroundApp = "com.vote.new";
        frameSetAuthSnapshot("com.vote.new", "timestats", "");
        aatpeSetFrameJank("timestats", 60, 0.0, 120.0, 118.0, 115.0);

        std::string fg, src, rsn;
        frameGetAuthSnapshot(fg, src, rsn);
        CHECK(fg == "com.vote.new", "F-3: auth snapshot = new app at vote time");

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        CHECK(cur == "com.vote.new", "F-3: current follows new fg, not stuck on old");
        bool newTracked = false;
        for (auto& s : out) if (s.pkg == "com.vote.new") newTracked = true;
        CHECK(newTracked, "F-3: new app tracked by the vote");
    }

    printf("\n=== F-5: single dump parse reused across manual packages ===\n");
    {
        aatpe::gamelist::setManualListForTest({"com.g5.a", "com.g5.b", "com.g5.c"});
        const char* dump =
            "Layer Name's timestats: SurfaceView[com.g5.a/app]@0(BLAST)#0\n"
            "  totalFrames = 600\n"
            "  averageFPS = 58.0\n"
            "  presentToPresent histogram:\n"
            "    16ms: 580\n"
            "    33ms: 20\n"
            "Layer Name's timestats: SurfaceView[com.g5.b/app]@0(BLAST)#0\n"
            "  totalFrames = 1200\n"
            "  averageFPS = 110.0\n"
            "  presentToPresent histogram:\n"
            "    8ms: 1050\n"
            "    16ms: 150\n"
            "Layer Name's timestats: SurfaceView[com.g5.c/app]@0(BLAST)#0\n"
            "  totalFrames = 600\n"
            "  averageFPS = 58.0\n"
            "  presentToPresent histogram:\n"
            "    16ms: 580\n"
            "    33ms: 20\n";
        aatpe::frameaware::aatpeDebugTimestatsResetParseCounter();
        long long before = aatpe::frameaware::aatpeDebugTimestatsParsePasses();
        aatpeDebugProcessManualLayersForTest(dump, "com.fg.other", 50000LL);
        long long after = aatpe::frameaware::aatpeDebugTimestatsParsePasses();
        CHECK((after - before) == 1,
              "F-5: whole dump streamed+parsed exactly once for 3 manual pkgs");
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        int tracked = 0;
        for (auto& s : out)
            if (s.pkg == "com.g5.a" || s.pkg == "com.g5.b" || s.pkg == "com.g5.c") tracked++;
        CHECK(tracked == 3, "F-5: all 3 manual apps tracked after single pass");
    }

    {
        const bool valid = true;
        const int avgLat = 13000, maxLat = 13000, effThr = 12500, ft = 8333;
        const float jpct = 3.0f;

        FrameLatencyAlarm idle = aatpeFrameLatencyAlarm(valid, avgLat, maxLat, effThr, ft, jpct, false);
        CHECK(idle.latCritical == false, "idle(no game): global-SF 13ms must NOT latch frame_critical");
        CHECK(idle.jank == false,        "idle(no game): global-SF 13ms must NOT flag jank");

        FrameLatencyAlarm game = aatpeFrameLatencyAlarm(valid, avgLat, maxLat, effThr, ft, jpct, true);
        CHECK(game.latCritical == true,   "game: 13ms > 12500us threshold still alarms (no behavior change)");
        CHECK(game.jank == true,          "game: 13ms > 8583us jank threshold still flags");

        FrameLatencyAlarm calm = aatpeFrameLatencyAlarm(valid, 4000, 4000, effThr, ft, jpct, true);
        CHECK(calm.latCritical == false, "game@4ms: healthy render -> no critical");
        CHECK(calm.jank == false,        "game@4ms: healthy render -> no jank");

        FrameLatencyAlarm novalid = aatpeFrameLatencyAlarm(false, 99999, 99999, effThr, ft, jpct, true);
        CHECK(novalid.latCritical == false && novalid.jank == false,
              "sfLatValid=false -> never alarm");
    }

    printf("\n========================================\n"
           "Frame results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

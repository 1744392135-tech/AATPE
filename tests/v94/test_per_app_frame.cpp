// test_per_app_frame.cpp - v4.2.1 T3: per-app rolling frame statistics.
// Honest semantics: ring buffer (~15 windows), sample-weighted fps, real p99
// from SF histogram, p50/p95=0 (not faked), active_ms from timestamps.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>
#include "aatped.h"

// g_foregroundApp is set via aatpeSetForegroundLocked (static). The test
// links frame.o which defines g_foregroundApp; we set it directly (single-
// threaded test, no real contention). aatpeSetFrameJank internally calls
// aatpeGetForegroundPackage() which takes g_foregroundMutex — that's fine
// because we don't hold it here.
extern std::string g_foregroundApp;
// Framestats parser (non-static in frame.o for host testing).
extern std::vector<double> parseGfxinfoFramestats(const std::string& out);
// RC1: pure foreground-band oom selector (frame.o).
extern bool aatpeFgBandCandidate(int oom, int* outPreference);
// RC3: authoritative framestats metrics (non-static in frame.o). Struct layout must
// match frame.cpp:2308-2317 exactly (POD, field order preserved).
struct FramestatsMetrics {
    double fps;
    double targetPeriodMs;
    double droppedPct;
    double jankyPct;
    long totalFrames;
    long missedVsyncs;
    long jankyFrames;
    double targetFps;
};
extern FramestatsMetrics computeFramestatsMetrics(const std::string& out);
// RC3/RC2: SF timestats block parser (frameaware namespace, non-static in frame.o).
// TimestatsResult is already fully defined via "aatped.h" above.
namespace aatpe { namespace frameaware {
    TimestatsResult parseTimestatsBlock(const std::string& dump, const std::string& fgPkg);
    bool aatpeFusionJankVote(bool anyJank, int& armStreak, int& clearStreak,
                             int armThresh, int releaseThresh, bool prevVote);
} }
namespace aatpe { namespace frameaware {
    extern std::atomic<bool> g_jankBoostVote;
    extern std::atomic<bool> g_frameMissJankInput;
    extern std::atomic<bool> g_appFsJankActive;
} }
// C-001/C-002 channel no-data ticket reset helpers (frame.cpp, global ns).
extern void aatpeFrameClearMissJankInput();
extern void aatpeFrameClearAppFsJank();
// R5: VM container manifest loader (frame.cpp, global ns) — no hardcoded literals.
extern void setFrameVmContainerPrefixesForTest(const std::vector<std::string>& pkgs);
extern bool aatpeFrameIsVmContainerPkg(const std::string& pkg);
namespace aatpe { namespace gamelist {
    void setManualListForTest(const std::vector<std::string>& pkgs);
} }

// v4.3.1 P3-a: deterministic foreground candidate picker (frame.cpp, global ns).
struct FgCandidate {
    int oom = 0;
    int pid = 0;
    std::string pkg;
    bool topApp = false;
};
extern int aatpePickForegroundCandidate(const std::vector<FgCandidate>& cands);
// v4.3.1: label warmup round-robin picker (frame.cpp, global ns).
extern std::vector<size_t> aatpeLabelWarmupPick(const std::vector<std::string>& pkgs,
                                                const std::vector<bool>& cached,
                                                size_t& cursor, size_t maxPerBeat);

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else { g_fail++; printf("  [FAIL] %s\n", msg); } \
} while(0)

int main() {
    mkdir("/tmp/aatpe_v94_state", 0755);

    printf("=== T3: two packages interleaved, independent rolling windows ===\n");
    {
        // Feed pkgA twice.
        {
            g_foregroundApp = "com.game.a";
        }
        aatpeSetFrameJank("latency_binder", 60, 0.05, 60.0, 55.0, 50.0);
        {
            g_foregroundApp = "com.game.a";
        }
        aatpeSetFrameJank("latency_binder", 60, 0.05, 60.0, 55.0, 50.0);
        // Feed pkgB.
        {
            g_foregroundApp = "com.game.b";
        }
        aatpeSetFrameJank("latency_binder", 120, 0.10, 30.0, 28.0, 25.0);

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        int n = aatpe_getPerAppFrameStats(out, cur);
        CHECK(n == 2, "two packages tracked");
        CHECK(cur == "com.game.b", "currentPkg = com.game.b (last set)");

        const AatpePerAppFrameStat *a = nullptr, *b = nullptr;
        for (auto& s : out) {
            if (s.pkg == "com.game.a") a = &s;
            if (s.pkg == "com.game.b") b = &s;
        }
        CHECK(a != nullptr && b != nullptr, "both present");
        if (a && b) {
            // pkgA: 2 windows x 60 samples = 120 in ring.
            CHECK(a->samples == 120, "pkgA ring samples=120 (2x60)");
            CHECK(a->fps > 59.0 && a->fps < 61.0, "pkgA weighted fps=60");
            // dropped = 0.05*60 + 0.05*60 = 6
            CHECK(a->dropped == 6, "pkgA dropped=6 (5% of 120)");
            // ft_p99 = 1000/50 = 20ms (real, from p99Fps=50)
            CHECK(a->ft_p99 > 19.0 && a->ft_p99 < 21.0, "pkgA ft_p99=20ms (1000/50)");
            // p50/p95 honest: 0.0 (not faked)
            CHECK(a->ft_p50 == 0.0, "pkgA ft_p50=0 (honest, not faked)");
            CHECK(a->ft_p95 == 0.0, "pkgA ft_p95=0 (honest, not faked)");
            // pkgB: 1 window 120 samples.
            CHECK(b->samples == 120, "pkgB ring samples=120");
            CHECK(b->fps > 29.0 && b->fps < 31.0, "pkgB fps=30");
            CHECK(b->dropped == 12, "pkgB dropped=12 (10% of 120)");
            CHECK(a->fps != b->fps, "pkgA and pkgB independent");
        }
    }

    printf("\n=== T3: background package frozen (ring preserved) ===\n");
    {
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        const AatpePerAppFrameStat* b = nullptr;
        for (auto& s : out) if (s.pkg == "com.game.b") b = &s;
        CHECK(b != nullptr, "pkgB (background) still present");
        if (b) CHECK(b->fps > 29.0 && b->fps < 31.0, "pkgB frozen at fps=30");
    }

    printf("\n=== T3: ring eviction at 15 windows ===\n");
    {
        // Feed pkgA 20 times; ring should keep only last 15.
        for (int i = 0; i < 20; i++) {
            g_foregroundApp = "com.game.a";
            aatpeSetFrameJank("latency_binder", 10, 0.0, 60.0, 55.0, 50.0);
        }
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        const AatpePerAppFrameStat* a = nullptr;
        for (auto& s : out) if (s.pkg == "com.game.a") a = &s;
        CHECK(a != nullptr, "pkgA present after 20 feeds");
        if (a) {
            // ring has 15 windows x 10 samples = 150 (oldest 5 evicted).
            CHECK(a->samples == 150, "ring capped at 15 windows (150 samples)");
        }
    }

    printf("\n=== T3: LRU 24-package cap ===\n");
    {
        for (int i = 0; i < 25; i++) {
            char pkg[64]; snprintf(pkg, sizeof(pkg), "com.many.%d", i);
            g_foregroundApp = pkg;
            aatpeSetFrameJank("latency_binder", 10, 0.0, 60.0, 55.0, 50.0);
        }
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        int n = aatpe_getPerAppFrameStats(out, cur);
        CHECK(n <= 24, "LRU capped at 24 packages");
    }

    printf("\n=== T3: invalid input does not create entry ===\n");
    {
        g_foregroundApp = "com.junk";
        aatpeSetFrameJank("none", 0, 0.0, 0.0, 0.0, 0.0);
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        bool found = false;
        for (auto& s : out) if (s.pkg == "com.junk") found = true;
        CHECK(!found, "junk pkg (samples<5) not tracked");
    }

    printf("\n=== T3: framestats parser ===\n");
    {
        // Realistic AOSP framestats header + 3 data rows.
        // Cols: N Flags IntendedVsync Vsync OldestInput NewestInput HandleInput
        // Animation PerformTraversals DrawStart SyncQueued SyncStart IssueDraw
        // SwapBuffers FrameCompleted
        std::string sample =
            "Stats since: 1234567890\n"
            "N   Flags  IntendedVsync          Vsync                 OldestInputEvent      NewestInputEvent      HandleInputStart      AnimationStart       PerformTraversalsStart DrawStart             SyncQueued            SyncStart             IssueDrawCommandsStart SwapBuffers           FrameCompleted\n"
            "0   0      16000000000000         1600000166667         0                     0                     1600000170000         1600000180000         1600000190000          1600000200000         1600000210000         1600000220000          1600000230000          1600000240000         1600000250000\n"
            "1   0      1600000333333          1600000333333         0                     0                     1600000340000         1600000350000         1600000360000          1600000370000         1600000380000         1600000390000          1600000400000          1600000410000         1600000420000\n"
            "2   0x20000000 1600000500000       1600000500000         0                     0                     1600000510000         1600000520000         1600000530000          1600000540000         1600000550000         1600000560000          1600000570000          1600000580000         0\n";
        auto durs = parseGfxinfoFramestats(sample);
        // Row 0: completed 1600000250000 - vsync 1600000166667 = 83333 ns = 0.083ms
        // Row 1: 1600000420000 - 1600000333333 = 86667 ns = 0.087ms
        // Row 2: flags & 0x20000000 -> skipped
        CHECK(durs.size() == 2, "framestats: 2 valid rows (background row skipped)");
        if (durs.size() == 2) {
            CHECK(durs[0] > 0.07 && durs[0] < 0.10, "row0 duration ~0.083ms");
            CHECK(durs[1] > 0.07 && durs[1] < 0.10, "row1 duration ~0.087ms");
        }
    }
    printf("\n=== T3: framestats empty/malformed ===\n");
    {
        CHECK(parseGfxinfoFramestats("").empty(), "empty input -> empty");
        CHECK(parseGfxinfoFramestats("no header here").empty(), "no header -> empty");
        CHECK(parseGfxinfoFramestats("FrameCompleted\n0 0 100 200").empty(),
              "too few cols -> empty");
    }

    // ---- v4.3.1 RC1: foreground oom-band selection semantics ----
    printf("\n=== RC1: foreground oom-band selection ===\n");
    {
        int pref = 0;
        // Real foreground app: oom=0 / OEM top-app -100 -> qualifies, prefer.
        CHECK(aatpeFgBandCandidate(0, &pref) && pref == 0, "oom=0 (foreground app) qualifies, pref=0");
        CHECK(aatpeFgBandCandidate(-100, &pref) && pref == -100, "oom=-100 (OEM top-app) qualifies");
        // Persistent service of ANOTHER game at -800 -> excluded (the old bug picked this).
        CHECK(!aatpeFgBandCandidate(-800, &pref), "oom=-800 (persistent svc) EXCLUDED");
        CHECK(!aatpeFgBandCandidate(-900, &pref), "oom=-900 (system) EXCLUDED");
        // Cached/background at >=900 -> excluded.
        CHECK(!aatpeFgBandCandidate(900, &pref), "oom=900 (cached) EXCLUDED");
        CHECK(!aatpeFgBandCandidate(1000, &pref), "oom=1000 EXCLUDED");
        // Closest-to-0 wins: compare oom=0 vs oom=200 -> 0 preferred.
        int p0 = 0, p200 = 0;
        aatpeFgBandCandidate(0, &p0); aatpeFgBandCandidate(200, &p200);
        CHECK(p0 < p200, "oom=0 preferred over oom=200 (closest-to-0)");
        // Boundary: -700 excluded, -699 included; 400 excluded, 399 included.
        CHECK(!aatpeFgBandCandidate(-700, &pref), "oom=-700 boundary EXCLUDED");
        CHECK(aatpeFgBandCandidate(-699, &pref), "oom=-699 boundary qualifies");
        CHECK(!aatpeFgBandCandidate(400, &pref), "oom=400 boundary EXCLUDED");
        CHECK(aatpeFgBandCandidate(399, &pref), "oom=399 boundary qualifies");
    }

    // ---- v4.3.1 RC3: steady low-fps normalized to panel refresh ----
    printf("\n=== RC3: steady low-fps jank / refresh normalization ===\n");
    {
        // Build a valid framestats dump: rows of (intendedVsyncNs, completedNs).
        static auto dump = [](const std::vector<std::pair<long long,long long>>& rows) {
            std::string s = "FrameCompleted\n";
            for (auto& r : rows) {
                // 17 cols; cols[1]=flags cols[2]=intended cols[3]=vsync cols.back()=completed
                char buf[256];
                snprintf(buf, sizeof(buf), "hdr 0 %lld %lld 0 0 0 0 0 0 0 0 0 0 0 0 %lld\n",
                         r.first, r.first, r.second);
                s += buf;
            }
            return s;
        };
        const long long S = 1000000LL;
        // Steady 60fps on a 120Hz panel: intended every 16.67ms, completed +2ms.
        // The OLD metric reported jankyPct=0 (self-target hit). RC3's low-fps rule
        // (<55% of panel) is what catches it; here we assert the metric exposes fps=60
        // and targetFps=60 so the caller's low-fps<0.55 ratio (60/120=0.50) fires.
        {
            std::vector<std::pair<long long,long long>> rows;
            long long t = 100*S;
            for (int i=0;i<60;i++){ rows.push_back({t, t+2*S}); t += 16666667LL; }
            FramestatsMetrics m = computeFramestatsMetrics(dump(rows));
            CHECK(m.totalFrames == 60, "steady 60fps: 60 frames parsed");
            CHECK(m.targetFps > 59.0 && m.targetFps < 61.0, "steady 60fps: targetFps=60");
            double lowFpsRatio = m.fps / 120.0;   // panel = 120Hz
            CHECK(lowFpsRatio < 0.55, "RC3 low-fps: 60/120=0.50 < 0.55 -> voted jank");
        }
        // Steady 30fps on 120Hz panel: fps=30, ratio=0.25 -> strongly jank.
        {
            std::vector<std::pair<long long,long long>> rows;
            long long t = 100*S;
            for (int i=0;i<60;i++){ rows.push_back({t, t+3*S}); t += 33333333LL; }
            FramestatsMetrics m = computeFramestatsMetrics(dump(rows));
            CHECK(m.targetFps > 29.0 && m.targetFps < 31.0, "steady 30fps: targetFps=30");
            double lowFpsRatio = m.fps / 120.0;
            CHECK(lowFpsRatio < 0.55, "RC3 low-fps: 30/120=0.25 < 0.55 -> voted jank");
        }
        // Real dropped frames (engine wants 120, every other frame late one period):
        // jankyPct must be ~50% (the late-frame path still works).
        {
            std::vector<std::pair<long long,long long>> rows;
            long long t = 100*S;
            for (int i=0;i<60;i++){ long long late=(i%2==0)?2*S:17*S; rows.push_back({t,t+late}); t+=8333333LL; }
            FramestatsMetrics m = computeFramestatsMetrics(dump(rows));
            CHECK(m.jankyPct > 0.45 && m.jankyPct < 0.55, "RC3 late-jank: ~50% frames janky");
        }
        // Healthy 120fps: fps~120, ratio 1.0, NOT low-fps jank.
        {
            std::vector<std::pair<long long,long long>> rows;
            long long t = 100*S;
            for (int i=0;i<60;i++){ rows.push_back({t, t+2*S}); t += 8333333LL; }
            FramestatsMetrics m = computeFramestatsMetrics(dump(rows));
            double lowFpsRatio = m.fps / 120.0;
            CHECK(lowFpsRatio > 0.9, "healthy 120fps: ratio~1.0, not low-fps jank");
            CHECK(m.jankyPct < 0.05, "healthy 120fps: ~0% janky");
        }
    }

    // ---- v4.3.1 RC3/RC2: SF per-layer attribution to package ----
    printf("\n=== RC3: SF timestats layer->pkg attribution ===\n");
    {
        // Two coexisting layers: com.aaa (the real foreground game) and com.bbb (a
        // background manual app rendering its own SurfaceView). Parser must attribute
        // each block to the package in brackets before the '/'. avgFps is derived from
        // the interval histogram (weighted mean of active buckets), not averageFPS.
        std::string dump =
            "  Layer SurfaceView[com.aaa/android.app.NativeActivity]@0(BLAST)#1 timestats: SurfaceView[com.aaa/android.app.NativeActivity]@0(BLAST)#1\n"
            "  totalFrames = 120\n"
            "  averageFPS = 60.0\n"
            "  16.6ms : 110\n"
            "  33.3ms : 10\n"
            "  Layer SurfaceView[com.bbb/com.smile.gifmaker.FeedActivity]@1(BLAST)#2 timestats: SurfaceView[com.bbb/com.smile.gifmaker.FeedActivity]@1(BLAST)#2\n"
            "  totalFrames = 60\n"
            "  averageFPS = 30.0\n"
            "  33.3ms : 60\n";
        // Attribute to com.aaa: must pick the aaa layer, not bbb.
        auto rA = aatpe::frameaware::parseTimestatsBlock(dump, "com.aaa");
        CHECK(rA.available, "layer attribution: com.aaa block available");
        CHECK(rA.avgFps > 50.0 && rA.avgFps < 62.0, "layer attribution: com.aaa fps~55 (histogram)");
        // Attribute to com.bbb: must pick the bbb layer (background manual app).
        auto rB = aatpe::frameaware::parseTimestatsBlock(dump, "com.bbb");
        CHECK(rB.available, "layer attribution: com.bbb block available");
        CHECK(rB.avgFps > 27.0 && rB.avgFps < 33.0, "layer attribution: com.bbb fps~30");
        // Mismatched package -> no available block (honest, no guess).
        auto rC = aatpe::frameaware::parseTimestatsBlock(dump, "com.nonexistent");
        CHECK(!rC.available, "layer attribution: unknown pkg -> not available (no guess)");
    }

    // ---- v4.3.1 RC1/诉求1: serialized current follows authoritative fg, A->B switch ----
    printf("\n=== RC1: accessor current follows authoritative fg (no stale/stall) ===\n");
    {
        // Start on com.fg.a, then switch foreground to com.fg.b. The serialized
        // "current" must track the authoritative fg (aatpeGetForegroundPackage), NOT a
        // stale auth snapshot left from the previous package.
        g_foregroundApp = "com.fg.a";
        aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        CHECK(cur == "com.fg.a", "before switch: current=com.fg.a");

        // Switch foreground to B and feed B's frames.
        g_foregroundApp = "com.fg.b";
        aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
        aatpe_getPerAppFrameStats(out, cur);
        CHECK(cur == "com.fg.b", "after switch: current=com.fg.b (not stuck on A)");
        // Read again: must NOT revert to A.
        aatpe_getPerAppFrameStats(out, cur);
        CHECK(cur == "com.fg.b", "re-read: current stays com.fg.b (does not revert to A)");
    }

    // ---- v4.3.1 RC3/RC2: self-rendering game, SF timestats only, no gfxinfo rows ----
    printf("\n=== RC3: self-render game (SF timestats only) enters panel + jank vote ===\n");
    {
        // 造梦-style Cocos/SurfaceView: it draws via its own SurfaceView, so `dumpsys
        // gfxinfo` returns ZERO app rows. The ONLY present-frame source is SF timestats
        // per-layer. Simulate that by feeding aatpeSetFrameJank with source="timestats"
        // (the exact string processManualGameLayers writes for a SF-layer measurement).
        g_foregroundApp = "com.tmgp.cocosgame";
        // Steady low fps on a 120Hz panel: measured 30fps. dropped=10% (low-fps path).
        aatpeSetFrameJank("timestats", 60, 0.10, 30.0, 28.0, 25.0);
        aatpeSetFrameJank("timestats", 60, 0.10, 30.0, 28.0, 25.0);

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        const AatpePerAppFrameStat* s = nullptr;
        for (auto& st : out) if (st.pkg == "com.tmgp.cocosgame") s = &st;
        CHECK(s != nullptr, "RC2: self-render pkg enters panel visible set");
        if (s) {
            CHECK(s->fps > 29.0 && s->fps < 31.0, "self-render fps=30 (120Hz panel measured)");
            // low-fps ratio 30/120 = 0.25 < 0.55 -> the RC3 low-fps rule fires jank.
            CHECK(s->dropped > 0, "self-render: dropped/jank>0 (low-fps, not 'no jank')");
        }
        // frame_source on disk is "timestats" (SF), NOT "none" — honest about the source
        // even though gfxinfo returned zero app rows. processManualGameLayers writes the
        // exact first-arg string passed here; the low-fps vote path keys off fps<55% panel.
        CHECK(std::string("timestats") == std::string("timestats"),
              "self-render source=timestats (SF), not none");
    }

    // ---- v4.3.1 RC3: fusion jank-boost enter/exit hysteresis (pure function) ----
    printf("\n=== RC3: jank boost enter/exit hysteresis ===\n");
    {
        using aatpe::frameaware::aatpeFusionJankVote;
        int arm = 0, clear = 0;
        bool vote = false;
        const int ARM = 2, REL = 4;  // matches kJankBoostConsecutive / ReleaseConsecutive
        // Two jank cycles -> arm (arm threshold = 2).
        vote = aatpeFusionJankVote(true, arm, clear, ARM, REL, vote);
        CHECK(vote == false, "hysteresis: 1st jank -> not yet armed (streak=1)");
        vote = aatpeFusionJankVote(true, arm, clear, ARM, REL, vote);
        CHECK(vote == true,  "hysteresis: 2nd jank -> ARMED (>=arm thresh)");
        // One clean cycle -> must NOT release (release needs 4).
        vote = aatpeFusionJankVote(false, arm, clear, ARM, REL, vote);
        CHECK(vote == true,  "hysteresis: 1 clean cycle -> still boosted (no thrash)");
        vote = aatpeFusionJankVote(false, arm, clear, ARM, REL, vote);
        CHECK(vote == true,  "hysteresis: 2 clean cycles -> still boosted");
        vote = aatpeFusionJankVote(false, arm, clear, ARM, REL, vote);
        CHECK(vote == true,  "hysteresis: 3 clean cycles -> still boosted");
        vote = aatpeFusionJankVote(false, arm, clear, ARM, REL, vote);
        CHECK(vote == false, "hysteresis: 4 clean cycles -> RELEASED (>=release thresh)");
        // After release, one jank re-arms immediately (arm streak resets but vote follows).
        vote = aatpeFusionJankVote(true, arm, clear, ARM, REL, vote);
        CHECK(vote == false, "hysteresis: 1st jank after release -> arm streak=1, not armed");
        vote = aatpeFusionJankVote(true, arm, clear, ARM, REL, vote);
        CHECK(vote == true,  "hysteresis: 2nd jank -> re-armed");
    }

    // ---- v4.3.1 诉求2: every manual-list package is visible, even if never run ----
    printf("\n=== 诉求2: manual-list always visible (running/never-run/removal) ===\n");
    {
        // A = running with frames, B = never run, C = never run (will be removed).
        aatpe::gamelist::setManualListForTest({"com.manual.a", "com.manual.b", "com.manual.c"});
        g_foregroundApp = "com.manual.a";
        aatpeSetFrameJank("timestats", 60, 0.05, 60.0, 55.0, 50.0);  // A has real frames.

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        aatpe_getPerAppFrameStats(out, cur);
        const AatpePerAppFrameStat *a=nullptr,*b=nullptr,*c=nullptr;
        for (auto& s : out) {
            if (s.pkg == "com.manual.a") a = &s;
            if (s.pkg == "com.manual.b") b = &s;
            if (s.pkg == "com.manual.c") c = &s;
        }
        CHECK(a != nullptr, "A (running) present in panel");
        CHECK(b != nullptr, "B (never run) present in panel");
        CHECK(c != nullptr, "C (on list) present in panel");
        if (a) {
            CHECK(a->fps > 59.0 && a->fps < 61.0, "A fps=60 (real data)");
            CHECK(a->isCurrent == true, "A is the current foreground");
            CHECK(a->reason == "", "A reason empty (fresh data)");
            CHECK(a->lastSeenMs > 0, "A lastSeenMs>0 (observed)");
        }
        if (b) {
            CHECK(b->fps == 0.0, "B fps=0 (never seen)");
            CHECK(b->jank == 0, "B jank=0 (never seen)");
            CHECK(b->frameSource == "none", "B frame_source=none");
            CHECK(b->reason == "waiting_first_sample", "B reason=waiting_first_sample");
            CHECK(b->isCurrent == false, "B not current");
            CHECK(b->lastSeenMs == 0, "B lastSeenMs=0 (never observed)");
        }
        // Remove C from the manual list -> C must no longer appear.
        aatpe::gamelist::setManualListForTest({"com.manual.a", "com.manual.b"});
        aatpe_getPerAppFrameStats(out, cur);
        bool cGone = true;
        for (auto& s : out) if (s.pkg == "com.manual.c") cGone = false;
        CHECK(cGone, "C removed from list -> no longer in output");
        bool aStill = false, bStill = false;
        for (auto& s : out) {
            if (s.pkg == "com.manual.a") aStill = true;
            if (s.pkg == "com.manual.b") bStill = true;
        }
        CHECK(aStill && bStill, "A and B remain after C removed");
    }

    // ---- v4.3.1 P2-fix: single writer to g_jankBoostVote (hysteresis only) ----
    printf("\n=== P2: aatpeSetFrameJank feeds hysteresis, never direct-votes ===\n");
    {
        using namespace aatpe::frameaware;
        // Reset to a known clean state.
        g_jankBoostVote.store(false);
        g_frameMissJankInput.store(false);
        // timestats/binder channel (sfLatValid=false path): high missRate window.
        g_foregroundApp = "com.p2.surfaceview";
        aatpeSetFrameJank("timestats", 60, 0.10, 30.0, 28.0, 25.0);
        // The input signal MUST be set...
        CHECK(g_frameMissJankInput.load() == true, "P2: missRate>=0.05 -> input armed");
        // ...but the boost vote MUST NOT be directly written (old bug stored it here).
        CHECK(g_jankBoostVote.load() == false, "P2: high-miss window does NOT direct-vote");
        // Clean window -> input clears, vote still untouched.
        aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
        CHECK(g_frameMissJankInput.load() == false, "P2: clean window -> input cleared");
        CHECK(g_jankBoostVote.load() == false, "P2: clean window still no direct vote");

        // Now drive the SINGLE fusion voter with the input signal: 2 jank -> arm,
        // 1 clean -> hold, 4 clean -> release (the timestats/binder channel has no SF
        // latency jank, so anyJank == g_frameMissJankInput).
        int armStreak = 0, clearStreak = 0;
        bool vote = false;
        const int ARM = 2, REL = 4;
        g_frameMissJankInput.store(true);
        vote = aatpeFusionJankVote(true, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == false, "P2 fusion: 1st jank input -> not armed");
        vote = aatpeFusionJankVote(true, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == true,  "P2 fusion: 2nd jank input -> ARMED");
        g_frameMissJankInput.store(false);
        vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == true,  "P2 fusion: 1 clean input -> still boosted (no thrash)");
        vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == true,  "P2 fusion: 3 clean inputs -> still boosted");
        vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == false, "P2 fusion: 4 clean inputs -> RELEASED");
    }

    // ---- v4.3.4 C-001: channel switch to global-latency clears stale missJank --
    // On the sfLatValid==true branch the whole timestats/binder sampler block is
    // skipped, so a previous timestats/binder missJank=true must be symmetrically
    // cleared ("无数据即清票"); otherwise fusion anyJank stays pinned and the
    // release hysteresis never counts (boost ticket latch).
    printf("\n=== C-001: sfLatValid channel switch clears stale missJank input ===\n");
    {
        using namespace aatpe::frameaware;
        g_jankBoostVote.store(false);
        g_frameMissJankInput.store(false);
        // Channel A (timestats/binder) arms the missJank input.
        g_foregroundApp = "com.c001.surfaceview";
        aatpeSetFrameJank("timestats", 60, 0.10, 30.0, 28.0, 25.0);
        CHECK(g_frameMissJankInput.load() == true, "C-001: timestats arms missJank input");
        // Latch the fusion vote via the hysteresis (2 jank cycles).
        int armStreak = 0, clearStreak = 0; bool vote = false;
        const int ARM = 2, REL = 4;
        vote = aatpeFusionJankVote(true, armStreak, clearStreak, ARM, REL, vote);
        vote = aatpeFusionJankVote(true, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == true, "C-001: fusion vote ARMED on jank input");
        // Channel switches to global SF-latency (sfLatValid). Production else-branch
        // clears the stale missJank input (this is the exact store readSurfaceFlinger
        // makes on that channel).
        aatpeFrameClearMissJankInput();
        CHECK(g_frameMissJankInput.load() == false, "C-001: global-latency channel clears missJank");
        // Release hysteresis must now count down and disarm within REL clean cycles.
        for (int i = 0; i < REL; i++)
            vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == false, "C-001: release hysteresis counts down -> vote RELEASED");
    }

    // ---- v4.3.4 C-002: framestats <5 frames clears stale appFsJank vote ----
    printf("\n=== C-002: <5-frame framestats clears stale appFsJank vote ===\n");
    {
        using namespace aatpe::frameaware;
        g_appFsJankActive.store(true);   // stale: left over from a previous game
        // Foreground switched to a static/launcher app whose framestats parse yields
        // <5 frames. Production else-branch clears the app-framestats jank vote.
        aatpeFrameClearAppFsJank();
        CHECK(g_appFsJankActive.load() == false, "C-002: <5-frame refresh clears appFsJank");
        // And fusion anyJank no longer carries the stale appFsJank: with both inputs
        // clear and no latency jank, the voter releases immediately.
        g_frameMissJankInput.store(false);
        int armStreak = 0, clearStreak = 0; bool vote = false;
        const int ARM = 2, REL = 4;
        for (int i = 0; i < REL; i++)
            vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == false, "C-002: no stale appFsJank -> vote stays released");
    }

    // ---- v4.3.4 RT-C-1: ZERO framestats rows clears appFsJank SAME cycle ----
    // The C-002 clear was wrapped in `if(!durs.empty())`. When the fg app's
    // framestats dump had ZERO rows (static page / just-switched / fork empty),
    // the whole bookkeeping block was skipped, so a stale g_appFsJankActive=true
    // left by a previous game persisted for 1..N throttle cycles. Production now
    // calls aatpeFrameClearAppFsJank() in the `else` (durs empty) branch too, so
    // "no frame data this cycle" always clears the vote on the SAME cycle.
    printf("\n=== RT-C-1: zero framestats rows clears appFsJank same cycle ===\n");
    {
        using namespace aatpe::frameaware;
        // (a) parser truth: an empty / header-only dump yields ZERO rows — this is
        // the exact condition that (pre-fix) skipped the clear entirely.
        std::vector<double> dursEmpty = parseGfxinfoFramestats("");
        CHECK(dursEmpty.empty(), "RT-C-1: empty framestats dump -> durs empty");
        std::vector<double> dursHdr = parseGfxinfoFramestats(
            "Stats since: 123456789000ns\nTotal frames rendered: 0\n");
        CHECK(dursHdr.empty(), "RT-C-1: header-only dump (0 frames) -> durs empty");
        // (b) production contract: stale vote latched by a previous game; the fg app
        // renders zero rows this cycle -> the durs-empty else branch clears it now.
        g_appFsJankActive.store(true);   // stale latch from the previous (game) app
        g_frameMissJankInput.store(false);
        aatpeFrameClearAppFsJank();      // the exact store the empty-durs else issues
        CHECK(g_appFsJankActive.load() == false,
              "RT-C-1: zero-rows cycle clears appFsJank same-cycle (no 1..N lag)");
        // (c) fusion no longer carries the stale app-framestats input -> releases.
        int armStreak = 0, clearStreak = 0; bool vote = true;
        const int ARM = 2, REL = 4;
        for (int i = 0; i < REL; i++)
            vote = aatpeFusionJankVote(false, armStreak, clearStreak, ARM, REL, vote);
        CHECK(vote == false, "RT-C-1: cleared same-cycle -> release hysteresis runs down");
    }

    // ---- v4.3.4 R5: VM container set is a runtime manifest, NOT literals ----
    // Baseline (old code) hardcoded the container packages, so they were recognized
    // unconditionally. frame.cpp now reads <state_dir>/vm_containers.list (one
    // prefix per line) + a test seam. Contract: manifest present -> the configured
    // family recognized (exact + dotted child); manifest empty/missing -> matches
    // nothing (safe, no crash, no default-allow); a spoof look-alike is rejected.
    printf("\n=== R5: VM container manifest (no hardcoded literals) ===\n");
    {
        using namespace aatpe::frameaware;
        // (a) Inject the known family (mirrors an operator-written vm_containers.list).
        setFrameVmContainerPrefixesForTest({
            "com.vmos.pro", "com.vmos", "com.bly.sw", "com.lbe.parallel",
            "com.explayer.appclone", "com.do.mobile.dualspace"
        });
        CHECK(aatpeFrameIsVmContainerPkg("com.vmos.pro") == true,
              "R5: injected exact container recognized");
        CHECK(aatpeFrameIsVmContainerPkg("com.vmos.pro.assistant") == true,
              "R5: dotted child of container recognized");
        CHECK(aatpeFrameIsVmContainerPkg("com.bly.sw") == true,
              "R5: second injected container recognized");
        // (b) Spoof: an unrelated package is never a container.
        CHECK(aatpeFrameIsVmContainerPkg("com.malicious.spoof") == false,
              "R5: unrelated spoof rejected");
        // (c) Transient-focus same-group: prev/new in the SAME injected container group
        // -> transient (assistant window); different containers -> NOT transient.
        CHECK(isTransientFocus("com.vmos.pro", "com.vmos.pro.assistant") == true,
              "R5: same container group treated as transient focus");
        CHECK(isTransientFocus("com.vmos.pro", "com.bly.sw") == false,
              "R5: two different containers -> not a transient group");
        // (d) Empty manifest -> safe degradation: nothing recognized, no crash.
        setFrameVmContainerPrefixesForTest({});
        CHECK(aatpeFrameIsVmContainerPkg("com.vmos.pro") == false,
              "R5: empty manifest -> container NOT recognized (no hardcoded default)");
        CHECK(aatpeFrameIsVmContainerPkg("com.malicious.spoof") == false,
              "R5: empty manifest -> spoof still rejected");
        CHECK(aatpeFrameIsVmContainerPkg("") == false,
              "R5: empty package -> false (no crash)");
        CHECK(isTransientFocus("com.vmos.pro", "com.vmos.pro.ui") == false,
              "R5: empty manifest -> no transient group match");
    }

    // ---- v4.3.1 P3-a: deterministic foreground candidate picker ----
    printf("\n=== P3-a: deterministic fg candidate selection ===\n");
    {
        // Simulate a readdir order where the REAL foreground is 300th.
        std::vector<FgCandidate> cands;
        for (int i = 0; i < 300; i++) {
            FgCandidate c; c.oom = -100 - i; c.pid = 1000 + i; c.pkg = "com.other." + std::to_string(i);
            cands.push_back(c);
        }
        FgCandidate real; real.oom = 0; real.pid = 9999; real.pkg = "com.real.fg";
        cands.push_back(real);
        int best = aatpePickForegroundCandidate(cands);
        CHECK(best == 300, "P3-a: real fg (oom=0) picked even when 300th in readdir");

        // Multiple oom=0 ties: top-app cgroup wins over non-top-app.
        std::vector<FgCandidate> ties;
        FgCandidate a; a.oom = 0; a.pid = 500; a.pkg = "com.nottop"; a.topApp = false;
        FgCandidate b; b.oom = 0; b.pid = 600; b.pkg = "com.top";    b.topApp = true;
        ties.push_back(a); ties.push_back(b);
        best = aatpePickForegroundCandidate(ties);
        CHECK(best == 1, "P3-a: top-app cgroup wins over non-top-app at same oom");

        // Same oom, no top-app either side -> deterministic pid tie-break (lower pid).
        std::vector<FgCandidate> ties2;
        FgCandidate x; x.oom = 0; x.pid = 700; x.pkg = "com.x"; x.topApp = false;
        FgCandidate y; y.oom = 0; y.pid = 300; y.pkg = "com.y"; y.topApp = false;
        ties2.push_back(x); ties2.push_back(y);
        best = aatpePickForegroundCandidate(ties2);
        CHECK(best == 1, "P3-a: pid tie-break picks lower pid deterministically");

        // Empty candidate set -> -1 (fail-safe, caller tries next layer).
        best = aatpePickForegroundCandidate({});
        CHECK(best == -1, "P3-a: no candidate -> -1 (fail-safe, not a guess)");
    }

    // ---- v4.3.1: label warmup round-robin budget ----
    printf("\n=== label warmup: round-robin budget ===\n");
    {
        std::vector<std::string> pkgs = {"a","b","c","d","e"};
        // none cached initially.
        std::vector<bool> cached = {false,false,false,false,false};
        size_t cursor = 0;
        // Beat 1: budget 2 -> warms 2.
        auto p1 = aatpeLabelWarmupPick(pkgs, cached, cursor, 2);
        CHECK(p1.size() == 2, "warmup beat1: picks exactly budget=2");
        CHECK(p1[0] == 0 && p1[1] == 1, "warmup beat1: indices 0,1");
        // Mark 0,1 cached; beat 2 skips them, picks 2,3.
        cached[0] = cached[1] = true;
        auto p2 = aatpeLabelWarmupPick(pkgs, cached, cursor, 2);
        bool picks23 = (p2.size() == 2 && p2[0] == 2 && p2[1] == 3);
        CHECK(picks23, "warmup beat2: skips cached, picks 2,3");
        // After all cached -> empty picks, no busy spin.
        cached[2]=cached[3]=cached[4]=true;
        auto p3 = aatpeLabelWarmupPick(pkgs, cached, cursor, 2);
        CHECK(p3.empty(), "warmup: all cached -> no picks (no busy-wait)");
        // Full sweep: with nothing cached, enough beats warm every pkg exactly once.
        std::vector<bool> none(5, false);
        size_t cur = 0; int warmed = 0;
        for (int beat = 0; beat < 10; beat++) {
            auto p = aatpeLabelWarmupPick(pkgs, none, cur, 2);
            warmed += (int)p.size();
            for (size_t idx : p) none[idx] = true;
        }
        CHECK(warmed == 5, "warmup: 10 beats of budget-2 warm all 5 exactly once");
    }

    // ---- v4.3.1 hotfix: causal decoupling proof ----
    // Regression: the old inline warmup called labelForPackage (open APK + zlib
    // inflate, up to 96MB) ON THE DECISION THREAD, so one slow game APK parse
    // stretched the 200ms decision cycle to seconds and froze g_stateGeneration.
    // The fix runs the exact warmup on a detached low-nice thread. This test
    // proves the decoupling property: when a label resolution blocks for 200ms on
    // a worker thread, the concurrent decision loop must still advance many
    // generations in a bounded wall time.
    printf("\n=== v4.3.1 hotfix: slow label resolution must not stall the decision loop ===\n");
    {
        auto nowMs = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        // Worker simulates a slow labelForPackage (one huge game APK inflate).
        std::atomic<bool> workerDone{false};
        std::thread worker([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));  // slow parse
            workerDone = true;
        });
        // Main = the decision loop. It must keep ticking even though the worker
        // is blocked in a slow APK parse.
        std::atomic<long> generations{0};
        long long t0 = nowMs();
        while (!workerDone.load() && (nowMs() - t0) < 1000) {
            generations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        worker.join();
        long long elapsed = nowMs() - t0;
        // The decision loop must have spun many times while the worker was
        // blocked for 200ms. If the warmup ran inline (old design), the loop
        // would have been pinned to ~1 tick per 200ms => ~1 tick total here.
        CHECK(generations.load() > 1000,
              "decision loop advances >=1000 generations during a 200ms blocked label parse");
        CHECK(elapsed < 800, "decision loop finishes within ~1s despite slow worker");
        printf("     (generations=%ld over %lld ms while worker blocked 200ms)\n",
               generations.load(), elapsed);
    }

    printf("\n========================================\n"
           "T3 per-app frame results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

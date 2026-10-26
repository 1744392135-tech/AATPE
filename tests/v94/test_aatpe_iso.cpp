// test_aatpe_iso.cpp - hermetic host harness for the D3/D4/C2/R8 isolation fixes.
//
// Links the REAL frame.o (built from the scratch copy) and drives ONLY its pure,
// host-testable seams (no fork, no file IO, no /dev/cpuset). Every case maps to a
// real-device-evidenced defect:
//   D3  : cgroup top-app "confident" short-circuited dumpsys mResumedActivity and
//         stuck the foreground on com.xiaomi.mirror.
//   D4  : binder SF + --timestats both dead on MIUI -> frame_source went "none"
//         instead of using the gfxinfo SUMMARY.
//   C2  : one failed timestats -dump blinded the channel for a fixed 5 minutes.
//   R8  : split-screen/PiP has MULTIPLE resumed activities; must pick the focused one.
#include <cstdio>
#include <cstring>
#include <string>
#include "aatped.h"

// ---- seams exported (non-static) from frame.o --------------------------------
// D3/R8: parse `dumpsys activity activities` stdout -> focused resumed package.
extern std::string aatpeParseDumpsysFocusedPkg(const std::string& activitiesOut);
// D3: source-precedence decision (dumpsys wins over cgroup over /proc scan).
extern std::string aatpeFgSourceDecision(const std::string& dumpsysPkg,
                                        const std::string& cgroupPkg,
                                        const std::string& procPkg);
// D3: is a dumpsys result fetched at fetchedMs still fresh at nowMs (TTL=2s)?
extern bool aatpeFgDumpsysResultFresh(long long nowMs, long long fetchedMs);

// D4: gfxinfo summary metrics. Struct layout MUST match frame.cpp exactly (POD,
// field order preserved) -- same pattern the existing per_app_frame test uses for
// FramestatsMetrics.
struct GfxinfoSummary {
    bool valid = false;
    long totalFrames = 0;
    double jankyPct = 0.0;
    double fps = 0.0;
    long long missedVsyncs = 0;
    long long deadlineMissed = 0;
    double p50ms = 0.0, p90ms = 0.0, p95ms = 0.0, p99ms = 0.0;
    long long statsSinceNs = 0;
};
extern GfxinfoSummary parseGfxinfoSummary(const std::string& out, long long nowUptimeNs);
extern bool aatpeGfxinfoSummaryUsable(const GfxinfoSummary& g);

using namespace aatpe::frameaware;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static bool approx(double a, double b, double eps) {
    return (a > b - eps) && (a < b + eps);
}

int main() {
    printf("=== D3/R8: dumpsys-focused foreground attribution ===\n");

    // --- Case 1: dumpsys resumed WINS over cgroup -----------------------------
    CHECK(aatpeFgSourceDecision("bin.mt.plus", "com.xiaomi.mirror", "") == "bin.mt.plus",
          "case1: dumpsys resumed preferred over cgroup");

    // --- Case 2: cgroup holds mirror, dumpsys says bin.mt.plus -> bin.mt.plus --
    // Real `dumpsys activity activities` stdout on the buggy device: the cgroup
    // top-app group also contains com.xiaomi.mirror (casting), but the system's
    // topResumedActivity is bin.mt.plus (TextEditor -- the real foreground app).
    {
        const char* dump =
            "  mResumedActivity: ActivityRecord{a1 u0 com.xiaomi.mirror/.MirrorService}\n"
            "  topResumedActivity: ActivityRecord{b2 u0 bin.mt.plus/bin.mt.plus.MainActivity}\n"
            "  mFocusedActivity: ActivityRecord{b2 u0 bin.mt.plus/bin.mt.plus.MainActivity}\n";
        std::string got = aatpeParseDumpsysFocusedPkg(dump);
        CHECK(got == "bin.mt.plus",
              "case2: cgroup=mirror but dumpsys topResumed=bin.mt.plus -> bin.mt.plus");
        // decision layer also trusts dumpsys over the mirror cgroup read
        CHECK(aatpeFgSourceDecision(got, "com.xiaomi.mirror", "") == "bin.mt.plus",
              "case2b: decision trusts dumpsys over mirror cgroup");
    }

    // --- R8: split-screen has two resumed activities; pick the FOCUSED one ------
    {
        // Split screen: two resumed stacks; topResumedActivity = the side the user
        // last touched (bin.mt.plus), the other resumed stack = a launcher/recents.
        const char* split =
            "  mResumedActivity: ActivityRecord{c1 u0 com.android.launcher3/.UpActivity}\n"
            "  mResumedActivity: ActivityRecord{c2 u0 com.android.settings/.Settings}\n"
            "  topResumedActivity: ActivityRecord{c2 u0 com.android.settings/.Settings}\n";
        std::string got = aatpeParseDumpsysFocusedPkg(split);
        CHECK(got == "com.android.settings",
              "R8: split-screen picks the focused topResumed (settings, not launcher)");
    }
    {
        // PiP: only mResumedActivity lines (no topResumedActivity in this snippet);
        // first valid resumed wins.
        const char* pip =
            "  mResumedActivity: ActivityRecord{p1 u0 com.example.player/.PiPActivity}\n";
        std::string got = aatpeParseDumpsysFocusedPkg(pip);
        CHECK(got == "com.example.player", "R8: PiP falls back to first valid resumed");
    }

    // --- Case 5: stale dumpsys result must not linger --------------------------
    CHECK(aatpeFgDumpsysResultFresh(1000, 0) == true &&
          aatpeFgDumpsysResultFresh(3000, 0) == false,
          "case5: dumpsys result fresh within 2s TTL, stale after");
    // When this probe's dumpsys re-read comes back EMPTY (stale channel), the old
    // cached package (e.g. bin.mt.plus) must NOT be resurrected: the decision uses
    // whatever the fallbacks provide instead.
    CHECK(aatpeFgSourceDecision("", "com.xiaomi.mirror", "") == "com.xiaomi.mirror",
          "case5b: empty dumpsys probe does not serve stale value (falls to cgroup)");
    CHECK(aatpeFgSourceDecision("", "", "") == "",
          "case5c: nothing usable -> empty (caller marks unknown, no stale residue)");

    printf("\n=== D4: gfxinfo SUMMARY frame-source fallback ===\n");

    // --- Case 3: gfxinfo summary parses fps + jank ------------------------------
    {
        // 10s sampling window: statsSince=1.0e12 ns, now=1.0e12 + 10s.
        const long long statsSince = 1000000000000LL;             // 1e12 ns
        const long long nowNs      = statsSince + 10LL * 1000000000LL;  // +10s
        const char* summary =
            "  Stats since: 1000000000000ns\n"
            "  Total frames rendered: 600\n"
            "  Janky frames: 30 (5.00%)\n"
            "  50th percentile: 6ms\n"
            "  90th percentile: 12ms\n"
            "  95th percentile: 18ms\n"
            "  99th percentile: 34ms\n"
            "  Number Missed Vsync: 15\n"
            "  Number High input latency: 0\n"
            "  Number Slow UI thread: 20\n"
            "  Number Slow bitmap uploads: 0\n"
            "  Number Slow issue draw commands: 5\n"
            "  Number Frame deadline missed: 4\n";
        GfxinfoSummary g = parseGfxinfoSummary(summary, nowNs);
        CHECK(g.valid, "case3: summary parses as valid");
        CHECK(g.totalFrames == 600, "case3: total frames rendered = 600");
        CHECK(approx(g.fps, 60.0, 0.5), "case3: fps = 600 frames / 10s = 60fps");
        CHECK(approx(g.jankyPct, 0.05, 0.001), "case3: jankyPct = 5.00% -> 0.05");
        CHECK(g.missedVsyncs == 15, "case3: Number Missed Vsync = 15");
        CHECK(g.deadlineMissed == 4, "case3: Number Frame deadline missed = 4");
        CHECK(approx(g.p99ms, 34.0, 0.01), "case3: 99th percentile = 34ms");
    }

    // --- Case 4: frame-source all dead -> gfxinfo usable means NOT "none" ------
    {
        const long long statsSince = 2000000000000LL;
        const long long nowNs      = statsSince + 5LL * 1000000000LL;  // +5s, 300 frames
        const char* summary =
            "  Stats since: 2000000000000ns\n"
            "  Total frames rendered: 300\n"
            "  Janky frames: 9 (3.00%)\n"
            "  90th percentile: 10ms\n"
            "  95th percentile: 15ms\n"
            "  99th percentile: 28ms\n"
            "  Number Missed Vsync: 7\n"
            "  Number Frame deadline missed: 2\n";
        GfxinfoSummary g = parseGfxinfoSummary(summary, nowNs);
        CHECK(aatpeGfxinfoSummaryUsable(g) == true,
              "case4: usable gfxinfo summary -> source reports 'gfxinfo', not 'none'");
        // Empty / degenerate summaries stay unusable -> caller keeps "none" (correct).
        GfxinfoSummary empty = parseGfxinfoSummary("", nowNs);
        CHECK(aatpeGfxinfoSummaryUsable(empty) == false,
              "case4b: empty gfxinfo -> unusable (reports 'none')");
        const char* tiny =
            "  Stats since: 2000000000000ns\n  Total frames rendered: 3\n";
        GfxinfoSummary t = parseGfxinfoSummary(tiny, nowNs);
        CHECK(aatpeGfxinfoSummaryUsable(t) == false,
              "case4c: <5 frames -> unusable (does not falsely vote)");
    }

    printf("\n=== C2: timestats blind-window shortened (was fixed 5 min) ===\n");
    {
        TimestatsSession s;
        auto fetch = [](const std::string&) -> std::string { return std::string(); }; // empty -> "disabled"
        s.tick(0, "com.game", fetch);            // -clear -enable + warmup
        TimestatsResult r = s.tick(1500, "com.game", fetch);  // post-warmup dump: empty
        // disabled reason -> adaptive TTL = 60000ms (was 300000ms = 5min).
        CHECK(r.reason == "disabled", "C2: empty dump classified 'disabled'");
        long long blindMs = s.unavailableUntilMs - 1500;
        CHECK(blindMs == 60000, "C2: disabled channel blind window = 60s (was 300s)");
        CHECK(blindMs < 120000, "C2: blind window no longer a 5-minute outage");
    }

    printf("\n===================================================================\n");
    printf("AATPE isolation harness: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

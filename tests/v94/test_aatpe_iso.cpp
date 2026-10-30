#include <cstdio>
#include <cstring>
#include <string>
#include "aatped.h"

extern std::string aatpeParseDumpsysFocusedPkg(const std::string& activitiesOut);
extern std::string aatpeFgSourceDecision(const std::string& dumpsysPkg,
                                        const std::string& cgroupPkg,
                                        const std::string& procPkg);
extern bool aatpeFgDumpsysResultFresh(long long nowMs, long long fetchedMs);

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

    CHECK(aatpeFgSourceDecision("bin.mt.plus", "com.xiaomi.mirror", "") == "bin.mt.plus",
          "case1: dumpsys resumed preferred over cgroup");

    {
        const char* dump =
            "  mResumedActivity: ActivityRecord{a1 u0 com.xiaomi.mirror/.MirrorService}\n"
            "  topResumedActivity: ActivityRecord{b2 u0 bin.mt.plus/bin.mt.plus.MainActivity}\n"
            "  mFocusedActivity: ActivityRecord{b2 u0 bin.mt.plus/bin.mt.plus.MainActivity}\n";
        std::string got = aatpeParseDumpsysFocusedPkg(dump);
        CHECK(got == "bin.mt.plus",
              "case2: cgroup=mirror but dumpsys topResumed=bin.mt.plus -> bin.mt.plus");
        CHECK(aatpeFgSourceDecision(got, "com.xiaomi.mirror", "") == "bin.mt.plus",
              "case2b: decision trusts dumpsys over mirror cgroup");
    }

    {
        const char* split =
            "  mResumedActivity: ActivityRecord{c1 u0 com.android.launcher3/.UpActivity}\n"
            "  mResumedActivity: ActivityRecord{c2 u0 com.android.settings/.Settings}\n"
            "  topResumedActivity: ActivityRecord{c2 u0 com.android.settings/.Settings}\n";
        std::string got = aatpeParseDumpsysFocusedPkg(split);
        CHECK(got == "com.android.settings",
              "R8: split-screen picks the focused topResumed (settings, not launcher)");
    }
    {
        const char* pip =
            "  mResumedActivity: ActivityRecord{p1 u0 com.example.player/.PiPActivity}\n";
        std::string got = aatpeParseDumpsysFocusedPkg(pip);
        CHECK(got == "com.example.player", "R8: PiP falls back to first valid resumed");
    }

    CHECK(aatpeFgDumpsysResultFresh(1000, 0) == true &&
          aatpeFgDumpsysResultFresh(3000, 0) == false,
          "case5: dumpsys result fresh within 2s TTL, stale after");
    CHECK(aatpeFgSourceDecision("", "com.xiaomi.mirror", "") == "com.xiaomi.mirror",
          "case5b: empty dumpsys probe does not serve stale value (falls to cgroup)");
    CHECK(aatpeFgSourceDecision("", "", "") == "",
          "case5c: nothing usable -> empty (caller marks unknown, no stale residue)");

    printf("\n=== D4: gfxinfo SUMMARY frame-source fallback ===\n");

    {
        const long long statsSince = 1000000000000LL;
        const long long nowNs      = statsSince + 10LL * 1000000000LL;
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

    {
        const long long statsSince = 2000000000000LL;
        const long long nowNs      = statsSince + 5LL * 1000000000LL;
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
        auto fetch = [](const std::string&) -> std::string { return std::string(); };
        s.tick(0, "com.game", fetch);
        TimestatsResult r = s.tick(1500, "com.game", fetch);
        CHECK(r.reason == "disabled", "C2: empty dump classified 'disabled'");
        long long blindMs = s.unavailableUntilMs - 1500;
        CHECK(blindMs == 60000, "C2: disabled channel blind window = 60s (was 300s)");
        CHECK(blindMs < 120000, "C2: blind window no longer a 5-minute outage");
    }

    printf("\n===================================================================\n");
    printf("AATPE isolation harness: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

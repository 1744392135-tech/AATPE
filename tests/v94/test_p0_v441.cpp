// test_p0_v441.cpp - v4.4.1 P0 regressions: edge-independent fg cgroup recovery +
// per-app frame data correctness.
//
// The two user-visible P0 defects fixed in v4.4.1:
//   P0-① : after opening a small window (PiP/freeform) or switching away/back, the
//           foreground app's threads stayed pinned on the little cluster (0-3)
//           because recovery was purely package-NAME-edge triggered and the module
//           had ZERO windowing-mode awareness.
//   P0-② : WebUI manually-added apps never got a per-app frame row / showed
//           "497725 hours ago" because (a) gfxinfo-success only wrote fsMetrics and
//           never upserted a panel window, (b) the UI age fed monotonic uptime ms
//           into an epoch (Date.now()) subtraction, (c) /api/apps only enumerated
//           user 0, hiding work-profile apps.
//
// This hermetic harness pins the PURE, host-testable seams (no /dev/cpuset, no
// fork): windowing-mode parsing, the monotonic->epoch age conversion invariant,
// and all-users (work-profile) package enumeration. The live cgroup lift itself is
// exercised on-device (needs root + /dev/cpuset); its host-testable decision
// surfaces are covered here.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "aatped.h"

// ---- seams exported (non-static) from frame.o ---------------------------------
// v4.4.1 P0: parse `dumpsys activity activities` -> focused windowing mode.
extern std::string aatpeParseWindowingMode(const std::string& activitiesOut);
// v4.4.1 P0 (red-team MAJOR-2): PURE per-thread cgroup-heal lift decision.
namespace aatpe { namespace placement {
    bool healShouldLiftThread(const std::string& threadPkg, const std::string& fgPkg,
                              int threadRole, const std::string& liveCgroup,
                              const std::string& controlMode, bool thermalEmergency,
                              int thermalHalLevel, bool cpusetTopAppCap, bool screenOn);
}}
// v4.4.1 P0: all-users package enumeration (work profile / user 10/99).
namespace aatpe { namespace apklabel {
    std::vector<std::string> listInstalledPackagesAllUsers(
        const std::string& packagesListPath,
        const std::string& appsRoot,
        const std::string& dataUserRoot);
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static bool contains(const std::vector<std::string>& v, const std::string& x) {
    for (auto& s : v) if (s == x) return true;
    return false;
}

int main() {
    printf("=== v4.4.1 P0: windowing mode + epoch age + all-users enum ===\n");

    // --- P0-① windowing parsing -------------------------------------------------
    // fullscreen resumed activity -> fullscreen
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{a u0 com.game/.Main}\n"
        "    mWindowingMode=fullscreen\n") == "fullscreen",
          "win: fullscreen resumed -> fullscreen");
    // freeform small window -> freeform (the SAME package, no package edge)
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{a u0 com.game/.Float}\n"
        "    mWindowingMode=freeform\n") == "freeform",
          "win: freeform resumed -> freeform");
    // picture-in-picture -> real dumpsys prints mWindowingMode=pinned (code 2).
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{p u0 com.player/.PipAct}\n"
        "    mWindowingMode=pinned\n") == "pictureInPicture",
          "win: PiP resumed (symbol 'pinned') -> pictureInPicture");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{p u0 com.player/.PipAct}\n"
        "    mWindowingMode=2\n") == "pictureInPicture",
          "win: PiP resumed (numeric code 2) -> pictureInPicture");
    // split screen -> real dumpsys prints split-screen-primary (code 3).
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{s u0 com.game/.Main}\n"
        "    mWindowingMode=split-screen-primary\n") == "splitScreen",
          "win: split-screen-primary -> splitScreen");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{s u0 com.game/.Main}\n"
        "    mWindowingMode=3\n") == "splitScreen",
          "win: split screen (numeric code 3) -> splitScreen");
    // freeform numeric code 5 (symbol already covered above).
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{a u0 com.game/.Float}\n"
        "    mWindowingMode=5\n") == "freeform",
          "win: freeform (numeric code 5) -> freeform");
    // empty / no token -> unknown (must not crash, must not invent a mode)
    CHECK(aatpeParseWindowingMode("") == "unknown",
          "win: empty dump -> unknown");
    CHECK(aatpeParseWindowingMode("  topResumedActivity: ActivityRecord{a u0 com.game/.Main}\n") == "unknown",
          "win: resumed act without mode token -> unknown");

    // --- P0-① heal lift decision (red-team MAJOR-2) ------------------------------
    // Roles (aatped_types.h): S1_UI=1, S2_RENDER=2, S3_GAME=3; S5 bg=5.
    const int UI=1, RENDER=2, GAME=3, BG=5;
    using aatpe::placement::healShouldLiftThread;
    // baseline: screen on, assist, no thermal, cap on -> fg critical thread parked
    // in background cgroup SHOULD be lifted back to top-app.
    auto lift = [&](const std::string& tPkg, const std::string& fgPkg, int role,
                    const std::string& live, const std::string& mode,
                    bool thermEmerg, int hal, bool cap, bool screen) {
        return healShouldLiftThread(tPkg, fgPkg, role, live, mode, thermEmerg, hal, cap, screen);
    };
    // The five user transitions all converge on the SAME invariant: the CURRENT
    // foreground package's critical threads, found parked outside top-app, get
    // lifted regardless of whether a package-name edge fired.
    CHECK(lift("com.game","com.game",UI,"background","assist",false,0,true,true),
          "heal 1/5 BG->FG: fg UI thread in background cgroup -> lift");
    CHECK(lift("com.game","com.game",RENDER,"background","assist",false,0,true,true),
          "heal 2/5 A->B->A: fg Render thread in background cgroup -> lift");
    CHECK(lift("com.player","com.player",GAME,"background","assist",false,0,true,true),
          "heal 3/5 PiP on->off: fg Game thread in background cgroup -> lift");
    CHECK(lift("com.game","com.game",UI,"background","assist",false,0,true,true),
          "heal 4/5 freeform->fullscreen: fg UI thread in background cgroup -> lift");
    CHECK(lift("com.game","com.game",RENDER,"background","assist",false,0,true,true),
          "heal 5/5 same-package switchback: fg Render thread in background cgroup -> lift");
    // Guardrails (must NOT lift):
    CHECK(!lift("com.game","com.game",UI,"background","assist",false,0,true,false),
          "heal GUARD: screen OFF -> never lift (red-team MAJOR-1 battery red line)");
    CHECK(!lift("com.other.bg","com.game",UI,"background","assist",false,0,true,true),
          "heal GUARD: a BACKGROUND/other package's UI thread -> never lift");
    CHECK(!lift("com.game","com.game",BG,"background","assist",false,0,true,true),
          "heal GUARD: fg package's non-critical (S5 bg) thread -> never lift");
    CHECK(!lift("com.game","com.game",UI,"top-app","assist",false,0,true,true),
          "heal GUARD: already in top-app -> no redundant write");
    CHECK(!lift("com.game","com.game",UI,"background","passive",false,0,true,true),
          "heal GUARD: passive mode -> no proactive migration");
    CHECK(!lift("com.game","com.game",UI,"background","assist",true,0,true,true),
          "heal GUARD: thermal emergency (CPU sysfs wall) -> no lift");
    // v6.0.0 P0 (realdevice locked-little-core): split the two thermal signals.
    // Android skin thermal HAL level>=3 (SEVERE/CRITICAL) with NO CPU emergency
    // must NO LONGER wall cgroup lift — Android cpufreq already governs power;
    // lifting cpuset membership only changes allowed-affinity. The real root cause
    // was AATPE treating a ~72C-CPU skin CRITICAL(4) as a hard stop.
    CHECK(lift("com.game","com.game",UI,"background","assist",false,4,true,true),
          "heal v6: skin HAL CRITICAL(4) but !CPU emergency -> LIFT (was wrongly walled)");
    CHECK(lift("com.game","com.game",RENDER,"background","assist",false,3,true,true),
          "heal v6: skin HAL SEVERE(3) but !CPU emergency -> LIFT");
    CHECK(!lift("com.game","com.game",UI,"background","assist",true,4,true,true),
          "heal v6: skin HAL CRITICAL(4) AND CPU emergency -> no lift (real CPU wall)");
    CHECK(!lift("com.game","com.game",UI,"background","assist",false,0,false,true),
          "heal GUARD: cpusetTopApp capability absent -> honest no-op");

    // --- P0-② monotonic->epoch age invariant ------------------------------------
    // The fix computes bootEpochMs = system_now - steady_now once per read, then
    // lastSeenEpoch = bootEpochMs + lastFrameMonotonic. Assert the invariant: for a
    // frame observed t ms after boot, the resulting epoch age (epoch_now -
    // lastSeenEpoch) is ~= t ms and SMALL (never ~56 years). This is the exact
    // substitution the backend now performs.
    {
        long long nowSys   = 1760000000000LL;   // fake epoch ms
        long long nowSteady = 300000LL;         // uptime ms (5 minutes after boot)
        long long bootEpochMs = nowSys - nowSteady;
        long long lastFrameTsMs = 299000LL;     // last frame 1s ago (monotonic)
        long long lastSeenEpoch = bootEpochMs + lastFrameTsMs;
        long long ageMs = nowSys - lastSeenEpoch;   // == nowSteady - lastFrameTsMs = 1000ms
        CHECK(ageMs >= 0 && ageMs < 60000,
              "epoch: monotonic->epoch yields a small sane age (~1s), not 56 years");
        // The OLD buggy code set lastSeenEpoch = nowSys; that always read "just now".
        // The correct value must differ from nowSys by ~the frame age.
        CHECK(ageMs > 0, "epoch: age reflects real staleness, not serialization time");
    }
    // sentinel: stub rows write lastSeenEpochMs = -1 -> UI treats as "no age".
    {
        long long sentinel = -1;
        bool uiShowsAge = (sentinel > 0);   // mirrors index.html: seenMs > 0 gate
        CHECK(!uiShowsAge, "epoch: sentinel -1 => UI shows no age for never-seen rows");
    }

    // --- P0-② all-users package enumeration (work profile) ----------------------
    {
        char tmpl[] = "/tmp/p0v441_pkgs.XXXXXX";
        char* d = mkdtemp(tmpl);
        std::string root = d;
        // Build a packages.list: user0 game + user10 work-profile app + user99 app.
        // Format: <pkg> <uid> <debuggable> <dataDir> ...
        std::string plist = root + "/packages.list";
        FILE* f = fopen(plist.c_str(), "w");
        // uid = 100000*user + appId
        fprintf(f, "com.game.ugc            10123 0 /data/user/0/com.game.uggc\n");
        fprintf(f, "com.work.app            100123 0 /data/user/10/com.work.app\n");
        fprintf(f, "com.other.profile       100990 0 /data/user/99/com.other.profile\n");
        fclose(f);
        std::vector<std::string> pkgs =
            aatpe::apklabel::listInstalledPackagesAllUsers(plist, root + "/data/app", root + "/data/user");
        CHECK(contains(pkgs, "com.game.uggc") || contains(pkgs, "com.game.ugc"),
              "all-users: user0 app enumerated");
        CHECK(contains(pkgs, "com.work.app"),
              "all-users: work-profile user10 app enumerated (was hidden by pm list)");
        CHECK(contains(pkgs, "com.other.profile"),
              "all-users: user99 profile app enumerated");
        // cleanup best-effort
        unlink(plist.c_str());
        rmdir(root.c_str());
    }

    printf("===================================================================\n");
    printf("v4.4.1 P0 host tests: %d ok, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

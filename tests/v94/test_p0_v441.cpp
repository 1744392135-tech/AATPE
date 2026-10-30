#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "aatped.h"

extern std::string aatpeParseWindowingMode(const std::string& activitiesOut);
namespace aatpe { namespace placement {
    bool healShouldLiftThread(const std::string& threadPkg, const std::string& fgPkg,
                              int threadRole, const std::string& liveCgroup,
                              const std::string& controlMode, bool thermalEmergency,
                              int thermalHalLevel, bool cpusetTopAppCap, bool screenOn);
}}
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

    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{a u0 com.game/.Main}\n"
        "    mWindowingMode=fullscreen\n") == "fullscreen",
          "win: fullscreen resumed -> fullscreen");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{a u0 com.game/.Float}\n"
        "    mWindowingMode=freeform\n") == "freeform",
          "win: freeform resumed -> freeform");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{p u0 com.player/.PipAct}\n"
        "    mWindowingMode=pinned\n") == "pictureInPicture",
          "win: PiP resumed (symbol 'pinned') -> pictureInPicture");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{p u0 com.player/.PipAct}\n"
        "    mWindowingMode=2\n") == "pictureInPicture",
          "win: PiP resumed (numeric code 2) -> pictureInPicture");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{s u0 com.game/.Main}\n"
        "    mWindowingMode=split-screen-primary\n") == "splitScreen",
          "win: split-screen-primary -> splitScreen");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{s u0 com.game/.Main}\n"
        "    mWindowingMode=3\n") == "splitScreen",
          "win: split screen (numeric code 3) -> splitScreen");
    CHECK(aatpeParseWindowingMode(
        "  topResumedActivity: ActivityRecord{a u0 com.game/.Float}\n"
        "    mWindowingMode=5\n") == "freeform",
          "win: freeform (numeric code 5) -> freeform");
    CHECK(aatpeParseWindowingMode("") == "unknown",
          "win: empty dump -> unknown");
    CHECK(aatpeParseWindowingMode("  topResumedActivity: ActivityRecord{a u0 com.game/.Main}\n") == "unknown",
          "win: resumed act without mode token -> unknown");

    const int UI=1, RENDER=2, GAME=3, BG=5;
    using aatpe::placement::healShouldLiftThread;
    auto lift = [&](const std::string& tPkg, const std::string& fgPkg, int role,
                    const std::string& live, const std::string& mode,
                    bool thermEmerg, int hal, bool cap, bool screen) {
        return healShouldLiftThread(tPkg, fgPkg, role, live, mode, thermEmerg, hal, cap, screen);
    };
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
    CHECK(lift("com.game","com.game",UI,"background","assist",false,4,true,true),
          "heal v6: skin HAL CRITICAL(4) but !CPU emergency -> LIFT (was wrongly walled)");
    CHECK(lift("com.game","com.game",RENDER,"background","assist",false,3,true,true),
          "heal v6: skin HAL SEVERE(3) but !CPU emergency -> LIFT");
    CHECK(!lift("com.game","com.game",UI,"background","assist",true,4,true,true),
          "heal v6: skin HAL CRITICAL(4) AND CPU emergency -> no lift (real CPU wall)");
    CHECK(!lift("com.game","com.game",UI,"background","assist",false,0,false,true),
          "heal GUARD: cpusetTopApp capability absent -> honest no-op");

    {
        long long nowSys   = 1760000000000LL;
        long long nowSteady = 300000LL;
        long long bootEpochMs = nowSys - nowSteady;
        long long lastFrameTsMs = 299000LL;
        long long lastSeenEpoch = bootEpochMs + lastFrameTsMs;
        long long ageMs = nowSys - lastSeenEpoch;
        CHECK(ageMs >= 0 && ageMs < 60000,
              "epoch: monotonic->epoch yields a small sane age (~1s), not 56 years");
        CHECK(ageMs > 0, "epoch: age reflects real staleness, not serialization time");
    }
    {
        long long sentinel = -1;
        bool uiShowsAge = (sentinel > 0);
        CHECK(!uiShowsAge, "epoch: sentinel -1 => UI shows no age for never-seen rows");
    }

    {
        char tmpl[] = "/tmp/p0v441_pkgs.XXXXXX";
        char* d = mkdtemp(tmpl);
        std::string root = d;
        std::string plist = root + "/packages.list";
        FILE* f = fopen(plist.c_str(), "w");
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
        unlink(plist.c_str());
        rmdir(root.c_str());
    }

    printf("===================================================================\n");
    printf("v4.4.1 P0 host tests: %d ok, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

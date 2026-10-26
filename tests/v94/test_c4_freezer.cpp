// test_c4_freezer.cpp — host unit tests for the C4 freezer PURE decision layer.
#include <cstdio>
#include <cstring>
#include "c4_freezer.h"
#include "aatpe_caps.h"  // GPU platform pure classifier

using namespace aatpe;
using c4freezer::C4Action;
using c4freezer::C4Decision;
using c4freezer::C4DecisionInputs;

static int s_pass = 0, s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { s_pass++; printf("  [PASS] %s\n", msg); } \
    else { s_fail++; printf("  [FAIL] %s\n", msg); } \
} while (0)

static C4DecisionInputs base() {
    C4DecisionInputs i;
    i.featureEnabled   = true;
    i.isForeground     = false; // background by default
    i.audioActive      = false;
    i.freezerAvailable = true;
    i.alreadyFrozen    = false;
    return i;
}

int main() {
    printf("=== C4 freezer: background -> freeze ===\n");
    {
        C4DecisionInputs i = base();
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::FREEZE, "T1: background+audio off+v2 freezer -> FREEZE");
        CHECK(!d.degraded, "T1: not degraded when freezer present");
    }

    printf("=== C4 freezer: gate OFF is byte-identical (no action) ===\n");
    {
        C4DecisionInputs i = base();
        i.featureEnabled = false;
        i.audioActive = false; i.freezerAvailable = true;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE, "T2: gate-off -> NO_CHANGE");
        CHECK(!d.degraded, "T2: gate-off not degraded");
    }

    printf("=== C4 freezer: audio whitelist (never freeze) ===\n");
    {
        C4DecisionInputs i = base();
        i.audioActive = true;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE, "T3: audio active -> NO_CHANGE (whitelist)");
    }

    printf("=== C4 freezer: symmetric unfreeze (frozen -> foreground MUST unfreeze) ===\n");
    {
        C4DecisionInputs i = base();
        i.alreadyFrozen = true;
        i.isForeground = true;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::UNFREEZE, "T4: frozen + foreground edge -> UNFREEZE");
    }
    {
        C4DecisionInputs i = base();
        i.isForeground = true;
        i.alreadyFrozen = false;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE, "T4: foreground + not frozen -> NO_CHANGE");
    }

    printf("=== C4 freezer: no freezer backend (v1/v2 both absent) -> degrade ===\n");
    {
        C4DecisionInputs i = base();
        i.freezerAvailable = false;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE, "T5: no freezer -> NO_CHANGE");
        CHECK(d.degraded, "T5: no freezer -> degraded=true");
    }

    printf("=== C4 freezer: idempotent (already frozen -> no re-freeze, no永冻) ===\n");
    {
        C4DecisionInputs i = base();
        i.alreadyFrozen = true;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE, "T6: already frozen + background -> NO_CHANGE (idempotent)");
    }

    printf("=== C4 freezer: fail-safe ordering (audio beats freezer) ===\n");
    {
        // Both freezer present AND audio active -> audio whitelist wins (no freeze).
        C4DecisionInputs i = base();
        i.freezerAvailable = true; i.audioActive = true;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE && !d.degraded, "T7: audio whitelist beats available freezer");
    }

    printf("=== GPU platform pure classifier (GED/KGSL/Mali) ===\n");
    {
        // KGSL node present -> kgsl (Adreno)
        const char* t1 = aatpe::caps::classifyGpuPlatform(/*devfreqGpu*/false,/*devfreqMali*/false,
                                                    /*devfreqKgsl*/false,/*kgslNode*/true,
                                                    /*gedHal*/false,/*gedDev*/false);
        CHECK(strcmp(t1, "kgsl") == 0, "G1: /sys/class/kgsl node -> kgsl");
        // devfreq kgsl name -> kgsl
        const char* t2 = aatpe::caps::classifyGpuPlatform(false,false,true,false,false,false);
        CHECK(strcmp(t2, "kgsl") == 0, "G1: devfreq 'kgsl-3d0' -> kgsl");
        // Mali devfreq -> mali
        const char* t3 = aatpe::caps::classifyGpuPlatform(false,true,false,false,false,false);
        CHECK(strcmp(t3, "mali") == 0, "G1: devfreq 'mali' -> mali");
        // GED hal -> ged
        const char* t4 = aatpe::caps::classifyGpuPlatform(false,false,false,false,true,false);
        CHECK(strcmp(t4, "ged") == 0, "G1: /sys/kernel/ged/hal -> ged");
        // /dev/ged device node -> ged
        const char* t5 = aatpe::caps::classifyGpuPlatform(false,false,false,false,false,true);
        CHECK(strcmp(t5, "ged") == 0, "G1: /dev/ged node -> ged");
        // nothing -> none
        const char* t6 = aatpe::caps::classifyGpuPlatform(false,false,false,false,false,false);
        CHECK(strcmp(t6, "none") == 0, "G1: no GPU node -> none");
    }

    printf("=== PURE: parseV2CgroupPath (/proc/<tid>/cgroup) ===\n");
    {
        const char* v2 = "0::/a/b/top-app\n";
        std::string p = aatpe::c4freezer::parseV2CgroupPath(v2);
        CHECK(p == "a/b/top-app", "H1: v2 line strips leading slash");
        const char* v1only = "3:cpuset:/a/b/top-app\n2:cpu:/a/b\n";
        std::string q = aatpe::c4freezer::parseV2CgroupPath(v1only);
        CHECK(q == "", "H1: v1-only -> empty (degrade)");
        std::string r = aatpe::c4freezer::parseV2CgroupPath("");
        CHECK(r == "", "H1: empty/corrupt -> empty");
        const char* hybrid = "3:cpu:/sys\n0::/game/u0_a123\n";
        std::string s = aatpe::c4freezer::parseV2CgroupPath(hybrid);
        CHECK(s == "game/u0_a123", "H1: hybrid picks v2 line");
    }

    printf("=== PURE: buildFreezeDir join ===\n");
    {
        std::string d = aatpe::c4freezer::buildFreezeDir("game/u0_a123", "/sys/fs/cgroup");
        CHECK(d == "/sys/fs/cgroup/game/u0_a123", "H2: root+v2 path joined");
        std::string e = aatpe::c4freezer::buildFreezeDir("", "/sys/fs/cgroup");
        CHECK(e == "", "H2: empty v2 path -> empty (no freeze)");
    }

    printf("=== PURE: mediaSessionMentionsPlaying ===\n");
    {
        const char* playing =
            "Session #1:\n"
            "  package=com.mygame\n"
            "  playback state=3\n";
        CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(playing, "com.mygame"),
              "H3: pkg with state=3 (PLAYING) -> true");
        const char* paused =
            "Session #1:\n"
            "  package=com.mygame\n"
            "  playback state=2\n";
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(paused, "com.mygame"),
              "H3: pkg state=2 (PAUSED) -> false");
        const char* otherpkg =
            "Session #1:\n"
            "  package=com.music.app\n"
            "  playback state=3\n";
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(otherpkg, "com.mygame"),
              "H3: playing but OTHER pkg -> false");
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying("", "com.mygame"),
              "H3: empty output -> false (exec failure handled by glue)");
    }

    printf("=== R4 MJ-1: cgroupPathBelongsToUid (boundary match) ===\n");
    {
        CHECK(aatpe::c4freezer::cgroupPathBelongsToUid("uid_10123/cgroup_4567", 10123),
              "R4: uid_10123/cgroup_4567 belongs to 10123");
        CHECK(!aatpe::c4freezer::cgroupPathBelongsToUid("uid_10123/cgroup_4567", 1012),
              "R4: uid_10123 does NOT match appUid 1012 (boundary)");
        CHECK(!aatpe::c4freezer::cgroupPathBelongsToUid("uid_101230/x", 10123),
              "R4: uid_101230 does NOT match appUid 10123 (boundary)");
        CHECK(!aatpe::c4freezer::cgroupPathBelongsToUid("top-app", 10123),
              "R4: system 'top-app' -> false");
        CHECK(!aatpe::c4freezer::cgroupPathBelongsToUid("init", 10123),
              "R4: system 'init' -> false");
        CHECK(!aatpe::c4freezer::cgroupPathBelongsToUid("uid_10123", 0),
              "R4: appUid<=0 -> false");
    }

    printf("=== R4 MJ-2: audio re-eval cadence ===\n");
    {
        CHECK(aatpe::c4freezer::audioReevalDue(0, 5), "R4: bg edge tick0 -> due");
        CHECK(!aatpe::c4freezer::audioReevalDue(1, 5), "R4: tick1 -> not due");
        CHECK(!aatpe::c4freezer::audioReevalDue(4, 5), "R4: tick4 -> not due");
        CHECK(aatpe::c4freezer::audioReevalDue(5, 5), "R4: tick5 (period) -> due");
        CHECK(!aatpe::c4freezer::audioReevalDue(-1, 5), "R4: -1 (foreground) -> not due");
    }

    printf("=== R4 MJ-2 decision: audio-started-mid-freeze -> UNFREEZE ===\n");
    {
        C4DecisionInputs i = base();
        i.alreadyFrozen = true;
        i.audioActive   = true;
        C4Decision d = aatpe::c4freezer::decideC4Freeze(i);
        CHECK(d.action == aatpe::c4freezer::C4Action::UNFREEZE,
              "R4: frozen + audio now playing -> UNFREEZE (releases mid-freeze)");
    }
    {
        C4DecisionInputs i = base();
        i.alreadyFrozen = false;
        i.audioActive   = true;
        C4Decision d = aatpe::c4freezer::decideC4Freeze(i);
        CHECK(d.action == aatpe::c4freezer::C4Action::NO_CHANGE,
              "R4: not frozen + audio -> NO_CHANGE (whitelist)");
    }

    printf("=== v4.4.5/v4.4.6 NN: frozen-state serialize/parse round-trip ===\n");
    {
        std::string s = aatpe::c4freezer::serializeC4Frozen(
            "/sys/fs/cgroup/uid_10123/cg_42", "com.game", 10123, "com.game");
        aatpe::c4freezer::C4FrozenState st = aatpe::c4freezer::parseC4Frozen(s);
        CHECK(st.valid, "NN: round-trip parses valid");
        CHECK(st.cgDir == "/sys/fs/cgroup/uid_10123/cg_42", "NN: cgDir preserved");
        CHECK(st.pkg == "com.game", "NN: pkg preserved");
        CHECK(st.uid == 10123, "NN: uid preserved");
        CHECK(st.lastGame == "com.game", "NN: lastGame preserved (v4.4.6)");
        // corrupt / half-written
        CHECK(!aatpe::c4freezer::parseC4Frozen("").valid, "NN: empty -> invalid");
        CHECK(!aatpe::c4freezer::parseC4Frozen("cg=/x\npkg=com.g\n").valid,
              "NN: missing uid -> invalid");
        CHECK(!aatpe::c4freezer::parseC4Frozen("cg=\npkg=com.g\nuid=10123\n").valid,
              "NN: empty cg -> invalid (half-written)");
        CHECK(!aatpe::c4freezer::parseC4Frozen("cg=/x\npkg=\nuid=10123\n").valid,
              "NN: empty pkg -> invalid");
        // v4.4.6 backwards compat: old v4.4.5 file WITHOUT lg= is still valid.
        aatpe::c4freezer::C4FrozenState old = aatpe::c4freezer::parseC4Frozen(
            "cg=/sys/fs/cgroup/uid_10123/cg_42\npkg=com.game\nuid=10123\n");
        CHECK(old.valid, "NN: old file w/o lg= still valid (backward compat)");
        CHECK(old.lastGame.empty(), "NN: old file lastGame empty (safe)");
        // empty lastGame is NOT serialized (no lg= line).
        std::string s2 = aatpe::c4freezer::serializeC4Frozen("/d", "com.g", 1000, "");
        CHECK(s2.find("lg=") == std::string::npos, "NN: empty lastGame omits lg=");
    }

    printf("=== v4.4.5 MN: target selection (launcher-in-background keeps game) ===\n");
    {
        // game in background, launcher in foreground -> keep last game as target
        CHECK(aatpe::c4freezer::c4ChooseTarget("com.game", "com.android.launcher", false)
              == "com.game", "MN: launcher fg -> freeze last game");
        // game returns to foreground -> target = that game
        CHECK(aatpe::c4freezer::c4ChooseTarget("com.game", "com.game", true)
              == "com.game", "MN: game fg -> target = game");
        // switch to another game -> retarget
        CHECK(aatpe::c4freezer::c4ChooseTarget("com.game", "com.game2", true)
              == "com.game2", "MN: switch game -> new target");
        // no last game + non-game fg -> nothing to freeze
        CHECK(aatpe::c4freezer::c4ChooseTarget("", "com.android.launcher", false)
              == "", "MN: no tracked game + launcher -> no target");
    }

    printf("=== v4.4.6/v6.1.0: C4 WebUI state JSON ===\n");
    {
        std::string j = aatpe::c4freezer::c4StateJson(
            true, "com.game", true, "/sys/fs/cgroup/uid_10123/cg_1",
            "UNFREEZE(audio-started-mid-freeze)", true, true, "v2", "");
        CHECK(j.find("\"enabled\":true") != std::string::npos, "JSON: enabled field");
        CHECK(j.find("\"target\":\"com.game\"") != std::string::npos, "JSON: target field");
        CHECK(j.find("\"frozen\":true") != std::string::npos, "JSON: frozen field");
        CHECK(j.find("UNFREEZE(audio-started-mid-freeze)") != std::string::npos,
              "JSON: lastAction reason");
        CHECK(j.find("\"audioActive\":true") != std::string::npos, "JSON: audio field");
        CHECK(j.find("\"freezerBackend\":\"v2\"") != std::string::npos,
              "JSON: freezerBackend=v2 field");
        CHECK(j.front()=='{' && j.back()=='}', "JSON: balanced object");
        // OFF safety: disabled, no error fields leaking.
        std::string off = aatpe::c4freezer::c4StateJson(
            false, "", false, "", "", false, false, "", "disabled");
        CHECK(off.find("\"enabled\":false") != std::string::npos, "JSON: OFF -> disabled");
        CHECK(off.find("\"degraded\":\"disabled\"") != std::string::npos, "JSON: OFF degraded");
        // v1 backend reporting
        std::string v1 = aatpe::c4freezer::c4StateJson(
            true, "com.game", true, "/sys/fs/cgroup/freezer/uid_10123",
            "FREEZE(v1)", false, true, "v1", "");
        CHECK(v1.find("\"freezerBackend\":\"v1\"") != std::string::npos,
              "JSON: freezerBackend=v1 field");
    }

    printf("=== v6.1.0: v1 freezer pure helpers ===\n");
    {
        // parseV1FreezerPath: standard v1 freezer line
        std::string p1 = aatpe::c4freezer::parseV1FreezerPath(
            "3:freezer:/uid_10123/cg_42\n2:cpu:/uid_10123\n");
        CHECK(p1 == "uid_10123/cg_42", "V1: freezer: line extracted");
        // v2-only content -> empty
        std::string p2 = aatpe::c4freezer::parseV1FreezerPath("0::/uid_10123/cg_1\n");
        CHECK(p2.empty(), "V1: v2-only -> empty");
        // Combined controllers (cpu,freezer together)
        std::string p3 = aatpe::c4freezer::parseV1FreezerPath(
            "2:cpu,freezer:/uid_10123/bg\n");
        CHECK(p3 == "uid_10123/bg", "V1: combined ctrl line parsed");
        // No freezer controller -> empty
        std::string p4 = aatpe::c4freezer::parseV1FreezerPath("2:cpu:/uid_10123\n");
        CHECK(p4.empty(), "V1: cpu-only -> empty");

        // parseV1FreezerState
        CHECK(aatpe::c4freezer::parseV1FreezerState("FROZEN\n") == "FROZEN",
              "V1: FROZEN parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerState("THAWED\n") == "THAWED",
              "V1: THAWED parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerState("FREEZING\n") == "FREEZING",
              "V1: FREEZING parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerState("garbage") == "",
              "V1: unknown -> empty");

        // isV1FreezerDir
        CHECK(aatpe::c4freezer::isV1FreezerDir("/sys/fs/cgroup/freezer/uid_10123"),
              "V1: /freezer/ path detected as v1");
        CHECK(!aatpe::c4freezer::isV1FreezerDir("/sys/fs/cgroup/uid_10123"),
              "V1: v2 path (no /freezer/) not v1");
        CHECK(!aatpe::c4freezer::isV1FreezerDir(""),
              "V1: empty path -> not v1");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}

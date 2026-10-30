#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include "c4_freezer.h"
#include "aatpe_caps.h"

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
    i.isForeground     = false;
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
        C4DecisionInputs i = base();
        i.freezerAvailable = true; i.audioActive = true;
        C4Decision d = c4freezer::decideC4Freeze(i);
        CHECK(d.action == C4Action::NO_CHANGE && !d.degraded, "T7: audio whitelist beats available freezer");
    }

    printf("=== GPU platform pure classifier (GED/KGSL/Mali) ===\n");
    {
        const char* t1 = aatpe::caps::classifyGpuPlatform( false, false,
 false, true,
 false, false);
        CHECK(strcmp(t1, "kgsl") == 0, "G1: /sys/class/kgsl node -> kgsl");
        const char* t2 = aatpe::caps::classifyGpuPlatform(false,false,true,false,false,false);
        CHECK(strcmp(t2, "kgsl") == 0, "G1: devfreq 'kgsl-3d0' -> kgsl");
        const char* t3 = aatpe::caps::classifyGpuPlatform(false,true,false,false,false,false);
        CHECK(strcmp(t3, "mali") == 0, "G1: devfreq 'mali' -> mali");
        const char* t4 = aatpe::caps::classifyGpuPlatform(false,false,false,false,true,false);
        CHECK(strcmp(t4, "ged") == 0, "G1: /sys/kernel/ged/hal -> ged");
        const char* t5 = aatpe::caps::classifyGpuPlatform(false,false,false,false,false,true);
        CHECK(strcmp(t5, "ged") == 0, "G1: /dev/ged node -> ged");
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
        const char* lookalike =
            "Session #1:\n"
            "  package=com.mygame.hack\n"
            "  playback state=3\n";
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(lookalike, "com.mygame"),
              "H3: com.mygame.hack playing does NOT match com.mygame (exact pkg)");
        const char* crossblock =
            "Session #1:\n"
            "  package=com.mygame\n"
            "  playback state=2\n"
            "Session #2:\n"
            "  package=com.other\n"
            "  playback state=3\n";
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(crossblock, "com.mygame"),
              "H3: state=3 in another session block does not count for com.mygame");
        const char* flat =
            "package=com.mygame\n"
            "playback state=3\n";
        CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(flat, "com.mygame"),
              "H3: flat (non-indented) dumpsys still detects playing pkg");
        const char* flatOther =
            "package=com.other\n"
            "playback state=3\n"
            "package=com.mygame\n"
            "playback state=2\n";
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(flatOther, "com.mygame"),
              "H3: flat dump, other pkg playing does not match com.mygame");
        const char* bothPlaying =
            "Session #1:\n  package=com.mygame\n  playback state=3\n"
            "Session #2:\n  package=com.other\n  playback state=3\n";
        CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(bothPlaying, "com.mygame"),
              "R3N2: mygame session state=3 -> playing for mygame");
        CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(bothPlaying, "com.other"),
              "R3N2: other session independently state=3 -> playing for other");
        const char* mlDataRow =
            "Session #1:\n"
            "  package=com.mygame\n"
            "  playbackConfig volMax=100mL\n"
            "  playback state=3\n";
        CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(mlDataRow, "com.mygame"),
              "R3N2: data row 'volMax=100mL' does not reset block, state=3 still plays");
        const char* flagsState =
            "Session #1:\n  package=com.mygame\n  playback flags=state=3\n";
        CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(flagsState, "com.mygame"),
              "R3N4: 'flags=state=3' substring is not exact state=3 -> not playing");
        {
            std::string commas = "Session #1:\n  package=com.mygame\n  flags=AUDIO,VIDEO;rel;  playback state=2\n";
            bool done = false, result = false;
            std::thread t([&](){ result = aatpe::c4freezer::mediaSessionMentionsPlaying(commas, "com.mygame"); done = true; });
            t.join();
            CHECK(done && !result,
                  "R4: comma/semicolon row returns false promptly (no hang)");
        }
        {
            std::string csv = "Session #1:\n  package=com.mygame\n  flags=X,state=3,Y\n";
            CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(csv, "com.mygame"),
                  "R4: comma-separated exact 'state=3' token still plays");
        }
        {
            std::string colon = "Session #1:\n  package=com.mygame:123\n  playback state=3\n";
            CHECK(aatpe::c4freezer::mediaSessionMentionsPlaying(colon, "com.mygame"),
                  "R4: package=com.mygame:123 trims :id suffix, still plays");
        }
        {
            std::string noPkgBlock =
                "Session #1:\n  playback state=3\n"
                "Session #2:\n  package=com.other\n  playback state=3\n";
            CHECK(!aatpe::c4freezer::mediaSessionMentionsPlaying(noPkgBlock, "com.mygame"),
                  "R4: block without package= does not leak curPkg across sessions");
        }
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
        CHECK(!aatpe::c4freezer::parseC4Frozen("").valid, "NN: empty -> invalid");
        CHECK(!aatpe::c4freezer::parseC4Frozen("cg=/x\npkg=com.g\n").valid,
              "NN: missing uid -> invalid");
        CHECK(!aatpe::c4freezer::parseC4Frozen("cg=\npkg=com.g\nuid=10123\n").valid,
              "NN: empty cg -> invalid (half-written)");
        CHECK(!aatpe::c4freezer::parseC4Frozen("cg=/x\npkg=\nuid=10123\n").valid,
              "NN: empty pkg -> invalid");
        aatpe::c4freezer::C4FrozenState old = aatpe::c4freezer::parseC4Frozen(
            "cg=/sys/fs/cgroup/uid_10123/cg_42\npkg=com.game\nuid=10123\n");
        CHECK(old.valid, "NN: old file w/o lg= still valid (backward compat)");
        CHECK(old.lastGame.empty(), "NN: old file lastGame empty (safe)");
        std::string s2 = aatpe::c4freezer::serializeC4Frozen("/d", "com.g", 1000, "");
        CHECK(s2.find("lg=") == std::string::npos, "NN: empty lastGame omits lg=");
    }

    printf("=== v4.4.5 MN: target selection (launcher-in-background keeps game) ===\n");
    {
        CHECK(aatpe::c4freezer::c4ChooseTarget("com.game", "com.android.launcher", false)
              == "com.game", "MN: launcher fg -> freeze last game");
        CHECK(aatpe::c4freezer::c4ChooseTarget("com.game", "com.game", true)
              == "com.game", "MN: game fg -> target = game");
        CHECK(aatpe::c4freezer::c4ChooseTarget("com.game", "com.game2", true)
              == "com.game2", "MN: switch game -> new target");
        CHECK(aatpe::c4freezer::c4ChooseTarget("", "com.android.launcher", false)
              == "", "MN: no tracked game + launcher -> no target");
    }

    printf("=== v6.0.1: fgIsGame signal OR (manual/config/engine-thread) ===\n");
    {
        CHECK(aatpe::c4freezer::c4FgIsGameSignals(false, false, false) == false,
              "signals: zero signals -> not a game (safe)");
        CHECK(aatpe::c4freezer::c4FgIsGameSignals(true, false, false) == true,
              "signals: manual list alone -> game");
        CHECK(aatpe::c4freezer::c4FgIsGameSignals(false, true, false) == true,
              "signals: config list alone -> game");
        CHECK(aatpe::c4freezer::c4FgIsGameSignals(false, false, true) == true,
              "signals: auto engine-thread alone -> game");
        CHECK(aatpe::c4freezer::c4FgIsGameSignals(true, true, true) == true,
              "signals: all three -> game");
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
        std::string off = aatpe::c4freezer::c4StateJson(
            false, "", false, "", "", false, false, "", "disabled");
        CHECK(off.find("\"enabled\":false") != std::string::npos, "JSON: OFF -> disabled");
        CHECK(off.find("\"degraded\":\"disabled\"") != std::string::npos, "JSON: OFF degraded");
        std::string v1 = aatpe::c4freezer::c4StateJson(
            true, "com.game", true, "/sys/fs/cgroup/freezer/uid_10123",
            "FREEZE(v1)", false, true, "v1", "");
        CHECK(v1.find("\"freezerBackend\":\"v1\"") != std::string::npos,
              "JSON: freezerBackend=v1 field");
    }

    printf("=== v6.1.0: v1 freezer pure helpers ===\n");
    {
        std::string p1 = aatpe::c4freezer::parseV1FreezerPath(
            "3:freezer:/uid_10123/cg_42\n2:cpu:/uid_10123\n");
        CHECK(p1 == "uid_10123/cg_42", "V1: freezer: line extracted");
        std::string p2 = aatpe::c4freezer::parseV1FreezerPath("0::/uid_10123/cg_1\n");
        CHECK(p2.empty(), "V1: v2-only -> empty");
        std::string p3 = aatpe::c4freezer::parseV1FreezerPath(
            "2:cpu,freezer:/uid_10123/bg\n");
        CHECK(p3 == "uid_10123/bg", "V1: combined ctrl line parsed");
        std::string p4 = aatpe::c4freezer::parseV1FreezerPath("2:cpu:/uid_10123\n");
        CHECK(p4.empty(), "V1: cpu-only -> empty");

        CHECK(aatpe::c4freezer::parseV1FreezerState("FROZEN\n") == "FROZEN",
              "V1: FROZEN parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerState("THAWED\n") == "THAWED",
              "V1: THAWED parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerState("FREEZING\n") == "FREEZING",
              "V1: FREEZING parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerState("garbage") == "",
              "V1: unknown -> empty");

        CHECK(aatpe::c4freezer::isV1FreezerDir("/sys/fs/cgroup/freezer/uid_10123"),
              "V1: /freezer/ path detected as v1");
        CHECK(!aatpe::c4freezer::isV1FreezerDir("/sys/fs/cgroup/uid_10123"),
              "V1: v2 path (no /freezer/) not v1");
        CHECK(!aatpe::c4freezer::isV1FreezerDir(""),
              "V1: empty path -> not v1");

        const char* DEVICE_MOUNTS =
            "none /dev/cg2_bpf cgroup2 rw,seclabel,nosuid,nodev,noexec,relatime 0 0\n"
            "none /sys/fs/cgroup tmpfs rw,seclabel,relatime,mode=750,gid=1000 0 0\n"
            "none /sys/fs/cgroup/freezer cgroup rw,relatime,freezer 0 0\n"
            "none /sys/fs/cgroup/cpuset cgroup rw,relatime,cpuset,noprefix,release_agent=/sbin/cgroup_release_agent 0 0\n";
        CHECK(aatpe::c4freezer::parseV1FreezerMountPoint(DEVICE_MOUNTS) ==
                  "/sys/fs/cgroup/freezer",
              "V1: /proc/mounts freezer mount point extracted");
        CHECK(aatpe::c4freezer::parseV1FreezerMountPoint(
                  "cgroup2 /sys/fs/cgroup cgroup2 ro,nsdelegate 0 0\n") == "",
              "V1: cgroup2 vfstype ignored (v1 parser)");
        CHECK(aatpe::c4freezer::parseV1FreezerMountPoint(
                  "none /sys/fs/cgroup/cpu cgroup rw,relatime,cpu 0 0\n") == "",
              "V1: cgroup w/o freezer -> empty");
        CHECK(aatpe::c4freezer::parseV1FreezerMountPoint(
                  "none /sys/fs/cgroup/mix cgroup rw,relatime,cpu,freezer 0 0\n") ==
                  "/sys/fs/cgroup/mix",
              "V1: combined cpu,freezer mount parsed");
        CHECK(aatpe::c4freezer::parseV1FreezerMountPoint("") == "",
              "V1: empty mounts -> empty");
    }

    printf("=== v6.2.0: dual-bucket pure helpers ===\n");
    {
        aatpe::c4freezer::C4BucketPaths bp =
            aatpe::c4freezer::buildV1BucketPaths("/sys/fs/cgroup/freezer");
        CHECK(bp.valid, "BUCKET: buildV1BucketPaths valid");
        CHECK(bp.frozenDir == "/sys/fs/cgroup/freezer/perf/frozen",
              "BUCKET: frozen dir built");
        CHECK(bp.thawedDir == "/sys/fs/cgroup/freezer/perf/thawed",
              "BUCKET: thawed dir built");
        aatpe::c4freezer::C4BucketPaths bp2 =
            aatpe::c4freezer::buildV1BucketPaths("/sys/fs/cgroup/freezer/");
        CHECK(bp2.frozenDir == "/sys/fs/cgroup/freezer/perf/frozen",
              "BUCKET: trailing slash trimmed");
        aatpe::c4freezer::C4BucketPaths bp3 =
            aatpe::c4freezer::buildV1BucketPaths("");
        CHECK(!bp3.valid, "BUCKET: empty root -> invalid");

        CHECK(aatpe::c4freezer::isV1FrozenBucketDir("/sys/fs/cgroup/freezer/perf/frozen"),
              "BUCKET: /perf/frozen detected");
        CHECK(!aatpe::c4freezer::isV1FrozenBucketDir("/sys/fs/cgroup/freezer/perf/thawed"),
              "BUCKET: thawed dir NOT a frozen bucket");
        CHECK(!aatpe::c4freezer::isV1FrozenBucketDir("/sys/fs/cgroup/freezer/uid_10123"),
              "BUCKET: uid_<uid> dir NOT a bucket");
        CHECK(!aatpe::c4freezer::isV1FrozenBucketDir(""),
              "BUCKET: empty -> false");

        CHECK(aatpe::c4freezer::thawedBucketFromFrozen(
                  "/sys/fs/cgroup/freezer/perf/frozen") ==
                  "/sys/fs/cgroup/freezer/perf/thawed",
              "BUCKET: thawed derived from frozen");
        CHECK(aatpe::c4freezer::thawedBucketFromFrozen(
                  "/sys/fs/cgroup/freezer/uid_10123").empty(),
              "BUCKET: non-frozen dir -> empty");
    }

    printf("=== v6.2.0: pids list parse/format ===\n");
    {
        std::vector<int> p = aatpe::c4freezer::parsePidsList("1234,5678,9012");
        CHECK(p.size() == 3 && p[0] == 1234 && p[2] == 9012,
              "PIDS: comma list parsed");
        CHECK(aatpe::c4freezer::formatPidsList(p) == "1234,5678,9012",
              "PIDS: list formatted");
        std::vector<int> p2 = aatpe::c4freezer::parsePidsList(" 100 , 200 , ,abc,300");
        CHECK(p2.size() == 3 && p2[0] == 100 && p2[2] == 300,
              "PIDS: whitespace/garbage tokens skipped");
        CHECK(aatpe::c4freezer::parsePidsList("").empty(), "PIDS: empty -> empty");
        CHECK(aatpe::c4freezer::formatPidsList({}).empty(), "PIDS: empty vector -> empty");
        CHECK(aatpe::c4freezer::parsePidsList("0,-5").empty(),
              "PIDS: pid 0 and negative dropped -> empty");
    }

    printf("=== v6.2.0: pids serialize/parse round-trip + backward compat ===\n");
    {
        std::string s = aatpe::c4freezer::serializeC4Frozen(
            "/sys/fs/cgroup/freezer/perf/frozen", "com.tencent.qq", 10123,
            "com.tencent.qq", {1234, 5678, 9012});
        aatpe::c4freezer::C4FrozenState st = aatpe::c4freezer::parseC4Frozen(s);
        CHECK(st.valid, "PERSIST: bucket record valid");
        CHECK(st.cgDir == "/sys/fs/cgroup/freezer/perf/frozen", "PERSIST: cgDir preserved");
        CHECK(st.pids.size() == 3 && st.pids[1] == 5678, "PERSIST: pids round-trip");
        CHECK(s.find("pids=1234,5678,9012") != std::string::npos,
              "PERSIST: pids= line written");
        std::string s2 = aatpe::c4freezer::serializeC4Frozen(
            "/sys/fs/cgroup/uid_10123/cg_1", "com.game", 10123, "com.game");
        CHECK(s2.find("pids=") == std::string::npos,
              "PERSIST: empty pids omits pids=");
        aatpe::c4freezer::C4FrozenState st2 = aatpe::c4freezer::parseC4Frozen(s2);
        CHECK(st2.valid && st2.pids.empty(),
              "PERSIST: old format parses, pids empty (backward compat)");
        aatpe::c4freezer::C4FrozenState st3 = aatpe::c4freezer::parseC4Frozen(
            "cg=/sys/fs/cgroup/uid_10123/cg_1\npkg=com.game\nuid=10123\n");
        CHECK(st3.valid && st3.pids.empty() && st3.lastGame.empty(),
              "PERSIST: v4.4.5 file w/o lg=/pids= still valid");
    }

    printf("=== v6.2.0: procCgroupOnFrozenBucket ===\n");
    {
        const char* frozen =
            "5:freezer:/perf/frozen\n2:cpu:/perf\n";
        CHECK(aatpe::c4freezer::procCgroupOnFrozenBucket(frozen),
              "READBACK: freezer:/perf/frozen recognized");
        const char* thawed =
            "5:freezer:/perf/thawed\n";
        CHECK(!aatpe::c4freezer::procCgroupOnFrozenBucket(thawed),
              "READBACK: thawed bucket NOT frozen");
        const char* uid =
            "5:freezer:/uid_10123/cg_1\n";
        CHECK(!aatpe::c4freezer::procCgroupOnFrozenBucket(uid),
              "READBACK: per-uid cgroup NOT bucket");
    }

    printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
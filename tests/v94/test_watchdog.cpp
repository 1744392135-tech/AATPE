#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <chrono>
#define main watchdog_main_unused
#include "aatpe-watchdog.cpp"
#undef main
static int g_pass=0,g_fail=0;
#define CHECK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)
int main(){
    printf("=== Watchdog: isCriticalThreadComm whitelist (RC6 C3: zero vendor names) ===\n");
    CHECK(isCriticalThreadComm("surfaceflinger"), "surfaceflinger critical");
    CHECK(isCriticalThreadComm("RenderThread"), "RenderThread critical");
    CHECK(isCriticalThreadComm("UnityMain"), "UnityMain critical");
    CHECK(isCriticalThreadComm("mali-csf"), "GPU driver worker critical");
    CHECK(!isCriticalThreadComm("tencent.mobilepubg"), "C3: vendor 'tencent' NOT auto-critical");
    CHECK(!isCriticalThreadComm("com.miHoYo.genshin"), "C3: vendor 'miHoYo' NOT auto-critical");
    CHECK(!isCriticalThreadComm("com.tencent.pubg"), "C3: vendor 'pubg' NOT auto-critical");
    CHECK(!isCriticalThreadComm("com.activision.codm"), "C3: vendor 'codm' NOT auto-critical");
    CHECK(isCriticalThreadComm("com.miHoYo.genshin:RenderThread"), "C3: vendor pkg + generic RenderThread still critical");
    CHECK(!isCriticalThreadComm("some.helper"), "helper thread not critical");
    CHECK(!isCriticalThreadComm("logd"), "logd not critical");

    printf("=== Watchdog: trim ===\n");
    { std::string s = "  hello  "; trim(s); CHECK(s=="hello", "trim removes whitespace"); }
    { std::string s = "  "; trim(s); CHECK(s.empty(), "trim all-whitespace -> empty"); }

    printf("=== Watchdog: readConfigInt defaults ===\n");
    CHECK(readConfigInt("WATCHDOG_INTERVAL", 5)==5, "missing config -> default");
    CHECK(readConfigInt("NOSUCHKEY", -7)==-7, "unknown key -> default");

    printf("=== Watchdog: getCpuCount & detectBigCorePool ===\n");
    int n = getCpuCount();
    CHECK(n >= 1, "getCpuCount returns positive");
    std::vector<int> pool = detectBigCorePool();
    CHECK(!pool.empty(), "detectBigCorePool non-empty");
    printf("  [INFO] cpuCount=%d bigpool size=%zu\n", n, pool.size());

    printf("=== Watchdog: WD-13 countCpuDirs sysfs enumeration ===\n");
    {
        char tmp[] = "/tmp/wd_sysfs_XXXXXX";
        mkdir(tmp, 0755);
        std::string base = tmp;
        ::system(("rm -rf " + base + " && mkdir -p " + base + "/devices/system/cpu").c_str());
        for (int c = 0; c < 12; c++) {
            ::mkdir((base + "/devices/system/cpu/cpu" + std::to_string(c)).c_str(), 0755);
        }
        ::mkdir((base + "/devices/system/cpu/cpufreq").c_str(), 0755);
        int fd = ::open((base + "/devices/system/cpu/kernel_notes").c_str(),
                        O_CREAT|O_WRONLY, 0644);
        if (fd >= 0) ::close(fd);
        int got = countCpuDirs(base.c_str());
        CHECK(got == 12, "WD-13: countCpuDirs counts 12 cpuN dirs (not hardcoded 8)");
        CHECK(got > 8, "WD-13: enumeration exceeds old hardcoded fallback of 8");
        int zero = countCpuDirs("/nonexistent/path/xyz");
        CHECK(zero == 0, "WD-13: missing sysfs root returns 0 (caller falls back)");
        ::system(("rm -rf " + base).c_str());
    }

    printf("=== Watchdog: T1 selectBigPoolFromCapacities (PRIME exclusion) ===\n");
    {
        auto hasCpu = [](const std::vector<int>& v, int c){
            return std::find(v.begin(), v.end(), c) != v.end();
        };
        {
            std::vector<std::pair<int,int>> caps = {
                {300,0},{300,1},{300,2},{300,3},{800,4},{800,5},{800,6},{1024,7}};
            auto p = selectBigPoolFromCapacities(caps);
            CHECK(!hasCpu(p,7), "D1100: PRIME cpu7 excluded");
            CHECK(hasCpu(p,4)&&hasCpu(p,5)&&hasCpu(p,6), "D1100: BIG cpu4-6 selected");
            CHECK(!hasCpu(p,0), "D1100: LITTLE cpu0 not selected");
        }
        {
            std::vector<std::pair<int,int>> caps = {
                {256,0},{256,1},{256,2},{256,3},{1024,4},{1024,5},{1024,6},{1024,7}};
            auto p = selectBigPoolFromCapacities(caps);
            CHECK(p.size()==4, "845: exactly the 4 big cores");
            CHECK(hasCpu(p,4)&&hasCpu(p,7), "845: big cores selected");
            CHECK(!hasCpu(p,0), "845: little cores excluded");
        }
        {
            std::vector<std::pair<int,int>> caps = {
                {1024,0},{1024,1},{1024,2},{1024,3},{1024,4},{1024,5},{1024,6},{1024,7}};
            auto p = selectBigPoolFromCapacities(caps);
            CHECK(p.size()==8, "homogeneous: all 8 cores selected");
        }
        {
            std::vector<std::pair<int,int>> caps = {
                {300,0},{300,1},{300,2},{300,3},{600,4},{800,5},{800,6},{1024,7}};
            auto p = selectBigPoolFromCapacities(caps);
            CHECK(!hasCpu(p,7), "weird: PRIME cpu7 excluded");
            CHECK(hasCpu(p,5)&&hasCpu(p,6), "weird: BIG cpu5-6 selected");
            CHECK(!hasCpu(p,4), "weird: MID cpu4 not selected");
        }
        {
            std::vector<std::pair<int,int>> caps = {
                {800,0},{800,1},{800,2},{800,3},{1024,6},{1024,7}};
            auto p = selectBigPoolFromCapacities(caps);
            CHECK(!hasCpu(p,6)&&!hasCpu(p,7), "2xPRIME: both prime cores excluded");
            CHECK(p.size()==4, "2xPRIME: the 4 big cores selected");
        }
    }

    printf("=== Watchdog: rotateLogIfNeeded ===\n");
    FILE* fp = fopen("/tmp/wdlog_test.txt","w"); fputs("tiny\n",fp); fclose(fp);
    fp = fopen("/tmp/wdlog_test.txt","a");
    ftell(fp);
    rotateLogIfNeeded(&fp);
    CHECK(fp != nullptr, "tiny log: no rotation, fp valid");
    fclose(fp);

    printf("=== Watchdog: signal handler sets exit flag ===\n");
    g_shouldExit.store(0);
    signalHandler(SIGTERM);
    CHECK(g_shouldExit.load()==1, "SIGTERM sets g_shouldExit");
    signalHandler(SIGUSR1);
    CHECK(g_shouldExit.load()==1, "non-terminating signal leaves flag set");

    printf("=== Watchdog: G9 state machine transition edges ===\n");
    const int recStable = watchdog::kRecoveryStableSec;
    const int emClear   = 30;
    WdEvidence ev;
    ev.daemonAlive = true; ev.migOver = false; ev.loadOver = false; ev.explicitStop = false;

    ev.thermalEmergency = true;
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 1000, 0, 0, recStable, emClear)
          == WdRuntimeState::EMERGENCY, "EMERGENCY: thermal still active -> stays EMERGENCY");

    ev.thermalEmergency = false;
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 1000, 1000, 0, recStable, emClear)
          == WdRuntimeState::EMERGENCY, "EMERGENCY: just cleared, window not elapsed -> stays");
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 1040, 1000, 0, recStable, emClear)
          == WdRuntimeState::RECOVERY, "EMERGENCY: clear window elapsed -> RECOVERY");
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 1020, 1000, 0, recStable, emClear)
          == WdRuntimeState::EMERGENCY, "EMERGENCY: clear 20s < 30s -> stays (anti-flap)");

    ev.thermalEmergency = false;
    CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1010, 0, 1000, recStable, emClear)
          == WdRuntimeState::RECOVERY, "RECOVERY: 10s clean -> stays RECOVERY");
    CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1040, 0, 1000, recStable, emClear)
          == WdRuntimeState::NORMAL, "RECOVERY: 40s clean -> NORMAL");

    ev.migOver = true;
    CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 1040, 0, 1000, recStable, emClear)
          == WdRuntimeState::PASSIVE, "RECOVERY: mig over -> back to PASSIVE");
    ev.migOver = false;

    ev.explicitStop = true;
    ev.thermalEmergency = false;
    CHECK(wdSteadyTransition(WdRuntimeState::RECOVERY, ev, 999999, 0, 0, recStable, emClear)
          == WdRuntimeState::RECOVERY, "explicit stop freezes state (no promotion)");
    ev.explicitStop = false;

    CHECK(explicitStopRequested() == false, "stop_request absent -> not requested");

    printf("=== Watchdog: ROUND-003 independent thermal exit ===\n");
    const int wall = 90, margin = 5;
    CHECK(wdClassifyIndTemp(80, wall, margin) == WdTempRead::SAFE, "80C (<=85) = SAFE");
    CHECK(wdClassifyIndTemp(88, wall, margin) == WdTempRead::UNKNOWN, "88C (grey band) = UNKNOWN (hold)");
    CHECK(wdClassifyIndTemp(95, wall, margin) == WdTempRead::HOT, "95C = HOT");
    CHECK(wdClassifyIndTemp(-1, wall, margin) == WdTempRead::UNKNOWN, "no zone readable = UNKNOWN (fail-safe)");

    printf("=== Watchdog: rel15 wdComputeEmergencyWallC mirrors the daemon wall ===\n");
    CHECK(wdComputeEmergencyWallC(0, 0, 0) == 120,
          "no config -> 120C (matches daemon default, NOT the old 90C)");
    CHECK(wdComputeEmergencyWallC(105, 120, 0) == 105,
          "THERMAL_EMERGENCY_THRESHOLD=105 wins over THERMAL_EMERGENCY=120");
    CHECK(wdComputeEmergencyWallC(0, 110, 0) == 110,
          "threshold unset -> THERMAL_EMERGENCY=110 used");
    CHECK(wdComputeEmergencyWallC(150, 0, 0) == 120,
          "threshold=150 clamped to 120 hard ceiling");
    CHECK(wdComputeEmergencyWallC(0, 0, 80) == 120,
          "override=80 (tighter than daemon 120) IGNORED -- no second wall fighting");
    CHECK(wdComputeEmergencyWallC(0, 0, 130) == 130,
          "override=130 (looser) honored -- operator may relax backup scan");
    CHECK(wdComputeEmergencyWallC(110, 0, 90) == 110,
          "daemon wall=110, override=90 tighter -> kept 110 (daemon wins)");
    CHECK(wdComputeEmergencyWallC(110, 0, 115) == 115,
          "daemon wall=110, override=115 looser -> 115");

    int streak = 0;
    for (int i = 0; i < 9; ++i) wdIndTempSample(WdTempRead::SAFE, streak, 10);
    CHECK(streak == 9, "9 SAFE samples -> streak 9 (not yet met)");
    CHECK(wdIndTempSample(WdTempRead::SAFE, streak, 10) == true, "10th SAFE sample -> forced clear");
    ev.thermalEmergency = false;
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 1040, 1000, 0, recStable, emClear)
          == WdRuntimeState::RECOVERY, "(a) flag=1 + independent cool window -> RECOVERY");

    streak = 0;
    wdIndTempSample(WdTempRead::SAFE, streak, 10);
    CHECK(wdIndTempSample(WdTempRead::HOT, streak, 10) == false && streak == 0,
          "(b) HOT sample resets streak -> no clear");
    ev.thermalEmergency = true;
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 99999, 99990, 0, recStable, emClear)
          == WdRuntimeState::EMERGENCY, "(b) independent HOT -> stays EMERGENCY");

    streak = 0;
    wdIndTempSample(WdTempRead::SAFE, streak, 10);
    CHECK(wdIndTempSample(WdTempRead::UNKNOWN, streak, 10) == false && streak == 0,
          "(c) unreadable zones reset streak -> no clear");
    ev.thermalEmergency = true;
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 99999, 99990, 0, recStable, emClear)
          == WdRuntimeState::EMERGENCY, "(c) unreadable temp -> fail-safe stays EMERGENCY");

    ev.thermalEmergency = false;
    CHECK(wdSteadyTransition(WdRuntimeState::EMERGENCY, ev, 1005, 1000, 0, recStable, emClear)
          == WdRuntimeState::EMERGENCY, "(d) flag=0 but 5s<30s window -> stays (no regression)");

    printf("=== Watchdog: ROUND-011 unkillable D-state escalation ===\n");
    CHECK(unkillableStreakTriggered(0)==false, "0 kill-fail cycles -> no escalation");
    CHECK(unkillableStreakTriggered(2)==false, "2 cycles (<3) -> no escalation");
    CHECK(unkillableStreakTriggered(3)==true,  "3 cycles -> escalate to UnkillableDstate");
    CHECK(unkillableStreakTriggered(9)==true, "9 cycles -> keep re-killing every cycle");

    CHECK(thermalStaleTriggered(0)==false, "0 stale cycles -> no degraded");
    CHECK(thermalStaleTriggered(4)==false, "4 stale (<5) -> no degraded");
    CHECK(thermalStaleTriggered(5)==true,  "5 stale cycles -> watchdog_degraded");
    CHECK(thermalStaleTriggered(9)==true,  "9 stale -> stays degraded");

    printf("=== Watchdog: WD-02 aggregated watchdog_degraded reasons ===\n");
    g_degradedSet.clear();
    CHECK(degradedReasonsString()=="none", "empty set -> 'none'");
    addDegradedReason("rapid_crash_loop");
    CHECK(degradedReasonsString()=="rapid_crash_loop", "one reason -> itself");
    addDegradedReason("thermal_thread_stale");
    std::string agg = degradedReasonsString();
    CHECK(agg.find("rapid_crash_loop") != std::string::npos, "aggregated keeps rapid_crash_loop");
    CHECK(agg.find("thermal_thread_stale") != std::string::npos, "aggregated adds thermal_thread_stale");
    addDegradedReason("thermal_thread_stale");
    CHECK(degradedReasonsString()==agg, "re-add existing reason is idempotent (no dup)");
    removeDegradedReason("thermal_thread_stale");
    CHECK(degradedReasonsString()=="rapid_crash_loop", "thermal removed, rapid remains");
    addDegradedReason("binary_broken");
    agg = degradedReasonsString();
    CHECK(agg.find("rapid_crash_loop")!=std::string::npos &&
          agg.find("binary_broken")!=std::string::npos, "binary_broken + rapid coexist");
    clearAllDegradedReasons();
    CHECK(degradedReasonsString()=="none", "clearAll -> none");

    printf("\n========================================\nWatchdog results: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail==0?0:1;
}

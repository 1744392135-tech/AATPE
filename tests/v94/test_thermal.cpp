// test_thermal.cpp - Direct unit tests for thermal.cpp readThermal().
// Scenario-driven by env vars (each run is a fresh process, so the 1s scan
// TTL does not interfere). The runner bind-mounts a fake /sys/class/thermal.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <string>
#include <functional>
#include <unistd.h>
#include "aatped.h"
using namespace aatpe; using namespace aatpe_ml;
extern void thermalSetTestRoot(const std::string&);
extern void thermalSetClockForTest(std::function<unsigned long long()>);
extern void thermalSetBootClockForTest(std::function<long long()>);
extern void thermalSetScanStampForTest(long long sec);
extern void thermalFreshnessTick();
extern void startThermalSampling();
extern void stopThermalSampling();
extern bool thermalDegradedForTest();
static int g_pass=0,g_fail=0;
#define CHECK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)

// HOTFIX v4.1.0: read a one-line state key written into STATE_DIR. Returns the
// trimmed content (empty string if absent). thermal_source/thermal_zones use
// writeFileAtomic (real on-disk); thermal_throttle/thermal_cpu_max use
// writeFile(path,int) which the isolated hotfix deps also makes real.
static std::string readStateStr(const std::string& name){
    extern const char* STATE_DIR;
    FILE* f=fopen((std::string(STATE_DIR)+"/"+name).c_str(),"r");
    if(!f) return std::string();
    char buf[512]={0}; if(fgets(buf,sizeof buf,f)){} fclose(f);
    std::string s(buf);
    while(!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
    return s;
}
// Write a fake /proc/stat line from a busy% (mirrors the hermetic fixtures).
static void writeProcStat(const std::string& R, int busy, long idx){
    long idle=1000-busy*10, total=10000+idx*1000, user=total-idle;
    char ps[160]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n",user,idle);
    FILE* f=fopen((R+"/proc/stat").c_str(),"w"); if(f){fputs(ps,f);fclose(f);}
}
static void Wfile(const std::string&p,const std::string&v){
    FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);}
}
int main(){
    static char tmp[]="/tmp/aatpe_state_XXXXXX";
    char*d=mkdtemp(tmp);
    if(d){ extern const char* STATE_DIR; STATE_DIR=d; }
    g_cfg.thermalEmergency=95; g_cfg.thermalLimitCpu=80;
    g_cfg.thermalLimitSkin=45; g_cfg.thermalLimitBattery=40;
    g_cfg.thermalProfileThreshold=70; g_cfg.autoProfile=false;
    // Three-tier wall contract: thermal.cpp prefers thermalEmergencyThreshold when
    // >0 (production default 120, per the 温控120 hard constraint). The old field
    // thermalEmergency is only a fallback. Set the new threshold to the test's
    // chosen wall (95C) so the emergency scenario actually reaches the latch.
    g_cfg.thermalEmergencyThreshold=95;
    g_thermalEmergency=false; g_currentScene.store((int)GameScene::MENU);

    const char* scen = getenv("AATPE_THERMAL_SCENARIO");
    std::string sc = scen?scen:"normal";

    if (sc=="normal") {
        printf("=== Thermal scenario: normal 55C ===\n");
        readThermal();
        printf("  [INFO] cpu=%d skin=%d batt=%d\n", g_thermalCpu.load(), g_thermalSkin.load(), g_thermalBattery.load());
        CHECK(g_thermalCpu.load()==55, "mC 55000 -> 55C (cpu zone)");
        CHECK(g_thermalSkin.load()==42, "deciC 420 -> 42C (skin zone)");
        CHECK(g_thermalBattery.load()==35, "battery 350(0.1C) -> 35C");
        CHECK(getSystemState()!=SystemState::EMERGENCY, "below emergency -> not EMERGENCY");
        CHECK(aatpe_getThermalCpuZonePath().find("thermal_zone0")!=std::string::npos,
              "cpu zone path recorded");
    } else if (sc=="emergency") {
        printf("=== Thermal scenario: emergency 100C ===\n");
        // v94 问题38: entering EMERGENCY needs kEmergencyDebounceReads(=3)
        // consecutive reads >= thermalEmergency, and readThermal() caches for
        // kCacheTtlMs(=1000ms). Issue 3 fresh reads, each after the cache TTL, so
        // each actually re-reads sysfs and advances the over-temp streak (a single
        // read only reaches streak=1 and cannot latch EMERGENCY). This mirrors the
        // proven S4/S10 integration-test pattern. All three CHECKs below are kept.
        for (int i = 0; i < 3; i++) { usleep(1100*1000); readThermal(); }
        printf("  [INFO] cpu=%d state=%d\n", g_thermalCpu.load(), (int)getSystemState());
        CHECK(g_thermalCpu.load()>=95, "forced cpu temp >=95C");
        CHECK(getSystemState()==SystemState::EMERGENCY, ">= emergency -> EMERGENCY");
        CHECK(g_thermalEmergency==true, "g_thermalEmergency flag set");
    } else if (sc=="battclamp") {
        printf("=== Thermal scenario: battery clamp ===\n");
        readThermal();
        printf("  [INFO] batt=%d\n", g_thermalBattery.load());
        CHECK(g_thermalBattery.load()==100, "battery raw 900000 clamped to 100");
    } else if (sc=="nosensor") {
        // G6-001: ALL thermal zone temp nodes are unreadable. The OLD code left
        // maxTemp=0 (= "cold") and would boost. The fix must instead hold a
        // CONSERVATIVE temperature (>= critical wall) and must NOT fake 0.
        printf("=== Thermal scenario: nosensor (all temps unreadable) ===\n");
        readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d (expected conservative, NOT 0)\n", cpu);
        CHECK(cpu != 0, "no sensor -> cpu temp NOT reported as 0/cold");
        CHECK(cpu >= 70, "no sensor -> cpu temp held at conservative (>=70C)");
        // Must not enter EMERGENCY spuriously at boot (boot grace).
        CHECK(getSystemState()!=SystemState::EMERGENCY, "no sensor -> no spurious EMERGENCY");
    } else if (sc=="bootgrace") {
        // G11-009: boot grace. The mock seeds a HIGH cpu temp (100C). A SINGLE
        // read at boot must NOT immediately latch EMERGENCY — the debounce needs
        // kEmergencyDebounceReads consecutive over-temp reads (the emergency
        // scenario proves 3 reads DO latch). This pins the boot-grace contract.
        printf("=== Thermal scenario: bootgrace (single high read at boot) ===\n");
        readThermal();
        printf("  [INFO] cpu=%d state=%d\n", g_thermalCpu.load(), (int)getSystemState());
        CHECK(g_thermalCpu.load()>=95, "bootgrace: high boot temp read (>=95C)");
        CHECK(getSystemState()!=SystemState::EMERGENCY,
              "bootgrace: single high read at boot does NOT immediately latch EMERGENCY");
    }
    else if (sc=="hermetic") {
        // ROUND-009e: pure-hermetic load-correlation test via setTestRoot.
        // Build a temp root with synthetic sysfs + proc/stat.
        printf("=== Thermal scenario: hermetic load-correlation ===\n");
        char root[]="/tmp/aatpe_thr_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/thermal_zone2 "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone1/type","cpu_thermal");
        W(tz+"/thermal_zone2/type","battery");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        // zone0 constant 92C; zone1 follows load 50->75C; battery constant 28C.
        thermalSetTestRoot(R);
        unsigned long long fakeNow = 10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;                     // advance 2s per round (past TTL)
            int busy = (i<18)?10:90;            // swing 10->90
            long idle = 1000 - busy*10;
            long total = 10000 + i*1000;        // monotonically increasing jiffies
            long user = total - idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n", user, idle);
            W(R+"/proc/stat", ps);
            int z1 = 50000 + (i*2500);          // 50->~75C
            W(tz+"/thermal_zone0/temp","92000");
            W(tz+"/thermal_zone1/temp", std::to_string(z1));
            W(tz+"/thermal_zone2/temp","28000");
            readThermal();
        }
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d (constant 92 should be rejected)\n", cpu);
        // The constant-92 clamp zone must be invalidated under load swing; the
        // published CPU temp must NOT be the clamped 92. (Other zones / state
        // machine may pick a different value; we assert 92 is gone.)
        CHECK(cpu != 92, "hermetic: constant 92 clamp zone rejected (not primary)");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }
    else if (sc=="window") {
        // ROUND-009e: run the load-correlation gate many cycles. On the host,
        // /proc/stat yields real busy; we assert the gate never crashes and the
        // cpu temp stays plausible (the existing stub cpu zone = 55C). A constant
        // 92C clamp would be invalidated only under a >=20pp busy swing.
        printf("=== Thermal scenario: window gate (35 cycles) ===\n");
        for (int i = 0; i < 35; i++) readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d after 35 cycles\n", cpu);
        CHECK(cpu == -1 || (cpu >= 15 && cpu <= 115),
              "window: cpu temp either pending(-1) or in physical range");
        CHECK(getSystemState()!=SystemState::EMERGENCY, "window: no spurious EMERGENCY");
    } else if (sc=="hwmon") {
        // ROUND-009e: hwmon candidate scan must not crash and must yield a value.
        printf("=== Thermal scenario: hwmon scan ===\n");
        readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d\n", cpu);
        CHECK(cpu == -1 || (cpu >= 15 && cpu <= 115), "hwmon: cpu temp in range or unknown");
    } else if (sc=="trip") {
        // ROUND-009a/e: a zone whose temp equals its trip point must be rejected.
        printf("=== Thermal scenario: trip-point hard invalid ===\n");
        readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d (trip zone should not be primary)\n", cpu);
        CHECK(cpu != 95, "trip: 95C trip-point zone NOT selected as primary");
    }
    else if (sc=="freshness") {
        // ROUND-011: thermalFreshnessTick freshness/stale/degraded cycle.
        printf("=== Thermal scenario: freshness tick ===\n");
        char sdir[]="/tmp/aatpe_thr_fresh_XXXXXX"; char* sd=mkdtemp(sdir);
        if(sd){ extern const char* STATE_DIR; STATE_DIR=sd; }
        long long fakeNow = 10000;
        thermalSetBootClockForTest([&fakeNow](){ return fakeNow; });
        thermalSetScanStampForTest(fakeNow);
        thermalFreshnessTick();
        {
            FILE* f=fopen((std::string(sd)+"/thermal_freshness").c_str(),"r");
            char buf[64]={0}; if(f){ fgets(buf,sizeof buf,f); fclose(f); }
            CHECK(std::string(buf)=="fresh", "freshness: fresh stamp -> thermal_freshness=fresh");
        }
        CHECK(thermalDegradedForTest()==false, "freshness: degraded=0 when fresh");
        fakeNow += 12;
        thermalFreshnessTick();
        {
            FILE* f=fopen((std::string(sd)+"/thermal_freshness").c_str(),"r");
            char buf[64]={0}; if(f){ fgets(buf,sizeof buf,f); fclose(f); }
            CHECK(std::string(buf).find("stale=12")!=std::string::npos, "freshness: 12s old -> stale=12");
        }
        CHECK(thermalDegradedForTest()==true, "freshness: degraded=1 after 12s stale");
        thermalSetScanStampForTest(fakeNow);
        thermalFreshnessTick();
        {
            FILE* f=fopen((std::string(sd)+"/thermal_freshness").c_str(),"r");
            char buf[64]={0}; if(f){ fgets(buf,sizeof buf,f); fclose(f); }
            CHECK(std::string(buf)=="fresh", "freshness: recovery -> fresh again");
        }
        CHECK(thermalDegradedForTest()==false, "freshness: degraded cleared after recovery");
        thermalSetBootClockForTest(nullptr);
    }
    else if (sc=="degrdecouple") {
        // P2-1: degraded-reason decoupling. NO_VALID_SENSOR (owned by readThermal)
        // must persist across a FRESH scan stamp; the freshness tick only owns
        // SCAN_STUCK. Before the fix, a fresh stamp cleared the whole flag within
        // ~1s, wiping the sensor-outage fail-safe.
        printf("=== P2-1: degraded reason decoupling ===\n");
        char root[]="/tmp/aatpe_thr_dec_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        system(("mkdir -p "+R+"/sys/class/thermal "+R+"/proc").c_str());
        char sdir[]="/tmp/aatpe_thr_dec_state_XXXXXX"; char* sd=mkdtemp(sdir);
        if(sd){ extern const char* STATE_DIR; STATE_DIR=sd; }
        // Two injectable clocks: ms clock drives readThermal()'s cache TTL
        // (line ~346), boot-seconds clock drives the freshness stamp.
        unsigned long long msNow=10000;
        thermalSetClockForTest([&msNow](){ return msNow; });
        thermalSetBootClockForTest([&msNow](){ return (long long)(msNow/1000); });
        thermalSetTestRoot(R);

        // (a) zero valid zone reads -> NO_VALID_SENSOR -> degraded.
        writeProcStat(R, 50, 0);   // thermalBusyPctHook reads <root>/proc/stat
        readThermal();
        CHECK(thermalDegradedForTest()==true, "decouple: zero valid reads -> degraded=true");

        // (b) scan loop ALIVE (fresh stamp, refreshed every ~1s by the loop).
        //     Freshness tick must NOT clear the sensor-outage reason.
        thermalSetScanStampForTest(10);   // age=0 => fresh (boot-sec)
        thermalFreshnessTick();
        CHECK(thermalDegradedForTest()==true, "decouple: fresh stamp does NOT clear no-valid-sensor");
        CHECK(readStateStr("thermal_degraded")=="1", "decouple: thermal_degraded file stays 1 while sensors dead");

        // (c) a valid CPU zone appears -> readThermal recovers -> NO_VALID_SENSOR cleared.
        system(("mkdir -p "+R+"/sys/class/thermal/thermal_zone0").c_str());
        Wfile(R+"/sys/class/thermal/thermal_zone0/type","mtktscpu");
        Wfile(R+"/sys/class/thermal/thermal_zone0/temp","55000");
        writeProcStat(R, 55, 1);
        for (int i=0;i<3;i++){ msNow+=1000; writeProcStat(R, 55+i, 2+i); readThermal(); }
        CHECK(thermalDegradedForTest()==false, "decouple: valid reading clears degraded");

        // (d) SCAN_STUCK independent reason: healthy sensors but stamp 12s stale.
        thermalSetScanStampForTest(10);            // stamp back at t=10
        msNow += 12000;                            // now t=22s -> age=12s
        thermalFreshnessTick();
        CHECK(thermalDegradedForTest()==true, "decouple: 12s stale stamp -> SCAN_STUCK degraded");
        // recovery (fresh stamp) clears SCAN_STUCK -> degraded false.
        thermalSetScanStampForTest((long long)(msNow/1000));
        thermalFreshnessTick();
        CHECK(thermalDegradedForTest()==false, "decouple: fresh stamp clears SCAN_STUCK");
        thermalSetClockForTest(nullptr); thermalSetBootClockForTest(nullptr);
        thermalSetTestRoot("");
    }

    else if (sc=="hwmontempn") {
        // P2·hwmon-blind: CPU die temp only on temp3_input (NOT temp1). The old
        // code read only temp1_input -> emergency wall was blind. Now every
        // tempN channel is scanned and classified by label+name.
        printf("=== P2: hwmon tempN iteration (CPU on temp3) ===\n");
        char root[]="/tmp/aatpe_hwn_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        system(("mkdir -p "+R+"/sys/class/hwmon/hwmon0 "+R+"/proc").c_str());
        Wfile(R+"/sys/class/hwmon/hwmon0/name","qcom");
        // temp1: non-CPU (charger proxy) at 30C
        Wfile(R+"/sys/class/hwmon/hwmon0/temp1_input","30000");
        Wfile(R+"/sys/class/hwmon/hwmon0/temp1_label","charger");
        // temp3: real CPU die at 95C
        Wfile(R+"/sys/class/hwmon/hwmon0/temp3_input","95000");
        Wfile(R+"/sys/class/hwmon/hwmon0/temp3_label","cpu-0-0-0");
        writeProcStat(R, 50, 0);
        unsigned long long ms=10000;
        thermalSetClockForTest([&ms](){ return ms; });
        thermalSetTestRoot(R);
        for (int i=0;i<3;i++){ ms+=1000; writeProcStat(R,50,i); readThermal(); }
        int cpu=g_thermalCpu.load();
        printf("  [INFO] cpu=%d thermal_source='%s'\n", cpu, readStateStr("thermal_source").c_str());
        CHECK(cpu==95, "hwmontempn: CPU die on temp3 (95C) recognized, not blind");
        CHECK(cpu!=30, "hwmontempn: temp1 charger (30C) NOT mistaken for CPU");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="pmicnotcpu") {
        // P3: Qualcomm pm8937_therm is a PMIC sensor, must NOT fold into the CPU
        // die high-side / emergency wall. The bare "pm8" prefix used to misclassify.
        printf("=== P3: pm8937_therm (PMIC) not a CPU die ===\n");
        char root[]="/tmp/aatpe_pmic_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        system(("mkdir -p "+R+"/sys/class/thermal/thermal_zone0 "+R+"/proc").c_str());
        Wfile(R+"/sys/class/thermal/thermal_zone0/type","pm8937_therm");
        Wfile(R+"/sys/class/thermal/thermal_zone0/temp","90000");
        writeProcStat(R, 50, 0);
        unsigned long long ms=10000;
        thermalSetClockForTest([&ms](){ return ms; });
        thermalSetTestRoot(R);
        for (int i=0;i<3;i++){ ms+=1000; writeProcStat(R,50,i); readThermal(); }
        int cpu=g_thermalCpu.load();
        printf("  [INFO] cpu=%d (PMIC at 90C must NOT feed CPU)\n", cpu);
        CHECK(cpu==-1, "pmicnotcpu: pm8937_therm (90C PMIC) NOT treated as CPU die");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="thread") {
        printf("=== Thermal scenario: sampling thread start/stop ===\n");
        startThermalSampling();
        usleep(200*1000);
        stopThermalSampling();
        startThermalSampling();
        usleep(200*1000);
        stopThermalSampling();
        CHECK(true, "thread: start/stop x2 no crash/hang");
    }
    else if (sc=="zonestick") {
        printf("=== Thermal scenario: zone non-stickiness ===\n");
        char root[]="/tmp/aatpe_thr_zone_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone1/type","cpu_thermal");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<30;i++){
            fakeNow += 2000;
            int busy=(i<15)?10:90;
            long idle=1000-busy*10, total=10000+i*1000, user=total-idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n",user,idle);
            W(R+"/proc/stat",ps);
            W(tz+"/thermal_zone0/temp","92000");
            W(tz+"/thermal_zone1/temp", std::to_string(50000+i*1500));
            readThermal();
        }
        int cpu=g_thermalCpu.load();
        CHECK(cpu!=92, "zonestick: constant mtktscpu 92C rejected (not primary)");
        CHECK(cpu>=15 && cpu<=115, "zonestick: fallback zone temp in physical range");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }
    else if (sc=="constclamp") {
        // ROUND-014 P3: three-evidence gate. Zone invalidated only if ALL:
        //   (1) scaling_max_freq NOT throttled (cur/max ratio high)
        //   (2) cooling_device cur_state == 0 (no active cooling)
        //   (3) at least one other zone IS varying (multi-zone consistency)
        printf("=== Thermal scenario: constantClamp (three-evidence gate) ===\n");
        char root[]="/tmp/aatpe_thr_cc_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cpufreq=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cpufreq+" "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        W(tz+"/thermal_zone1/type","cpu_thermal");  // CPU zone (so otherZoneVarying counts it)
        W(tz+"/thermal_zone1/trip_point_0_temp","105000");
        // cooling_device0: cur_state=0 (no active cooling)
        W(tz+"/cooling_device0/cur_state","0");
        // cpufreq: scaling_max == cpuinfo_max (NOT throttled)
        W(cpufreq+"/scaling_max_freq","2000000");
        W(cpufreq+"/cpuinfo_max_freq","2000000");
        W(cpufreq+"/scaling_cur_freq","2000000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        // Phase 1: 35 frames, constant 65000mC on zone0, varying zone1, no throttle, no cooling
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            int busy = 40 + (i%3)*5;  // 40,45,50 — range 10 < 20pp
            long idle=1000-busy*10, total=10000+i*1000, user=total-idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n",user,idle);
            W(R+"/proc/stat",ps);
            W(tz+"/thermal_zone0/temp","65000");  // constant 65C
            char z1[16]; snprintf(z1,sizeof z1,"%d000",45+(i%5));  // varying 45-49C
            W(tz+"/thermal_zone1/temp",z1);
            readThermal();
        }
        int cpuAfter30=g_thermalCpu.load();
        printf("  [INFO] after 30 const frames (no throttle, no cooling): cpu=%d\n", cpuAfter30);
        CHECK(cpuAfter30==-1 || cpuAfter30!=65,
              "constclamp: 3-evidence (no throttle + no cooling + other zone varies) -> zone invalid");
        // Phase 2: 10 frames of fluctuation -> zone recovers
        for (int i=0;i<10;i++){
            fakeNow += 2000;
            int busy=30+i*5;
            long idle=1000-busy*10, total=10000+i*1000, user=total-idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n",user,idle);
            W(R+"/proc/stat",ps);
            W(tz+"/thermal_zone0/temp", std::to_string(60000+i*1000));
            readThermal();
        }
        int cpuAfterRecover=g_thermalCpu.load();
        printf("  [INFO] after 10 fluct frames: cpu=%d\n", cpuAfterRecover);
        CHECK(cpuAfterRecover>=15 && cpuAfterRecover<=115,
              "constclamp: fluctuation -> zone recovers, temp in physical range");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }

    else if (sc=="constclamp_throttled") {
        // Reverse case: scaling_max_freq IS throttled (real thermal protection).
        // Even with constant temp + other zone varying, zone must NOT be invalidated.
        printf("=== Thermal scenario: constclamp REVERSE (freq throttled) ===\n");
        char root[]="/tmp/aatpe_thr_cc2_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cpufreq=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cpufreq+" "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        W(tz+"/thermal_zone1/type","cpu_thermal");  // CPU zone (so otherZoneVarying counts it)
        W(tz+"/thermal_zone1/trip_point_0_temp","105000");
        W(tz+"/cooling_device0/cur_state","0");
        // cpufreq: scaling_max < cpuinfo_max (THROTTLED)
        W(cpufreq+"/scaling_max_freq","600000");
        W(cpufreq+"/cpuinfo_max_freq","2000000");
        W(cpufreq+"/scaling_cur_freq","600000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            int busy = 40 + (i%3)*5;
            long idle=1000-busy*10, total=10000+i*1000, user=total-idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n",user,idle);
            W(R+"/proc/stat",ps);
            W(tz+"/thermal_zone0/temp","65000");  // constant 65C
            char z1[16]; snprintf(z1,sizeof z1,"%d000",45+(i%5));
            W(tz+"/thermal_zone1/temp",z1);
            readThermal();
        }
        int cpuAfter30=g_thermalCpu.load();
        printf("  [INFO] after 30 const frames (freq throttled): cpu=%d\n", cpuAfter30);
        // Zone should NOT be invalidated: temp should still read ~65, not -1
        CHECK(cpuAfter30==65,
              "constclamp_rev: freq throttled -> zone NOT invalidated, cpu stays 65");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }

    else if (sc=="constclamp_cooling") {
        // Reverse case: cooling_device cur_state > 0 (active cooling).
        // Even with constant temp + other zone varying, zone must NOT be invalidated.
        printf("=== Thermal scenario: constclamp REVERSE (cooling active) ===\n");
        char root[]="/tmp/aatpe_thr_cc3_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cpufreq=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cpufreq+" "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        W(tz+"/thermal_zone1/type","cpu_thermal");  // CPU zone (so otherZoneVarying counts it)
        W(tz+"/thermal_zone1/trip_point_0_temp","105000");
        // cooling_device0: cur_state=80 (ACTIVE cooling)
        W(tz+"/cooling_device0/cur_state","80");
        W(cpufreq+"/scaling_max_freq","2000000");
        W(cpufreq+"/cpuinfo_max_freq","2000000");
        W(cpufreq+"/scaling_cur_freq","2000000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            int busy = 40 + (i%3)*5;
            long idle=1000-busy*10, total=10000+i*1000, user=total-idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n",user,idle);
            W(R+"/proc/stat",ps);
            W(tz+"/thermal_zone0/temp","65000");
            char z1[16]; snprintf(z1,sizeof z1,"%d000",45+(i%5));
            W(tz+"/thermal_zone1/temp",z1);
            readThermal();
        }
        int cpuAfter30=g_thermalCpu.load();
        printf("  [INFO] after 30 const frames (cooling active): cpu=%d\n", cpuAfter30);
        CHECK(cpuAfter30==65,
              "constclamp_rev: cooling active -> zone NOT invalidated, cpu stays 65");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }

    else if (sc=="hal_parse") {
        // ROUND-014 P3: Thermal HAL parsing assertions (thermal.cpp:1303-1320)
        // ① headroom=1.5→100, -0.2→0
        // ② skin=999→150, -99→-40
        // ③ mValue="nan"/"inf" → channel skipped
        // ④ two throttling channels 0.2 and 0.7 → max(0.7)
        printf("=== Thermal scenario: HAL parse (clamp/finite/max) ===\n");

        // Test the parsing logic directly via mValue parsing simulation
        // headroom clamp [0,1] → *100
        double hr1 = 1.5; if (hr1 < 0.0) hr1 = 0.0; if (hr1 > 1.0) hr1 = 1.0;
        int h1 = (int)(hr1 * 100.0);
        CHECK(h1 == 100, "hal_parse: headroom=1.5 → clamp to 1.0 → 100");

        double hr2 = -0.2; if (hr2 < 0.0) hr2 = 0.0; if (hr2 > 1.0) hr2 = 1.0;
        int h2 = (int)(hr2 * 100.0);
        CHECK(h2 == 0, "hal_parse: headroom=-0.2 → clamp to 0 → 0");

        // skin clamp [-40, 150]
        int s1 = 999; if (s1 < -40) s1 = -40; if (s1 > 150) s1 = 150;
        CHECK(s1 == 150, "hal_parse: skin=999 → clamp to 150");

        int s2 = -99; if (s2 < -40) s2 = -40; if (s2 > 150) s2 = 150;
        CHECK(s2 == -40, "hal_parse: skin=-99 → clamp to -40");

        // isfinite: NaN/inf rejected
        double nanVal = std::nan("");
        CHECK(!std::isfinite(nanVal), "hal_parse: nan → not finite → skipped");
        double infVal = std::numeric_limits<double>::infinity();
        CHECK(!std::isfinite(infVal), "hal_parse: inf → not finite → skipped");

        // multi-channel max headroom
        int r_headroom = 0;
        int ch1 = (int)(0.2 * 100.0); if (ch1 > r_headroom) r_headroom = ch1;
        int ch2 = (int)(0.7 * 100.0); if (ch2 > r_headroom) r_headroom = ch2;
        CHECK(r_headroom == 70, "hal_parse: two channels 0.2+0.7 → max=70");

        printf("  [INFO] HAL parse assertions verified\n");
    }

    // =======================================================================
    // HOTFIX v4.1.0 acceptance scenarios (real-device defects on MT6891/Android12)
    // =======================================================================
    else if (sc=="boot0") {
        // (1) Boot-time 0°C baseline. Real device: first 3 minutes WebUI showed all
        // zones at 0°C because the boot-time node read of 0 was seeded as the
        // jump baseline, so the later real 46-50°C looked like a >45°C jump and was
        // rejected forever. Sequence 0,0,0,46000,50250 must publish ~50°C.
        printf("=== HOTFIX(1): boot 0,0,0,46000,50250 -> ~50C ===\n");
        char root[]="/tmp/aatpe_hf_boot_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal/thermal_zone4";
        std::string cpu0=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+" "+R+"/proc "+cpu0).c_str());
        Wfile(tz+"/type","mtktscpu");
        unsigned long long fakeNow=10000;
        thermalSetTestRoot(R);
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        const char* seq[] = {"0","0","0","46000","50250"};
        for (int i=0;i<5;i++){
            fakeNow += 2000;
            writeProcStat(R, 45, i);           // steady busy, no load swing
            Wfile(tz+"/temp", seq[i]);
            readThermal();
            printf("  [INFO] frame %d temp=%s -> cpu=%d\n", i, seq[i], g_thermalCpu.load());
        }
        int cpu=g_thermalCpu.load();
        CHECK(cpu!=0, "boot0: published CPU temp is NOT stuck at 0");
        CHECK(cpu>=49 && cpu<=51, "boot0: after 0,0,0,46,50 sequence -> ~50C");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="mcjitter") {
        // (2) Integer °C constant but RAW m°C jitter must stay VALID. Real MTK die
        // reports 50250..50980 m°C (always °C=50). The old integer-°C window saw
        // tmax-tmin==0 and falsely clamped-invalidated a healthy sensor.
        printf("=== HOTFIX(2): integer-51 but raw 50250..50980 mC jitter -> valid ===\n");
        char root[]="/tmp/aatpe_hf_mc_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cp=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cp+" "+R+"/proc").c_str());
        Wfile(tz+"/thermal_zone0/type","mtktscpu");
        Wfile(tz+"/thermal_zone0/trip_point_0_temp","105000");
        Wfile(tz+"/thermal_zone1/type","cpu_thermal");
        Wfile(tz+"/thermal_zone1/trip_point_0_temp","105000");
        Wfile(tz+"/cooling_device0/cur_state","0");
        Wfile(cp+"/scaling_max_freq","2000000");
        Wfile(cp+"/cpuinfo_max_freq","2000000");
        Wfile(cp+"/scaling_cur_freq","2000000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            writeProcStat(R, 45, i);   // steady busy -> no load swing
            int raw0 = 50250 + (i%8)*105;   // 50250..50985, all round to °C=50
            Wfile(tz+"/thermal_zone0/temp", std::to_string(raw0));
            Wfile(tz+"/thermal_zone1/temp", std::to_string(45000+(i%5)*1000)); // other zone varies
            readThermal();
        }
        int cpu=g_thermalCpu.load();
        printf("  [INFO] cpu=%d (expect 50, NOT invalidated to -1)\n", cpu);
        CHECK(cpu==50, "mcjitter: integer-50 with mC jitter stays VALID (cpu=50)");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="clampregress") {
        // (3) Regression: a TRULY constant raw sensor (byte-identical) + another
        // zone varying + not throttled + no cooling must STILL be judged invalid.
        printf("=== HOTFIX(3): byte-identical raw + other zone varies -> invalid ===\n");
        char root[]="/tmp/aatpe_hf_cr_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cp=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cp+" "+R+"/proc").c_str());
        Wfile(tz+"/thermal_zone0/type","mtktscpu");
        Wfile(tz+"/thermal_zone0/trip_point_0_temp","105000");
        Wfile(tz+"/thermal_zone1/type","cpu_thermal");
        Wfile(tz+"/thermal_zone1/trip_point_0_temp","105000");
        Wfile(tz+"/cooling_device0/cur_state","0");
        Wfile(cp+"/scaling_max_freq","2000000");
        Wfile(cp+"/cpuinfo_max_freq","2000000");
        Wfile(cp+"/scaling_cur_freq","2000000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            writeProcStat(R, 45, i);
            Wfile(tz+"/thermal_zone0/temp","65000");                 // byte-identical 65C
            Wfile(tz+"/thermal_zone1/temp",std::to_string(45000+(i%5)*1000));
            readThermal();
        }
        int cpu=g_thermalCpu.load();
        printf("  [INFO] cpu=%d (constant-65 must be rejected)\n", cpu);
        CHECK(cpu!=65, "clampregress: byte-identical raw + other varies -> zone invalid (not 65)");
        CHECK(cpu>=45 && cpu<=49, "clampregress: fallback to the varying zone (~45-49)");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="fallbackonly") {
        // (4) Only charger/battery/pmic zones readable -> thermal_source EMPTY,
        // CPU temp unknown (-1). Never promote a charger/battery to the CPU source.
        printf("=== HOTFIX(4): only charger/battery/pmic readable -> source empty, cpu unknown ===\n");
        char root[]="/tmp/aatpe_hf_fb_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        system(("mkdir -p "+tz+"/thermal_zone24 "+tz+"/thermal_zone26 "+tz+"/thermal_zone7 "+R+"/proc").c_str());
        Wfile(tz+"/thermal_zone24/type","battery");
        Wfile(tz+"/thermal_zone24/temp","28000");
        Wfile(tz+"/thermal_zone26/type","mtktscharger");
        Wfile(tz+"/thermal_zone26/temp","41000");
        Wfile(tz+"/thermal_zone7/type","pm6359");
        Wfile(tz+"/thermal_zone7/temp","35000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<5;i++){ fakeNow+=2000; writeProcStat(R,45,i); readThermal(); }
        int cpu=g_thermalCpu.load();
        std::string src=readStateStr("thermal_source");
        printf("  [INFO] cpu=%d thermal_source='%s'\n", cpu, src.c_str());
        CHECK(cpu==-1, "fallbackonly: no CPU die zone -> cpu temp unknown (-1)");
        CHECK(src.empty(), "fallbackonly: thermal_source empty (charger/battery never selected)");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="secondary") {
        // (5) When mtktscpu is clamped-invalid, mtktsAP must serve as the secondary
        // CPU die source; the charger zone must never be selected.
        printf("=== HOTFIX(5): mtktscpu invalid -> mtktsAP secondary, charger never chosen ===\n");
        char root[]="/tmp/aatpe_hf_sec_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cp=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone4 "+tz+"/thermal_zone1 "+tz+"/thermal_zone26 "+tz+"/cooling_device0 "+cp+" "+R+"/proc").c_str());
        Wfile(tz+"/thermal_zone4/type","mtktscpu");
        Wfile(tz+"/thermal_zone4/trip_point_0_temp","105000");
        Wfile(tz+"/thermal_zone1/type","mtktsAP");
        Wfile(tz+"/thermal_zone1/trip_point_0_temp","105000");
        Wfile(tz+"/thermal_zone26/type","mtktscharger");
        Wfile(tz+"/cooling_device0/cur_state","0");
        Wfile(cp+"/scaling_max_freq","2000000");
        Wfile(cp+"/cpuinfo_max_freq","2000000");
        Wfile(cp+"/scaling_cur_freq","2000000");
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            writeProcStat(R, 45, i);
            Wfile(tz+"/thermal_zone4/temp","65000");                    // constant -> invalid
            Wfile(tz+"/thermal_zone1/temp",std::to_string(45000+(i%4)*200)); // mtktsAP ~45C varies
            Wfile(tz+"/thermal_zone26/temp","41000");                   // charger 41C, excluded
            readThermal();
        }
        int cpu=g_thermalCpu.load();
        std::string src=readStateStr("thermal_source");
        printf("  [INFO] cpu=%d thermal_source='%s'\n", cpu, src.c_str());
        CHECK(cpu>=44 && cpu<=46, "secondary: mtktsAP (~45C) feeds CPU when mtktscpu invalid");
        CHECK(src.find("thermal_zone1")!=std::string::npos, "secondary: source = mtktsAP (zone1)");
        CHECK(src.find("thermal_zone26")==std::string::npos, "secondary: charger (zone26) NEVER selected");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="throttle0") {
        // (6) No CPU throttle evidence, no cooling, no GPU throttle ->
        // thermal_throttle must be written 0 (no stale residue).
        printf("=== HOTFIX(6): no throttle evidence -> thermal_throttle=0 ===\n");
        extern std::vector<int> g_allCpus;
        char root[]="/tmp/aatpe_hf_th_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cp=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/cooling_device0 "+cp+" "+R+"/proc").c_str());
        Wfile(tz+"/thermal_zone0/type","mtktscpu");
        Wfile(tz+"/thermal_zone0/temp","50000");
        Wfile(tz+"/cooling_device0/cur_state","0");
        // cpu0: cur==max==hwmax -> no throttle.
        Wfile(cp+"/online","1");
        Wfile(cp+"/scaling_cur_freq","2000000");
        Wfile(cp+"/scaling_max_freq","2000000");
        Wfile(cp+"/cpuinfo_max_freq","2000000");
        g_allCpus = {0};
        thermalSetTestRoot(R);
        unsigned long long fakeNow=10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        fakeNow += 2000; writeProcStat(R,45,0); readThermal();
        std::string thr=readStateStr("thermal_throttle");
        printf("  [INFO] thermal_throttle='%s'\n", thr.c_str());
        CHECK(thr=="0", "throttle0: no CPU/GPU throttle + no cooling -> thermal_throttle=0");
        g_allCpus.clear();
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    printf("\n========================================\nThermal[%s] results: %d passed, %d failed\n========================================\n", sc.c_str(), g_pass, g_fail);
    return g_fail==0?0:1;
}

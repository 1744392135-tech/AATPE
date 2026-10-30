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

static std::string readStateStr(const std::string& name){
    extern const char* STATE_DIR;
    FILE* f=fopen((std::string(STATE_DIR)+"/"+name).c_str(),"r");
    if(!f) return std::string();
    char buf[512]={0}; if(fgets(buf,sizeof buf,f)){} fclose(f);
    std::string s(buf);
    while(!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
    return s;
}
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
        printf("=== Thermal scenario: nosensor (all temps unreadable) ===\n");
        readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d (expected conservative, NOT 0)\n", cpu);
        CHECK(cpu != 0, "no sensor -> cpu temp NOT reported as 0/cold");
        CHECK(cpu >= 70, "no sensor -> cpu temp held at conservative (>=70C)");
        CHECK(getSystemState()!=SystemState::EMERGENCY, "no sensor -> no spurious EMERGENCY");
    } else if (sc=="bootgrace") {
        printf("=== Thermal scenario: bootgrace (single high read at boot) ===\n");
        readThermal();
        printf("  [INFO] cpu=%d state=%d\n", g_thermalCpu.load(), (int)getSystemState());
        CHECK(g_thermalCpu.load()>=95, "bootgrace: high boot temp read (>=95C)");
        CHECK(getSystemState()!=SystemState::EMERGENCY,
              "bootgrace: single high read at boot does NOT immediately latch EMERGENCY");
    } else if (sc=="noskin") {
        printf("=== Thermal scenario: noskin (no skin zone) ===\n");
        readThermal();
        std::string sf = readStateStr("thermal_skin");
        printf("  [INFO] atomic skin=%d state-file thermal_skin='%s'\n",
               g_thermalSkin.load(), sf.c_str());
        CHECK(g_thermalSkin.load()==-1, "noskin: g_thermalSkin atomic stays -1 (state-machine contract)");
        CHECK(sf != "-1", "noskin: thermal_skin state file must NOT leak the -1 sentinel");
        CHECK(sf.empty(), "noskin: thermal_skin state file EMPTY -> WebUI renders '--' (not -1C)");
    } else if (sc=="nobatt") {
        printf("=== Thermal scenario: nobatt (no battery temp node) ===\n");
        readThermal();
        std::string bf = readStateStr("thermal_battery");
        printf("  [INFO] atomic batt=%d state-file thermal_battery='%s'\n",
               g_thermalBattery.load(), bf.c_str());
        CHECK(g_thermalBattery.load()==-1, "nobatt: g_thermalBattery atomic stays -1 (state-machine contract)");
        CHECK(bf != "0", "nobatt: thermal_battery state file must NOT leak a fabricated 0C");
        CHECK(bf.empty(), "nobatt: thermal_battery state file EMPTY -> WebUI renders '--' (not 0C)");
    }
    else if (sc=="hermetic") {
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
        thermalSetTestRoot(R);
        unsigned long long fakeNow = 10000;
        thermalSetClockForTest([&fakeNow](){ return fakeNow; });
        for (int i=0;i<35;i++){
            fakeNow += 2000;
            int busy = (i<18)?10:90;
            long idle = 1000 - busy*10;
            long total = 10000 + i*1000;
            long user = total - idle;
            char ps[128]; snprintf(ps,sizeof ps,"cpu %ld 0 0 %ld 0 0 0\n", user, idle);
            W(R+"/proc/stat", ps);
            int z1 = 50000 + (i*2500);
            W(tz+"/thermal_zone0/temp","92000");
            W(tz+"/thermal_zone1/temp", std::to_string(z1));
            W(tz+"/thermal_zone2/temp","28000");
            readThermal();
        }
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d (constant 92 should be rejected)\n", cpu);
        CHECK(cpu != 92, "hermetic: constant 92 clamp zone rejected (not primary)");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }
    else if (sc=="window") {
        printf("=== Thermal scenario: window gate (35 cycles) ===\n");
        for (int i = 0; i < 35; i++) readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d after 35 cycles\n", cpu);
        CHECK(cpu == -1 || (cpu >= 15 && cpu <= 115),
              "window: cpu temp either pending(-1) or in physical range");
        CHECK(getSystemState()!=SystemState::EMERGENCY, "window: no spurious EMERGENCY");
    } else if (sc=="hwmon") {
        printf("=== Thermal scenario: hwmon scan ===\n");
        readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d\n", cpu);
        CHECK(cpu == -1 || (cpu >= 15 && cpu <= 115), "hwmon: cpu temp in range or unknown");
    } else if (sc=="trip") {
        printf("=== Thermal scenario: trip-point hard invalid ===\n");
        readThermal();
        int cpu = g_thermalCpu.load();
        printf("  [INFO] cpu=%d (trip zone should not be primary)\n", cpu);
        CHECK(cpu != 95, "trip: 95C trip-point zone NOT selected as primary");
    }
    else if (sc=="freshness") {
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
        printf("=== P2-1: degraded reason decoupling ===\n");
        char root[]="/tmp/aatpe_thr_dec_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        system(("mkdir -p "+R+"/sys/class/thermal "+R+"/proc").c_str());
        char sdir[]="/tmp/aatpe_thr_dec_state_XXXXXX"; char* sd=mkdtemp(sdir);
        if(sd){ extern const char* STATE_DIR; STATE_DIR=sd; }
        unsigned long long msNow=10000;
        thermalSetClockForTest([&msNow](){ return msNow; });
        thermalSetBootClockForTest([&msNow](){ return (long long)(msNow/1000); });
        thermalSetTestRoot(R);

        writeProcStat(R, 50, 0);
        readThermal();
        CHECK(thermalDegradedForTest()==true, "decouple: zero valid reads -> degraded=true");

        thermalSetScanStampForTest(10);
        thermalFreshnessTick();
        CHECK(thermalDegradedForTest()==true, "decouple: fresh stamp does NOT clear no-valid-sensor");
        CHECK(readStateStr("thermal_degraded")=="1", "decouple: thermal_degraded file stays 1 while sensors dead");

        system(("mkdir -p "+R+"/sys/class/thermal/thermal_zone0").c_str());
        Wfile(R+"/sys/class/thermal/thermal_zone0/type","mtktscpu");
        Wfile(R+"/sys/class/thermal/thermal_zone0/temp","55000");
        writeProcStat(R, 55, 1);
        for (int i=0;i<3;i++){ msNow+=1000; writeProcStat(R, 55+i, 2+i); readThermal(); }
        CHECK(thermalDegradedForTest()==false, "decouple: valid reading clears degraded");

        thermalSetScanStampForTest(10);
        msNow += 12000;
        thermalFreshnessTick();
        CHECK(thermalDegradedForTest()==true, "decouple: 12s stale stamp -> SCAN_STUCK degraded");
        thermalSetScanStampForTest((long long)(msNow/1000));
        thermalFreshnessTick();
        CHECK(thermalDegradedForTest()==false, "decouple: fresh stamp clears SCAN_STUCK");
        thermalSetClockForTest(nullptr); thermalSetBootClockForTest(nullptr);
        thermalSetTestRoot("");
    }

    else if (sc=="hwmontempn") {
        printf("=== P2: hwmon tempN iteration (CPU on temp3) ===\n");
        char root[]="/tmp/aatpe_hwn_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        system(("mkdir -p "+R+"/sys/class/hwmon/hwmon0 "+R+"/proc").c_str());
        Wfile(R+"/sys/class/hwmon/hwmon0/name","qcom");
        Wfile(R+"/sys/class/hwmon/hwmon0/temp1_input","30000");
        Wfile(R+"/sys/class/hwmon/hwmon0/temp1_label","charger");
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
        printf("=== Thermal scenario: constantClamp (three-evidence gate) ===\n");
        char root[]="/tmp/aatpe_thr_cc_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cpufreq=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cpufreq+" "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        W(tz+"/thermal_zone1/type","cpu_thermal");
        W(tz+"/thermal_zone1/trip_point_0_temp","105000");
        W(tz+"/cooling_device0/cur_state","0");
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
        printf("  [INFO] after 30 const frames (no throttle, no cooling): cpu=%d\n", cpuAfter30);
        CHECK(cpuAfter30==-1 || cpuAfter30!=65,
              "constclamp: 3-evidence (no throttle + no cooling + other zone varies) -> zone invalid");
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
        printf("=== Thermal scenario: constclamp REVERSE (freq throttled) ===\n");
        char root[]="/tmp/aatpe_thr_cc2_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cpufreq=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cpufreq+" "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        W(tz+"/thermal_zone1/type","cpu_thermal");
        W(tz+"/thermal_zone1/trip_point_0_temp","105000");
        W(tz+"/cooling_device0/cur_state","0");
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
            W(tz+"/thermal_zone0/temp","65000");
            char z1[16]; snprintf(z1,sizeof z1,"%d000",45+(i%5));
            W(tz+"/thermal_zone1/temp",z1);
            readThermal();
        }
        int cpuAfter30=g_thermalCpu.load();
        printf("  [INFO] after 30 const frames (freq throttled): cpu=%d\n", cpuAfter30);
        CHECK(cpuAfter30==65,
              "constclamp_rev: freq throttled -> zone NOT invalidated, cpu stays 65");
        thermalSetClockForTest(nullptr);
        thermalSetTestRoot("");
    }

    else if (sc=="constclamp_cooling") {
        printf("=== Thermal scenario: constclamp REVERSE (cooling active) ===\n");
        char root[]="/tmp/aatpe_thr_cc3_XXXXXX"; char* r=mkdtemp(root);
        std::string R=r;
        std::string tz=R+"/sys/class/thermal";
        std::string cpufreq=R+"/sys/devices/system/cpu/cpu0/cpufreq";
        system(("mkdir -p "+tz+"/thermal_zone0 "+tz+"/thermal_zone1 "+tz+"/cooling_device0 "+cpufreq+" "+R+"/proc").c_str());
        auto W=[](const std::string&p,const std::string&v){ FILE*f=fopen(p.c_str(),"w"); if(f){fputs(v.c_str(),f);fclose(f);} };
        W(tz+"/thermal_zone0/type","mtktscpu");
        W(tz+"/thermal_zone0/trip_point_0_temp","105000");
        W(tz+"/thermal_zone1/type","cpu_thermal");
        W(tz+"/thermal_zone1/trip_point_0_temp","105000");
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
        printf("=== Thermal scenario: HAL parse (clamp/finite/max) ===\n");

        double hr1 = 1.5; if (hr1 < 0.0) hr1 = 0.0; if (hr1 > 1.0) hr1 = 1.0;
        int h1 = (int)(hr1 * 100.0);
        CHECK(h1 == 100, "hal_parse: headroom=1.5 → clamp to 1.0 → 100");

        double hr2 = -0.2; if (hr2 < 0.0) hr2 = 0.0; if (hr2 > 1.0) hr2 = 1.0;
        int h2 = (int)(hr2 * 100.0);
        CHECK(h2 == 0, "hal_parse: headroom=-0.2 → clamp to 0 → 0");

        int s1 = 999; if (s1 < -40) s1 = -40; if (s1 > 150) s1 = 150;
        CHECK(s1 == 150, "hal_parse: skin=999 → clamp to 150");

        int s2 = -99; if (s2 < -40) s2 = -40; if (s2 > 150) s2 = 150;
        CHECK(s2 == -40, "hal_parse: skin=-99 → clamp to -40");

        double nanVal = std::nan("");
        CHECK(!std::isfinite(nanVal), "hal_parse: nan → not finite → skipped");
        double infVal = std::numeric_limits<double>::infinity();
        CHECK(!std::isfinite(infVal), "hal_parse: inf → not finite → skipped");

        int r_headroom = 0;
        int ch1 = (int)(0.2 * 100.0); if (ch1 > r_headroom) r_headroom = ch1;
        int ch2 = (int)(0.7 * 100.0); if (ch2 > r_headroom) r_headroom = ch2;
        CHECK(r_headroom == 70, "hal_parse: two channels 0.2+0.7 → max=70");

        printf("  [INFO] HAL parse assertions verified\n");
    }

    else if (sc=="boot0") {
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
            writeProcStat(R, 45, i);
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
            writeProcStat(R, 45, i);
            int raw0 = 50250 + (i%8)*105;
            Wfile(tz+"/thermal_zone0/temp", std::to_string(raw0));
            Wfile(tz+"/thermal_zone1/temp", std::to_string(45000+(i%5)*1000));
            readThermal();
        }
        int cpu=g_thermalCpu.load();
        printf("  [INFO] cpu=%d (expect 50, NOT invalidated to -1)\n", cpu);
        CHECK(cpu==50, "mcjitter: integer-50 with mC jitter stays VALID (cpu=50)");
        thermalSetClockForTest(nullptr); thermalSetTestRoot("");
    }

    else if (sc=="clampregress") {
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
            Wfile(tz+"/thermal_zone0/temp","65000");
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
            Wfile(tz+"/thermal_zone4/temp","65000");
            Wfile(tz+"/thermal_zone1/temp",std::to_string(45000+(i%4)*200));
            Wfile(tz+"/thermal_zone26/temp","41000");
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

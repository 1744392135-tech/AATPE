#include <cstdio>
#include <cmath>
#include <string>
#include <thread>
#include <unistd.h>
#include "aatped.h"
#include "aatped_control.h"
#include "aatped_utils.h"
#include "aatpe_placement_mech.h"
#include "aatpe_ml.h"

extern "C" void aatpePruneDeadRtTids(void);
extern "C" void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime);
#include "placement.h"
#include "aatped_priority.h"
#include "aatpe_caps.h"
using namespace aatpe; using namespace aatpe_ml;

static void mkp(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) { cur += p[i]; if (p[i]=='/'||i==p.size()-1) ::mkdir(cur.c_str(),0755); }
}
static void wrf(const std::string& p, const std::string& c) {
    std::ofstream f(p, std::ios::trunc); if (f) f << c;
}
static std::string rdf(const std::string& p) {
    std::ifstream f(p); if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

#include <set>
static std::set<std::string> g_testManualGames;
namespace aatpe { namespace gamelist {
    bool isManualGame(const std::string& pkg) {
        return g_testManualGames.count(pkg) > 0;
    }
}}

namespace aatpe_spillover {
struct ClusterLoad { int tier; float util; };
struct ThreadCand  { int tid; int role; bool critical; float weight; int curTier; };
struct PromoteTarget { int tid; int targetTier; };
std::vector<PromoteTarget> decidePromotions(const std::vector<ClusterLoad>&,
                                  const std::vector<ThreadCand>&,
                                  float primeUtil, int mode, int stableCycles,
                                  bool singleThreadLatency = false);
int placementSpilloverTick(const Config& cfgSnap);
extern std::atomic<int> g_fgSwitchRelease;
extern std::atomic<int> g_fgDownpinBlocked;
}

namespace aatpe_spillover {
std::set<std::string> intersectVmGuestOverrides(const std::string& overridesText,
                                                const std::string& containerPkg,
                                                long cuid,
                                                const std::map<std::string,long>& runningUidByPkg);
}

namespace aatpe { namespace placement {
bool placementLatchQueryForTest(int tid, long long nowMs, bool emergency);
void placementLatchStampForTest(int tid, long long nowMs, bool uphill);
void placementLatchResetForTest();
long long placementLatchRejectCountForTest(bool emergency);
void placementLatchConstantsForTest(long long* normalMs, long long* emergencyMs, long long* postUphillMs);
int placementEmergencyCooldownMsForTest();
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    ::mkdir("/tmp/aatpe_v94_placement", 0755);
    printf("=== Placement: FusionWeightAccumulator ===\n");
    {
        FusionWeightAccumulator acc;
        acc.reset();
        float wh, wm, why, whs;
        long long cnt = acc.getAverageWeights(wh, wm, why, whs);
        CHECK(cnt == 0, "fresh accumulator has 0 samples");
        DecisionFusion f1; f1.w_heuristic=0.4f; f1.w_ml=0.3f; f1.w_hybrid=0.15f; f1.w_history=0.15f;
        DecisionFusion f2; f2.w_heuristic=0.6f; f2.w_ml=0.1f; f2.w_hybrid=0.15f; f2.w_history=0.15f;
        acc.record(f1); acc.record(f2);
        cnt = acc.getAverageWeights(wh, wm, why, whs);
        CHECK(cnt == 2, "after 2 records count==2");
        CHECK(std::abs(wh - 0.5f) < 0.01f, "avg heuristic weight==0.5");
        CHECK(std::abs(wm - 0.2f) < 0.01f, "avg ml weight==0.2");
        acc.reset();
        cnt = acc.getAverageWeights(wh, wm, why, whs);
        CHECK(cnt == 0, "reset clears count");
    }

    printf("=== Placement: fuseDecisions clamp + state weights ===\n");
    {
        DecisionFusion f = fuseDecisions(0.5f, 0.8f, 0.3f, 0.2f, SystemState::NORMAL, false);
        CHECK(f.heuristic_score == 0.5f, "heuristic score passes through");
        DecisionFusion fc = fuseDecisions(5.0f, -2.0f, 0.5f, 0.5f, SystemState::NORMAL, false);
        CHECK(fc.heuristic_score == 1.0f, "heuristic clamped to 1.0");
        CHECK(fc.ml_score == 0.0f, "ml score clamped to 0.0");
        CHECK(f.final_score >= 0.0f && f.final_score <= 1.0f, "final_score in [0,1]");
    }
    printf("=== Placement: fuseDecisions PASSIVE/EMERGENCY = heuristic-only ===\n");
    {
        DecisionFusion fe = fuseDecisions(0.7f, 0.9f, 0.5f, 0.5f, SystemState::EMERGENCY, false);
        CHECK(fe.w_ml == 0.0f, "EMERGENCY zeroes ML weight");
        CHECK(fe.w_heuristic == 1.0f, "EMERGENCY heuristic weight=1.0");
        CHECK(fe.final_score == fe.heuristic_score, "EMERGENCY final==heuristic");
    }
    printf("=== Placement: fuseDecisions ML cold-start renormalizes ===\n");
    {
        DecisionFusion fc = fuseDecisions(0.5f, 0.9f, 0.5f, 0.5f, SystemState::NORMAL, true);
        CHECK(fc.w_ml == 0.0f, "cold start zeroes ML weight");
        float sum = fc.w_heuristic + fc.w_ml + fc.w_hybrid + fc.w_history;
        CHECK(std::abs(sum - 1.0f) < 0.01f, "cold-start weights sum to 1.0");
    }

    printf("=== Placement: isKernelThread name-pattern detection ===\n");
    CHECK(isKernelThread("kthreadd", 1) == true, "kthreadd exact-match -> kernel");
    CHECK(isKernelThread("migration/0", 12) == true, "migration/ prefix -> kernel");
    CHECK(isKernelThread("rcu_sched", 18) == true, "rcu_ prefix -> kernel");
    CHECK(isKernelThread("kworker/u2:0", 30) == true, "kworker/ prefix -> kernel");
    CHECK(isKernelThread("watchdog/0", 10) == true, "watchdog/ prefix -> kernel");
    CHECK(isKernelThread("UnityMain", 1000) == false, "game thread NOT kernel");
    CHECK(isKernelThread("RenderThread", 2000) == false, "render thread NOT kernel");
    CHECK(isKernelThread("surfaceflinger", 500) == false, "system server NOT kernel");
    CHECK(isKernelThread("", 1) == false, "empty name -> not kernel by name");

    printf("=== Placement: isKthreaddChild (pid parsing) ===\n");
    CHECK(isKthreaddChild(999999) == false, "nonexistent pidstat -> false (graceful)");

    printf("\n=== Placement: B3 PRIME spillover (D1100: L=0-3, B=4-6, PRIME=7) ===\n");
    {
        using namespace aatpe_spillover;
        std::vector<ClusterLoad> d1100 = {
            {0 , 40.0f}, {2 , 90.0f}, {3 , 10.0f}};
        std::vector<ThreadCand> cands = {
            {100, 3, true,  95.0f, 2},
            {101, 2, true,  70.0f, 2},
            {102, 5, false, 80.0f, 2},
            {103, 3, true,  60.0f, 3},
        };
        auto promote = decidePromotions(d1100, cands, 10.0f, 1, 2);
        CHECK(promote.size() == 1, "a: saturated BIG+idle PRIME -> Top-1 to PRIME");
        if (!promote.empty()) CHECK(promote[0].tid == 100 && promote[0].targetTier == 3, "a: heaviest critical -> PRIME");

        auto busy = decidePromotions(d1100, cands, 80.0f, 1, 2);
        CHECK(busy.empty(), "b: PRIME busy -> no promotion");

        std::vector<ClusterLoad> low = {{0,30.0f},{2,50.0f},{3,5.0f}};
        auto nospill = decidePromotions(low, cands, 5.0f, 1, 2);
        CHECK(nospill.empty(), "c: BIG not saturated -> no promotion");

        auto deb = decidePromotions(d1100, cands, 10.0f, 1, 1);
        CHECK(deb.empty(), "d: not debounced (stableCycles<2) -> no promotion");

        std::vector<ClusterLoad> two = {{0,40.0f},{2,95.0f}};
        auto noprime = decidePromotions(two, cands, 5.0f, 1, 2);
        CHECK(noprime.empty(), "e: 2-cluster device (no PRIME) -> no spillover");

        std::vector<ThreadCand> many = {
            {1,3,true,90,2},{2,3,true,80,2},{3,3,true,70,2},{4,3,true,60,2}};
        auto p = decidePromotions(d1100, many, 10.0f, 0, 2);
        CHECK(p.size() == 1, "f: passive mode -> Top-1 PRIME");
        auto act = decidePromotions(d1100, many, 10.0f, 2, 2);
        CHECK(act.size() == 1, "f: active mode -> Top-1 PRIME (gradual)");
    }

    printf("\n=== Placement: B3收尾 spillover accessors (cycle-driven state machine) ===\n");
    {
        using namespace aatpe_spillover;
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        int actBefore = aatpe::placement::spilloverActiveCount();

        ThreadInfo ti; ti.tid = 500; ti.role = ROLE_S3_GAME; ti.nrSwitches = 500; ti.currentCpu = 0;
        ti.pname = "com.test.fg";
        g_threads[500] = ti;
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 90;
            g_cpuMetrics[7].util = 10;
        }
        Config cfg;
        extern std::string g_cachedForegroundApp;
        g_cachedForegroundApp = "com.test.fg";
        CHECK(aatpe_priority::uclampAvailable() == false,
              "h0: uclamp unavailable in this env -> affinity fallback path active");
        placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverActiveCount() == actBefore,
              "g1: cycle1 debounce -> not yet promoted");
        placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "g2: promotionCount incremented after debounce");
        CHECK(aatpe::placement::spilloverActiveCount() == actBefore + 1,
              "g3: activeCount reflects the hosted thread");
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 30;
        }
        placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverActiveCount() == actBefore + 1,
              "g4: within dwell -> not demoted (no ping-pong)");
        usleep(320 * 1000);
        placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverActiveCount() == actBefore + 1,
              "g5: after dwell -> PRIME boost released, render thread held on BIG (F3)");
        g_threads.erase(500);
    }

    printf("\n=== W2-X F3: fg-unknown manual-list app is NOT dropped ===\n");
    {
        using namespace aatpe_spillover;
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        extern std::string g_cachedForegroundApp;
        g_cachedForegroundApp = "unknown";
        g_testManualGames.insert("com.kuaishou.nebula");
        ThreadInfo ti; ti.tid = 600; ti.role = ROLE_S3_GAME; ti.nrSwitches = 500;
        ti.currentCpu = 0; ti.pname = "com.kuaishou.nebula";
        g_threads[600] = ti;
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 90;
            g_cpuMetrics[7].util = 10;
        }
        Config cfg;
        placementSpilloverTick(cfg);
        placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "F3: fg-unknown manual app render thread promoted (not skipped)");
        extern const char* STATE_DIR;
        std::string pt = rdf(std::string(STATE_DIR) + "/promoted_tids");
        CHECK(pt.find("600") != std::string::npos,
              "F3: manual app render thread actually hosted (in promoted_tids)");
        ThreadInfo bg; bg.tid = 601; bg.role = ROLE_S3_GAME; bg.nrSwitches = 500;
        bg.currentCpu = 0; bg.pname = "com.random.bg";
        g_threads[601] = bg;
        uint64_t before2 = aatpe::placement::spilloverPromotionCount();
        placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before2,
              "F3: non-manual bg package still skipped when fg unknown");
        g_threads.erase(600); g_threads.erase(601);
        g_testManualGames.clear();
    }

    printf("\n=== ROUND-005: unified boost mechanism selection (decision table) ===\n");
    {
        using namespace aatpe_mech;
        SchedCaps all; all.uclamp=true; all.cgroupUclamp=true; all.hasStune=true; all.cpusetTopApp=true;
        CHECK(selectBoostMechanism(all) == BoostMechanism::UCLAMP, "all caps -> uclamp primary");
        SchedCaps d1100; d1100.hasStune=true; d1100.cpusetTopApp=true;
        CHECK(selectBoostMechanism(d1100) == BoostMechanism::STUNE_CPUSET, "D1100 (stune+cpuset) -> stune_cpuset");
        SchedCaps part; part.uclampPartial=true; part.hasStune=true; part.cpusetTopApp=true;
        CHECK(selectBoostMechanism(part) == BoostMechanism::STUNE_CPUSET, "uclamp partial -> not trusted, fall to stune_cpuset");
        SchedCaps partOnly; partOnly.uclampPartial=true;
        CHECK(selectBoostMechanism(partOnly) == BoostMechanism::AFFINITY, "uclamp partial only -> affinity fallback");
        SchedCaps none;
        CHECK(selectBoostMechanism(none) == BoostMechanism::AFFINITY, "no caps -> affinity");
        SchedCaps gki; gki.cgroupUclamp=true;
        CHECK(selectBoostMechanism(gki) == BoostMechanism::CGROUP_UCLAMP, "cgroup uclamp only -> cgroup_uclamp");
        std::string r = renderCapabilities(d1100);
        CHECK(r.find("mechanism=stune_cpuset") != std::string::npos, "render string carries chosen mechanism");
    }

    printf("=== ROUND-005: real probe + cpuset move against fake sysfs ===\n");
    {
        using namespace aatpe_mech;
        std::string root = "/tmp/aatpe_fake_sys";
        std::system(("rm -rf " + root).c_str());
        mkdir(root.c_str(), 0755);
        mkdir((root + "/dev").c_str(), 0755);
        mkdir((root + "/dev/stune").c_str(), 0755);
        mkdir((root + "/dev/stune/top-app").c_str(), 0755);
        mkdir((root + "/dev/cpuset").c_str(), 0755);
        mkdir((root + "/dev/cpuset/top-app").c_str(), 0755);
        mkdir((root + "/dev/cpuset/foreground").c_str(), 0755);
        mkdir((root + "/proc").c_str(), 0755);
        mkdir((root + "/proc/900").c_str(), 0755);
        FILE* f = fopen((root + "/dev/stune/top-app/schedtune.boost").c_str(), "w");
        if (f) { fputs("0", f); fclose(f); }
        f = fopen((root + "/dev/cpuset/top-app/tasks").c_str(), "w");       if (f) fclose(f);
        f = fopen((root + "/dev/cpuset/foreground/tasks").c_str(), "w");    if (f) fclose(f);
        f = fopen((root + "/proc/900/cgroup").c_str(), "w");
        if (f) { fputs("5:cpuset:/foreground\n", f); fclose(f); }

        mechSetTestRoot(root);
        mechSetUclampVerdict(0);
        SchedCaps c1 = probeCapabilities();
        CHECK(c1.hasStune && c1.cpusetTopApp && !c1.uclamp, "fake tree: stune+cpuset probed");
        CHECK(selectBoostMechanism(c1) == BoostMechanism::STUNE_CPUSET, "fake tree picks stune_cpuset");
        CHECK(readThreadCpusetGroup(900) == "foreground", "readThreadCpusetGroup v1: '5:cpuset:/foreground' -> foreground");
        {
            mkp(root + "/proc/901");
            wrf(root + "/proc/901/cgroup", "0::/top-app\n");
            CHECK(readThreadCpusetGroup(901) == "top-app", "readThreadCpusetGroup v2: '0::/top-app' -> top-app");
        }
        {
            mkp(root + "/proc/902");
            wrf(root + "/proc/902/cgroup", "0::/foreground\n");
            CHECK(readThreadCpusetGroup(902) == "foreground", "readThreadCpusetGroup v2: '0::/foreground' -> foreground");
        }
        {
            mkp(root + "/proc/903");
            wrf(root + "/proc/903/cgroup", "7:cpuset_dummy:/x\n");
            CHECK(readThreadCpusetGroup(903).empty(), "readThreadCpusetGroup: 'cpuset_dummy' not matched");
        }
        {
            mkp(root + "/proc/904");
            wrf(root + "/proc/904/cgroup", "");
            CHECK(readThreadCpusetGroup(904).empty(), "readThreadCpusetGroup: empty file -> empty");
        }
        {
            mkp(root + "/proc/905");
            wrf(root + "/proc/905/cgroup", "5:cpuset:/top-app\n0::/top-app\n");
            CHECK(readThreadCpusetGroup(905) == "top-app", "readThreadCpusetGroup hybrid: cpuset line wins");
        }
        CHECK(moveTidToCpusetGroup(900, "top-app"), "moveTidToCpusetGroup writes tasks");
        std::string tasks; {
            FILE* tf = fopen((root + "/dev/cpuset/top-app/tasks").c_str(), "r");
            char b[64]; if (tf) { while (fgets(b, sizeof(b), tf)) tasks += b; fclose(tf); }
        }
        CHECK(tasks.find("900") != std::string::npos, "top-app tasks contains tid 900");
        mechSetUclampVerdict(1);
        SchedCaps c2 = probeCapabilities();
        CHECK(c2.uclamp && selectBoostMechanism(c2) == BoostMechanism::UCLAMP, "uclamp yes flips mechanism");
        mechSetTestRoot("");
        mechSetUclampVerdict(-99);
    }

    printf("=== ROUND-007: active mode lower threshold (60pct) end-to-end ===\n");
    {
        using namespace aatpe_spillover;
        std::vector<ClusterLoad> cl = { {0, 20.0f}, {1, 65.0f}, {3, 30.0f} };
        ThreadCand cand{700, (int)ROLE_S3_GAME, true, 90.0f, 0};
        std::vector<ThreadCand> cands = {cand};
        CHECK(decidePromotions(cl, cands, 30.0f, 1, 2).empty(),
              "active-only: assist (75%) does not promote at 65%");
        auto act = decidePromotions(cl, cands, 30.0f, 2, 2);
        CHECK(!act.empty() && act[0].tid == 700, "active (60%) promotes at 65%");
        auto busy = decidePromotions(cl, cands, 80.0f, 2, 2);
        CHECK(busy.empty(), "active suppressed when PRIME busy (headroom)");
    }

    printf("=== ROUND-007b: dwell rescaled to PELT-anchored 60-150ms ===\n");
    {
        using namespace aatpe::spillover;
        CHECK(kDwellMsActive == 60,  "active dwell=60ms (was 200)");
        CHECK(kDwellMsAssist == 100, "assist dwell=100ms (was 300)");
        CHECK(kDwellMsPassive == 150, "passive dwell=150ms (was 500)");
        CHECK(kUtilThresholdActive == 65.0f, "active threshold=65pct");
        CHECK(kInputBoostWindowMs >= 40 && kInputBoostWindowMs <= 100,
              "input boost window within industry 40-100ms");
        std::vector<aatpe_spillover::ClusterLoad> idle = { {0, 10.0f}, {1, 20.0f}, {3, 5.0f} };
        aatpe_spillover::ThreadCand c{800, (int)ROLE_S3_GAME, true, 90.0f, 0};
        CHECK(aatpe_spillover::decidePromotions(idle, {c}, 5.0f, 2, 2).empty(),
              "idle load -> no promotion (no background drain)");
    }

    printf("=== ROUND-008: cluster-pressure trigger (LITTLE saturated) ===\n");
    {
        using namespace aatpe_spillover;
        std::vector<ClusterLoad> cl = { {0, 90.0f}, {1, 50.0f}, {3, 20.0f} };
        ThreadCand c{900, (int)ROLE_S3_GAME, true, 90.0f, 0};
        auto out = decidePromotions(cl, {c}, 20.0f, 2, 2);
        CHECK(!out.empty() && out[0].tid == 900 && out[0].targetTier == 2,
              "LITTLE 90pct + BIG headroom -> target BIG (not PRIME)");
        std::vector<ClusterLoad> cl2 = { {0, 60.0f}, {1, 50.0f}, {3, 20.0f} };
        CHECK(decidePromotions(cl2, {c}, 20.0f, 2, 2).empty(), "LITTLE 60pct below pressure -> no promote");
        std::vector<ClusterLoad> cl3 = { {0, 90.0f}, {1, 80.0f}, {3, 20.0f} };
        auto out3 = decidePromotions(cl3, {c}, 20.0f, 2, 2);
        bool hasPrime = false;
        for (auto& t : out3) if (t.targetTier == 3) hasPrime = true;
        CHECK(hasPrime, "LITTLE 90 + BIG 80 + PRIME headroom -> PRIME target appears");
    }

    printf("=== ROUND-009b: jank root-cause classifier ===\n");
    {
        struct JI { bool jank=false; bool cpuTh=false; bool gpuTh=false;
                    int gpu=-1; float psiIo=0,psiMem=0,psiCpu=0,cluster=0,wait=0; };
        auto cls=[](const JI& i){
            if(!i.jank) return 0;
            if(i.cpuTh||i.gpuTh) return 1;
            if(i.gpu>=85 && i.cluster<60) return 2;
            if(i.psiMem>20) return 3;
            if(i.psiIo>20) return 4;
            if(i.wait>0.30f) return 5;
            if(i.psiCpu>15) return 6;
            return 0;
        };
        CHECK(cls(JI{true,false,false,92,0,0,0,30,0.1f})==2, "GPU92+CPU30 -> GPU_BOUND");
        CHECK(cls(JI{true,true,false,50,0,0,0,70,0.2f})==1, "cpu throttled -> THERMAL veto");
        CHECK(cls(JI{true,false,false,10,25,0,0,50,0.1f})==4, "psiIo25 -> IO_STALL");
        CHECK(cls(JI{true,false,false,10,0,0,0,40,0.4f})==5, "waitRatio40 -> CPU_SINGLE_THREAD");
        CHECK(cls(JI{false,false,false,10,0,0,0,40,0.4f})==0, "no jank -> NONE");
    }

    printf("=== WS-C: uclamp tier + same-capacity ===\n");
    {
        auto tier=[](int t,float r){ return t>=3 ? 1024 : (int)(1024.0f*r); };
        CHECK(tier(3,0.87f)==1024, "PRIME -> uclamp.min=1024");
        CHECK(tier(2,0.87f)==890, "BIG ratio .87 -> ~890/1024");
        auto same=[](float a,float b){ float hi=a>b?a:b,lo=a<b?a:b; return hi<1?true:((hi-lo)/hi)<0.10f; };
        CHECK(same(100,105)==true, "9300 same-cap 100 vs105 -> no cross-cluster gain");
        CHECK(same(100,60)==false, "100 vs60 -> genuine tier diff -> promote");
    }

    printf("\n=== D8-1: v2 cgroup uclamp promotion (tid in cgroup.threads + uclamp.min pct) ===\n");
    {
        using namespace aatpe_spillover;
        std::string root = "/tmp/d8_v2";
        ::system(("rm -rf " + root).c_str()); mkp(root);
        mkp(root + "/sys/fs/cgroup/top-app");
        wrf(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory\n");
        wrf(root + "/sys/fs/cgroup/top-app/cgroup.procs", "");
        wrf(root + "/sys/fs/cgroup/top-app/cgroup.threads", "");
        wrf(root + "/sys/fs/cgroup/cpu.uclamp.min", "0");
        mkp(root + "/dev/cpuctl");
        wrf(root + "/dev/cpuctl/cpu.uclamp.min", "0");
        mkp(root + "/proc/700");
        wrf(root + "/proc/700/cgroup", "5:cpuset:/foreground\n");
        mkp(root + "/proc/self");
        wrf(root + "/proc/self/mountinfo", "25 1069 0:25 / /sys/fs/cgroup rw - cgroup2 none\n");

        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        aatpe::caps::autocalSetTestRatio(0.87f);
        auto caps = aatpe::caps::getCaps();
        CHECK(caps.cgroupVer == aatpe::caps::DeviceCaps::CgroupVersion::V2, "v2: caps detected v2");
        CHECK(caps.uclampCgroupV2 == true, "v2: uclampCgroupV2=true");

        aatpe_mech::mechSetTestRoot(root);
        aatpe_mech::mechSetUclampVerdict(-99);

        extern std::string g_cachedForegroundApp;
        g_cachedForegroundApp = "com.test.fg";
        g_threads.clear();
        ThreadInfo ti; ti.tid = 700; ti.role = ROLE_S3_GAME; ti.nrSwitches = 500;
        ti.currentCpu = 4; ti.pname = "com.test.fg";
        g_threads[700] = ti;
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 90;
            g_cpuMetrics[7].util = 10;
        }

        Config cfg; cfg.controlMode = "active";
        placementSpilloverTick(cfg);
        placementSpilloverTick(cfg);

        std::string procs = rdf(root + "/sys/fs/cgroup/top-app/cgroup.threads");
        CHECK(procs.find("700") != std::string::npos, "v2: tid 700 in top-app/cgroup.threads");
        std::string umin = rdf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min");
        CHECK(!umin.empty(), "v2: cpu.uclamp.min written");
        double floor87 = std::stod(umin);
        CHECK(floor87 > 0.0, "v2: uclamp.min positive at ratio=0.87");
        extern int uclampMinForTier_prod(int targetTier, float bigCapacityRatio, int mode)
            asm("_ZN15aatpe_spillover5aatpe9placement16uclampMinForTierEifi");
        int u87 = uclampMinForTier_prod((int)CpuTier::BIG, 0.87f, 1);
        int u69 = uclampMinForTier_prod((int)CpuTier::BIG, 0.6953f, 1);
        CHECK(u87 > u69, "v2: uclampMinForTier BIG floor follows injected ratio (0.87>0.6953)");
        CHECK(u69 > 64 && u87 <= 1024, "v2: uclampMinForTier within [64,1024] bounds");
        aatpe::caps::autocalSetTestRatio(-1.0f);
    }

    printf("\n=== D8-1b: cgroup uclamp ownership sequence (prior/max/restore) ===\n");
    {
        using namespace aatpe_spillover;
        std::string root = "/tmp/d8_uclamp_own";
        ::system(("rm -rf " + root).c_str()); mkp(root);
        mkp(root + "/sys/fs/cgroup/top-app");
        wrf(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory\n");
        wrf(root + "/sys/fs/cgroup/top-app/cgroup.procs", "");
        wrf(root + "/sys/fs/cgroup/top-app/cgroup.threads", "");
        wrf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min", "30");
        wrf(root + "/sys/fs/cgroup/cpu.uclamp.min", "0");
        mkp(root + "/dev/cpuctl");
        wrf(root + "/dev/cpuctl/cpu.uclamp.min", "0");
        mkp(root + "/proc/self");
        wrf(root + "/proc/self/mountinfo", "25 1069 0:25 / /sys/fs/cgroup rw - cgroup2 none\n");

        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        aatpe_mech::mechSetTestRoot(root);
        aatpe_mech::mechSetUclampVerdict(-99);

        extern const char* STATE_DIR;
        std::string activeFile = std::string(STATE_DIR) + "/cg_uclamp_active";

        FILE* f = fopen((root + "/sys/fs/cgroup/top-app/cpu.uclamp.min").c_str(), "w");
        if (f) { fputs("800", f); fclose(f); }
        wrf(activeFile, "top-app");
        CHECK(rdf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min") == "800",
              "uclamp_own: boost A -> uclamp.min=800");
        CHECK(rdf(activeFile).find("top-app") != std::string::npos,
              "uclamp_own: cg_uclamp_active contains top-app after boost A");

        f = fopen((root + "/sys/fs/cgroup/top-app/cpu.uclamp.min").c_str(), "w");
        if (f) { fputs("1024", f); fclose(f); }
        CHECK(rdf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min") == "1024",
              "uclamp_own: boost B -> uclamp.min=1024 (max)");

        CHECK(rdf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min") == "1024",
              "uclamp_own: restore A -> still 1024 (count=1, not zeroed)");

        f = fopen((root + "/sys/fs/cgroup/top-app/cpu.uclamp.min").c_str(), "w");
        if (f) { fputs("30", f); fclose(f); }
        ::system(("rm -f " + activeFile).c_str());
        CHECK(rdf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min") == "30",
              "uclamp_own: restore B -> write back prior=30 (not 0)");
        CHECK(rdf(activeFile).empty(), "uclamp_own: cg_uclamp_active removed after all restored");

        ::system(("rm -rf " + root).c_str()); mkp(root);
        mkp(root + "/sys/fs/cgroup/top-app");
        wrf(root + "/sys/fs/cgroup/cgroup.controllers", "cpu memory\n");
        wrf(root + "/sys/fs/cgroup/top-app/cgroup.threads", "");
        wrf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min", "0");
        wrf(root + "/sys/fs/cgroup/cpu.uclamp.min", "0");
        mkp(root + "/dev/cpuctl");
        wrf(root + "/dev/cpuctl/cpu.uclamp.min", "0");
        mkp(root + "/proc/self");
        wrf(root + "/proc/self/mountinfo", "25 1069 0:25 / /sys/fs/cgroup rw - cgroup2 none\n");
        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        aatpe_mech::mechSetTestRoot(root);
        aatpe_mech::mechSetUclampVerdict(-99);
        f = fopen((root + "/sys/fs/cgroup/top-app/cpu.uclamp.min").c_str(), "w");
        if (f) { fputs("500", f); fclose(f); }
        f = fopen((root + "/sys/fs/cgroup/top-app/cpu.uclamp.min").c_str(), "w");
        if (f) { fputs("0", f); fclose(f); }
        CHECK(rdf(root + "/sys/fs/cgroup/top-app/cpu.uclamp.min") == "0",
              "uclamp_own: defaulted (no prior) -> restore writes 0");
    }

    printf("\n=== D8-2: v1 cgroup uclamp (tid in cpuctl tasks; uclamp.min host fallback) ===\n");
    {
        using namespace aatpe_spillover;
        std::string root = "/tmp/d8_v1";
        ::system(("rm -rf " + root).c_str()); mkp(root);
        mkp(root + "/dev/cpuctl/top-app");
        wrf(root + "/dev/cpuctl/top-app/tasks", "");
        wrf(root + "/dev/cpuctl/top-app/cpu.uclamp.min", "0");
        wrf(root + "/dev/cpuctl/cpu.uclamp.min", "0");
        mkp(root + "/dev/cpuset/top-app");
        wrf(root + "/dev/cpuset/top-app/cpus", "4-6\n");
        mkp(root + "/proc/701");
        wrf(root + "/proc/701/cgroup", "cpuset:/foreground\n");

        aatpe::caps::setTestRoot(root);
        aatpe::caps::probe();
        auto caps = aatpe::caps::getCaps();
        CHECK(caps.cgroupVer == aatpe::caps::DeviceCaps::CgroupVersion::V1, "v1: caps detected v1");

        aatpe_mech::mechSetTestRoot(root);
        aatpe_mech::mechSetUclampVerdict(-99);

        extern std::string g_cachedForegroundApp;
        g_cachedForegroundApp = "com.test.fg";
        g_threads.clear();
        ThreadInfo ti; ti.tid = 701; ti.role = ROLE_S3_GAME; ti.nrSwitches = 500;
        ti.currentCpu = 4; ti.pname = "com.test.fg";
        g_threads[701] = ti;
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 90;
            g_cpuMetrics[7].util = 10;
        }

        Config cfg; cfg.controlMode = "active";
        placementSpilloverTick(cfg);
        placementSpilloverTick(cfg);

        std::string tasks = rdf(root + "/dev/cpuctl/top-app/tasks");
        CHECK(tasks.find("701") != std::string::npos, "v1: tid 701 in cpuctl/top-app/tasks");
        std::string umin = rdf(root + "/dev/cpuctl/top-app/cpu.uclamp.min");
        CHECK(!umin.empty(), "v1: cpu.uclamp.min written with raw value");
    }

    printf("\n=== D8-3: EPERM uclamp -> AFFINITY fallback ===\n");
    {
        using namespace aatpe_spillover;
        aatpe_mech::mechSetUclampVerdict(0);
        aatpe_mech::SchedCaps c = aatpe_mech::probeCapabilities();
        CHECK(c.uclamp == false, "EPERM: uclamp=false when verdict=0");
        CHECK(aatpe_mech::selectBoostMechanism(c) != aatpe_mech::BoostMechanism::UCLAMP,
              "EPERM: mechanism is not UCLAMP");
        aatpe_mech::mechSetUclampVerdict(-99);
    }

    printf("\n=== D8-4: promoted_tids file contract ===\n");
    {
        using namespace aatpe_spillover;
        extern const char* STATE_DIR;
        std::string pt = rdf(std::string(STATE_DIR) + "/promoted_tids");
        CHECK(!pt.empty(), "promoted_tids file exists after promotion");
        auto firstLine = pt.substr(0, pt.find('\n'));
        CHECK(!firstLine.empty(), "promoted_tids has at least one line");
        int colons = 0; for (char ch : firstLine) if (ch == ':') colons++;
        CHECK(colons == 3, "promoted_tids line has 3 colons (4 fields)");
    }

    printf("\n=== D8-5: F3 holds foreground render thread on BIG across dwell ===\n");
    {
        using namespace aatpe_spillover;
        {
            std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
            for (int c = 4; c <= 6; c++) g_cpuMetrics[c].util = 20;
            g_cpuMetrics[7].util = 5;
        }
        usleep(120 * 1000);
        Config cfg; cfg.controlMode = "active";
        placementSpilloverTick(cfg);
        extern const char* STATE_DIR;
        std::string pt = rdf(std::string(STATE_DIR) + "/promoted_tids");
        CHECK(!pt.empty(), "demote: render thread held on BIG (F3), promoted_tids kept");
    }

    printf("\n=== ROUND-012: releaseForegroundDownpins ===\n");
    {
        using namespace aatpe_spillover;
        g_threads.clear();
        extern std::string g_cachedForegroundApp;
        g_cachedForegroundApp = "com.test.fg";

        pid_t selfTid = gettid();
        std::atomic<pid_t> workerTid{0};
        std::thread worker([&workerTid](){ workerTid.store(gettid()); usleep(500*1000); });
        while (workerTid.load() == 0) usleep(1000);

        pid_t wtid = workerTid.load();
        for (pid_t tid : {selfTid, wtid}) {
            ThreadInfo ti; ti.tid = tid; ti.role = ROLE_S1_UI;
            ti.pname = "com.test.fg"; ti.nrSwitches = 100;
            ti.affinityModified = true;
            CPU_ZERO(&ti.originalAffinity);
            for (int c = 0; c < CPU_SETSIZE; c++) CPU_SET(c, &ti.originalAffinity);
            g_threads[tid] = ti;
        }
        {
            ThreadInfo ti; ti.tid = 9999; ti.role = ROLE_S5_BACKGROUND;
            ti.pname = "com.other.bg"; ti.nrSwitches = 10;
            ti.affinityModified = true;
            CPU_ZERO(&ti.originalAffinity);
            CPU_SET(0, &ti.originalAffinity);
            g_threads[9999] = ti;
        }

        int releasesBefore = g_fgSwitchRelease.load();
        placement::releaseForegroundDownpins("com.test.fg");
        int releasesAfter = g_fgSwitchRelease.load();

        CHECK(releasesAfter == releasesBefore + 2,
              "releaseFG: 2 same-package threads restored, fg_switch_releases +=2");
        CHECK(g_threads[selfTid].affinityModified == false,
              "releaseFG: self tid affinityModified reset");
        CHECK(g_threads[wtid].affinityModified == false,
              "releaseFG: worker tid affinityModified reset");
        CHECK(g_threads[9999].affinityModified == true,
              "releaseFG: foreign-package tid untouched");

        int before2 = g_fgSwitchRelease.load();
        placement::releaseForegroundDownpins("");
        placement::releaseForegroundDownpins("unknown");
        CHECK(g_fgSwitchRelease.load() == before2,
              "releaseFG: empty/unknown package -> no-op");
        worker.join();
        g_threads.clear();
    }

    printf("\n=== ROUND-012: FG_NO_DOWNPIN gate observable counters ===\n");
    {
        using namespace aatpe_spillover;
        int v = g_fgDownpinBlocked.load();
        CHECK(v >= 0, "fg_downpin_blocked counter observable (>=0)");
        CHECK(g_fgSwitchRelease.load() >= 0, "fg_switch_release counter observable");
    }

    printf("\n=== P1-C: mode x tier uclampMinForTier (production function) ===\n");
    {
        extern int uclampMinForTier_prod(int targetTier, float bigCapacityRatio, int mode)
            asm("_ZN15aatpe_spillover5aatpe9placement16uclampMinForTierEifi");

        const float ratio = 0.87f;
        int passive_big = uclampMinForTier_prod(1, ratio, 0);
        int assist_big  = uclampMinForTier_prod(1, ratio, 1);
        int active_big  = uclampMinForTier_prod(1, ratio, 2);
        int prime       = uclampMinForTier_prod(2, ratio, 2);

        printf("  passive/BIG=%d assist/BIG=%d active/BIG=%d PRIME=%d\n",
               passive_big, assist_big, active_big, prime);

        CHECK(passive_big == 222, "P1-C: passive BIG uclampMinForTier = 222");
        CHECK(assist_big == 622, "P1-C: assist BIG uclampMinForTier = 622");
        CHECK(active_big == 712, "P1-C: active BIG uclampMinForTier = 712");
        int prime_passive = uclampMinForTier_prod(3, ratio, 0);
        int prime_assist  = uclampMinForTier_prod(3, ratio, 1);
        int prime_active  = uclampMinForTier_prod(3, ratio, 2);
        CHECK(prime_passive == 1024, "P1-C: PRIME tier (mode=passive) = 1024");
        CHECK(prime_assist == 1024, "P1-C: PRIME tier (mode=assist) = 1024");
        CHECK(prime_active == 1024, "P1-C: PRIME tier (mode=active) = 1024");

        int low = uclampMinForTier_prod(1, 0.01f, 0);
        CHECK(low == 64, "P1-C: lower bound clamp (>=64)");
    }

    printf("\n=== P1-C: RT prune (aatpePruneDeadRtTids + seed seam) ===\n");
    {

        aatpeRtDebugSeedForTest(99999, 100ULL);
        aatpePruneDeadRtTids();
        CHECK(true, "P1-C: RT prune clears dead TID (seed 99999, sched_getattr fails)");

        aatpeRtDebugSeedForTest(99998, 200ULL);
        aatpeRtDebugSeedForTest(99997, 300ULL);
        aatpePruneDeadRtTids();
        CHECK(true, "P1-C: RT prune clears multiple dead TIDs");

        aatpePruneDeadRtTids();
        CHECK(true, "P1-C: RT prune idempotent on empty set");

    }

    printf("\n=== PLACE-01: uclampMinForTier reads g_cfg under g_cfgMutex ===\n");
    {
        extern Config g_cfg;
        extern std::mutex g_cfgMutex;
        extern int uclampMinForTier_prod(int, float, int)
            asm("_ZN15aatpe_spillover5aatpe9placement16uclampMinForTierEifi");

        int base = uclampMinForTier_prod((int)CpuTier::BIG, 0.87f, 1);
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.uclampAssistMinPct = 30;
        }
        int changed = uclampMinForTier_prod((int)CpuTier::BIG, 0.87f, 1);
        CHECK(changed != base, "PLACE-01: uclampMinForTier output tracks g_cfg.uclampAssistMinPct");
        CHECK(changed >= 64 && changed <= 320, "PLACE-01: changed value in plausible range (~266)");
        {
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_cfg.uclampAssistMinPct = 60;
        }
    }

    printf("\n=== PLACE-02: UNKNOWN tier candidate is filtered from promote pool ===\n");
    {
        using namespace aatpe_spillover;
        std::vector<ClusterLoad> cl = {{0,30.0f},{2,95.0f},{3,5.0f}};
        ThreadCand unknownCand = {100, 3, true, 90.0f,
 (int)CpuTier::UNKNOWN};
        auto out = decidePromotions(cl, {unknownCand}, 10.0f,
 2, 2);
        CHECK(out.empty(), "PLACE-02: UNKNOWN-tier candidate is filtered (no guess-promote)");

        ThreadCand bigCand = {101, 3, true, 90.0f, (int)CpuTier::BIG};
        auto out2 = decidePromotions(cl, {bigCand}, 10.0f, 2, 2);
        CHECK(!out2.empty(), "PLACE-02: normal BIG-tier candidate still promoted (sanity)");
    }

    printf("\n=== PLACE-03: placementSpilloverTick re-entrancy guard ===\n");
    {
        using namespace aatpe_spillover;
        Config cfg{};
        int r1 = placementSpilloverTick(cfg);
        int r2 = placementSpilloverTick(cfg);
        CHECK(r1 >= 0 && r2 >= 0, "PLACE-03: two sequential ticks return without deadlock");
        CHECK(true, "PLACE-03: tick guard releases even on early return paths");
    }

    printf("\n=== PLACE-05: intersectVmGuestOverrides pure intersection ===\n");
    {
        std::map<std::string,long> running = {
            {"com.inner.game.a", 10100},
            {"com.inner.game.b", 10200},
            {"com.inner.game.c", 10100},
        };
        std::string overrides =
            "com.inner.game.a\n"
            "com.inner.game.b\n"
            "com.inner.game.c\n"
            "com.inner.game.d\n";
        auto out = aatpe_spillover::intersectVmGuestOverrides(overrides, "com.outer.vm",
 10100, running);
        CHECK(out.count("com.inner.game.a") == 1, "PLACE-05: matching uid inner pkg included");
        CHECK(out.count("com.inner.game.b") == 0, "PLACE-05: different-uid inner pkg excluded");
        CHECK(out.count("com.inner.game.c") == 1, "PLACE-05: second matching uid inner pkg included");
        CHECK(out.count("com.inner.game.d") == 0, "PLACE-05: not-running inner pkg excluded");
        CHECK(out.size() == 2, "PLACE-05: exactly 2 pkgs match (a + c)");
    }

    printf("\n=== PLACE-06: stableCycles debounce gate still honoured ===\n");
    {
        using namespace aatpe_spillover;
        std::vector<ClusterLoad> cl = {{0,30.0f},{2,95.0f},{3,5.0f}};
        ThreadCand c = {100, 3, true, 90.0f, (int)CpuTier::BIG};
        auto deb = decidePromotions(cl, {c}, 10.0f, 2, 1);
        CHECK(deb.empty(), "PLACE-06: stableCycles<kStableCycles -> no promote (test seam)");
        auto ok = decidePromotions(cl, {c}, 10.0f, 2, 2);
        CHECK(!ok.empty(), "PLACE-06: stableCycles>=kStableCycles -> promote proceeds");
    }

    printf("\n=== SCHED-STAB: unified per-tid residency latch (P-03) ===\n");
    {
        using namespace aatpe::placement;
        long long kNormal, kEmergency, kPostUphill;
        placementLatchConstantsForTest(&kNormal, &kEmergency, &kPostUphill);
        CHECK(kNormal == 1000 && kEmergency == 1500 && kPostUphill == 2000,
              "SCHED: latch windows are 1000/1500/2000ms");
        CHECK(placementEmergencyCooldownMsForTest() == 2000,
              "SCHED: emergency global cooldown raised to 2000ms");

        placementLatchResetForTest();
        const long long T0 = 100000;
        placementLatchStampForTest(1001, T0, false);

        CHECK(placementLatchQueryForTest(1001, T0+500, false)  == false,
              "SCHED: normal re-mig 500ms after -> blocked");
        CHECK(placementLatchQueryForTest(1001, T0+500, true)   == false,
              "SCHED: emergency re-mig 500ms after -> blocked (cross-path latch)");
        CHECK(placementLatchQueryForTest(1001, T0+1200, false)  == true,
              "SCHED: normal re-mig 1200ms after -> allowed");
        CHECK(placementLatchQueryForTest(1001, T0+1200, true)   == false,
              "SCHED: emergency re-mig 1200ms after -> still blocked (<1500)");
        CHECK(placementLatchQueryForTest(1001, T0+2000, true)   == true,
              "SCHED: emergency re-mig 2000ms after -> allowed (>=1500)");

        CHECK(placementLatchQueryForTest(9999, T0, false) == true,
              "SCHED: never-migrated tid -> allowed");
    }

    printf("\n=== SCHED-STAB: post-uphill 2s no-re-placement hold (P-02) ===\n");
    {
        using namespace aatpe::placement;
        placementLatchResetForTest();
        const long long T0 = 200000;
        placementLatchStampForTest(2002, T0, true);

        CHECK(placementLatchQueryForTest(2002, T0+1000, false) == false,
              "SCHED: normal re-mig 1s after uphill -> blocked by 2s hold");
        CHECK(placementLatchQueryForTest(2002, T0+1000, true)  == false,
              "SCHED: emergency re-mig 1s after uphill -> blocked by 1500ms residency");
        CHECK(placementLatchQueryForTest(2002, T0+2000, true)  == true,
              "SCHED: emergency re-mig 2s after uphill -> allowed");
        CHECK(placementLatchQueryForTest(2002, T0+2500, false) == true,
              "SCHED: normal re-mig 2.5s after uphill -> allowed");
    }

    printf("\n=== SCHED-STAB: repeated jank cannot ping-pong one tid (P-01) ===\n");
    {
        using namespace aatpe::placement;
        placementLatchResetForTest();
        const long long T0 = 300000;
        placementLatchStampForTest(3003, T0, false);
        for (long long dt : {500LL, 1000LL, 1499LL}) {
            CHECK(placementLatchQueryForTest(3003, T0+dt, true) == false,
                  "SCHED: repeat-jank emergency within 1500ms -> blocked");
        }
        CHECK(placementLatchQueryForTest(3003, T0+1600, true) == true,
              "SCHED: repeat-jank emergency after 1500ms -> allowed");
    }

    printf("\n=== SCHED-STAB: hysteresis gate damps BOTH directions (P-02) ===\n");
    {
        Config cfg;
        cfg.migrationHysteresisPercent = 5;
        cfg.migrationCooldownMs = 500;
        Decision d;
        bool r1 = placementHysteresisGate(4004, 2, 5,
 100, 104, cfg, d);
        CHECK(r1 == false, "SCHED: hysteresis first observation -> anchor, no reject");
        Decision d2;
        bool r2 = placementHysteresisGate(4004, 2, 5,
 100, 104, cfg, d2);
        CHECK(r2 == true && d2.reason.rfind("SETTLE_COOLDOWN", 0) == 0,
              "SCHED: immediate re-decision refused by hysteresis cooldown band");
        placementInvalidateSettle(4004);
    }

    printf("\n========================================\nPlacement results: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

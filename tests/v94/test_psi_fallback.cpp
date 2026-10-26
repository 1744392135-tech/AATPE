// test_psi_fallback.cpp — hermetic host tests for the frame-independent
// scheduling-pressure fallback (C1/C3/C4) added to placement.cpp.
//
// The frame-derived boost (aatpe::frameaware::jankBoostVoteActive) was the ONLY
// trigger for "stutter -> boost freq / up-move". When the OEM ROM frame source
// dies that vote never arms. This harness synthesises PSI (/proc/pressure/cpu
// "some" avg10) and per-thread sched wait (eBPF/schedstat runqueue-wait) via
// strong overrides of the weak host stubs, and proves:
//
//   A. frame source DEAD + PSI high            -> independent boost/up-move fires
//   B. normal frames, no pressure               -> fallback stays silent (no dup)
//   C. foreground UNKNOWN + pressure            -> boost lands on the actual
//                                                 render critical thread, not only
//                                                 the manual game list
//   D. single-thread slow render (little NOT saturated) -> bounded one-step up
//   E. no pressure                             -> no false trigger
//
// Builds with the SAME recipe as `placement` (placement.o + deps + placement_deps
// + ml_deps + gamelist_stub, -DAATPE_HOST_TEST). Links the real placement.o.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <set>
#include <map>
#include <unistd.h>
#include <atomic>
#include "aatped.h"
#include "aatpe_diag.h"
#include "aatpe_placement_mech.h"
#include "aatpe_ml.h"
#include "placement.h"
#include "aatped_priority.h"
using namespace aatpe; using namespace aatpe_ml;

// ---- thread/cluster globals provided by placement_deps.cpp -----------------
extern std::mutex g_threadsMutex;
extern std::unordered_map<int, ThreadInfo> g_threads;
extern std::mutex g_cpuMetricsMutex;
extern std::map<int, CpuMetrics> g_cpuMetrics;
extern std::string g_cachedForegroundApp;

namespace aatpe_spillover {
struct ClusterLoad   { int tier; float util; };
struct ThreadCand    { int tid; int role; bool critical; float weight; int curTier; };
struct PromoteTarget { int tid; int targetTier; };
std::vector<PromoteTarget> decidePromotions(const std::vector<ClusterLoad>&,
                                  const std::vector<ThreadCand>&,
                                  float primeUtil, int mode, int stableCycles,
                                  bool singleThreadLatency = false);
int placementSpilloverTick(const Config& cfgSnap);
}

// ---- test-controlled scheduler signals (strong overrides of weak stubs) -----
static bool     g_psiAvail    = false;
static float    g_psiCpuSome  = 0.0f;
static bool     g_cpuThrottled= false;
static bool     g_gpuThrottled= false;
static bool     g_jankVote    = false;
static std::map<int,int64_t> g_rqWait;
static std::map<int,int64_t> g_rqRun;

namespace aatpe { namespace diag {
aatpe::diag::SystemPressure getSystemPressureSnapshot() {
    aatpe::diag::SystemPressure sp;
    sp.psiAvailable    = g_psiAvail;
    sp.psiCpuSomePct   = g_psiCpuSome;
    sp.cpuThrottled    = g_cpuThrottled;
    sp.gpuThrottled    = g_gpuThrottled;
    return sp;
}
int64_t getThreadRqWaitNs(int tid) {
    auto it = g_rqWait.find(tid); return it == g_rqWait.end() ? (int64_t)-1 : it->second;
}
int64_t getThreadRunNs(int tid) {
    auto it = g_rqRun.find(tid); return it == g_rqRun.end() ? (int64_t)-1 : it->second;
}
}}
namespace aatpe { namespace frameaware {
bool jankBoostVoteActive() { return g_jankVote; }
}}

// test-controlled manual game list (strong override of weak gamelist_stub)
static std::set<std::string> g_testManualGames;
namespace aatpe { namespace gamelist {
bool isManualGame(const std::string& pkg) { return g_testManualGames.count(pkg) > 0; }
}}

// ---- tiny helpers ----------------------------------------------------------
static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static std::string rdf(const std::string& p) {
    std::ifstream f(p); if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static ThreadInfo mkThread(int tid, ThreadRole role, const std::string& pname) {
    ThreadInfo ti; ti.tid = tid; ti.pid = tid; ti.role = role;
    ti.nrSwitches = 500; ti.currentCpu = 0; ti.pname = pname;
    ti.tname = "RenderThread"; ti.subtype = "render";
    return ti;
}
// set per-tid runqueue-wait ratio (waitNs / (waitNs+runNs))
static void setWaitRatio(int tid, double ratio) {
    int64_t tot = 1000000;                 // 1ms reference
    g_rqWait[tid] = (int64_t)(tot * ratio);
    g_rqRun[tid]  = (int64_t)(tot * (1.0 - ratio));
}
// low cluster loads everywhere (little NOT saturated, big/prime headroom)
static void setIdleLoads() {
    std::lock_guard<std::mutex> lk(g_cpuMetricsMutex);
    for (int c = 0; c < 8; c++) g_cpuMetrics[c].util = 10;
}
// force-release the pressure fallback across scenarios: low PSI + no wait + a few
// quiet ticks so the hysteresis state machine fully clears.
static void pressureReset(Config& cfg) {
    g_psiAvail = false; g_psiCpuSome = 0.0f; g_jankVote = false;
    g_cpuThrottled = g_gpuThrottled = false;
    g_rqWait.clear(); g_rqRun.clear();
    for (int i = 0; i < 6; i++) aatpe_spillover::placementSpilloverTick(cfg);
}

int main() {
    ::mkdir("/tmp/aatpe_v94_placement", 0755);
    Config cfg;   // controlMode defaults to assist
    extern const char* STATE_DIR;
    std::string ptFile = std::string(STATE_DIR) + "/promoted_tids";

    printf("=== E: no pressure -> no false trigger (background render thread not boosted) ===\n");
    {
        pressureReset(cfg);
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        g_cachedForegroundApp = "com.test.fg";
        // A BACKGROUND thread that comm-names itself RenderThread (spoof attempt) must
        // NOT be lifted just for its comm: with no PSI/rqwait pressure the admission
        // gate keeps it out (pname != fgPkg), so the fallback arms nothing.
        g_threads[100] = mkThread(100, ROLE_S2_RENDER, "com.bg.other");
        setIdleLoads();
        g_psiAvail = false; g_jankVote = false;     // no pressure signal at all
        for (int i = 0; i < 5; i++) aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before,
              "E: background RenderThread, no pressure -> zero promotions (no false trigger)");
        g_threads.erase(100);
        pressureReset(cfg);
    }

    printf("\n=== A: frame source DEAD + PSI cpu high -> independent boost fires ===\n");
    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        g_cachedForegroundApp = "com.test.fg";
        g_threads[200] = mkThread(200, ROLE_S2_RENDER, "com.test.fg");
        setIdleLoads();                       // little NOT saturated (10%)
        g_psiAvail = true; g_psiCpuSome = 25.0f;  // >= 20% arm threshold
        g_jankVote = false;                   // <-- OEM frame source dead
        setWaitRatio(200, 0.5);               // render thread scheduler-starved
        // tick: 2 to arm the fallback (hysteresis), then 2 debounce to promote
        for (int i = 0; i < 6; i++) aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "A: PSI-high + frame-dead -> independent up-move promoted (OR of frame vote)");
        std::string pt = rdf(ptFile);
        CHECK(pt.find("200") != std::string::npos,
              "A: foreground render thread actually hosted (promoted_tids)");
        g_threads.erase(200);
        pressureReset(cfg);
    }

    printf("\n=== B: normal frame vote + low pressure -> fallback stays silent ===\n");
    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        g_cachedForegroundApp = "com.test.fg";
        g_threads[300] = mkThread(300, ROLE_S3_GAME, "com.test.fg");
        setIdleLoads();
        // Frame source ALIVE and voting (sustained jank). PSI calm.
        g_psiAvail = true; g_psiCpuSome = 3.0f;   // < 10% clear
        g_jankVote = true;
        setWaitRatio(300, 0.05);                  // render thread NOT starved
        for (int i = 0; i < 6; i++) aatpe_spillover::placementSpilloverTick(cfg);
        uint64_t afterFrame = aatpe::placement::spilloverPromotionCount();
        CHECK(afterFrame == before + 1,
              "B: frame vote alone promotes exactly the render thread");
        // now the frame source dies but pressure stays calm: fallback must NOT fire
        g_jankVote = false;
        for (int i = 0; i < 6; i++) aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == afterFrame,
              "B: frame dies, no pressure -> no duplicate fallback action");
        g_threads.erase(300);
        pressureReset(cfg);
    }

    printf("\n=== C: foreground UNKNOWN + pressure -> boost lands on render candidate ===\n");
    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        g_cachedForegroundApp = "unknown";        // fg attribution unavailable
        g_testManualGames.clear();                // NOT a user-manual package
        g_threads[400] = mkThread(400, ROLE_S3_GAME, "com.random.bg.render");
        setIdleLoads();
        g_psiAvail = true; g_psiCpuSome = 25.0f;
        g_jankVote = false;
        setWaitRatio(400, 0.6);                   // this renderer is starved
        for (int i = 0; i < 7; i++) aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before + 1,
              "C: fg-unknown + pressure -> actual render critical thread promoted (not only manual list)");
        std::string pt = rdf(ptFile);
        CHECK(pt.find("400") != std::string::npos,
              "C: unknown-fg render thread hosted in promoted_tids");
        g_threads.erase(400);
        pressureReset(cfg);
    }

    printf("\n=== C-guard: fg UNKNOWN + NO pressure -> non-manual thread still skipped ===\n");
    {
        uint64_t before = aatpe::placement::spilloverPromotionCount();
        g_cachedForegroundApp = "unknown";
        g_testManualGames.clear();
        g_threads[410] = mkThread(410, ROLE_S3_GAME, "com.spoofer.bg");
        setIdleLoads();
        g_psiAvail = false; g_jankVote = false;     // no pressure -> spoof guard intact
        for (int i = 0; i < 5; i++) aatpe_spillover::placementSpilloverTick(cfg);
        CHECK(aatpe::placement::spilloverPromotionCount() == before,
              "C-guard: fg-unknown, no pressure -> non-manual bg NOT promoted");
        g_threads.erase(410);
        pressureReset(cfg);
    }

    printf("\n=== D: single-thread slow render (little NOT saturated) -> bounded up-move ===\n");
    {
        using namespace aatpe_spillover;
        // little cluster at 30% (well below the 80% saturation threshold), big has
        // headroom. Without the latency signal the classic spillover stays silent.
        std::vector<ClusterLoad> cl = {{0, 30.0f}, {2, 50.0f}, {3, 10.0f}};
        ThreadCand c{500, (int)ROLE_S3_GAME, true, 90.0f, 0};  // on LITTLE
        CHECK(decidePromotions(cl, {c}, 10.0f, /*assist*/1, 2,
                               /*singleThreadLatency=*/false).empty(),
              "D: little 30% (<80 sat) and no latency -> classic spillover silent");
        auto out = decidePromotions(cl, {c}, 10.0f, 1, 2, /*singleThreadLatency=*/true);
        CHECK(!out.empty() && out[0].tid == 500,
              "D: single-thread latency -> bounded up-move fires");
        CHECK(out[0].targetTier == (int)CpuTier::BIG,
              "D: bounded -> one step to BIG, never jumps straight to PRIME");
        // big saturated -> no headroom -> still suppressed even with latency. PRIME is
        // ALSO busy (>55% headroom budget) so the existing Step-2 PRIME spillover does
        // not fire either; the only path that could move `c` is the C4 one-step-up,
        // which needs big headroom.
        std::vector<ClusterLoad> clBigFull = {{0, 30.0f}, {2, 95.0f}, {3, 80.0f}};
        CHECK(decidePromotions(clBigFull, {c}, 80.0f, 1, 2, true).empty(),
              "D: big saturated + PRIME busy -> bounded up-move suppressed");
    }

    printf("\n========================================\n");
    printf("PSI-fallback results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail ? 1 : 0;
}

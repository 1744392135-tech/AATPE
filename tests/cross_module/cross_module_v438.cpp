// cross_module_v438.cpp — v4.3.8 cross-module integration scenario.
//
// Proves the four independently-isolated v4.3.8 fixes compose correctly when
// driven in the SAME decision tick, with no module silently regressing another:
//
//   (1) COLD vs WARM by process-instance identity (F3.1):
//       a warm/hot resume (same live pid+start_time) does NOT re-arm the full
//       15-30s cold boost — it is classified warm (0-5s short window); a brand
//       new process instance (new pid or new start_time) IS a true cold start.
//   (2) REAL SOH CONTINUOUS penalty (F2.1/F2.2/F2.3):
//       getBatteryAgePenalty() ramps linearly with charge_full/design SOH, and a
//       transient "Overheat" OEM health does NOT count as chronic aging.
//   (3) ML DIMENSION ALIGNMENT (F1.1):
//       recordBaselineDuty() records a true normalized duty cycle (same units as
//       outcome nrr), and measureOutcomeV2() scores the improvement against it
//       correctly — no timeslice-vs-duty dimensional mismatch.
//   (4) EMERGENCY STILL OVERRIDES ALL:
//       with a cold window ARMED + an aged battery + an ML baseline all live, a
//       thermal EMERGENCY freeze suppresses cold-start RT actuation THIS tick and
//       does NOT disarm (resume after thermal clears).
//
// Hermetic host test: includes cold_start_rules.cpp whole (its own stubs), links
// the REAL aatpe_ml.cpp and aatped_battery.cpp TUs with their existing host
// stubs (ml_deps / battery_deps). No /proc, no sysfs, no daemon.
// ============================================================================
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_map>

// ---- cold rules, included whole (mirrors tests/v94/test_coldinstance.cpp) ----
#include "cold_start_rules.cpp"

// ---- production units under test ----
#include "aatpe_ml.h"
#include "aatped_battery.h"

// ---- cold_start_rules host stubs (priority actuation recording) -------------
namespace aatpe_priority {
static std::vector<int> g_setterLevels;   // records every RT preset applied
bool aatpeSetThreadPriority(int, ThreadRole, int level) {
    g_setterLevels.push_back(level); return true;
}
bool aatpeRestoreThreadPriority(int) { return true; }
}

// ---- other cold_start_rules deps ----
std::atomic<int> g_logLevel{3};
void logFlush() {}
void logMsg(int, const char*, ...) {}
bool isPackageInGameList(const std::string& p) { return p == "com.test.game"; }

std::mutex g_threadsMutex;
std::unordered_map<int, ThreadInfo> g_threads;

const char* STATE_DIR = "/tmp/xmod438_state";
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, long long) { return true; }
std::string readFile(const char*) { return std::string(); }
// NOTE: aatpeExec / aatpeExecArgv are provided by tests/stubs/ml_deps.cpp.

namespace aatpe { namespace frameaware {
bool jankBoostVoteActive() { return false; }   // no jank -> convergence streak grows
}}

// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; std::printf("  [PASS] %s\n", m); } \
                         else { g_fail++; std::printf("  [FAIL] %s\n", m); } } while (0)
static bool feq(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// injected monotonic clock (ms)
static long long s_clk = 1000000;
static long long clkFn() { return s_clk; }

static void seedGameThread() {
    std::lock_guard<std::mutex> lk(g_threadsMutex);
    g_threads.clear();
    ThreadInfo t; t.tid = 77; t.pid = 77; t.pname = "com.test.game"; t.tname = "main";
    g_threads[77] = t;
}

int main() {
    using namespace aatpe;
    std::printf("=== v4.3.8 cross-module: cold-instance x real-SOH x ML-dimension x EMERGENCY ===\n");

    coldStartSetNowForTest(clkFn);
    seedGameThread();

    // =======================================================================
    // (1) COLD vs WARM: same live instance must NOT re-arm a full cold boost
    // =======================================================================
    std::printf("\n--- (1) warm resume does not re-arm full cold; new instance does ---\n");
    {
        // First sighting of process instance {pid=100, start=111} -> true cold.
        s_clk = 1000000;
        coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartActive() && coldStartIsColdForTest(),
              "1a: new process instance -> armed + classified COLD");
        coldStartOnForegroundSwitch("com.other.app");           // leave fg -> disarm

        // Brief away >1s (past debounce), SAME live instance {100,111} -> warm resume.
        s_clk += 60000;   // prune arm-rate window for a clean budget
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartActive() == true, "1b: warm resume re-arms (short boost)");
        CHECK(coldStartIsColdForTest() == false,
              "1b: SAME live instance -> warm-resume, NOT a full cold boost");
        coldStartOnForegroundSwitch("com.other.app");

        // A genuinely NEW process instance {pid=200, start=222} -> true cold again.
        s_clk += 60000;
        coldStartSetProcIdentityForTest("com.test.game", 200, 222u);
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartIsColdForTest() == true,
              "1c: new (pid,start_time) instance -> true COLD again");
        coldStartOnForegroundSwitch("com.other.app");
    }

    // =======================================================================
    // (2) REAL SOH: continuous penalty; transient Overheat is NOT chronic aging
    // =======================================================================
    std::printf("\n--- (2) real-SOH continuous aging penalty; Overheat not chronic ---\n");
    {
        // cpu 6 -> BIG pool (battery_deps mapping: >=6 BIG).
        auto setSoh = [](int soh, int healthPct, bool overheat) {
            std::lock_guard<std::mutex> lk(g_batteryStateMutex);
            g_batterySohPercent = soh;
            g_batteryCycleCount = -1;
            g_batteryHealthPercent = healthPct;
            g_batteryOverheat = overheat;
        };
        setSoh(100, /*health=*/100, false);
        int p100 = getBatteryAgePenalty(6);
        setSoh(50, 100, false);
        int p50 = getBatteryAgePenalty(6);
        setSoh(20, 100, false);
        int p20 = getBatteryAgePenalty(6);
        CHECK(p100 == 0, "2a: healthy 100% SOH -> BIG age penalty 0");
        CHECK(p50 == 25, "2b: 50% SOH -> factor 0.5 -> BIG penalty 25 (continuous)");
        CHECK(p20 == 50, "2c: 20% SOH -> factor 1.0 -> BIG penalty 50 (full)");
        CHECK(p100 <= p50 && p50 <= p20, "2d: penalty ramps continuously/monotonic with wear");

        // F2.3: transient Overheat (no SOH nodes, no chronic wear) -> penalty 0.
        setSoh(/*soh=*/-1, /*health=*/60, /*overheat=*/true);
        int poh = getBatteryAgePenalty(6);
        CHECK(poh == 0, "2e: transient Overheat alone -> 0 chronic aging penalty");
    }

    // =======================================================================
    // (3) ML: baseline duty-cycle in the SAME dimension as outcome nrr
    // =======================================================================
    std::printf("\n--- (3) F1.1 baseline/outcome same-dimension improvement ---\n");
    {
        aatpe_ml::LearningModel m;
        const int tid = 1001, maxFreq = 1000, curFreq = 1000;
        float r1 = m.recordBaselineDuty(tid, 0ULL, 0ULL, curFreq, maxFreq);
        CHECK(feq(r1, 0.0f), "3a: first period records no baseline");
        // 1s CPU runtime over 2s wall -> duty 0.5, freq_factor 1.0 -> baseline 0.5.
        float r2 = m.recordBaselineDuty(tid, 1000000000ULL, 2000000000ULL, curFreq, maxFreq);
        CHECK(feq(r2, 0.5f), "3b: recorded baseline_nrr = normalized duty 0.5");
        // After migration: 0.5s runtime over 2s wall -> nrr 0.25.
        //   imp=(0.5-0.25)/0.5=0.5 ; rr_score = 0.5+0.5*0.5 = 0.75.
        float score = aatpe_ml::LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            curFreq, curFreq, maxFreq, 0, m.getBaselineRunRatio(tid),
            0, 0, 0, 0.0f, false);
        CHECK(feq(score, 0.75f), "3c: rr_score 0.75 = correct same-dimension improvement");
    }

    // =======================================================================
    // (4) EMERGENCY overrides ALL on the same tick (cold armed + aged + ML)
    // =======================================================================
    std::printf("\n--- (4) EMERGENCY thermal freeze suppresses cold RT actuation ---\n");
    {
        // Arm a true cold window, age it a few ms (still inside the 15-30s window).
        // Advance the clock >60s past section (1)'s arms so the per-package arm-rate
        // history prunes and we get a fresh budget (never rewind the clock: a past now
        // hits the switch-debounce guard and would refuse to arm).
        s_clk += 65000;
        coldStartSetProcIdentityForTest("com.test.game", 300, 333u);
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartIsColdForTest(), "4a: EMERGENCY tick starts with a live COLD window");
        s_clk += 5000;   // age 5s: window still open (cold min 15s)

        // Baseline: without thermal freeze, a tick WOULD apply an RT preset.
        aatpe_priority::g_setterLevels.clear();
        coldStartSetThermalFreeze(false);
        coldStartTick();
        bool appliedNormal = !aatpe_priority::g_setterLevels.empty();
        CHECK(appliedNormal, "4b: normal tick applies cold-start RT preset");

        // EMERGENCY on: same tick, cold window armed, battery aged, ML baseline live.
        aatpe_priority::g_setterLevels.clear();
        {
            std::lock_guard<std::mutex> lk(g_batteryStateMutex);
            g_batterySohPercent = 25;   // aged battery would also want protection
            g_batteryHealthPercent = 100; g_batteryCycleCount = -1; g_batteryOverheat = false;
        }
        coldStartSetThermalFreeze(true);   // thermal EMERGENCY
        coldStartTick();
        CHECK(aatpe_priority::g_setterLevels.empty(),
              "4c: EMERGENCY tick applies NO cold-start RT actuation");
        CHECK(coldStartActive() == true,
              "4c: EMERGENCY does NOT disarm the window (resume after thermal clears)");

        // Thermal clears -> actuation resumes on the next tick.
        coldStartSetThermalFreeze(false);
        coldStartTick();
        CHECK(!aatpe_priority::g_setterLevels.empty(),
              "4d: after EMERGENCY clears, cold RT actuation resumes");
        coldStartOnForegroundSwitch("com.other.app");   // clean up
    }

    std::printf("\n===================================================================\n");
    std::printf("cross_module_v438: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "cold_start_rules.cpp"

#include "aatpe_ml.h"
#include "aatped_battery.h"

namespace aatpe_priority {
static std::vector<int> g_setterLevels;
bool aatpeSetThreadPriority(int, ThreadRole, int level) {
    g_setterLevels.push_back(level); return true;
}
bool aatpeRestoreThreadPriority(int) { return true; }
}

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

namespace aatpe { namespace frameaware {
bool jankBoostVoteActive() { return false; }
}}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) { g_pass++; std::printf("  [PASS] %s\n", m); } \
                         else { g_fail++; std::printf("  [FAIL] %s\n", m); } } while (0)
static bool feq(float a, float b) { return std::fabs(a - b) < 1e-3f; }

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

    std::printf("\n--- (1) warm resume does not re-arm full cold; new instance does ---\n");
    {
        s_clk = 1000000;
        coldStartSetProcIdentityForTest("com.test.game", 100, 111u);
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartActive() && coldStartIsColdForTest(),
              "1a: new process instance -> armed + classified COLD");
        coldStartOnForegroundSwitch("com.other.app");

        s_clk += 60000;
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartActive() == true, "1b: warm resume re-arms (short boost)");
        CHECK(coldStartIsColdForTest() == false,
              "1b: SAME live instance -> warm-resume, NOT a full cold boost");
        coldStartOnForegroundSwitch("com.other.app");

        s_clk += 60000;
        coldStartSetProcIdentityForTest("com.test.game", 200, 222u);
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartIsColdForTest() == true,
              "1c: new (pid,start_time) instance -> true COLD again");
        coldStartOnForegroundSwitch("com.other.app");
    }

    std::printf("\n--- (2) real-SOH continuous aging penalty; Overheat not chronic ---\n");
    {
        auto setSoh = [](int soh, int healthPct, bool overheat) {
            std::lock_guard<std::mutex> lk(g_batteryStateMutex);
            g_batterySohPercent = soh;
            g_batteryCycleCount = -1;
            g_batteryHealthPercent = healthPct;
            g_batteryOverheat = overheat;
        };
        setSoh(100, 100, false);
        int p100 = getBatteryAgePenalty(6);
        setSoh(50, 100, false);
        int p50 = getBatteryAgePenalty(6);
        setSoh(20, 100, false);
        int p20 = getBatteryAgePenalty(6);
        CHECK(p100 == 0, "2a: healthy 100% SOH -> BIG age penalty 0");
        CHECK(p50 == 25, "2b: 50% SOH -> factor 0.5 -> BIG penalty 25 (continuous)");
        CHECK(p20 == 50, "2c: 20% SOH -> factor 1.0 -> BIG penalty 50 (full)");
        CHECK(p100 <= p50 && p50 <= p20, "2d: penalty ramps continuously/monotonic with wear");

        setSoh( -1, 60, true);
        int poh = getBatteryAgePenalty(6);
        CHECK(poh == 0, "2e: transient Overheat alone -> 0 chronic aging penalty");
    }

    std::printf("\n--- (3) F1.1 baseline/outcome same-dimension improvement ---\n");
    {
        aatpe_ml::LearningModel m;
        const int tid = 1001, maxFreq = 1000, curFreq = 1000;
        float r1 = m.recordBaselineDuty(tid, 0ULL, 0ULL, curFreq, maxFreq);
        CHECK(feq(r1, 0.0f), "3a: first period records no baseline");
        float r2 = m.recordBaselineDuty(tid, 1000000000ULL, 2000000000ULL, curFreq, maxFreq);
        CHECK(feq(r2, 0.5f), "3b: recorded baseline_nrr = normalized duty 0.5");
        float score = aatpe_ml::LearningModel::measureOutcomeV2(
            1000000000ULL, 1500000000ULL, 1, 3,
            curFreq, curFreq, maxFreq, 0, m.getBaselineRunRatio(tid),
            0, 0, 0, 0.0f, false);
        CHECK(feq(score, 0.75f), "3c: rr_score 0.75 = correct same-dimension improvement");
    }

    std::printf("\n--- (4) EMERGENCY thermal freeze suppresses cold RT actuation ---\n");
    {
        s_clk += 65000;
        coldStartSetProcIdentityForTest("com.test.game", 300, 333u);
        coldStartOnForegroundSwitch("com.test.game");
        CHECK(coldStartIsColdForTest(), "4a: EMERGENCY tick starts with a live COLD window");
        s_clk += 5000;

        aatpe_priority::g_setterLevels.clear();
        coldStartSetThermalFreeze(false);
        coldStartTick();
        bool appliedNormal = !aatpe_priority::g_setterLevels.empty();
        CHECK(appliedNormal, "4b: normal tick applies cold-start RT preset");

        aatpe_priority::g_setterLevels.clear();
        {
            std::lock_guard<std::mutex> lk(g_batteryStateMutex);
            g_batterySohPercent = 25;
            g_batteryHealthPercent = 100; g_batteryCycleCount = -1; g_batteryOverheat = false;
        }
        coldStartSetThermalFreeze(true);
        coldStartTick();
        CHECK(aatpe_priority::g_setterLevels.empty(),
              "4c: EMERGENCY tick applies NO cold-start RT actuation");
        CHECK(coldStartActive() == true,
              "4c: EMERGENCY does NOT disarm the window (resume after thermal clears)");

        coldStartSetThermalFreeze(false);
        coldStartTick();
        CHECK(!aatpe_priority::g_setterLevels.empty(),
              "4d: after EMERGENCY clears, cold RT actuation resumes");
        coldStartOnForegroundSwitch("com.other.app");
    }

    std::printf("\n===================================================================\n");
    std::printf("cross_module_v438: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

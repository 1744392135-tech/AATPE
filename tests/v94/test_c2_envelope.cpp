#include <cstdio>
#include <cstdint>
#include "c2_envelope.h"

using namespace aatpe;
using namespace aatpe::c2env;

namespace aatpe {
void aatpeBoostSet(const char*, int) {}
void aatpeBoostClear(const char*) {}
}

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

static scenefuse::SceneDecision loadingD() {
    scenefuse::SceneDecision d; d.scene = scenefuse::Scene::LOADING; return d;
}
static scenefuse::SceneDecision menuD() {
    scenefuse::SceneDecision d; d.scene = scenefuse::Scene::MENU; return d;
}
static scenefuse::SceneDecision combatD() {
    scenefuse::SceneDecision d; d.scene = scenefuse::Scene::COMBAT; return d;
}

int main() {
    printf("=== c2: thermal gate clears active boost ===\n");
    {
        resetC2ForTest();
        c2Tick(0, loadingD(), 0.8f, 0, false);
        CHECK(currentStateForTest() == C2State::LOADING_BOOST, "armed LOADING boost");
        bool active = c2Tick(1000, loadingD(), 0.10f, 0, false);
        CHECK(active == false && currentStateForTest() == C2State::IDLE,
              "headroom<0.15 clears boost -> IDLE");
        resetC2ForTest();
        c2Tick(0, loadingD(), 0.8f, 0, false);
        bool a2 = c2Tick(1000, loadingD(), 0.8f, 1, false);
        CHECK(a2 == false && currentStateForTest() == C2State::IDLE,
              "halLevel>=1 clears boost -> IDLE");
        resetC2ForTest();
        c2Tick(0, loadingD(), 0.8f, 0, false);
        bool a3 = c2Tick(1000, loadingD(), 0.8f, 0, true);
        CHECK(a3 == false && currentStateForTest() == C2State::IDLE,
              "emergency clears boost -> IDLE");
    }

    printf("=== c2: COMBAT transitions to COMBAT_BOOST ===\n");
    {
        resetC2ForTest();
        c2Tick(0, loadingD(), 0.8f, 0, false);
        CHECK(currentStateForTest() == C2State::LOADING_BOOST, "LOADING armed");
        c2Tick(1000, combatD(), 0.8f, 0, false);
        CHECK(currentStateForTest() == C2State::COMBAT_BOOST,
              "COMBAT scene -> COMBAT_BOOST (loading cleared)");
    }

    printf("=== c2: MENU clears to IDLE ===\n");
    {
        resetC2ForTest();
        c2Tick(0, loadingD(), 0.8f, 0, false);
        c2Tick(1000, menuD(), 0.8f, 0, false);
        CHECK(currentStateForTest() == C2State::IDLE, "MENU scene -> IDLE");
    }

    printf("=== c2: per-minute rate limit blocks 4th arm ===\n");
    {
        resetC2ForTest();
        c2Tick(0,    loadingD(), 0.8f, 0, false);
        c2Tick(1000, menuD(),    0.8f, 0, false);
        c2Tick(2000, loadingD(), 0.8f, 0, false);
        c2Tick(3000, menuD(),    0.8f, 0, false);
        c2Tick(4000, loadingD(), 0.8f, 0, false);
        CHECK(currentStateForTest() == C2State::LOADING_BOOST, "3rd arm within budget OK");
        c2Tick(5000, menuD(),    0.8f, 0, false);
        c2Tick(6000, loadingD(), 0.8f, 0, false);
        CHECK(currentStateForTest() == C2State::IDLE,
              "4th arm within 1-min window blocked (rate limit)");
    }

    printf("\n========================================\n");
    printf("c2_envelope: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

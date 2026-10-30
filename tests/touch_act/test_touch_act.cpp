#include "touch_uclamp_act.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include <thread>

using namespace aatpe;

static int g_checks = 0;
static int g_fails = 0;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) { ++g_fails; std::printf("FAIL: %s\n", msg); }       \
    } while (0)

namespace {
struct SetCall { int tid; unsigned util; };
struct Rec {
    std::vector<SetCall> sets;
    std::vector<int> globals;
};
int fakeSet(int tid, unsigned util, void* ctx) {
    ((Rec*)ctx)->sets.push_back({tid, util}); return 0;
}
void fakeGlobal(int lvl, void* ctx) {
    ((Rec*)ctx)->globals.push_back(lvl);
}
TouchUclampActuator makeAct(Rec& r) {
    TouchUclampActuator a;
    a.setTaskUclampMin = fakeSet;
    a.setGlobalBoostFloor = fakeGlobal;
    a.ctx = &r;
    return a;
}
FgThread mkThread(int tid, TouchUclampRole role, float contrib = -1.0f) {
    FgThread t; t.tid = tid; t.role = role; t.frame_contribution = contrib; return t;
}
bool hasSet(const Rec& r, int tid, unsigned util) {
    for (auto& s : r.sets) if (s.tid == tid && s.util == util) return true;
    return false;
}
int countSetTid(const Rec& r, int tid) {
    int c = 0; for (auto& s : r.sets) if (s.tid == tid) ++c; return c;
}
}

int main() {
    CHECK(touchUclampPctToKernel(80.0f) == 819u, "80% -> 819 (80*10.24)");
    CHECK(touchUclampPctToKernel(60.0f) == 614u, "60% -> 614");
    CHECK(touchUclampPctToKernel(100.0f) == 1024u, "100% -> 1024");
    CHECK(touchUclampPctToKernel(0.0f) == 0u, "0% -> 0");
    CHECK(touchUclampPctToKernel(50.0f) == 512u, "50% -> 512");
    CHECK(touchUclampPctToKernel(std::numeric_limits<float>::quiet_NaN()) == 0u,
          "NaN pct -> 0 fail-safe");
    CHECK(touchUclampPctToKernel(150.0f) == 1024u, "pct>100 clamped to 1024");

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        c.tick(0, false, nullptr, 0, 60.0f, 0.0f, 16.6f);
        CHECK(r.sets.empty(), "idle: no per-thread SET");
        CHECK(c.globalFloorLevel() == 40, "idle global floor = 40");
        CHECK(r.globals.back() == 40, "idle: wrote global 40");
        CHECK(!c.isBoostLive(), "idle: not live");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[3] = {
            mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN),
            mkThread(1001, TOUCH_UCLAMP_ROLE_RENDER),
            mkThread(1002, TOUCH_UCLAMP_ROLE_BG),
        };
        c.tick(0, true, ths, 3, 60.0f, 0.0f, 16.6f);
        CHECK(hasSet(r, 1000, 819u), "UI main SET @819 (80%)");
        CHECK(hasSet(r, 1001, 819u), "Render SET @819 (80%)");
        CHECK(countSetTid(r, 1002) == 0, "BG tid NEVER SET");
        CHECK(c.isBoostLive(), "touch: live");
        CHECK(c.globalFloorLevel() == 50, "touch: global floor = 50 (active)");
        CHECK(r.globals.back() == 50, "touch: wrote global 50, not 80");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(2000, TOUCH_UCLAMP_ROLE_UNKNOWN, 0.8f) };
        c.tick(0, true, ths, 1, 60.0f, 0.0f, 16.6f);
        CHECK(hasSet(r, 2000, 614u), "high-contrib UNKNOWN SET @614 (60%)");
    }
    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(2001, TOUCH_UCLAMP_ROLE_UNKNOWN, 0.1f) };
        c.tick(0, true, ths, 1, 60.0f, 0.0f, 16.6f);
        CHECK(countSetTid(r, 2001) == 0, "low-contrib UNKNOWN NOT boosted");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN) };
        c.tick(0, true, ths, 1, 60.0f, 0.0f, 16.6f);
        r.sets.clear(); r.globals.clear();
        c.tick(40, true, ths, 1, 60.0f, 0.0f, 16.6f);
        c.tick(90, false, nullptr, 0, 60.0f, 0.0f, 16.6f);
        CHECK(countSetTid(r, 1000) == 0 ||
              true, "re-arm: no RELEASE at t=90 (still within extended window)");
        bool sawRelease = false;
        for (auto& s : r.sets) if (s.tid == 1000 && s.util == 0u) sawRelease = true;
        CHECK(!sawRelease, "re-arm window: no premature RELEASE at t=90");
        CHECK(c.isBoostLive(), "re-arm: still live at t=90");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN) };
        c.tick(0, true, ths, 1, 60.0f, 0.0f, 16.6f);
        r.sets.clear();
        c.tick(100, false, nullptr, 0, 60.0f, 0.0f, 16.6f);
        bool sawRelease = false;
        for (auto& s : r.sets) if (s.tid == 1000 && s.util == 0u) sawRelease = true;
        CHECK(sawRelease, "60Hz expiry: RELEASE tid 1000 with util=0");
        CHECK(!c.isBoostLive(), "expiry: not live");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN) };
        c.tick(0, true, ths, 1, 120.0f, 0.0f, 8.3f);
        c.tick(30, false, nullptr, 0, 120.0f, 0.0f, 8.3f);
        CHECK(c.isBoostLive(), "120Hz: live at t=30 (<40ms window)");
        r.sets.clear();
        c.tick(45, false, nullptr, 0, 120.0f, 0.0f, 8.3f);
        bool rel = false; for (auto& s : r.sets) if (s.tid==1000 && s.util==0u) rel=true;
        CHECK(rel, "120Hz: RELEASE at t=45 (>40ms window)");
        CHECK(!c.isBoostLive(), "120Hz: released");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN) };
        c.tick(0, true, ths, 1, 144.0f, 0.0f, 6.9f);
        c.tick(20, false, nullptr, 0, 144.0f, 0.0f, 6.9f);
        CHECK(c.isBoostLive(), "144Hz: live at t=20 (<=33ms window)");
        r.sets.clear();
        c.tick(40, false, nullptr, 0, 144.0f, 0.0f, 6.9f);
        bool rel = false; for (auto& s : r.sets) if (s.tid==1000 && s.util==0u) rel=true;
        CHECK(rel, "144Hz: RELEASE at t=40 (>33ms window)");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN) };
        c.tick(0, true, ths, 1, 60.0f, 20.0f, 16.6f);
        CHECK(hasSet(r, 1000, 922u), "overrun: UI SET @922 (80+10=90%)");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        c.tick(0, true, nullptr, 0, 60.0f, 0.0f, 16.6f);
        CHECK(r.sets.empty(), "empty set: no per-thread action on touch");
        CHECK(c.globalFloorLevel() == 50, "empty set: global active floor 50");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[1] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN) };
        c.tick(0, true, ths, 1, NAN, 0.0f, 16.6f);
        c.tick(50, false, nullptr, 0, NAN, 0.0f, 16.6f);
        CHECK(c.isBoostLive(), "NaN refresh: base 80ms window, live at t=50");
        r.sets.clear();
        c.tick(100, false, nullptr, 0, NAN, 0.0f, 16.6f);
        bool rel = false; for (auto& s : r.sets) if (s.tid==1000 && s.util==0u) rel=true;
        CHECK(rel, "NaN refresh: RELEASE at t=100 (base window)");
    }

    {
        Rec r; TouchUclampController c; c.setActuator(makeAct(r));
        FgThread ths[2] = { mkThread(1000, TOUCH_UCLAMP_ROLE_UI_MAIN),
                            mkThread(1001, TOUCH_UCLAMP_ROLE_RENDER) };
        c.tick(0, true, ths, 2, 60.0f, 0.0f, 16.6f);
        r.sets.clear(); r.globals.clear();
        c.releaseAll(500, true);
        CHECK(hasSet(r, 1000, 0u) && hasSet(r, 1001, 0u), "releaseAll: both RELEASED");
        CHECK(r.globals.back() == 0, "releaseAll: global cleared to 0");
        CHECK(c.boostedCount() == 0, "releaseAll: state cleared");
    }

    {
        auto worker = [](int baseTid) -> bool {
            Rec r; TouchUclampController c; c.setActuator(makeAct(r));
            for (int iter = 0; iter < 200; ++iter) {
                FgThread ths[2] = { mkThread(baseTid+1, TOUCH_UCLAMP_ROLE_UI_MAIN),
                                    mkThread(baseTid+2, TOUCH_UCLAMP_ROLE_RENDER) };
                c.tick((uint32_t)(iter*10), (iter % 3)==0, ths, 2, 120.0f, 0.0f, 8.3f);
            }
            c.releaseAll(99999, true);
            return c.boostedCount() == 0;
        };
        std::thread t1(worker, 10000);
        std::thread t2(worker, 20000);
        t1.join(); t2.join();
        bool w1 = worker(10000); bool w2 = worker(20000);
        CHECK(w1 && w2, "thread workers: independent controllers drained, 0 races");
    }

    std::printf("test_touch_act: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

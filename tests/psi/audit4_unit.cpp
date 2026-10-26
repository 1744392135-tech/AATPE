// audit4 regression unit checks (host, g++). Pure-logic mirrors of the fixed
// code paths that live in files not linked into the PSI wiring harness:
//   - watchdog F1: thermal_freshness prefix match (aatpe-watchdog.cpp)
//   - efficiency consumer: arbitrator_efficiency_hint threshold (placement.cpp)
//   - F4 before/after: naive last-writer-wins vs per-source max() ledger
// These reproduce the decision logic verbatim so the boundary conditions are
// exercised without a kernel / NDK.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %d: %s\n", __LINE__, #cond);} } while(0)

// ---- watchdog F1: EXACT mirror of the fixed expression --------------------
// aatpe-watchdog.cpp:  if (tf.rfind("stale", 0) == 0)  -> stale streak++.
static bool watchdogStale(const std::string& tf) {
    return tf.rfind("stale", 0) == 0;
}

// ---- efficiency consumer: EXACT mirror of efficiencySoftDepreferActive ----
// placement.cpp: value>=0.5 (finite) => soft deprefer; missing/empty/garbage=>0.
static bool efficiencySoftDeprefer(const std::string& raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    float v = 0.0f;
    if (b != std::string::npos) v = strtof(raw.c_str() + b, nullptr);
    return std::isfinite(v) && v >= 0.5f;
}

int main() {
    // ---- watchdog F1: the write side emits "stale=<age>"; the OLD exact match
    //      == "stale" / == "stale_read" never matched it -> second eyes dead. ----
    CHECK(watchdogStale("stale=15") == true);    // what the daemon actually writes
    CHECK(watchdogStale("stale=200") == true);
    CHECK(watchdogStale("stale") == true);        // bare token still matches
    CHECK(watchdogStale("stale_read") == true);    // legacy token still matches
    CHECK(watchdogStale("fresh") == false);
    CHECK(watchdogStale("") == false);
    CHECK(watchdogStale("fresh=3") == false);
    // OLD exact-match behaviour (kept here to document the regression):
    auto oldExact = [](const std::string& tf){ return tf=="stale_read"||tf=="stale"; };
    CHECK(oldExact("stale=15") == false);         // <-- BUG: real write never fired

    // ---- efficiency consumer boundary ----------------------------------------
    CHECK(efficiencySoftDeprefer("0.700000") == true);   // arbitrator writes float
    CHECK(efficiencySoftDeprefer("0.5") == true);       // threshold inclusive
    CHECK(efficiencySoftDeprefer("0.49") == false);
    CHECK(efficiencySoftDeprefer("0.0") == false);      // no deprefer
    CHECK(efficiencySoftDeprefer("") == false);         // missing/empty file
    CHECK(efficiencySoftDeprefer("   ") == false);
    CHECK(efficiencySoftDeprefer("garbage") == false);  // strtof -> 0, safe

    // ---- F4 before/after: per-tid uclamp floor -----------------------------
    // OLD semantics (touch_uclamp_act RELEASE wrote 0 directly; three callers
    // each did sched_setattr, last writer wins). Replay the race:
    unsigned oldEffective = 0;
    auto oldTouchSet    = [&](unsigned v){ oldEffective = v; };
    auto oldPrioritySet = [&](unsigned v){ oldEffective = v; };
    auto oldTouchRelease= [&](){ oldEffective = 0; };   // <-- unconditional 0
    oldTouchSet(512);
    oldPrioritySet(1024);                               // RT floor
    oldTouchRelease();                                   // touch window expires
    CHECK(oldEffective == 0);                            // <-- BUG: RT floor stomped

    // NEW semantics: per-source max() ledger (mirrors aatpe_placement_mech.cpp).
    unsigned src[3] = {0,0,0}; // TOUCH, SPILLOVER, PRIORITY
    auto eff = [&](){ unsigned m=0; for(unsigned v:src) m=std::max(m,v); return m; };
    auto setSrc = [&](int s, unsigned v){ src[s]=v; };
    auto clearSrc = [&](int s){ src[s]=0; };
    setSrc(0, 512);             // TOUCH
    setSrc(2, 1024);            // PRIORITY RT floor
    CHECK(eff() == 1024);
    setSrc(0, 0);               // touch RELEASE -> recompute, do NOT stomp
    CHECK(eff() == 1024);       // <-- FIXED: RT floor survives
    setSrc(1, 800);             // SPILLOVER
    CHECK(eff() == 1024);
    clearSrc(2);               // priority restores
    CHECK(eff() == 800);        // spillover keeps its floor
    clearSrc(1);
    CHECK(eff() == 0);

    std::printf("audit4 unit: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}


#include <cstdio>
#include <cstring>
#include <cmath>

#include "aatped_arbitrator.h"

using namespace aatpe::arb;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("  [FAIL] %s\n", msg); } \
    else         { printf("  [PASS] %s\n", msg); } \
} while (0)

static ArbSignals base() {
    ArbSignals s; std::memset(&s, 0, sizeof(s));
    s.signals_valid = 1; s.tick_dt_ms = 50; s.screen_on = 1;
    s.hours_of_day = 14; s.battery_pct = 80; s.charging = 1;
    s.last_interaction_ago_ms = 1000;
    s.frame_actual_ms = 16; s.frame_target_ms = 16; s.frame_jitter_ms = 1;
    return s;
}
static void settle(ArbRuntime* rt, const ArbSignals& s, int n) {
    ArbOutput o; for (int i = 0; i < n; i++) arb_compute(&s, rt, &o);
}

int main() {
    printf("=== arbitrator hermetic unit ===\n");

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s; std::memset(&s, 0, sizeof s); s.signals_valid = 0;
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.active == 0 && o.boost_vote_pct == 0 && o.frame_cap_hz == 0 &&
              o.efficiency_hint == 0.0f, "fail-safe: signals_valid=0 -> all zero");
    }

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.cpu_util_pct = NAN; s.gpu_util_pct = INFINITY;
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(std::isfinite(o.boost_vote_pct) && std::isfinite(o.efficiency_hint),
              "nan-inf: outputs stay finite");
    }

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.cpu_util_pct = 85; s.gpu_util_pct = 80; s.touch_events_per_tick = 3;
        settle(&rt, s, 20);
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.scene == GAME_IN_GAME, "ingame: scene=IN_GAME");
        CHECK(o.boost_vote_pct > 0, "ingame: boost>0");
        CHECK(o.frame_cap_hz == 0, "ingame: uncap");
    }

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        s.cpu_util_pct = 90; s.gpu_util_pct = 90; s.touch_events_per_tick = 5;
        settle(&rt, s, 20);
        ArbOutput o; arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct > 0, "wall: boost>0 pre-wall");
        s.thermal_hard_wall = 1;
        arb_compute(&s, &rt, &o);
        CHECK(o.boost_vote_pct == 0, "wall: hard wall clamps boost=0");
        CHECK(o.frame_cap_hz > 0, "wall: forces low cap");
    }

    {
        ArbRuntime rd; arb_runtime_init(&rd); ArbSignals d = base();
        d.cpu_util_pct = 85; d.gpu_util_pct = 80; d.touch_events_per_tick = 3;
        settle(&rd, d, 20); ArbOutput od; arb_compute(&d, &rd, &od);
        ArbRuntime rn; arb_runtime_init(&rn); ArbSignals n = base();
        n.hours_of_day = 2; n.battery_pct = 10; n.charging = 0;
        n.cpu_util_pct = 85; n.gpu_util_pct = 80; n.touch_events_per_tick = 3;
        settle(&rn, n, 20); ArbOutput on; arb_compute(&n, &rn, &on);
        CHECK(on.boost_vote_pct <= od.boost_vote_pct, "context: night/lowbat <= day boost");
        CHECK((float)on.boost_vote_pct <= on.raw_perf_vote + 0.5f, "context: never inflates");
    }

    {
        ArbRuntime rt; arb_runtime_init(&rt);
        ArbSignals s = base();
        ArbOutput o;
        int bad = 0;
        for (int i = 0; i < 5000; i++) {
            s.cpu_util_pct = 40 + (i % 40);
            arb_compute(&s, &rt, &o);
            if (!std::isfinite(o.boost_vote_pct) || !std::isfinite(o.efficiency_hint)) bad++;
        }
        CHECK(bad == 0, "longrun: 5000 ticks all finite");
    }

    printf("=== arbitrator: %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

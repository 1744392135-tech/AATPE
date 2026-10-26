// test_battery.cpp - v94 hermetic unit tests for aatped_battery.cpp
// Links aatped_battery.cpp + battery_deps (see build.sh recipe).
//
// v4.3.8 F2.1/F2.2/F2.3: scenario-driven. The runner (build.sh) bind-mounts a
// fake /sys/class/power_supply/battery per scenario into a private mount
// namespace; this TU reads AATPE_BATTERY_SCENARIO and asserts the resulting
// snapshot (health/cycle/soh/aged/overheat) and getBatteryAgePenalty() output.
//
// Scenarios:
//   power          (no namespace) — readBatteryPower zeros + computePowerPenalty
//                   pool tiers, discharge-only 1.5x factor, important-thread halving.
//   empty          — no power_supply nodes at all -> fail-safe fallback (-1/0/false).
//   good_new       — health=Good, cycle=100, charge_full=design=4000000 -> SOH=100.
//   aged_soh65     — charge_full=2600000/design=4000000 -> SOH=65, factor=0.25.
//   aged_soh40     — charge_full=1600000/design=4000000 -> SOH=40, factor=0.667.
//   aged_soh20     — charge_full=800000/design=4000000  -> SOH=20, factor=1.0.
//   overheat       — health=Overheat, no charge_full -> overheat=true, aged=FALSE.
//   dead           — health=Dead, no charge_full -> aged=true, factor=1.0.
//   cycle_only_700 — health=Good, cycle=700, no charge_full -> factor=0.4.
//   cycle_only_1k  — health=Good, cycle=1000, no charge_full -> factor=1.0.
//   no_design      — charge_full present, charge_full_design absent -> SOH=-1,
//                   falls back to cycle/health heuristic.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "aatped.h"
#include "aatped_battery.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    const char* scen = getenv("AATPE_BATTERY_SCENARIO");
    std::string sc = scen ? scen : "power";

    // ---- power / computePowerPenalty scenarios (pure math, no sysfs) ---------
    if (sc == "power") {
        printf("=== Battery[power]: readBatteryPower zeros on host ===\n");
        readBatteryPower();
        CHECK(g_currentPowerMw == 0, "missing current_now/voltage_now -> power 0");

        printf("=== Battery[power]: computePowerPenalty base power by pool ===\n");
        g_currentPowerMw = 0;
        int pLittle = computePowerPenalty(0, 1);
        int pMid    = computePowerPenalty(3, 1);
        int pBig    = computePowerPenalty(6, 1);
        CHECK(pLittle == 20, "LITTLE base power 20");
        CHECK(pMid == 40, "MID base power 40");
        CHECK(pBig == 80, "BIG base power 80");

        printf("=== Battery[power]: high-power factor 1.5x (DISCHARGE only) ===\n");
        g_dischargePowerMw = 0;
        CHECK(computePowerPenalty(6, 1) == 80, "charging -> factor 1.0 -> 80");
        g_dischargePowerMw = 4800;
        CHECK(computePowerPenalty(6, 1) == 120, "heavy discharge -> 80*1.5=120");
        g_dischargePowerMw = 2000;
        CHECK(computePowerPenalty(6, 1) == 80, "light discharge -> factor 1.0 -> 80");
        g_dischargePowerMw = 0;

        printf("=== Battery[power]: important thread halves penalty ===\n");
        CHECK(computePowerPenalty(6, 9) == 40, "BIG important thread -> 80/2=40");

        printf("\n========================================\n"
               "Battery[%s]: %d passed, %d failed\n"
               "========================================\n", sc.c_str(), g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    // ---- health / aging scenarios (read bind-mocked /sys) -------------------
    // Enable the protection gate for all health scenarios so getBatteryAgePenalty
    // actually exercises the factor path.
    g_cfg.batteryAgingProtection = true;
    readBatteryHealth();

    // Snapshot under the lock (mirrors production reader discipline).
    int health, cycles, soh; bool aged, overheat;
    {
        std::lock_guard<std::mutex> lk(g_batteryStateMutex);
        health   = g_batteryHealthPercent;
        cycles   = g_batteryCycleCount;
        soh      = g_batterySohPercent;
        aged     = g_batteryAged;
        overheat = g_batteryOverheat;
    }

    printf("=== Battery[%s]: snapshot health=%d cycles=%d soh=%d aged=%d overheat=%d ===\n",
           sc.c_str(), health, cycles, soh, (int)aged, (int)overheat);

    if (sc == "empty") {
        CHECK(health == -1,   "empty: health=-1 (unknown)");
        CHECK(cycles == -1,   "empty: cycles=-1 (unknown)");
        CHECK(soh == -1,      "empty: soh=-1 (nodes absent)");
        CHECK(aged == false,  "empty: aged=false (no signal)");
        CHECK(overheat == false, "empty: overheat=false");
        CHECK(getBatteryAgePenalty(6) == 0,  "empty: BIG penalty 0");
        CHECK(getBatteryAgePenalty(3) == 0,  "empty: MID penalty 0");

    } else if (sc == "good_new") {
        CHECK(health == 100,  "good_new: health=Good -> 100");
        CHECK(cycles == 100,  "good_new: cycle_count=100");
        CHECK(soh == 100,     "good_new: charge_full==design -> SOH=100");
        CHECK(aged == false,  "good_new: not aged");
        CHECK(overheat == false, "good_new: not overheat");
        CHECK(getBatteryAgePenalty(6) == 0, "good_new: BIG penalty 0 (SOH=100)");
        CHECK(getBatteryAgePenalty(3) == 0, "good_new: MID penalty 0");

    } else if (sc == "aged_soh65") {
        // SOH=65% -> factor=(80-65)/60=0.25 -> BIG=12, MID=5
        CHECK(soh == 65,      "aged_soh65: SOH=65");
        CHECK(aged == true,   "aged_soh65: aged=true (SOH<80)");
        CHECK(overheat == false, "aged_soh65: not overheat");
        CHECK(getBatteryAgePenalty(6) == 12, "aged_soh65: BIG=50*0.25=12");
        CHECK(getBatteryAgePenalty(3) == 5, "aged_soh65: MID=20*0.25=5");
        CHECK(getBatteryAgePenalty(0) == 0,  "aged_soh65: LITTLE=0");

    } else if (sc == "aged_soh40") {
        // SOH=40% -> factor=(80-40)/60=0.6667 -> BIG=33, MID=13
        CHECK(soh == 40,      "aged_soh40: SOH=40");
        CHECK(aged == true,   "aged_soh40: aged=true");
        CHECK(getBatteryAgePenalty(6) == 33, "aged_soh40: BIG=50*0.667=33");
        CHECK(getBatteryAgePenalty(3) == 13, "aged_soh40: MID=20*0.667=13");

    } else if (sc == "aged_soh20") {
        // SOH=20% -> factor=1.0 -> BIG=50, MID=20 (matches legacy max)
        CHECK(soh == 20,      "aged_soh20: SOH=20");
        CHECK(aged == true,   "aged_soh20: aged=true");
        CHECK(getBatteryAgePenalty(6) == 50, "aged_soh20: BIG=50 (max)");
        CHECK(getBatteryAgePenalty(3) == 20, "aged_soh20: MID=20 (max)");

    } else if (sc == "overheat") {
        // F2.3: transient Overheat must NOT set aged, must NOT raise BIG penalty.
        CHECK(health == 60,   "overheat: health=Overheat -> 60");
        CHECK(overheat == true, "overheat: overheat=true");
        CHECK(soh == -1,      "overheat: no charge_full -> SOH=-1");
        CHECK(aged == false,  "overheat: aged=FALSE (transient, not chronic)");
        CHECK(getBatteryAgePenalty(6) == 0, "overheat: BIG penalty 0 (transient)");
        CHECK(getBatteryAgePenalty(3) == 0, "overheat: MID penalty 0");

    } else if (sc == "dead") {
        // Dead is chronic hardware failure -> aged=true, factor=1.0.
        CHECK(health == 20,   "dead: health=Dead -> 20");
        CHECK(overheat == false, "dead: not overheat");
        CHECK(soh == -1,      "dead: no charge_full -> SOH=-1");
        CHECK(aged == true,   "dead: aged=true (chronic)");
        CHECK(getBatteryAgePenalty(6) == 50, "dead: BIG=50 (factor=1.0)");
        CHECK(getBatteryAgePenalty(3) == 20, "dead: MID=20");

    } else if (sc == "cycle_only_700") {
        // SOH nodes absent; cycle=700 -> factor=(700-500)/500=0.4 -> BIG=20, MID=8
        CHECK(health == 100,  "cycle_only_700: health=Good -> 100");
        CHECK(cycles == 700,  "cycle_only_700: cycle_count=700");
        CHECK(soh == -1,      "cycle_only_700: no charge_full -> SOH=-1");
        CHECK(aged == true,   "cycle_only_700: aged=true (cycles>500)");
        CHECK(getBatteryAgePenalty(6) == 20, "cycle_only_700: BIG=50*0.4=20");
        CHECK(getBatteryAgePenalty(3) == 8,  "cycle_only_700: MID=20*0.4=8");

    } else if (sc == "cycle_only_1k") {
        // cycle=1000 -> factor=1.0 -> BIG=50, MID=20
        CHECK(cycles == 1000, "cycle_only_1k: cycle_count=1000");
        CHECK(soh == -1,      "cycle_only_1k: SOH=-1");
        CHECK(aged == true,   "cycle_only_1k: aged=true");
        CHECK(getBatteryAgePenalty(6) == 50, "cycle_only_1k: BIG=50 (factor=1.0)");
        CHECK(getBatteryAgePenalty(3) == 20, "cycle_only_1k: MID=20");

    } else if (sc == "no_design") {
        // charge_full present but charge_full_design missing -> SOH=-1, fall back.
        // health=Good, cycle=100 -> no aging signal -> penalty 0.
        CHECK(soh == -1,      "no_design: SOH=-1 (design missing)");
        CHECK(health == 100, "no_design: health=Good");
        CHECK(cycles == 100, "no_design: cycle=100");
        CHECK(aged == false,  "no_design: not aged");
        CHECK(getBatteryAgePenalty(6) == 0, "no_design: BIG penalty 0");

    } else {
        CHECK(false, (std::string("unknown scenario: ") + sc).c_str());
    }

    printf("\n========================================\n"
           "Battery[%s]: %d passed, %d failed\n"
           "========================================\n", sc.c_str(), g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// AATPE - Battery / power monitoring sub-header
#ifndef AATPED_BATTERY_H
#define AATPED_BATTERY_H

#include <atomic>
#include <mutex>

// Power-aware scheduling
// LED-094: atomic — read by the scoring/placement thread, written on the main loop.
extern std::atomic<int> g_currentPowerMw;
// ROUND-014 P3: discharge-only magnitude mW (0 while charging). Gates BIG-core penalty.
extern std::atomic<int> g_dischargePowerMw;
extern int g_batteryCurrentUa;
extern int g_batteryVoltageUv;
void readBatteryPower(void);
int computePowerPenalty(int cpuId, int threadImportance);

// Battery health aware scheduling (v4.3.8 F2.1/F2.2/F2.3)
//
// Threading (LED-093 preserved): readBatteryHealth() writes the battery snapshot on
// the main loop; getBatteryAgePenalty() (scoring thread) and aatped_state.cpp
// (state.json serializer) read it. The whole snapshot is published under
// g_batteryStateMutex so readers never observe a half-updated set.
//
// Snapshot fields (all guarded by g_batteryStateMutex):
//   g_batteryHealthPercent : OEM health string mapped to 0-100 (-1 unknown).
//                              Good=100, Overheat=60, Dead=20, other=80.
//   g_batteryCycleCount     : reported full charge cycles (-1 unknown).
//   g_batterySohPercent    : v4.3.8 F2.1 — real State-of-Health % from
//                              charge_full/charge_full_design * 100. -1 when the
//                              nodes are absent/unreadable (then the penalty falls
//                              back to health-string + cycle heuristic).
//   g_batteryAged           : v4.3.8 F2.3 — CHRONIC aging only. True when
//                              soh<80, cycles>500, or OEM health=="Dead".
//                              Transient "Overheat" does NOT set this.
//   g_batteryOverheat       : v4.3.8 F2.3 — transient thermal state (OEM health
//                              string contains "Overheat"). Published separately
//                              so it can be observed without being conflated
//                              with chronic wear.
extern std::mutex g_batteryStateMutex;
extern int g_batteryHealthPercent;
extern int g_batteryCycleCount;
extern int g_batterySohPercent;
extern bool g_batteryAged;
extern bool g_batteryOverheat;
void readBatteryHealth(void);
int getBatteryAgePenalty(int cpuId);

// v4.3.9: arbitrator battery signals (audit4_pipeline F1).
// readArbBatteryState() samples capacity% + charging state from sysfs and
// publishes a locked snapshot; getArbBatterySnapshot() copies it. The reader
// (arbitrator tick) calls read then snapshot each cycle. Missing nodes ->
// capacity=-1 (unknown) and charging=0; the arbitrator treats -1 as "no signal".
extern std::mutex g_arbBatteryMutex;
extern int g_arbBatteryCapacityPct;   // 0..100, or -1 = unknown
extern int g_arbBatteryCharging;       // 1 = on charger, 0 = on battery
void readArbBatteryState(void);
void getArbBatterySnapshot(int& capacityPct, int& charging);

#endif // AATPED_BATTERY_H

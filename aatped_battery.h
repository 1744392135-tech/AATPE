#ifndef AATPED_BATTERY_H
#define AATPED_BATTERY_H

#include <atomic>
#include <mutex>

extern std::atomic<int> g_currentPowerMw;
extern std::atomic<int> g_dischargePowerMw;
extern int g_batteryCurrentUa;
extern int g_batteryVoltageUv;
void readBatteryPower(void);
int computePowerPenalty(int cpuId, int threadImportance);

extern std::mutex g_batteryStateMutex;
extern int g_batteryHealthPercent;
extern int g_batteryCycleCount;
extern int g_batterySohPercent;
extern bool g_batteryAged;
extern bool g_batteryOverheat;
void readBatteryHealth(void);
int getBatteryAgePenalty(int cpuId);

extern std::mutex g_arbBatteryMutex;
extern int g_arbBatteryCapacityPct;
extern int g_arbBatteryCharging;
void readArbBatteryState(void);
void getArbBatterySnapshot(int& capacityPct, int& charging);

#endif

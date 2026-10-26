// battery_deps.cpp - host link deps for aatped_battery.cpp (NOT linked with
// v94_daemon_deps, which would multiply-define g_batteryAged). Provides only
// the externs battery.cpp references but does not itself define.
#include "aatped.h"

Config g_cfg{};
void snapshotConfig(Config& out) { out = g_cfg; }

// Deterministic pool mapping so computePowerPenalty / getBatteryAgePenalty can
// be exercised across LITTLE/MID/BIG without a real sysfs topology.
CpuPool getPoolForCpu(int cpu) {
    if (cpu >= 6) return CpuPool::BIG;
    if (cpu >= 3) return CpuPool::MID;
    return CpuPool::LITTLE;
}

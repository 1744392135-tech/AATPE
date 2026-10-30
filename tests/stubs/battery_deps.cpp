#include "aatped.h"

Config g_cfg{};
void snapshotConfig(Config& out) { out = g_cfg; }

CpuPool getPoolForCpu(int cpu) {
    if (cpu >= 6) return CpuPool::BIG;
    if (cpu >= 3) return CpuPool::MID;
    return CpuPool::LITTLE;
}

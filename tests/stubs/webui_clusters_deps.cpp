#include <vector>
#include "aatped.h"

std::vector<CpuCluster> g_clusters;
const std::vector<CpuCluster>& clusters() { return g_clusters; }

std::vector<AatpePerAppFrameStat> g_stubFrameAppStats;
std::string g_stubFrameAppCurrent;
int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>& out, std::string& currentPkg) {
    out = g_stubFrameAppStats;
    currentPkg = g_stubFrameAppCurrent;
    return (int)out.size();
}

#include "aatpe_diag.h"
aatpe::diag::SystemPressure g_stubPsi;
namespace aatpe { namespace diag {
SystemPressure getSystemPressureSnapshot() { return g_stubPsi; }
}}

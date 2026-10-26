// webui_clusters_deps.cpp - ROUND-007e
// webui_json.cpp writeModelJson() derives the PRIME pool cpus from clusters()
// while holding g_topologyRwMutex. On-device this lives in aatped_topology.cpp.
// The webui_json hermetic test does NOT link aatped_topology.o (it would drag in
// aatped_gpu/utils/scoring); it links this stub so the test can seed a D1100
// layout. The topology target links the real aatped_topology.o, which is why
// clusters()/g_clusters must NOT also live in v94_daemon_deps.cpp (multiple defn).
#include <vector>
#include "aatped.h"

std::vector<CpuCluster> g_clusters;
const std::vector<CpuCluster>& clusters() { return g_clusters; }

// W2: per-app rolling frame accessor stub. The real implementation lives in
// frame.cpp (currently being refactored) and is NOT linked into the webui_json
// hermetic test. The test seeds these globals to drive aatpe_buildFrameAppsJson().
std::vector<AatpePerAppFrameStat> g_stubFrameAppStats;
std::string g_stubFrameAppCurrent;
int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>& out, std::string& currentPkg) {
    out = g_stubFrameAppStats;
    currentPkg = g_stubFrameAppCurrent;
    return (int)out.size();
}

// v4.2.2 P3-1: PSI snapshot stub. The real accessor lives in aatpe_diag.cpp (not
// linked into the hermetic webui_json test). The test seeds g_stubPsi to drive the
// psi block round-trip.
#include "aatpe_diag.h"
aatpe::diag::SystemPressure g_stubPsi;
namespace aatpe { namespace diag {
SystemPressure getSystemPressureSnapshot() { return g_stubPsi; }
}}

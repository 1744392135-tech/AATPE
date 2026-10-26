// placement_heal_decision.cpp - v4.4.1 P0 (red-team MAJOR-2).
//
// The PURE, side-effect-free decision for whether ONE foreground critical thread
// should be lifted out of a background cpuset (0-3) back into top-app. It is
// deliberately split into its own zero-dependency translation unit: it touches no
// globals, no /proc, no /dev, only std::string comparisons and role constants, so
// the hermetic host test can link just this one object instead of the whole
// placement graph. placement.cpp (the IO/thread enumeration side) calls this.
#include <string>
#include "aatped.h"
#include "aatped_types.h"

namespace aatpe { namespace placement {
bool healShouldLiftThread(
    const std::string& threadPkg, const std::string& fgPkg,
    int threadRole, const std::string& liveCgroup,
    const std::string& controlMode, bool thermalEmergency,
    int thermalHalLevel, bool cpusetTopAppCap, bool screenOn) {
    if (!screenOn) return false;                 // MAJOR-1: never lift while screen off/standby
    if (controlMode == "passive") return false;  // passive: no proactive migration
    // v6.0.0 P0 (realdevice "locked little core"): split the thermal wall into two
    // DISTINCT signals. thermalEmergency is the CPU sysfs temperature wall
    // (anyCpuTempC >= ~120C hard ceiling, sustained) — a REAL CPU-overheat state,
    // and we keep the wall (do not migrate/lift during genuine CPU emergency).
    // thermalHalLevel is the Android thermal-service mStatus WORST zone, which on
    // MTK dimensity platforms trips SEVERE(3)/CRITICAL(4) on SKIN temperature
    // (~45-50C skin, CPU only ~72C) while the CPU is nowhere near its wall. A
    // skin-throttle Android cpufreq already governs power; lifting a foreground
    // thread's cpuset membership to top-app only changes AFFINITY (which CPUs it
    // is ALLOWED on), it does not raise frequency or add load. So during skin
    // throttle with NO CPU emergency, cgroup heal must NOT be walled — otherwise
    // AMS parks the foreground game in the background cpuset (0-3) and nobody
    // lifts it back, permanently pinning it to the little cluster.
    if (thermalEmergency) return false;        // real CPU temperature wall -> keep wall
    // NOTE: thermalHalLevel>=3 ALONE (skin throttle, !thermalEmergency) is NO LONGER a wall here.
    (void)thermalHalLevel;  // kept in the signature for callers/telemetry; no longer a lift gate.
    if (!cpusetTopAppCap) return false;          // capability gate
    if (fgPkg.empty() || fgPkg == "unknown") return false;
    if (threadPkg != fgPkg) return false;        // never touch a bg / other package
    if (threadRole != ROLE_S1_UI && threadRole != ROLE_S2_RENDER && threadRole != ROLE_S3_GAME)
        return false;                            // only critical roles
    if (liveCgroup.empty() || liveCgroup == "top-app") return false;  // already there / unknown
    return true;
}
}}

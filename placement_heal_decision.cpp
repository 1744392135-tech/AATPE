#include <string>
#include "aatped.h"
#include "aatped_types.h"

namespace aatpe { namespace placement {
bool healShouldLiftThread(
    const std::string& threadPkg, const std::string& fgPkg,
    int threadRole, const std::string& liveCgroup,
    const std::string& controlMode, bool thermalEmergency,
    int thermalHalLevel, bool cpusetTopAppCap, bool screenOn) {
    if (!screenOn) return false;
    if (controlMode == "passive") return false;
    if (thermalEmergency) return false;
    (void)thermalHalLevel;
    if (!cpusetTopAppCap) return false;
    if (fgPkg.empty() || fgPkg == "unknown") return false;
    if (threadPkg != fgPkg) return false;
    if (threadRole != ROLE_S1_UI && threadRole != ROLE_S2_RENDER && threadRole != ROLE_S3_GAME)
        return false;
    if (liveCgroup.empty() || liveCgroup == "top-app") return false;
    return true;
}
}}

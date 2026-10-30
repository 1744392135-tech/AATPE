
#include <string>
#include <cstdint>

#include "touch_uclamp.h"
#include "aatpe_placement_mech.h"

namespace aatpe {

uint32_t touchUclampCollectForegroundThreads(const std::string& fgPackage,
                                            FgThread* out, uint32_t cap) {
    (void)fgPackage;
    (void)out;
    (void)cap;
    return 0u;
}

}

namespace aatpe_mech {

__attribute__((weak)) int aatpeUclampSetMin(int tid, unsigned int utilMin) {
    (void)tid;
    (void)utilMin;
    return 0;
}

}

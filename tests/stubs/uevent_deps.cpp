// uevent_deps.cpp - extra host link deps for aatped_uevent.cpp. The existing
// v94_daemon_deps stub covers pools/mutexes/counters; this adds the one symbol
// uevent.cpp references that lives in aatped_state.cpp on device.
#include "aatped.h"
#include <string>

__attribute__((weak)) std::string aatpe_getThermalCpuZonePath(void) {
    return std::string();  // host: no sysfs -> caller falls back to scanning zones
}

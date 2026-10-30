#include "aatped.h"
#include <string>

__attribute__((weak)) std::string aatpe_getThermalCpuZonePath(void) {
    return std::string();
}

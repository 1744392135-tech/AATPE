#include <cstdint>
#include <cstddef>
#include <string>
#include "aatpe_command_dispatcher.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    std::string cmd(reinterpret_cast<const char*>(data), size);
    volatile aatpe::DispatchResult r1 =
        aatpe::CommandDispatcher::instance().dispatch(cmd, aatpe::DispatchAuth::NONE);
    volatile aatpe::DispatchResult r2 =
        aatpe::CommandDispatcher::instance().dispatch(cmd, aatpe::DispatchAuth::ROOT);
    (void)r1.httpStatus; (void)r2.httpStatus;
    return 0;
}

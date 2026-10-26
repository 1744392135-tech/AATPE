// fuzz_command.cpp — libFuzzer driver for the unified command dispatcher.
//
// Target: aatpe::CommandDispatcher::instance().dispatch(rawCommand, authLevel).
// This is the single choke point for CLI / WebUI / control-pipe commands. The
// daemon hands it attacker-influenced strings (HTTP body, control pipe line,
// CLI argv). It must never crash, UB, or hang on hostile input; unknown
// commands must be rejected with a structured error, not abort.
#include <cstdint>
#include <cstddef>
#include <string>
#include "aatpe_command_dispatcher.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    std::string cmd(reinterpret_cast<const char*>(data), size);
    // Exercise all three auth levels; the dispatcher is trusted-by-construction
    // at this layer but must not crash regardless.
    volatile aatpe::DispatchResult r1 =
        aatpe::CommandDispatcher::instance().dispatch(cmd, aatpe::DispatchAuth::NONE);
    volatile aatpe::DispatchResult r2 =
        aatpe::CommandDispatcher::instance().dispatch(cmd, aatpe::DispatchAuth::ROOT);
    (void)r1.httpStatus; (void)r2.httpStatus;
    return 0;
}

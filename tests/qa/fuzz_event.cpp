#include <cstdint>
#include <cstddef>
#include <string>
#include "uevent_monitor.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    std::string raw(reinterpret_cast<const char*>(data), size);
    auto msg = aatpe::event::UeventMonitor::parseUevent(
        reinterpret_cast<const char*>(data), size);
    volatile auto type = aatpe::event::UeventMonitor::classifyMessage(msg);
    (void)type;
    volatile size_t sink = msg.action.size() + msg.devpath.size() + msg.subsystem.size();
    (void)sink;
    return 0;
}

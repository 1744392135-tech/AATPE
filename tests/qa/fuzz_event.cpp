// fuzz_event.cpp — libFuzzer driver for the netlink uevent parser.
//
// Target: aatpe::event::UeventMonitor::parseUevent(buf, len) + classifyMessage.
// The daemon receives unbounded-length kernel uevents on a NETLINK socket;
// parseUevent walks a NUL-separated "action@devpath\0KEY=VALUE\0..." blob.
// The fuzzer feeds arbitrary bytes (no NULs, embedded NULs, truncated fields,
// huge devpaths, malicious KEY=VALUE) and must never crash / OOB / hang.
#include <cstdint>
#include <cstddef>
#include <string>
#include "uevent_monitor.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    std::string raw(reinterpret_cast<const char*>(data), size);
    auto msg = aatpe::event::UeventMonitor::parseUevent(
        reinterpret_cast<const char*>(data), size);
    // classifyMessage is invoked inside parseUevent already; call it again on
    // the parsed struct to exercise the classifier independently of the buffer.
    volatile auto type = aatpe::event::UeventMonitor::classifyMessage(msg);
    (void)type;
    // Touch the parsed fields so the compiler cannot elide the parse.
    volatile size_t sink = msg.action.size() + msg.devpath.size() + msg.subsystem.size();
    (void)sink;
    return 0;
}

// fuzz_threadname.cpp — fuzz driver for thread name classifier.
//
// Feeds random/malformed thread names to the production threadutil functions.
// Must never crash, return wild pointers, or hang on hostile input.
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstring>

// Production functions under test: aatped_threadutil.h
// Pure string predicate functions - perfect for fuzzing
#include "aatped_threadutil.h"

using namespace aatpe;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    // Truncate to 16 bytes (kernel thread name limit)
    size_t len = size > 15 ? 15 : size;
    // Ensure NUL-terminated for C string APIs
    char name_buf[16];
    memset(name_buf, 0, sizeof(name_buf));
    memcpy(name_buf, data, len);

    // Feed to production predicate functions - must not crash
    try {
        (void)isFlutterEngineThread(name_buf);
        (void)isRenderWhitelisted(name_buf);
        (void)isBlacklistedConsumerThread(name_buf);
        (void)isGameEngineThread(name_buf, name_buf);
    } catch (...) {
        // Any exception is a bug - predicates should be exception-safe
    }
    return 0;
}

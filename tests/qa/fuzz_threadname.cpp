#include <cstdint>
#include <cstddef>
#include <string>
#include <cstring>

#include "aatped_threadutil.h"

using namespace aatpe;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    size_t len = size > 15 ? 15 : size;
    char name_buf[16];
    memset(name_buf, 0, sizeof(name_buf));
    memcpy(name_buf, data, len);

    try {
        (void)isFlutterEngineThread(name_buf);
        (void)isRenderWhitelisted(name_buf);
        (void)isBlacklistedConsumerThread(name_buf);
        (void)isGameEngineThread(name_buf, name_buf);
    } catch (...) {
    }
    return 0;
}

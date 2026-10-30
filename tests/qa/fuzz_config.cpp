#include <cstdint>
#include <cstddef>
#include <string>
#include "aatpe_config_schema.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    size_t cut = size;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == '\0') { cut = i; break; }
    }
    if (cut == size) cut = size / 2;
    std::string key(reinterpret_cast<const char*>(data), cut);
    std::string value(reinterpret_cast<const char*>(data + cut), size - cut);

    std::string out;
    (void)aatpe::configSchemaValidate(key, value, out);
    (void)aatpe::configSchemaLookup(key);
    return 0;
}

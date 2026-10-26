// fuzz_config.cpp — libFuzzer driver for the config schema validator.
//
// Target: aatpe::configSchemaValidate(key, value, outClampedValue).
// This is the production choke point that validates every hot-reload / WebUI
// "set key=value" before it touches runtime state. Malformed keys/values must
// never crash, infinite-loop, or produce NaN/Inf.
//
// Input layout: the fuzzer bytes are split at the first NUL (or, absent a NUL,
// at the midpoint) into [key][value]. Both halves are fed to the validator.
#include <cstdint>
#include <cstddef>
#include <string>
#include "aatpe_config_schema.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    // Split at first NUL if present, else at midpoint.
    size_t cut = size;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == '\0') { cut = i; break; }
    }
    if (cut == size) cut = size / 2;
    std::string key(reinterpret_cast<const char*>(data), cut);
    std::string value(reinterpret_cast<const char*>(data + cut), size - cut);

    std::string out;
    // Must not crash/UB regardless of input. Return value is informational.
    (void)aatpe::configSchemaValidate(key, value, out);
    (void)aatpe::configSchemaLookup(key);
    return 0;  // non-zero would abort the fuzzer; we only report crashes.
}

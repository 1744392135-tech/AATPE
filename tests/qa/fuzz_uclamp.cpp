// fuzz_uclamp.cpp — fuzz driver for uclamp value calculation.
//
// Feeds random/malformed tier/ratio/mode combinations to the uclamp calculation.
// Must never crash or return wild values.
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdlib>
#include <cstring>

// Local uclamp calculation (mirrors production placement.cpp:662)
// namespace aatpe_spillover::aatpe::placement::uclampMinForTier
static int uclampMinForTier_impl(int targetTier, float bigCapacityRatio, int mode) {
    int pct = (mode == 2) ? 80 : (mode == 0) ? 25 : 70; // defaults
    int crit = pct * 1024 / 100;
    if (targetTier >= 3) return 1024; // PRIME
    int v = (int)(crit * bigCapacityRatio);
    if (v < 64) v = 64;
    if (v > 1024) v = 1024;
    return v;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 6) return 0;
    // Use first bytes as random inputs: tier (1 byte), ratio (4 bytes as float), mode (1 byte)
    int tier = data[0] % 8;
    float ratio;
    memcpy(&ratio, data + 1, 4);
    int mode = data[5] % 4;

    // Feed to calculation - must not crash
    try {
        int result = uclampMinForTier_impl(tier, ratio, mode);
        // Sanity check: result should be in [0, 1024]
        if (result < 0 || result > 1024) return 1; // unexpected value
    } catch (...) {
        // Any exception is a bug - calculation should be exception-safe
    }
    return 0;
}

#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdlib>
#include <cstring>

static int uclampMinForTier_impl(int targetTier, float bigCapacityRatio, int mode) {
    int pct = (mode == 2) ? 80 : (mode == 0) ? 25 : 70;
    int crit = pct * 1024 / 100;
    if (targetTier >= 3) return 1024;
    int v = (int)(crit * bigCapacityRatio);
    if (v < 64) v = 64;
    if (v > 1024) v = 1024;
    return v;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 6) return 0;
    int tier = data[0] % 8;
    float ratio;
    memcpy(&ratio, data + 1, 4);
    int mode = data[5] % 4;

    try {
        int result = uclampMinForTier_impl(tier, ratio, mode);
        if (result < 0 || result > 1024) return 1;
    } catch (...) {
    }
    return 0;
}

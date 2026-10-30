#include <cstdio>
#include "aatped.h"
#include "aatped_gpu.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== GPU: readGpuUtilization with no sysfs on host ===\n");
    readGpuUtilization();
    CHECK(g_gpuUtilization.load() == 0.0f, "no sysfs -> gpu util clamped to 0");
    CHECK(g_gpuBusyPercent == 0, "no sysfs -> busy clamped to 0");
    CHECK(g_gpuState.available == false, "no sysfs -> unavailable");

    printf("=== GPU: isGpuBottleneck unavailable guard ===\n");
    bool b0 = isGpuBottleneck(30.0f);
    CHECK(b0 == false, "gpu unavailable -> not a bottleneck");

    printf("=== GPU: bottleneck gate (busy>80 && cpu<50) ===\n");
    g_gpuBusyPercent = 95;
    bool b1 = isGpuBottleneck(30.0f);
    CHECK(b1 == true, "gpu busy 95 + cpu 30 -> bottleneck");
    CHECK(g_gpuBottleneck == true, "g_gpuBottleneck flag set");
    bool b2 = isGpuBottleneck(70.0f);
    CHECK(b2 == false, "gpu busy 95 + cpu 70 -> not bottleneck");

    printf("=== GPU: decision interval doubles on bottleneck ===\n");
    g_gpuBottleneck = true;
    CHECK(getGpuBottleneckDecisionInterval() == 200, "bottleneck -> 200ms interval");
    g_gpuBottleneck = false;
    CHECK(getGpuBottleneckDecisionInterval() == 100, "normal -> 100ms interval");

    printf("\n=== GPU: GED freq parsing (v4.2.7) ===\n");
    CHECK(aatpeParseGedFreqMhz("curfreq=836000;util=4;levelcnt=33") == 836, "GED: key=value format → 836 MHz");
    CHECK(aatpeParseGedFreqMhz("836000") == 836, "GED: raw number → 836 MHz");
    CHECK(aatpeParseGedFreqMhz("836") == 836, "GED: already MHz → 836 MHz");
    CHECK(aatpeParseGedFreqMhz("") == 0, "GED: empty → 0");
    CHECK(aatpeParseGedFreqMhz("no digits here") == 0, "GED: no digits → 0");

    printf("\n========================================\n"
           "GPU results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

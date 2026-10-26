// test_caps_pure.cpp — host tests for the PURE decision helpers in aatpe_caps
// that test_caps.cpp (which drives the on-disk probe) does not cover:
//   - classifyGpuPlatform() priority order (kgsl > mali > ged > none)
//   - inferCgroupUclampScaleReadOnly() threshold boundaries (fail-safe to unknown)
//
// Security property: an unreadable/garbage uclamp node must resolve to "unknown"
// (callers treat as percent = safe under-boost) and never to "raw1024".
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "aatpe_caps.h"

using namespace aatpe::caps;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

int main() {
    printf("=== classifyGpuPlatform: priority kgsl > mali > ged > none ===\n");
    {
        CHECK(strcmp(classifyGpuPlatform(false,false,false,false,false,false), "none") == 0,
              "no GPU flags -> 'none'");
        // kgsl wins even when mali/gpu flags also present
        CHECK(strcmp(classifyGpuPlatform(/*devfreqGpu=*/true, /*devfreqMali=*/true,
                    /*devfreqHasKgsl=*/false, /*kgslNodePresent=*/true,
                    false, false), "kgsl") == 0,
              "kgslNodePresent beats mali -> 'kgsl'");
        CHECK(strcmp(classifyGpuPlatform(false, false, /*devfreqHasKgsl=*/true,
                    false, false, false), "kgsl") == 0,
              "devfreqHasKgsl -> 'kgsl'");
        // mali when no kgsl
        CHECK(strcmp(classifyGpuPlatform(false, /*devfreqHasMali=*/true, false,
                    false, false, false), "mali") == 0,
              "mali flag only -> 'mali'");
        CHECK(strcmp(classifyGpuPlatform(/*devfreqHasGpu=*/true, false, false,
                    false, false, false), "mali") == 0,
              "generic devfreq gpu -> 'mali'");
        // ged only
        CHECK(strcmp(classifyGpuPlatform(false, false, false, false,
                    /*gedHalPresent=*/true, false), "ged") == 0,
              "gedHal only -> 'ged'");
        CHECK(strcmp(classifyGpuPlatform(false, false, false, false,
                    false, /*gedDevPresent=*/true), "ged") == 0,
              "gedDev only -> 'ged'");
        // kgsl beats ged even when both
        CHECK(strcmp(classifyGpuPlatform(false, false, false, true, true, true), "kgsl") == 0,
              "kgsl beats ged -> 'kgsl'");
    }

    printf("=== inferCgroupUclampScaleReadOnly: fail-safe boundaries ===\n");
    {
        CHECK(strcmp(inferCgroupUclampScaleReadOnly(""), "unknown") == 0,
              "empty node value -> 'unknown' (safe)");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("notanumber"), "unknown") == 0,
              "non-numeric node value -> 'unknown' (safe)");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("0"), "percent") == 0,
              "0 -> 'percent'");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("50"), "percent") == 0,
              "50 -> 'percent'");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("100"), "percent") == 0,
              "100 -> 'percent'");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("101.5"), "percent") == 0,
              "101.5 exactly -> 'percent' (threshold is >101.5)");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("101.6"), "raw1024") == 0,
              "101.6 -> 'raw1024'");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("102"), "raw1024") == 0,
              "102 -> 'raw1024'");
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("800"), "raw1024") == 0,
              "800 -> 'raw1024'");
        // negative is a valid percent-scale resting value, never raw1024
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("-5"), "percent") == 0,
              "-5 -> 'percent'");
    }

    printf("\n========================================\n");
    printf("caps_pure: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

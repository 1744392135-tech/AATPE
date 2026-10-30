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
        CHECK(strcmp(classifyGpuPlatform( true, true,
 false, true,
                    false, false), "kgsl") == 0,
              "kgslNodePresent beats mali -> 'kgsl'");
        CHECK(strcmp(classifyGpuPlatform(false, false, true,
                    false, false, false), "kgsl") == 0,
              "devfreqHasKgsl -> 'kgsl'");
        CHECK(strcmp(classifyGpuPlatform(false, true, false,
                    false, false, false), "mali") == 0,
              "mali flag only -> 'mali'");
        CHECK(strcmp(classifyGpuPlatform( true, false, false,
                    false, false, false), "mali") == 0,
              "generic devfreq gpu -> 'mali'");
        CHECK(strcmp(classifyGpuPlatform(false, false, false, false,
 true, false), "ged") == 0,
              "gedHal only -> 'ged'");
        CHECK(strcmp(classifyGpuPlatform(false, false, false, false,
                    false, true), "ged") == 0,
              "gedDev only -> 'ged'");
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
        CHECK(strcmp(inferCgroupUclampScaleReadOnly("-5"), "percent") == 0,
              "-5 -> 'percent'");
    }

    printf("\n========================================\n");
    printf("caps_pure: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

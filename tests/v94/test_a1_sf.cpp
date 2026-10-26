// test_a1_sf.cpp — v4.2 Batch A1: SurfaceFlinger/compositor whitelist truth table.
// Links the real aatped_threadutil.cpp (no mocks). 15-char-comm truncated, so we
// test both exact and truncated-prefix forms. VR/AR scenes must be EXCLUDED.
#include <cstdio>
#include "aatped_threadutil.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

using namespace aatpe;

int main() {
    printf("=== A1: aatpeIsSurfaceFlingerThread truth table ===\n");
    // Whitelist hits
    CHECK(aatpeIsSurfaceFlingerThread("SurfaceFlinger") == true, "SF exact");
    CHECK(aatpeIsSurfaceFlingerThread("RenderThread") == true, "RenderThread exact");
    CHECK(aatpeIsSurfaceFlingerThread("RenderThread 1") == true, "RenderThread prefix");
    CHECK(aatpeIsSurfaceFlingerThread("CrRendererMain") == true, "CrRendererMain");
    CHECK(aatpeIsSurfaceFlingerThread("VizCompositorTh") == true, "VizCompositorTh prefix");
    CHECK(aatpeIsSurfaceFlingerThread("VizCompositorThr") == true, "Viz truncated 15ch");
    CHECK(aatpeIsSurfaceFlingerThread("UnityGfxDeviceW") == true, "UnityGfxDeviceW prefix");
    // VR/AR exclusions
    CHECK(aatpeIsSurfaceFlingerThread("GoogleVrCore") == false, "GoogleVrCore EXCLUDED");
    CHECK(aatpeIsSurfaceFlingerThread("VrThread:ar.core") == false, "ar.core EXCLUDED");
    // Non-SF threads must NOT match (default-off: they keep the v4.1.1 path)
    CHECK(aatpeIsSurfaceFlingerThread("GLThread 123") == false, "GLThread NOT SF (render-WL but not SF)");
    CHECK(aatpeIsSurfaceFlingerThread("hwuiTask0") == false, "hwuiTask0 NOT SF");
    CHECK(aatpeIsSurfaceFlingerThread("main") == false, "main NOT SF");
    CHECK(aatpeIsSurfaceFlingerThread("Binder:123") == false, "binder NOT SF");
    CHECK(aatpeIsSurfaceFlingerThread("UnityMain") == false, "UnityMain NOT SF (gfx device only)");
    CHECK(aatpeIsSurfaceFlingerThread("") == false, "empty");
    CHECK(aatpeIsSurfaceFlingerThread(nullptr) == false, "nullptr safe");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

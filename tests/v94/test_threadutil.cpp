#include <cstdio>
#include "aatped_threadutil.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

using namespace aatpe;

int main() {
    printf("=== ThreadUtil: Flutter engine thread ===\n");
    CHECK(isFlutterEngineThread("1.ui") == true, "1.ui is flutter UI");
    CHECK(isFlutterEngineThread("2.raster") == true, "2.raster is flutter raster");
    CHECK(isFlutterEngineThread("1.gpu") == true, "1.gpu is flutter gpu");
    CHECK(isFlutterEngineThread("1.platform") == true, "1.platform");
    CHECK(isFlutterEngineThread("1.io") == false, "1.io NOT flutter render (black)");
    CHECK(isFlutterEngineThread("ui") == false, "bare 'ui' NOT flutter");
    CHECK(isFlutterEngineThread("RenderThread") == false, "RenderThread not flutter");
    CHECK(isFlutterEngineThread("123.ui.bak") == false, "123.ui.bak not whole-name");
    CHECK(isFlutterEngineThread("") == false, "empty");
    CHECK(isFlutterEngineThread(nullptr) == false, "nullptr safe");

    printf("=== ThreadUtil: render whitelist ===\n");
    CHECK(isRenderWhitelisted("RenderThread") == true, "RenderThread");
    CHECK(isRenderWhitelisted("hwuiTask0") == true, "hwuiTask0");
    CHECK(isRenderWhitelisted("CrRendererMain") == true, "CrRendererMain");
    CHECK(isRenderWhitelisted("CrGpuMain") == true, "CrGpuMain");
    CHECK(isRenderWhitelisted("VizCompositorTh") == true, "VizCompositorTh prefix");
    CHECK(isRenderWhitelisted("VizCompositorThr") == true, "VizCompositorThr truncated");
    CHECK(isRenderWhitelisted("UnityGfxDeviceW") == true, "UnityGfxDeviceW prefix");
    CHECK(isRenderWhitelisted("GLThread 123") == true, "GLThread prefix");
    CHECK(isRenderWhitelisted("SurfaceFlinger") == true, "SurfaceFlinger");
    CHECK(isRenderWhitelisted("CameraThread") == false, "CameraThread not render WL");
    CHECK(isRenderWhitelisted("FinalizerDaemon") == false, "FinalizerDaemon not render");
    CHECK(isRenderWhitelisted("main") == false, "main not render");

    printf("=== ThreadUtil: blacklisted consumer ===\n");
    CHECK(isBlacklistedConsumerThread("FinalizerDaemon") == true, "FinalizerDaemon");
    CHECK(isBlacklistedConsumerThread("HeapTaskDaemon") == true, "HeapTaskDaemon");
    CHECK(isBlacklistedConsumerThread("ReferenceQueueDaemon") == true, "ReferenceQueueDaemon");
    CHECK(isBlacklistedConsumerThread("Signal Catcher") == true, "Signal Catcher");
    CHECK(isBlacklistedConsumerThread("Jit thread pool") == true, "Jit thread pool");
    CHECK(isBlacklistedConsumerThread("Camera$1") == true, "Camera black");
    CHECK(isBlacklistedConsumerThread("MediaCodec_loop") == true, "MediaCodec black");
    CHECK(isBlacklistedConsumerThread("ExoPlayerImpl") == true, "ExoPlayer black");
    CHECK(isBlacklistedConsumerThread("InputMethod") == true, "IME black");
    CHECK(isBlacklistedConsumerThread("TTAdSdk") == true, "Ad SDK black");
    CHECK(isBlacklistedConsumerThread("RenderThread") == false, "RenderThread not black");
    CHECK(isBlacklistedConsumerThread("UnityMain") == false, "UnityMain not black");

    printf("=== ThreadUtil: game engine thread ===\n");
    CHECK(isGameEngineThread("UnityMain", nullptr) == true, "UnityMain");
    CHECK(isGameEngineThread("UnityGfxDeviceW", nullptr) == true, "UnityGfx");
    CHECK(isGameEngineThread("UnrealGame", nullptr) == true, "UnrealGame");
    CHECK(isGameEngineThread("GameActivity", nullptr) == true, "GameActivity");
    CHECK(isGameEngineThread("VulkanRender", nullptr) == true, "VulkanRender");
    CHECK(isGameEngineThread("bgfx::render", nullptr) == true, "bgfx render");
    CHECK(isGameEngineThread("GameView", nullptr) == true, "GameView");
    CHECK(isGameEngineThread("UnityMain", "com.unity3d") == true, "procUnity fallback");
    CHECK(isGameEngineThread("RenderThread", "com.unity3d") == true, "procUnity from RenderThread");
    CHECK(isGameEngineThread("main", nullptr) == false, "main not engine");
    CHECK(isGameEngineThread("RenderThread", "com.android.chrome") == false, "chrome RenderThread not engine");
    CHECK(isGameEngineThread(nullptr, nullptr) == false, "nullptr safe");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// aatped_threadutil.cpp — 统一线程名角色识别真值实现
#include "aatped_threadutil.h"

#include <cctype>
#include <cstring>

namespace aatpe {

static bool startsWith(const char* s, const char* pre) {
    if (!s || !pre) return false;
    size_t n = std::strlen(pre);
    return std::strncmp(s, pre, n) == 0;
}

static bool contains(const char* s, const char* needle) {
    if (!s || !needle) return false;
    return std::strstr(s, needle) != nullptr;
}

bool isFlutterEngineThread(const char* comm) {
    if (!comm) return false;
    // ^[0-9]+\.(ui|raster|gpu|platform)$
    const char* p = comm;
    if (!std::isdigit((unsigned char)*p)) return false;
    while (std::isdigit((unsigned char)*p)) p++;
    if (*p != '.') return false;
    p++;
    const char* suffix = p;
    static const char* kTypes[] = {"ui", "raster", "gpu", "platform"};
    for (const char* t : kTypes) {
        if (std::strcmp(suffix, t) == 0) return true;
    }
    return false;
}

bool isRenderWhitelisted(const char* comm) {
    if (!comm) return false;
    // exact
    if (std::strcmp(comm, "RenderThread") == 0) return true;
    if (std::strcmp(comm, "CrRendererMain") == 0) return true;
    if (std::strcmp(comm, "CrGpuMain") == 0) return true;
    if (std::strcmp(comm, "RenderEngine") == 0) return true;
    if (std::strcmp(comm, "SurfaceFlinger") == 0) return true;
    if (std::strcmp(comm, "GPU completion") == 0) return true;
    // prefix
    if (startsWith(comm, "hwuiTask")) return true;
    if (startsWith(comm, "GLThread")) return true;
    if (startsWith(comm, "VulkanThread")) return true;
    if (startsWith(comm, "HWC")) return true;
    if (startsWith(comm, "VizCompositorTh")) return true;  // VizCompositorTh(15ch)
    // UnityGfxDeviceW* (15-char truncated prefix)
    if (startsWith(comm, "UnityGfxDeviceW")) return true;
    return false;
}

// v4.2 A1: SurfaceFlinger / system-compositor whitelist. These threads get a
// dedicated low uclamp.floor (UCLAMP_SF_PCT=20%) so they stay schedulable without
// grabbing the PRIME 1024 floor from the foreground game's own render thread.
// 15-char-comm truncated -> exact / prefix wordMatch, same style as
// isRenderWhitelisted. VR/AR scenes are EXCLUDED (they are heavy interactive
// workloads, not pure compositors). Names aligned with the watchdog emergency
// critical-comm list (aatpe-watchdog.cpp:700: surfaceflinger/RenderThread/
// UnityGfxDevice). Conservative: unmatched -> false (caller keeps v4.1.1 path).
bool aatpeIsSurfaceFlingerThread(const char* comm) {
    if (!comm || !*comm) return false;
    // VR/AR exclusion FIRST — these run SF-like compositor threads but must not be
    // floored to the SF tier.
    if (startsWith(comm, "GoogleVrCore")) return false;
    if (contains(comm, "ar.core")) return false;
    // Whitelist.
    if (std::strcmp(comm, "SurfaceFlinger") == 0) return true;   // system service (exact)
    if (startsWith(comm, "RenderThread")) return true;           // app compositor (15ch trunc)
    if (std::strcmp(comm, "CrRendererMain") == 0) return true;   // Chrome/WebView renderer
    if (startsWith(comm, "VizCompositorTh")) return true;        // Viz compositor (15ch trunc)
    if (startsWith(comm, "UnityGfxDeviceW")) return true;         // Unity gfx device (15ch trunc)
    return false;
}

bool isBlacklistedConsumerThread(const char* comm) {
    if (!comm) return false;
    static const char* kBlack[] = {
        // IME
        "InputMethod", "InputMethodManager", "Ime",
        // camera / video
        "Camera", "camera", "MediaCodec", "ExoPlayer", "VideoPlayer",
        // ad SDK
        "AdContainer", "AdWebView", "AdView", "gdt", "GDT", "ocean",
        "mraid", "TTAd", "pangxie",
        // ART daemon
        "FinalizerDaemon", "HeapTaskDaemon", "ReferenceQueueDaemon",
        "Signal Catcher", "Jit thread pool",
    };
    for (const char* b : kBlack) {
        if (contains(comm, b)) return true;
    }
    return false;
}

bool isGameEngineThread(const char* comm, const char* procComm) {
    if (!comm) return false;
    // Unity (UnityMain/UnityGfxDeviceW/UnityRender — all start with "Unity")
    if (startsWith(comm, "Unity")) return true;
    // Unreal
    if (startsWith(comm, "Unreal")) return true;
    if (contains(comm, "GameActivity")) return true;
    if (contains(comm, "com.epicgames")) return true;
    // generic engine render
    if (startsWith(comm, "VulkanRender")) return true;
    if (contains(comm, "bgfx")) return true;
    if (contains(comm, "GameView")) return true;
    // procComm fallback: package-style names embed "unity"/"unreal" as substring
    if (procComm && (contains(procComm, "Unity") || contains(procComm, "unity") ||
                     contains(procComm, "Unreal") || contains(procComm, "unreal") ||
                     contains(procComm, "com.epicgames"))) {
        return true;
    }
    return false;
}

} // namespace aatpe

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
    if (std::strcmp(comm, "RenderThread") == 0) return true;
    if (std::strcmp(comm, "CrRendererMain") == 0) return true;
    if (std::strcmp(comm, "CrGpuMain") == 0) return true;
    if (std::strcmp(comm, "RenderEngine") == 0) return true;
    if (std::strcmp(comm, "SurfaceFlinger") == 0) return true;
    if (std::strcmp(comm, "GPU completion") == 0) return true;
    if (startsWith(comm, "hwuiTask")) return true;
    if (startsWith(comm, "GLThread")) return true;
    if (startsWith(comm, "VulkanThread")) return true;
    if (startsWith(comm, "HWC")) return true;
    if (startsWith(comm, "VizCompositorTh")) return true;
    if (startsWith(comm, "UnityGfxDeviceW")) return true;
    return false;
}

bool aatpeIsSurfaceFlingerThread(const char* comm) {
    if (!comm || !*comm) return false;
    if (startsWith(comm, "GoogleVrCore")) return false;
    if (contains(comm, "ar.core")) return false;
    if (std::strcmp(comm, "SurfaceFlinger") == 0) return true;
    if (startsWith(comm, "RenderThread")) return true;
    if (std::strcmp(comm, "CrRendererMain") == 0) return true;
    if (startsWith(comm, "VizCompositorTh")) return true;
    if (startsWith(comm, "UnityGfxDeviceW")) return true;
    return false;
}

bool isBlacklistedConsumerThread(const char* comm) {
    if (!comm) return false;
    static const char* kBlack[] = {
        "InputMethod", "InputMethodManager", "Ime",
        "Camera", "camera", "MediaCodec", "ExoPlayer", "VideoPlayer",
        "AdContainer", "AdWebView", "AdView", "gdt", "GDT", "ocean",
        "mraid", "TTAd", "pangxie",
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
    if (startsWith(comm, "Unity")) return true;
    if (startsWith(comm, "Unreal")) return true;
    if (contains(comm, "GameActivity")) return true;
    if (contains(comm, "com.epicgames")) return true;
    if (startsWith(comm, "VulkanRender")) return true;
    if (contains(comm, "bgfx")) return true;
    if (contains(comm, "GameView")) return true;
    if (procComm && (contains(procComm, "Unity") || contains(procComm, "unity") ||
                     contains(procComm, "Unreal") || contains(procComm, "unreal") ||
                     contains(procComm, "com.epicgames"))) {
        return true;
    }
    return false;
}

}

// aatped_threadutil.h — 统一线程名角色识别真值（ROUND-014 P3 §2.2）
// G4 唯一实现，G7 thread_scan 与 G4 frame 选层共用，避免两份黑名单漂移。
// 真值基线：findings/ROUND-014-P1-THREADNAMES.md（38 条样本）。
#pragma once

namespace aatpe {

// Flutter 引擎线程：整名匹配 ^[0-9]+\.(ui|raster|gpu|platform)$
//   1.ui/2.ui/1.platform -> UI 面；1.raster/1.gpu -> 渲染面。
//   显式排除 1.io 与任意非"数字.类型"自命名线程。
bool isFlutterEngineThread(const char* comm);

// 渲染白名单（framework/webview/引擎渲染线程）：
//   RenderThread, hwuiTask*, GLThread*, VulkanThread, "GPU completion",
//   RenderEngine, SurfaceFlinger, HWC*, CrRendererMain, CrGpuMain,
//   VizCompositorTh* (Viz 合成), UnityGfxDeviceW* (15 字符截断前缀 wordMatch)。
bool isRenderWhitelisted(const char* comm);

// v4.2 A1: SurfaceFlinger / system-compositor whitelist. Exact/prefix wordMatch
// on "SurfaceFlinger", "RenderThread*", "CrRendererMain", "VizCompositorTh*",
// "UnityGfxDeviceW*"; GoogleVrCore* / *.ar.core* excluded. Unmatched -> false.
bool aatpeIsSurfaceFlingerThread(const char* comm);

// 黑名单消费线程（不抬票/不当游戏层/不当渲染）：
//   IME/InputMethod、camera/Camera、video/MediaCodec/ExoPlayer、
//   广告 SDK (Ad*/gdt/ocean/mraid/TTAd/pangxie)、ART 守护线程
//   (FinalizerDaemon/HeapTaskDaemon/ReferenceQueueDaemon/Signal Catcher)。
bool isBlacklistedConsumerThread(const char* comm);

// 游戏引擎线程（comm + 进程名）：Unity(UnityMain/UnityGfx*/UnityRender)、
//   Unreal(UnrealGame/GameActivity/com.epicgames)、bgfx/VulkanRender/GameView。
// procComm 可为空；comm 命中引擎特征即 true。
bool isGameEngineThread(const char* comm, const char* procComm);

} // namespace aatpe

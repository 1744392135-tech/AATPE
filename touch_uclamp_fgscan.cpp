// touch_uclamp_fgscan.cpp — production foreground thread collector.
//
// Pulls in the live g_threads table (owned by thread_scan.cpp) and maps the
// foreground app's threads onto the core's FgThread role enum. This TU is only
// linked into the production daemon (it needs g_threads / ThreadInfo); the host
// unit test links touch_uclamp.c + touch_uclamp_act.cpp only and feeds the
// controller an explicit FgThread array.
//
// Fail-safe: empty / unmatched foreground -> returns 0 -> the controller then
// emits NO per-thread action (only the low global floor), which is exactly the
// required "空线程集 -> 无 per-thread 动作".
#include "touch_uclamp_act.h"

#include "aatped.h"         // daemon umbrella: aatpe_ml, ThreadInfo/ThreadRole, <mutex>, g_threads decl
#include "aatped_utils.h"   // g_threads, g_threadsMutex (redundant with aatped.h; explicit for clarity)

#include <mutex>

namespace aatpe {

uint32_t touchUclampCollectForegroundThreads(const std::string& fgPackage,
                                            FgThread* out, uint32_t cap)
{
    if (!out || cap == 0u || fgPackage.empty()) return 0u;

    uint32_t n = 0u;
    std::lock_guard<std::mutex> lk(g_threadsMutex);
    for (const auto& kv : g_threads) {
        if (n >= cap) break;
        const ThreadInfo& ti = kv.second;
        if (ti.tid <= 0) continue;
        // Only threads owned by the foreground process.
        if (ti.pname != fgPackage) continue;

        FgThread ft;
        ft.tid = ti.tid;
        // frame_contribution left -1 (unknown): without a real per-thread frame
        // attribution we must NOT fabricate a share. The high-contrib branch
        // therefore stays off on-device unless a real signal is later plumbed.
        ft.frame_contribution = -1.0f;

        if (ti.role == ROLE_S5_BACKGROUND) {
            ft.role = TOUCH_UCLAMP_ROLE_BG;                      // never boosted
        } else if (ti.role == ROLE_S2_RENDER ||
                   ti.tname.find("RenderThread") != std::string::npos) {
            ft.role = TOUCH_UCLAMP_ROLE_RENDER;
        } else if (ti.tid == ti.pid || ti.role == ROLE_S1_UI) {
            ft.role = TOUCH_UCLAMP_ROLE_UI_MAIN;                 // main / UI thread
        } else {
            ft.role = TOUCH_UCLAMP_ROLE_UNKNOWN;                 // core needs contrib>=0.5
        }
        out[n++] = ft;
    }
    return n;
}

}  // namespace aatpe

#include "touch_uclamp_act.h"

#include "aatped.h"
#include "aatped_utils.h"

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
        if (ti.pname != fgPackage) continue;

        FgThread ft;
        ft.tid = ti.tid;
        ft.frame_contribution = -1.0f;

        if (ti.role == ROLE_S5_BACKGROUND) {
            ft.role = TOUCH_UCLAMP_ROLE_BG;
        } else if (ti.role == ROLE_S2_RENDER ||
                   ti.tname.find("RenderThread") != std::string::npos) {
            ft.role = TOUCH_UCLAMP_ROLE_RENDER;
        } else if (ti.tid == ti.pid || ti.role == ROLE_S1_UI) {
            ft.role = TOUCH_UCLAMP_ROLE_UI_MAIN;
        } else {
            ft.role = TOUCH_UCLAMP_ROLE_UNKNOWN;
        }
        out[n++] = ft;
    }
    return n;
}

}

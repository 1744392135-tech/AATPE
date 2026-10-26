// rel12 (Bug1/Bug2) regression: per-app frame accounting on the SukiSU WebUI
// foreground (com.sukisu.ultra, non_game).
//
// Bug1: on-device screenshot showed "丢帧574 / 总帧242" -- an accounting
//   impossibility. Root cause: st.dropped was assigned fm.missedVsyncs (the raw
//   vsync accumulator; one badly-late frame can miss MANY vsyncs, so it can and
//   did exceed the frame count). After the fix, "丢帧 dropped" is the real
//   janky/missed FRAME count and MUST satisfy dropped <= total_frames; the raw
//   vsync count is preserved in the new missed_vsyncs field.
//
// Bug2: the same screenshot showed "前台 0ms" even though the WebUI had been
//   rendering. Root cause: active_ms only summed <=4s gaps between sampling
//   windows, so a freshly-opened app with a single window reported 0. After the
//   fix, the authoritative framestats rendering span (totalFrames / present-rate)
//   is used, so active_ms is non-zero for a real rendering session.
//
// Links the REAL frame.o (same recipe as per_app_frame).
#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "aatped.h"

extern void aatpeSetFrameJank(const char*, int, double, double, double, double);
extern int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>&, std::string&);
extern void aatpeDebugFsMetricsInject(const std::string&, double, double, long, long);

// g_foregroundApp lives in frame.o.
extern std::string g_foregroundApp;

static int g_pass = 0, g_fail = 0;
#define CK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                      else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

int main() {
    mkdir("/tmp/aatpe_frame_rel12", 0755);

    // --- rel14 on-device scenario: 242 frames, 93% janky, fps 2.6, missed 574 vsyncs
    g_foregroundApp = "com.sukisu.ultra";
    // First create a per-app window + LRU entry (so the accessor emits the row).
    aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
    // Then override the authoritative framestats metrics to the rel14 numbers.
    // missedVsyncs (574) deliberately EXCEEDS totalFrames (242).
    aatpeDebugFsMetricsInject("com.sukisu.ultra", 2.6, 0.93, 242, 574);

    std::vector<AatpePerAppFrameStat> out;
    std::string cur;
    int n = aatpe_getPerAppFrameStats(out, cur);
    CK(n > 0, "rel12: per-app stats returned a row");

    const AatpePerAppFrameStat* s = nullptr;
    for (const auto& r : out) {
        if (r.pkg == "com.sukisu.ultra") { s = &r; break; }
    }
    CK(s != nullptr, "rel12: com.sukisu.ultra row present");
    if (!s) return 1;

    printf("  [INFO] dropped=%ld total_frames=%ld missed_vsyncs=%ld jank=%ld active_ms=%ld\n",
           s->dropped, s->total_frames, s->missed_vsyncs, s->jank, s->active_ms);

    // Bug1 invariants.
    CK(s->total_frames == 242, "rel12: total_frames=242 (authoritative framestats)");
    CK(s->missed_vsyncs == 574, "rel12: missed_vsyncs preserves raw vsync accumulator (574)");
    CK(s->dropped <= s->total_frames, "rel12: dropped <= total_frames (no more 574/242)");
    CK(s->dropped >= 0, "rel12: dropped is non-negative");
    // janky frame count = round(0.93 * 242) = 225 (clamped to <= totalFrames).
    CK(s->dropped <= 242 && s->dropped >= 200, "rel12: dropped is the janky frame count (~225), not raw vsyncs");
    CK(s->dropped != 574, "rel12: dropped is no longer the raw missedVsyncs (was 574)");

    // Bug2 invariant: a real rendering session (242 frames at 2.6 fps ~= 93s) must
    // not report active_ms == 0.
    CK(s->active_ms > 1000, "rel12: active_ms reflects real rendering span (>1s, was 0)");
    printf("  [INFO] active_ms=%ld ms (~%.1fs rendering span)\n",
           s->active_ms, s->active_ms / 1000.0);

    printf("\nresults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

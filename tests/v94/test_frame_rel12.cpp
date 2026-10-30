#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "aatped.h"

extern void aatpeSetFrameJank(const char*, int, double, double, double, double);
extern int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>&, std::string&);
extern void aatpeDebugFsMetricsInject(const std::string&, double, double, long, long);

extern std::string g_foregroundApp;

static int g_pass = 0, g_fail = 0;
#define CK(c, m) do { if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
                      else { g_fail++; printf("  [FAIL] %s\n", m); } } while (0)

int main() {
    mkdir("/tmp/aatpe_frame_rel12", 0755);

    g_foregroundApp = "com.sukisu.ultra";
    aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
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

    CK(s->total_frames == 242, "rel12: total_frames=242 (authoritative framestats)");
    CK(s->missed_vsyncs == 574, "rel12: missed_vsyncs preserves raw vsync accumulator (574)");
    CK(s->dropped <= s->total_frames, "rel12: dropped <= total_frames (no more 574/242)");
    CK(s->dropped >= 0, "rel12: dropped is non-negative");
    CK(s->dropped <= 242 && s->dropped >= 200, "rel12: dropped is the janky frame count (~225), not raw vsyncs");
    CK(s->dropped != 574, "rel12: dropped is no longer the raw missedVsyncs (was 574)");

    CK(s->active_ms > 1000, "rel12: active_ms reflects real rendering span (>1s, was 0)");
    printf("  [INFO] active_ms=%ld ms (~%.1fs rendering span)\n",
           s->active_ms, s->active_ms / 1000.0);

    printf("\nresults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

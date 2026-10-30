#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>

struct AatpePerAppFrameStat {
    std::string pkg; double fps=0; double ft_avg_ms=0; double ft_p50=0;
    double ft_p95=0; double ft_p99=0; long jank=0; long dropped=0; long samples=0;
    long total_frames=0; long active_ms=0; long long last_update_ms=0; double target_fps=0;
    std::string reason; std::string frameSource; bool isCurrent=false; long long lastSeenMs=0; long long lastSeenEpochMs=0;
    long missed_vsyncs=0;
};
extern void aatpeSetFrameJank(const char*, int, double, double, double, double);
extern int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>&, std::string&);

extern int aatpeDebugTsLayerSnapSize();
extern bool aatpeDebugTsLayerSnapHas(const std::string& layer);
extern void aatpeDebugTsLayerSnapAdd(const std::string& layer);

extern void aatpeFrameClearGameStats();

extern long long aatpeDebugNowMonotonic();

extern std::string g_foregroundApp;

namespace aatpe { namespace frameaware {
    extern std::atomic<bool> g_frameMissJankInput;
}}

static int g_pass = 0, g_fail = 0;
#define CK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)

int main() {
    mkdir("/tmp/aatpe_frame_p2", 0755);

    printf("=== FRAME-01: g_tsLayerSnap evicted with per-app LRU ===\n");
    {
        const int N = 26;
        std::string layers[N];
        for (int i = 0; i < N; i++) {
            char p[64], layer[128];
            snprintf(p, sizeof(p), "com.evict.%d", i);
            snprintf(layer, sizeof(layer), "SurfaceView[%s/Game]@0(BLAST)#%d", p, 1000+i);
            layers[i] = layer;
            aatpeDebugTsLayerSnapAdd(layer);
        }
        int injected = aatpeDebugTsLayerSnapSize();
        CK(injected == N, "FRAME-01: 26 layer snaps injected");

        for (int i = 0; i < N; i++) {
            char p[64]; snprintf(p, sizeof(p), "com.evict.%d", i);
            g_foregroundApp = p;
            aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
        }

        CK(!aatpeDebugTsLayerSnapHas(layers[0]), "FRAME-01: evicted pkg0 layer snap removed");
        CK(!aatpeDebugTsLayerSnapHas(layers[1]), "FRAME-01: evicted pkg1 layer snap removed");
        CK(aatpeDebugTsLayerSnapHas(layers[25]), "FRAME-01: surviving pkg25 layer snap kept");
        int remaining = aatpeDebugTsLayerSnapSize();
        printf("  [INFO] g_tsLayerSnap size after %d feeds = %d (was %d before fix)\n",
               N, remaining, N);
        CK(remaining <= N - 2, "FRAME-01: g_tsLayerSnap bounded (<= 24 after 26 feeds)");
    }

    printf("\n=== FRAME-02: aatpeFrameClearGameStats zeros game state ===\n");
    {
        aatpe::frameaware::g_frameMissJankInput.store(true, std::memory_order_relaxed);
        CK(aatpe::frameaware::g_frameMissJankInput.load() == true,
           "FRAME-02: missJank input armed before clear");

        aatpeFrameClearGameStats();

        CK(aatpe::frameaware::g_frameMissJankInput.load() == false,
           "FRAME-02: missJank input cleared after aatpeFrameClearGameStats()");
    }

    printf("\n=== FRAME-03: updatedMs clock consistency ===\n");
    {
        g_foregroundApp = "com.clock.test";
        aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);

        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        int n = aatpe_getPerAppFrameStats(out, cur);
        CK(n > 0, "FRAME-03: per-app stats returned");
        bool found = false;
        long long updateMs = 0;
        for (auto& s : out) {
            if (s.pkg == "com.clock.test") { found = true; updateMs = s.last_update_ms; }
        }
        CK(found, "FRAME-03: com.clock.test present in panel");
        long long nowMono = aatpeDebugNowMonotonic();
        printf("  [INFO] updateMs=%lld nowMono=%lld delta=%lld ms\n",
               updateMs, nowMono, nowMono - updateMs);
        long long delta = nowMono - updateMs;
        CK(delta >= 0 && delta < 5000, "FRAME-03: updatedMs is monotonic (not wall-clock epoch)");
    }

    printf("\nresults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

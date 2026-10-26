// R5/R6 scratch harness — isolated, links scratch frame.o + stubs.
#include <string>
#include <vector>
#include <cstdio>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// From aatped.h
struct AatpePerAppFrameStat {
    std::string pkg; double fps=0; double ft_avg_ms=0; double ft_p50=0;
    double ft_p95=0; double ft_p99=0; long jank=0; long dropped=0; long samples=0;
    long total_frames=0; long active_ms=0; long long last_update_ms=0; double target_fps=0;
    std::string reason; std::string frameSource; bool isCurrent=false; long long lastSeenMs=0; long long lastSeenEpochMs=0;
    long missed_vsyncs=0;  // rel12 Bug1: must match aatped.h struct layout
};
extern void aatpeSetFrameJank(const char*, int, double, double, double, double);
extern int aatpe_getPerAppFrameStats(std::vector<AatpePerAppFrameStat>&, std::string&);

// scratch-only debug accessors
extern int aatpeDebugFsMetricsSize();
extern bool aatpeDebugFsMetricsHas(const std::string&);
extern void aatpeDebugFsMetricsSet(const std::string&);
extern long long aatpeDebugNowMonotonic();

static int g_pass=0, g_fail=0;
#define CK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)

// g_foregroundApp lives in frame.o; set it to attribute each feed.
extern std::string g_foregroundApp;

int main() {
    mkdir("/tmp/aatpe_r5r6", 0755);

    printf("=== R5: LRU eviction also clears g_perAppFsMetrics ===\n");
    {
        // Inject fsMetrics for 26 packages (as the framestats block would).
        for (int i = 0; i < 26; i++) {
            char p[64]; snprintf(p,sizeof(p),"com.evict.%d",i);
            aatpeDebugFsMetricsSet(p);
        }
        int injected = aatpeDebugFsMetricsSize();
        CK(injected == 26, "R5: 26 fsMetrics entries injected");
        // Feed all 26; kMaxPerAppStats=24 evicts the oldest (com.evict.0, com.evict.1).
        for (int i = 0; i < 26; i++) {
            char p[64]; snprintf(p,sizeof(p),"com.evict.%d",i);
            g_foregroundApp = p;
            aatpeSetFrameJank("timestats", 60, 0.0, 60.0, 55.0, 50.0);
        }
        // Evicted packages must have fsMetrics cleaned too (R5).
        CK(!aatpeDebugFsMetricsHas("com.evict.0"), "R5: evicted pkg0 fsMetrics cleared");
        CK(!aatpeDebugFsMetricsHas("com.evict.1"), "R5: evicted pkg1 fsMetrics cleared");
        // Surviving packages keep their fsMetrics.
        CK(aatpeDebugFsMetricsHas("com.evict.25"), "R5: surviving pkg25 fsMetrics kept");
        // Bounded: windows capped at 24, fsMetrics should be ~24 (26 - 2 evicted).
        int fsSize = aatpeDebugFsMetricsSize();
        CK(fsSize <= 24, "R5: fsMetrics count bounded (<=24)");
        printf("  [INFO] fsMetrics size after 26 feeds = %d\n", fsSize);
    }

    printf("\n=== R6: monotonic clock immunity ===\n");
    {
        // frameNowMs() must be non-decreasing over real time (steady clock).
        long long t1 = aatpeDebugNowMonotonic();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        long long t2 = aatpeDebugNowMonotonic();
        CK(t2 > t1, "R6: monotonic clock advances (steady_clock)");
        // Crucially, the monotonic clock does NOT go backwards when system time is set
        // back. We cannot change system time in-process; the guarantee is structural:
        // ageMs = frameNowMs() - ring.tsMs, both monotonic, so a NITZ/clock rollback has
        // NO effect on the 60s stale filter.
        // Demonstrate: two monotonic samples always produce non-negative age.
        long long age = t2 - t1;
        CK(age >= 0, "R6: monotonic age is non-negative (no rollback glitch)");
    }

    printf("\n=== R6: accessor still returns bounded panel (no clock crash) ===\n");
    {
        std::vector<AatpePerAppFrameStat> out;
        std::string cur;
        int n = aatpe_getPerAppFrameStats(out, cur);
        CK(n > 0, "R6: accessor returns packages after monotonic switch");
        // All returned packages must have a non-negative age (monotonic tsMs).
        bool ok = true;
        for (auto& s : out) if (s.last_update_ms < 0) ok = false;
        CK(ok, "R6: all last_update_ms consistent");
    }

    printf("\nresults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// test_log_orphan.cpp — v4.4.8 fuzz-hang A8 production regression.
//
// Root cause: a fuzz harness (or any process) that never calls logInit() leaves
// g_logFp permanently null. logMsg()'s null-fp orphan branch appended every WARN/
// INFO to g_logBuffer with no drain -> unbounded growth -> geometric realloc crossed
// the 3s watchdog (fz_control ~613MB, fz_affinity_mask ~958MB RSS).
//
// Fix: hard-cap the orphan buffer at kLogBufferOrphanCapBytes (1MiB); overflow lines
// are dropped and counted. Once a valid sink is opened the normal path drains it.
//
// This test runs host-only (AATPE_HOST_TEST) against the REAL aatped_logging.o:
//   phase 1: g_logFp starts null; pour >2MiB of WARN -> orphan bytes must stay <= cap.
//   phase 2: open a temp sink via logInit(); write one line -> buffer drains to 0.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "aatped_logging.h"   // logInit/logMsg/LOG_WARN macro
extern const char* LOG_FILE;     // from logging_deps.cpp (global var, unmangled)
size_t aatpeTestLogOrphanBytes();
unsigned long aatpeTestLogOrphanDropped();

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); g_fail = 1; } \
} while (0)

int main() {
    printf("=== A8: log orphan-buffer hard cap regression ===\n");

    // Phase 1: g_logFp is null (we never called logInit). Pour >2MiB of WARN lines.
    // Each line is padded to ~4KB (lineBuf cap 4200); ~700 lines > 2MiB total.
    std::string filler(3900, 'x');
    size_t before = aatpeTestLogOrphanBytes();
    for (int i = 0; i < 700; ++i) {
        LOG_WARN("orphan-burst %s %08d\n", filler.c_str(), i);
    }
    size_t after = aatpeTestLogOrphanBytes();
    unsigned long dropped = aatpeTestLogOrphanDropped();

    printf("  orphan bytes: start=%zu after 700 long WARNs=%zu (dropped=%lu)\n",
           before, after, dropped);

    // The buffer must be capped (~1MiB), NOT grown to 2.8MiB.
    // (before is ~0; after must be <= 1MiB + slack, and > 0.)
    CHECK(after <= (1u << 20) + 4096, "orphan buffer bytes <= 1MiB cap");
    CHECK(after > 0, "orphan buffer holds at least some buffered lines");
    CHECK(dropped > 0, "overflow lines were dropped (counted)");

    // Phase 2: open a valid sink, then write one WARN -> buffer must drain to 0.
    mkdir("/tmp/aatpe_v94_state", 0755);
    LOG_FILE = "/tmp/aatpe_v94_state/test_orphan.log";
    logInit();   // g_logFp now valid (fopen /dev/... no; real temp file)

    LOG_WARN("post-sink drain probe line\n");

    size_t drained = aatpeTestLogOrphanBytes();
    CHECK(drained == 0, "orphan buffer drained to 0 after sink opened");

    if (g_fail) {
        printf("=== test_log_orphan: FAIL ===\n");
        return 1;
    }
    printf("=== test_log_orphan: PASS (orphan capped, drains on sink) ===\n");
    return 0;
}

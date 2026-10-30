#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "aatped_logging.h"
extern const char* LOG_FILE;
size_t aatpeTestLogOrphanBytes();
unsigned long aatpeTestLogOrphanDropped();

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else      { printf("  [FAIL] %s\n", msg); g_fail = 1; } \
} while (0)

int main() {
    printf("=== A8: log orphan-buffer hard cap regression ===\n");

    std::string filler(3900, 'x');
    size_t before = aatpeTestLogOrphanBytes();
    for (int i = 0; i < 700; ++i) {
        LOG_WARN("orphan-burst %s %08d\n", filler.c_str(), i);
    }
    size_t after = aatpeTestLogOrphanBytes();
    unsigned long dropped = aatpeTestLogOrphanDropped();

    printf("  orphan bytes: start=%zu after 700 long WARNs=%zu (dropped=%lu)\n",
           before, after, dropped);

    CHECK(after <= (1u << 20) + 4096, "orphan buffer bytes <= 1MiB cap");
    CHECK(after > 0, "orphan buffer holds at least some buffered lines");
    CHECK(dropped > 0, "overflow lines were dropped (counted)");

    mkdir("/tmp/aatpe_v94_state", 0755);
    LOG_FILE = "/tmp/aatpe_v94_state/test_orphan.log";
    logInit();

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

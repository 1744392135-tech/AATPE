#include <cstdio>
#include "aatpe_sched_abi.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++fail; } \
    else         { printf("ok:   %s\n", msg); } \
  } while (0)

int main() {
    int fail = 0;

    printf("=== ROUND-W3 SCHED_FLAG UAPI verification ===\n");
    printf("KEEP_POLICY    = 0x%02lx  (expect 0x08)\n", (unsigned long)SCHED_FLAG_KEEP_POLICY);
    printf("KEEP_PARAMS    = 0x%02lx  (expect 0x10)\n", (unsigned long)SCHED_FLAG_KEEP_PARAMS);
    printf("UTIL_CLAMP_MIN = 0x%02lx  (expect 0x20)\n", (unsigned long)SCHED_FLAG_UTIL_CLAMP_MIN);
    printf("UTIL_CLAMP_MAX = 0x%02lx  (expect 0x40)\n", (unsigned long)SCHED_FLAG_UTIL_CLAMP_MAX);

    CHECK(SCHED_FLAG_KEEP_POLICY    == 0x08, "KEEP_POLICY == 0x08");
    CHECK(SCHED_FLAG_KEEP_PARAMS    == 0x10, "KEEP_PARAMS == 0x10");
    CHECK(SCHED_FLAG_UTIL_CLAMP_MIN == 0x20, "UTIL_CLAMP_MIN == 0x20");
    CHECK(SCHED_FLAG_UTIL_CLAMP_MAX == 0x40, "UTIL_CLAMP_MAX == 0x40");

    unsigned long step2_flags =
        SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS |
        SCHED_FLAG_UTIL_CLAMP_MIN | SCHED_FLAG_UTIL_CLAMP_MAX;
    printf("step2 sched_flags = 0x%02lx  (expect 0x78)\n", step2_flags);
    CHECK(step2_flags == 0x78, "step2 composition == 0x78");

    unsigned long restore_flags = SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS;
    printf("restore sched_flags = 0x%02lx  (expect 0x18)\n", restore_flags);
    CHECK(restore_flags == 0x18, "restore composition == 0x18");

    unsigned long probe_flags =
        SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN;
    printf("probe sched_flags = 0x%02lx  (expect 0x38)\n", probe_flags);
    CHECK(probe_flags == 0x38, "probe composition == 0x38");

    CHECK(sizeof(struct sched_attr) == 56, "sched_attr sizeof == 56");

    printf(fail ? "=== FAIL (%d) ===\n" : "=== PASS ===\n", fail);
    return fail ? 1 : 0;
}

// test_cmdq_v449.cpp - F02[P3] command-queue enqueue defense-in-depth regression.
//
// The queue dir is 0700 root-only and drain() already re-runs isValidControlChars,
// but before this fix enqueue() accepted any <=256-byte line and wrote it to the
// queue. We now apply the SAME character whitelist at enqueue time, so a line
// carrying shell metacharacters (which only the auth-gated HTTP path could ever
// have legitimately produced) is refused before it lands in the queue.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "aatpe_command_queue.h"

extern const char* RUN_DIR;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

int main() {
    printf("=== F02: enqueue applies the control-character whitelist ===\n");
    { std::string rm = std::string("rm -rf ") + RUN_DIR + "/commands"; system(rm.c_str()); }
    mkdir(RUN_DIR, 0755);

    // A well-formed control command must still be accepted.
    aatpe_cmdq::EnqueueResult ok = aatpe_cmdq::commandQueueEnqueue("set_log_level=3");
    CHECK(ok.ok, "valid control command enqueued");

    // Shell metacharacters / spaces must be REJECTED at enqueue time (F02).
    aatpe_cmdq::EnqueueResult bad1 = aatpe_cmdq::commandQueueEnqueue("set_mode=assist; reboot");
    CHECK(!bad1.ok, "line with ';' and space rejected at enqueue");

    aatpe_cmdq::EnqueueResult bad2 = aatpe_cmdq::commandQueueEnqueue("x=$(id)");
    CHECK(!bad2.ok, "line with $() rejected at enqueue");

    aatpe_cmdq::EnqueueResult bad3 = aatpe_cmdq::commandQueueEnqueue("a=b|c");
    CHECK(!bad3.ok, "line with '|' rejected at enqueue");

    aatpe_cmdq::EnqueueResult bad4 = aatpe_cmdq::commandQueueEnqueue("");
    CHECK(!bad4.ok, "empty line rejected");

    printf("=== RESULT: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

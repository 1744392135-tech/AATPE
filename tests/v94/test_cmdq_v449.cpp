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

    aatpe_cmdq::EnqueueResult ok = aatpe_cmdq::commandQueueEnqueue("set_log_level=3");
    CHECK(ok.ok, "valid control command enqueued");

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

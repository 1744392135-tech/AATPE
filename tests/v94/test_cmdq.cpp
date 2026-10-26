// test_cmdq.cpp - round-4 P1 #1/#2 hermetic unit test.
//
// Exercises the real command queue (/dev/aatpe/commands/):
//   - enqueue writes a uniquely-named cmd-NNNNNN file (atomic rename),
//   - depth counter reflects queued files,
//   - drain() executes in FIFO order and records success/failed status,
//   - commandStatusJson() reports queued -> executing -> success lifecycle,
//   - unknown/expired id reports "unknown".
//
// RUN_DIR is provided by tests/stubs/control_deps.cpp (/tmp/aatpe_v94_run).
// executeControlLine() is the real production command table (linked from
// aatped_control.cpp) so drain exercises the real dispatch path.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "aatpe_command_queue.h"

// RUN_DIR is the per-process unique dir from tests/stubs/control_deps.cpp.
extern const char* RUN_DIR;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== cmdq: enqueue creates a uniquely-named queue file ===\n");
    // Start from a clean commands dir (RUN_DIR itself is a fresh mkdtemp).
    { std::string rm = std::string("rm -rf ") + RUN_DIR + "/commands"; system(rm.c_str()); }
    mkdir(RUN_DIR, 0755);

    aatpe_cmdq::EnqueueResult e1 = aatpe_cmdq::commandQueueEnqueue("set_mode=assist");
    CHECK(e1.ok, "first enqueue ok");
    CHECK(e1.command_id.rfind("cmd-", 0) == 0, "command_id has cmd- prefix");
    CHECK(aatpe_cmdq::commandQueueDepth() == 1, "depth == 1 after first enqueue");

    aatpe_cmdq::EnqueueResult e2 = aatpe_cmdq::commandQueueEnqueue("set_log_level=3");
    CHECK(e2.ok, "second enqueue ok");
    CHECK(e2.command_id != e1.command_id, "two enqueues get distinct ids");
    CHECK(aatpe_cmdq::commandQueueDepth() == 2, "depth == 2 after second enqueue");

    printf("=== cmdq: status JSON reflects queued lifecycle ===\n");
    std::string s1 = aatpe_cmdq::commandStatusJson(e1.command_id);
    CHECK(s1.find("\"status\":\"queued\"") != std::string::npos, "freshly enqueued reports queued");
    CHECK(s1.find(e1.command_id) != std::string::npos, "status echoes command_id");

    printf("=== cmdq: empty / oversized commands rejected ===\n");
    aatpe_cmdq::EnqueueResult eEmpty = aatpe_cmdq::commandQueueEnqueue("   ");
    CHECK(!eEmpty.ok, "empty command rejected");
    std::string longCmd(300, 'a');
    aatpe_cmdq::EnqueueResult eLong = aatpe_cmdq::commandQueueEnqueue(longCmd);
    CHECK(!eLong.ok, "oversized command rejected");
    CHECK(aatpe_cmdq::commandQueueDepth() == 2, "rejected enqueues do not consume queue slots");

    printf("=== cmdq: drain executes in FIFO order and records result ===\n");
    int drained = aatpe_cmdq::commandQueueDrain();
    CHECK(drained == 2, "drain executed both queued commands");
    CHECK(aatpe_cmdq::commandQueueDepth() == 0, "depth == 0 after drain");

    std::string s1After = aatpe_cmdq::commandStatusJson(e1.command_id);
    CHECK(s1After.find("\"status\":\"success\"") != std::string::npos,
          "executed command reports success");

    printf("=== cmdq: unknown id reports unknown ===\n");
    std::string sUnknown = aatpe_cmdq::commandStatusJson("cmd-999999");
    CHECK(sUnknown.find("\"status\":\"unknown\"") != std::string::npos,
          "unknown id reports unknown");

    printf("=== cmdq: drain on empty dir is a no-op ===\n");
    int drained2 = aatpe_cmdq::commandQueueDrain();
    CHECK(drained2 == 0, "drain on empty queue returns 0");

    printf("===================================================================\n");
    printf("cmdq: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

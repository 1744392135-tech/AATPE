// test_cmdq_replaysafe.cpp — MON-11: crash-replay classification.
//
// After an unclean shutdown, leftover cmd-NNNNNN files in commands/ are re-drained
// on the next start. State-setting commands (set_*, migrate=, game_add=) are
// idempotent and safe to replay; one-shot triggers (restart, clear_log,
// manual_unlock_watchdog, shadow_dump=1, restored_after_crash_window) must NOT
// be replayed. This test exercises commandLineIsReplaySafe() directly.
#include <cstdio>
#include <string>
#include "aatpe_command_queue.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== cmdq_replaysafe: idempotent commands are replay-safe ===\n");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("set_mode=assist"),        "set_mode=assist replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("set_log_level=3"),       "set_log_level=3 replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("set_profile=gaming"),    "set_profile=gaming replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("migrate=1234"),          "migrate= replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("game_add=com.tencent.tmgp.sgame"), "game_add= replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("game_remove=com.x.y"),   "game_remove= replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("reload_config"),         "reload_config replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("model_save=1"),         "model_save= replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("model_reset=1"),         "model_reset= replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("explain_tid=1234"),      "explain_tid= replay-safe");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe(""),                     "empty is replay-safe (no-op)");

    printf("=== cmdq_replaysafe: one-shot triggers are NOT replay-safe ===\n");
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("restart"),               "'restart' NOT replay-safe");
    // P3-3: "stop" sets g_shouldExit; a crash-leftover "stop" would kill the
    // freshly-started daemon on next drain. Must be quarantined at startup.
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("stop"),                 "'stop' NOT replay-safe (P3-3)");
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("clear_log"),            "'clear_log' NOT replay-safe");
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("manual_unlock_watchdog"),
          "'manual_unlock_watchdog' NOT replay-safe");
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("restored_after_crash_window"),
          "'restored_after_crash_window' NOT replay-safe");
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("shadow_dump=1"),        "shadow_dump=1 NOT replay-safe");
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("shadow_dump"),           "bare shadow_dump NOT replay-safe");

    printf("=== cmdq_replaysafe: boundary conditions ===\n");
    // P3-3 boundary: exact match only. "stop_foo" / "stopped" / "stop=now"
    // must NOT be misclassified as the one-shot "stop" command.
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("stop_foo=1"),          "'stop_foo=1' does NOT collide with 'stop'");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("stopped"),              "'stopped' does NOT collide with 'stop'");
    // Trailing whitespace/newline should be trimmed before classification.
    CHECK(!aatpe_cmdq::commandLineIsReplaySafe("restart\n"),             "'restart\\n' trims then blocks");
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("set_mode=passive  "),   "trailing spaces trimmed, replay-safe");
    // Prefix false-positive: "shadow_dump_other" must NOT match shadow_dump.
    CHECK( aatpe_cmdq::commandLineIsReplaySafe("shadow_dump_other=1"),
           "shadow_dump_other=1 does NOT collide with shadow_dump");

    printf("\n========================================\n");
    printf("cmdq_replaysafe: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

// aatpe_command_queue.h - round-4 (P1 #1/#2)
//
// Real command queue for the WebUI / control surface. Replaces the single-file
// "mailbox" (control_cmd, atomic-rename handoff) with a directory queue:
//
//   /dev/aatpe/commands/cmd-000001
//   /dev/aatpe/commands/cmd-000002
//   ...
//
// Enqueue: write to a temp file, then atomic-rename it to cmd-NNNNNN. The daemon
// drains the directory in sorted (FIFO) order, executes each command, records the
// result, and unlinks the file. Because each command owns a unique filename, two
// concurrent producers can never overwrite each other, and the daemon never loses
// a command between read and delete.
//
// Backward compatibility: the legacy /dev/aatpe/control pipe file (used by
// aatpe-cli and direct root writers) and the old control_cmd mailbox are still
// drained by processControlCommands(). New WebUI commands go through this queue
// and get a command_id they can poll.
//
// Depth limit: kMaxQueueDepth (64). An enqueue beyond the limit fails with
// ERR_BUSY so a runaway client cannot exhaust /dev/aatpe.
#ifndef AATPE_COMMAND_QUEUE_H
#define AATPE_COMMAND_QUEUE_H

#include <string>

namespace aatpe_cmdq {

// Max queued commands. Enqueue beyond this returns ERR_BUSY.
static constexpr int kMaxQueueDepth = 64;

// Result of an enqueue attempt.
struct EnqueueResult {
    bool ok = false;
    std::string command_id;    // e.g. "cmd-000007" on success
    int error_code = 0;        // 0 = ok; aatpe::ERR_* on failure
    std::string message;
};

// Enqueue one validated command line. The line MUST already have passed the
// WebUI whitelist + HMAC/session auth at the HTTP boundary (this function does
// not re-authenticate; it is the trusted write side). Returns a command_id the
// client can pass to commandStatusJson().
//
// A2-007: NON-BLOCKING on fullness. The queue is bounded (kMaxQueueDepth); when
// saturated this returns immediately with ok=false + error_code=ERR_BUSY so the
// HTTP I/O thread can reply 503 instead of waiting for a free slot. tryPush() is
// the explicit form of the same contract.
EnqueueResult commandQueueEnqueue(const std::string& line);

// A2-007: explicit non-blocking try-push. Returns true iff the command was
// accepted; on saturation returns false immediately with out.error_code=ERR_BUSY
// (HTTP 503). Never blocks on I/O or waits on a condition variable.
bool tryPush(const std::string& line, EnqueueResult& out);

// Drain /dev/aatpe/commands/ in FIFO order. For each file: mark executing,
// execute through the production command table (executeControlLine), record the
// success/failure result, then unlink the file. Returns the number of commands
// executed. Called once per daemon loop iteration from processControlCommands().
int commandQueueDrain();

// JSON snapshot of a command's lifecycle for GET /api/command/<id>.
// States: queued | executing | success | failed | unknown.
// On success includes result.error_code / result.message / result.applied_value.
std::string commandStatusJson(const std::string& id);

// Diagnostic: current number of queued files on disk (best effort).
int commandQueueDepth();

// MON-11: classify a command line as safe to re-execute after a crash.
// Idempotent commands (set_*, migrate=, game_add=, model_save=, reload_config)
// can be replayed; one-shot triggers (restart, clear_log, manual_unlock_watchdog,
// shadow_dump=1, restored_after_crash_window) must not. Exposed for unit tests.
bool commandLineIsReplaySafe(const std::string& line);

} // namespace aatpe_cmdq

#endif // AATPE_COMMAND_QUEUE_H

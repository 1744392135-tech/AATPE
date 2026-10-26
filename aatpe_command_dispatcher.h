// AATPE - Shared command dispatcher (#100, partial)
// Single entry point for CLI / WebUI / control-pipe commands.
// Pipeline: parse -> authorize -> validate -> dispatch.
// For round-3, the control pipe path is wired through this dispatcher;
// CLI and WebUI keep their existing entry points but call into it.
#ifndef AATPE_COMMAND_DISPATCHER_H
#define AATPE_COMMAND_DISPATCHER_H

#include <string>

namespace aatpe {

enum class DispatchAuth {
    NONE,       // no auth needed (status, version)
    ROOT,       // requires root / daemon token
    HMAC,       // requires HMAC signature
};

// unified machine-readable error codes shared by the
// CLI, WebUI, and control-pipe paths. executeControlLine() fills ControlResult
// .error_code with one of these; the dispatcher maps them to HTTP status.
// Kept as plain enum (not enum class) so existing ControlResult int fields
// continue to compare without casts.
enum DispatchErrorCode {
    ERR_OK                 = 0,
    ERR_UNKNOWN_COMMAND    = 1,   // 404
    ERR_INVALID_ARGUMENT   = 2,   // 400
    ERR_UNAUTHORIZED       = 3,   // 401
    ERR_FORBIDDEN          = 4,   // 403
    ERR_BUSY               = 5,   // 429
    ERR_PROFILE_INVALID    = 6,   // 400 (unknown profile name)
    ERR_CONFIG_INVALID     = 7,   // 400 (rejected config value, e.g. NaN/Inf)
    ERR_APPLY_FAILED       = 8,   // 500 (syscall/apply returned error)
    ERR_NOT_SUPPORTED      = 9,   // 501 (kernel/feature missing)
};

struct DispatchResult {
    int httpStatus;     // 200, 400, 401, 403, 500
    std::string body;   // JSON or text response
};

class CommandDispatcher {
public:
    static CommandDispatcher& instance();

    // Parse a raw command string (e.g. "set log_level=3" or "migrate 1234 4").
    // Returns a DispatchResult. For now, delegates to the existing processControlCommands
    // logic but provides a single choke point for authorization/validation.
    DispatchResult dispatch(const std::string& rawCommand, DispatchAuth authLevel);

private:
    CommandDispatcher() = default;
    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;
};

} // namespace aatpe

#endif // AATPE_COMMAND_DISPATCHER_H

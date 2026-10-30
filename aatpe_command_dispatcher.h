#ifndef AATPE_COMMAND_DISPATCHER_H
#define AATPE_COMMAND_DISPATCHER_H

#include <string>

namespace aatpe {

enum class DispatchAuth {
    NONE,
    ROOT,
    HMAC,
};

enum DispatchErrorCode {
    ERR_OK                 = 0,
    ERR_UNKNOWN_COMMAND    = 1,
    ERR_INVALID_ARGUMENT   = 2,
    ERR_UNAUTHORIZED       = 3,
    ERR_FORBIDDEN          = 4,
    ERR_BUSY               = 5,
    ERR_PROFILE_INVALID    = 6,
    ERR_CONFIG_INVALID     = 7,
    ERR_APPLY_FAILED       = 8,
    ERR_NOT_SUPPORTED      = 9,
};

struct DispatchResult {
    int httpStatus;
    std::string body;
};

class CommandDispatcher {
public:
    static CommandDispatcher& instance();

    DispatchResult dispatch(const std::string& rawCommand, DispatchAuth authLevel);

private:
    CommandDispatcher() = default;
    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;
};

}

#endif

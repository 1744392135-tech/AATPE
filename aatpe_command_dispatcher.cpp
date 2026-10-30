#include "aatped.h"
#include "aatpe_command_dispatcher.h"
#include "aatped_control.h"
#include "webui_http.h"
#include <cstring>
#include <ctime>

namespace aatpe {

static std::string dispJsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char uc = (unsigned char)c;
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (uc < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", uc);
            out += buf;
        } else out += c;
    }
    return out;
}

CommandDispatcher& CommandDispatcher::instance() {
    static CommandDispatcher inst;
    return inst;
}

DispatchResult CommandDispatcher::dispatch(const std::string& rawCommand,
                                            DispatchAuth authLevel) {
    DispatchResult r;
    r.httpStatus = 200;

    std::string cmd = rawCommand;
    while (!cmd.empty() && (cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t'))
        cmd.pop_back();
    while (!cmd.empty() && (cmd.front()==' '||cmd.front()=='\t'))
        cmd.erase(cmd.begin());

    if (cmd.empty()) {
        r.httpStatus = 400;
        r.body = "{\"ok\":false,\"error\":\"empty command\"}";
        return r;
    }

    if (cmd.size() > 256) {
        r.httpStatus = 400;
        r.body = "{\"ok\":false,\"error\":\"command too long\"}";
        return r;
    }

    if (!isValidControlChars(cmd)) {
        r.httpStatus = 400;
        r.body = "{\"ok\":false,\"error\":\"invalid control characters\"}";
        return r;
    }

    if (authLevel == DispatchAuth::NONE) {
        static const char* kReadOnly[] = {
            "status", "explain", "version", "help", "ping", nullptr
        };
        bool readOnly = false;
        for (int i = 0; kReadOnly[i] && !readOnly; i++) {
            const char* e = kReadOnly[i];
            size_t elen = strlen(e);
            if (cmd.size() < elen) continue;
            if (cmd.compare(0, elen, e) != 0) continue;
            if (cmd.size() == elen) { readOnly = true; break; }
            char boundary = cmd[elen];
            if (boundary == '=' || boundary == ' ' || boundary == '\t') {
                readOnly = true; break;
            }
        }
        if (!readOnly) {
            r.httpStatus = 401;
            r.body = "{\"ok\":false,\"error\":\"unauthorized: privileged command requires root or HMAC\"}";
            return r;
        }
    }

    ControlResult cr = executeControlLine(cmd);

    LOG_INFO("[DISPATCH] auth=%d cmd='%s' ok=%d err=%d", (int)authLevel, cmd.c_str(),
             cr.success ? 1 : 0, cr.error_code);

    {
        const char* src = (authLevel == DispatchAuth::HMAC) ? "webui" : "cli";
        std::string reason;
        if (!cr.success) reason = cr.message.empty() ? "rejected" : cr.message;
        aatpe::webui_http::auditSensitiveCommand(cmd, src, cr.success, reason);
    }

    if (cr.success) {
        r.httpStatus = 200;
        r.body = "{\"ok\":true,\"cmd\":\"" + dispJsonEscape(cmd) + "\"}";
    } else {
        switch (cr.error_code) {
            case ERR_UNKNOWN_COMMAND:  r.httpStatus = 404; break;
            case ERR_UNAUTHORIZED:      r.httpStatus = 401; break;
            case ERR_FORBIDDEN:         r.httpStatus = 403; break;
            case ERR_BUSY:              r.httpStatus = 503; break;
            case ERR_NOT_SUPPORTED:     r.httpStatus = 501; break;
            case ERR_APPLY_FAILED:      r.httpStatus = 500; break;
            case ERR_INVALID_ARGUMENT:
            case ERR_PROFILE_INVALID:
            case ERR_CONFIG_INVALID:
            default:                    r.httpStatus = 400; break;
        }
        std::string msg = cr.message.empty() ? "command rejected" : cr.message;
        r.body = "{\"ok\":false,\"cmd\":\"" + dispJsonEscape(cmd) + "\",\"error_code\":"
               + std::to_string(cr.error_code)
               + ",\"error\":\"" + dispJsonEscape(msg) + "\"}";
    }
    return r;
}

}

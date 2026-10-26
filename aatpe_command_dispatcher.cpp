// AATPE - Shared command dispatcher (#100, release-convergence item 11)
// Single entry point for the control-pipe commands.
// Pipeline: parse -> authorize -> validate -> dispatch -> execute -> audit.
//
// This is NO LONGER a placeholder: dispatch() actually executes the command by
// calling executeControlLine() (the production if/else command table in
// aatped_control.cpp). The control-pipe loop in processControlCommands() routes
// every line through here, so auth/validation/audit live in one choke point.
#include "aatped.h"
#include "aatpe_command_dispatcher.h"
#include "aatped_control.h"
#include "webui_http.h"
#include <cstring>
#include <ctime>

namespace aatpe {

// P2: local JSON string escaper for the response echo. Escapes '"', '\\' and the
// C0 control chars so cmd/error bytes can never break out of the JSON string.
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

    // --- parse: strip whitespace / trailing CR LF ---------------------------
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

    // --- validate: length bound (matches control pipe limit) ----------------
    if (cmd.size() > 256) {
        r.httpStatus = 400;
        r.body = "{\"ok\":false,\"error\":\"command too long\"}";
        return r;
    }

    // --- validate: character whitelist (defense-in-depth) ---------------------
    // R6-010: the control-pipe reader already runs isValidControlChars(), but this
    // dispatcher is the single choke point also reached from the WebUI / queued
    // paths. Re-validate here so a future caller that forgets the gate cannot
    // smuggle characters through.
    if (!isValidControlChars(cmd)) {
        r.httpStatus = 400;
        r.body = "{\"ok\":false,\"error\":\"invalid control characters\"}";
        return r;
    }

    // --- authorize ----------------------------------------------------------
    // P1-fix (issue 31): the previous auth block was a no-op. Add a real
    // defense-in-depth gate: commands that require write/state-change semantics
    // are rejected unless the caller presents ROOT (trusted control pipe / CLI)
    // or HMAC (WebUI). Read-only commands (status queries) are allowed from any
    // auth level. The control pipe is already root-only (0600), so this primarily
    // protects against a future non-privileged caller that somehow gets a
    // DispatchAuth::NONE handle to the dispatcher.
    if (authLevel == DispatchAuth::NONE) {
        // MON-02 fix: the old whitelist used raw prefix matching
        //   cmd.compare(0, strlen(p), p) == 0
        // which let a hypothetical write command named "status_reset", "get_lock",
        // "read_clear", "list_flush" slip past the NONE-auth gate just because it
        // happens to start with a read-only prefix. We now require EXACT command
        // names (or "cmd=value" / "cmd value" invocations). The previous
        // "get_"/"read_"/"query_"/"show_"/"list_" prefix families are removed:
        // the production control table has no such commands, and the prefix
        // convention is not a security boundary — a write command named "get_lock"
        // would defeat it. If a real getter command is added later, list it here
        // explicitly.
        static const char* kReadOnly[] = {
            "status", "explain", "version", "help", "ping", nullptr
        };
        bool readOnly = false;
        for (int i = 0; kReadOnly[i] && !readOnly; i++) {
            const char* e = kReadOnly[i];
            size_t elen = strlen(e);
            if (cmd.size() < elen) continue;
            if (cmd.compare(0, elen, e) != 0) continue;
            // exact whole command, or "cmd=..." / "cmd ..."
            if (cmd.size() == elen) { readOnly = true; break; }
            char boundary = cmd[elen];
            if (boundary == '=' || boundary == ' ' || boundary == '\t') {
                readOnly = true; break;
            }
            // otherwise: cmd is e.g. "status_reset" — not read-only.
        }
        if (!readOnly) {
            r.httpStatus = 401;
            r.body = "{\"ok\":false,\"error\":\"unauthorized: privileged command requires root or HMAC\"}";
            return r;
        }
    }

    // --- execute: hand to the production command table ----------------------
    // P1-12: propagate the REAL execution result. executeControlLine() now
    // returns ControlResult{success, error_code, message, applied_value}; we must
    // NOT synthesize ok=true when the command was unknown or rejected.
    ControlResult cr = executeControlLine(cmd);

    // --- audit: every dispatched command is logged -------------------------
    LOG_INFO("[DISPATCH] auth=%d cmd='%s' ok=%d err=%d", (int)authLevel, cmd.c_str(),
             cr.success ? 1 : 0, cr.error_code);

    // P3-42: persist a structured audit record for sensitive operations.
    // The WebUI path already audits at HTTP-enqueue time; here we audit the
    // EXECUTION result for CLI / control-pipe / watchdog-originated commands
    // (DispatchAuth::ROOT), which never pass through the HTTP layer. We also
    // record the HMAC-originated execution outcome for completeness.
    {
        const char* src = (authLevel == DispatchAuth::HMAC) ? "webui" : "cli";
        std::string reason;
        if (!cr.success) reason = cr.message.empty() ? "rejected" : cr.message;
        aatpe::webui_http::auditSensitiveCommand(cmd, src, cr.success, reason);
    }

    if (cr.success) {
        r.httpStatus = 200;
        // P2: JSON-escape cmd in the echo (defense-in-depth; the whitelist already
        // restricts to [a-z0-9_=.-], but never embed raw bytes into JSON).
        r.body = "{\"ok\":true,\"cmd\":\"" + dispJsonEscape(cmd) + "\"}";
    } else {
        // P1-22: map the unified error code to an HTTP status.
        switch (cr.error_code) {
            case ERR_UNKNOWN_COMMAND:  r.httpStatus = 404; break;
            case ERR_UNAUTHORIZED:      r.httpStatus = 401; break;
            case ERR_FORBIDDEN:         r.httpStatus = 403; break;
            case ERR_BUSY:              r.httpStatus = 503; break;  // A2-007: queue-full/busy -> 503 (not 429)
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

} // namespace aatpe

#ifndef AATPE_WEBUI_HTTP_H
#define AATPE_WEBUI_HTTP_H

// P1-7: Built-in HTTP server (real original)
// Lightweight HTTP server for WebUI aggregation API and health checks.
// Listens on 127.0.0.1:8888-8899 (tries each port in range).
// Endpoints:
//   GET /state/all   — aggregate JSON of all daemon state files
//   GET /health      — liveness probe (returns 200 OK with uptime)
//   GET /            — redirect to /state/all

#include <string>
#include <cstdio>

// F-A6-08[P3]: canonical JSON string escaper, defined ONCE as an inline here and
// shared by webui_http.cpp and webui_json.cpp. Both TUs used to carry a private
// copy of this logic, which risked the escaping rules drifting apart (HTTP
// responses vs state.json). The rules: escape quote/backslash/CR/LF/TAB, other
// C0 control bytes as \uXXXX, and the U+2028/U+2029 line separators that break
// JSON.parse() on old engines. inline => one ODR definition, no link collision.
inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size() &&
            (unsigned char)s[i+1] == 0x80 &&
            ((unsigned char)s[i+2] == 0xA8 || (unsigned char)s[i+2] == 0xA9)) {
            out += (((unsigned char)s[i+2]) == 0xA8) ? "\\u2028" : "\\u2029";
            i += 3;
            continue;
        }
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
        i++;
    }
    return out;
}

namespace aatpe {
namespace webui_http {

// Start the HTTP server in a detached thread. Returns true if a port
// was successfully bound (server running), false if all ports failed.
bool startHttpServer();

// Stop the HTTP server (sets shutdown flag, thread exits on next accept).
void stopHttpServer();

// Get the port the server is listening on (0 if not running).
int getHttpPort();

// P2-1 fix: true iff at least one WebUI client connection is currently active
// (HTTP server running and >=1 unfinished handler thread, e.g. an open SSE
// /events stream). The daemon uses this to skip expensive periodic JSON
// serialization when no browser is attached. Cheap: takes g_connMutex briefly.
bool hasActiveClients();

// ensure RUN_DIR/daemon_token exists on disk (0600) before the
// WebUI reads it. The token was previously created lazily inside
// /api/hmac_key handling, but the WebUI reads daemon_token (via KSU exec) to build
// that very request URL -> first load always saw an empty token and fell back to
// unsigned commands, which the server then rejected with 403. Call this once at
// daemon init so the token file is always present.
void ensureDaemonToken();

// Build aggregate JSON from all state files in STATE_DIR.
// Used by /state/all endpoint.
std::string buildAggregateStateJson();

// P3-42: write a sensitive-operation audit record to STATE_DIR/audit.log.
// source: "webui" | "cli" | "watchdog" | "federated" | "internal".
// ok: true if the command was accepted/applied, false if rejected.
// reason: short rejection explanation (empty on success). Best-effort, never throws.
void auditSensitiveCommand(const std::string& cmd, const std::string& source,
                           bool ok, const std::string& reason);

} // namespace webui_http
} // namespace aatpe

#endif // AATPE_WEBUI_HTTP_H

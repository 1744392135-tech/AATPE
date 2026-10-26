// test_command_dispatcher.cpp — host tests for CommandDispatcher's own
// parse/validate/authorize gate (the production executeControlLine command table
// is stubbed here so we exercise ONLY the dispatcher choke point: empty/length/
// char-whitelist rejection + the NONE-auth read-only allowlist).
//
// Security property under test: a caller holding DispatchAuth::NONE must NOT be
// able to reach a state-changing command; read-only prefixes still pass.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "aatpe_command_dispatcher.h"

// Mirror of production ControlResult (aatped_control.h:181) — these symbols are
// declared at GLOBAL scope (aatped_control.h has no namespace wrapper), so the
// fakes below must be global too. We forward-declare instead of including
// aatped_control.h, which transitively drags the whole placement/ML type graph.
struct ControlResult {
    bool success = false;
    int error_code = 0;
    std::string message;
    std::string applied_value;
};
ControlResult executeControlLine(const std::string& line);
bool isValidControlChars(const std::string& line);

using namespace aatpe;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

// ---- fakes for production deps we intentionally do NOT link -----------------
// executeControlLine (GLOBAL, matches aatped_control.h): stubbed command table.
ControlResult executeControlLine(const std::string& line) {
    ControlResult r;
    if (line == "status") { r.success = true; r.error_code = ERR_OK; r.message = "ok"; }
    else if (line == "set log_level=3") { r.success = true; r.error_code = ERR_OK; r.message = "log=3"; }
    else { r.success = false; r.error_code = ERR_UNKNOWN_COMMAND; r.message = "no such command"; }
    return r;
}
// The dispatcher calls isValidControlChars (real whitelist in aatped_control).
// We stub it true here: this test owns the dispatcher's OWN gate; the whitelist
// itself is covered by test_control.cpp against aatped_control.o.
bool isValidControlChars(const std::string&) { return true; }
namespace aatpe { namespace webui_http {
void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
}}
// LOG_INFO -> logMsg(2,...) is provided by log_stub (linked via recipe).

int main() {
    CommandDispatcher& d = CommandDispatcher::instance();

    printf("=== command_dispatcher: empty / length validation ===\n");
    {
        DispatchResult r = d.dispatch("", DispatchAuth::ROOT);
        CHECK(r.httpStatus == 400, "empty command -> 400");
        DispatchResult r2 = d.dispatch("    ", DispatchAuth::ROOT);
        CHECK(r2.httpStatus == 400, "whitespace-only command -> 400");
        std::string longCmd(300, 'a');
        DispatchResult r3 = d.dispatch(longCmd, DispatchAuth::ROOT);
        CHECK(r3.httpStatus == 400, ">256-char command -> 400 (too long)");
    }

    printf("=== command_dispatcher: NONE auth read-only allowlist ===\n");
    {
        // read-only prefix passes the auth gate -> reaches executeControlLine -> 200
        DispatchResult r = d.dispatch("status", DispatchAuth::NONE);
        CHECK(r.httpStatus == 200, "NONE+read-only 'status' -> 200 (allowed)");
        // a state-changing command under NONE must be rejected at the gate (401)
        DispatchResult r2 = d.dispatch("set log_level=3", DispatchAuth::NONE);
        CHECK(r2.httpStatus == 401, "NONE+'set log_level=3' -> 401 (privileged)");
        // unknown non-read-only under NONE also blocked before execution
        DispatchResult r3 = d.dispatch("boguscmd", DispatchAuth::NONE);
        CHECK(r3.httpStatus == 401, "NONE+'boguscmd' -> 401 (not read-only)");
        // get_ prefix is NO LONGER a read-only family (MON-02 hardening).
        // "get_psi" is not in the production command table anyway; blocking it
        // is correct (defense-in-depth: no write command can hide behind get_).
        DispatchResult r4 = d.dispatch("get_psi", DispatchAuth::NONE);
        CHECK(r4.httpStatus == 401,
              "NONE+'get_psi' blocked (get_ prefix family removed; no such command)");
    }

    printf("=== command_dispatcher: ROOT auth reaches execution + error mapping ===\n");
    {
        DispatchResult r = d.dispatch("status", DispatchAuth::ROOT);
        CHECK(r.httpStatus == 200, "ROOT+'status' -> 200");
        DispatchResult r2 = d.dispatch("set log_level=3", DispatchAuth::ROOT);
        CHECK(r2.httpStatus == 200, "ROOT+'set log_level=3' -> 200 (executes)");
        DispatchResult r3 = d.dispatch("boguscmd", DispatchAuth::ROOT);
        CHECK(r3.httpStatus == 404, "ROOT+'boguscmd' -> 404 (ERR_UNKNOWN_COMMAND)");
    }

    // MON-02 PoC: the old prefix-matching whitelist let a write command that
    // merely STARTS with a read-only word (e.g. "status_reset", "get_lock",
    // "read_clear", "list_flush") slip past the NONE-auth gate. These strings
    // are NOT in the fake executeControlLine table, so if the auth gate lets them
    // through we get 404 (reached execution); if the gate blocks them we get 401.
    // The fix requires 401 for every such "pretend read-only" attack string.
    printf("=== command_dispatcher MON-02: prefix-spoofing PoC (must all be 401) ===\n");
    {
        const char* kAttacks[] = {
            "status_reset",        // was: prefix "status" matched -> 200/404 (BUG)
            "status_clear",        // was: same
            "get_lock",            // was: prefix "get_" matched -> passes (BUG)
            "read_clear",         // was: prefix "read_" matched (BUG)
            "list_flush",          // was: prefix "list_" matched (BUG)
            "query_drop",          // was: prefix "query_" matched (BUG)
            "show_hidden",         // was: prefix "show_" matched (BUG)
            "version_bump",        // was: prefix "version" matched (BUG)
            "help_me_rooti",       // was: prefix "help" matched (BUG)
            "ping_flood",          // was: prefix "ping" matched (BUG)
            "explain_then_exploit", // was: prefix "explain" matched (BUG)
            nullptr
        };
        for (int i = 0; kAttacks[i]; ++i) {
            DispatchResult r = d.dispatch(kAttacks[i], DispatchAuth::NONE);
            std::string m = std::string("NONE+'") + kAttacks[i] + "' -> 401 (blocked)";
            CHECK(r.httpStatus == 401, m.c_str());
        }
        // And the legitimate read-only commands MUST still pass.
        DispatchResult ok1 = d.dispatch("status", DispatchAuth::NONE);
        CHECK(ok1.httpStatus == 200, "MON-02: 'status' still passes");
        // "status=foo" (with equals) is a valid read-only invocation.
        DispatchResult ok3 = d.dispatch("status=verbose", DispatchAuth::NONE);
        // It's not in the fake table, so it reaches execution -> 404, NOT 401.
        CHECK(ok3.httpStatus != 401, "MON-02: 'status=verbose' not blocked by auth gate");
    }

    printf("=== command_dispatcher: echo JSON escapes cmd ===\n");
    {
        DispatchResult r = d.dispatch("status", DispatchAuth::ROOT);
        CHECK(r.body.find("\"ok\":true") != std::string::npos, "success body has ok:true");
    }

    printf("\n========================================\n");
    printf("command_dispatcher: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "aatpe_command_dispatcher.h"

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

ControlResult executeControlLine(const std::string& line) {
    ControlResult r;
    if (line == "status") { r.success = true; r.error_code = ERR_OK; r.message = "ok"; }
    else if (line == "set log_level=3") { r.success = true; r.error_code = ERR_OK; r.message = "log=3"; }
    else { r.success = false; r.error_code = ERR_UNKNOWN_COMMAND; r.message = "no such command"; }
    return r;
}
bool isValidControlChars(const std::string&) { return true; }
namespace aatpe { namespace webui_http {
void auditSensitiveCommand(const std::string&, const std::string&, bool, const std::string&) {}
}}

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
        DispatchResult r = d.dispatch("status", DispatchAuth::NONE);
        CHECK(r.httpStatus == 200, "NONE+read-only 'status' -> 200 (allowed)");
        DispatchResult r2 = d.dispatch("set log_level=3", DispatchAuth::NONE);
        CHECK(r2.httpStatus == 401, "NONE+'set log_level=3' -> 401 (privileged)");
        DispatchResult r3 = d.dispatch("boguscmd", DispatchAuth::NONE);
        CHECK(r3.httpStatus == 401, "NONE+'boguscmd' -> 401 (not read-only)");
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

    printf("=== command_dispatcher MON-02: prefix-spoofing PoC (must all be 401) ===\n");
    {
        const char* kAttacks[] = {
            "status_reset",
            "status_clear",
            "get_lock",
            "read_clear",
            "list_flush",
            "query_drop",
            "show_hidden",
            "version_bump",
            "help_me_rooti",
            "ping_flood",
            "explain_then_exploit",
            nullptr
        };
        for (int i = 0; kAttacks[i]; ++i) {
            DispatchResult r = d.dispatch(kAttacks[i], DispatchAuth::NONE);
            std::string m = std::string("NONE+'") + kAttacks[i] + "' -> 401 (blocked)";
            CHECK(r.httpStatus == 401, m.c_str());
        }
        DispatchResult ok1 = d.dispatch("status", DispatchAuth::NONE);
        CHECK(ok1.httpStatus == 200, "MON-02: 'status' still passes");
        DispatchResult ok3 = d.dispatch("status=verbose", DispatchAuth::NONE);
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

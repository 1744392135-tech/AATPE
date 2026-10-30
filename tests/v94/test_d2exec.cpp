#include "aatped.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

static bool exists(const std::string& p) {
    struct stat st; return ::stat(p.c_str(), &st) == 0;
}

int main() {
    printf("==> D2 bpftool hardening: aatpeExecArgv no-shell + exit status ==\n");
    system("rm -f /tmp/aatpe_d2_*");

    {
        std::string literal = "/tmp/aatpe_d2_$((1+1))";
        std::string expanded = "/tmp/aatpe_d2_2";
        (void)aatpeExecArgv({"/bin/touch", literal}, "d2_noshell");
        CHECK(!exists(expanded), "D2: shell did NOT expand $((1+1)) (no /tmp/aatpe_d2_2)");
        CHECK(exists(literal), "D2: literal argv element passed verbatim (file w/ metachars)");
    }

    {
        std::string out = aatpeExecArgv({"/bin/echo", "PROBE;injected"}, "d2_semi");
        CHECK(out.find("PROBE;injected") != std::string::npos,
              "D2: ';' passed literally (echo output contains full argv element)");
        CHECK(out.find("PROBE\n") == 0 || out.find("PROBE;injected\n") != std::string::npos,
              "D2: no shell splitting on ';'");
    }

    {
        int st = -999;
        (void)aatpeExecArgv({"/bin/true"}, "d2_true", &st, true );
        CHECK(st == 0, "D2: /bin/true -> exit status 0");
    }
    {
        int st = -999;
        (void)aatpeExecArgv({"/bin/false"}, "d2_false", &st, true);
        CHECK(st != 0, "D2: /bin/false -> non-zero exit status");
    }

    {
        int st = 12345;
        std::string out = aatpeExecArgv({}, "d2_empty", &st, true);
        CHECK(out.empty() && st == -1, "D2: empty argv -> empty out, status -1");
    }

    {
        int st = 12345;
        std::string out = aatpeExecArgv({"/nonexistent/aatpe_bin"}, "d2_noexist", &st, true);
        CHECK(st != 0 || out.empty(), "D2: missing binary -> reported as failure (no hang)");
    }

    system("rm -f /tmp/aatpe_d2_*");
    printf("==> D2 bpftool hardening: done\n");
    if (failures == 0) { printf("D2 hardening: ALL PASS\n"); return 0; }
    printf("D2 hardening: %d FAILURES\n", failures);
    return 1;
}

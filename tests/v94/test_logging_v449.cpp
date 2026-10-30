#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include "aatped.h"
#include "aatped_logging.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while (0)

int main() {
    mkdir("/tmp/aatpe_v94_state", 0755);
    remove(LOG_FILE);
    logReinit();
    g_logLevel.store(3);

    logMsg(0, "injection probe ESC=\x1b]0;evil\x07 DEL=\x7f tail");
    logFlush();
    logShutdown();

    std::ifstream lf(LOG_FILE);
    std::string contents((std::istreambuf_iterator<char>(lf)),
                         std::istreambuf_iterator<char>());

    CHECK(contents.find("\x1b") == std::string::npos, "ESC (0x1b) stripped from log output");
    CHECK(contents.find("\x7f") == std::string::npos, "DEL (0x7f) stripped from log output");
    CHECK(contents.find("injection probe") != std::string::npos, "visible log text preserved");

    printf("=== RESULT: pass=%d fail=%d ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

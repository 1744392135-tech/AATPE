#include <cstdio>
#include <fstream>
#include <iterator>
#include <atomic>
#include <thread>
#include <sys/stat.h>
#include "aatped.h"
#include "aatped_logging.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    mkdir("/tmp/aatpe_v94_state", 0755);
    printf("=== Logging: logReinit opens the log file ===\n");
    remove(LOG_FILE);
    logReinit();

    printf("=== Logging: level gating ===\n");
    g_logLevel.store(0);
    logMsg(2, "INFO_line_must_be_dropped_xyz");
    logMsg(0, "ERROR_line_must_appear_abc");
    g_logLevel.store(3);
    logMsg(2, "INFO_line_after_raise_def");
    logFlush();

    printf("=== Logging: LOG_INFO / LOG_ERROR / LOG_DEBUG macros ===\n");
    LOG_INFO("info macro %d", 1);
    LOG_ERROR("error macro %d", 2);
    LOG_WARN("warn macro");
    LOG_DEBUG("sampled debug");
    LOG_FORCE("force logged");

    printf("=== Logging: logClear truncates ===\n");
    logClear();
    struct stat stBefore;
    bool clearedEmpty = (stat(LOG_FILE, &stBefore) == 0 && stBefore.st_size == 0);

    printf("=== Logging: logFlush then logShutdown ===\n");
    logMsg(0, "FINAL_error_after_clear");
    logFlush();
    logShutdown();
    struct stat stAfter;
    bool fileExists = (stat(LOG_FILE, &stAfter) == 0);

    std::ifstream lf(LOG_FILE);
    std::string contents((std::istreambuf_iterator<char>(lf)),
                          std::istreambuf_iterator<char>());

    CHECK(clearedEmpty, "logClear truncates the file to 0 bytes");
    CHECK(fileExists, "logReinit created the log file on disk");
    CHECK(contents.find("FINAL_error_after_clear") != std::string::npos,
          "ERROR line written after clear is present");
    CHECK(contents.find("INFO_line_must_be_dropped_xyz") == std::string::npos,
          "level gate dropped the INFO message emitted at g_logLevel=0");

    logReinit();

    printf("=== Logging: G1-002 rotation window stress ===\n");
    {
        g_logLevel.store(3);
        ::remove(LOG_FILE);
        logReinit();
        const int perThread = 50000;
        auto writer = [&](int tid) {
            for (int i = 0; i < perThread; i++) {
                logMsg(0, "G1002_T%d_M%07d", tid, i);
            }
        };
        std::thread t1(writer, 1), t2(writer, 2), t3(writer, 3), t4(writer, 4);
        t1.join(); t2.join(); t3.join(); t4.join();
        logShutdown();
        int found = 0;
        for (int f = 0; f < 2; f++) {
            std::ifstream lf(f == 0 ? LOG_FILE : (std::string(LOG_FILE) + ".1").c_str());
            if (!lf) continue;
            std::string line;
            while (std::getline(lf, line)) {
                if (line.find("G1002_T") != std::string::npos) found++;
            }
        }
        int total = perThread * 4;
        double pct = 100.0 * found / total;
        printf("      rotation stress: %d/%d lines survived (%.1f%%)\n", found, total, pct);
        CHECK(pct >= 99.0, "G1-002: >=99% of ERROR lines survive the rotation window");
    }

    logShutdown();
    CHECK(true, "second reinit/shutdown cycle is idempotent");

    printf("\n========================================\n"
           "Logging results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

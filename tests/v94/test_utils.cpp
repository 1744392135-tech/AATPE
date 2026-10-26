// test_utils.cpp - v94 hermetic unit tests for aatped_utils.cpp
// Links aatped_utils.cpp + utils_deps + log_stub (see build.sh recipe).
// Covers: readFile trim, readInt (valid/missing/garbage), writeFile overloads,
// writeFileAtomic/atomicWriteFile round-trip, trim, parseCpusetCpuList ranges,
// isCpuAllowed, readFirstLine, getOnlineCpus fallback, aatpeSdk detection.
#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include "aatped.h"
#include "aatpe_helper_audit.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    printf("=== Utils: readFile trim ===\n");
    FILE* fp = fopen("/tmp/aatpe_util_rw.txt", "w"); fputs("  hello  \n", fp); fclose(fp);
    CHECK(readFile("/tmp/aatpe_util_rw.txt") == "hello", "readFile trims whitespace");
    CHECK(readFile("/tmp/aatpe_nonexistent") == "", "missing file -> empty");

    printf("=== Utils: readInt ===\n");
    CHECK(readInt("/tmp/aatpe_util_rw.txt", -7) == -7, "non-numeric content -> default");
    FILE* f2 = fopen("/tmp/aatpe_util_int.txt", "w"); fputs("42", f2); fclose(f2);
    CHECK(readInt("/tmp/aatpe_util_int.txt", 0) == 42, "readInt parses 42");
    CHECK(readInt("/tmp/aatpe_nonexistent", 99) == 99, "missing file -> default");

    printf("=== Utils: writeFile overloads + read back ===\n");
    CHECK(writeFile("/tmp/aatpe_wf.txt", std::string("abc")), "writeFile string ok");
    CHECK(readFile("/tmp/aatpe_wf.txt") == "abc", "string round-trip");
    writeFile("/tmp/aatpe_wfi.txt", 7);
    CHECK(readInt("/tmp/aatpe_wfi.txt", 0) == 7, "writeFile int round-trip");

    printf("=== Utils: writeFileAtomic / atomicWriteFile ===\n");
    CHECK(writeFileAtomic("/tmp/aatpe_atomic.txt", std::string("data")), "writeFileAtomic ok");
    CHECK(readFile("/tmp/aatpe_atomic.txt") == "data", "atomic round-trip");
    CHECK(atomicWriteFile("/tmp/aatpe_atomic2.txt", 123), "atomicWriteFile int ok");
    CHECK(readInt("/tmp/aatpe_atomic2.txt", 0) == 123, "atomic int round-trip");

    printf("=== Utils: trim ===\n");
    std::string s = "  xy z  ";
    trim(s);
    CHECK(s == "xy z", "trim removes outer space only");
    std::string empty = "   ";
    trim(empty);
    CHECK(empty.empty(), "all-space -> empty");

    printf("=== Utils: parseCpusetCpuList ===\n");
    cpu_set_t m = parseCpusetCpuList("0-2,7");
    CHECK(CPU_ISSET(0, &m) && CPU_ISSET(2, &m) && CPU_ISSET(7, &m), "range + single parsed");
    CHECK(!CPU_ISSET(3, &m), "cpu 3 not in set");
    cpu_set_t m2 = parseCpusetCpuList("");
    CHECK(CPU_COUNT(&m2) == 0, "empty string -> empty mask");
    cpu_set_t m3 = parseCpusetCpuList("4");
    CHECK(CPU_ISSET(4, &m3) && CPU_COUNT(&m3) == 1, "single cpu parsed");

    printf("=== Utils: isCpuAllowed ===\n");
    CHECK(isCpuAllowed(1, m) == true, "cpu1 in mask -> allowed");
    CHECK(isCpuAllowed(3, m) == false, "cpu3 not in mask -> not allowed");
    CHECK(isCpuAllowed(-1, m) == false, "negative cpu -> not allowed");

    printf("=== Utils: readFirstLine ===\n");
    FILE* f3 = fopen("/tmp/aatpe_line.txt", "w"); fputs("  first  \nsecond\n", f3); fclose(f3);
    CHECK(readFirstLine("/tmp/aatpe_line.txt") == "first", "readFirstLine trims first line");

    printf("=== Utils: getOnlineCpus fallback (stub returns all 8) ===\n");
    std::vector<int> online = getOnlineCpus();
    CHECK(online.size() == 8, "host getOnlineCpus returns 8 cpus");

    printf("=== Utils: android version detection (host stubs) ===\n");
    CHECK(aatpeAndroidSdkInt() >= 0, "aatpeAndroidSdkInt returns nonnegative");
    const char* vn = aatpeAndroidVersionName();
    CHECK(vn != nullptr, "aatpeAndroidVersionName returns non-null");

    // G1-005: concurrent atomicWriteFile to the SAME path must always publish a
    // coherent whole content. The old fixed "<path>.tmp" meant two threads shared
    // the temp file and interleaved write() streams; the final file could be an
    // in-between mix of HEAD_A...HEAD_B. mkstemp gives each call its own tmp.
    printf("=== Utils: G1-005 concurrent atomic write coherence ===\n");
    {
        const char* p = "/tmp/aatpe_concurrent_atomic.txt";
        std::string bigA = "HEAD_A:" + std::string(64 * 1024, 'A') + ":TAIL_A";
        std::string bigB = "HEAD_B:" + std::string(64 * 1024, 'B') + ":TAIL_B";
        auto writer = [&](const std::string& c) {
            for (int i = 0; i < 300; i++) atomicWriteFile(p, c);
        };
        std::thread t1(writer, bigA);
        std::thread t2(writer, bigB);
        t1.join();
        t2.join();
        std::string final = readFile(p);
        bool coherent = (final == bigA || final == bigB);
        CHECK(coherent, "G1-005: final atomic content is one coherent write (no interleaving)");
        unlink(p);
    }

    printf("=== Helper audit: protected config/asset names are NOT deleted ===\n");
    // Legitimate user files / backups must survive.
    CHECK(!isSensitiveHelperFile("thread_roles.conf.bak"), "thread_roles.conf.bak protected");
    CHECK(!isSensitiveHelperFile("ml_config.json"), "ml_config.json protected");
    CHECK(!isSensitiveHelperFile("game_profiles.rules"), "game_profiles.rules protected");
    CHECK(!isSensitiveHelperFile("screen_off_rules.prop"), "screen_off_rules.prop protected");
    // Truly sensitive names must be flagged.
    CHECK(isSensitiveHelperFile("daemon_token"), "daemon_token flagged");
    CHECK(isSensitiveHelperFile("hmac_key"), "hmac_key flagged");
    CHECK(isSensitiveHelperFile(".daemon_token"), ".daemon_token dotfile flagged");
    CHECK(isSensitiveHelperFile(".hmac_key.bak"), ".hmac_key.bak dotfile backup flagged");

    printf("\n========================================\n"
           "Utils results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

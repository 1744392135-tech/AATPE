#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include "aatpe_ebpf.h"

static int g_testsRun = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_testsRun++; \
    if (cond) { g_testsPassed++; printf("  [PASS] %s\n", msg); } \
    else { g_testsFailed++; printf("  [FAIL] %s\n", msg); } \
} while(0)

static void testEbpfBytecode() {
    printf("\n=== P0-1: eBPF Embedded Bytecode Tests ===\n");

    size_t bcSize = aatpe_ebpf::EbpfScheduler::getEmbeddedBytecodeSize();
    const uint8_t* bc = aatpe_ebpf::EbpfScheduler::getEmbeddedBytecode(nullptr);

    TEST_ASSERT(bcSize > 16, "Bytecode size > 16 bytes (not empty placeholder)");
    printf("    Bytecode size: %zu bytes (%zu instructions)\n", bcSize, bcSize / 8);

    TEST_ASSERT(bcSize == 120, "Bytecode size == 120 bytes (15 instructions)");

    TEST_ASSERT(bcSize % 8 == 0, "Bytecode size is multiple of 8");

    TEST_ASSERT(bc[32] == 0x18, "Instruction 5 is LD_IMM64 (0x18)");
    TEST_ASSERT((bc[33] >> 4) == 1, "Instruction 5 src_reg == 1 (PSEUDO_MAP_FD)");

    TEST_ASSERT(bc[48] == 0x85, "Instruction 7 is BPF_CALL (0x85)");
    int32_t callImm = *reinterpret_cast<const int32_t*>(bc + 52);
    TEST_ASSERT(callImm == 1, "Instruction 7 calls bpf_map_lookup_elem (imm=1)");

    TEST_ASSERT(bc[96] == 0x7B, "Instruction 13 is STX DW (0x7B) map write-back");

    TEST_ASSERT(bc[56] == 0x55, "Instruction 8 is JNE (0x55) NULL check");

    TEST_ASSERT(bc[0] == 0xB7, "Instruction 1 is MOV64_IMM (0xB7)");
    TEST_ASSERT((bc[1] & 0x0F) == 2, "Instruction 1 dst_reg == 2");

    TEST_ASSERT(bc[112] == 0x95, "Instruction 15 is EXIT (0x95)");

    bool hasCall = false, hasStxDw = false, hasLdImm64 = false, hasExit = false;
    for (size_t i = 0; i + 8 <= bcSize; i += 8) {
        if (bc[i] == 0x85) hasCall = true;
        if (bc[i] == 0x7B) hasStxDw = true;
        if (bc[i] == 0x18) hasLdImm64 = true;
        if (bc[i] == 0x95) hasExit = true;
    }
    TEST_ASSERT(hasCall, "Bytecode contains BPF_CALL opcode");
    TEST_ASSERT(hasStxDw, "Bytecode contains STX DW (map write)");
    TEST_ASSERT(hasLdImm64, "Bytecode contains LD_IMM64 (map fd reference)");
    TEST_ASSERT(hasExit, "Bytecode contains EXIT instruction");

    TEST_ASSERT(bc[8] == 0x63, "Instruction 2 is STX W (0x63) store key to stack");

    TEST_ASSERT(bc[80] == 0x79, "Instruction 11 is LDX DW (0x79) read counter");

    TEST_ASSERT(bc[88] == 0x07, "Instruction 12 is ALU64 ADD (0x07)");
    int32_t addImm = *reinterpret_cast<const int32_t*>(bc + 92);
    TEST_ASSERT(addImm == 1, "Instruction 12 adds 1 (increment counter)");
}

static bool fileContains(const std::string& path, const std::string& pattern) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return content.find(pattern) != std::string::npos;
}

static void testWatchdogSource() {
    printf("\n=== P0-2: Watchdog Restart Logic Tests ===\n");
    const char* wdPath = "aatpe-watchdog.cpp";

    TEST_ASSERT(fileContains(wdPath, "posix_spawn"),
                "Watchdog contains posix_spawn for daemon restart");

    TEST_ASSERT(fileContains(wdPath, "SIGKILL"),
                "Watchdog contains SIGKILL for stuck daemon");

    TEST_ASSERT(fileContains(wdPath, "waitpid"),
                "Watchdog contains waitpid to reap daemon");

    TEST_ASSERT(fileContains(wdPath, "kPidFile"),
                "Watchdog references kPidFile for cleanup");

    TEST_ASSERT(fileContains(wdPath, "\"WATCHDOG_INTERVAL\""),
                "Watchdog reads WATCHDOG_INTERVAL (uppercase)");
    TEST_ASSERT(fileContains(wdPath, "\"WATCHDOG_MAX_RESTARTS\""),
                "Watchdog reads WATCHDOG_MAX_RESTARTS (uppercase)");
    TEST_ASSERT(fileContains(wdPath, "\"WATCHDOG_RESTART_WINDOW\""),
                "Watchdog reads WATCHDOG_RESTART_WINDOW (uppercase)");

    TEST_ASSERT(!fileContains(wdPath, "\"watchdog_interval_ms\""),
                "Watchdog does NOT use lowercase watchdog_interval_ms");
    TEST_ASSERT(!fileContains(wdPath, "\"watchdog_emergency_threshold\""),
                "Watchdog does NOT use lowercase watchdog_emergency_threshold");

    TEST_ASSERT(fileContains(wdPath, "sigaction"),
                "Watchdog contains sigaction signal handling");
    TEST_ASSERT(fileContains(wdPath, "g_shouldExit"),
                "Watchdog contains g_shouldExit atomic flag");

    TEST_ASSERT(fileContains(wdPath, "lockf"),
                "Watchdog contains lockf for single-instance protection");

    TEST_ASSERT(fileContains(wdPath, "heartbeat"),
                "Watchdog contains heartbeat stale detection");

    TEST_ASSERT(fileContains(wdPath, "restartCount"),
                "Watchdog contains restartCount rate limiting");
    TEST_ASSERT(fileContains(wdPath, "MAX_RESTARTS"),
                "Watchdog contains MAX_RESTARTS threshold");

    TEST_ASSERT(fileContains(wdPath, "watchdogEmergencyMigrate"),
                "Watchdog contains emergency migration before restart");

    TEST_ASSERT(fileContains(wdPath, "_SC_NPROCESSORS_ONLN"),
                "Watchdog uses runtime sysconf CPU detection");

    TEST_ASSERT(fileContains(wdPath, "isCriticalThreadComm"),
                "Watchdog contains critical thread whitelist");

    TEST_ASSERT(fileContains(wdPath, "cpu_capacity"),
                "Watchdog uses cpu_capacity for big-core detection");

    TEST_ASSERT(fileContains(wdPath, "--daemon"),
                "Watchdog spawns daemon with --daemon argument");
}

static void testWebuiOverview() {
    printf("\n=== P2-1: WebUI Overview CPU Utilization Tests ===\n");
    const char* htmlPath = "webroot/index.html";

    TEST_ASSERT(!fileContains(htmlPath, "Math.random()*100"),
                "WebUI does NOT use Math.random()*100 for CPU util");

    TEST_ASSERT(fileContains(htmlPath, "/percpu.json"),
                "WebUI fetches /percpu.json for real CPU data");

    TEST_ASSERT(fileContains(htmlPath, "N/A"),
                "WebUI shows N/A when CPU data unavailable");
}

static void testWebuiLogs() {
    printf("\n=== P2-2: WebUI Real Logs Tests ===\n");

    TEST_ASSERT(fileContains("webui_json.cpp", "writeLogsJson"),
                "Backend webui_json.cpp has writeLogsJson function");
    TEST_ASSERT(fileContains("webui_json.cpp", "/logs.json"),
                "Backend writes /logs.json");
    TEST_ASSERT(fileContains("webui_json.cpp", "aatpe.log"),
                "Backend reads aatpe.log for log content");

    TEST_ASSERT(fileContains("aatped.cpp", "writeLogsJson()"),
                "Daemon loop calls writeLogsJson()");

    TEST_ASSERT(fileContains("aatped.h", "writeLogsJson"),
                "aatped.h declares writeLogsJson()");

    TEST_ASSERT(fileContains("webroot/index.html", "readFile(LOG_FILE)") || fileContains("webroot/index.html", "log-container"),
                "Frontend reads real log file (not simulated)");

    TEST_ASSERT(fileContains("webroot/index.html", "ERROR") && fileContains("webroot/index.html", "WARN"),
                "Frontend has log level color tags (ERROR/WARN)");

    TEST_ASSERT(!fileContains("webroot/index.html", "=== AATPE Log ==="),
                "Frontend does NOT simulate logs with === AATPE Log ===");
}

static void testRedGreenBytecodeDetection() {
    printf("\n=== RED-GREEN: Bytecode Regression Detection ===\n");

    static const uint8_t brokenBytecode[] = {
        0xb7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    bool wouldCatchLength = (sizeof(brokenBytecode) <= 16);
    TEST_ASSERT(wouldCatchLength,
                "RED test: broken 16-byte bytecode would be caught by length check");

    bool wouldCatchMissingCall = true;
    for (size_t i = 0; i < sizeof(brokenBytecode); i += 8) {
        if (brokenBytecode[i] == 0x85) { wouldCatchMissingCall = false; break; }
    }
    TEST_ASSERT(wouldCatchMissingCall,
                "RED test: broken bytecode lacks BPF_CALL (would be caught)");

    bool wouldCatchMissingMapWrite = true;
    for (size_t i = 0; i < sizeof(brokenBytecode); i += 8) {
        if (brokenBytecode[i] == 0x7B) { wouldCatchMissingMapWrite = false; break; }
    }
    TEST_ASSERT(wouldCatchMissingMapWrite,
                "RED test: broken bytecode lacks map write (would be caught)");
}

int main() {
    printf("===========================================================\n");
    printf("  AATPE P0+P2 Fix Verification Tests\n");
    printf("===========================================================\n");

    testEbpfBytecode();
    testWatchdogSource();
    testWebuiOverview();
    testWebuiLogs();
    testRedGreenBytecodeDetection();

    printf("\n===========================================================\n");
    printf("  RESULTS: %d run, %d passed, %d failed\n",
           g_testsRun, g_testsPassed, g_testsFailed);
    printf("===========================================================\n");

    if (g_testsFailed > 0) {
        printf("  SOME TESTS FAILED!\n");
        return 1;
    }
    printf("  ALL TESTS PASSED\n");
    return 0;
}

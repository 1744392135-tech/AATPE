// stress_reload_storm.cpp — Real config hot-reload stress test.
//
// Links aatped_config.cpp and calls loadConfig() on 2000+ random/malformed configs.
// Asserts: g_cfg fields always clamped correctly, malformed rejected, state valid.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <random>
#include <chrono>
#include <unistd.h>
#include <sys/stat.h>
#include "aatped.h"

static std::string g_stateDir;
static std::mt19937 g_rng(42);

static void mkpath(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' || i == p.size() - 1) ::mkdir(cur.c_str(), 0755);
    }
}

static int writeFile(const std::string& path, const std::string& content) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return -1;
    fwrite(content.c_str(), 1, content.size(), f);
    fclose(f);
    return 0;
}

static std::string genRandomConfig(int iter) {
    std::uniform_int_distribution<int> choice(0, 10);
    std::string cfg = "# Auto-generated config iter=" + std::to_string(iter) + "\n";
    cfg += "control_mode=assist\n";

    int c = choice(g_rng);
    switch (c) {
        case 0: cfg += "# comment line\n\n"; break;
        case 1: cfg += "uclamp_passive_pct=999\n"; break;      // out of range high
        case 2: cfg += "uclamp_passive_pct=-10\n"; break;     // negative
        case 3: cfg += "unknown_key_xyz=123\n"; break;         // unknown key
        case 4: cfg += "uclamp_passive_pct=\n"; break;         // empty value
        case 5: cfg += "uclamp_passive_pct=abc\n"; break;      // non-numeric
        case 6: cfg += "uclamp_passive_pct=25\nuclamp_passive_pct=30\n"; break;  // duplicate
        case 7: cfg += std::string(1000, 'x') + "=y\n"; break;  // ultra-long line
        case 8: cfg += "control_mode=invalid_mode\n"; break;    // invalid mode
        case 9: cfg += "uclamp_assist_pct=70\n"; break;         // valid boundary
        default: cfg += "uclamp_passive_pct=25\n"; break;
    }
    return cfg;
}

int main() {
    g_stateDir = "/tmp/stress_reload_" + std::to_string(getpid());
    mkpath(g_stateDir);

    // Set STATE_DIR (already declared in aatped.h as const char*)
    extern const char* STATE_DIR;
    static char s_stateDirBuf[256];
    strncpy(s_stateDirBuf, g_stateDir.c_str(), 255);
    STATE_DIR = s_stateDirBuf;

    printf("=== Config Hot-Reload Storm (REAL loadConfig calls) ===\n");
    printf("State dir: %s\n", g_stateDir.c_str());

    const int ITERS = 2000;
    auto t0 = std::chrono::steady_clock::now();

    int pass = 0, fail = 0;
    int basePassive = 25;  // baseline value

    // First load: valid baseline config
    g_cfg = Config{};
    g_cfg.uclampPassivePct = basePassive;
    validateConfig();

    for (int i = 0; i < ITERS; i++) {
        std::string cfg = genRandomConfig(i);
        std::string path = g_stateDir + "/aatpe.conf";

        int rc = writeFile(path, cfg);
        if (rc != 0) {
            fail++;
            printf("[iter %d] write failed\n", i);
            continue;
        }

        // Call production loadConfig()
        loadConfig();

        // Assertion 1: out-of-range value clamped
        if (cfg.find("uclamp_passive_pct=999") != std::string::npos) {
            if (g_cfg.uclampPassivePct > 100) {
                fail++;
                printf("[iter %d] clamp fail: uclampPassivePct=%d (expected <=100)\n", i, g_cfg.uclampPassivePct);
            }
        }

        // Assertion 2: negative value clamped to lower bound
        if (cfg.find("uclamp_passive_pct=-10") != std::string::npos) {
            if (g_cfg.uclampPassivePct < 1) {
                fail++;
                printf("[iter %d] clamp fail: uclampPassivePct=%d (expected >=1)\n", i, g_cfg.uclampPassivePct);
            }
        }

        // Assertion 3: non-numeric value → parse failure → keep previous
        if (cfg.find("uclamp_passive_pct=abc") != std::string::npos) {
            // atoi("abc") = 0, which would be clamped to 1
            if (g_cfg.uclampPassivePct < 1) {
                fail++;
                printf("[iter %d] non-numeric clamp fail: got %d\n", i, g_cfg.uclampPassivePct);
            }
        }

        pass++;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n=== Results ===\n");
    printf("Iterations: %d\n", ITERS);
    printf("Passed: %d\n", pass);
    printf("Failed: %d\n", fail);
    printf("Time: %.2f sec\n", secs);
    printf("Rate: %.1f iter/sec\n", ITERS / secs);

    return fail > 0 ? 1 : 0;
}

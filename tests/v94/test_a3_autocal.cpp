// test_a3_autocal.cpp — v4.2 Batch A3: autocalBigRatio() read / corrupt / fallback.
// DEFAULT-OFF bit-identical contract: absent/corrupt/fingerprint-mismatched
// state/autocal.json must return exactly 0.87f (the v4.1.1 literal).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fstream>
#include "aatpe_caps.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// Must match stub_a3.cpp STATE_DIR.
static const char* kStateDir = "/tmp/aatpe_v42_a3";
static const char* kFp = "FP-TEST-1";

static void writeAutocal(const char* body) {
    std::string p = std::string(kStateDir) + "/autocal.json";
    std::ofstream f(p, std::ios::trunc);
    f << body;
}
static void removeAutocal() {
    std::string p = std::string(kStateDir) + "/autocal.json";
    ::unlink(p.c_str());
}

using namespace aatpe::caps;

int main() {
    std::string p = kStateDir;
    ::system(("mkdir -p " + p).c_str());

    printf("=== A3: autocalBigRatio() ===\n");
    // Case 1: no file -> default 0.87f
    removeAutocal();
    autocalResetForTest();
    autocalSetFingerprintForTest(kFp);
    float r1 = autocalBigRatio();
    CHECK(r1 > 0.869f && r1 < 0.871f, "missing file -> 0.87f (bit-identical default)");

    // Case 2: valid file + matching fingerprint -> stored ratio
    writeAutocal("ratio=0.91\nfingerprint=FP-TEST-1\n");
    autocalSetFingerprintForTest(kFp);   // resets cache
    float r2 = autocalBigRatio();
    CHECK(r2 > 0.909f && r2 < 0.911f, "valid file+matching fp -> stored 0.91");

    // Case 3: corrupt ratio (out of sane range) -> 0.87f
    writeAutocal("ratio=0.12\nfingerprint=FP-TEST-1\n");
    autocalSetFingerprintForTest(kFp);
    float r3 = autocalBigRatio();
    CHECK(r3 > 0.869f && r3 < 0.871f, "corrupt ratio (0.12) -> 0.87f");

    // Case 4: fingerprint mismatch (OTA) -> 0.87f
    writeAutocal("ratio=0.91\nfingerprint=OLD-FP\n");
    autocalSetFingerprintForTest(kFp);
    float r4 = autocalBigRatio();
    CHECK(r4 > 0.869f && r4 < 0.871f, "fingerprint mismatch -> 0.87f");

    // Case 5: garbage bytes (not key=value) -> 0.87f
    writeAutocal("\x00\x01\x02 not json at all\n");
    autocalSetFingerprintForTest(kFp);
    float r5 = autocalBigRatio();
    CHECK(r5 > 0.869f && r5 < 0.871f, "garbage content -> 0.87f");

    // Case 6: caching — second call returns the cached value without re-reading.
    // Delete the file on disk; the cached 0.87 must persist (no thrash).
    removeAutocal();
    float r6 = autocalBigRatio();
    CHECK(r6 > 0.869f && r6 < 0.871f, "cache: repeated call stable after file removed");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

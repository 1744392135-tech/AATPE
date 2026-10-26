// test_pkglist_parsing.cpp — host regression for getPkgUid() (frame.cpp, MAJOR-1/M4).
//
// The packages.list parser was previously exercised only by fz_pkglist.cpp which
// #include'd frame.cpp with a fopen interposer. Per the v4.4.8 R1/RCV close-out
// the parser body lives in getPkgUidImpl(pkg,path) and is exposed to host tests
// through the guarded seam aatpeTestGetPkgUid(pkg, path) (#ifdef AATPE_HOST_TEST).
// This test LINKS frame.o (like the other frame tests) instead of #include'ing
// the whole TU, passing an explicit on-disk packages.list path per case.
//
// Case matrix (R1 Q5, 8 cases): normal line, 37-byte junk-line fuzz sample,
// missing columns, non-numeric uid, missing file, prefix trap, empty file,
// negative uid.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>

// Guarded seam defined in frame.cpp (only compiled under AATPE_HOST_TEST, which
// this whole host build defines). Returns the parsed uid, or -1 on any failure.
extern "C" long aatpeTestGetPkgUid(const char* pkg, const char* pkglistPath);

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, msg) do {                                            \
    if (!(cond)) { printf("[FAIL] %s\n", msg); failures++; }             \
    else         { printf("[ok]   %s\n", msg); }                         \
} while (0)

// Write `content` to a uniquely-named temp packages.list, return its path.
static std::string writePkglist(const std::string& tag, const std::string& content) {
    fs::path dir = fs::temp_directory_path() / "aatpe_v94_pkglist";
    fs::create_directories(dir);
    fs::path p = dir / ("packages_" + tag + ".list");
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
    f.close();
    return p.string();
}

static long runCase(const std::string& tag, const std::string& content,
                    const std::string& pkg) {
    std::string path = writePkglist(tag, content);
    return aatpeTestGetPkgUid(pkg.c_str(), path.c_str());
}

int main() {
    printf("=== test_pkglist_parsing: getPkgUidImpl regression ===\n");

    // 1) Normal line: "pkg uid debugFlag dataPath ..." -> uid 10047.
    CHECK(runCase("normal",
                  "com.fuzz.target 10047 0 /data/data/com.fuzz.target\n",
                  "com.fuzz.target") == 10047,
          "normal line -> uid=10047");

    // 2) The exact 36-byte fuzz sample: binary junk first line must be skipped,
    //    second line "com.fuzz.target 42" must parse to 42. No crash.
    CHECK(runCase("junk36",
                  std::string("\x01\x02\x03 garbage line\ncom.fuzz.target 42\n", 36),
                  "com.fuzz.target") == 42,
          "36B junk-line sample -> uid=42, no crash");

    // 3) Missing columns: "com.fuzz.target" with no trailing space / no uid
    //    must NOT match (line[pkgLen] != ' ') -> -1.
    CHECK(runCase("nocol", "com.fuzz.target\n", "com.fuzz.target") == -1,
          "missing columns (pkg only, no space) -> -1");

    // 4) Non-numeric uid: strtol("abc")=0; the production caller discards
    //    uid<=0, but the parser itself returns 0.
    CHECK(runCase("nandum", "com.fuzz.target abc 0 /data\n", "com.fuzz.target") == 0,
          "non-numeric uid -> 0 (caller discards <=0)");

    // 5) Missing file -> -1 (fail-closed).
    CHECK(aatpeTestGetPkgUid("com.anything",
                             "/tmp/aatpe_v94_pkglist/does_not_exist_xyz.list") == -1,
          "missing packages.list -> -1");

    // 6) Prefix trap: "com.fuzz.targetX" must NOT shadow "com.fuzz.target".
    CHECK(runCase("prefix",
                  "com.fuzz.targetX 99999 0 /data\ncom.fuzz.target 2000 1 /data\n",
                  "com.fuzz.target") == 2000,
          "prefix trap: com.fuzz.targetX does not shadow com.fuzz.target");

    // 7) Empty file -> EOF immediately -> -1.
    CHECK(runCase("empty", "", "com.fuzz.target") == -1,
          "empty file -> -1");

    // 8) Negative uid: strtol("-1")=-1; the production caller treats uid<=0 as
    //    invalid, so a negative line must not be mistaken for a valid container.
    CHECK(runCase("neguid", "com.fuzz.target -1 0 /data\n", "com.fuzz.target") == -1,
          "negative uid -> -1 (discarded as invalid)");

    if (failures == 0) {
        printf("=== test_pkglist_parsing: ALL PASS (8/8) ===\n");
        return 0;
    }
    printf("=== test_pkglist_parsing: %d FAILURES ===\n", failures);
    return 1;
}

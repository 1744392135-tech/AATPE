#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>

extern "C" long aatpeTestGetPkgUid(const char* pkg, const char* pkglistPath);

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, msg) do {                                            \
    if (!(cond)) { printf("[FAIL] %s\n", msg); failures++; }             \
    else         { printf("[ok]   %s\n", msg); }                         \
} while (0)

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

    CHECK(runCase("normal",
                  "com.fuzz.target 10047 0 /data/data/com.fuzz.target\n",
                  "com.fuzz.target") == 10047,
          "normal line -> uid=10047");

    CHECK(runCase("junk36",
                  std::string("\x01\x02\x03 garbage line\ncom.fuzz.target 42\n", 36),
                  "com.fuzz.target") == 42,
          "36B junk-line sample -> uid=42, no crash");

    CHECK(runCase("nocol", "com.fuzz.target\n", "com.fuzz.target") == -1,
          "missing columns (pkg only, no space) -> -1");

    CHECK(runCase("nandum", "com.fuzz.target abc 0 /data\n", "com.fuzz.target") == 0,
          "non-numeric uid -> 0 (caller discards <=0)");

    CHECK(aatpeTestGetPkgUid("com.anything",
                             "/tmp/aatpe_v94_pkglist/does_not_exist_xyz.list") == -1,
          "missing packages.list -> -1");

    CHECK(runCase("prefix",
                  "com.fuzz.targetX 99999 0 /data\ncom.fuzz.target 2000 1 /data\n",
                  "com.fuzz.target") == 2000,
          "prefix trap: com.fuzz.targetX does not shadow com.fuzz.target");

    CHECK(runCase("empty", "", "com.fuzz.target") == -1,
          "empty file -> -1");

    CHECK(runCase("neguid", "com.fuzz.target -1 0 /data\n", "com.fuzz.target") == -1,
          "negative uid -> -1 (discarded as invalid)");

    if (failures == 0) {
        printf("=== test_pkglist_parsing: ALL PASS (8/8) ===\n");
        return 0;
    }
    printf("=== test_pkglist_parsing: %d FAILURES ===\n", failures);
    return 1;
}

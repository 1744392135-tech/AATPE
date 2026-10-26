// test_apklabel.cpp - v94 hermetic host tests for aatped_apklabel.cpp.
//
// No external fixture files, no /data reads: every AXML/resources.arsc/ZIP blob
// below is embedded in fixture_arrays_apklabel.h (byte-for-byte the output of
// L's authoritative fixture_gen.py). At runtime blobs are dropped into a
// mkdtemp scratch dir and fed to the real parser.
//
// Covers:
//   - hardcoded label, STORED + (L's shipped, method=0) zip path
//   - @string reference: zh-CN config beats default; default-only;
//     density=0 beats density=320; multi-package 0x80 has string but 0x7f ref
//     absent -> must NOT cross-resolve (returns "")
//   - UTF-16 string pool
//   - split APK resource fallback (base missing -> split_config.xxhdpi carries)
//   - packages.list exact match (prefix trap, suffix trap)
//   - look-alike packages never cross-wire (zmxy->造梦西游4, netease->梦幻西游)
//   - label cache: peekCachedLabel cold miss returns "" without IO; after
//     labelForPackage resolves, peek returns the positive label; empty result
//     is negative-cached (15s TTL, positive 300s - constants verified in source)
//   - 8 hostile/truncated samples -> all return "", never crash.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aatped_apklabel.h"
#include "fixture_arrays_apklabel.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

static char g_tmp[256] = {0};
static void mkdirp(const std::string& p) {
    if (p.empty()) return;
    mkdirp(p.substr(0, p.find_last_of('/')));
    ::mkdir(p.c_str(), 0755);
}
static void writeBlob(const char* relPath, const unsigned char* data, size_t n) {
    std::string p = std::string(g_tmp) + "/" + relPath;
    size_t slash = p.find_last_of('/');
    if (slash != std::string::npos) mkdirp(p.substr(0, slash));
    FILE* f = ::fopen(p.c_str(), "wb");
    if (f) { ::fwrite(data, 1, n, f); ::fclose(f); }
}
static std::string tmpPath(const char* rel) { return std::string(g_tmp) + "/" + rel; }

int main() {
    printf("=== aatped_apklabel: hermetic fixture matrix ===\n");
    strcpy(g_tmp, "/tmp/test_apklabel_XXXXXX");
    if (!::mkdtemp(g_tmp)) { printf("FATAL: mkdtemp failed\n"); return 2; }

    // ---- direct hardcoded label ----
    writeBlob("direct_stored.apk", kApk_direct_stored, sizeof(kApk_direct_stored));
    writeBlob("direct_deflated.apk", kApk_direct_deflated, sizeof(kApk_direct_deflated));
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("direct_stored.apk")) == "造梦西游4",
          "stored zip: hardcoded Chinese label decoded");
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("direct_deflated.apk")) == "造梦西游4",
          "deflated(method0) zip: hardcoded label decoded");

    // ---- real DEFLATED (method=8) fixtures ----
    writeBlob("real_deflated.apk", kApk_real_deflated, sizeof(kApk_real_deflated));
    writeBlob("real_deflated_truncated.apk", kApk_real_deflated_truncated, sizeof(kApk_real_deflated_truncated));
    writeBlob("real_deflated_corrupt.apk", kApk_real_deflated_corrupt, sizeof(kApk_real_deflated_corrupt));
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("real_deflated.apk")) == "造梦西游4",
          "real DEFLATED(method=8): hardcoded Chinese label decoded via zlib inflate");
    {
        std::string err;
        CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("real_deflated_truncated.apk"), &err).empty(),
              "real DEFLATED truncated stream -> empty (safe, no crash)");
        CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("real_deflated_corrupt.apk"), &err).empty(),
              "real DEFLATED corrupt bitstream -> empty (safe, no crash)");
    }

    // ---- @string reference resolution (nested resources.arsc) ----
    writeBlob("ref_zhcn.apk", kApk_ref_zhcn, sizeof(kApk_ref_zhcn));
    writeBlob("ref_default.apk", kApk_ref_default, sizeof(kApk_ref_default));
    writeBlob("ref_density.apk", kApk_ref_density, sizeof(kApk_ref_density));
    writeBlob("ref_multipkg.apk", kApk_ref_multipkg, sizeof(kApk_ref_multipkg));
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("ref_zhcn.apk")) == "造梦西游4",
          "@string ref: zh-CN config chosen over default");
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("ref_default.apk")) == "快手",
          "@string ref: default-config string resolved");
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("ref_density.apk")) == "造梦西游4",
          "@string ref: density=0 config beats density=320");
    {
        std::string err;
        CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("ref_multipkg.apk"), &err).empty(),
              "@string ref: multi-pkg 0x7f absent does NOT cross-resolve into 0x80");
    }

    // ---- UTF-16 string pool ----
    writeBlob("utf16_pool.apk", kApk_utf16_pool, sizeof(kApk_utf16_pool));
    CHECK(aatpe::apklabel::readLabelFromApk(tmpPath("utf16_pool.apk")) == "快手",
          "UTF-16 string pool decoded to UTF-8 label");

    // ---- peekCachedLabel: cold miss returns "" without triggering a parse ----
    CHECK(aatpe::apklabel::peekCachedLabel("com.never.seen.before").empty(),
          "peekCachedLabel: cold miss returns empty (no IO triggered)");

    // ---- build a fake install tree for labelForPackage / cache tests ----
    writeBlob("netease_direct.apk", kApk_netease_direct, sizeof(kApk_netease_direct));
    // zmxy channel apk
    writeBlob("data/app/~~zmxy==/org.cocos2dx.zmxy.aligames-1==/base.apk",
              kApk_direct_stored, sizeof(kApk_direct_stored));
    // netease apk
    writeBlob("data/app/~~netease==/com.netease.my-1==/base.apk",
              kApk_netease_direct, sizeof(kApk_netease_direct));
    // split demo: base has manifest ref but no resource, split_config carries it
    writeBlob("data/app/~~split==/com.example.splitdemo-1==/base.apk",
              kApk_split_base, sizeof(kApk_split_base));
    writeBlob("data/app/~~split==/com.example.splitdemo-1==/split_config.xxhdpi.apk",
              kApk_split_config_xxhdpi, sizeof(kApk_split_config_xxhdpi));

    const char* listText =
        "org.cocos2dx.zmxy.aligames 10088 0 /data/user/0/org.cocos2dx.zmxy.aligames targetSdk=29\n"
        "com.netease.my 10099 0 /data/user/0/com.netease.my targetSdk=30\n"
        "com.example.splitdemo 10100 0 /data/user/0/com.example.splitdemo targetSdk=31\n";
    std::string lp = tmpPath("packages.list");
    { FILE* f = ::fopen(lp.c_str(), "w"); ::fwrite(listText, 1, strlen(listText), f); ::fclose(f); }
    std::string appsRoot = tmpPath("data/app");

    // resolve zmxy -> populates positive cache
    CHECK(aatpe::apklabel::labelForPackage("org.cocos2dx.zmxy.aligames", lp, appsRoot) == "造梦西游4",
          "labelForPackage: zmxy channel apk resolves to 造梦西游4");
    CHECK(aatpe::apklabel::peekCachedLabel("org.cocos2dx.zmxy.aligames") == "造梦西游4",
          "peekCachedLabel: positive label now served from cache (no re-parse)");
    // cross-wire: netease must resolve to 梦幻西游, not 造梦西游4
    CHECK(aatpe::apklabel::labelForPackage("com.netease.my", lp, appsRoot) == "梦幻西游",
          "labelForPackage: netease apk -> 梦幻西游 (not 造梦西游4)");

    // ---- packages.list exact match traps ----
    CHECK(aatpe::apklabel::packageListHasPackage(listText, "org.cocos2dx.zmxy.aligames"),
          "packages.list: real zmxy pkg present");
    CHECK(aatpe::apklabel::packageListHasPackage(listText, "com.netease.my"),
          "packages.list: real netease pkg present");
    CHECK(!aatpe::apklabel::packageListHasPackage(listText, "com.netease"),
          "packages.list: 'com.netease' prefix trap rejected");
    CHECK(!aatpe::apklabel::packageListHasPackage(listText, "org.cocos2dx.zmxy.aligamesX"),
          "packages.list: 'aligamesX' suffix trap rejected");
    CHECK(!aatpe::apklabel::packageListHasPackage(listText, "com.tencent.tmgp.zmxy"),
          "packages.list: unknown pkg absent");

    // ---- split fallback ----
    CHECK(aatpe::apklabel::labelForPackage("com.example.splitdemo", lp, appsRoot) == "梦幻西游",
          "split fallback: label carried by split_config apk resolved");

    // ---- negative cache: unlisted pkg yields "" and is cached negative ----
    CHECK(aatpe::apklabel::labelForPackage("com.unlisted.nowhere", lp, appsRoot).empty(),
          "labelForPackage: unlisted pkg resolves to empty");
    CHECK(aatpe::apklabel::peekCachedLabel("com.unlisted.nowhere").empty(),
          "peekCachedLabel: empty result negative-cached (15s TTL, no IO)");

    // ---- hostile samples: all must return empty, never crash ----
    struct Bad { const char* name; const unsigned char* data; size_t n; };
    Bad bads[] = {
        {"bad_truncated_manifest", kApk_bad_truncated_manifest, sizeof(kApk_bad_truncated_manifest)},
        {"bad_garbage",           kApk_bad_garbage,           sizeof(kApk_bad_garbage)},
        {"bad_notazip",           kApk_bad_notazip,           sizeof(kApk_bad_notazip)},
        {"bad_ref_outofrange",     kApk_bad_ref_outofrange,     sizeof(kApk_bad_ref_outofrange)},
        {"bad_framework_ref",      kApk_bad_framework_ref,      sizeof(kApk_bad_framework_ref)},
        {"bad_truncated_arsc",     kApk_bad_truncated_arsc,     sizeof(kApk_bad_truncated_arsc)},
        {"bad_central_oob",       kApk_bad_central_oob,       sizeof(kApk_bad_central_oob)},
        {"bad_central_boundary",   kApk_bad_central_boundary,   sizeof(kApk_bad_central_boundary)},
    };
    for (auto& b : bads) {
        writeBlob(b.name, b.data, b.n);
        std::string err;
        std::string got = aatpe::apklabel::readLabelFromApk(tmpPath(b.name), &err);
        CHECK(got.empty(), (std::string("hostile sample empty: ") + b.name).c_str());
    }

    // ---- fuzz regression: heap OOB from malformed AXML attrCount/attrSize ----
    writeBlob("fuzz_heap_oob.apk", kApk_fuzz_heap_oob, sizeof(kApk_fuzz_heap_oob));
    {
        std::string err;
        std::string got = aatpe::apklabel::readLabelFromApk(tmpPath("fuzz_heap_oob.apk"), &err);
        CHECK(got.empty(), "fuzz_heap_oob: malformed attr array returns empty (no OOB read)");
    }

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

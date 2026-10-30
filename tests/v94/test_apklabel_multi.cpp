#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aatped_apklabel.h"

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
static void writeFile(const char* rel, const std::string& content) {
    std::string p = std::string(g_tmp) + "/" + rel;
    size_t slash = p.find_last_of('/');
    if (slash != std::string::npos) mkdirp(p.substr(0, slash));
    FILE* f = ::fopen(p.c_str(), "wb");
    if (f) { ::fwrite(content.data(), 1, content.size(), f); ::fclose(f); }
}
static std::string tmpPath(const char* rel) { return std::string(g_tmp) + "/" + rel; }

static std::string freshDir(const char* tag) {
    std::string p = std::string(g_tmp) + "/" + tag;
    mkdirp(p);
    return p;
}

static bool hasPkg(const std::vector<std::string>& v, const std::string& p) {
    for (auto& x : v) if (x == p) return true;
    return false;
}

int main() {
    printf("=== R7 multi-user enumeration (final) ===\n");
    strcpy(g_tmp, "/tmp/test_r7final_XXXXXX");
    if (!::mkdtemp(g_tmp)) { printf("FATAL: mkdtemp failed\n"); return 2; }

    CHECK(aatpe::apklabel::userIdFromUid(10100) == 0, "uid 10100 -> user 0");
    CHECK(aatpe::apklabel::userIdFromUid(110100) == 1, "uid 110100 -> user 1");
    CHECK(aatpe::apklabel::userIdFromUid(1000) == 0, "system uid 1000 -> user 0");
    CHECK(aatpe::apklabel::userIdFromUid(99100) == 0, "isolated uid 99100 -> user 0");
    CHECK(aatpe::apklabel::userIdFromUid(100000) == 1, "uid 100000 -> user 1");

    writeFile("f1/list",
        "com.example.game1 10100 0 /data/user/0/com.example.game1\n"
        "com.example.game2 10101 1 /data/user/0/com.example.game2\n");
    auto r1 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f1/list"), freshDir("f1_app"), freshDir("f1_user"));
    CHECK(r1.size() == 2, "user0-only: 2 third-party packages");
    CHECK(hasPkg(r1, "com.example.game1"), "user0-only: game1 present");

    writeFile("f2/list",
        "com.example.game1 10100 0 /data/user/0/com.example.game1\n"
        "com.work.app1 110100 0 /data/user/10/com.work.app1\n"
        "com.work.app2 110101 0 /data/user/10/com.work.app2\n");
    auto r2 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f2/list"), freshDir("f2_app"), freshDir("f2_user"));
    CHECK(r2.size() == 3, "user0+work: 3 packages (work apps included)");
    CHECK(hasPkg(r2, "com.work.app1"), "user0+work: work-profile app1 present");

    writeFile("f3/list",
        "com.android.settings 1000 0 /data/user/0/com.android.settings\n"
        "com.android.systemui 1001 0 /data/user/0/com.android.systemui\n"
        "com.user.game 10200 0 /data/user/0/com.user.game\n");
    auto r3 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f3/list"), freshDir("f3_app"), freshDir("f3_user"));
    CHECK(r3.size() == 1, "system apps excluded: only 1 third-party");
    CHECK(hasPkg(r3, "com.user.game"), "system apps excluded: user game kept");
    CHECK(!hasPkg(r3, "com.android.settings"), "system apps excluded: settings dropped");

    writeFile("f4/list",
        "org.cocos2dx.zmxy.mi 10300 0 /data/user/0/org.cocos2dx.zmxy.mi\n"
        "com.smile.gifmaker 10301 0 /data/user/0/com.smile.gifmaker\n"
        "com.tencent.tmgp.sgame 10302 0 /data/user/0/com.tencent.tmgp.sgame\n"
        "com.netease.my 10303 0 /data/user/0/com.netease.my\n");
    auto r4 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f4/list"), freshDir("f4_app"), freshDir("f4_user"));
    CHECK(hasPkg(r4, "org.cocos2dx.zmxy.mi"), "real games: zmxy kept");
    CHECK(hasPkg(r4, "com.smile.gifmaker"), "real games: 快手 kept");
    CHECK(hasPkg(r4, "com.tencent.tmgp.sgame"), "real games: 王者 kept");
    CHECK(hasPkg(r4, "com.netease.my"), "real games: 梦幻西游 kept");

    writeFile("f5/list",
        "com.normal.app 10400 0 /data/user/0/com.normal.app\n"
        "../etc/passwd 10401 0 /data/user/0/x\n"
        "/absolute/path 10402 0 /data/user/0/y\n"
        "system.uid.low 2000 0 /data/user/0/sysuid\n");
    auto r5 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f5/list"), freshDir("f5_app"), freshDir("f5_user"));
    CHECK(hasPkg(r5, "com.normal.app"), "malicious: normal app kept");
    CHECK(r5.size() == 1, "malicious: ../ and /abs and sysuid all rejected, only 1 valid");

    {
        std::string longPkg = "com.valid.longname.";
        while (longPkg.size() < 4000) longPkg += "x";
        writeFile("f6/list",
            longPkg + " 10500 0 /data/user/0/x\n"
            "com.after.long 10501 0 /data/user/0/after\n");
        auto r6 = aatpe::apklabel::listInstalledPackagesAllUsers(
            tmpPath("f6/list"), freshDir("f6_app"), freshDir("f6_user"));
        CHECK(hasPkg(r6, "com.after.long"), "long line: next valid package survives");
        CHECK(r6.size() == 1, "long line: oversize pkg rejected, no fragment injected");
    }

    auto r7 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f7/nonexistent"), freshDir("f7_app"), freshDir("f7_user"));
    CHECK(r7.empty(), "missing list: empty result (fail-closed)");

    writeFile("f8/list", "com.list.only 10600 0 /data/user/0/com.list.only\n");
    writeFile("f8_app/~~aaa==/com.extra.app-XyZ==/base.apk", "fake");
    auto r8 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f8/list"), tmpPath("f8_app"), freshDir("f8_user"));
    CHECK(hasPkg(r8, "com.list.only"), "app scan: list package kept");
    CHECK(hasPkg(r8, "com.extra.app"), "app scan: extra app found");

    writeFile("f9/list",
        "com.uid.garbage notanumber 0 /data/user/0/x\n"
        "com.uid.ok 10700 0 /data/user/0/com.uid.ok\n");
    auto r9 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f9/list"), freshDir("f9_app"), freshDir("f9_user"));
    CHECK(hasPkg(r9, "com.uid.ok"), "malformed uid: valid package kept");

    writeFile("f10/list", "\n\n  \n\ncom.blank.ok 10800 0 /data/user/0/com.blank.ok\n\n");
    auto r10 = aatpe::apklabel::listInstalledPackagesAllUsers(
        tmpPath("f10/list"), freshDir("f10_app"), freshDir("f10_user"));
    CHECK(r10.size() == 1, "blank lines: only 1 package");
    CHECK(hasPkg(r10, "com.blank.ok"), "blank lines: valid package present");

    printf("\n=== RESULT: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

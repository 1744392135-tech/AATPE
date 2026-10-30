#include <cstdio>
#include <string>
#include <set>
#include <map>
#include <vector>

namespace aatpe_spillover {
bool isVmContainerPkg(const std::string& pkg);
void setVmContainerPrefixesForTest(const std::vector<std::string>& pkgs);
void setVmGuestPnamesForTest(const std::set<std::string>& pkgs);
std::set<std::string> resolveVmGuestPnames(const std::string& containerPkg);
std::set<std::string> intersectVmGuestOverrides(const std::string& overridesText,
                                                 const std::string& containerPkg,
                                                 long cuid,
                                                 const std::map<std::string,long>& runningUidByPkg);
}
namespace vm = aatpe_spillover;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

int main() {
    printf("==> v4.2.7 VM container guest-game resolution (host) ==\n");

    vm::setVmContainerPrefixesForTest({
        "com.vmos.pro", "com.vmos", "com.bly.sw",
        "com.lbe.parallel", "com.explayer.appclone", "com.do.mobile.dualspace",
    });

    CHECK(vm::isVmContainerPkg("com.vmos.pro"), "v1: com.vmos.pro is a VM container");
    CHECK(vm::isVmContainerPkg("com.vmos.pro.vmsupport"), "v1: com.vmos.pro.* variant is VM");
    CHECK(vm::isVmContainerPkg("com.vmos.plus"), "v1: com.vmos.* variant is VM");
    CHECK(vm::isVmContainerPkg("com.bly.sw"), "v1: com.bly.sw is VM");
    CHECK(vm::isVmContainerPkg("com.lbe.parallel"), "v1: com.lbe.parallel is VM");
    CHECK(vm::isVmContainerPkg("com.explayer.appclone"), "v1: com.explayer.appclone is VM");
    CHECK(vm::isVmContainerPkg("com.do.mobile.dualspace"), "v1: com.do.mobile.dualspace is VM");
    CHECK(!vm::isVmContainerPkg("com.tencent.mm"), "v1: normal app is NOT a VM container");
    CHECK(!vm::isVmContainerPkg("com.tencent.tmgp.sgame"), "v1: native game is NOT a VM container");
    CHECK(!vm::isVmContainerPkg(""), "v1: empty string is NOT a VM container");

    vm::setVmGuestPnamesForTest({"org.cocos2dx.zmxy.mi"});
    const auto& guests = vm::resolveVmGuestPnames("com.vmos.pro");
    CHECK(guests.size() == 1, "v2: forced guest set has 1 entry");
    CHECK(guests.count("org.cocos2dx.zmxy.mi") == 1, "v2: guest game pname present");
    CHECK(guests.count("com.vmos.pro") == 0, "v2: container itself not in guest set");

    std::string fg = "com.vmos.pro";
    std::set<std::string> vmGuest = vm::resolveVmGuestPnames(fg);
    auto allowed = [&](const std::string& pname) {
        return pname == fg || (!vmGuest.empty() && vmGuest.count(pname));
    };
    CHECK(allowed("org.cocos2dx.zmxy.mi"), "v3: guest game tid admitted for promotion");
    CHECK(allowed("com.vmos.pro"), "v3: container's own threads still admitted");
    CHECK(!allowed("com.malicious.spoof"), "v3: unrelated bg package still excluded");
    CHECK(!allowed("com.tencent.tmgp.sgame"), "v3: non-VM native game excluded when fg=VM");

    vm::setVmGuestPnamesForTest({});
    std::string nfg = "com.tencent.tmgp.sgame";
    std::set<std::string> noGuests;
    auto nallowed = [&](const std::string& pname) {
        return pname == nfg || (!noGuests.empty() && noGuests.count(pname));
    };
    CHECK(nallowed("com.tencent.tmgp.sgame"), "v4: native game foreground admitted");
    CHECK(!nallowed("org.cocos2dx.zmxy.mi"), "v4: guest pkg NOT admitted when fg is native (no VM)");

    {
        std::string overrides =
            "org.cocos2dx.zmxy.mi\ncom.tencent.tmgp.sgame\n# a comment line\n   \n";
        std::map<std::string,long> running = {
            {"org.cocos2dx.zmxy.mi", 10240},
            {"com.tencent.tmgp.sgame", 10141},
            {"com.vmos.chat", 10240},
        };
        auto s = vm::intersectVmGuestOverrides(overrides, "com.vmos.pro", 10240, running);
        CHECK(s.size() == 1, "v5: exactly one guest admitted (real game in this VM)");
        CHECK(s.count("org.cocos2dx.zmxy.mi") == 1, "v5: zmxy admitted");
        CHECK(s.count("com.tencent.tmgp.sgame") == 0, "v5: sgame (wrong uid) rejected");
        CHECK(s.count("com.vmos.chat") == 0, "v5: non-game same-uid app rejected");
    }
    {
        std::map<std::string,long> running = {{"com.a", 10240}};
        auto s = vm::intersectVmGuestOverrides("", "com.vmos.pro", 10240, running);
        CHECK(s.empty(), "v5b: empty overrides -> empty set");
    }
    {
        std::map<std::string,long> running = {{"com.vmos.pro", 10240}};
        auto s = vm::intersectVmGuestOverrides("com.vmos.pro\n", "com.vmos.pro", 10240, running);
        CHECK(s.empty(), "v5c: container pkg itself not in guest set");
    }
    {
        std::map<std::string,long> running;
        auto s = vm::intersectVmGuestOverrides("com.a\ncom.b\n", "com.vmos.pro", 10240, running);
        CHECK(s.empty(), "v5d: no running override -> empty set");
    }

    vm::setVmGuestPnamesForTest({});

    if (failures == 0) printf("==> VM guest resolution PASS ==\n");
    else printf("==> VM guest resolution FAIL (%d) ==\n", failures);
    return failures ? 1 : 0;
}

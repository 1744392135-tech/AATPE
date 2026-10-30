
#include <string>
#include <iostream>
#include <cassert>
#include <fstream>
#include <filesystem>

#include "aatpe_pid_identity.h"

static const char* kExpected = "/system/bin/aatped";

static bool exeIdentityOk(const std::string& liveExe,
                          const std::string& publishedExe,
                          const std::string& resolvedExpected) {
    if (!publishedExe.empty()) {
        return liveExe == publishedExe;
    }
    return liveExe == kExpected || liveExe == resolvedExpected;
}

int main() {
    std::string moduleExe = "/data/adb/modules/aatpe/system/bin/aatped";
    std::string resolved   = "/system/bin/aatped";

    assert(exeIdentityOk(moduleExe, moduleExe, resolved) == true);
    std::cout << "[id] module liveExe == published daemon_exe -> PASS\n";

    assert(exeIdentityOk("/system/bin/aatped", "", resolved) == true);
    std::cout << "[id] bootstrap /system/bin/aatped in old set -> PASS\n";

    assert(exeIdentityOk("/system/bin/other", moduleExe, resolved) == false);
    std::cout << "[id] recycled PID liveExe != published -> FAIL (rejected)\n";

    assert(exeIdentityOk("/data/x/aatped_fake", "", resolved) == false);
    std::cout << "[id] aatped_fake not in old set -> FAIL (rejected)\n";

    assert(exeIdentityOk("/data/adb/modules/aatpe/system/bin/aatped_fake", moduleExe, resolved) == false);
    std::cout << "[id] published-match but trailing _fake -> FAIL (rejected)\n";

    using namespace aatpe::pid_identity;
    assert(std::string(stateDir()) == "/dev/aatpe/state");
    std::cout << "[sec05] default stateDir() == /dev/aatpe/state -> PASS\n";

    std::string tmp = (std::filesystem::temp_directory_path() /
                       ("aatpe_pid_" + std::to_string(::getpid()))).string();
    std::filesystem::create_directories(tmp);
    setStateDir(tmp.c_str());
    assert(std::string(stateDir()) == tmp);
    {
        std::ofstream(tmp + "/daemon_starttime") << "123456";
        std::ofstream(tmp + "/daemon_exe") << "/data/adb/modules/aatpe/system/bin/aatped\n";
    }
    assert(readPublishedStarttime() == 123456LL);
    std::cout << "[sec05] readPublishedStarttime() reads from overridden state dir -> PASS\n";
    assert(readPublishedExe() == "/data/adb/modules/aatpe/system/bin/aatped");
    std::cout << "[sec05] readPublishedExe() reads from overridden state dir -> PASS\n";

    std::filesystem::remove(tmp + "/daemon_starttime");
    assert(readPublishedStarttime() == -1);
    std::cout << "[sec05] missing starttime -> -1 (bootstrap) -> PASS\n";

    setStateDir("/dev/aatpe/state");
    std::filesystem::remove_all(tmp);

    std::cout << "ALL PASS\n";
    return 0;
}

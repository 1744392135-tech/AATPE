// test_pid_identity_exe.cpp - h6 D2 identity check #2 decision regression.
//
// Mirrors the exe-match decision inside verifyAatpedPid() (aatpe_pid_identity.h):
//   - if the live daemon PUBLISHED its own exe (daemon_exe), require EXACT byte
//     equality liveExe == publishedExe;
//   - else (bootstrap / downgraded) fall back to the old exact set
//     {"/system/bin/aatped", realpath("/system/bin/aatped")}.
//
// We model the predicate as a pure function here so the four cases the on-device
// regression required are host-exercised without needing /dev/aatpe/state writes:
//   1. liveExe = module path, daemon_exe published = module path  -> PASS
//   2. daemon_exe missing (bootstrap) -> old set applies
//   3. PID recycled: liveExe differs from publishedExe            -> FAIL
//   4. liveExe = /data/x/aatped_fake, no publishedExe           -> FAIL (old set)

#include <string>
#include <iostream>
#include <cassert>
#include <fstream>
#include <filesystem>

#include "aatpe_pid_identity.h"

static const char* kExpected = "/system/bin/aatped";

// Pure mirror of verifyAatpedPid check #2.
static bool exeIdentityOk(const std::string& liveExe,
                          const std::string& publishedExe,
                          const std::string& resolvedExpected) {
    if (!publishedExe.empty()) {
        return liveExe == publishedExe;          // exact, byte-for-byte
    }
    return liveExe == kExpected || liveExe == resolvedExpected;
}

int main() {
    std::string moduleExe = "/data/adb/modules/aatpe/system/bin/aatped";
    std::string resolved   = "/system/bin/aatped";  // plain install (no magic mount)

    // 1. on-device: live exe == published module path -> PASS
    assert(exeIdentityOk(moduleExe, moduleExe, resolved) == true);
    std::cout << "[id] module liveExe == published daemon_exe -> PASS\n";

    // 2. bootstrap: no published exe -> old exact set applies
    assert(exeIdentityOk("/system/bin/aatped", "", resolved) == true);
    std::cout << "[id] bootstrap /system/bin/aatped in old set -> PASS\n";

    // 3. PID recycled: live exe differs from published -> FAIL
    assert(exeIdentityOk("/system/bin/other", moduleExe, resolved) == false);
    std::cout << "[id] recycled PID liveExe != published -> FAIL (rejected)\n";

    // 4. spoof: /data/x/aatped_fake, no published -> old set rejects
    assert(exeIdentityOk("/data/x/aatped_fake", "", resolved) == false);
    std::cout << "[id] aatped_fake not in old set -> FAIL (rejected)\n";

    // extra: substring look-alike with published must ALSO differ byte-for-byte
    assert(exeIdentityOk("/data/adb/modules/aatpe/system/bin/aatped_fake", moduleExe, resolved) == false);
    std::cout << "[id] published-match but trailing _fake -> FAIL (rejected)\n";

    // ---- SEC-05: state dir is overridable; readers follow setStateDir() ----
    using namespace aatpe::pid_identity;
    // Default is the production path (standalone aatpe-cli / watchdog see this).
    assert(std::string(stateDir()) == "/dev/aatpe/state");
    std::cout << "[sec05] default stateDir() == /dev/aatpe/state -> PASS\n";

    // Point the reader at a throwaway dir and publish daemon_starttime/exe there.
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

    // Missing record in the overridden dir -> bootstrap sentinels.
    std::filesystem::remove(tmp + "/daemon_starttime");
    assert(readPublishedStarttime() == -1);
    std::cout << "[sec05] missing starttime -> -1 (bootstrap) -> PASS\n";

    // Restore production default so we never pollute other cases.
    setStateDir("/dev/aatpe/state");
    std::filesystem::remove_all(tmp);

    std::cout << "ALL PASS\n";
    return 0;
}

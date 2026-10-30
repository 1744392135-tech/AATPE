#ifndef AATPED_APKLABEL_H
#define AATPED_APKLABEL_H

#include <cstdint>
#include <string>
#include <vector>

namespace aatpe {
namespace apklabel {

std::string readLabelFromApk(const std::string& apkPath, std::string* err = nullptr);

std::string labelForPackage(const std::string& pkg,
                            const std::string& packagesListPath = "/data/system/packages.list",
                            const std::string& appsRoot = "/data/app");

std::string peekCachedLabel(const std::string& pkg);

bool packageListHasPackage(const std::string& packagesListText, const std::string& pkg);

std::vector<std::string> baseApkCandidates(const std::string& pkg,
                                           const std::string& packagesListText,
                                           const std::string& appsRoot);

std::vector<std::string> listInstalledPackagesAllUsers(
    const std::string& packagesListPath = "/data/system/packages.list",
    const std::string& appsRoot = "/data/app",
    const std::string& dataUserRoot = "/data/user");

int userIdFromUid(long uid);

}
}

#endif

#include <string>
#include <vector>

namespace aatpe {
namespace gamelist {
__attribute__((weak)) bool addManualGame(const std::string& ) { return true; }
__attribute__((weak)) bool removeManualGame(const std::string& ) { return true; }
__attribute__((weak)) std::string currentManualGameList() { return std::string(); }
__attribute__((weak)) bool isManualGame(const std::string& ) { return false; }
}
namespace apps {
__attribute__((weak)) std::vector<std::string> listInstalledPackages() {
    return std::vector<std::string>();
}
}
}

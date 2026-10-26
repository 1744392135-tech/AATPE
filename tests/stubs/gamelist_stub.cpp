// gamelist_stub.cpp — v4.2.1 hotfix backflow: link-time stubs for the new
// aatpe::gamelist / aatpe::apps symbols that production aatped_control.cpp and
// webui_http.cpp now reference. The REAL implementations live in frame.cpp and
// are exercised end-to-end by the `game_layer` target (which links frame.o +
// frame_deps). control/webui_http are NOT game-list tests — they just need the
// symbols to resolve, so we provide minimal no-op/test-safe definitions here
// instead of dragging the whole frame.o closure (which would collide with the
// g_cfg/STATE_DIR globals already supplied by v94_daemon_deps/webui_deps).
//
// These are deliberately WEAK so that any future recipe that DOES link the real
// frame.o overrides them cleanly instead of failing on a duplicate symbol.
#include <string>
#include <vector>

namespace aatpe {
namespace gamelist {
__attribute__((weak)) bool addManualGame(const std::string& /*pkg*/) { return true; }
__attribute__((weak)) bool removeManualGame(const std::string& /*pkg*/) { return true; }
__attribute__((weak)) std::string currentManualGameList() { return std::string(); }
// W2-X: placement.cpp admission gate calls this when fg attribution is unknown.
// Weak default = "not a manual game" (fail-closed on the spoof guard); a recipe
// that links real frame.o, or a test TU providing a strong override, replaces it.
__attribute__((weak)) bool isManualGame(const std::string& /*pkg*/) { return false; }
}  // namespace gamelist
namespace apps {
__attribute__((weak)) std::vector<std::string> listInstalledPackages() {
    return std::vector<std::string>();
}
}  // namespace apps
}  // namespace aatpe

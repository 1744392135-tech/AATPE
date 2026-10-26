// scenewire.cpp — host stub for the W5 scene-infer wiring glue.
//
// The production glue (auxTickSlot3 / loadingActiveSuppressesPreboost) lives in
// scene_infer_wiring.cpp and reads the live daemon sampling globals. For the v94
// host build of aatped_main.o we cannot pull that whole sampling chain, so this
// stub provides linkable no-ops: the slow thread and the C1 LOADING suppression
// simply do not fire under host tests. The pure mapping layer (buildFeatures /
// mapSceneToGameScene / chooseRoute / shouldSuppressPreboost) is tested for real
// in the scene_wiring recipe against scene_infer_wiring.o.
#include "scene_infer_wiring.h"
#include "c4_freezer.h"

namespace aatpe {
namespace scenewire {

// Host stub: keep the legacy path selected so test_main's daemon loop runs the
// v4.4.2 detectGameScene path untouched.
SceneRoute chooseRoute(bool) {
    return SceneRoute::LEGACY_DETECTGAME;
}

// Host stub: no live sampling thread.
void auxTickSlot3(float) {}

// Host stub: never suppress C1 preboost under test.
bool loadingActiveSuppressesPreboost() { return false; }

} // namespace scenewire
} // namespace aatpe

namespace aatpe {
namespace c4freezer {
// Host stub: c4FreezerTick() glue (sysfs /proc scan + dumpsys) compiles out under
// AATPE_HOST_TEST. The switch getter/setter are inline in c4_freezer.h.
void c4FreezerTick() {}
// Host stub: production snapshot compiles out; report disabled (self-contained,
// does NOT call c4StateJson so this stub has no extra link deps).
std::string c4FreezerStateJson() {
    return "{\"enabled\":false,\"target\":\"\",\"frozen\":false,\"cgDir\":\"\","
           "\"lastAction\":\"\",\"audioActive\":false,\"freezerAvailable\":false,"
           "\"degraded\":\"disabled\"}";
}
} // namespace c4freezer
} // namespace aatpe

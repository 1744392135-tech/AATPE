#include "scene_infer_wiring.h"
#include "c4_freezer.h"

namespace aatpe {
namespace scenewire {

SceneRoute chooseRoute(bool) {
    return SceneRoute::LEGACY_DETECTGAME;
}

void auxTickSlot3(float) {}

bool loadingActiveSuppressesPreboost() { return false; }

}
}

namespace aatpe {
namespace c4freezer {
void c4FreezerTick() {}
std::string c4FreezerStateJson() {
    return "{\"enabled\":false,\"target\":\"\",\"frozen\":false,\"cgDir\":\"\","
           "\"lastAction\":\"\",\"audioActive\":false,\"freezerAvailable\":false,"
           "\"degraded\":\"disabled\"}";
}
}
}

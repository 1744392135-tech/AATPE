#ifndef AATPE_PLACEMENT_H
#define AATPE_PLACEMENT_H
#include <cstdint>
namespace aatpe {
namespace placement {
std::uint64_t spilloverPromotionCount() noexcept;
int spilloverActiveCount() noexcept;
void onInputEvent() noexcept;

void onForegroundSwitchEdge(const std::string& newFg) noexcept;
}
}
#endif

// placement.h - B3: spillover observability accessors.
// G1 calls these from the state-export path (WebUI keys prime_spillover_count /
// prime_hosted_threads). Thread-safe atomic reads; no locks.
#ifndef AATPE_PLACEMENT_H
#define AATPE_PLACEMENT_H
#include <cstdint>
namespace aatpe {
namespace placement {
// Total number of threads ever promoted to the PRIME cluster (cumulative).
std::uint64_t spilloverPromotionCount() noexcept;
// Number of threads currently hosted on PRIME (post-debounce, pre-demote).
int spilloverActiveCount() noexcept;
// ROUND-007b: event-driven immediate boost — input_monitor/cold-start calls this
// the moment a touch/launch fires (no 200ms tick wait). Short-lived thread-level
// boost; auto-releases after the short dwell. No global min_freq change.
void onInputEvent() noexcept;

// ROUND-015: foreground-switch edge hook. Called by loopForegroundTick the moment
// the foreground package changes. Two jobs:
//   (1) refresh the placement-side foreground cache with the freshly-detected
//       package so the very next spillover tick does NOT act on the stale 3s-TTL
//       value (the two-layer fg-cache decoupling that caused the fast
//       away-and-back stuck bug);
//   (2) arm an unconditional re-apply sweep consumed by the next spillover tick:
//       every promoted thread of `newFg` is re-pinned into the boosted cgroup
//       (top-app), bypassing kStableCycles debounce and the promoted==true short
//       circuit, and the kernel's real cgroup membership is read back to heal any
//       drift AMS caused while the app briefly left the foreground.
void onForegroundSwitchEdge(const std::string& newFg) noexcept;
}  // namespace placement
}  // namespace aatpe
#endif  // AATPE_PLACEMENT_H

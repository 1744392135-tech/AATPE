// AATPE CPU/GPU joint scheduling — shared schedtune boost bridge (P2-5 / P1-5)
//
// The existing aatped_gpu layer already probes KGSL/Mali/devfreq GPU freq and
// busy% into g_gpuUtilization. On MTK the big-CPU DVFS governor does not see
// GPU pressure, so a GPU-pegged game can starve its CPU-side render/upload
// threads. This module owns the SINGLE writer to the schedtune.boost node so
// that TWO independent producers do not clobber each other:
//
//   - the touch-boost path (input_monitor.cpp)  -> aatpeBoostSet("touch", 80)
//   - the GPU-pegged path   (gpuBoostTick)      -> aatpeBoostSet("gpu",   60)
//
// Internally it keeps per-source levels and writes max(touch, gpu) to the node.
// When either source clears its level, the other's level (if any) remains
// applied — previously touch and GPU each wrote the node directly, so the GPU
// dwell-release wrote 0 and silently killed an active touch boost (and vice
// versa).
//
// Best-effort: no boost node on the system => silent no-op.

#ifndef AATPE_GPU_BOOST_H
#define AATPE_GPU_BOOST_H

#include <string>

namespace aatpe {

// ---- Shared boost bridge (P1-5) ------------------------------------------
// Set/release a named producer's boost level (0..100). The bridge recomputes
// max() across all active producers and writes it to the schedtune.boost node.
// `source` is an arbitrary short label ("touch", "gpu"); unknown sources are
// tracked independently. Thread-safe.
void aatpeBoostSet(const char* source, int level);
void aatpeBoostClear(const char* source);

// ---- GPU-pegged dwell tick (P2-5) ----------------------------------------
// Call every decision cycle. Dwells a boost while GPU is pegged in-game,
// releases after GPU load drops (via aatpeBoostClear("gpu")).
void gpuBoostTick();

bool gpuBoostActive();
void aatpeGpuBoostForceZero(const char* reason);   // ROUND-014 P3

#ifdef AATPE_GPU_BOOST_TEST
// Test seam for the re-probe state machine (ROUND-002 audit fix). Not for
// production use.
namespace boost_test {
void resetForTest();
void tickForTest(long long nowMs, bool(*writable)(const char*));
std::string nodeForTest();
int consecFailsForTest();
// ROUND-014 P3: devfreq test seam.
void devfreqResetForTest();
void devfreqSetNodeForTest(const std::string& path);
void devfreqBoostOnForTest();
void devfreqBoostOffForTest();
bool devfreqBoostedForTest();
unsigned long devfreqSavedMinForTest();
std::string devfreqNodeForTest();
int devfreqOffFailsForTest();
std::string devfreqMarkerPathForTest();
// T-08 hysteresis test seams.
bool boostEngagedForTest();
void boostResetHysteresisForTest();
long long boostEngagedMsForTest();
}
#endif

}  // namespace aatpe

#endif  // AATPE_GPU_BOOST_H

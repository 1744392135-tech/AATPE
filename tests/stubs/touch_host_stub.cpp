// touch_host_stub.cpp - v4.3.6 host-only stubs for the touch per-thread uclamp wiring.
//
// The v94/san host unit-test link graph does NOT include the production daemon
// thread table (g_threads, owned by thread_scan.cpp) nor the device sched_setattr
// path. These stubs let input_monitor.o link on host while preserving the exact
// fail-safe semantics the production code relies on:
//
//   * touchUclampCollectForegroundThreads() -> returns 0 (no foreground threads).
//     With an empty thread set the verified touch_uclamp controller emits NO
//     per-thread uclamp action and only holds the low global schedtune floor --
//     identical to "unknown foreground" on device. The production daemon links
//     the REAL definition in touch_uclamp_fgscan.cpp (reads g_threads).
//
//   * aatpe_mech::aatpeUclampSetMin() -> no-op returning 0. On host there is no
//     sched_setattr/uclamp sysfs node; the production aatpe_placement_mech.o
//     provides the real write on device.
//
// This file is ONLY added to host recipes that link input_monitor.o (the `main`
// recipe). It is never compiled into the device daemon.

#include <string>
#include <cstdint>

#include "touch_uclamp.h"        // FgThread
#include "aatpe_placement_mech.h" // aatpe_mech::aatpeUclampSetMin

namespace aatpe {

uint32_t touchUclampCollectForegroundThreads(const std::string& fgPackage,
                                            FgThread* out, uint32_t cap) {
    (void)fgPackage;
    (void)out;
    (void)cap;
    return 0u;  // host: empty foreground set -> controller holds only the low floor
}

}  // namespace aatpe

namespace aatpe_mech {

// v4.3.9: weak so the real aatpe_placement_mech definition (which routes the
// per-tid source ledger) wins when that object is linked.
__attribute__((weak)) int aatpeUclampSetMin(int tid, unsigned int utilMin) {
    (void)tid;
    (void)utilMin;
    return 0;  // host no-op (no sched_setattr on device node)
}

}  // namespace aatpe_mech

// AATPE v4.3.6 - PSI (Pressure Stall Information) decision closed-loop wiring.
//
// This layer is the ONLY consumer of the pure-logic core psi_decision.{h,c}.
// It owns the "group-level uclamp / overload driver" side of the closed loop:
//
//   diag snapshot (already collected by aatpe_diag) + frame timing
//        -> psi_decide()                        (pure logic, untouched)
//        -> actuator:
//             SEVERE ........ pause non-critical thread migration (placement seam)
//             downgrade_background .. lower the "background" cgroup cpu.uclamp.min
//             MODERATE/AGGRESSIVE ... raise the "top-app" cgroup cpu.uclamp.min
//             io_pressure .......... reuse coldStartSetIoPressure()
//             mem pressure ......... de-prefer big-core placement (placement seam)
//             reasons[] ............ written to STATE_DIR for WebUI/diag
//
// Boundaries (hard contract):
//   * Reuses the verified core BY VALUE; never edits its decision logic.
//   * Reuses the existing diag collection; adds NO new /proc/pressure path.
//     The core's io_full_avg10 / mem_full_avg10 inputs are wired 0 (not exposed
//     by the current collector), so the core degrades to the "some" line --
//     exactly what production already keys off (psiIoSomePct > 20).
//   * Writes ONLY GROUP-level cgroup uclamp.min nodes. Per-thread uclamp
//     (sched_setattr / writeCgroupUclampMin) belongs to the touch route and is
//     NOT touched here.
//   * Fail-safe: PSI unavailable (/proc/pressure missing) or a non-finite sample
//     => release every actuator, no node write, no decision injected.
#ifndef AATPED_PSI_H
#define AATPED_PSI_H

#include <atomic>

namespace aatpe {
namespace psi {

// One-time init. Installs default PsiConfig and initialises the persistent
// decision state. Safe to call again (idempotent).
void init();

// Call once per main-loop iteration. Cheap no-op when the diag PSI sample has
// not advanced since the previous decision (so it is safe to call at the full
// ~33ms loop cadence; the actual closed-loop runs at the ~1-2s sample cadence).
// On a new sample: fill PsiSample, run psi_decide, and apply/release actuators.
void tick();

// --- placement seams (lock-free; called from placement.cpp) ---------------
// SEVERE runqueue stall: caller must NOT initiate NEW thread migration churn
// (affinity/cgroup moves) for non-critical work. Already-promoted threads stay
// put (no demotion flap).
bool noncriticalMigrationPaused() noexcept;
// Memory reclaim pressure: de-weight big-core placement preference (big-core
// migration is expensive under reclaim).
bool bigcoreDeprefer() noexcept;

// Test seam: drop persistent decision state AND release every applied actuator
// (restores saved group uclamp originals). Host tests use this between cases.
#ifdef AATPE_HOST_TEST  // RC7(C6): psi test seam, no production caller
void resetForTest();
#endif

// audit4 F3: release every applied group-uclamp actuator AND clear the placement
// seams. Called from the daemon shutdown sequence (after spillover/gpu teardown,
// before restoreAllCgroupUclamp) so a clean shutdown or watchdog-killed exit never
// leaves the PSI group-uclamp cut/boost frozen on the cgroup node. Idempotent.
void shutdown();

}  // namespace psi
}  // namespace aatpe

#endif  // AATPED_PSI_H

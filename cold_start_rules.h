// AATPE Game Cold-Start Preset Rules (P1-7 / P1-13/P1-14)
//
// During ML cold-start the decision quality is low, so the first seconds after
// a game comes to the foreground are scheduling chaos. This module applies a
// deterministic preset the moment the foreground switches to a known game
// package, and re-asserts it for a short grace window until the hybrid ML
// (alpha) is confident enough to take over.
//
// Preset (applied via the existing aatpeSetThreadPriority, which uses
// SCHED_FIFO + uclamp and degrades to nice on EPERM):
//   RenderThread            -> ROLE_S2_RENDER level 3  (big-core class, RT)
//   main thread (tid==pid)  -> ROLE_S3_GAME   level 3  (game loop, RT)
//   Audio* thread           -> ROLE_S1_UI     level 2  (real-time audio)
//
// Affinity-to-big-core itself stays in placement.cpp (untouched); this module
// only drives the priority/uclamp dimension, which is the reliable lever on
// MTK 4.14.

#ifndef AATPE_COLD_START_RULES_H
#define AATPE_COLD_START_RULES_H

#include <string>

namespace aatpe {

// Call from the foreground-app switch hook. If `pkg` is a known game, arm the
// cold-start preset window. If it is a non-game, disarm.
void coldStartOnForegroundSwitch(const std::string& pkg);

// Call every decision cycle. While the window is open, re-assert presets on the
// game's live threads (threads spawned after the switch need catching).
void coldStartTick();

// ROUND-014 P3 (S14-07): main loop sets this from G6 thermal HAL. true = thermal
// SEVERE+ -> skip RT presets this tick (resume after it clears); does not disarm.
void coldStartSetThermalFreeze(bool f);
// ROUND-014 P3: main loop sets from PSI io. true = IO-bound launch -> downgrade presets.
void coldStartSetIoPressure(bool f);
// ROUND-014 P3: pure parse of dumpsys activity output -> app already Displayed.
bool coldStartDisplayedPresent(const std::string& dumpsysOut);

#ifdef AATPE_HOST_TEST
// F-8 clock injection (host only). Pass nullptr to restore the real clock.
void coldStartSetNowForTest(long long (*fn)());
// F-11 host seams. nullptr restores real frame/dumpsys behaviour.
void coldStartSetJankForTest(bool (*fn)());
void coldStartSetDisplayedForTest(std::string (*fn)());
// F-8 (v4.2): cumulative cold-start arm refusals by the per-package frequency cap.
long long coldStartRejectedCountForTest();
// F3.1 (v4.3.8): process-instance identity injection (host only). Sets the live
// process identity {pid, start_time} that resolveProcIdentity() reports for
// `pkg` instead of scanning /proc. Pass pid<=0 to mark the process as gone.
// The first call switches resolve off /proc into the test-override map; the
// cold-vs-warm classification is otherwise identical to production.
void coldStartSetProcIdentityForTest(const std::string& pkg, int pid,
                                     unsigned long long start_time);
// F3.1 (v4.3.8): observability — true while the CURRENTLY armed window is a full
// cold boost (false => short warm-resume boost). Locked read.
bool coldStartIsColdForTest();
#endif

// Diagnostics.
bool coldStartActive();
// P1-13: returns a std::string BY VALUE (not a const char* into an internal
// buffer). The old `return s_coldPkg.c_str()` handed out a pointer into a
// mutable static that another thread could overwrite (or clear) before the
// caller used it. By-value is the safe contract.
std::string coldStartPkg();

}  // namespace aatpe

#endif  // AATPE_COLD_START_RULES_H

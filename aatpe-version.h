// AATPE Version Header
// Single source of truth for version strings and codes.
// 版本与配置一致性综合修复 —
//   1) 版本号碎片化统一(原为 v93-thread-limit-fix / final-fix104 / v74-kprobe / batch2-fix5 四套并存)
//   2) ENABLE_HTTP_SERVER 注释与值矛盾修正
//   3) 根目录 thermal.conf 补齐 TARGET_FRAME_TIME=16667
//   4) thread_roles.conf 删除4条完全重复规则(GameThread/Physics/AI/AudioWorker)
//   5) battery_aging 测试默认值与 aatpe.conf(BATTERY_AGING_PROTECTION=0)对齐为 false
//   6) MIN_SAMPLES_FOR_LEARNING 统一为 HISTORY_MIN_SAMPLES(含 aatped.cpp 解析key同步)
//   7) *:binder* 模糊匹配拆分为 *:Binder_* 与 *:binder:*
//   8) *:kworker* 优先级 99→30(isKernelThread 已排除)
//   9) customize.sh 对 sched_events.bpf.o 改为存在性条件判断
// P2 Problem12: thermal_runtime latency/filter params (kCacheTtlMs,
//   kEmergencyBootGraceSec, kEmergencyDebounceReads, kMaxTempJumpC/CGame,
//   kThermalProfileRestoreHysteresis, thermal::kEmergencyHysteresis,
//   kEmergencyRecoverySec) are centralized in aatpe-constants.h but their values
//   are UNVALIDATED on real devices. They require on-device measurement before
//   shipping to non-reference hardware; do not tune them from host.
//
// ---------------------------------------------------------------------------
// DEVELOPMENT-PRIORITY STATEMENT (no code/logic change).
// The project (~139 files, daemon/eBPF/ML/thermal/frame/placement/WebUI) has
// reached a stage where CORRECTNESS and STABILITY outrank new features.
//   1. Current phase prioritizes stabilizing the CORE EVENT PATH:
//        eBPF tracepoint -> ring buffer -> thread library -> placement -> sched.
//      Any change to this path is high-risk and gated by rule (3).
//   2. A new feature MUST have test coverage FIRST (tests/v94/) before it lands;
//      no "ship now, test later".
//   3. Core-path changes MUST be validated on a real arm64 Android 10+ device
//      (kernel 4.14.x baseline) — host-only static review is not
//      sufficient for eBPF offsets, SELinux denials, or scheduler behavior.
//   4. Default posture for this phase: converge, harden, document — do not grow.
//      Architectural notes live in docs/ARCHITECTURE.md; single-source-of-truth
//      layout in docs/SOURCE_OF_TRUTH.md.
// ---------------------------------------------------------------------------
#ifndef AATPE_VERSION_H
#define AATPE_VERSION_H
// version is single-sourced from ./VERSION (item12/item23).
// module.prop / WebUI footer / APK versionName all mirror these two macros.
#define AATPE_VERSION     "v6.0.0"
#define AATPE_VERSION_CODE 4413
// round6: unique build id + build date (single-sourced from ./VERSION). NOT a
// version bump — lets the panel/state distinguish rel7..rel13 visually.
#define AATPE_RELEASE_ID  "rel19"
#define AATPE_BUILD_DATE  "2026-10-29"
// P30: build toolchain identity (mirrors Makefile / module.prop description).
#define AATPE_TOOLCHAIN   "NDK r30 / Clang 21.0.0 / android29 / lld"
#endif // AATPE_VERSION_H

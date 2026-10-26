// aatpe_sched_abi.h — unified scheduler ABI for per-task uclamp (ROUND-013).
// Uses kernel UAPI when available; otherwise a complete, correctly-laid-out
// fallback struct. The fallback MUST be 56 bytes on 64-bit and match UAPI
// field order exactly (incl. sched_period, which was missing in the old private
// copy and shifted util_min).
#pragma once
#include <cstddef>
#include <cstdint>

// On Android (NDK), use the kernel UAPI header. On host (glibc), the system
// <linux/sched/types.h> has a redefinition conflict with <sched.h> (both
// define sched_param), so we use our own fallback which matches UAPI exactly.
#if defined(__ANDROID__) && defined(__has_include)
#  if __has_include(<linux/sched/types.h>)
#    include <linux/sched/types.h>
#    define AATPE_HAVE_UAPI_SCHED_ATTR 1
#  endif
#endif

#ifndef AATPE_HAVE_UAPI_SCHED_ATTR
// UAPI-equivalent fallback (kernel sched/types.h). Layout (64-bit):
//   u32 size; s32 policy; u64 flags; s32 nice; u32 priority;
//   u64 runtime; u64 deadline; u64 period; u32 util_min; u32 util_max;
struct sched_attr {
    std::uint32_t size;
    std::uint32_t sched_policy;
    std::uint64_t sched_flags;
    std::int32_t  sched_nice;
    std::uint32_t sched_priority;
    std::uint64_t sched_runtime;
    std::uint64_t sched_deadline;
    std::uint64_t sched_period;
    std::uint32_t sched_util_min;
    std::uint32_t sched_util_max;
};
#endif

// Correct SCHED_FLAG_* values (UAPI linux/sched.h).
#ifndef SCHED_FLAG_KEEP_POLICY
#  define SCHED_FLAG_KEEP_POLICY   0x08
#endif
#ifndef SCHED_FLAG_KEEP_PARAMS
#  define SCHED_FLAG_KEEP_PARAMS    0x10
#endif
#ifndef SCHED_FLAG_UTIL_CLAMP_MIN
#  define SCHED_FLAG_UTIL_CLAMP_MIN 0x20
#endif
#ifndef SCHED_FLAG_UTIL_CLAMP_MAX
#  define SCHED_FLAG_UTIL_CLAMP_MAX 0x40
#endif

// Compile-time layout guards (64-bit).
static_assert(sizeof(struct sched_attr) == 56, "sched_attr must be 56 bytes on 64-bit");
static_assert(offsetof(struct sched_attr, sched_util_min) == 48, "util_min offset must be 48");

// ROUND-W3: compile-time guard that SCHED_FLAG_* resolve to the Linux UAPI values
// (verified against NDK r30 sysroot linux/sched.h:67-70). Catches any future
// re-introduction of the shifted-bit fallback (priority.cpp previously had
// KEEP_POLICY=0x04 / KEEP_PARAMS=0x08 / UTIL_CLAMP_MIN=0x10 / MAX=0x20, one bit low,
// which silently broke uclamp on 4.14 vendor kernels).
//
// These asserts fire regardless of whether the value came from the NDK UAPI header
// or from the fallback #define below — if either source drifts, the build breaks.
static_assert(SCHED_FLAG_KEEP_POLICY    == 0x08, "SCHED_FLAG_KEEP_POLICY must be 0x08 (UAPI)");
static_assert(SCHED_FLAG_KEEP_PARAMS    == 0x10, "SCHED_FLAG_KEEP_PARAMS must be 0x10 (UAPI)");
static_assert(SCHED_FLAG_UTIL_CLAMP_MIN == 0x20, "SCHED_FLAG_UTIL_CLAMP_MIN must be 0x20 (UAPI)");
static_assert(SCHED_FLAG_UTIL_CLAMP_MAX == 0x40, "SCHED_FLAG_UTIL_CLAMP_MAX must be 0x40 (UAPI)");


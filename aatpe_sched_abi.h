#pragma once
#include <cstddef>
#include <cstdint>

#if defined(__ANDROID__) && defined(__has_include)
#  if __has_include(<linux/sched/types.h>)
#    include <linux/sched/types.h>
#    define AATPE_HAVE_UAPI_SCHED_ATTR 1
#  endif
#endif

#ifndef AATPE_HAVE_UAPI_SCHED_ATTR
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

static_assert(sizeof(struct sched_attr) == 56, "sched_attr must be 56 bytes on 64-bit");
static_assert(offsetof(struct sched_attr, sched_util_min) == 48, "util_min offset must be 48");

static_assert(SCHED_FLAG_KEEP_POLICY    == 0x08, "SCHED_FLAG_KEEP_POLICY must be 0x08 (UAPI)");
static_assert(SCHED_FLAG_KEEP_PARAMS    == 0x10, "SCHED_FLAG_KEEP_PARAMS must be 0x10 (UAPI)");
static_assert(SCHED_FLAG_UTIL_CLAMP_MIN == 0x20, "SCHED_FLAG_UTIL_CLAMP_MIN must be 0x20 (UAPI)");
static_assert(SCHED_FLAG_UTIL_CLAMP_MAX == 0x40, "SCHED_FLAG_UTIL_CLAMP_MAX must be 0x40 (UAPI)");


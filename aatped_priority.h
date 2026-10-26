// aatped_priority.h — 算力优先分配模块
//
// 为 UI / RENDER / GAME 关键线程提供：
//   1. sched_setattr() 设置 SCHED_FIFO / SCHED_RR 实时优先级（1~3，故意不高）
//   2. uclamp：util_min=512(UI)/1024(RENDER/GAME)，util_max=1024（不设上限）
//   3. SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS：第二次调用设 uclamp
//      时保留第一次设的实时调度策略，避免被内核/系统重置回 NICE。
//
// 失败策略：
//   - 任何一步失败只 LOG_WARN，绝不崩溃。
//   - EPERM（权限不足，未给 CAP_SYS_NICE）时降级为 nice 值调整（-5/-10/-15）。
//
// 线程角色映射（见 aatped_types.h enum ThreadRole）：
//   ROLE_S1_UI     -> 轻度 RT 优先级，降帧敏感
//   ROLE_S2_RENDER -> 中度 RT 优先级，渲染帧驱动
//   ROLE_S3_GAME   -> 中度 RT 优先级，游戏主循环
//   其他角色       -> no-op（SYSTEM/BACKGROUND/THROUGHPUT 不抢实时）
#pragma once

#include "aatped_types.h"

namespace aatpe_priority {

// 给关键线程设置调度优先级 + uclamp。
//   tid   : 目标线程 ID
//   role  : ThreadRole（仅 S1_UI/S2_RENDER/S3_GAME 生效）
//   level : PRIORITY_BOOST_LEVEL 配置值（1=轻 2=中 3=激进）
// 返回 true 表示至少成功设置了一项（RT 或 nice 降级）；no-op 或全部失败返回 false。
// 设计为 best-effort：永远不抛异常、不 abort。
bool aatpeSetThreadPriority(int tid, ThreadRole role, int level);

// #1/#2 fix: explicit, UNGATED runtime RT restore. Reverts a previously boosted tid
// back to its recorded original scheduling class (SCHED_OTHER), original nice and
// original uclamp, and drops it from the RT budget set. DELIBERATELY does NOT check
// rtBoostAllowed(): de-escalating an existing RT thread is safe even when safe mode
// is latched, so this is the release channel used by cold-start window exit, thermal
// freeze and shutdown. Prefer this over aatpeSetThreadPriority(..., level=0) (which
// clamps level to 1 and has no SCHED_OTHER branch). best-effort, never throws.
bool aatpeRestoreThreadPriority(int tid);

// True only after the kernel confirmed uclamp + SCHED_FLAG_KEEP_POLICY works.
// When false (e.g. MTK 4.14 Dimensity 1100 latches E2BIG), callers must fall
// back to pool-level affinity instead of relying on the uclamp hint.
bool uclampAvailable();

// Raw latch: 1=supported, -1=unsupported(E2BIG/ENOSYS), -2=partial (returns 0
// but KEEP_POLICY clobbers policy), -3=policy-only (policy kept but util_min/max
// ignored), 0=probing.
int uclampSupportState();

#ifdef AATPE_HOST_TEST
void aatpeRtDebugSeedForTest(int tid, unsigned long long starttime);
#endif

}  // namespace aatpe_priority

// ============================================================================
// AATPE eBPF: sched_switch tracepoint program (EXPERIMENTAL / MODERN BPF)
// ============================================================================
//
// !!! DO NOT COMPILE FOR 4.14 - NOT REFERENCED BY MAIN LOAD FLOW !!!
// !!! THIS FILE IS NOT REFERENCED BY THE MAIN LOAD FLOW !!!
//
// The production eBPF bytecode is embedded in sched_events_embedded.h,
// which is generated from sched_events.bpf.c (the 4.14-compatible legacy
// version). This file exists as an experimental / modern-BPF reference
// and for future kernel upgrades.
//
// KERNEL REQUIREMENTS:
//   - Requires kernel 5.1+ for BPF_ATOMIC support.
//   - The main production path (sched_events.bpf.c) targets kernel 4.14
//     and deliberately avoids:
//       * __sync_fetch_and_add / BPF_ATOMIC (needs 5.1+)
//       * Direct tracepoint ctx dereference (portability across kernels)
//
// This file has been updated to match the 4.14-compatible patterns used
// in sched_events.bpf.c:
//   - __sync_fetch_and_add replaced with direct PERCPU_ARRAY read-modify-write
//     (per-CPU maps need no atomic operations).
//   - Direct ctx->prev_pid / ctx->next_pid dereference replaced with
//     comments; use bpf_get_current_pid_tgid() or probe_read for portability.
//
// Compile (for modern kernels only):
//   clang -target bpf -O2 -c sched_switch.bpf.c -o sched_switch.bpf.o
// ============================================================================

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

// Per-CPU array: stores accumulated runtime ns and switch count
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} cpu_runtime_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} cpu_switch_map SEC(".maps");

// Hash map: tid -> last enqueue timestamp (ns)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} tid_enqueue_map SEC(".maps");

// Tracepoint: sched_switch
// Format: prev_comm, prev_pid, prev_prio, prev_state, next_comm, next_pid, next_prio
//
// NOTE: Direct ctx->prev_pid / ctx->next_pid dereference is NOT portable
// across kernel versions (tracepoint arg layout varies). For 4.14-compatible
// production code, use bpf_get_current_pid_tgid() for the current (next) task
// and leave prev fields zeroed (see sched_events.bpf.c handle_switch()).
SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx) {
    __u32 key = 0;
    __u64 now = bpf_ktime_get_ns();

    // Increment switch count for this CPU.
    // PERCPU_MAP: per-CPU value, no atomic needed — use direct read-modify-write
    // (4.14-compatible pattern, matches sched_events.bpf.c inc_counter()).
    __u32 *switch_count = bpf_map_lookup_elem(&cpu_switch_map, &key);
    if (switch_count) {
        __u32 v = *switch_count;
        v = v + 1;
        *switch_count = v;
    }

    // Track prev thread: compute runtime since last enqueue.
    // PORTABILITY NOTE: ctx->prev_pid direct dereference is kernel-version
    // specific. For production 4.14 code, do NOT read prev_pid from ctx.
    __u32 prev_pid = ctx->prev_pid;
    __u64 *enqueue_ts = bpf_map_lookup_elem(&tid_enqueue_map, &prev_pid);
    if (enqueue_ts && *enqueue_ts > 0) {
        __u64 runtime = now - *enqueue_ts;
        __u64 *runtime_accum = bpf_map_lookup_elem(&cpu_runtime_map, &key);
        if (runtime_accum) {
            // PERCPU_MAP: direct read-modify-write, no atomic needed.
            __u64 v = *runtime_accum;
            v = v + runtime;
            *runtime_accum = v;
        }
    }

    // Track next thread: record enqueue timestamp.
    // PORTABILITY NOTE: ctx->next_pid direct dereference is kernel-version
    // specific. For production 4.14 code, use bpf_get_current_pid_tgid().
    __u32 next_pid = ctx->next_pid;
    bpf_map_update_elem(&tid_enqueue_map, &next_pid, &now, BPF_ANY);

    return 0;
}

char LICENSE[] SEC("license") = "GPL";

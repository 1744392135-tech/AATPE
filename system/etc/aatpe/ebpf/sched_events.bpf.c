// AATPE eBPF: TRACEPOINT-based scheduler event monitor
// Tracks thread create/destroy, CPU scheduling, and process exec via tracepoints.
// Events are pushed to user-space through a PERF_EVENT_ARRAY ring buffer.
//
// Compile: clang -target bpf -O2 -g -c sched_events.bpf.c -o sched_events.bpf.o
//
// This file is FULLY self-contained: no libbpf / kernel headers needed.
// All types and constants are defined inline for maximum portability.
//
// v82 PERF-OUTPUT FIX:
//   - Added perf_output_safe() wrapper that checks bpf_perf_event_output()
//     return value and increments COUNTER_PERF_OUTPUT_FAIL on failure.
//   - Added COUNTER_PERF_OUTPUT_FAIL (index 5) to event_counters map
//     (max_entries increased from 5 to 6).
//   - All 4 bpf_perf_event_output call sites now use perf_output_safe().
//   - User-space loader now uses PERF_TYPE_SOFTWARE / PERF_COUNT_SW_BPF_OUTPUT
//     events for ring buffers (not PERF_TYPE_TRACEPOINT), which is the standard
//     design for BPF output and works correctly on 4.14 kernels.
//
// TRACEPOINT attach points (user-space reads tracepoint ID from tracefs):
//   tracepoint/sched/sched_process_fork   -> child process/thread created
//   tracepoint/sched/sched_process_exit   -> task exiting
//   tracepoint/sched/sched_switch         -> context switch
//   tracepoint/sched/sched_process_exec   -> process exec
//
// Program type: BPF_PROG_TYPE_TRACEPOINT (type=5)
//
// KERNEL 4.14 COMPATIBILITY NOTES (CRITICAL):
//   - DO NOT use 32-bit signed comparisons (generates BPF_JMP32 class=6
//     instructions like 0xc6 = BPF_JSLE|BPF_K|BPF_JMP32, which require
//     kernel 4.20+). Use only 64-bit unsigned comparisons (BPF_JGT/BPF_JGE
//     with BPF_JMP class=5).
//   - DO NOT use __sync_fetch_and_add or atomics (generates BPF_ATOMIC,
//     requires kernel 5.1+). Use PERCPU_ARRAY for counters — per-CPU values
//     need no atomic operations.
//   - DO NOT dereference tracepoint context (ctx) directly. The tracepoint
//     args structure varies by kernel. Use only bpf_get_current_* helpers.
//   - bpf_perf_event_output() flags must be BPF_F_CURRENT_CPU (0xFFFFFFFF),
//     not OR'd with a CPU number.
//   - License must be "GPL" to use bpf_perf_event_output and other helpers.
//   - Map references are LD_IMM (0x18) with src_reg=0; the user-space loader
//     patches the immediate field at explicit relocation offsets with the map FD.

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef int __s32;

// ---------------------------------------------------------------------------
// BPF constants
// ---------------------------------------------------------------------------
#define BPF_MAP_TYPE_UNSPEC           0
#define BPF_MAP_TYPE_HASH             1
#define BPF_MAP_TYPE_ARRAY            2
/* v4.5.0 F-03: LRU_HASH (type 9 — verified against kernel UAPI
 * <linux/bpf.h>: UNSPEC=0,HASH=1,ARRAY=2,PROG_ARRAY=3,PERF_EVENT_ARRAY=4,
 * PERCPU_HASH=5,PERCPU_ARRAY=6,STACK_TRACE=7,CGROUP_ARRAY=8,LRU_HASH=9,
 * LRU_PERCPU_HASH=10). Available since kernel 4.10; the module targets
 * Android 10+ (kernel 4.14+), so this is always satisfied. The kernel evicts
 * the least-recently-used entry automatically when the map nears max_entries.
 * This is the in-kernel TTL reaper: a task that was woken but never scheduled
 * on a CPU and never exits leaves a cold wakeup_rec; plain HASH would hold it
 * forever (until exit), and a 4.14 BPF program cannot bpf_for_each_map_elem
 * (needs 5.13) nor bpf_timer (5.15) to sweep. With LRU_HASH the orphaned
 * (cold) entry is evicted before live pending wakeups (warm). The userspace
 * loader still creates this map with a plain-HASH(1) fallback on kernels that
 * reject the type, so behavior degrades to the v4.4.9 bounded-HASH+ENOSPC-count
 * rather than failing load. Do NOT use 3 here: 3 is PROG_ARRAY, not LRU_HASH. */
#define BPF_MAP_TYPE_LRU_HASH         9
#define BPF_MAP_TYPE_PERCPU_ARRAY     6
#define BPF_MAP_TYPE_PERF_EVENT_ARRAY 4

#define BPF_FUNC_map_lookup_elem      1
#define BPF_FUNC_map_update_elem      2
#define BPF_FUNC_map_delete_elem      3
#define BPF_FUNC_probe_read           4
#define BPF_FUNC_ktime_get_ns         5
#define BPF_FUNC_get_smp_processor_id 8
#define BPF_FUNC_get_current_pid_tgid 14
#define BPF_FUNC_get_current_comm     16
#define BPF_FUNC_perf_event_output    25

#define BPF_F_CURRENT_CPU             0xFFFFFFFFULL

// ---------------------------------------------------------------------------
// Section macro
// ---------------------------------------------------------------------------
#define SEC(NAME) __attribute__((section(NAME), used))

// ---------------------------------------------------------------------------
// BPF helper function stubs (resolved by kernel verifier at load time)
// ---------------------------------------------------------------------------
static __u64 (*bpf_map_lookup_elem)(void *map, const void *key) =
    (void *)BPF_FUNC_map_lookup_elem;
static __u64 (*bpf_map_update_elem)(void *map, const void *key, const void *value, __u64 flags) =
    (void *)BPF_FUNC_map_update_elem;
static __u64 (*bpf_map_delete_elem)(void *map, const void *key) =
    (void *)BPF_FUNC_map_delete_elem;
static __u64 (*bpf_probe_read)(void *dst, __u64 size, const void *unsafe_ptr) =
    (void *)BPF_FUNC_probe_read;
static __u64 (*bpf_perf_event_output)(void *ctx, void *map, __u64 flags, void *data, __u64 size) =
    (void *)BPF_FUNC_perf_event_output;
static __u64 (*bpf_ktime_get_ns)(void) =
    (void *)BPF_FUNC_ktime_get_ns;
static __u64 (*bpf_get_smp_processor_id)(void) =
    (void *)BPF_FUNC_get_smp_processor_id;
static __u64 (*bpf_get_current_pid_tgid)(void) =
    (void *)BPF_FUNC_get_current_pid_tgid;
static __u64 (*bpf_get_current_comm)(void *buf, __u64 size) =
    (void *)BPF_FUNC_get_current_comm;

// ---------------------------------------------------------------------------
// Map definition (legacy bpf_map_def format — widest kernel compatibility)
// ---------------------------------------------------------------------------
struct bpf_map_def {
    __u32 type;
    __u32 key_size;
    __u32 value_size;
    __u32 max_entries;
    __u32 map_flags;
};

// ---------------------------------------------------------------------------
// Event types sent to user-space (must match aatpe_ebpf.h)
// ---------------------------------------------------------------------------
#define AATPE_EVT_FORK   1
#define AATPE_EVT_EXIT   2
#define AATPE_EVT_SWITCH 3
#define AATPE_EVT_EXEC   4
#define AATPE_EVT_RUNQ   5   /* P1-1: runqueue (wakeup->run) latency sample */

struct aatpe_event_header {
    __u32 type;
    __u32 cpu;
    __u64 timestamp_ns;
};

struct aatpe_fork_event {
    struct aatpe_event_header hdr;
    __u32 parent_pid;
    __u32 child_pid;
    __u32 child_tid;
};

struct aatpe_exit_event {
    struct aatpe_event_header hdr;
    __u32 pid;
    __u32 tid;
};

struct aatpe_switch_event {
    struct aatpe_event_header hdr;
    __u32 prev_pid;
    __u32 next_pid;
    char prev_comm[16];
    char next_comm[16];
};

struct aatpe_exec_event {
    struct aatpe_event_header hdr;
    __u32 pid;
    __u32 tid;
    char comm[16];
};

/* P1-1: runqueue latency = ktime between sched_wakeup(pid) and the moment
 * the kernel actually picks `pid` as next on a CPU. Emitted only when the
 * woken task is scheduled in; long samples are full-output, short samples are
 * 1/8-sampled by (cpu&7).
 * P2-4: waker_tid records the thread that issued the wakeup (1-hop wakeup
 * dependency edge A->B), so user-space can build the wakeup dependency graph. */
struct aatpe_runq_event {
    struct aatpe_event_header hdr;
    __u32 pid;
    __u32 target_cpu;
    __u32 waker_tid;
    __u32 pad;
    __u64 delay_ns;
};

// ---------------------------------------------------------------------------
// eBPF Maps
// ---------------------------------------------------------------------------

// PERF_EVENT_ARRAY: ring buffer for sending events to user-space
struct bpf_map_def SEC("maps") events_map = {
    .type = BPF_MAP_TYPE_PERF_EVENT_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u32),
    .max_entries = 256,
};

// PERCPU_ARRAY: per-CPU event counters (no atomic ops needed)
// Index 0=total, 1=fork, 2=exit, 3=switch, 4=exec, 5=perf_output_fail,
//       6=runq_long, 7=runq_short, 8=runq_stale (P1-1 TTL),
//       9=short_sample_counter (P1-2 uniform 1/8 sampling),
//      10=wakeup_map_fail (P3: bpf_map_update_elem ENOSPC on wakeup_cache)
struct bpf_map_def SEC("maps") event_counters = {
    .type = BPF_MAP_TYPE_PERCPU_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u64),
    .max_entries = 12,   /* R9-005 / LED-136: grew 11 -> 12 for COUNTER_WAKEUP_NOT_SUCCESS */
};

// P1-1/P2-2: HASH_MAP wakeup_cache (tid -> wakeup record).
// Written by sched_wakeup/sched_wakeup_new (map-only, ~50ns, NO perf output),
// looked up + deleted by sched_switch to compute runqueue latency.
// P2-2: value extended to 24 bytes: ktime(8)+waker_tid(4)+target_cpu(4)+pad(8)
// so the runq event carries the wakeup target CPU (not the CPU that picked it).
struct wakeup_rec {
    __u64 ktime_ns;
    __u32 waker_tid;
    __u32 target_cpu;
    __u64 pad;
};
struct bpf_map_def SEC("maps") wakeup_cache = {
    /* v4.5.0 F-03: LRU_HASH so the kernel auto-reaps cold/orphaned entries
     * (woken but never scheduled, never exited) before live pending wakeups.
     * Same key/value/max_entries as the v4.4.9 HASH; the loader falls back to
     * plain HASH if the kernel lacks LRU_HASH. */
    .type = BPF_MAP_TYPE_LRU_HASH,
    .key_size = sizeof(__u32),
    .value_size = sizeof(struct wakeup_rec),
    /* P2-3: raised 4096 -> 8192. On a busy system the live thread count can
     * exceed 4096; a HASH map at 4096 then rejects new wakeup entries (max
     * reached) and new runq events are lost. 8192 gives headroom. Entries are
     * consumed on sched_switch and purged on sched_process_exit, so the map
     * never holds more than concurrently-pending wakeups. */
    .max_entries = 8192,
};

// v94: PERCPU_ARRAY per-CPU sched_switch 1/10 sampling counter.
// key=0, value=__u64, max_entries=1. Free-running per-CPU counter that
// increments on EVERY sched_switch (no atomic ops needed on PERCPU_ARRAY).
// Only when (counter % 10) == 0 is the switch event pushed to ringbuf.
// COUNTER_SWITCH / COUNTER_TOTAL in event_counters still count 100% for
// true event-rate display. fork/exec/exit/wakeup/runq events are NOT sampled.
struct bpf_map_def SEC("maps") switch_sample_counter = {
    .type = BPF_MAP_TYPE_PERCPU_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u64),
    .max_entries = 1,
};

// Counter indices for event_counters PERCPU_ARRAY
#define COUNTER_TOTAL           0
#define COUNTER_FORK            1
#define COUNTER_EXIT            2
#define COUNTER_SWITCH          3
#define COUNTER_EXEC            4
#define COUNTER_PERF_OUTPUT_FAIL 5
#define COUNTER_RUNQ_LONG       6
#define COUNTER_RUNQ_SHORT      7
#define COUNTER_RUNQ_STALE      8   /* P1-1: TTL-expired wakeup records */
#define COUNTER_SHORT_SAMPLE    9   /* P1-2: free-running 1/8 sampling counter */
#define COUNTER_WAKEUP_MAP_FAIL 10  /* P3: bpf_map_update_elem rejected wakeup_cache write */
#define COUNTER_WAKEUP_NOT_SUCCESS 11 /* R9-005 / LED-136: wakeup tracepoint success==0 (not actually enqueued); skipped */

// P1-1: runqueue latency threshold. Above this = "long" (full perf output).
// Below = sampled 1/8. 200us is a classic interactive-scheduling red line.
#define RUNQ_LONG_THRESHOLD_NS  200000ULL

// P1-1: TTL for wakeup_cache entries. If wakeup->switch delta exceeds 1 second,
// the record is stale (task never ran, got migrated, or wakeup was spurious).
// Delete it and don't emit a runq event — this prevents ghost multi-second delays.
#define WAKEUP_TTL_NS           1000000000ULL

// ---------------------------------------------------------------------------
// Helper: increment per-CPU counter (no atomic needed on PERCPU_ARRAY)
// Uses 64-bit unsigned operations only — compatible with kernel 4.14.
// ---------------------------------------------------------------------------
static inline void inc_counter(__u32 idx) {
    __u64 *val = (__u64 *)bpf_map_lookup_elem(&event_counters, &idx);
    if (val) {
        __u64 v = *val;
        v = v + 1;
        *val = v;
    }
}

// ---------------------------------------------------------------------------
// Helper: safe perf_event_output wrapper.
// Checks bpf_perf_event_output() return value and increments
// COUNTER_PERF_OUTPUT_FAIL on failure. This is critical for diagnosing
// ring-buffer output issues on 4.14 kernels where PERF_TYPE_TRACEPOINT
// events may silently fail to receive BPF output.
// ---------------------------------------------------------------------------
static inline void perf_output_safe(void *ctx, void *data, __u64 size) {
    __u64 ret = bpf_perf_event_output(ctx, &events_map, BPF_F_CURRENT_CPU,
                                      data, size);
    if (ret != 0) {
        inc_counter(COUNTER_PERF_OUTPUT_FAIL);
    }
}

// ---------------------------------------------------------------------------
// Helper: safe 16-byte comm copy (unrolled for verifier)
// ---------------------------------------------------------------------------
static inline void copy_comm(char *dst, const char *src) {
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
    dst[8] = src[8]; dst[9] = src[9]; dst[10] = src[10]; dst[11] = src[11];
    dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
}

// ---------------------------------------------------------------------------
// Helper: get current tid (low 32 bits of pid_tgid)
// ---------------------------------------------------------------------------
static inline __u32 current_tid(void) {
    __u64 pt = bpf_get_current_pid_tgid();
    return (__u32)(pt & 0xFFFFFFFFULL);
}

// ---------------------------------------------------------------------------
// Helper: get current tgid (high 32 bits of pid_tgid)
// ---------------------------------------------------------------------------
static inline __u32 current_tgid(void) {
    __u64 pt = bpf_get_current_pid_tgid();
    return (__u32)(pt >> 32);
}

// ---------------------------------------------------------------------------
// Helper: fill common event header fields
// ---------------------------------------------------------------------------
static inline void fill_header(struct aatpe_event_header *hdr, __u32 type) {
    hdr->type = type;
    hdr->cpu = bpf_get_smp_processor_id();
    hdr->timestamp_ns = bpf_ktime_get_ns();
}

// ---------------------------------------------------------------------------
// TRACEPOINT: sched_process_fork — child process/thread created
// current = parent.
//
// v94 fix (audit breakpoint 1): child_tid was hardcoded to 0 because the old
// code refused to touch the tracepoint ctx. The sched_process_fork tracepoint
// ctx layout is ABI-stable on kernel 4.14 (MTK Dimensity 1100 / 4.14.186):
//
//   offset 0..7   : struct trace_entry common header
//                   (common_type:u16=2, common_flags:u8=1,
//                    common_preempt_count:u8=1, common_pid:s32=4) = 8 bytes
//   offset 8..23  : char comm[16]  (parent->comm)
//   offset 24     : pid_t parent_pid   (parent->pid  == parent TID)
//   offset 28..43 : char child_comm[16] (child->comm)
//                   P0-1 FIX: previous revision fabricated a 4-byte parent_tgid
//                   field at offset 28. The real kernel 4.14
//                   TRACE_EVENT(sched_process_fork) TP_STRUCT__entry is:
//                     __array(char, comm, TASK_COMM_LEN)     // parent comm
//                     __field(pid_t, parent_pid)
//                     __array(char, child_comm, TASK_COMM_LEN) // child comm, 16 bytes!
//                     __field(pid_t, child_pid)
//                     __field(pid_t, child_tgid)
//                   There is NO parent_tgid field in this tracepoint. The
//                   fabricated parent_tgid caused child_pid to be read from
//                   ctx+32 (which is actually child_comm bytes 4..7) instead
//                   of the real ctx+44. P0-1 corrects this.
//   offset 44     : pid_t child_pid    (child->pid   == child TID)   <-- WANT
//   offset 48     : pid_t child_tgid   (child->tgid  == child PID)
//
// Basis: include/trace/events/sched.h TRACE_EVENT(sched_process_fork, ...)
// TP_STRUCT__entry order: comm[16], parent_pid, child_comm[16], child_pid,
// child_tgid. The 8-byte common trace_entry header precedes every tracepoint
// entry, so the first field of TP_STRUCT__entry starts at ctx+8.
//
// We read child_pid (== child TID for a freshly forked task, since task->pid
// is the thread ID) and child_tgid via bpf_probe_read (verifier-safe; no
// direct ctx dereference, no signed-32-bit compares, compatible with 4.14).
// ---------------------------------------------------------------------------
SEC("tracepoint/sched/sched_process_fork")
int handle_fork(void *ctx) {
    struct aatpe_fork_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_FORK);
    /* P2-3: read real parent TID from tracepoint ctx+24 (parent_pid field).
     * Previously used current_tgid() which returns the parent's TGID (PID),
     * not its TID. The tracepoint field parent_pid = parent->pid = parent TID. */
    __u32 parent_tid = 0;
    /* R9-006 / LED-137: check the read result and bail on fault. Previously a
     * failed parent_tid read was ignored and emitted a fork event with
     * ev.parent_pid=0, corrupting the parent-child map. */
    if (bpf_probe_read(&parent_tid, sizeof(parent_tid),
                   (const void *)ctx + 24) != 0) {   /* parent_pid == parent TID */
        return 0;
    }
    ev.parent_pid = parent_tid;

    // v94 fix: read child_pid (= child TID) and child_tgid from tracepoint ctx.
    // P2-5: if the child read faults, emit NOTHING rather than a zero-value
    // fork event (which would poison placement with pid=0). The bpf_probe_read
    // return is __u64; `!= 0` is a 64-bit compare (4.14 safe, no BPF_JMP32).
    __u32 child_tid = 0;
    if (bpf_probe_read(&child_tid, sizeof(child_tid),
                       (const void *)ctx + 44) != 0) {   /* child_pid == child TID */
        return 0;
    }
    /* R9-022 / LED-145: dropped the out-of-bounds child_tgid (ctx+48) probe
     * read. The sched_process_fork tracepoint only has child_pid at ctx+44
     * (the new process leader, whose TGID == TID at fork); there is no field
     * at +48. Both child_pid and child_tid come from that one value. */
    ev.child_pid = child_tid;
    ev.child_tid = child_tid;

    perf_output_safe(ctx, &ev, sizeof(ev));
    inc_counter(COUNTER_FORK);
    inc_counter(COUNTER_TOTAL);
    return 0;
}

// ---------------------------------------------------------------------------
// TRACEPOINT: sched_process_exit — task exiting
// current = the exiting task.
// ---------------------------------------------------------------------------
SEC("tracepoint/sched/sched_process_exit")
int handle_exit(void *ctx) {
    struct aatpe_exit_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_EXIT);
    /* P2-2: call bpf_get_current_pid_tgid() exactly once and derive tgid/tid
     * from the 64-bit return value, instead of 3 separate helper calls (the
     * BPF verifier does not CSE the helper, so each call was a real helper
     * invocation). */
    __u64 pt = bpf_get_current_pid_tgid();
    __u32 tgid = (__u32)(pt >> 32);
    __u32 tid  = (__u32)(pt & 0xFFFFFFFFULL);
    ev.pid = tgid;
    ev.tid = tid;

    perf_output_safe(ctx, &ev, sizeof(ev));

    /* P2-1: thread_cache map removed (dead map, only deleted, never looked up).
     * Clean up wakeup_cache entry for the exiting thread. */
    bpf_map_delete_elem(&wakeup_cache, &tid);  /* P1-1 */

    inc_counter(COUNTER_EXIT);
    inc_counter(COUNTER_TOTAL);
    return 0;
}

// ---------------------------------------------------------------------------
// TRACEPOINT: sched_switch — context switch
//
// v94 P1-1 FIX (semantic-correctness of next_pid):
//   The previous implementation derived next_pid via bpf_get_current_pid_tgid().
//   That helper returns the CURRENT task at the moment the BPF program runs.
//   sched_switch fires on the CPU that is performing the switch, BEFORE the
//   scheduler has committed "current = next"; on kernel 4.14 the tracepoint is
//   emitted from __schedule() with `prev` still in the current/pickled context,
//   so bpf_get_current_pid_tgid() can yield PREV, not next. Using it as next_pid
//   is a silent semantic error that mis-assigns currentCpu and mis-classifies.
//
//   The robust, ABI-stable source is the tracepoint ctx itself. The
//   sched_switch tracepoint entry explicitly records both tasks, so we read
//   prev_pid/next_pid/prev_comm/next_comm straight from ctx with bpf_probe_read.
//
// KERNEL 4.14 sched_switch ctx LAYOUT (offset basis — verified against
// include/trace/events/sched.h, TRACE_EVENT(sched_switch,...) TP_STRUCT__entry):
//
//   struct trace_entry common header precedes EVERY tracepoint entry:
//     offset 0..1  : common_type            (__data_loc u16)
//     offset 2     : common_flags           (u8)
//     offset 3     : common_preempt_count   (u8)
//     offset 4..7  : common_pid              (s32)
//                   => 8-byte common header total. TP_STRUCT__entry starts @ +8.
//
//   TP_STRUCT__entry field order (4.14, ABI-stable on MTK 4.14.186):
//     __array(char, prev_comm,  TASK_COMM_LEN=16)
//     __field(pid_t, prev_pid)
//     __field(int,   prev_prio)
//     __field(long,  prev_state)          ← 8 bytes on 64-bit kernel!
//     __array(char, next_comm,  TASK_COMM_LEN=16)
//     __field(pid_t, next_pid)
//     __field(int,   next_prio)
//
//   Therefore, absolute offsets from ctx:
//     ctx +  8 : char  prev_comm[16]   (16 bytes)   <-- read
//     ctx + 24 : pid_t prev_pid         (4 bytes)    <-- read
//     ctx + 28 : int   prev_prio        (4 bytes)    (unused, skip)
//     ctx + 32 : long  prev_state       (8 bytes)    (SKIP — 64-bit long!)
//     ctx + 40 : char  next_comm[16]   (16 bytes)   <-- read
//     ctx + 56 : pid_t next_pid         (4 bytes)    <-- read
//     ctx + 60 : int   next_prio        (4 bytes)    (unused, skip)
//
//   CRITICAL NOTE: prev_state is `long` (8 bytes on arm64), not 4 bytes.
//   Omitting it shifts ALL subsequent fields by +8. The previous revision
//   (next_comm@32 / next_pid@48 / next_prio@52) was wrong for exactly this
//   reason — it read prev_state's low 4 bytes as next_pid and garbage as
//   next_comm, silently corrupting thread identity. Fixed in v94-r2.
//
//   pid_t is a signed 32-bit int in the kernel; we read it into a __u32 (raw
//   4-byte copy) — all valid task pids are non-negative and fit.
//
//   Consistency check: the sched_process_fork handler in this file already uses
//   the identical methodology (8-byte common header + TP_STRUCT__entry order),
//   reading child_pid @ +44 / child_tgid @ +48 (P0-1 corrected; was @+32/@+36
//   due to fabricated parent_tgid field), which is the documented, correct basis.
//   We apply the exact same rule here.
//
//   Verify on-device if needed:
//     cat /sys/kernel/debug/tracing/events/sched/sched_switch/format
//   The "field:char prev_comm[16];" / "field:pid_t next_pid;" lines and their
//   "offset:" values must match the table above.
//
//   Compatibility: bpf_probe_read only (no bpf_core_read / CO-RE, no direct ctx
//   dereference, no signed-32-bit compare) — all safe on kernel 4.14.
// ---------------------------------------------------------------------------
SEC("tracepoint/sched/sched_switch")
int handle_switch(void *ctx) {
    struct aatpe_switch_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_SWITCH);

    // Read prev_pid / next_pid and both comm strings directly from the
    // tracepoint ctx using the offsets documented above. Any fault leaves the
    // locals at 0 (memset above), preserving the old safe fallback.
    __u32 prev_pid = 0;
    __u32 next_pid = 0;
    char prev_comm[16];
    char next_comm[16];
    __builtin_memset(prev_comm, 0, sizeof(prev_comm));
    __builtin_memset(next_comm, 0, sizeof(next_comm));

    /* L-fix: prev_pid read return was previously discarded. prev_pid is
     * non-load-bearing (the runq-latency path keys on next_pid), so on fault we
     * keep the zero-initialized value rather than dropping the whole switch
     * event — but the helper return is now checked explicitly instead of being
     * ignored. 64-bit compare on the __u64 helper return (4.14 verifier-safe,
     * no BPF_JMP32), matching the next_pid pattern below. */
    if (bpf_probe_read(&prev_pid, sizeof(prev_pid), (const void *)ctx + 24) != 0) {
        prev_pid = 0;
    } /* prev_pid */
    /* P2-5: next_pid is load-bearing (wakeup_cache lookup + switch event). If
     * the read faults, emit nothing rather than a next_pid=0 event. 64-bit
     * compare on the __u64 helper return — 4.14 safe (no BPF_JMP32). */
    if (bpf_probe_read(&next_pid, sizeof(next_pid),
                       (const void *)ctx + 56) != 0) {  /* next_pid (R9-001: +56, NOT +48) */
        return 0;
    }
    bpf_probe_read(prev_comm, sizeof(prev_comm), (const void *)ctx + 8);  /* prev_comm[16] */
    bpf_probe_read(next_comm, sizeof(next_comm), (const void *)ctx + 40); /* next_comm[16] */

    ev.prev_pid = prev_pid;
    ev.next_pid = next_pid;
    copy_comm(ev.prev_comm, prev_comm);
    copy_comm(ev.next_comm, next_comm);

    /* v94: kernel-side 1/10 sampling for sched_switch perf_output.
     * Per-CPU free-running counter (switch_sample_counter PERCPU_ARRAY, key=0).
     * Increments on EVERY switch; only (v % 10ULL) == 0ULL triggers the
     * ringbuf push. COUNTER_SWITCH / COUNTER_TOTAL below still count 100%
     * so userspace sees the true event rate. 64-bit unsigned modulo + compare
     * only — BPF_MOD64 (class=4) + BPF_JEQ64 (class=5), kernel 4.14 safe. */
    {
        __u32 ss_key = 0;
        __u64 *ss = (__u64 *)bpf_map_lookup_elem(&switch_sample_counter, &ss_key);
        if (ss) {
            __u64 v = *ss;
            v = v + 1;
            *ss = v;
            if ((v % 10ULL) == 0ULL) {
                perf_output_safe(ctx, &ev, sizeof(ev));
            }
        }
    }
    inc_counter(COUNTER_SWITCH);
    inc_counter(COUNTER_TOTAL);

    /* P1-1: runqueue latency. If `next` was previously woken (sched_wakeup
     * recorded its ktime), compute wakeup->run delay. P0-1: removed the outer
     * `if (next_pid != 0)` guard (which generated BPF_JMP32 class=6). The
     * bpf_map_lookup_elem returns NULL for unknown keys, so `if (wrec)` is the
     * sole guard — 4.14 safe. */
    {
        struct wakeup_rec *wrec = (struct wakeup_rec *)bpf_map_lookup_elem(&wakeup_cache, &next_pid);
        if (wrec) {
            __u64 wakeup_ts = wrec->ktime_ns;
            __u32 waker = wrec->waker_tid;
            __u32 tcpu = wrec->target_cpu;   /* P2-2: wakeup target CPU */
            bpf_map_delete_elem(&wakeup_cache, &next_pid);
            __u64 now = bpf_ktime_get_ns();
            __u64 delta = now - wakeup_ts;
            /* 64-bit unsigned compare only — 4.14 safe. */

            /* P1-1: TTL check. If delta > 1 second, this is a stale record.
             * Count it and discard — no ghost multi-second runq events. */
            if (delta > WAKEUP_TTL_NS) {
                inc_counter(COUNTER_RUNQ_STALE);
            } else {
                __u32 emit = 0;
                if (delta > RUNQ_LONG_THRESHOLD_NS) {
                    emit = 1;
                    inc_counter(COUNTER_RUNQ_LONG);
                } else {
                    /* P1-2: short delay — uniform 1/8 sampling via percpu
                     * free-running counter (COUNTER_SHORT_SAMPLE). Previously
                     * used (cpu & 7)==0 which only sampled CPU0 on 8-core.
                     * R9-021 / LED-153: DESIGN NOTE. COUNTER_RUNQ_SHORT below is
                     * incremented for EVERY short run-queue delay (true 100% total),
                     * while only 1 of every 8 is pushed to userspace as
                     * AATPE_EVT_RUNQ. This is deliberate: the counter reports the
                     * true short-delay rate cheaply, while the sampled events bound
                     * the ringbuf cost. Consumers averaging delay_ns from emitted
                     * events must NOT divide by the counter (which is already the
                     * full total) -- emitted events are a 1/8 subset whose average
                     * delay is unbiased for short delays. Do not "fix" by making the
                     * sampled event also 100%; that was the ringbuf-overflow tradeoff. */
                    __u32 sc_idx = COUNTER_SHORT_SAMPLE;
                    __u64 *sc = (__u64 *)bpf_map_lookup_elem(&event_counters, &sc_idx);
                    if (sc) {
                        __u64 v = *sc;
                        v = v + 1;
                        *sc = v;
                        if ((v & 7ULL) == 0) {
                            emit = 1;
                        }
                    }
                    inc_counter(COUNTER_RUNQ_SHORT);
                }
                if (emit) {
                    struct aatpe_runq_event rq;
                    __builtin_memset(&rq, 0, sizeof(rq));
                    rq.hdr.type = AATPE_EVT_RUNQ;
                    rq.hdr.cpu = bpf_get_smp_processor_id();
                    rq.hdr.timestamp_ns = now;
                    rq.pid = next_pid;
                    rq.target_cpu = tcpu;   /* P2-2: wakeup target, not current CPU */
                    rq.waker_tid = waker;   /* P2-4: 1-hop wakeup edge */
                    rq.delay_ns = delta;
                    perf_output_safe(ctx, &rq, sizeof(rq));
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// TRACEPOINT: sched_process_exec — process exec
// current = the task doing exec.
// ---------------------------------------------------------------------------
SEC("tracepoint/sched/sched_process_exec")
int handle_exec(void *ctx) {
    struct aatpe_exec_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_EXEC);
    /* P2-2: single pid_tgid call, derive tgid/tid (avoid 2 helper calls). */
    __u64 pt = bpf_get_current_pid_tgid();
    ev.pid = (__u32)(pt >> 32);
    ev.tid = (__u32)(pt & 0xFFFFFFFFULL);

    char comm[16];
    __builtin_memset(comm, 0, sizeof(comm));
    bpf_get_current_comm(comm, sizeof(comm));
    copy_comm(ev.comm, comm);

    perf_output_safe(ctx, &ev, sizeof(ev));
    inc_counter(COUNTER_EXEC);
    inc_counter(COUNTER_TOTAL);
    return 0;
}

// ---------------------------------------------------------------------------
// P1-1: TRACEPOINT: sched_wakeup / sched_wakeup_new
//
// Records the ktime at which a task is woken up into wakeup_cache[tid].
// THIS PROGRAM WRITES A MAP ONLY — it never calls perf_event_output and never
// increments counters, keeping the fast path to a single bpf_map_update_elem
// (~50ns). The matching sched_switch handler computes wakeup->run latency.
//
// KERNEL 4.14 sched_wakeup ctx LAYOUT (TP_STRUCT__entry, after 8-byte common
// trace_entry header; verified against include/trace/events/sched.h):
//     __array(char, comm, TASK_COMM_LEN=16)
//     __field(pid_t, pid)        // p->pid == woken task TID  <-- WANT
//     __field(int,   prio)
//     __field(int,   success)
//     __field(int,   target_cpu)
//
// Absolute offsets from ctx:
//     ctx +  8 : char  comm[16]   (16 bytes)
//     ctx + 24 : pid_t pid        (4 bytes)   <-- read
//     ctx + 28 : int   prio       (4 bytes)
//     ctx + 32 : int   success    (4 bytes)
//     ctx + 36 : int   target_cpu (4 bytes)
//
// sched_wakeup_new shares the identical TP_STRUCT__entry layout.
// ---------------------------------------------------------------------------
SEC("tracepoint/sched/sched_wakeup")
int handle_wakeup(void *ctx) {
    __u32 pid = 0;
    /* P2-4: only write wakeup_cache when the tracepoint pid read SUCCEEDS.
     * bpf_probe_read returns 0 on success (64-bit compare, 4.14 safe). On
     * failure pid stays 0; previously we unconditionally wrote
     * wakeup_cache[0] creating a ghost entry keyed by 0. Skipping the update
     * on failure avoids that without a 32-bit `pid == 0` compare (BPF_JMP32). */
    if (bpf_probe_read(&pid, sizeof(pid), (const void *)ctx + 24) != 0) {
        return 0;
    }
    /* R9-005 / LED-136: read success@32 together with target_cpu@36 as one 8-byte
     * word (low32=success, high32=target_cpu). success==0 means the wakeup did NOT
     * actually enqueue the task; recording it would plant a ghost wakeup_cache entry
     * that later yields a spurious (inflated) run-queue latency. Skip the update.
     * The (sc & 0xffffffff)==0 test is a 64-bit AND + compare (BPF_AND64/JEQ64),
     * avoiding a 32-bit BPF_JMP32 that the 4.14 verifier rejects. */
    __u64 sc = 0;
    if (bpf_probe_read(&sc, sizeof(sc), (const void *)ctx + 32) != 0) {
        return 0;
    }
    __u32 target_cpu = (__u32)(sc >> 32);
    __u64 low = sc & 0xFFFFFFFFULL;
    /* volatile prevents clang from narrowing the low32 test into a 32-bit
     * JMP32 (opcode 0x56 at offset 160), which the kernel 4.14 verifier
     * rejects. It forces a 64-bit JEQ64 against the stack word. */
    volatile __u64 vlow = low;
    if (vlow == 0) {
        inc_counter(COUNTER_WAKEUP_NOT_SUCCESS);
        return 0;
    }
    struct wakeup_rec rec;
    __builtin_memset(&rec, 0, sizeof(rec));
    rec.ktime_ns = bpf_ktime_get_ns();
    rec.waker_tid = (__u32)(bpf_get_current_pid_tgid() & 0xFFFFFFFFULL);
    rec.target_cpu = target_cpu;   /* P2-2 */
    /* BPF_ANY(0): overwrite any stale pending wakeup for this tid.
     * P3: check the update return — on ENOSPC (map full) the runq latency for
     * this wakeup is lost; count it rather than silently dropping. 64-bit
     * compare on the __u64 helper return (4.14 safe, no BPF_JMP32). */
    __u64 uret = bpf_map_update_elem(&wakeup_cache, &pid, &rec, 0);
    if (uret != 0) {
        inc_counter(COUNTER_WAKEUP_MAP_FAIL);
    }
    return 0;
}

SEC("tracepoint/sched/sched_wakeup_new")
int handle_wakeup_new(void *ctx) {
    __u32 pid = 0;
    /* P2-4: same read-success guard as handle_wakeup (no pid=0 ghost entry). */
    if (bpf_probe_read(&pid, sizeof(pid), (const void *)ctx + 24) != 0) {
        return 0;
    }
    /* R9-005 / LED-136: same success@32 guard as handle_wakeup (shared tracepoint
     * layout). Skip the map update when success==0 so a non-enqueued wakeup does not
     * inflate run-queue latency. 64-bit AND+compare, 4.14 verifier-safe. */
    __u64 sc = 0;
    if (bpf_probe_read(&sc, sizeof(sc), (const void *)ctx + 32) != 0) {
        return 0;
    }
    __u32 target_cpu = (__u32)(sc >> 32);
    __u64 low = sc & 0xFFFFFFFFULL;
    /* volatile prevents clang from narrowing the low32 test into a 32-bit
     * JMP32 (opcode 0x56 at offset 160), which the kernel 4.14 verifier
     * rejects. It forces a 64-bit JEQ64 against the stack word. */
    volatile __u64 vlow = low;
    if (vlow == 0) {
        inc_counter(COUNTER_WAKEUP_NOT_SUCCESS);
        return 0;
    }
    struct wakeup_rec rec;
    __builtin_memset(&rec, 0, sizeof(rec));
    rec.ktime_ns = bpf_ktime_get_ns();
    rec.waker_tid = (__u32)(bpf_get_current_pid_tgid() & 0xFFFFFFFFULL);
    rec.target_cpu = target_cpu;   /* P2-2 */
    __u64 uret = bpf_map_update_elem(&wakeup_cache, &pid, &rec, 0);
    if (uret != 0) {
        inc_counter(COUNTER_WAKEUP_MAP_FAIL);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";


#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

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

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} tid_enqueue_map SEC(".maps");

SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx) {
    __u32 key = 0;
    __u64 now = bpf_ktime_get_ns();

    __u32 *switch_count = bpf_map_lookup_elem(&cpu_switch_map, &key);
    if (switch_count) {
        __u32 v = *switch_count;
        v = v + 1;
        *switch_count = v;
    }

    __u32 prev_pid = ctx->prev_pid;
    __u64 *enqueue_ts = bpf_map_lookup_elem(&tid_enqueue_map, &prev_pid);
    if (enqueue_ts && *enqueue_ts > 0) {
        __u64 runtime = now - *enqueue_ts;
        __u64 *runtime_accum = bpf_map_lookup_elem(&cpu_runtime_map, &key);
        if (runtime_accum) {
            __u64 v = *runtime_accum;
            v = v + runtime;
            *runtime_accum = v;
        }
    }

    __u32 next_pid = ctx->next_pid;
    bpf_map_update_elem(&tid_enqueue_map, &next_pid, &now, BPF_ANY);

    return 0;
}

char LICENSE[] SEC("license") = "GPL";

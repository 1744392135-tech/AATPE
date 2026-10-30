
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef int __s32;

#define BPF_MAP_TYPE_UNSPEC           0
#define BPF_MAP_TYPE_HASH             1
#define BPF_MAP_TYPE_ARRAY            2
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

#define SEC(NAME) __attribute__((section(NAME), used))

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

struct bpf_map_def {
    __u32 type;
    __u32 key_size;
    __u32 value_size;
    __u32 max_entries;
    __u32 map_flags;
};

#define AATPE_EVT_FORK   1
#define AATPE_EVT_EXIT   2
#define AATPE_EVT_SWITCH 3
#define AATPE_EVT_EXEC   4
#define AATPE_EVT_RUNQ   5

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

struct aatpe_runq_event {
    struct aatpe_event_header hdr;
    __u32 pid;
    __u32 target_cpu;
    __u32 waker_tid;
    __u32 pad;
    __u64 delay_ns;
};

struct bpf_map_def SEC("maps") events_map = {
    .type = BPF_MAP_TYPE_PERF_EVENT_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u32),
    .max_entries = 256,
};

struct bpf_map_def SEC("maps") event_counters = {
    .type = BPF_MAP_TYPE_PERCPU_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u64),
    .max_entries = 12,
};

struct wakeup_rec {
    __u64 ktime_ns;
    __u32 waker_tid;
    __u32 target_cpu;
    __u64 pad;
};
struct bpf_map_def SEC("maps") wakeup_cache = {
    .type = BPF_MAP_TYPE_LRU_HASH,
    .key_size = sizeof(__u32),
    .value_size = sizeof(struct wakeup_rec),
    .max_entries = 8192,
};

struct bpf_map_def SEC("maps") switch_sample_counter = {
    .type = BPF_MAP_TYPE_PERCPU_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = sizeof(__u64),
    .max_entries = 1,
};

#define COUNTER_TOTAL           0
#define COUNTER_FORK            1
#define COUNTER_EXIT            2
#define COUNTER_SWITCH          3
#define COUNTER_EXEC            4
#define COUNTER_PERF_OUTPUT_FAIL 5
#define COUNTER_RUNQ_LONG       6
#define COUNTER_RUNQ_SHORT      7
#define COUNTER_RUNQ_STALE      8
#define COUNTER_SHORT_SAMPLE    9
#define COUNTER_WAKEUP_MAP_FAIL 10
#define COUNTER_WAKEUP_NOT_SUCCESS 11

#define RUNQ_LONG_THRESHOLD_NS  200000ULL

#define WAKEUP_TTL_NS           1000000000ULL

static inline void inc_counter(__u32 idx) {
    __u64 *val = (__u64 *)bpf_map_lookup_elem(&event_counters, &idx);
    if (val) {
        __u64 v = *val;
        v = v + 1;
        *val = v;
    }
}

static inline void perf_output_safe(void *ctx, void *data, __u64 size) {
    __u64 ret = bpf_perf_event_output(ctx, &events_map, BPF_F_CURRENT_CPU,
                                      data, size);
    if (ret != 0) {
        inc_counter(COUNTER_PERF_OUTPUT_FAIL);
    }
}

static inline void copy_comm(char *dst, const char *src) {
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
    dst[8] = src[8]; dst[9] = src[9]; dst[10] = src[10]; dst[11] = src[11];
    dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
}

static inline __u32 current_tid(void) {
    __u64 pt = bpf_get_current_pid_tgid();
    return (__u32)(pt & 0xFFFFFFFFULL);
}

static inline __u32 current_tgid(void) {
    __u64 pt = bpf_get_current_pid_tgid();
    return (__u32)(pt >> 32);
}

static inline void fill_header(struct aatpe_event_header *hdr, __u32 type) {
    hdr->type = type;
    hdr->cpu = bpf_get_smp_processor_id();
    hdr->timestamp_ns = bpf_ktime_get_ns();
}

SEC("tracepoint/sched/sched_process_fork")
int handle_fork(void *ctx) {
    struct aatpe_fork_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_FORK);
    __u32 parent_tid = 0;
    if (bpf_probe_read(&parent_tid, sizeof(parent_tid),
                   (const void *)ctx + 24) != 0) {
        return 0;
    }
    ev.parent_pid = parent_tid;

    __u32 child_tid = 0;
    if (bpf_probe_read(&child_tid, sizeof(child_tid),
                       (const void *)ctx + 44) != 0) {
        return 0;
    }
    ev.child_pid = child_tid;
    ev.child_tid = child_tid;

    perf_output_safe(ctx, &ev, sizeof(ev));
    inc_counter(COUNTER_FORK);
    inc_counter(COUNTER_TOTAL);
    return 0;
}

SEC("tracepoint/sched/sched_process_exit")
int handle_exit(void *ctx) {
    struct aatpe_exit_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_EXIT);
    __u64 pt = bpf_get_current_pid_tgid();
    __u32 tgid = (__u32)(pt >> 32);
    __u32 tid  = (__u32)(pt & 0xFFFFFFFFULL);
    ev.pid = tgid;
    ev.tid = tid;

    perf_output_safe(ctx, &ev, sizeof(ev));

    bpf_map_delete_elem(&wakeup_cache, &tid);

    inc_counter(COUNTER_EXIT);
    inc_counter(COUNTER_TOTAL);
    return 0;
}

SEC("tracepoint/sched/sched_switch")
int handle_switch(void *ctx) {
    struct aatpe_switch_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_SWITCH);

    __u32 prev_pid = 0;
    __u32 next_pid = 0;
    char prev_comm[16];
    char next_comm[16];
    __builtin_memset(prev_comm, 0, sizeof(prev_comm));
    __builtin_memset(next_comm, 0, sizeof(next_comm));

    if (bpf_probe_read(&prev_pid, sizeof(prev_pid), (const void *)ctx + 24) != 0) {
        prev_pid = 0;
    }
    if (bpf_probe_read(&next_pid, sizeof(next_pid),
                       (const void *)ctx + 56) != 0) {
        return 0;
    }
    bpf_probe_read(prev_comm, sizeof(prev_comm), (const void *)ctx + 8);
    bpf_probe_read(next_comm, sizeof(next_comm), (const void *)ctx + 40);

    ev.prev_pid = prev_pid;
    ev.next_pid = next_pid;
    copy_comm(ev.prev_comm, prev_comm);
    copy_comm(ev.next_comm, next_comm);

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

    {
        struct wakeup_rec *wrec = (struct wakeup_rec *)bpf_map_lookup_elem(&wakeup_cache, &next_pid);
        if (wrec) {
            __u64 wakeup_ts = wrec->ktime_ns;
            __u32 waker = wrec->waker_tid;
            __u32 tcpu = wrec->target_cpu;
            bpf_map_delete_elem(&wakeup_cache, &next_pid);
            __u64 now = bpf_ktime_get_ns();
            __u64 delta = now - wakeup_ts;

            if (delta > WAKEUP_TTL_NS) {
                inc_counter(COUNTER_RUNQ_STALE);
            } else {
                __u32 emit = 0;
                if (delta > RUNQ_LONG_THRESHOLD_NS) {
                    emit = 1;
                    inc_counter(COUNTER_RUNQ_LONG);
                } else {
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
                    rq.target_cpu = tcpu;
                    rq.waker_tid = waker;
                    rq.delay_ns = delta;
                    perf_output_safe(ctx, &rq, sizeof(rq));
                }
            }
        }
    }
    return 0;
}

SEC("tracepoint/sched/sched_process_exec")
int handle_exec(void *ctx) {
    struct aatpe_exec_event ev;
    __builtin_memset(&ev, 0, sizeof(ev));
    fill_header(&ev.hdr, AATPE_EVT_EXEC);

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

SEC("tracepoint/sched/sched_wakeup")
int handle_wakeup(void *ctx) {
    __u32 pid = 0;
    if (bpf_probe_read(&pid, sizeof(pid), (const void *)ctx + 24) != 0) {
        return 0;
    }
    __u64 sc = 0;
    if (bpf_probe_read(&sc, sizeof(sc), (const void *)ctx + 32) != 0) {
        return 0;
    }
    __u32 target_cpu = (__u32)(sc >> 32);
    __u64 low = sc & 0xFFFFFFFFULL;
    volatile __u64 vlow = low;
    if (vlow == 0) {
        inc_counter(COUNTER_WAKEUP_NOT_SUCCESS);
        return 0;
    }
    struct wakeup_rec rec;
    __builtin_memset(&rec, 0, sizeof(rec));
    rec.ktime_ns = bpf_ktime_get_ns();
    rec.waker_tid = (__u32)(bpf_get_current_pid_tgid() & 0xFFFFFFFFULL);
    rec.target_cpu = target_cpu;
    __u64 uret = bpf_map_update_elem(&wakeup_cache, &pid, &rec, 0);
    if (uret != 0) {
        inc_counter(COUNTER_WAKEUP_MAP_FAIL);
    }
    return 0;
}

SEC("tracepoint/sched/sched_wakeup_new")
int handle_wakeup_new(void *ctx) {
    __u32 pid = 0;

    if (bpf_probe_read(&pid, sizeof(pid), (const void *)ctx + 24) != 0) {
        return 0;
    }
    __u64 sc = 0;
    if (bpf_probe_read(&sc, sizeof(sc), (const void *)ctx + 32) != 0) {
        return 0;
    }
    __u32 target_cpu = (__u32)(sc >> 32);
    __u64 low = sc & 0xFFFFFFFFULL;
    volatile __u64 vlow = low;
    if (vlow == 0) {
        inc_counter(COUNTER_WAKEUP_NOT_SUCCESS);
        return 0;
    }
    struct wakeup_rec rec;
    __builtin_memset(&rec, 0, sizeof(rec));
    rec.ktime_ns = bpf_ktime_get_ns();
    rec.waker_tid = (__u32)(bpf_get_current_pid_tgid() & 0xFFFFFFFFULL);
    rec.target_cpu = target_cpu;
    __u64 uret = bpf_map_update_elem(&wakeup_cache, &pid, &rec, 0);
    if (uret != 0) {
        inc_counter(COUNTER_WAKEUP_MAP_FAIL);
    }
    return 0;
}

char LICENSE[] SEC("license") = "GPL";

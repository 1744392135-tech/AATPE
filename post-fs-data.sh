#!/system/bin/sh
MODDIR="${0%/*}"
if [ ! -d "$MODDIR/system/bin" ]; then
    for p in /data/adb/modules/aatpe /data/adb/ksu/modules/aatpe /data/adb/ap/modules/aatpe /data/adb/modules_update/aatpe; do
        if [ -d "$p/system/bin" ]; then MODDIR="$p"; break; fi
    done
fi
RUN_DIR="/dev/aatpe"

PERSIST_DIR="/data/adb/aatpe"
BOOT_MARKER="$PERSIST_DIR/boot_in_progress"
FAIL_FILE="$PERSIST_DIR/boot_fail_count"
LATCH_FILE="$PERSIST_DIR/self_disabled_latch"
BOOT_FAIL_LIMIT=3
mkdir -p "$PERSIST_DIR" 2>/dev/null
chmod 0700 "$PERSIST_DIR" 2>/dev/null
chown 0:0 "$PERSIST_DIR" 2>/dev/null
CUR_BOOT_ID="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d ' \r')"

if [ -f "$BOOT_MARKER" ]; then
    PREV_BOOT_ID="$(cat "$BOOT_MARKER" 2>/dev/null | tr -d ' \r')"
    if [ -n "$CUR_BOOT_ID" ] && [ "$PREV_BOOT_ID" = "$CUR_BOOT_ID" ]; then
        :
    elif [ -f "$LATCH_FILE" ]; then
        rm -f "$BOOT_MARKER" "$LATCH_FILE" 2>/dev/null
        echo "0" > "$FAIL_FILE" 2>/dev/null
        echo "$(date '+%F %T') recovery: user re-enabled, fresh boot allowed" >> "$PERSIST_DIR/restore.log" 2>/dev/null
    else
        _fc="$(cat "$FAIL_FILE" 2>/dev/null | tr -d ' \r')"
        case "$_fc" in ''|*[!0-9]*) _fc=0 ;; esac
        _fc=$((_fc + 1))
        echo "$_fc" > "$FAIL_FILE" 2>/dev/null
        if [ "$_fc" -ge "$BOOT_FAIL_LIMIT" ]; then
            touch "$MODDIR/disable" "$LATCH_FILE" 2>/dev/null
            echo "$(date '+%F %T') bootloop self-disable: $_fc consecutive failed boots" >> "$PERSIST_DIR/restore.log" 2>/dev/null
            exit 0
        fi
    fi
fi
echo "$CUR_BOOT_ID" > "$BOOT_MARKER" 2>/dev/null

mkdir -p "$RUN_DIR" "$RUN_DIR/state" "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" "$RUN_DIR/helper"
chmod 0755 "$RUN_DIR" 2>/dev/null
chmod 0700 "$RUN_DIR/state" 2>/dev/null
chmod 0755 "$RUN_DIR/helper" 2>/dev/null
chmod 0700 "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" 2>/dev/null
chcon -R u:object_r:system_data_file:s0 "$RUN_DIR/helper" 2>/dev/null
chmod 0644 "$RUN_DIR"/helper/* 2>/dev/null
echo "$MODDIR" > "$RUN_DIR/moddir"
chmod 0600 "$RUN_DIR/moddir" 2>/dev/null

echo "[$(date '+%Y-%m-%d %H:%M:%S')] AATPE post-fs-data started (v6.0.0 C++ Edition)" > "$RUN_DIR/aatpe.log"
chmod 0600 "$RUN_DIR/aatpe.log" 2>/dev/null

if [ ! -d /sys/kernel/debug/tracing ]; then
    mount -t debugfs none /sys/kernel/debug 2>/dev/null
    if [ -d /sys/kernel/debug/tracing ]; then
        echo "1" > "$RUN_DIR/aatpe_mounted_debugfs"
        echo "[eBPF] debugfs mount attempted by AATPE" >> "$RUN_DIR/aatpe.log"
    fi
fi
if [ ! -d /sys/kernel/tracing/events ]; then
    mount -t tracefs none /sys/kernel/tracing 2>/dev/null
    if [ -d /sys/kernel/tracing/events ]; then
        echo "1" > "$RUN_DIR/aatpe_mounted_tracefs"
        echo "[eBPF] tracefs mount attempted by AATPE" >> "$RUN_DIR/aatpe.log"
    fi
fi
echo "[eBPF] perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null) (detect-only, not modified)" >> "$RUN_DIR/aatpe.log"
MAGISK_POLICY_APPLIED=0
SUPOLICY_APPLIED=0
if command -v magiskpolicy >/dev/null 2>&1; then
    magiskpolicy --live "allow magisk self bpf { map_create prog_load map_read map_write }" 2>/dev/null
    magiskpolicy --live "allow magisk self perf_event { open read write ioctl }" 2>/dev/null
    MAGISK_POLICY_APPLIED=1
    echo "[eBPF] magiskpolicy minimal (magisk self bpf+perf_event) applied" >> "$RUN_DIR/aatpe.log"
fi
if command -v supolicy >/dev/null 2>&1; then
    supolicy --live "allow init init bpf { map_create prog_load map_read map_write }" 2>/dev/null
    supolicy --live "allow init init perf_event { open read write ioctl }" 2>/dev/null
    supolicy --live "allow su su bpf { map_create prog_load map_read map_write }" 2>/dev/null
    supolicy --live "allow su su perf_event { open read write ioctl }" 2>/dev/null
    supolicy --live "allow magisk magisk bpf { map_create prog_load map_read map_write }" 2>/dev/null
    supolicy --live "allow magisk magisk perf_event { open read write ioctl }" 2>/dev/null
    SUPOLICY_APPLIED=1
    echo "[eBPF] supolicy minimal (init/su/magisk self bpf+perf_event) applied" >> "$RUN_DIR/aatpe.log"
fi
if [ "$MAGISK_POLICY_APPLIED" = "0" ] && [ "$SUPOLICY_APPLIED" = "0" ]; then
    echo "[eBPF] WARNING: no magiskpolicy/supolicy available; eBPF load may fail under SELinux enforcing (daemon will NOT setenforce 0)" >> "$RUN_DIR/aatpe.log"
fi
echo "[eBPF] SELinux enforce=$(cat /sys/fs/selinux/enforce 2>/dev/null) (daemon detect-only, not modified)" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] tracepoint id (debugfs): $(cat /sys/kernel/debug/tracing/events/sched/sched_switch/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] tracepoint id (tracefs): $(cat /sys/kernel/tracing/events/sched/sched_switch/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_process_fork id: $(cat /sys/kernel/tracing/events/sched/sched_process_fork/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_process_exit id: $(cat /sys/kernel/tracing/events/sched/sched_process_exit/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_process_exec id: $(cat /sys/kernel/tracing/events/sched/sched_process_exec/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_wakeup id: $(cat /sys/kernel/tracing/events/sched/sched_wakeup/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_wakeup_new id: $(cat /sys/kernel/tracing/events/sched/sched_wakeup_new/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] unprivileged_bpf_disabled=$(cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null || echo 'unknown')" >> "$RUN_DIR/aatpe.log"

echo "[*] post-fs-data complete" >> "$RUN_DIR/aatpe.log"

#!/system/bin/sh
# set -u removed: KernelSU sources scripts, unset vars cause immediate exit
# AATPE post-fs-data.sh (C++ Edition)
# fix22: state dir 0755 (was 0750 in fix20) so WebView file:// fetch can read
# Runs before zygote, sets up runtime environment
MODDIR="${0%/*}"
if [ ! -d "$MODDIR/system/bin" ]; then
    for p in /data/adb/modules/aatpe /data/adb/ksu/modules/aatpe /data/adb/ap/modules/aatpe /data/adb/modules_update/aatpe; do
        if [ -d "$p/system/bin" ]; then MODDIR="$p"; break; fi
    done
fi
RUN_DIR="/dev/aatpe"

# ============================================================================
# G10-003 / ROUND-003: bootloop self-disable with re-enable recovery.
# State machine (persistent state under /data/adb/aatpe):
#   boot_in_progress   = file whose CONTENT is the boot_id that opened the boot.
#   boot_fail_count    = consecutive incomplete boots.
#   self_disabled_latch= created together with $MODDIR/disable the moment we
#                        self-disable. It records "we have already latched off".
#   A boot is "complete" only if boot-completed.sh runs, which deletes the
#   marker and resets the counter.
#
#   Magisk/KSU/APatch skip ALL module scripts while $MODDIR/disable exists, so
#   boot-completed.sh cannot clear the state during a disabled period. The fix:
#   the moment we self-disable we also set self_disabled_latch. On the NEXT boot
#   (which runs post-fs-data only because the USER removed `disable` in the
#   Manager), a stale marker + latch means "recovery attempt": we clear the stale
#   counter/marker and give THIS reboot one full chance instead of immediately
#   re-disabling. A genuinely failing device that has not been re-enabled still
#   accumulates count every incomplete boot and self-disables at the limit.
# ============================================================================
PERSIST_DIR="/data/adb/aatpe"
BOOT_MARKER="$PERSIST_DIR/boot_in_progress"
FAIL_FILE="$PERSIST_DIR/boot_fail_count"
LATCH_FILE="$PERSIST_DIR/self_disabled_latch"
BOOT_FAIL_LIMIT=3
mkdir -p "$PERSIST_DIR" 2>/dev/null
# P3 hardening: the persistent state dir holds manifest.sha256, the re-anchor
# marker, install_id/boot_id and other trust anchors. It MUST be root-private
# (0700) so no other context can read/replace the baseline or forge the marker.
# chmod is idempotent: it converges an already-existing dir to 0700 without
# touching contents. /data/adb itself is root-only, but we enforce it explicitly
# so a prior loose install cannot persist across upgrades.
chmod 0700 "$PERSIST_DIR" 2>/dev/null
chown 0:0 "$PERSIST_DIR" 2>/dev/null
CUR_BOOT_ID="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d ' \r')"

if [ -f "$BOOT_MARKER" ]; then
    PREV_BOOT_ID="$(cat "$BOOT_MARKER" 2>/dev/null | tr -d ' \r')"
    if [ -n "$CUR_BOOT_ID" ] && [ "$PREV_BOOT_ID" = "$CUR_BOOT_ID" ]; then
        # Same boot re-entering post-fs-data (e.g. manual re-run): not a new failure.
        :
    elif [ -f "$LATCH_FILE" ]; then
        # Recovery path: user removed `disable` and rebooted. The leftover marker and
        # counter are stale from the disabled period. Give this boot a fresh chance.
        rm -f "$BOOT_MARKER" "$LATCH_FILE" 2>/dev/null
        echo "0" > "$FAIL_FILE" 2>/dev/null
        echo "$(date '+%F %T') recovery: user re-enabled, fresh boot allowed" >> "$PERSIST_DIR/restore.log" 2>/dev/null
    else
        # A real previous boot never reached boot-completed.sh.
        _fc="$(cat "$FAIL_FILE" 2>/dev/null | tr -d ' \r')"
        case "$_fc" in ''|*[!0-9]*) _fc=0 ;; esac
        _fc=$((_fc + 1))
        echo "$_fc" > "$FAIL_FILE" 2>/dev/null
        if [ "$_fc" -ge "$BOOT_FAIL_LIMIT" ]; then
            touch "$MODDIR/disable" "$LATCH_FILE" 2>/dev/null
            echo "$(date '+%F %T') bootloop self-disable: $_fc consecutive failed boots" >> "$PERSIST_DIR/restore.log" 2>/dev/null
            # Do NOT proceed with our runtime setup on a self-disabled module.
            exit 0
        fi
    fi
fi
# Open this boot (content = current boot_id so a same-boot re-entry is ignored).
echo "$CUR_BOOT_ID" > "$BOOT_MARKER" 2>/dev/null

# Create runtime directories
mkdir -p "$RUN_DIR" "$RUN_DIR/state" "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" "$RUN_DIR/helper"
# fix22: state 0755 (WebView file:// fetch needs world-read; daemon runs as root anyway)
# security hardening (item8): state/ is INTERNAL daemon state. Tightened to
# 0700 (root-only dir) + files 0600. The WebUI does NOT need direct filesystem read:
# its canonical channel is the daemon loopback HTTP server (127.0.0.1:8888-8899)
# serving /state/all JSON, and the helper APK reads/writes via root exec (KSU/Magisk),
# both of which bypass DAC. The world-facing export channel is helper/ (kept 0755 +
# system_data_file below). The file:// Tier-3 fallback in WebUI now requires root.
chmod 0755 "$RUN_DIR" 2>/dev/null
chmod 0700 "$RUN_DIR/state" 2>/dev/null
chmod 0755 "$RUN_DIR/helper" 2>/dev/null
# #73-#76: unified internal-dir security policy. metrics/decisions/thread_db/
# history/rules hold internal daemon data and are consumed only by the daemon and
# the loopback HTTP /state/all channel (not by third-party apps). Tighten to 0700
# root-only like state/. Only helper/ stays world-readable (APK export channel).
chmod 0700 "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" 2>/dev/null
# I-004 (v4.3.4): removed the dead `chmod 0600 "$RUN_DIR/state"/*` here. At
# post-fs-data time the state/ dir was just mkdir'd on empty tmpfs (dev/aatpe
# is wiped every boot), so the glob matched zero files -- it was a no-op. The
# correct 0600 tightening happens in service.sh AFTER the daemon starts, via
# `find state -maxdepth 1 -type f -exec chmod 0600 {} +` (which scopes to
# regular files only, not subdirs). The persistent fix for newly-written files
# lives in C++ aatped_utils.cpp writeFile()/writeFileAtomic().
# Set SELinux context so third-party apps (APK) can read /dev/aatpe/helper/*
# NOTE: Do NOT chcon $RUN_DIR itself to system_data_file.
# Changing /dev/aatpe from default 'device' context to 'system_data_file'
# causes the daemon's lock/pid files and popen()-based dumpsys calls to
# inherit a restricted context, breaking foreground app detection on KernelSU.
# Only helper/ needs system_data_file for third-party APK read access.
chcon -R u:object_r:system_data_file:s0 "$RUN_DIR/helper" 2>/dev/null
chmod 0644 "$RUN_DIR"/helper/* 2>/dev/null
# Record module path for other scripts
# R9-016 / LED-149: moddir holds the absolute on-disk module path (info leak to any
# local app via the world-readable /dev/aatpe/moddir). Tighten to 0600 root-only;
# service.sh recomputes MODDIR from $0 anyway.
echo "$MODDIR" > "$RUN_DIR/moddir"
chmod 0600 "$RUN_DIR/moddir" 2>/dev/null

# Initialize log (before eBPF setup so eBPF logs append correctly)
echo "[$(date '+%Y-%m-%d %H:%M:%S')] AATPE post-fs-data started (v6.0.0 C++ Edition)" > "$RUN_DIR/aatpe.log"
# #73: aatpe.log contains system internals -> 0600 root-only (was 0644 world-readable).
chmod 0600 "$RUN_DIR/aatpe.log" 2>/dev/null

# fix104 EMBEDDED-FIX: eBPF environment setup (zero-dependency embedded bytecode loader)
# P1-16 real-original aligned: NO perf_event_paranoid modification (security),
#   NO setenforce 0 (daemon detect-only), multi-strategy SELinux rules.
# 1. Mount debugfs if not mounted (needed for tracepoint id)
# #47: record whether AATPE itself performed the mount so uninstall.sh can umount
# ONLY what we created (never unmount a debugfs that the platform already had).
if [ ! -d /sys/kernel/debug/tracing ]; then
    mount -t debugfs none /sys/kernel/debug 2>/dev/null
    if [ -d /sys/kernel/debug/tracing ]; then
        echo "1" > "$RUN_DIR/aatpe_mounted_debugfs"
        echo "[eBPF] debugfs mount attempted by AATPE" >> "$RUN_DIR/aatpe.log"
    fi
fi
# 2. Mount tracefs if not mounted (alternative path for tracepoint id)
if [ ! -d /sys/kernel/tracing/events ]; then
    mount -t tracefs none /sys/kernel/tracing 2>/dev/null
    if [ -d /sys/kernel/tracing/events ]; then
        echo "1" > "$RUN_DIR/aatpe_mounted_tracefs"
        echo "[eBPF] tracefs mount attempted by AATPE" >> "$RUN_DIR/aatpe.log"
    fi
fi
# 3. P1-16: perf_event_paranoid — DETECT ONLY, do NOT modify (security)
echo "[eBPF] perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null) (detect-only, not modified)" >> "$RUN_DIR/aatpe.log"
# 4. SELinux rules — item 17 minimal-permission.
#    The AATPE daemon loads its own eBPF programs and opens perf_event ONLY in
#    the domain it actually runs in: Magisk launches service.sh in the `magisk`
#    domain; KernelSU runs post-fs-data/service in the `init` domain. We grant
#    bpf + perf_event to THAT domain only (self). Previously we also granted
#    zygote/rootfs/shell/kernel/magisk->bpf_prog cross-domain rules, which is
#    far broader than the daemon needs. Those are removed; if a future device
#    genuinely needs another domain, add it explicitly with a comment rather
#    than re-broadcasting.
MAGISK_POLICY_APPLIED=0
SUPOLICY_APPLIED=0
if command -v magiskpolicy >/dev/null 2>&1; then
    # Magisk: daemon runs in the magisk domain -> only magisk self needs bpf+perf.
    magiskpolicy --live "allow magisk self bpf { map_create prog_load map_read map_write }" 2>/dev/null
    magiskpolicy --live "allow magisk self perf_event { open read write ioctl }" 2>/dev/null
    # (dropped: allow magisk bpf_prog ... / kernel_t module_request / init / shell)
    MAGISK_POLICY_APPLIED=1
    echo "[eBPF] magiskpolicy minimal (magisk self bpf+perf_event) applied" >> "$RUN_DIR/aatpe.log"
fi
if command -v supolicy >/dev/null 2>&1; then
    # KernelSU: daemon runs in the init domain -> only init self needs bpf+perf.
    supolicy --live "allow init init bpf { map_create prog_load map_read map_write }" 2>/dev/null
    supolicy --live "allow init init perf_event { open read write ioctl }" 2>/dev/null
    # P0-2: KernelSU actually execs su-owned helpers / the daemon may be launched
    # from the `su` domain (e.g. when the APK root-execs aatped). Grant the su
    # domain the same bpf+perf_event self rules; without these, BPF_PROG_LOAD /
    # perf_event_open are denied in the su domain even when init has them.
    supolicy --live "allow su su bpf { map_create prog_load map_read map_write }" 2>/dev/null
    supolicy --live "allow su su perf_event { open read write ioctl }" 2>/dev/null
    # If KernelSU is configured to use the magisk domain (KSU with magiskpolicy
    # shim / APatch-compatible mode), also cover magisk self.
    supolicy --live "allow magisk magisk bpf { map_create prog_load map_read map_write }" 2>/dev/null
    supolicy --live "allow magisk magisk perf_event { open read write ioctl }" 2>/dev/null
    # (dropped: zygote / rootfs / kernel / shell domains — not used by the daemon)
    SUPOLICY_APPLIED=1
    echo "[eBPF] supolicy minimal (init/su/magisk self bpf+perf_event) applied" >> "$RUN_DIR/aatpe.log"
fi
if [ "$MAGISK_POLICY_APPLIED" = "0" ] && [ "$SUPOLICY_APPLIED" = "0" ]; then
    echo "[eBPF] WARNING: no magiskpolicy/supolicy available; eBPF load may fail under SELinux enforcing (daemon will NOT setenforce 0)" >> "$RUN_DIR/aatpe.log"
fi
echo "[eBPF] SELinux enforce=$(cat /sys/fs/selinux/enforce 2>/dev/null) (daemon detect-only, not modified)" >> "$RUN_DIR/aatpe.log"
# 5. Log eBPF environment diagnostics
echo "[eBPF] tracepoint id (debugfs): $(cat /sys/kernel/debug/tracing/events/sched/sched_switch/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] tracepoint id (tracefs): $(cat /sys/kernel/tracing/events/sched/sched_switch/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_process_fork id: $(cat /sys/kernel/tracing/events/sched/sched_process_fork/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_process_exit id: $(cat /sys/kernel/tracing/events/sched/sched_process_exit/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_process_exec id: $(cat /sys/kernel/tracing/events/sched/sched_process_exec/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
# R9-005 / LED-144: the eBPF sched-events program ALSO attaches the sched_wakeup and
# sched_wakeup_new tracepoints, but their ids were never logged. On a 4.14 kernel
# that lacks them the BPF attach would fail silently; logging the ids lets the log
# tell the operator WHY wakeup accounting came out empty (missing tracepoint vs.
# wrong offset).
echo "[eBPF] sched_wakeup id: $(cat /sys/kernel/tracing/events/sched/sched_wakeup/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] sched_wakeup_new id: $(cat /sys/kernel/tracing/events/sched/sched_wakeup_new/id 2>/dev/null || echo 'NOT FOUND')" >> "$RUN_DIR/aatpe.log"
echo "[eBPF] unprivileged_bpf_disabled=$(cat /proc/sys/kernel/unprivileged_bpf_disabled 2>/dev/null || echo 'unknown')" >> "$RUN_DIR/aatpe.log"

# Initialize CPU topology via C++ binary
# P1-8: post-fs-data runs before zygote / late_start; probing CPU
# topology here was too early (cpufreq / cpuset nodes not yet settled, and the
# daemon binary's --init-topology raced against first uevents). post-fs-data now
# only creates runtime dirs; --init-topology runs in service.sh after
# boot_completed + sleep 5, when sysfs is fully populated.
# (moved to service.sh)
# I-008 (v4.3.4): removed the per-boot backup of /dev/cpuset/cpuset.cpus and
# .mems to $RUN_DIR/cpuset.backup/. ROUND-013 already declared the legacy
# restore path REMOVED (uninstall.sh just `rm -rf`s cpuset.backup, it never
# restores from it), so the backup was dead I/O on every boot with no consumer.
# AATPE never rewrites the top-level cpuset partition; it only writes tids into
# group task lists, which are restored from recovery/*.rec on uninstall.
echo "[*] post-fs-data complete" >> "$RUN_DIR/aatpe.log"

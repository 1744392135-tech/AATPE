#!/system/bin/sh
# set -u removed: KernelSU sources scripts, unset vars cause immediate exit
# AATPE service.sh (C++ Edition)
# fix20 P1-5: daemon PID detection via daemon.pid file (not $!)
# fix22: state dir 0755 (was 0750) so WebView file:// fetch can read
# Runs in late_start service mode, starts daemon and watchdog
MODDIR="${0%/*}"
if [ ! -d "$MODDIR/system/bin" ]; then
    # 问题11 fix: align with getModuleDir()'s 4-candidate list (adds APatch
    # and Magisk pending-update slots).
    for p in /data/adb/modules/aatpe /data/adb/ksu/modules/aatpe /data/adb/ap/modules/aatpe /data/adb/modules_update/aatpe; do
        if [ -d "$p/system/bin" ]; then MODDIR="$p"; break; fi
    done
fi
RUN_DIR="/dev/aatpe"
LOG_FILE="$RUN_DIR/aatpe.log"
# Wait for boot to complete. R9-002 / LED-133: cap the wait so a stuck
# sys.boot_completed property can never hang service.sh forever (and leave the
# watchdog/daemon dead with no log). We still START after the cap rather than
# aborting -- the daemon is fail-closed on missing state anyway -- but we must not
# block indefinitely.
_boot_wait=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$_boot_wait" -lt 120 ]; do
    sleep 1
    _boot_wait=$((_boot_wait + 1))
done
if [ "$(getprop sys.boot_completed)" != "1" ]; then
    echo "[WARN] sys.boot_completed still !=1 after ${_boot_wait}s; continuing anyway" >> "$LOG_FILE"
fi
sleep 5
# Ensure runtime dirs exist
mkdir -p "$RUN_DIR" "$RUN_DIR/state" "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/helper" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" 2>/dev/null
# fix22: state 0755 (WebView file:// fetch needs world-read)
# (item8): state/ is internal daemon state -> 0700 dir. WebUI reads via
# loopback HTTP /state/all (primary); helper APK uses root exec. helper/ stays the
# world-readable export channel. Files tightened to 0600 after daemon start below.
# C1 (W1-04): RUN_DIR itself 0711 (not 0755). Other apps need --x only to TRAVERSE
# into helper/ (the APK export channel); they do NOT need --r to list the runtime
# dir (pid/state/metrics names). 0711 keeps helper/ reachable while hiding the
# directory listing. All sensitive files are already 0600 and subdirs 0700.
chmod 0711 "$RUN_DIR" 2>/dev/null
chmod 0700 "$RUN_DIR/state" 2>/dev/null
chmod 0755 "$RUN_DIR/helper" 2>/dev/null
# #73-#76: internal data dirs unified to 0700 root-only (WebUI reads via loopback
# /state/all, not direct filesystem access). helper/ remains the export channel.
chmod 0700 "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" 2>/dev/null
# #73: aatpe.log holds system internals -> 0600 root-only.
chmod 0600 "$LOG_FILE" 2>/dev/null

# fix103 P1-3.5: SELinux context for runtime files
# NOTE (problem2): Must stay consistent with post-fs-data.sh. Only helper/ gets
# system_data_file. Do NOT chcon the whole $RUN_DIR to system_data_file, or the
# daemon's lock/pid files inherit a restricted context and break foreground-app
# detection on KernelSU.
SELINUX_CTX="u:object_r:system_data_file:s0"
[ -d "$RUN_DIR/helper" ] && chcon -R "$SELINUX_CTX" "$RUN_DIR/helper" 2>/dev/null
# Set context for binaries in module directory (system_file)
[ -d "$MODDIR/system/bin" ] && chcon -R "u:object_r:system_file:s0" "$MODDIR/system/bin" 2>/dev/null
# Set context for config files (problem13: real config lives under system/etc)
[ -f "$MODDIR/system/etc/aatpe/aatpe.conf" ] && chcon "$SELINUX_CTX" "$MODDIR/system/etc/aatpe/aatpe.conf" 2>/dev/null
echo "[$(date '+%Y-%m-%d %H:%M:%S')] AATPE service starting (MODDIR=$MODDIR)" >> "$LOG_FILE"
# P1-8: initialize CPU topology HERE (after boot_completed + sleep 5),
# not in post-fs-data.sh. At post-fs-data time cpufreq/cpuset/thermal nodes are
# not yet settled; probing then produced bogus pools that the daemon had to
# rediscover later.
if [ -x "$MODDIR/system/bin/aatped" ]; then
    "$MODDIR/system/bin/aatped" --init-topology >> "$LOG_FILE" 2>&1
fi
# Run environment probe (problem: use module path, not systemless /system/bin)
# R9-007 / LED-138: guard with [ -x ] like aatped/watchdog above; a missing probe
# otherwise only leaves a bare "not found" line in the log and looks identical to
# a successful-but-empty run.
if [ -x "$MODDIR/system/bin/aatpe-probe" ]; then
    "$MODDIR/system/bin/aatpe-probe" >> "$LOG_FILE" 2>&1
else
    echo "[WARN] aatpe-probe not found/executable at $MODDIR/system/bin/aatpe-probe; skipping" >> "$LOG_FILE"
fi
# Verify binaries exist
if [ ! -x "$MODDIR/system/bin/aatped" ]; then
    echo "[ERROR] aatped binary not found or not executable at $MODDIR/system/bin/aatped" >> "$LOG_FILE"
    exit 1
fi
if [ ! -x "$MODDIR/system/bin/aatpe-watchdog" ]; then
    echo "[ERROR] aatpe-watchdog binary not found at $MODDIR/system/bin/aatpe-watchdog" >> "$LOG_FILE"
fi

# ---- W3-TASK A: consume trusted-install re-anchor request (BEFORE verify) ----
# customize.sh drops /data/adb/aatpe/manifest_reanchor.pending during a trusted
# install/upgrade. On the first boot after the modules_update->modules swap, the
# on-disk payload is the FINAL settled tree. If the recovery-time generate failed
# (or the upgrade left a stale pre-upgrade manifest), regenerate the baseline
# NOW against $MODDIR -- BEFORE the verify below and BEFORE the daemon forks --
# so the verify never compares new files against a stale baseline.
#
# Safety: the marker is created ONLY by the trusted installer. If it is absent
# (normal boot) we do NOTHING here and fall through to the fail-closed verify.
# On regen failure we LEAVE the marker for the next boot and still run the
# fail-closed verify (we never weaken verification to paper over the failure).
REANCHOR_MARKER="/data/adb/aatpe/manifest_reanchor.pending"
# P3 hardening: converge the persistent trust-anchor dir to root-private 0700 on
# every boot (idempotent). Guards against a prior loose install / manual tweak
# leaving the world/group able to read or replace the baseline or marker.
chmod 0700 /data/adb/aatpe 2>/dev/null
chown 0:0 /data/adb/aatpe 2>/dev/null
# Converge the re-anchor marker itself to 0600 if it exists (root-only).
[ -f "$REANCHOR_MARKER" ] && chmod 0600 "$REANCHOR_MARKER" 2>/dev/null
if [ -f "$REANCHOR_MARKER" ]; then
    echo "[*] Consuming trusted-install re-anchor marker; re-baselining integrity manifest against $MODDIR" >> "$LOG_FILE"
    # Anchor the FINAL settled module dir explicitly (argv wins over env/probe;
    # an invalid root makes aatped exit non-zero instead of silently probing a
    # wrong tree). Refuse to proceed if the binary is not runnable here.
    if [ -x "$MODDIR/system/bin/aatped" ] && [ -d "$MODDIR/system/bin" ]; then
        if "$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR" >> "$LOG_FILE" 2>&1 \
           && [ -f /data/adb/aatpe/manifest.sha256 ]; then
            echo "[OK] Re-anchor: manifest regenerated against $MODDIR" >> "$LOG_FILE"
            rm -f "$REANCHOR_MARKER" 2>/dev/null
        else
            echo "[WARN] Re-anchor: --generate-manifest failed; keeping marker for next boot, verify will be fail-closed" >> "$LOG_FILE"
        fi
    else
        echo "[WARN] Re-anchor: $MODDIR/system/bin/aatped not runnable; keeping marker for next boot" >> "$LOG_FILE"
    fi
fi

# ---- Integrity verification BEFORE daemon start (SECURITY FIX) -------
# Previously this block ran AFTER `aatped --daemon` was already spawned (old
# line 167+), meaning a potentially tampered binary was already executing
# before we verified it. moves the check to BEFORE the daemon fork so
# the verification actually guards the long-running process.
#
# Result policy (I-001 / A1-006, v4.3.4: STRICT FAIL-CLOSED):
#   - manifest missing  -> attempt bootstrap-generate (first-install fallback).
#                          customize.sh normally generates the manifest during
#                          trusted install; this path only triggers if that
#                          step did not run. If the bootstrap attempt does NOT
#                          produce a manifest, we exit 1 here -- a live daemon
#                          must never run with no baseline to verify against.
#   - verify OK        -> daemon starts normally.
#   - verify FAILED    -> we DO NOT start the daemon and DO NOT apply any
#                          scheduling action (no promoted_tids sweep, no
#                          uclamp/boost residue reset, no daemon fork, no
#                          watchdog, no supervisor). We persist the DEGRADED
#                          verdict for the WebUI/operator and exit 1. The
#                          manifest is NEVER auto-regenerated: baking a tampered
#                          file into the baseline would erase the tamper
#                          evidence. Reinstall module to reset.
if [ -x "$MODDIR/system/bin/aatped" ]; then
    echo "[*] Running module integrity verification (SHA-256 hash chain)..." >> "$LOG_FILE"

    # If manifest does not exist, bootstrap one first.
    # NOTE: customize.sh generates the manifest during the trusted install
    # action. This fallback only triggers if that step did not run (e.g. manual
    # file copy or upgrade edge case) -- it is NOT a runtime auto-recovery.
    if [ ! -f /data/adb/aatpe/manifest.sha256 ]; then
        echo "[*] No integrity manifest found, generating bootstrap (first-install fallback)..." >> "$LOG_FILE"
        # ROUND-002: pass $MODDIR explicitly (the active module dir at runtime) so
        # generation never guesses between modules/ and modules_update/.
        "$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR" >> "$LOG_FILE" 2>&1
        # A1-006 (v4.3.4): conservative. If the bootstrap generation did not
        # produce a manifest, do NOT proceed to verify (which would run against a
        # missing baseline and either auto-promote current bytes or fail open).
        # Fail closed: persist DEGRADED and exit before any scheduling action.
        if [ ! -f /data/adb/aatpe/manifest.sha256 ]; then
            echo "[FATAL] Bootstrap integrity manifest generation failed; /data/adb/aatpe/manifest.sha256 absent." >> "$LOG_FILE"
            echo "[FATAL] fail-closed: NOT starting daemon and NOT applying any scheduling action. Reinstall module." >> "$LOG_FILE"
            echo "DEGRADED:integrity=bootstrap_manifest_missing:reinstall_required" > "$RUN_DIR/state/integrity_degraded" 2>/dev/null
            chmod 0600 "$RUN_DIR/state/integrity_degraded" 2>/dev/null
            exit 1
        fi
    fi

    # Run verification against the stored baseline.
    "$MODDIR/system/bin/aatped" --verify-integrity "$MODDIR" >> "$LOG_FILE" 2>&1
    _int_status=$(cat "$RUN_DIR/state/integrity_status" 2>/dev/null)
    if [ "$_int_status" = "OK" ]; then
        echo "[OK] Module integrity verified (SHA-256 hash chain OK)" >> "$LOG_FILE"
    else
        # I-001 (v4.3.4): FAIL-CLOSED. Previously the daemon still started in
        # DEGRADED mode (availability-first trade-off). That conflicts with the
        # user's fail-closed red line: a tampered binary must not be allowed to
        # apply any scheduling action (affinity/policy/uclamp/cgroup/boost). We
        # persist the DEGRADED verdict for the WebUI/operator and exit NOW,
        # before the promoted_tids sweep, the uclamp/boost residue resets, the
        # daemon fork, the watchdog fork, and the supervisor loop. Manifest is
        # NEVER auto-regenerated (would erase tamper evidence). Reinstall.
        echo "[FATAL] Integrity FAILED - module files may have been tampered." >> "$LOG_FILE"
        echo "[FATAL] Manifest will NOT be auto-regenerated. Reinstall module to reset." >> "$LOG_FILE"
        echo "[FATAL] Module integrity status: ${_int_status:-UNKNOWN}" >> "$LOG_FILE"
        echo "[FATAL] fail-closed: NOT starting daemon and NOT applying any scheduling action." >> "$LOG_FILE"
        echo "DEGRADED:integrity=${_int_status:-FAILED}:reinstall_required" > "$RUN_DIR/state/integrity_degraded" 2>/dev/null
        chmod 0600 "$RUN_DIR/state/integrity_degraded" 2>/dev/null
        exit 1
    fi
fi

# ROUND-010 T2 (D6-P1-2): SIGKILL boot sweep for promoted-thread residue.
# Contract (placement.cpp), one row per line:  tid:starttime:field3:mechanism
#   field3 = cpulist ("0-3"/"0,1,2,3") -> taskset -p restore affinity
#           = "grp=<group>"             -> write tid back into that cgroup group
#           = "-"                        -> task-level uclamp only, no action
# A row is applied ONLY if the live task still exists AND its live starttime
# (stat field 22) equals the recorded one (TID-reuse guard). Only threads the
# daemon itself recorded are touched. The file is cleared after the sweep.
PROM="$RUN_DIR/state/promoted_tids"
if [ -f "$PROM" ]; then
    echo "[*] Sweeping residual promoted-thread state from previous session..." >> "$LOG_FILE"
    while IFS=: read -r ptid pstart pfield pmech; do
        [ -n "$ptid" ] || continue
        # F-A6-06[P3]: ptid comes from a root-written state file but must be a
        # strict non-negative integer before it is used in a /proc glob or written
        # to a cgroup tasks file. Reject anything non-numeric (and bound the range)
        # so a tampered/garbage entry cannot inject shell metacharacters. POSIX
        # case glob; [0-9]* anchors, *[!0-9]* catches any embedded non-digit.
        case "$ptid" in
            ''|*[!0-9]*) continue ;;
        esac
        # Bound to the kernel pid_max range (64-bit pid ns <= 4194304).
        [ "$ptid" -le 4194304 ] 2>/dev/null || continue
        [ "$pfield" = "-" ] && continue                 # task-level uclamp, nothing to undo
        tp="$(ls -d /proc/[0-9]*/task/$ptid 2>/dev/null | head -1)"
        [ -n "$tp" ] || continue                       # thread already dead -> nothing to do
        # C09 (v4.1, corrected): starttime lives AFTER the "(comm)" prefix. comm
        # may contain spaces ("Signal Catcher"/"Jit thread pool") or parens, so raw
        # field numbering shifts. Strip everything up to the LAST ')' first (sed
        # greedy), then field 20 of the remainder == starttime (1-indexed; same as
        # uninstall.sh awk '{sub(/^.*\) /,"")}{print $20}' and topology.cpp:166).
        # sed+cut chosen over awk for toybox regex-portability.
        live_start="$(sed 's/^.*) //' "$tp/stat" 2>/dev/null | cut -d' ' -f20)"
        [ -z "$live_start" ] && continue               # stat unreadable -> fail-closed
        [ "$live_start" != "$pstart" ] && continue     # TID reused by another thread
        case "$pfield" in
            grp=*)
                grp="${pfield#grp=}"
                # #6c: cgroup v1 cpuset/cpuctl hierarchies — tasks is thread-level.
                for tgt in "/dev/cpuset/$grp/tasks" "/dev/cpuctl/$grp/tasks"; do
                    if [ -e "$tgt" ]; then
                        echo "$ptid" > "$tgt" 2>>"$LOG_FILE" \
                            || echo "PROM v1 tasks write fail ptid=$ptid -> $tgt" >> "$LOG_FILE"
                    fi
                done
                # #6c: cgroup v2 unified. cgroup.threads is THREAD-level and is the
                # correct target for restoring a single (non-leader) tid back to the
                # recorded group. cgroup.procs is PROCESS-level and only accepts the
                # thread-group leader; writing a non-leader tid there EINVALs/ESRCHs
                # (previously silently swallowed). Write cgroup.threads for every tid;
                # additionally write cgroup.procs only when this tid IS the TGID.
                v2base="/sys/fs/cgroup/$grp"
                if [ -e "$v2base/cgroup.threads" ]; then
                    echo "$ptid" > "$v2base/cgroup.threads" 2>>"$LOG_FILE" \
                        || echo "PROM v2 cgroup.threads write fail ptid=$ptid -> $v2base" >> "$LOG_FILE"
                    live_tgid="$(sed -n 's/^Tgid:[[:space:]]*//p' "$tp/status" 2>/dev/null | tr -d '[:space:]')"
                    if [ "$live_tgid" = "$ptid" ] && [ -e "$v2base/cgroup.procs" ]; then
                        echo "$ptid" > "$v2base/cgroup.procs" 2>>"$LOG_FILE" \
                            || echo "PROM v2 cgroup.procs write fail tgid=$ptid" >> "$LOG_FILE"
                    fi
                fi
                ;;
            *)
                # cpulist affinity restore
                command -v taskset >/dev/null 2>&1 && taskset -cp "$pfield" "$ptid" >> "$LOG_FILE" 2>&1
                ;;
        esac
    done < "$PROM"
    rm -f "$PROM" 2>/dev/null
fi

# ROUND-013: cgroup-uclamp floor boot 兜底. While boosting, G5 writes a
# top-app (and recorded groups) cpu.uclamp.min floor and signals it with
# state/cg_uclamp_active; on a clean exit it restores floor=0 and deletes the
# marker. If the daemon was SIGKILLed the marker survives and the floor leaks
# across reboots. On THIS boot, if the marker exists, reset every known group's
# cpu.uclamp.min to 0 (AOSP default for top-app has no floor) and drop the
# marker. Marker absent -> touch nothing (normal fresh boot).
CGA="$RUN_DIR/state/cg_uclamp_active"
if [ -f "$CGA" ]; then
    echo "[*] cg_uclamp_active residue: resetting top-app cpu.uclamp.min floors..." >> "$LOG_FILE"
    for u in /dev/cpuctl/top-app/cpu.uclamp.min \
             /sys/fs/cgroup/top-app/cpu.uclamp.min \
             /sys/fs/cgroup/cpu.uclamp.min; do
        [ -e "$u" ] && echo 0 > "$u" 2>/dev/null
    done
    # any groups recorded in the marker payload (comma-separated, e.g.
    # "top-app,foreground" or one per line). Split both into one group per line.
    if [ -s "$CGA" ]; then
        tr ',' '\n' < "$CGA" | while IFS= read -r g; do
            g="${g// /}"                       # trim spaces
            [ -n "$g" ] || continue
            [ "$g" = "top-app" ] && continue
            for u in "/dev/cpuctl/$g/cpu.uclamp.min" "/sys/fs/cgroup/$g/cpu.uclamp.min"; do
                [ -e "$u" ] && echo 0 > "$u" 2>/dev/null
            done
        done
    fi
    rm -f "$CGA" 2>/dev/null
fi

# ROUND-014 P3: schedtune.boost boot 兜底 (C-W7). G5 writes the actual chosen
# boost node under gpu_boost_active marker (payload = node path). Only if the
# marker exists do we write 0 to the recorded node(s); NEVER sweep all stune
# nodes unconditionally (would clobber other components / system defaults).
GB="$RUN_DIR/state/gpu_boost_active"
if [ -f "$GB" ]; then
    echo "[*] gpu_boost_active residue: resetting recorded schedtune.boost nodes..." >> "$LOG_FILE"
    if [ -s "$GB" ]; then
        while IFS= read -r node; do
            case "$node" in
                /*) [ -e "$node" ] && echo 0 > "$node" 2>/dev/null ;;  # absolute recorded node
            esac
        done < "$GB"
    fi
    rm -f "$GB" 2>/dev/null
fi

# ROUND-014 P3: schema fence + OTA fingerprint (§2.6, v4.1 schema=1).
# - state/aatpe_state_schema: on version mismatch, .rec/promoted_tids are
#   conservatively cleared (daemon owns the detailed cache rebuild; shell just
#   removes stale thread records to avoid mis-restore). ML model keeps its own
#   magic/CRC; token/key files are NEVER touched here.
# - last_build_fingerprint: if ro.build.fingerprint changed (OTA), drop the
#   capabilities cache so the daemon re-probes on the new kernel/node layout.
SCHEMA="$RUN_DIR/state/aatpe_state_schema"
[ -f "$SCHEMA" ] || echo "1" > "$SCHEMA"
_old_schema="$(cat "$SCHEMA" 2>/dev/null)"
if [ "$_old_schema" != "1" ]; then
    echo "[*] state schema mismatch ($_old_schema->1); clearing thread records" >> "$LOG_FILE"
    rm -rf "$RUN_DIR/recovery" 2>/dev/null
    rm -f "$RUN_DIR/state/promoted_tids" 2>/dev/null
    echo "1" > "$SCHEMA"
fi
unset _old_schema
_fp_now="$(getprop ro.build.fingerprint 2>/dev/null)"
_fp_prev="$(cat "$RUN_DIR/state/last_build_fingerprint" 2>/dev/null)"
if [ -n "$_fp_now" ] && [ "$_fp_now" != "$_fp_prev" ]; then
    echo "[*] build fingerprint changed; dropping capabilities cache for re-probe" >> "$LOG_FILE"
    rm -f "$RUN_DIR/state/scheduler_capabilities" \
          "$RUN_DIR/state/cgroup_uclamp_scale" 2>/dev/null
    echo "$_fp_now" > "$RUN_DIR/state/last_build_fingerprint"
fi
unset _fp_now _fp_prev

# Start main daemon (problem: use module path so we do not depend on
# systemless /system/bin mount; binaries already chcon'd to system_file above)
echo "[*] Starting aatped daemon..." >> "$LOG_FILE"
# v94 fix / 问题48: IDEMPOTENT start. If a daemon is already alive (per its
# pid file), do NOT spawn a second instance. The daemon also takes its own
# flock on daemon.lock, but guarding at the shell avoids spawning-and-failing
# on every re-run of service.sh (manual restart / module toggle).
# P2-16 (round-2): before trusting a PID from daemon.pid, verify the
# process actually IS aatped. A stale/reused PID must not make us think the
# daemon is alive (which would skip spawning a new one) — nor must we ever
# signal an unrelated process. We require /proc/$pid/exe to resolve into the
# aatped binary.
is_aatped_pid() {
    local pid="$1"
    [ -n "$pid" ] || return 1
    [ -d "/proc/$pid" ] || return 1
    local exe
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 1
    # I-006 (v4.3.4): exact-path match, not substring. A process named
    # "my-aatped-backdoor" in /data/local/tmp must NOT be mistaken for our
    # daemon. The only legitimate exe is $MODDIR/system/bin/aatped, which
    # always ends in /system/bin/aatped regardless of the module root.
    case "$exe" in
        */system/bin/aatped) return 0 ;;
        *) return 1 ;;
    esac
}
EXISTING_DAEMON_PID=""
if [ -f "$RUN_DIR/daemon.pid" ]; then
    EXISTING_DAEMON_PID=$(cat "$RUN_DIR/daemon.pid" 2>/dev/null)
fi
if [ -n "$EXISTING_DAEMON_PID" ] && kill -0 "$EXISTING_DAEMON_PID" 2>/dev/null && is_aatped_pid "$EXISTING_DAEMON_PID"; then
    echo "[*] AATPE daemon already running (PID=$EXISTING_DAEMON_PID), skip start (idempotent)" >> "$LOG_FILE"
    DAEMON_PID="$EXISTING_DAEMON_PID"
else
    if [ -n "$EXISTING_DAEMON_PID" ] && ! is_aatped_pid "$EXISTING_DAEMON_PID"; then
        echo "[!] daemon.pid=$EXISTING_DAEMON_PID is not an aatped process (stale/reused), clearing" >> "$LOG_FILE"
    fi
    # Remove stale PID file before starting
    rm -f "$RUN_DIR/daemon.pid" 2>/dev/null
    # ROUND-001 (G9-003): clear stale explicit-stop flag so reboot always revives
    # the daemon. stop_request is only meant to hold for a single uptime; if it
    # survives a reboot the watchdog would not bring the daemon back up.
    rm -f "$RUN_DIR/state/stop_request" 2>/dev/null
    # ISSUE-001 (A7): launch the resident root daemon with a whitelisted env.
    # `env -i` starts from an EMPTY environment, clearing inherited LD_PRELOAD /
    # LD_LIBRARY_PATH / arbitrary AATPE_* that a prior Magisk/KSU module's service
    # may have polluted. We re-export only what the daemon legitimately needs:
    #   PATH            -> aatpeExecArgv fork/exec (frame.cpp:392)
    #   HOME / TZ       -> sane defaults
    #   INIT_RC/KSU/APATCH -> root-impl detection (aatpe_caps.cpp); preserved via
    #                        the ${VAR:+...} idiom ONLY when actually set, so we
    #                        never break KSU/APatch/Magisk detection.
    #   AATPE_MODULE_ROOT -> module root override (aatped_main.cpp:5188)
    env -i \
        PATH=/system/bin:/system/xbin:/vendor/bin \
        HOME=/root \
        TZ="$TZ" \
        ${INIT_RC:+INIT_RC="$INIT_RC"} \
        ${KSU:+KSU="$KSU"} \
        ${APATCH:+APATCH="$APATCH"} \
        AATPE_MODULE_ROOT="$MODDIR" \
        "$MODDIR/system/bin/aatped" --daemon >> "$LOG_FILE" 2>&1 &
    # fix20 P1-5: Do NOT use $! as daemon PID.
    # aatped --daemon calls daemon(0,0) which forks, so $! is the parent
    # process that exits immediately, not the actual daemon.
    # The real daemon writes its PID to $RUN_DIR/daemon.pid via getpid().
    # Wait for the PID file to appear (up to ~5 seconds).
    # R9-021 / LED-152: do not use `for i in $(seq 1 50)` -- on a minimal ROM
    # without toybox seq the loop body never runs at all and the PID wait is
    # silently skipped. Use a pure-shell counter instead (no external seq).
    DAEMON_PID=""
    _i=1
    while [ "$_i" -le 50 ]; do
        if [ -f "$RUN_DIR/daemon.pid" ]; then
            DAEMON_PID=$(cat "$RUN_DIR/daemon.pid" 2>/dev/null)
            if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
                break
            fi
            DAEMON_PID=""
        fi
        sleep 0.1
        _i=$((_i + 1))
    done
fi
# fix20 P2-8: daemon.pid 0600 (R9-008 / LED-139: was 0644, world-readable,
# leaking internal PIDs). Root rw only.
[ -f "$RUN_DIR/daemon.pid" ] && chmod 0600 "$RUN_DIR/daemon.pid" 2>/dev/null
if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "[OK] Daemon started successfully (PID=$DAEMON_PID)" >> "$LOG_FILE"
else
    echo "[ERROR] Daemon failed to start (daemon.pid not found or process not alive)" >> "$LOG_FILE"
    echo "[ERROR] Check log above for crash reason" >> "$LOG_FILE"
fi
# Start watchdog (v94 fix / 问题48: idempotent — skip if already alive)
# 问题8 fix: WATCHDOG_ENABLED config item is no longer dead. service.sh reads
# the installed aatpe.conf and skips spawning aatpe-watchdog entirely when the
# operator (or a profile) sets WATCHDOG_ENABLED=0. Default = 1 (watchdog on).
WATCHDOG_ENABLED="1"
if [ -f "$MODDIR/system/etc/aatpe/aatpe.conf" ]; then
    WATCHDOG_ENABLED=$(grep -m1 '^WATCHDOG_ENABLED=' "$MODDIR/system/etc/aatpe/aatpe.conf" 2>/dev/null | cut -d= -f2 | tr -d ' \r')
    [ -z "$WATCHDOG_ENABLED" ] && WATCHDOG_ENABLED="1"
fi
if [ "$WATCHDOG_ENABLED" = "0" ]; then
    echo "[*] WATCHDOG_ENABLED=0 in config; skipping aatpe-watchdog start" >> "$LOG_FILE"
elif [ -x "$MODDIR/system/bin/aatpe-watchdog" ]; then
    WATCHDOG_PID=""
    if [ -f "$RUN_DIR/watchdog.pid" ]; then
        WATCHDOG_PID=$(cat "$RUN_DIR/watchdog.pid" 2>/dev/null)
    fi
    if [ -n "$WATCHDOG_PID" ] && kill -0 "$WATCHDOG_PID" 2>/dev/null; then
        echo "[*] AATPE watchdog already running (PID=$WATCHDOG_PID), skip start (idempotent)" >> "$LOG_FILE"
    else
        echo "[*] Starting aatpe-watchdog..." >> "$LOG_FILE"
        # ISSUE-001: same whitelisted env as the daemon (clear LD_PRELOAD/LD_LIBRARY_PATH).
        env -i \
            PATH=/system/bin:/system/xbin:/vendor/bin \
            HOME=/root \
            TZ="$TZ" \
            ${INIT_RC:+INIT_RC="$INIT_RC"} \
            ${KSU:+KSU="$KSU"} \
            ${APATCH:+APATCH="$APATCH"} \
            AATPE_MODULE_ROOT="$MODDIR" \
            "$MODDIR/system/bin/aatpe-watchdog" >> "$LOG_FILE" 2>&1 &
        WATCHDOG_PID=$!
        # ROUND-010 T4: atomic pid write (tmp + mv), matches the C++ self-write.
        echo "$WATCHDOG_PID" > "$RUN_DIR/.watchdog.pid.tmp"
        mv -f "$RUN_DIR/.watchdog.pid.tmp" "$RUN_DIR/watchdog.pid"
        chmod 0600 "$RUN_DIR/watchdog.pid" 2>/dev/null
        sleep 1
        if kill -0 "$WATCHDOG_PID" 2>/dev/null; then
            echo "[OK] Watchdog started successfully (PID=$WATCHDOG_PID)" >> "$LOG_FILE"
        else
            echo "[WARN] Watchdog may have exited (PID=$WATCHDOG_PID)" >> "$LOG_FILE"
        fi
    fi
fi

# ROUND-010 T1 (D6-P1-1): persistent supervisor for the watchdog itself.
# service.sh runs once per boot; if the watchdog later dies (OOM, SELinux,
# crash) nothing else would re-launch it, and the daemon's safety net silently
# disappears. We detach a tiny watcher loop that re-spawns a dead watchdog,
# with a rapid-crash backoff identical in spirit to the daemon crash-loop gate.
# The loop backgrounds itself so Magisk/KSU late_start never blocks on it.
# (daemon death is the watchdog's own job; this only guards the guard.)
if [ "$WATCHDOG_ENABLED" != "0" ] && [ -x "$MODDIR/system/bin/aatpe-watchdog" ]; then
    (
        w_since=$(cat "$RUN_DIR/.wd_superv_window" 2>/dev/null || echo 0)
        w_restarts=0
        while :; do
            sleep 10
            wpid=$(cat "$RUN_DIR/watchdog.pid" 2>/dev/null)
            alive=0
            if [ -n "$wpid" ] && [ -d "/proc/$wpid" ]; then
                # C09: strip "(comm)" then field 20 == starttime (comm may contain
                # spaces; same sed+cut form as the promoted_tids sweep above).
                wexe=$(readlink "/proc/$wpid/exe" 2>/dev/null)
                wstart=$(sed 's/^.*) //' "/proc/$wpid/stat" 2>/dev/null | cut -d' ' -f20)
                # C-W: require exe match AND a readable live starttime (PID-reuse guard)
                # I-006 (v4.3.4 follow-up): exact-path match, not substring. The
                # watchdog supervisor must only recognize a real watchdog whose exe
                # lives under /system/bin/aatpe-watchdog (same convention as the
                # daemon's */system/bin/aatped check above). A planted binary named
                # "my-aatpe-watchdog-bkdr" must not be treated as a live watchdog.
                case "$wexe" in */system/bin/aatpe-watchdog)
                    [ -n "$wstart" ] && alive=1 ;;
                esac
            fi
            if [ "$alive" = "1" ]; then continue; fi
            # watchdog is (stale-)dead. Rapid-crash gate: <=5 respawns / 60s.
            now=$(date +%s)
            [ "$now" -lt "$((w_since + 60))" ] || { w_since=$now; w_restarts=0; }
            w_restarts=$((w_restarts + 1))
            echo "$w_since" > "$RUN_DIR/.wd_superv_window" 2>/dev/null
            if [ "$w_restarts" -gt 5 ]; then
                echo "[$(date '+%T')] watchdog supervisor: watchdog crashed $w_restarts in 60s, STOP respawning (manual recovery needed)" >> "$LOG_FILE"
                break
            fi
            echo "[$(date '+%T')] watchdog supervisor: respawning watchdog (attempt $w_restarts)" >> "$LOG_FILE"
            # ISSUE-001: whitelisted env on respawn too.
            env -i \
                PATH=/system/bin:/system/xbin:/vendor/bin \
                HOME=/root \
                TZ="$TZ" \
                ${INIT_RC:+INIT_RC="$INIT_RC"} \
                ${KSU:+KSU="$KSU"} \
                ${APATCH:+APATCH="$APATCH"} \
                AATPE_MODULE_ROOT="$MODDIR" \
                "$MODDIR/system/bin/aatpe-watchdog" >> "$LOG_FILE" 2>&1 &
            neww=$!
            echo "$neww" > "$RUN_DIR/.watchdog.pid.tmp"
            mv -f "$RUN_DIR/.watchdog.pid.tmp" "$RUN_DIR/watchdog.pid"
            chmod 0600 "$RUN_DIR/watchdog.pid" 2>/dev/null
        done
    ) &
    disown 2>/dev/null
fi
# fix20 P2-8: control file 0600 (root only, no other users can write)
[ -f "$RUN_DIR/control" ] && chmod 0600 "$RUN_DIR/control" 2>/dev/null
# (item8): tighten internal state files to 0600 (root-only). Best-effort;
# the daemon keeps (re)writing them via writeFile() (0644 at the C++ layer), so
# this is a baseline sweep -- the persistent fix is in aatped_utils.cpp.
# R9-008 / LED-139: scope the sweep to regular FILES only. `chmod 0600 state/*`
# would also chmod any subdirectory the daemon created to 0600, breaking
# traversal by other tooling. find -maxdepth 1 -type f leaves dirs at 0700.
find "$RUN_DIR/state" -maxdepth 1 -type f -exec chmod 0600 {} + 2>/dev/null

# Backup snapshot informational count (integrity verify already ran above,
# BEFORE daemon start -- see the "Integrity verification BEFORE daemon start"
# block near the top of this script).
if [ -d /data/adb/aatpe/backup ]; then
    # G10-008: snapshots live under backup/<install_id>/*.kv (per-install isolation),
    # not directly in backup/*.kv. Recurse so the count is actually correct.
    _snap_count=$(find /data/adb/aatpe/backup -name '*.kv' -type f 2>/dev/null | wc -l)
    echo "[*] Found $_snap_count snapshot entries in backup dir" >> "$LOG_FILE"
fi

# Final summary
echo "[*] AATPE v6.0.0 C++ Edition service complete" >> "$LOG_FILE"
echo "[*] Mode: $(cat "$RUN_DIR/state/control_mode" 2>/dev/null || echo 'assist')" >> "$LOG_FILE"
echo "[*] Profile: $(cat "$RUN_DIR/state/current_profile" 2>/dev/null || echo 'balanced')" >> "$LOG_FILE"

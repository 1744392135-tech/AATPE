#!/system/bin/sh
# NOTE: deliberately NO 'set -u'. Magisk/KernelSU source these scripts in an
# environment where variables like $REPLACE / $MODDIR may be unset, and
# 'set -u' would abort immediately. Same convention as service.sh / post-fs-data.sh.
# AATPE uninstall.sh -
# ROUND-013 WRITE-NODE INVENTORY: AATPE only changes per-thread affinity/policy/
# priority/nice/uclamp, cgroup MEMBERSHIP (writing a tid into a group's tasks/
# cgroup.procs), and the top-app cpu.uclamp.min boost floor. It does NOT touch
# any governor / freq / kernel-knob / thermal / cooling / GPU / cpuset-group-cpus
# node at runtime. Therefore:
#   - customize.sh records NO system-node .kv snapshots anymore (those node classes
#     are listed only in the header comment); uninstall never writes them back,
#     so another component's later legitimate changes / dynamic runtime state
#     (cur_freq/temp/cur_state) are NOT clobbered.
#   - Thread-level restore comes only from the daemon's own recovery/*.rec, and
#     only when live==applied on EVERY recorded dimension (with TID starttime
#     reuse guard). Unknown/empty saved cgroup group is SKIPPED (never defaulted
#     to foreground).
RUN_DIR="/dev/aatpe"
BACKUP_DIR_BASE="/data/adb/aatpe/backup"
STATE_DIR="/data/adb/aatpe"
RESTORE_LOG="$STATE_DIR/restore.log"
echo "[*] AATPE v6.0.0 uninstall started" >> "$RESTORE_LOG" 2>/dev/null

# ---- item 15: restore ONLY the current install's snapshot -----------------
# customize.sh wrote the active install_id to $STATE_DIR/install_id and stored
# that install's .kv files under $BACKUP_BASE/$INSTALL_ID/. We must NOT walk the
# whole backup dir (that would restore a previous version's stale state). Fall
# back to the legacy flat dir only if no install_id record exists (very old).
INSTALL_ID=""
[ -r "$STATE_DIR/install_id" ] && INSTALL_ID=$(cat "$STATE_DIR/install_id" 2>/dev/null)
# S-002: install_id is attacker-influenced (a file on disk). Reject any value
# that could escape BACKUP_DIR_BASE via path traversal (".."), an absolute path
# (leading "/"), or a bare ".." component. On rejection fall back to the flat
# legacy backup dir instead of walking an attacker-controlled path.
case "$INSTALL_ID" in
    *../*|/*|*".."*)
        echo "invalid install_id, using fallback" >&2
        INSTALL_ID="invalid"
        ;;
esac
if [ -n "$INSTALL_ID" ] && [ -d "$BACKUP_DIR_BASE/$INSTALL_ID" ]; then
    BACKUP_DIR="$BACKUP_DIR_BASE/$INSTALL_ID"
else
    BACKUP_DIR="$BACKUP_DIR_BASE"
fi

# ---- dry-run mode (default off; enable with AATPE_DRY_RUN=1) ---------------
DRY_RUN=0
[ -n "$AATPE_DRY_RUN" ] && DRY_RUN="$AATPE_DRY_RUN"

# Counters for the restore report.
RESTORE_OK=0
RESTORE_FAIL=0
RESTORE_SKIP=0

log_restore() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$RESTORE_LOG" 2>/dev/null
}

log_restore "=== AATPE uninstall.sh start (DRY_RUN=$DRY_RUN) ==="

# ---- 0) Stop daemons ------------------------------------------------------
# round-3 (P1-21): kill only after verifying the PID actually refers to an
# aatpe binary via /proc/<pid>/exe (same identity check as service.sh and the
# watchdog). A stale/reused PID must never be signaled.
kill_if_aatpe() {
    local pid="$1"
    local expect="$2"   # basename the exe path must END with under /system/bin/
                        # ("aatped" or "aatpe-watchdog"); I-006 exact-path match.
    [ -n "$pid" ] || return
    [ -d "/proc/$pid" ] || return
    local exe
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return
    # I-006 (v4.3.4): exact-path match `*/system/bin/<expect>`, not substring.
    # A process named "my-aatped-backdoor" in /data/local/tmp must NOT be killed
    # by us. $expect is expanded literally inside the case pattern.
    case "$exe" in
        */system/bin/"$expect") kill "$pid" 2>/dev/null ;;
        *) log_restore "SKIP kill pid=$pid (exe=$exe not */system/bin/$expect; stale/reused)" ;;
    esac
}
# C10 (v4.1): SIGTERM the daemon, WAIT for it to exit, SIGKILL if it won't.
# Only AFTER the process is confirmed gone do we proceed to .rec restore, so the
# daemon's own SIGTERM restoreAll (aatped_main.cpp shutdown path) never races our
# .rec restore. If the daemon exits cleanly it already restored every thread, so
# live!=applied and our .rec restore SKIPS (verified: natural no-double-write).
# .rec restore is therefore only a fallback for a daemon that wedged in D-state.
_dpid="$(cat "$RUN_DIR/daemon.pid" 2>/dev/null)"
kill_if_aatpe "$_dpid" "aatped"
# poll /proc up to 5s (0.2s steps)
_i=0; while [ "$_i" -lt 25 ] && [ -n "$_dpid" ] && [ -d "/proc/$_dpid" ]; do
    sleep 0.2; _i=$((_i+1)); done
# still alive -> SIGKILL, wait 2s more
if [ -n "$_dpid" ] && [ -d "/proc/$_dpid" ]; then
    # I-006 (v4.3.4): exact-path match (end of exe path == /system/bin/aatped),
    # not substring "aatped", so a stale PID pointing at an unrelated process is
    # never SIGKILLed by us.
    readlink "/proc/$_dpid/exe" 2>/dev/null | grep -qE '/system/bin/aatped$' && kill -9 "$_dpid" 2>/dev/null
    _i=0; while [ "$_i" -lt 10 ] && [ -d "/proc/$_dpid" ]; do sleep 0.2; _i=$((_i+1)); done
fi
rm -f "$RUN_DIR/daemon.pid" 2>/dev/null

_wpid="$(cat "$RUN_DIR/watchdog.pid" 2>/dev/null)"
kill_if_aatpe "$_wpid" "aatpe-watchdog"
_i=0; while [ "$_i" -lt 15 ] && [ -n "$_wpid" ] && [ -d "/proc/$_wpid" ]; do
    sleep 0.2; _i=$((_i+1)); done
rm -f "$RUN_DIR/watchdog.pid" 2>/dev/null

# C-W7 (v4.1): schedtune.boost marker cleanup. Only if gpu_boost_active exists do
# we write 0 to the recorded boost node path; never sweep all stune nodes.
if [ -f "$RUN_DIR/state/gpu_boost_active" ]; then
    while IFS= read -r _node; do
        case "$_node" in
            /*) [ -e "$_node" ] && echo 0 > "$_node" 2>/dev/null ;;
        esac
    done < "$RUN_DIR/state/gpu_boost_active"
    rm -f "$RUN_DIR/state/gpu_boost_active" 2>/dev/null
fi

# ---- 0.5) Tear down ONLY the debugfs/tracefs mounts AATPE created ---------
# #47: post-fs-data.sh writes a marker (aatpe_mounted_debugfs / aatpe_mounted_tracefs)
# only when IT performed the mount. We umount conditionally on those markers so we
# never unmount a debugfs/tracefs that the platform or another component brought up.
if [ "$(cat "$RUN_DIR/aatpe_mounted_debugfs" 2>/dev/null)" = "1" ]; then
    umount /sys/kernel/debug 2>/dev/null && log_restore "[debugfs] unmounted AATPE-created debugfs"
    rm -f "$RUN_DIR/aatpe_mounted_debugfs"
fi
if [ "$(cat "$RUN_DIR/aatpe_mounted_tracefs" 2>/dev/null)" = "1" ]; then
    umount /sys/kernel/tracing 2>/dev/null && log_restore "[tracefs] unmounted AATPE-created tracefs"
    rm -f "$RUN_DIR/aatpe_mounted_tracefs"
fi

# ---- 2) Restore in dependency order ---------------------------------------
# Restore order (by path pattern priority):
#   P1: kernel params (/proc/sys/)          - restore first (global effects)
#   P2: cpuset groups (/dev/cpuset/, /sys/fs/cgroup/) - thread placement
#   P3: governor (scaling_governor)         - scheduler policy
#   P4: frequency (scaling_min/max/cur)     - clock constraints
#   P5: thermal + GPU                       - last (cooling policy)
# Per-file self-verification: sha256(value_b64) must match the stored sha256=
# field. A tampered / truncated snapshot is SKIPPED (not blindly restored).
restore_one() {
    _kv="$1"
    [ -f "$_kv" ] || return 1

    _path=$(grep '^path=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
    _b64=$(grep '^value_b64=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
    _sha_stored=$(grep '^sha256=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
    # version gate. Legacy snapshots (no snapshot_version field) are
    # treated as v0 and restored with the legacy path. Any unknown future version
    # is SKIPPED rather than blindly written.
    _sv=$(grep '^snapshot_version=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
    [ -n "$_sv" ] && [ "$_sv" != "1" ] && {
        log_restore "SKIP (unknown snapshot_version=$_sv): $_kv"
        RESTORE_SKIP=$((RESTORE_SKIP + 1))
        return 1
    }

    if [ -z "$_path" ]; then
        log_restore "SKIP (malformed): $_kv"
        RESTORE_SKIP=$((RESTORE_SKIP + 1))
        return 1
    fi

    # P2: trust boundary on the .kv `path=` field. A tampered or
    # hand-crafted snapshot must never cause us to write arbitrary content to an
    # arbitrary file. Whitelist the only prefixes AATPE ever snapshots:
    #   /sys/*          (cpufreq / thermal / devfreq / sched)
    #   /proc/*         (sysctl knobs)
    #   /dev/cpuset/*   (cpuset groups)
    # Anything else (e.g. /data/..., /vendor/, an absolute file outside the
    # sysfs/procfs namespace) is rejected before we touch the node.
    case "$_path" in
        /sys/*|/proc/*|/dev/cpuset/*) : ;;  # allowed
        *)
            log_restore "SKIP (path not in whitelist): $_path"
            RESTORE_SKIP=$((RESTORE_SKIP + 1))
            return 1
            ;;
    esac

    # F-A6-07[P3]: the prefix whitelist above matches /sys/../../data/adb/...
    # because the glob only checks the prefix. Reject ANY entry that contains a
    # parent-dot component before we touch the node, even though this code path is
    # currently dead (snapshot writer never emits '..').
    case "$_path" in
        *..*)
            log_restore "SKIP (path contains '..'): $_path"
            RESTORE_SKIP=$((RESTORE_SKIP + 1))
            return 1
            ;;
    esac

    # Integrity self-check on the snapshot itself.
    _sha_actual=$(printf "%s" "$_b64" | sha256sum 2>/dev/null | cut -d' ' -f1)
    if [ "$_sha_actual" != "$_sha_stored" ]; then
        log_restore "SKIP (sha mismatch): $_path stored=$_sha_stored actual=$_sha_actual"
        RESTORE_SKIP=$((RESTORE_SKIP + 1))
        return 1
    fi

    # Decode base64 back to plaintext. Empty b64 decodes to empty (a node that
    # was originally empty/"0" is correctly restored to empty, not skipped).
    _val=$(printf "%s" "$_b64" | base64 -d 2>/dev/null)

    # Target node must still exist.
    if [ ! -e "$_path" ]; then
        log_restore "SKIP (node gone): $_path"
        RESTORE_SKIP=$((RESTORE_SKIP + 1))
        return 1
    fi

    if [ "$DRY_RUN" = "1" ]; then
        log_restore "DRY-RUN would restore: $_path <- '$_val'"
        RESTORE_OK=$((RESTORE_OK + 1))
        return 0
    fi

    # Write original value back. Best-effort: failures logged but non-fatal.
    # printf '%s' (no trailing newline) matches how the value was captured.
    if printf '%s' "$_val" > "$_path" 2>/dev/null; then
        log_restore "OK restore: $_path"
        RESTORE_OK=$((RESTORE_OK + 1))
    else
        log_restore "FAIL restore: $_path (errno=$?)"
        RESTORE_FAIL=$((RESTORE_FAIL + 1))
    fi
    return 0
}

# ROUND-013: NO system-node restore. customize.sh no longer snapshots governor /
# freq / kernel knobs / thermal / cooling / GPU / cpuset-group-cpus nodes (AATPE
# never writes them). The legacy P1-P5 .kv restore loops are removed so a stale
# .kv from an old install version cannot clobber another component's later
# changes. Thread-level restore continues below via recovery/*.rec only.
# (restore_one kept available for any future genuinely-snapshotted node.)

# ---- 3) Legacy cpuset backup ----------------------------------
# ROUND-013: REMOVED. AATPE never rewrites the top-level /dev/cpuset/cpuset.cpus
# partition (only writes tids into group task lists). Restoring a saved
# top-level cpus partition on uninstall would clobber whatever the ROM / another
# component set it to. The legacy block is deleted; leftover
# $RUN_DIR/cpuset.backup is removed by the state-dir cleanup at the end.


# ---- 4) Unified recovery restore (schema v2, .rec files) -------------------
# round-3: the daemon now writes ONE unified record per tid at
#   $RUN_DIR/recovery/<tid>.rec  (schema_version=2) containing every dimension
#   AATPE may touch: affinity / sched policy / sched priority / nice /
#   uclamp min / uclamp max. We restore the original ONLY when the live thread
#   still matches the applied values on EVERY dimension AATPE recorded; any
#   mismatch means another component has taken over the thread and we leave it.
#
# P0-5: before restoring we compare current_policy==applied_policy AND
#   current_priority==applied_priority AND current_nice==applied_nice AND
#   current_uclamp_min==applied_min AND current_uclamp_max==applied_max
#   (AND current affinity == applied affinity). All must match to restore.
# P0-6: orig_sched_priority/applied_sched_priority are restored exactly (the old
#   chrt restore hardcoded priority=1, losing a thread's original RR prio=10).

# Map a chrt policy name to its numeric constant.
policy_name_to_num() {
    case "$1" in
        SCHED_OTHER)   echo 0 ;;
        SCHED_FIFO)    echo 1 ;;
        SCHED_RR)      echo 2 ;;
        SCHED_BATCH)   echo 3 ;;
        SCHED_IDLE)    echo 5 ;;
        SCHED_DEADLINE) echo 6 ;;
        *)             echo -1 ;;
    esac
}
policy_num_to_name() {
    case "$1" in
        0) echo "SCHED_OTHER" ;;
        1) echo "SCHED_FIFO" ;;
        2) echo "SCHED_RR" ;;
        3) echo "SCHED_BATCH" ;;
        5) echo "SCHED_IDLE" ;;
        6) echo "SCHED_DEADLINE" ;;
        *) echo "SCHED_OTHER" ;;
    esac
}

restore_unified_records() {
    [ -d "$RUN_DIR/recovery" ] || return 0
    for rf in "$RUN_DIR/recovery"/*.rec; do
        [ -f "$rf" ] || continue
        r_tid=$(grep "^tid=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_start=$(grep "^starttime=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_opolicy=$(grep "^orig_policy=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_oprio=$(grep "^orig_priority=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_onice=$(grep "^orig_nice=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_omin=$(grep "^orig_min=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_omax=$(grep "^orig_max=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_oolist=$(grep "^orig_affinity=" "$rf" 2>/dev/null | cut -d= -f2)
        r_apolicy=$(grep "^applied_policy=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_aprio=$(grep "^applied_priority=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_anice=$(grep "^applied_nice=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_amin=$(grep "^applied_min=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_amax=$(grep "^applied_max=" "$rf" 2>/dev/null | cut -d'=' -f2)
        r_alist=$(grep "^applied_affinity=" "$rf" 2>/dev/null | cut -d= -f2)
        [ -n "$r_tid" ] || continue
        taskpath=$(ls -d /proc/[0-9]*/task/"$r_tid" 2>/dev/null | head -1)
        [ -n "$taskpath" ] || continue
        r_pid=$(echo "$taskpath" | cut -d/ -f3)

        # TID-reuse identity check.
        if [ -n "$r_start" ]; then
            # I-002 (v4.3.4): sed+cut instead of awk for toybox portability
            # (matches service.sh promoted_tids sweep). Strip everything up to
            # the last ')' (comm may contain spaces/parens), then field 20 ==
            # starttime. On a device without awk, the old awk invocation returned
            # empty and the TID-reuse guard silently skipped -- now it always runs.
            live_start=$(sed 's/^.*) //' "$taskpath/stat" 2>/dev/null | cut -d' ' -f20)
            if [ -n "$live_start" ] && [ "$live_start" != "$r_start" ]; then
                log_restore "SKIP tid=$r_tid (starttime $live_start != recorded $r_start, TID reused)"
                continue
            fi
        fi

        skip=0

        # --- uclamp (only if the uclamp nodes exist and applied uclamp recorded) ---
        if [ -n "$r_amin" ] && [ -n "$r_amax" ]; then
            u_min_node="/proc/$r_pid/task/$r_tid/sched.uclamp.min"
            u_max_node="/proc/$r_pid/task/$r_tid/sched.uclamp.max"
            if [ -e "$u_min_node" ]; then
                live_min=$(cat "$u_min_node" 2>/dev/null | tr -d ' \n')
                live_max=$(cat "$u_max_node" 2>/dev/null | tr -d ' \n')
                if [ "$live_min" != "$r_amin" ] || [ "$live_max" != "$r_amax" ]; then
                    log_restore "SKIP tid=$r_tid uclamp (live min=$live_min!=$r_amin max=$live_max!=$r_amax, taken over)"
                    skip=1
                fi
            fi
        fi

        # --- nice ---
        if [ -n "$r_anice" ]; then
            # I-002 (v4.3.4): sed+cut for toybox portability (field 17 == nice).
            live_nice=$(sed 's/^.*) //' "$taskpath/stat" 2>/dev/null | cut -d' ' -f17)
            if [ -n "$live_nice" ] && [ "$live_nice" != "$r_anice" ]; then
                log_restore "SKIP tid=$r_tid nice (live=$live_nice != applied=$r_anice, taken over)"
                skip=1
            fi
        fi

        # --- sched policy + priority via chrt -p ---
        # R9-017 / LED-148: parse `chrt -p` output structurally instead of relying on
        # toybox's exact "scheduling policy:" / "scheduling priority:" labels, which
        # differ across ROMs. Extract the SCHED_* token and the trailing integer
        # directly. If the extraction yields nothing we simply skip the comparison
        # (conservative: never wrongly write the wrong value), matching the safe
        # direction already used elsewhere.
        if [ -n "$r_apolicy" ] && command -v chrt >/dev/null 2>&1; then
            chrt_out=$(chrt -p "$r_tid" 2>/dev/null)
            live_policy_name=$(printf '%s\n' "$chrt_out" | grep -oE 'SCHED_[A-Z]+' | head -1)
            live_prio=$(printf '%s\n' "$chrt_out" | grep -oE 'current scheduling priority: [0-9]+' | grep -oE '[0-9]+$' | head -1)
            live_policy=$(policy_name_to_num "$live_policy_name")
            if [ -n "$r_apolicy" ] && [ -n "$live_policy" ] && [ "$live_policy" != "$r_apolicy" ]; then
                log_restore "SKIP tid=$r_tid policy (live=$live_policy != applied=$r_apolicy, taken over)"
                skip=1
            fi
            if [ -n "$r_aprio" ] && [ -n "$live_prio" ] && [ "$live_prio" != "$r_aprio" ]; then
                log_restore "SKIP tid=$r_tid priority (live=$live_prio != applied=$r_aprio, taken over)"
                skip=1
            fi
        fi

        # --- affinity ---
        if [ -n "$r_alist" ]; then
            live_aff=$(grep "^Cpus_allowed_list:" "$taskpath/status" 2>/dev/null | cut -d: -f2 | tr -d ' \n')
            if [ "$live_aff" != "$r_alist" ]; then
                log_restore "SKIP tid=$r_tid affinity (live=$live_aff != applied=$r_alist, taken over)"
                skip=1
            fi
        fi

        if [ "$skip" = "1" ]; then
            RESTORE_SKIP=$((RESTORE_SKIP+1))
            continue
        fi

        # All dimensions match: restore original.
        if [ "$DRY_RUN" = "1" ]; then
            log_restore "DRY-RUN would restore tid=$r_tid -> policy=$r_opolicy prio=$r_oprio nice=$r_onice min=$r_omin max=$r_omax affinity=$r_oolist"
            RESTORE_OK=$((RESTORE_OK+1))
            continue
        fi
        # uclamp
        if [ -n "$r_omin" ] && [ -e "/proc/$r_pid/task/$r_tid/sched.uclamp.min" ]; then
            echo "$r_omin" > "/proc/$r_pid/task/$r_tid/sched.uclamp.min" 2>/dev/null
            echo "$r_omax" > "/proc/$r_pid/task/$r_tid/sched.uclamp.max" 2>/dev/null
        fi
        # sched policy + priority exactly (P0-6).
        # R9-003 / LED-134: the restore case previously only covered
        # SCHED_OTHER(0)/FIFO(1)/RR(2); BATCH(3)/IDLE(5)/DEADLINE(6) fell into the
        # `*) :` no-op and the thread kept AATPE's changed policy after uninstall.
        # Restore BATCH/IDLE to their own classes (priority is ignored by both);
        # DEADLINE cannot be reconstructed with chrt (needs runtime/deadline params),
        # so log an explicit SKIP rather than silently leaving it.
        if [ -n "$r_opolicy" ] && command -v chrt >/dev/null 2>&1; then
            case "$r_opolicy" in
                0) chrt -o -p 0 "$r_tid" 2>/dev/null ;;
                1) chrt -f -p "${r_oprio:-1}" "$r_tid" 2>/dev/null ;;
                2) chrt -r -p "${r_oprio:-1}" "$r_tid" 2>/dev/null ;;
                3) chrt -b -p 0 "$r_tid" 2>/dev/null ;;
                5) chrt -i -p 0 "$r_tid" 2>/dev/null ;;
                6) log_restore "SKIP tid=$r_tid policy DEADLINE(6) not restorable via chrt" ;;
                *) log_restore "SKIP tid=$r_tid unknown policy=$r_opolicy" ;;
            esac
        fi
        # nice
        if [ -n "$r_onice" ] && command -v renice >/dev/null 2>&1; then
            renice -n "$r_onice" -p "$r_tid" 2>/dev/null
        fi
        # affinity
        if [ -n "$r_oolist" ] && command -v taskset >/dev/null 2>&1; then
            taskset -cp "$r_oolist" "$r_tid" 2>/dev/null
        fi
        log_restore "OK restore tid=$r_tid -> policy=$r_opolicy prio=$r_oprio nice=$r_onice min=$r_omin max=$r_omax affinity=$r_oolist"
        RESTORE_OK=$((RESTORE_OK+1))
    done 2>/dev/null
}
restore_unified_records

# ---- 5) Legacy .uclamp / .affinity restore (backward compat) ----------------
# Older daemons wrote split <tid>.uclamp and <tid>.affinity files. If the unified
# .rec migration has not happened (no .rec for a tid), restore those best-effort.
restore_legacy_records() {
    [ -d "$RUN_DIR/recovery" ] || return 0
    # R9-004 / LED-135: TID-reuse identity check shared by both legacy loops. The
    # unified .rec path already compares the recorded starttime to the live thread's
    # starttime before restoring; the legacy .uclamp/.affinity path only checked
    # that /proc/<pid>/task/<tid> exists, so a recycled TID would get the recorded
    # old uclamp/affinity written onto an unrelated new thread. If the legacy record
    # carries a starttime= field, verify it against the live stat before writing.
    legacy_tid_ok() {
        local taskpath="$1" rec_start="$2"
        [ -n "$rec_start" ] || return 0   # very old record without baseline: skip check
        local live_start
        # I-002 (v4.3.4): sed+cut for toybox portability (field 20 == starttime).
        live_start=$(sed 's/^.*) //' "$taskpath/stat" 2>/dev/null | cut -d' ' -f20)
        if [ -n "$live_start" ] && [ "$live_start" != "$rec_start" ]; then
            log_restore "SKIP legacy tid (starttime $live_start != recorded $rec_start, TID reused)"
            return 1
        fi
        return 0
    }
    for uf in "$RUN_DIR/recovery"/*.uclamp; do
        [ -f "$uf" ] || continue
        lt=$(grep "^tid=" "$uf" 2>/dev/null | cut -d'=' -f2)
        # Skip if a unified .rec already covered this tid.
        [ -f "$RUN_DIR/recovery/$lt.rec" ] && continue
        l_omin=$(grep "^orig_min=" "$uf" 2>/dev/null | cut -d'=' -f2)
        l_omax=$(grep "^orig_max=" "$uf" 2>/dev/null | cut -d'=' -f2)
        l_onice=$(grep "^orig_nice=" "$uf" 2>/dev/null | cut -d'=' -f2)
        l_opolicy=$(grep "^orig_policy=" "$uf" 2>/dev/null | cut -d'=' -f2)
        l_start=$(grep "^starttime=" "$uf" 2>/dev/null | cut -d'=' -f2)
        taskpath=$(ls -d /proc/[0-9]*/task/"$lt" 2>/dev/null | head -1)
        [ -n "$taskpath" ] || continue
        l_pid=$(echo "$taskpath" | cut -d/ -f3)
        legacy_tid_ok "$taskpath" "$l_start" || continue
        [ "$DRY_RUN" = "1" ] && { log_restore "DRY-RUN legacy restore tid=$lt"; continue; }
        [ -n "$l_omin" ] && echo "$l_omin" > "/proc/$l_pid/task/$lt/sched.uclamp.min" 2>/dev/null
        [ -n "$l_omax" ] && echo "$l_omax" > "/proc/$l_pid/task/$lt/sched.uclamp.max" 2>/dev/null
        [ -n "$l_onice" ] && command -v renice >/dev/null 2>&1 && renice -n "$l_onice" -p "$lt" 2>/dev/null
        [ -n "$l_opolicy" ] && command -v chrt >/dev/null 2>&1 && [ "$l_opolicy" = "0" ] && chrt -o -p 0 "$lt" 2>/dev/null
        log_restore "OK legacy restore tid=$lt"
    done 2>/dev/null
    for af in "$RUN_DIR/recovery"/*.affinity; do
        [ -f "$af" ] || continue
        at=$(grep "^tid=" "$af" 2>/dev/null | cut -d'=' -f2)
        [ -f "$RUN_DIR/recovery/$at.rec" ] && continue
        a_olist=$(grep "^orig_list=" "$af" 2>/dev/null | cut -d= -f2)
        a_start=$(grep "^starttime=" "$af" 2>/dev/null | cut -d'=' -f2)
        taskpath=$(ls -d /proc/[0-9]*/task/"$at" 2>/dev/null | head -1)
        [ -n "$taskpath" ] || continue
        legacy_tid_ok "$taskpath" "$a_start" || continue
        [ "$DRY_RUN" = "1" ] && { log_restore "DRY-RUN legacy affinity restore tid=$at"; continue; }
        [ -n "$a_olist" ] && command -v taskset >/dev/null 2>&1 && taskset -cp "$a_olist" "$at" 2>/dev/null
        log_restore "OK legacy affinity restore tid=$at"
    done 2>/dev/null
}
restore_legacy_records

# ---- 6) SELinux context cleanup -------------------------------------------
if command -v restorecon >/dev/null 2>&1; then
    restorecon -R "$RUN_DIR" 2>/dev/null
fi

# ---- 7) Emit restore report -----------------------------------------------
log_restore "--- restore report ---"
log_restore "  OK=$RESTORE_OK  FAIL=$RESTORE_FAIL  SKIP=$RESTORE_SKIP"
log_restore "=== AATPE uninstall.sh done ==="

# Also print to stdout (Magisk/KSU capture this in the install log).
echo "AATPE restore: OK=$RESTORE_OK FAIL=$RESTORE_FAIL SKIP=$RESTORE_SKIP"

# ---- 8) Final cleanup (skip when dry-run) -----------------------------------
if [ "$DRY_RUN" != "1" ]; then
    rm -rf "$RUN_DIR/state" "$RUN_DIR/metrics" "$RUN_DIR/decisions" \
           "$RUN_DIR/thread_db" "$RUN_DIR/recovery" "$RUN_DIR/history" "$RUN_DIR/rules" \
           "$RUN_DIR/cpuset.backup" "$RUN_DIR/daemon.lock" 2>/dev/null
    # R9-010 / LED-141: also remove the runtime artifacts the previous list missed:
    #   - helper/          (world-readable export channel; was left behind)
    #   - moddir           (leaked the module absolute path)
    #   - aatpe.log        (system-internal log)
    #   - aatpe_mounted_*  (leftover mount markers if the umount branch above missed them)
    #   - hmac_key / watchdog state files left directly under $RUN_DIR
    rm -rf "$RUN_DIR/helper" "$RUN_DIR/moddir" "$RUN_DIR/aatpe.log" \
           "$RUN_DIR/aatpe_mounted_debugfs" "$RUN_DIR/aatpe_mounted_tracefs" \
           "$RUN_DIR/hmac_key" "$RUN_DIR/watchdog.pid" "$RUN_DIR/daemon.pid" 2>/dev/null
    # Remove persistent backup + manifest so a fresh install starts clean.
    rm -rf "$BACKUP_DIR" "$STATE_DIR/manifest.sha256" \
           "$STATE_DIR/manifest_reanchor.pending" \
           "$STATE_DIR/boot_id" "$STATE_DIR/snapshot.log" 2>/dev/null
    # G10-003 / ROUND-003: also drop the bootloop-watchdog state so a reinstall starts fresh.
    rm -f "$STATE_DIR/boot_in_progress" "$STATE_DIR/boot_fail_count" "$STATE_DIR/self_disabled_latch" "$STATE_DIR/boot.log" 2>/dev/null
    # UNINST: sweep the runtime tmpfs artifacts the earlier list missed. All of
    # these are written by the RUNNING daemon/watchdog under $RUN_DIR=/dev/aatpe
    # (tmpfs), never module-bundled, so they are safe to delete on uninstall.
    #   control / control_cmd / *.processing : command mailboxes
    #   commands/                            : on-disk command queue dir
    #   watchdog.lock / watchdog.pid         : single-instance lock + pid
    #   version                              : runtime-written version stamp
    #   federated/                           : federated learner export dir
    #   aatpe-watchdog.log                   : watchdog runtime log
    #   foreground_app                       : runtime foreground-app state
    # webui/ is NOT here: it ships inside the module image (system/etc/aatpe/webui)
    # and is removed automatically with the module dir by Magisk/KSU.
    rm -f "$RUN_DIR/control" "$RUN_DIR/control_cmd" "$RUN_DIR/control.processing" \
           "$RUN_DIR/watchdog.lock" "$RUN_DIR/version" "$RUN_DIR/aatpe-watchdog.log" \
           "$RUN_DIR/foreground_app" 2>/dev/null
    rm -rf "$RUN_DIR/commands" "$RUN_DIR/federated" 2>/dev/null
fi

echo "[*] AATPE uninstalled, system defaults restored" >> "$RESTORE_LOG" 2>/dev/null

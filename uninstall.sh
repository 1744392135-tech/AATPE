#!/system/bin/sh
RUN_DIR="/dev/aatpe"
BACKUP_DIR_BASE="/data/adb/aatpe/backup"
STATE_DIR="/data/adb/aatpe"
RESTORE_LOG="$STATE_DIR/restore.log"
echo "[*] AATPE v6.0.0 uninstall started" >> "$RESTORE_LOG" 2>/dev/null

INSTALL_ID=""
[ -r "$STATE_DIR/install_id" ] && INSTALL_ID=$(cat "$STATE_DIR/install_id" 2>/dev/null)
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

DRY_RUN=0
[ -n "$AATPE_DRY_RUN" ] && DRY_RUN="$AATPE_DRY_RUN"

RESTORE_OK=0
RESTORE_FAIL=0
RESTORE_SKIP=0

log_restore() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$RESTORE_LOG" 2>/dev/null
}

log_restore "=== AATPE uninstall.sh start (DRY_RUN=$DRY_RUN) ==="

kill_if_aatpe() {
    local pid="$1"
    local expect="$2"
    [ -n "$pid" ] || return
    [ -d "/proc/$pid" ] || return
    local exe
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return
    case "$exe" in
        */system/bin/"$expect") kill "$pid" 2>/dev/null ;;
        *) log_restore "SKIP kill pid=$pid (exe=$exe not */system/bin/$expect; stale/reused)" ;;
    esac
}
_dpid="$(cat "$RUN_DIR/daemon.pid" 2>/dev/null)"
kill_if_aatpe "$_dpid" "aatped"
_i=0; while [ "$_i" -lt 25 ] && [ -n "$_dpid" ] && [ -d "/proc/$_dpid" ]; do
    sleep 0.2; _i=$((_i+1)); done
if [ -n "$_dpid" ] && [ -d "/proc/$_dpid" ]; then
    readlink "/proc/$_dpid/exe" 2>/dev/null | grep -qE '/system/bin/aatped$' && kill -9 "$_dpid" 2>/dev/null
    _i=0; while [ "$_i" -lt 10 ] && [ -d "/proc/$_dpid" ]; do sleep 0.2; _i=$((_i+1)); done
fi
rm -f "$RUN_DIR/daemon.pid" 2>/dev/null

_wpid="$(cat "$RUN_DIR/watchdog.pid" 2>/dev/null)"
kill_if_aatpe "$_wpid" "aatpe-watchdog"
_i=0; while [ "$_i" -lt 15 ] && [ -n "$_wpid" ] && [ -d "/proc/$_wpid" ]; do
    sleep 0.2; _i=$((_i+1)); done
rm -f "$RUN_DIR/watchdog.pid" 2>/dev/null

if [ -f "$RUN_DIR/state/gpu_boost_active" ]; then
    while IFS= read -r _node; do
        case "$_node" in
            /*) [ -e "$_node" ] && echo 0 > "$_node" 2>/dev/null ;;
        esac
    done < "$RUN_DIR/state/gpu_boost_active"
    rm -f "$RUN_DIR/state/gpu_boost_active" 2>/dev/null
fi

if [ "$(cat "$RUN_DIR/aatpe_mounted_debugfs" 2>/dev/null)" = "1" ]; then
    umount /sys/kernel/debug 2>/dev/null && log_restore "[debugfs] unmounted AATPE-created debugfs"
    rm -f "$RUN_DIR/aatpe_mounted_debugfs"
fi
if [ "$(cat "$RUN_DIR/aatpe_mounted_tracefs" 2>/dev/null)" = "1" ]; then
    umount /sys/kernel/tracing 2>/dev/null && log_restore "[tracefs] unmounted AATPE-created tracefs"
    rm -f "$RUN_DIR/aatpe_mounted_tracefs"
fi

restore_one() {
    _kv="$1"
    [ -f "$_kv" ] || return 1

    _path=$(grep '^path=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
    _b64=$(grep '^value_b64=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
    _sha_stored=$(grep '^sha256=' "$_kv" 2>/dev/null | head -1 | cut -d= -f2-)
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

    case "$_path" in
        /sys/*|/proc/*|/dev/cpuset/*) : ;;
        *)
            log_restore "SKIP (path not in whitelist): $_path"
            RESTORE_SKIP=$((RESTORE_SKIP + 1))
            return 1
            ;;
    esac

    case "$_path" in
        *..*)
            log_restore "SKIP (path contains '..'): $_path"
            RESTORE_SKIP=$((RESTORE_SKIP + 1))
            return 1
            ;;
    esac

    _sha_actual=$(printf "%s" "$_b64" | sha256sum 2>/dev/null | cut -d' ' -f1)
    if [ "$_sha_actual" != "$_sha_stored" ]; then
        log_restore "SKIP (sha mismatch): $_path stored=$_sha_stored actual=$_sha_actual"
        RESTORE_SKIP=$((RESTORE_SKIP + 1))
        return 1
    fi

    _val=$(printf "%s" "$_b64" | base64 -d 2>/dev/null)

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

    if printf '%s' "$_val" > "$_path" 2>/dev/null; then
        log_restore "OK restore: $_path"
        RESTORE_OK=$((RESTORE_OK + 1))
    else
        log_restore "FAIL restore: $_path (errno=$?)"
        RESTORE_FAIL=$((RESTORE_FAIL + 1))
    fi
    return 0
}

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

        if [ -n "$r_start" ]; then
            live_start=$(sed 's/^.*) //' "$taskpath/stat" 2>/dev/null | cut -d' ' -f20)
            if [ -n "$live_start" ] && [ "$live_start" != "$r_start" ]; then
                log_restore "SKIP tid=$r_tid (starttime $live_start != recorded $r_start, TID reused)"
                continue
            fi
        fi

        skip=0

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

        if [ -n "$r_anice" ]; then
            live_nice=$(sed 's/^.*) //' "$taskpath/stat" 2>/dev/null | cut -d' ' -f17)
            if [ -n "$live_nice" ] && [ "$live_nice" != "$r_anice" ]; then
                log_restore "SKIP tid=$r_tid nice (live=$live_nice != applied=$r_anice, taken over)"
                skip=1
            fi
        fi

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

        if [ "$DRY_RUN" = "1" ]; then
            log_restore "DRY-RUN would restore tid=$r_tid -> policy=$r_opolicy prio=$r_oprio nice=$r_onice min=$r_omin max=$r_omax affinity=$r_oolist"
            RESTORE_OK=$((RESTORE_OK+1))
            continue
        fi
        if [ -n "$r_omin" ] && [ -e "/proc/$r_pid/task/$r_tid/sched.uclamp.min" ]; then
            echo "$r_omin" > "/proc/$r_pid/task/$r_tid/sched.uclamp.min" 2>/dev/null
            echo "$r_omax" > "/proc/$r_pid/task/$r_tid/sched.uclamp.max" 2>/dev/null
        fi
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
        if [ -n "$r_onice" ] && command -v renice >/dev/null 2>&1; then
            renice -n "$r_onice" -p "$r_tid" 2>/dev/null
        fi
        if [ -n "$r_oolist" ] && command -v taskset >/dev/null 2>&1; then
            taskset -cp "$r_oolist" "$r_tid" 2>/dev/null
        fi
        log_restore "OK restore tid=$r_tid -> policy=$r_opolicy prio=$r_oprio nice=$r_onice min=$r_omin max=$r_omax affinity=$r_oolist"
        RESTORE_OK=$((RESTORE_OK+1))
    done 2>/dev/null
}
restore_unified_records

restore_legacy_records() {
    [ -d "$RUN_DIR/recovery" ] || return 0
    legacy_tid_ok() {
        local taskpath="$1" rec_start="$2"
        [ -n "$rec_start" ] || return 0
        local live_start
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

if command -v restorecon >/dev/null 2>&1; then
    restorecon -R "$RUN_DIR" 2>/dev/null
fi

log_restore "--- restore report ---"
log_restore "  OK=$RESTORE_OK  FAIL=$RESTORE_FAIL  SKIP=$RESTORE_SKIP"
log_restore "=== AATPE uninstall.sh done ==="

echo "AATPE restore: OK=$RESTORE_OK FAIL=$RESTORE_FAIL SKIP=$RESTORE_SKIP"

if [ "$DRY_RUN" != "1" ]; then
    rm -rf "$RUN_DIR/state" "$RUN_DIR/metrics" "$RUN_DIR/decisions" \
           "$RUN_DIR/thread_db" "$RUN_DIR/recovery" "$RUN_DIR/history" "$RUN_DIR/rules" \
           "$RUN_DIR/cpuset.backup" "$RUN_DIR/daemon.lock" 2>/dev/null
    rm -rf "$RUN_DIR/helper" "$RUN_DIR/moddir" "$RUN_DIR/aatpe.log" \
           "$RUN_DIR/aatpe_mounted_debugfs" "$RUN_DIR/aatpe_mounted_tracefs" \
           "$RUN_DIR/hmac_key" "$RUN_DIR/watchdog.pid" "$RUN_DIR/daemon.pid" 2>/dev/null
    rm -rf "$BACKUP_DIR" "$STATE_DIR/manifest.sha256" \
           "$STATE_DIR/manifest_reanchor.pending" \
           "$STATE_DIR/boot_id" "$STATE_DIR/snapshot.log" 2>/dev/null
    rm -f "$STATE_DIR/boot_in_progress" "$STATE_DIR/boot_fail_count" "$STATE_DIR/self_disabled_latch" "$STATE_DIR/boot.log" 2>/dev/null
    rm -f "$RUN_DIR/control" "$RUN_DIR/control_cmd" "$RUN_DIR/control.processing" \
           "$RUN_DIR/watchdog.lock" "$RUN_DIR/version" "$RUN_DIR/aatpe-watchdog.log" \
           "$RUN_DIR/foreground_app" 2>/dev/null
    rm -rf "$RUN_DIR/commands" "$RUN_DIR/federated" 2>/dev/null
fi

echo "[*] AATPE uninstalled, system defaults restored" >> "$RESTORE_LOG" 2>/dev/null

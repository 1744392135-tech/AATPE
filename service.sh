#!/system/bin/sh
MODDIR="${0%/*}"
if [ ! -d "$MODDIR/system/bin" ]; then
    for p in /data/adb/modules/aatpe /data/adb/ksu/modules/aatpe /data/adb/ap/modules/aatpe /data/adb/modules_update/aatpe; do
        if [ -d "$p/system/bin" ]; then MODDIR="$p"; break; fi
    done
fi
RUN_DIR="/dev/aatpe"
LOG_FILE="$RUN_DIR/aatpe.log"
_boot_wait=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$_boot_wait" -lt 120 ]; do
    sleep 1
    _boot_wait=$((_boot_wait + 1))
done
if [ "$(getprop sys.boot_completed)" != "1" ]; then
    echo "[WARN] sys.boot_completed still !=1 after ${_boot_wait}s; continuing anyway" >> "$LOG_FILE"
fi
sleep 5
mkdir -p "$RUN_DIR" "$RUN_DIR/state" "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/helper" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" 2>/dev/null
chmod 0711 "$RUN_DIR" 2>/dev/null
chmod 0700 "$RUN_DIR/state" 2>/dev/null
chmod 0755 "$RUN_DIR/helper" 2>/dev/null
chmod 0700 "$RUN_DIR/metrics" "$RUN_DIR/decisions" "$RUN_DIR/thread_db" "$RUN_DIR/history" "$RUN_DIR/rules" 2>/dev/null
chmod 0600 "$LOG_FILE" 2>/dev/null

SELINUX_CTX="u:object_r:system_data_file:s0"
[ -d "$RUN_DIR/helper" ] && chcon -R "$SELINUX_CTX" "$RUN_DIR/helper" 2>/dev/null
[ -d "$MODDIR/system/bin" ] && chcon -R "u:object_r:system_file:s0" "$MODDIR/system/bin" 2>/dev/null
[ -f "$MODDIR/system/etc/aatpe/aatpe.conf" ] && chcon "$SELINUX_CTX" "$MODDIR/system/etc/aatpe/aatpe.conf" 2>/dev/null
echo "[$(date '+%Y-%m-%d %H:%M:%S')] AATPE service starting (MODDIR=$MODDIR)" >> "$LOG_FILE"
if [ -x "$MODDIR/system/bin/aatped" ]; then
    "$MODDIR/system/bin/aatped" --init-topology >> "$LOG_FILE" 2>&1
fi
if [ -x "$MODDIR/system/bin/aatpe-probe" ]; then
    "$MODDIR/system/bin/aatpe-probe" >> "$LOG_FILE" 2>&1
else
    echo "[WARN] aatpe-probe not found/executable at $MODDIR/system/bin/aatpe-probe; skipping" >> "$LOG_FILE"
fi
if [ ! -x "$MODDIR/system/bin/aatped" ]; then
    echo "[ERROR] aatped binary not found or not executable at $MODDIR/system/bin/aatped" >> "$LOG_FILE"
    exit 1
fi
if [ ! -x "$MODDIR/system/bin/aatpe-watchdog" ]; then
    echo "[ERROR] aatpe-watchdog binary not found at $MODDIR/system/bin/aatpe-watchdog" >> "$LOG_FILE"
fi

REANCHOR_MARKER="/data/adb/aatpe/manifest_reanchor.pending"
chmod 0700 /data/adb/aatpe 2>/dev/null
chown 0:0 /data/adb/aatpe 2>/dev/null
[ -f "$REANCHOR_MARKER" ] && chmod 0600 "$REANCHOR_MARKER" 2>/dev/null
if [ -f "$REANCHOR_MARKER" ]; then
    echo "[*] Consuming trusted-install re-anchor marker; re-baselining integrity manifest against $MODDIR" >> "$LOG_FILE"
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

if [ -x "$MODDIR/system/bin/aatped" ]; then
    echo "[*] Running module integrity verification (SHA-256 hash chain)..." >> "$LOG_FILE"

    if [ ! -f /data/adb/aatpe/manifest.sha256 ]; then
        echo "[*] No integrity manifest found, generating bootstrap (first-install fallback)..." >> "$LOG_FILE"
        "$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR" >> "$LOG_FILE" 2>&1
        if [ ! -f /data/adb/aatpe/manifest.sha256 ]; then
            echo "[FATAL] Bootstrap integrity manifest generation failed; /data/adb/aatpe/manifest.sha256 absent." >> "$LOG_FILE"
            echo "[FATAL] fail-closed: NOT starting daemon and NOT applying any scheduling action. Reinstall module." >> "$LOG_FILE"
            echo "DEGRADED:integrity=bootstrap_manifest_missing:reinstall_required" > "$RUN_DIR/state/integrity_degraded" 2>/dev/null
            chmod 0600 "$RUN_DIR/state/integrity_degraded" 2>/dev/null
            exit 1
        fi
    fi

    "$MODDIR/system/bin/aatped" --verify-integrity "$MODDIR" >> "$LOG_FILE" 2>&1
    _int_status=$(cat "$RUN_DIR/state/integrity_status" 2>/dev/null)
    if [ "$_int_status" = "OK" ]; then
        echo "[OK] Module integrity verified (SHA-256 hash chain OK)" >> "$LOG_FILE"
    else
        echo "[FATAL] Integrity FAILED - module files may have been tampered." >> "$LOG_FILE"
        echo "[FATAL] Manifest will NOT be auto-regenerated. Reinstall module to reset." >> "$LOG_FILE"
        echo "[FATAL] Module integrity status: ${_int_status:-UNKNOWN}" >> "$LOG_FILE"
        echo "[FATAL] fail-closed: NOT starting daemon and NOT applying any scheduling action." >> "$LOG_FILE"
        echo "DEGRADED:integrity=${_int_status:-FAILED}:reinstall_required" > "$RUN_DIR/state/integrity_degraded" 2>/dev/null
        chmod 0600 "$RUN_DIR/state/integrity_degraded" 2>/dev/null
        exit 1
    fi
fi

PROM="$RUN_DIR/state/promoted_tids"
if [ -f "$PROM" ]; then
    echo "[*] Sweeping residual promoted-thread state from previous session..." >> "$LOG_FILE"
    while IFS=: read -r ptid pstart pfield pmech; do
        [ -n "$ptid" ] || continue
        case "$ptid" in
            ''|*[!0-9]*) continue ;;
        esac
        [ "$ptid" -le 4194304 ] 2>/dev/null || continue
        [ "$pfield" = "-" ] && continue
        tp="$(ls -d /proc/[0-9]*/task/$ptid 2>/dev/null | head -1)"
        [ -n "$tp" ] || continue
        live_start="$(sed 's/^.*) //' "$tp/stat" 2>/dev/null | cut -d' ' -f20)"
        [ -z "$live_start" ] && continue
        [ "$live_start" != "$pstart" ] && continue
        case "$pfield" in
            grp=*)
                grp="${pfield#grp=}"
                for tgt in "/dev/cpuset/$grp/tasks" "/dev/cpuctl/$grp/tasks"; do
                    if [ -e "$tgt" ]; then
                        echo "$ptid" > "$tgt" 2>>"$LOG_FILE" \
                            || echo "PROM v1 tasks write fail ptid=$ptid -> $tgt" >> "$LOG_FILE"
                    fi
                done
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
                command -v taskset >/dev/null 2>&1 && taskset -cp "$pfield" "$ptid" >> "$LOG_FILE" 2>&1
                ;;
        esac
    done < "$PROM"
    rm -f "$PROM" 2>/dev/null
fi

CGA="$RUN_DIR/state/cg_uclamp_active"
if [ -f "$CGA" ]; then
    echo "[*] cg_uclamp_active residue: resetting top-app cpu.uclamp.min floors..." >> "$LOG_FILE"
    for u in /dev/cpuctl/top-app/cpu.uclamp.min \
             /sys/fs/cgroup/top-app/cpu.uclamp.min \
             /sys/fs/cgroup/cpu.uclamp.min; do
        [ -e "$u" ] && echo 0 > "$u" 2>/dev/null
    done
    if [ -s "$CGA" ]; then
        tr ',' '\n' < "$CGA" | while IFS= read -r g; do
            g="${g// /}"
            [ -n "$g" ] || continue
            [ "$g" = "top-app" ] && continue
            for u in "/dev/cpuctl/$g/cpu.uclamp.min" "/sys/fs/cgroup/$g/cpu.uclamp.min"; do
                [ -e "$u" ] && echo 0 > "$u" 2>/dev/null
            done
        done
    fi
    rm -f "$CGA" 2>/dev/null
fi

GB="$RUN_DIR/state/gpu_boost_active"
if [ -f "$GB" ]; then
    echo "[*] gpu_boost_active residue: resetting recorded schedtune.boost nodes..." >> "$LOG_FILE"
    if [ -s "$GB" ]; then
        while IFS= read -r node; do
            case "$node" in
                /*) [ -e "$node" ] && echo 0 > "$node" 2>/dev/null ;;
            esac
        done < "$GB"
    fi
    rm -f "$GB" 2>/dev/null
fi

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

echo "[*] Starting aatped daemon..." >> "$LOG_FILE"
is_aatped_pid() {
    local pid="$1"
    [ -n "$pid" ] || return 1
    [ -d "/proc/$pid" ] || return 1
    local exe
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 1
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
    rm -f "$RUN_DIR/daemon.pid" 2>/dev/null
    rm -f "$RUN_DIR/state/stop_request" 2>/dev/null
    env -i \
        PATH=/system/bin:/system/xbin:/vendor/bin \
        HOME=/root \
        TZ="$TZ" \
        ${INIT_RC:+INIT_RC="$INIT_RC"} \
        ${KSU:+KSU="$KSU"} \
        ${APATCH:+APATCH="$APATCH"} \
        AATPE_MODULE_ROOT="$MODDIR" \
        "$MODDIR/system/bin/aatped" --daemon >> "$LOG_FILE" 2>&1 &
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
[ -f "$RUN_DIR/daemon.pid" ] && chmod 0600 "$RUN_DIR/daemon.pid" 2>/dev/null
if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "[OK] Daemon started successfully (PID=$DAEMON_PID)" >> "$LOG_FILE"
else
    echo "[ERROR] Daemon failed to start (daemon.pid not found or process not alive)" >> "$LOG_FILE"
    echo "[ERROR] Check log above for crash reason" >> "$LOG_FILE"
fi
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

if [ "$WATCHDOG_ENABLED" != "0" ] && [ -x "$MODDIR/system/bin/aatpe-watchdog" ]; then
    (
        w_since=$(cat "$RUN_DIR/.wd_superv_window" 2>/dev/null || echo 0)
        w_restarts=0
        while :; do
            sleep 10
            wpid=$(cat "$RUN_DIR/watchdog.pid" 2>/dev/null)
            alive=0
            if [ -n "$wpid" ] && [ -d "/proc/$wpid" ]; then
                wexe=$(readlink "/proc/$wpid/exe" 2>/dev/null)
                wstart=$(sed 's/^.*) //' "/proc/$wpid/stat" 2>/dev/null | cut -d' ' -f20)
                case "$wexe" in */system/bin/aatpe-watchdog)
                    [ -n "$wstart" ] && alive=1 ;;
                esac
            fi
            if [ "$alive" = "1" ]; then continue; fi
            now=$(date +%s)
            [ "$now" -lt "$((w_since + 60))" ] || { w_since=$now; w_restarts=0; }
            w_restarts=$((w_restarts + 1))
            echo "$w_since" > "$RUN_DIR/.wd_superv_window" 2>/dev/null
            if [ "$w_restarts" -gt 5 ]; then
                echo "[$(date '+%T')] watchdog supervisor: watchdog crashed $w_restarts in 60s, STOP respawning (manual recovery needed)" >> "$LOG_FILE"
                break
            fi
            echo "[$(date '+%T')] watchdog supervisor: respawning watchdog (attempt $w_restarts)" >> "$LOG_FILE"
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
[ -f "$RUN_DIR/control" ] && chmod 0600 "$RUN_DIR/control" 2>/dev/null
find "$RUN_DIR/state" -maxdepth 1 -type f -exec chmod 0600 {} + 2>/dev/null

if [ -d /data/adb/aatpe/backup ]; then
    _snap_count=$(find /data/adb/aatpe/backup -name '*.kv' -type f 2>/dev/null | wc -l)
    echo "[*] Found $_snap_count snapshot entries in backup dir" >> "$LOG_FILE"
fi

echo "[*] AATPE v6.0.0 C++ Edition service complete" >> "$LOG_FILE"
echo "[*] Mode: $(cat "$RUN_DIR/state/control_mode" 2>/dev/null || echo 'assist')" >> "$LOG_FILE"
echo "[*] Profile: $(cat "$RUN_DIR/state/current_profile" 2>/dev/null || echo 'balanced')" >> "$LOG_FILE"

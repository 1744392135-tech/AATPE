#!/system/bin/sh
# AATPE - Installation script with system-state snapshot
# NOTE: Do NOT use `set -u` here. KernelSU/Magisk source this script,
# and framework variables like $REPLACE may be unset in some environments.
# Use $MODPATH (provided by the framework) with fallback to $0 dirname.
MODDIR="${MODPATH:-$(dirname "$0")}"
MODID="${MODID:-aatpe}"

# ============================================================================
# Config merge + default-value migration helpers.
#
# Defined BEFORE any installer side effect so the host self-test
# (AATPE_MERGE_TEST=1) can source this script and exercise the pure merge logic
# without touching /data/adb or invoking installer-only functions.
# ----------------------------------------------------------------------------

# Normalize a bool-ish conf token to 0/1; empty/unrecognizable -> -1.
# The conf accepts true/false/1/0/on/off (see parseFlexBool in aatped_config.cpp).
norm_bool() {
    v=$(printf '%s' "$1" | tr 'A-Z' 'a-z' | tr -d ' \t\r')
    case "$v" in
        1|true|on|yes) echo 1 ;;
        0|false|off|no) echo 0 ;;
        *) echo -1 ;;
    esac
}

# Default-value migration table: "KEY|OLD_DEFAULT|NEW_DEFAULT".
#
# ROOT CAUSE (rel11 real device): merge_conf_with_old used "any key already
# present in the old conf keeps its old value". That means a SHIPPED DEFAULT
# CHANGE never reached upgraders: rel7 shipped C4_FREEZER_ENABLED=false,
# v6.0 ships =true, but the upgrader kept the old false -> the "new feature
# default ON" red line broke on every upgrade. Observed on the real device:
# state c4_freezer_enabled=0 / c4.json {"freezerAvailable":false,...} even
# though system/etc/aatpe/aatpe.conf line 292 says C4_FREEZER_ENABLED=true.
#
# Rule: if the old value EQUALS the key's historical OLD default (after bool
# normalization), the user never touched it -> adopt the NEW shipped default.
# If the old value DIFFERS from the old default, the user deliberately set it
# -> keep the user value. When in doubt (unparseable old value), the red line
# "new features default ON" wins: adopt the new shipped default.
CONF_DEFAULT_MIGRATION="
C4_FREEZER_ENABLED|false|true
EBPF_ENABLED|0|1
FEDERATED_ENABLED|0|1
"

# $1 = relative path under both MODDIR (shipped) and OLD_MODDIR (user)
merge_conf_with_old() {
    rel="$1"
    if [ -z "$OLD_MODDIR" ] || [ ! -f "$OLD_MODDIR/$rel" ]; then
        return 0
    fi
    shipped="$MODDIR/$rel"
    old="$OLD_MODDIR/$rel"
    [ -f "$shipped" ] || return 0
    # Work on a temp; keep the user's file as the base.
    tmp="$MODDIR/$rel.aatpe-newkeys"
    mig="$MODDIR/$rel.aatpe-migrated"
    : > "$tmp"; : > "$mig"
    # Append every shipped key that the old config does not already define.
    OLD_KEYS="$(sed -n 's/^\([A-Za-z0-9_]*\)=.*/\1/p' "$old" 2>/dev/null)"

    # Stream the OLD conf line by line. For a key in the migration table whose
    # old value equals the historical old default, replace the line with the
    # shipped (new default) line instead of preserving the stale default.
    migrated_any=""
    while IFS= read -r line; do
        case "$line" in
            ''|\#*) printf '%s\n' "$line" >> "$mig"; continue ;;
            *=*)
                k="${line%%=*}"
                v="${line#*=}"
                hit=""
                for entry in $CONF_DEFAULT_MIGRATION; do
                    mk=$(printf '%s' "$entry" | cut -d'|' -f1)
                    if [ "$mk" = "$k" ]; then hit="$entry"; break; fi
                done
                if [ -n "$hit" ]; then
                    olddef=$(printf '%s' "$hit" | cut -d'|' -f2)
                    shipped_line=$(grep -m1 "^$k=" "$shipped" 2>/dev/null)
                    ov=$(norm_bool "$v"); od=$(norm_bool "$olddef")
                    if [ -n "$shipped_line" ] && { [ "$ov" = "$od" ] || [ "$ov" = "-1" ]; }; then
                        # old value == historical shipped default (or unparseable ->
                        # red line wins): adopt the new shipped default.
                        printf '%s\n' "$shipped_line" >> "$mig"
                        migrated_any="$migrated_any $k"
                        continue
                    fi
                    # old value != historical default -> user deliberately set it.
                fi
                printf '%s\n' "$line" >> "$mig"
                ;;
            *) printf '%s\n' "$line" >> "$mig" ;;
        esac
    done < "$old"

    # Brand-new shipped keys the old conf lacks entirely.
    while IFS= read -r line; do
        case "$line" in
            ''|\#*) continue ;;
            *=*)
                k="${line%%=*}"
                # Exact whole-line key match. The old `case "$OLD_KEYS" in *"$k"*)`
                # substring test misfired when one key was a prefix of another:
                # e.g. shipped key "C4" matched an old "C4_FREEZER_ENABLED" line
                # and was dropped as if the user already defined it. grep -Fxq
                # matches the key only as a complete line in the newline-separated
                # OLD_KEYS list.
                if printf '%s\n' "$OLD_KEYS" | grep -Fxq -- "$k"; then
                    :                 # user already has this key -> keep user value
                else
                    printf '%s\n' "$line" >> "$tmp"  # brand-new key -> migrate default
                fi
                ;;
        esac
    done < "$shipped"

    if [ -s "$tmp" ] || [ -s "$mig" ]; then
        {
            echo "# --- AATPE upgrade: user config preserved; default-migration applied:$migrated_any ---"
            cat "$mig"
            if [ -s "$tmp" ]; then
                echo "# --- new keys introduced in this release ---"
                cat "$tmp"
            fi
        } > "$MODDIR/$rel.merged" && mv "$MODDIR/$rel.merged" "$shipped"
    fi
    rm -f "$tmp" "$mig"
}

# Host self-test entry point: source customize.sh with AATPE_MERGE_TEST=1 to
# load ONLY the helpers above, then stop before installer side effects.
if [ "${AATPE_MERGE_TEST:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

# --- Coverage-install (update) hint (R14-101, v4.1) ------------------------
# If an OLD daemon is still running (no reboot between updates) and the shipped
# binary differs (size), tell the user to reboot. We do NOT kill the daemon here
# (killing would race .rec restore against the new install). Standard practice
# across Magisk/KSU/APatch: a reboot is the clean restart.
_oldpid=$(cat /data/adb/aatpe/state/daemon_pid 2>/dev/null)
if [ -n "$_oldpid" ] && [ -d "/proc/$_oldpid" ] 2>/dev/null; then
    ui_print "[!] AATPE: an older daemon instance is still running."
    ui_print "[!] Reboot after install so the new binaries take effect cleanly."
fi
unset _oldpid

# ============================================================================
# G10-002: pre-flight install gates (fail-closed, before any side effect).
# abort() is provided by the Magisk/KSU/APatch installer framework.
# ============================================================================

# --- 1) ABI whitelist: this module ships arm64-v8a binaries ONLY ------------
# $ABI is set by Magisk; fall back to the primary ro.product.cpu.abi on KSU/APatch.
DEV_ABI="${ABI:-$(getprop ro.product.cpu.abi 2>/dev/null)}"
case "$DEV_ABI" in
    arm64-v8a) : ;;
    *)
        ui_print "[!] Unsupported ABI: '$DEV_ABI'."
        ui_print "[!] AATPE ships arm64-v8a binaries only; aborting install."
        abort "[!] AATPE: unsupported ABI ($DEV_ABI); arm64-v8a required."
        ;;
esac

# --- 2) Android version floor: built against API 29 (Android 10) ----------
SDK="$(getprop ro.build.version.sdk 2>/dev/null | tr -d ' \r')"
case "$SDK" in
    ''|*[!0-9]*) ui_print "[!] WARNING: cannot read ro.build.version.sdk; continuing." ;;
    *)
        if [ "$SDK" -lt 29 ]; then
            ui_print "[!] Android SDK $SDK < 29 (Android 10); AATPE requires API 29+."
            abort "[!] AATPE: Android version too old (sdk=$SDK, need >=29)."
        fi
        ;;
esac

# --- 3) Locate the PREVIOUS install (for downgrade check + config preserve) -
# During an in-place upgrade the new package is extracted to MODPATH while the
# previously-active install still lives at one of these paths until reboot.
OLD_MODDIR=""
for c in "/data/adb/modules/$MODID" "/data/adb/ksu/modules/$MODID" "/data/adb/ap/modules/$MODID"; do
    if [ -d "$c" ] && [ "$c" != "$MODDIR" ] && [ -f "$c/module.prop" ]; then
        OLD_MODDIR="$c"; break
    fi
done

# --- 4) versionCode downgrade rejection -------------------------------------
NEW_CODE="$(grep -m1 '^versionCode=' "$MODDIR/module.prop" 2>/dev/null | cut -d= -f2 | tr -d ' \r')"
if [ -n "$OLD_MODDIR" ]; then
    OLD_CODE="$(grep -m1 '^versionCode=' "$OLD_MODDIR/module.prop" 2>/dev/null | cut -d= -f2 | tr -d ' \r')"
    if [ -n "$OLD_CODE" ] && [ -n "$NEW_CODE" ] && [ "$NEW_CODE" -lt "$OLD_CODE" ] 2>/dev/null; then
        ui_print "[!] Downgrade refused: new versionCode=$NEW_CODE < installed=$OLD_CODE."
        abort "[!] AATPE: downgrade not allowed ($NEW_CODE < $OLD_CODE)."
    fi
fi

# --- 5) Critical payload must be present, else fail closed -----------------
for bin in aatped aatpe-cli aatpe-watchdog aatpe-probe; do
    if [ ! -f "$MODDIR/system/bin/$bin" ]; then
        ui_print "[!] Critical binary missing: system/bin/$bin"
        abort "[!] AATPE: package incomplete (missing $bin)."
    fi
done

# --- 5b) ROUND-010: 16KB page-size alignment gate (R1 §4) -------------------
# On a 16KB-kernel device a 4KB-aligned binary fails to exec (ENOEXEC). If this
# device's page size is 16384, every shipped native binary's LOAD segment must
# be 16384-aligned (p_align == 0x4000).
PAGE_SZ="$(getconf PAGE_SIZE 2>/dev/null | tr -d ' \r')"
if [ "$PAGE_SZ" = "16384" ]; then
    ui_print "[*] 16KB page device detected; verifying binary alignment..."
    bad_align=""
    for bin in aatped aatpe-cli aatpe-watchdog aatpe-probe; do
        bpath="$MODDIR/system/bin/$bin"
        if command -v readelf >/dev/null 2>&1; then
            # v6.0.0 RT-B B1: the old `readelf -l | grep -qE 'LOAD .* (10000|4000)\b'`
            # was broken on non-wide output: readelf wraps each LOAD program header
            # across TWO lines (address/size on the LOAD line, p_align on the NEXT
            # line), so the LOAD line never carried the alignment value and EVERY
            # shipped binary (p_align=0x4000) was misjudged as bad -> 16KB devices
            # aborted install. Use `readelf -lW` (wide: one line per segment, p_align
            # last column) and awk the LAST field of every LOAD row. Require ALL
            # LOAD alignments to be exactly 0x4000 or 0x10000 (a 0x1000/4KB LOAD is
            # rejected). No `\b` (toybox/busybox grep compatibility); column match
            # is explicit. If readelf sees zero LOAD rows, treat as bad (cannot
            # verify) rather than silently trusting.
            if ! readelf -lW "$bpath" 2>/dev/null | awk '
                /^[[:space:]]*LOAD[[:space:]]/ {
                    seen = 1
                    a = $NF
                    if (a != "0x4000" && a != "0x10000") bad = 1
                }
                END { exit (seen && !bad) ? 0 : 1 }
            '; then
                bad_align="$bad_align $bin"
            fi
        else
            # C11 (v4.1): our shipped binaries are built with
            # -Wl,-z,max-page-size=16384, so they are ALREADY 16KB-compatible. If
            # readelf is unavailable in recovery we cannot re-verify, but we do NOT
            # abort (a known-good compatible build must not be blocked by a missing
            # host tool). Only a measured 0x1000 (4KB) LOAD alignment aborts.
            ui_print "[*] readelf unavailable; trusting prebuilt 16KB-aligned binaries"
        fi
    done
    if [ -n "$bad_align" ]; then
        abort "[!] AATPE: 16KB page device but binaries NOT 16KB-aligned:$bad_align -- use a 16KB-aligned build (Makefile max-page-size=16384)."
    fi
    ui_print "[*] 16KB alignment OK"
fi

# --- 6) Preserve user-edited config across upgrade, migrate new keys --------
# merge_conf_with_old (with default-value migration) is defined near the top of
# this file. On upgrade it keeps the user's tuned values, but adopts the new
# shipped default for keys whose old value was still the historical old default
# (e.g. C4_FREEZER_ENABLED: rel7 default=false -> v6.0 default=true).

merge_conf_with_old "system/etc/aatpe/aatpe.conf"
for p in balanced battery gaming thermal; do
    merge_conf_with_old "system/etc/aatpe/profiles/$p.conf"
done


# ---------------------------------------------------------------------------
# System-state snapshot for clean uninstall (uninstall.sh restores these).
# Layout:
#   /data/adb/aatpe/backup/<nn>_<safe_path>.kv
#     path=<original sysfs/procfs path>
#     value_b64=<base64 of original value>
#     timestamp=<epoch>
#     sha256=<sha256 of value_b64 for snapshot self-verification>
# ---------------------------------------------------------------------------
BACKUP_DIR="/data/adb/aatpe/backup"
STATE_DIR="/data/adb/aatpe"
SNAPSHOT_LOG="$STATE_DIR/snapshot.log"

mkdir -p "$BACKUP_DIR" "$STATE_DIR" 2>/dev/null

# ---- item 15: per-install snapshot isolation ------------------------------
# Every install generates a fresh INSTALL_ID; snapshots go to
# $BACKUP_DIR/$INSTALL_ID/. Uninstall only restores the CURRENT install_id's
# snapshots, so an upgrade cannot have the new uninstall.sh walk
# the old install's *.kv and restore stale state. Old install_id dirs are
# cleaned here on install.
INSTALL_ID="$(date +%s)_$(od -An -N2 -tu2 /dev/urandom 2>/dev/null | tr -d ' ')"
# I-005 (v4.3.4): if urandom read failed, do NOT fall back to a predictable
# hardcoded "_0000" suffix (an attacker who knows the rough install time could
# then guess the install_id). Use shell-built-in entropy ($RANDOM, doubled for
# ~31 bits) plus $$ as a last-resort fallback on shells without $RANDOM.
[ -z "$INSTALL_ID" ] && INSTALL_ID="$(date +%s)_${RANDOM}${RANDOM}$$"
# Remove previous install_id subdirs (anything that looks like <ts>_<rand>),
# but keep the top-level backup dir itself.
# R9-018 / LED-150: the previous cleanup relied only on the `<digits>_<digits>`
# name pattern, so a symlink planted in the backup dir (or an unrelated dir that
# happened to match) would be `rm -rf`'d. Guard: skip symlinks explicitly (do not
# follow them), and only remove a candidate if it actually holds our snapshot .kv
# files -- i.e. it is one of OUR install backup dirs, not arbitrary on-disk state.
for d in "$BACKUP_DIR"/*_*; do
    [ -e "$d" ] || continue
    [ -L "$d" ] && { echo "[cleanup] skip $d (symlink)" >> "$SNAPSHOT_LOG" 2>/dev/null; continue; }
    [ -d "$d" ] || continue
    case "$d" in
        */[0-9]*_[0-9]*)
            # Confirm it is an AATPE snapshot dir (contains .kv files) before rm.
            if ls "$d"/*.kv >/dev/null 2>&1; then
                rm -rf "$d" 2>/dev/null
            fi
            ;;
    esac
done
BACKUP_DIR="$BACKUP_DIR/$INSTALL_ID"
mkdir -p "$BACKUP_DIR" 2>/dev/null
echo "$INSTALL_ID" > "$STATE_DIR/install_id"

# Record boot_id so uninstall.sh can sanity-check "same boot" semantics.
BOOT_ID=""
if [ -r /proc/sys/kernel/random/boot_id ]; then
    BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d '\n')
fi
echo "boot_id=$BOOT_ID" > "$STATE_DIR/boot_id"
echo "install_ts=$(date +%s)" >> "$STATE_DIR/boot_id"

# ROUND-014 P3 (C-R14-006): snapshot_one() DELETED — dead code after ROUND-013
# removed all snapshot calls. AATPE no longer records any system-node .kv
# snapshots (see write-node inventory below). The manifest scaffolding below
# writes a 0-entry manifest only.
SNAP_IDX=0

# ROUND-013: WRITE-NODE INVENTORY (authoritative). AATPE (the four C++
# binaries) NEVER modifies these system nodes at runtime:
#   - scaling_governor / scaling_min,max,cur,setspeed / cpuinfo_freq
#   - per-governor tuning (target_loads/sampling_rate/rate_limit)
#   - /proc/sys kernel knobs (sched_boost/swappiness/dirty_*/numa_balancing/...)
#   - thermal_zone temp/policy/mode, cooling_device cur_state
#   - GPU (kgsl/mali) governor/min,max freq
#   - cpuset group cpus / mems (top-level or per-group)
# AATPE only changes, on a per-thread / per-cgroup basis:
#   - per-tid: affinity, sched policy/priority, nice, task-level uclamp
#   - cgroup MEMBERSHIP: writing a tid into /dev/cpuset/{grp}/tasks or
#     /sys/fs/cgroup/cpuset/{grp}/cgroup.procs
#   - (ROUND-013) top-app cpu.uclamp.min boost floor, reset on clean exit
#     via state/cg_uclamp_active; boot兜底 in service.sh.
# Therefore NONE of those read-only-but-previously-snapshotted system nodes are
# recorded, so uninstall can never clobber another component's / ROM's later
# legitimate changes (review items 17/18/19). Dynamic nodes (cur_freq/temp/
# cur_state) are never recorded/restored either. Thread-level state is restored
# from the daemon's own recovery/*.rec (see uninstall.sh).

echo "[snapshot] done: $SNAP_IDX entries under $BACKUP_DIR" >> "$SNAPSHOT_LOG"

# ---- Write snapshot manifest (index file) ----------------------------------
# Lists all .kv entries with their original path for uninstall.sh verification.
SNAP_MANIFEST="$BACKUP_DIR/snapshot.manifest"
{
    echo "# AATPE snapshot manifest"
    echo "# generated=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "# boot_id=$BOOT_ID"
    echo "# total_entries=$SNAP_IDX"
    for kv in "$BACKUP_DIR"/*.kv; do
        [ -f "$kv" ] || continue
        _p=$(grep '^path=' "$kv" 2>/dev/null | head -1 | cut -d= -f2-)
        echo "$(basename "$kv") $_p"
    done
} > "$SNAP_MANIFEST"
echo "[snapshot] manifest written: $SNAP_MANIFEST ($SNAP_IDX entries)" >> "$SNAPSHOT_LOG"

# SECURITY FIX: integrity manifest is generated HERE (trusted install
# environment), NOT auto-generated at runtime by service.sh. This ensures the
# SHA-256 baseline is computed from the untouched module files delivered by the
# package. service.sh still keeps a bootstrap fallback if the manifest is
# missing, but it will NEVER regenerate the manifest after a FAILED verify.

# Set permissions for binaries
set_perm "$MODDIR"/system/bin/aatped 0 0 0755
set_perm "$MODDIR"/system/bin/aatpe-cli 0 0 0755
set_perm "$MODDIR"/system/bin/aatpe-watchdog 0 0 0755
set_perm "$MODDIR"/system/bin/aatpe-probe 0 0 0755

# Set permissions for config files
set_perm "$MODDIR"/system/etc/aatpe/aatpe.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/rules/thread_roles.conf 0 0 0644

# v73: Set permissions for eBPF files (v94: sched_events.bpf.o 可能不存在, 改为存在性条件判断)
[ -f "$MODDIR"/system/etc/aatpe/ebpf/sched_events.bpf.o ] && set_perm "$MODDIR"/system/etc/aatpe/ebpf/sched_events.bpf.o 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/ebpf/sched_events.bpf.c 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/ebpf/sched_events_embedded.h 0 0 0644

# Set permissions for profiles
set_perm "$MODDIR"/system/etc/aatpe/profiles/balanced.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/profiles/battery.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/profiles/gaming.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/profiles/thermal.conf 0 0 0644

# Set permissions for web UI (root webroot/ for KernelSU + webui/ for Magisk + system/etc for runtime)
set_perm "$MODDIR"/webroot/index.html 0 0 0644
set_perm "$MODDIR"/webui/index.html 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/webui/index.html 0 0 0644

# Set permissions for scripts
set_perm "$MODDIR"/service.sh 0 0 0755
set_perm "$MODDIR"/post-fs-data.sh 0 0 0755
set_perm "$MODDIR"/boot-completed.sh 0 0 0755
set_perm "$MODDIR"/uninstall.sh 0 0 0755

# ---- Generate integrity manifest during trusted install ----------
# All files are now copied and permissions are set. Run aatped --generate-manifest
# so the SHA-256 baseline is baked from the pristine module payload. This is the
# ONLY trusted place where the manifest is created; service.sh will not rebuild
# it at runtime even if verify fails.
mkdir -p /data/adb/aatpe 2>/dev/null
# P3 hardening: converge the persistent trust-anchor dir to root-private 0700
# (idempotent; never touches contents). Holds manifest.sha256 / re-anchor marker.
chmod 0700 /data/adb/aatpe 2>/dev/null
chown 0:0 /data/adb/aatpe 2>/dev/null
if [ -x "$MODDIR/system/bin/aatped" ]; then
    # ROUND-002: pass $MODDIR explicitly. Without it, --generate-manifest would
    # auto-probe /data/adb/modules/aatpe FIRST and (during an upgrade, when the
    # new tree is still in modules_update and the old tree in modules/) anchor the
    # OLD binaries. $MODDIR is the pristine install tree -> anchor the new payload.
    "$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR" 2>/dev/null
    if [ -f /data/adb/aatpe/manifest.sha256 ]; then
        ui_print "[*] Integrity manifest generated (trusted install)"
    else
        ui_print "[!] WARNING: integrity manifest was not created; service.sh will re-anchor on first boot"
    fi
else
    ui_print "[!] WARNING: aatped binary not found for manifest generation; service.sh will re-anchor on first boot"
fi

# W3-TASK A: trusted-install re-anchor request.
#
# Why this exists (root cause of the historical "TAMPER DETECTED after upgrade ->
# eBPF refuses attach -> polling" failure):
#   On Magisk/KSU upgrade the new payload lands in modules_update/aatpe and is NOT
#   promoted to modules/aatpe until the FIRST reboot. The manifest lives OUTSIDE
#   the module dir at /data/adb/aatpe/manifest.sha256 and therefore SURVIVES the
#   upgrade (uninstall is not run). If the recovery-time generate above failed for
#   any reason (binary not yet runnable in recovery's restricted context, 16KB-page
#   mismatch, SELinux, `2>/dev/null` swallowing the error), the OLD v1 baseline
#   stays on disk. service.sh previously only bootstrapped the manifest when it
#   was *missing*; an upgrade leaves it *stale*, so post-swap the new v2 files are
#   hashed against the v1 chain -> definite TAMPER -> fail-closed eBPF gate ->
#   polling fallback.
#
# Fix: ALWAYS drop a one-time re-anchor request here, in the TRUSTED install
# context. service.sh consumes it on the first boot AFTER the modules_update->
# modules swap (full userspace up, binary guaranteed runnable) and regenerates the
# baseline against the FINAL settled module dir -- BEFORE any integrity verify /
# privileged eBPF attach. This closes the "stale baseline vs new files" window.
#
# Why this does NOT weaken fail-closed:
#   - The marker is written ONLY by customize.sh, which runs as root in the trusted
#     recovery/install context. A runtime tamperer cannot legitimately produce it.
#   - Without the marker (every normal boot), service.sh behavior is UNCHANGED:
#     verify runs fail-closed and the manifest is NEVER regenerated on a FAILED
#     verify. The marker is consumed exactly once and then deleted.
#   - The C++ verify side (integrity.cpp) is untouched: missing manifest still
#     means FAILED, still no auto-bootstrap on the verify path.
REANCHOR_MARKER="/data/adb/aatpe/manifest_reanchor.pending"
{
    echo "versionCode=$NEW_CODE"
    echo "moddir=$MODDIR"
    echo "ts=$(date +%s)"
} > "$REANCHOR_MARKER" 2>/dev/null
chmod 0600 "$REANCHOR_MARKER" 2>/dev/null
ui_print "[*] Re-anchor marker written; service.sh will re-baseline on first post-upgrade boot"

# ============================================================================
# Retired-file cleanup (W3-TASK B / audit #17-19)
# ----------------------------------------------------------------------------
# Overlay updates (Magisk/KSU/APatch) extract the new zip over the existing
# module dir but do NOT delete files that were dropped from the package. We keep
# an explicit WHITELIST of files retired in earlier versions and rm them here so
# a stale page / dead helper is never left to shadow the new install.
#
# Safety contract (never weaken):
#   - Every entry is a $MODDIR-relative path. The loop refuses anything that is
#     not a regular file (symlink/dir/device) and refuses paths that escape
#     $MODDIR (no "..", no absolute). This can NEVER delete files outside the
#     module dir, so other modules / ROM / user edits are untouched.
#   - We NEVER rm recursively, never glob, never touch STATE_DIR / user data
#     (aatpe.conf is preserved by merge_conf_with_old above).
#   - When you DROP a shipped file from the module, add its $MODDIR-relative
#     path to RETIRED_FILES in the SAME release.
#
# Current retired set: (empty -- v4.2.1 ships the same file set as v4.1.x; the
# phantom customize.sh / aatpe-temp-probe.sh entries removed from the integrity
# chain were never shipped and therefore need no cleanup).
RETIRED_FILES="
"

# Hardened per-entry removal. $1 = $MODDIR-relative path.
remove_retired() {
    _rel="$1"
    [ -n "$_rel" ] || return 0
    # reject traversal / absolute / empty
    case "$_rel" in
        ""|/*|*..*) return 0 ;;
    esac
    _full="$MODDIR/$_rel"
    # must be a regular file, not a symlink (defense: never follow a planted link)
    [ -f "$_full" ] || return 0
    [ -L "$_full" ] && return 0
    rm -f "$_full" 2>/dev/null && ui_print "[cleanup] removed retired file: $_rel"
}
for _r in $RETIRED_FILES; do
    remove_retired "$_r"
done
unset _r _full _rel

ui_print "AATPE v6.0.0 installed successfully"
ui_print "Binaries: aatped, aatpe-cli, aatpe-watchdog, aatpe-probe"
ui_print "Config: /system/etc/aatpe/aatpe.conf"
ui_print "Rules: /system/etc/aatpe/rules/thread_roles.conf"
ui_print "System snapshot: $SNAP_IDX nodes backed up to $BACKUP_DIR"
ui_print "Reboot to activate"

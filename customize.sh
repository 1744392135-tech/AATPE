#!/system/bin/sh
MODDIR="${MODPATH:-$(dirname "$0")}"
MODID="${MODID:-aatpe}"

norm_bool() {
    v=$(printf '%s' "$1" | tr 'A-Z' 'a-z' | tr -d ' \t\r')
    case "$v" in
        1|true|on|yes) echo 1 ;;
        0|false|off|no) echo 0 ;;
        *) echo -1 ;;
    esac
}

CONF_DEFAULT_MIGRATION="
C4_FREEZER_ENABLED|false|true
EBPF_ENABLED|0|1
FEDERATED_ENABLED|0|1
"

merge_conf_with_old() {
    rel="$1"
    if [ -z "$OLD_MODDIR" ] || [ ! -f "$OLD_MODDIR/$rel" ]; then
        return 0
    fi
    shipped="$MODDIR/$rel"
    old="$OLD_MODDIR/$rel"
    [ -f "$shipped" ] || return 0
    tmp="$MODDIR/$rel.aatpe-newkeys"
    mig="$MODDIR/$rel.aatpe-migrated"
    : > "$tmp"; : > "$mig"
    OLD_KEYS="$(sed -n 's/^\([A-Za-z0-9_]*\)=.*/\1/p' "$old" 2>/dev/null)"

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
                        printf '%s\n' "$shipped_line" >> "$mig"
                        migrated_any="$migrated_any $k"
                        continue
                    fi
                fi
                printf '%s\n' "$line" >> "$mig"
                ;;
            *) printf '%s\n' "$line" >> "$mig" ;;
        esac
    done < "$old"

    while IFS= read -r line; do
        case "$line" in
            ''|\#*) continue ;;
            *=*)
                k="${line%%=*}"
                if printf '%s\n' "$OLD_KEYS" | grep -Fxq -- "$k"; then
                    :
                else
                    printf '%s\n' "$line" >> "$tmp"
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

if [ "${AATPE_MERGE_TEST:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

_oldpid=$(cat /data/adb/aatpe/state/daemon_pid 2>/dev/null)
if [ -n "$_oldpid" ] && [ -d "/proc/$_oldpid" ] 2>/dev/null; then
    ui_print "[!] AATPE: an older daemon instance is still running."
    ui_print "[!] Reboot after install so the new binaries take effect cleanly."
fi
unset _oldpid

DEV_ABI="${ABI:-$(getprop ro.product.cpu.abi 2>/dev/null)}"
case "$DEV_ABI" in
    arm64-v8a) : ;;
    *)
        ui_print "[!] Unsupported ABI: '$DEV_ABI'."
        ui_print "[!] AATPE ships arm64-v8a binaries only; aborting install."
        abort "[!] AATPE: unsupported ABI ($DEV_ABI); arm64-v8a required."
        ;;
esac

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

OLD_MODDIR=""
for c in "/data/adb/modules/$MODID" "/data/adb/ksu/modules/$MODID" "/data/adb/ap/modules/$MODID"; do
    if [ -d "$c" ] && [ "$c" != "$MODDIR" ] && [ -f "$c/module.prop" ]; then
        OLD_MODDIR="$c"; break
    fi
done

NEW_CODE="$(grep -m1 '^versionCode=' "$MODDIR/module.prop" 2>/dev/null | cut -d= -f2 | tr -d ' \r')"
if [ -n "$OLD_MODDIR" ]; then
    OLD_CODE="$(grep -m1 '^versionCode=' "$OLD_MODDIR/module.prop" 2>/dev/null | cut -d= -f2 | tr -d ' \r')"
    if [ -n "$OLD_CODE" ] && [ -n "$NEW_CODE" ] && [ "$NEW_CODE" -lt "$OLD_CODE" ] 2>/dev/null; then
        ui_print "[!] Downgrade refused: new versionCode=$NEW_CODE < installed=$OLD_CODE."
        abort "[!] AATPE: downgrade not allowed ($NEW_CODE < $OLD_CODE)."
    fi
fi

for bin in aatped aatpe-cli aatpe-watchdog aatpe-probe; do
    if [ ! -f "$MODDIR/system/bin/$bin" ]; then
        ui_print "[!] Critical binary missing: system/bin/$bin"
        abort "[!] AATPE: package incomplete (missing $bin)."
    fi
done

PAGE_SZ="$(getconf PAGE_SIZE 2>/dev/null | tr -d ' \r')"
if [ "$PAGE_SZ" = "16384" ]; then
    ui_print "[*] 16KB page device detected; verifying binary alignment..."
    bad_align=""
    for bin in aatped aatpe-cli aatpe-watchdog aatpe-probe; do
        bpath="$MODDIR/system/bin/$bin"
        if command -v readelf >/dev/null 2>&1; then
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
            ui_print "[*] readelf unavailable; trusting prebuilt 16KB-aligned binaries"
        fi
    done
    if [ -n "$bad_align" ]; then
        abort "[!] AATPE: 16KB page device but binaries NOT 16KB-aligned:$bad_align -- use a 16KB-aligned build (Makefile max-page-size=16384)."
    fi
    ui_print "[*] 16KB alignment OK"
fi

merge_conf_with_old "system/etc/aatpe/aatpe.conf"
for p in balanced battery gaming thermal; do
    merge_conf_with_old "system/etc/aatpe/profiles/$p.conf"
done

BACKUP_DIR="/data/adb/aatpe/backup"
STATE_DIR="/data/adb/aatpe"
SNAPSHOT_LOG="$STATE_DIR/snapshot.log"

mkdir -p "$BACKUP_DIR" "$STATE_DIR" 2>/dev/null

INSTALL_ID="$(date +%s)_$(od -An -N2 -tu2 /dev/urandom 2>/dev/null | tr -d ' ')"
[ -z "$INSTALL_ID" ] && INSTALL_ID="$(date +%s)_${RANDOM}${RANDOM}$$"
for d in "$BACKUP_DIR"/*_*; do
    [ -e "$d" ] || continue
    [ -L "$d" ] && { echo "[cleanup] skip $d (symlink)" >> "$SNAPSHOT_LOG" 2>/dev/null; continue; }
    [ -d "$d" ] || continue
    case "$d" in
        */[0-9]*_[0-9]*)
            if ls "$d"/*.kv >/dev/null 2>&1; then
                rm -rf "$d" 2>/dev/null
            fi
            ;;
    esac
done
BACKUP_DIR="$BACKUP_DIR/$INSTALL_ID"
mkdir -p "$BACKUP_DIR" 2>/dev/null
echo "$INSTALL_ID" > "$STATE_DIR/install_id"

BOOT_ID=""
if [ -r /proc/sys/kernel/random/boot_id ]; then
    BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d '\n')
fi
echo "boot_id=$BOOT_ID" > "$STATE_DIR/boot_id"
echo "install_ts=$(date +%s)" >> "$STATE_DIR/boot_id"

SNAP_IDX=0

echo "[snapshot] done: $SNAP_IDX entries under $BACKUP_DIR" >> "$SNAPSHOT_LOG"

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

set_perm "$MODDIR"/system/bin/aatped 0 0 0755
set_perm "$MODDIR"/system/bin/aatpe-cli 0 0 0755
set_perm "$MODDIR"/system/bin/aatpe-watchdog 0 0 0755
set_perm "$MODDIR"/system/bin/aatpe-probe 0 0 0755

set_perm "$MODDIR"/system/etc/aatpe/aatpe.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/rules/thread_roles.conf 0 0 0644

[ -f "$MODDIR"/system/etc/aatpe/ebpf/sched_events.bpf.o ] && set_perm "$MODDIR"/system/etc/aatpe/ebpf/sched_events.bpf.o 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/ebpf/sched_events.bpf.c 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/ebpf/sched_events_embedded.h 0 0 0644

set_perm "$MODDIR"/system/etc/aatpe/profiles/balanced.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/profiles/battery.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/profiles/gaming.conf 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/profiles/thermal.conf 0 0 0644

set_perm "$MODDIR"/webroot/index.html 0 0 0644
set_perm "$MODDIR"/webui/index.html 0 0 0644
set_perm "$MODDIR"/system/etc/aatpe/webui/index.html 0 0 0644

set_perm "$MODDIR"/service.sh 0 0 0755
set_perm "$MODDIR"/post-fs-data.sh 0 0 0755
set_perm "$MODDIR"/boot-completed.sh 0 0 0755
set_perm "$MODDIR"/uninstall.sh 0 0 0755

mkdir -p /data/adb/aatpe 2>/dev/null
chmod 0700 /data/adb/aatpe 2>/dev/null
chown 0:0 /data/adb/aatpe 2>/dev/null
if [ -x "$MODDIR/system/bin/aatped" ]; then
    "$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR" 2>/dev/null
    if [ -f /data/adb/aatpe/manifest.sha256 ]; then
        ui_print "[*] Integrity manifest generated (trusted install)"
    else
        ui_print "[!] WARNING: integrity manifest was not created; service.sh will re-anchor on first boot"
    fi
else
    ui_print "[!] WARNING: aatped binary not found for manifest generation; service.sh will re-anchor on first boot"
fi

REANCHOR_MARKER="/data/adb/aatpe/manifest_reanchor.pending"
{
    echo "versionCode=$NEW_CODE"
    echo "moddir=$MODDIR"
    echo "ts=$(date +%s)"
} > "$REANCHOR_MARKER" 2>/dev/null
chmod 0600 "$REANCHOR_MARKER" 2>/dev/null
ui_print "[*] Re-anchor marker written; service.sh will re-baseline on first post-upgrade boot"

RETIRED_FILES="
"

remove_retired() {
    _rel="$1"
    [ -n "$_rel" ] || return 0
    case "$_rel" in
        ""|/*|*..*) return 0 ;;
    esac
    _full="$MODDIR/$_rel"
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

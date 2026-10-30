#!/system/bin/sh

PERSIST_DIR="/data/adb/aatpe"
BOOT_MARKER="$PERSIST_DIR/boot_in_progress"
FAIL_FILE="$PERSIST_DIR/boot_fail_count"
LATCH_FILE="$PERSIST_DIR/self_disabled_latch"
LOG="$PERSIST_DIR/boot.log"

rm -f "$BOOT_MARKER" "$LATCH_FILE" 2>/dev/null
echo "0" > "$FAIL_FILE" 2>/dev/null
chmod 0600 "$FAIL_FILE" 2>/dev/null

echo "$(date '+%F %T') boot-completed: healthy, boot_fail_count reset" >> "$LOG" 2>/dev/null

exit 0

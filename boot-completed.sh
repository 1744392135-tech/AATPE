#!/system/bin/sh
# G10-003: AATPE boot-completed.sh
# Runs only AFTER sys.boot_completed=1 (device fully booted). This is the healthy
# endpoint that (a) proves the module did NOT cause a bootloop and (b) resets the
# consecutive-boot-failure counter used by the post-fs-data self-disable watchdog.
#
# It deliberately does almost nothing heavy: the daemon + watchdog are already
# started by service.sh. Keep this script seconds-fast.

PERSIST_DIR="/data/adb/aatpe"
BOOT_MARKER="$PERSIST_DIR/boot_in_progress"
FAIL_FILE="$PERSIST_DIR/boot_fail_count"
LATCH_FILE="$PERSIST_DIR/self_disabled_latch"
LOG="$PERSIST_DIR/boot.log"

# Healthy boot: clear the "in progress" marker, the re-enable latch, and reset
# the failure counter. (ROUND-003: clearing the latch is what lets a recovery
# reboot, once it completes, settle into the fully-healthy steady state.)
rm -f "$BOOT_MARKER" "$LATCH_FILE" 2>/dev/null
echo "0" > "$FAIL_FILE" 2>/dev/null
chmod 0600 "$FAIL_FILE" 2>/dev/null

# If we had previously self-disabled due to a transient bootloop, a clean
# boot means the user probably resolved it (or it was flaky) — leave the
# `disable` flag in place deliberately: the user must re-enable from the
# Manager. Auto-re-enabling would defeat the safety feature.

echo "$(date '+%F %T') boot-completed: healthy, boot_fail_count reset" >> "$LOG" 2>/dev/null

# If a stale self-disable happened but the module is otherwise fine, this hook is
# still skipped by the framework when `disable` is present, so we never get here.
exit 0

#!/bin/bash
set -u
fail=0
check() {
  if [ "$2" = "$3" ]; then echo "PASS  $1 = $2";
  else echo "FAIL  $1: want $3 got $2"; fail=1; fi
}

run_postfs() {
  local BOOT_MARKER="$PERSIST_DIR/boot_in_progress"
  local FAIL_FILE="$PERSIST_DIR/boot_fail_count"
  local LATCH="$PERSIST_DIR/self_disabled_latch"
  local LIMIT=3 CUR="$1"
  if [ -f "$BOOT_MARKER" ]; then
    local PREV; PREV="$(cat "$BOOT_MARKER" 2>/dev/null)"
    if [ -n "$CUR" ] && [ "$PREV" = "$CUR" ]; then :
    elif [ -f "$LATCH" ]; then
      rm -f "$BOOT_MARKER" "$LATCH"; echo 0 > "$FAIL_FILE"
    else
      local fc; fc="$(cat "$FAIL_FILE" 2>/dev/null)"; [ -z "$fc" ] && fc=0
      fc=$((fc+1)); echo "$fc" > "$FAIL_FILE"
      if [ "$fc" -ge "$LIMIT" ]; then touch "$MODDIR/disable" "$LATCH"; return 2; fi
    fi
  fi
  echo "$CUR" > "$BOOT_MARKER"
  return 0
}
run_bootcompleted() {
  rm -f "$PERSIST_DIR/boot_in_progress" "$PERSIST_DIR/self_disabled_latch"
  echo 0 > "$PERSIST_DIR/boot_fail_count"
}

T=$(mktemp -d); MODDIR="$T/mod"; PERSIST_DIR="$T/persist"; mkdir -p "$MODDIR" "$PERSIST_DIR"
echo 0 > "$PERSIST_DIR/boot_fail_count"

echo "--- Scenario A: 3 normal reboots (each boot completes) -> never disabled ---"
run_postfs bootA; run_bootcompleted
run_postfs bootB; run_bootcompleted
run_postfs bootC; run_bootcompleted
check "A: disable absent" "$([ -f $MODDIR/disable ] && echo yes || echo no)" "no"
check "A: count 0" "$(cat $PERSIST_DIR/boot_fail_count)" "0"

echo "--- Scenario B: 4 incomplete boots (first opens marker, 3 counted) -> self-disabled ---"
rm -f "$MODDIR/disable"; run_bootcompleted
run_postfs boot1
run_postfs boot2
run_postfs boot3
run_postfs boot4
rc=$?
check "B: disable armed after 4 incomplete boots" "$([ -f $MODDIR/disable ] && echo yes || echo no)" "yes"
check "B: exit rc" "$rc" "2"

echo "--- Scenario C: user re-enables + ONE healthy boot -> recovers, no re-disable ---"
rm -f "$MODDIR/disable"
run_postfs bootR
check "C: no immediate re-disable" "$([ -f $MODDIR/disable ] && echo yes || echo no)" "no"
check "C: latch cleared" "$([ -f $PERSIST_DIR/self_disabled_latch ] && echo yes || echo no)" "no"
check "C: count reset 0" "$(cat $PERSIST_DIR/boot_fail_count)" "0"
run_bootcompleted
check "C: after healthy boot, no disable" "$([ -f $MODDIR/disable ] && echo yes || echo no)" "no"

echo "--- Scenario D: recovered device bootloops again -> re-arms ---"
run_postfs b1; run_postfs b2; run_postfs b3; run_postfs b4; rc=$?
check "D: re-disabled after 4 more incomplete boots" "$([ -f $MODDIR/disable ] && echo yes || echo no)" "yes"
check "D: rc" "$rc" "2"

echo; [ $fail -eq 0 ] && echo "ALL STATE-MACHINE TESTS PASS" || { echo "SOME TESTS FAILED"; exit 1; }

#!/bin/bash
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export QA_MODE=plain;
source "$QA_DIR/common.sh"
PROD_FLAGS="$PROD_FLAGS -DAATPE_HOST_TEST"
DRIVER_FLAGS="$DRIVER_FLAGS -DAATPE_HOST_TEST"

ART="$QA_DIR/artifacts"; mkdir -p "$ART"
NAME="fault_inject"
PROD="aatpe_recovery aatped_utils"
STUBS="log_stub sprop_stub"

echo "==> [qa:$NAME]"
if ! qa_cc_driver "$NAME" "$QA_DIR"; then
  qa_record_fail "$NAME" "driver compile failed"; qa_summary; exit 1
fi
objs=""
for p in $PROD; do qa_prod_obj "$p" || { qa_record_fail "$NAME" "prod $p"; qa_summary; exit 1; }; objs="$objs $QA_BUILD/$p.o"; done
for s in $STUBS; do qa_stub_obj "$s" || { qa_record_fail "$NAME" "stub $s"; qa_summary; exit 1; }; objs="$objs $QA_BUILD/stub_$s.o"; done
BIN="$QA_BUILD/$NAME"
echo "  [link] $NAME" >&2
if ! $CXX $DRIVER_FLAGS $objs "$QA_BUILD/${NAME}.o" -o "$BIN" 2>"$QA_BUILD/${NAME}.link.err"; then
  echo "LINK FAILED:"; head -30 "$QA_BUILD/${NAME}.link.err"
  qa_record_fail "$NAME" "link failed see build/${NAME}.link.err"; qa_summary; exit 1
fi

LOG="$ART/fault_inject.log"
qa_run "$NAME" "$LOG" -- "$BIN"

qa_summary
[ "$QA_FAIL" -eq 0 ]

#!/bin/bash
# =============================================================================
# run_fault_inject.sh — v4.2 Batch D fault-injection harness (D4).
#
# Builds and runs tests/qa/fault_inject.cpp against the REAL production TUs
# (aatpe_recovery.cpp, aatped_utils.cpp) in plain (uninstrumented) mode and
# asserts the daemon degrades safely under injected faults:
#   FI-1  exec timeout fault   -> child killed within ~3s, non-zero status (no hang)
#   FI-2  corrupt .rec fault   -> bootload tolerates/truncates/garbage records,
#                                 still loads the good record beside them
#   FI-3  half-restore record  -> filename tid != embedded tid skipped (uninstall-safe)
#
# Link set mirrors the proven isolated postfreeze/v42_hotfix/build_d.sh d4 recipe:
#   prod  = aatpe_recovery aatped_utils
#   stubs = log_stub sprop_stub
# The driver TU self-supplies RUN_DIR/STATE_DIR + getOnlineCpus + the caps cgroup
# writers, so no utils_deps/recovery stubs are needed.
#
# Usage: bash tests/qa/run_fault_inject.sh
# =============================================================================
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export QA_MODE=plain; # shellcheck source=common.sh
source "$QA_DIR/common.sh"
# v4.4.9 A2: the writeFileAtomic fault-injection seams (g_fi_*) are guarded by
# AATPE_HOST_TEST. common.sh's case overwrites EXTRA_FLAGS, so append the define to
# the resolved prod+driver flag sets here (both prod obj and driver TU).
PROD_FLAGS="$PROD_FLAGS -DAATPE_HOST_TEST"
DRIVER_FLAGS="$DRIVER_FLAGS -DAATPE_HOST_TEST"

ART="$QA_DIR/artifacts"; mkdir -p "$ART"
NAME="fault_inject"
PROD="aatpe_recovery aatped_utils"
STUBS="log_stub sprop_stub"

echo "==> [qa:$NAME]"
# compile the driver strict (qa driver flags; not -Werror by design)
if ! qa_cc_driver "$NAME" "$QA_DIR"; then
  qa_record_fail "$NAME" "driver compile failed"; qa_summary; exit 1
fi
# build prod + stub objects, then link (inline; mirrors run_sanitizers.sh)
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

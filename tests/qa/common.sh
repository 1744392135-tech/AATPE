#!/bin/bash
# =============================================================================
# AATPE QA infrastructure — common.sh
#
# Shared variables + compile/link/counter helpers for the QA test matrix under
# tests/qa/. Mirrors the hermetic v94 approach: link the REAL production TUs and
# supply Android-only symbols from host stubs under tests/stubs/.
#
# Modes:
#   $QA_MODE selects the object-cache subdir + extra compile/link flags.
#   Known modes: asan ubsan tsan lsan coverage fuzz stress plain
#
# Design notes:
#   * Production .cpp files are compiled lenient (-w) — they are owned by the
#     daemon build. QA drivers / stress / fuzz mains are compiled strict.
#   * Object files are cached per-mode so ASan/TSan/coverage instrumented
#     objects never mix (sanitizer runtime + .gcno/.gcda are ABI-incompatible
#     across modes).
#   * This file is meant to be SOURCED, not executed directly.
# =============================================================================

# --- locate paths (works no matter which script sourced us) ------------------
_QA_COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$_QA_COMMON_DIR/../.." && pwd)"          # pkg_b root
STUB="$SRC/tests/stubs"
QA_DIR="$_QA_COMMON_DIR"
export SRC STUB QA_DIR

CXX="${CXX:-g++}"
QA_MODE="${QA_MODE:-plain}"
QA_BUILD="$QA_DIR/build/$QA_MODE"
mkdir -p "$QA_BUILD"

# Base flags (shared across modes). -D__clang_version__ neutralises the
# NDK-clang builtin that aatpe-cli.cpp references on host.
BASE_INCLUDES="-I$SRC -I$SRC/shared -I$STUB"
# AATPE_HOST_TEST: every HOST hermetic build defines this (mirrors the canonical
# tests/v94/build.sh TFLAGS/PFLAGS). It exposes the #ifdef AATPE_HOST_TEST test
# seams (set*ForTest / g_*TestOverride) to the drivers, and compiles out the
# production-only glue blocks that host stubs (scenewire/thermal_deps/...) replace.
PROD_FLAGS="-std=c++17 -O1 -w -fPIC -pthread -DAATPE_HOST_TEST -D__clang_version__=\"host-g++\""
DRIVER_FLAGS="-std=c++17 -Wall -Wextra -Wno-unused-parameter -fPIC -pthread -DAATPE_HOST_TEST -D__clang_version__=\"host-g++\""

# Per-mode extra flags.
case "$QA_MODE" in
  asan|ubsan)  EXTRA_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" ;;
  tsan)        EXTRA_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1" ;;
  lsan)        EXTRA_FLAGS="-fsanitize=address,leak -fno-omit-frame-pointer -g -O1" ;;
  coverage)    EXTRA_FLAGS="--coverage -g -O0" ;;
  fuzz)        EXTRA_FLAGS="-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1" ;;
  stress)      EXTRA_FLAGS="-O2 -g" ;;
  plain)       EXTRA_FLAGS="-O2 -g" ;;
  *) echo "common.sh: unknown QA_MODE='$QA_MODE' (falling back to plain)" >&2
               QA_MODE=plain; EXTRA_FLAGS="-O2 -g" ;;
esac

PROD_FLAGS="$PROD_FLAGS $EXTRA_FLAGS"
DRIVER_FLAGS="$DRIVER_FLAGS $EXTRA_FLAGS"

# --- result counters ---------------------------------------------------------
QA_PASS=0
QA_FAIL=0
QA_SKIP=0
declare -a QA_FAILURES=()
declare -a QA_SKIPPED=()
declare -A QA_EVIDENCE=()   # name -> one-line evidence

# Recompute compile flags + object dir for a new sanitizer/coverage mode WITHOUT
# resetting the result counters. Used by run_sanitizers.sh to run asan then tsan.
qa_set_mode() {
  local m="$1"; QA_MODE="$m"
  QA_BUILD="$QA_DIR/build/$QA_MODE"; mkdir -p "$QA_BUILD"
  case "$QA_MODE" in
    asan|ubsan)  EXTRA_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" ;;
    tsan)        EXTRA_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1" ;;
    lsan)        EXTRA_FLAGS="-fsanitize=address,leak -fno-omit-frame-pointer -g -O1" ;;
    coverage)    EXTRA_FLAGS="--coverage -g -O0" ;;
    fuzz)        EXTRA_FLAGS="-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1" ;;
    stress|plain) EXTRA_FLAGS="-O2 -g" ;;
    *) EXTRA_FLAGS="-O2 -g" ;;
  esac
  PROD_FLAGS="-std=c++17 -O1 -w -fPIC -pthread -DAATPE_HOST_TEST -D__clang_version__=\"host-g++\" $EXTRA_FLAGS"
  DRIVER_FLAGS="-std=c++17 -Wall -Wextra -Wno-unused-parameter -fPIC -pthread -DAATPE_HOST_TEST -D__clang_version__=\"host-g++\" $EXTRA_FLAGS"
}

qa_record_pass() { QA_PASS=$((QA_PASS+1)); QA_EVIDENCE["$1"]="${2:-PASS}"; }
qa_record_fail() { QA_FAIL=$((QA_FAIL+1)); QA_FAILURES+=("$1"); QA_EVIDENCE["$1"]="${2:-FAIL}"; }
qa_record_skip() { QA_SKIP=$((QA_SKIP+1)); QA_SKIPPED+=("$1"); QA_EVIDENCE["$1"]="${2:-SKIP}"; }

# --- compile helpers (cached per mode) ---------------------------------------
# qa_prod_obj <basename-no-ext>   ->  $QA_BUILD/<basename>.o
# Detects .cpp vs .c (pure-C cores like psi_decision / touch_uclamp /
# thermal_ff/thermal_ff), mirroring tests/v94/build.sh:107.
qa_prod_obj() {
  local o="$QA_BUILD/$1.o"
  [ -f "$o" ] && return 0
  local psrc=""
  [ -f "$SRC/$1.cpp" ] && psrc="$SRC/$1.cpp"
  [ -z "$psrc" ] && [ -f "$SRC/$1.c" ] && psrc="$SRC/$1.c"
  [ -z "$psrc" ] && { echo "PROD COMPILE FAILED: $1 (no .cpp/.c)"; return 1; }
  mkdir -p "$(dirname "$o")"
  case "$psrc" in
    *.c) echo "  [cc:prod-c] $1.c" >&2
         gcc -std=c11 -O1 -w -fPIC -pthread $BASE_INCLUDES -c "$psrc" -o "$o" -lm 2>"$QA_BUILD/$1.err" || {
           echo "PROD COMPILE FAILED: $1"; head -25 "$QA_BUILD/$1.err"; return 1; } ;;
    *) echo "  [cc:prod] $1.cpp" >&2
       $CXX $PROD_FLAGS $BASE_INCLUDES -c "$psrc" -o "$o" 2>"$QA_BUILD/$1.err" || {
         echo "PROD COMPILE FAILED: $1"; head -25 "$QA_BUILD/$1.err"; return 1; } ;;
  esac
}

# qa_stub_obj <basename-no-ext>   ->  $QA_BUILD/stub_<basename>.o
qa_stub_obj() {
  local o="$QA_BUILD/stub_$1.o"
  [ -f "$o" ] && return 0
  echo "  [cc:stub] $1.cpp" >&2
  $CXX $PROD_FLAGS -I$SRC -c "$STUB/$1.cpp" -o "$o" 2>"$QA_BUILD/stub_$1.err" || {
    echo "STUB COMPILE FAILED: $1"; head -25 "$QA_BUILD/stub_$1.err"; return 1; }
}

# qa_cc_driver <basename> <dir>    -> compile <dir>/<basename>.cpp strict -> .o
qa_cc_driver() {
  local name="$1" dir="$2"
  local o="$QA_BUILD/${name}.o"
  echo "  [cc:drv] $name.cpp" >&2
  $CXX $DRIVER_FLAGS -I$SRC -I$STUB -c "$dir/$name.cpp" -o "$o" 2>"$QA_BUILD/${name}.err" || {
    echo "DRIVER COMPILE FAILED: $name"; head -30 "$QA_BUILD/${name}.err"; return 1; }
}

# qa_link_bin <bin-name> <prod-names space-sep> <stub-names space-sep> <driver-.o>
qa_link_bin() {
  local bin="$QA_BUILD/$1"; shift
  local prod_list="$1"; shift
  local stub_list="$1"; shift
  local objs="" p s
  for p in $prod_list; do qa_prod_obj "$p" || return 1; objs="$objs $QA_BUILD/$p.o"; done
  for s in $stub_list; do qa_stub_obj "$s" || return 1; objs="$objs $QA_BUILD/stub_$s.o"; done
  echo "  [link] $1" >&2
  $CXX $DRIVER_FLAGS $objs "$@" -o "$bin" 2>"$QA_BUILD/$1.link.err" || {
    echo "LINK FAILED: $1"; head -30 "$QA_BUILD/$1.link.err"; return 1; }
  echo "$bin"
}

# qa_run <name> <logfile> -- cmd...   run cmd, capture to log, classify.
# Prints nothing; caller inspects $? . On nonzero exit, tail the log.
qa_run() {
  local name="$1" log="$2"; shift 2
  if [ "$1" = "--" ]; then shift; fi
  echo "  [run] $name"
  if "$@" >"$log" 2>&1; then
    qa_record_pass "$name" "exit=0 log=$(basename "$log")"
  else
    local rc=$?
    echo "  ---- failed log tail ($name) ----"
    tail -25 "$log"
    qa_record_fail "$name" "exit=$rc log=$(basename "$log")"
  fi
}

# qa_summary -- print the one-table summary.
qa_summary() {
  echo ""
  echo "==================================================================="
  echo "QA [$QA_MODE] results: PASS=$QA_PASS  FAIL=$QA_FAIL  SKIP=$QA_SKIP"
  if [ "$QA_FAIL" -gt 0 ]; then echo "FAILED targets: ${QA_FAILURES[*]}"; fi
  if [ "$QA_SKIP" -gt 0 ]; then echo "Skipped:        ${QA_SKIPPED[*]}"; fi
  echo "-------------------------------------------------------------------"
  printf "%-22s %-8s %s\n" "TARGET" "RESULT" "EVIDENCE"
  for k in "${!QA_EVIDENCE[@]}"; do printf "%-22s %-8s %s\n" "$k" "${QA_EVIDENCE[$k]%% *}" "${QA_EVIDENCE[$k]#* }"; done
  echo "==================================================================="
}

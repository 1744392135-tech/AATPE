#!/bin/bash
# Reproduce the F1.1/F1.4 v4.3.8 regression gate on host g++.
# Usage: bash tests/v94/run_baseline_dim.sh
# Builds the hermetic baseline-dim test against the real aatpe_ml TU and runs
# it under plain, ASan+UBSan, and TSan. Exits non-zero on any failure.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
STUB="$SRC/tests/stubs"
OUT="$HERE/build_baseline_dim"
mkdir -p "$OUT"
CXX="${CXX:-g++}"
INC="-I$SRC -I$STUB -I$SRC/shared -I$SRC/mech/include -D__clang_version__=host-g++"
TEST="$SRC/tests/v94/test_ml_baseline_dim.cpp"

fail=0
run_one() { # $1=flavor  $2=extra flags
  local flavor="$1"; local flags="$2"
  echo "=== build+run [$flavor] ==="
  $CXX -std=c++17 -O1 -g $flags -fPIC -pthread $INC -c "$SRC/aatpe_ml.cpp" -o "$OUT/ml_$flavor.o" || { echo "COMPILE ML FAIL $flavor"; fail=1; return; }
  $CXX -std=c++17 -O1 -g $flags -fPIC -pthread $INC -c "$STUB/ml_deps.cpp" -o "$OUT/deps_$flavor.o" || { echo "COMPILE DEPS FAIL $flavor"; fail=1; return; }
  $CXX -std=c++17 -O1 -g -Wall -Wextra -Werror $flags -fPIC -pthread $INC "$TEST" "$OUT/ml_$flavor.o" "$OUT/deps_$flavor.o" -o "$OUT/test_$flavor" || { echo "LINK FAIL $flavor"; fail=1; return; }
  if ! "$OUT/test_$flavor"; then echo "RUN FAIL $flavor"; fail=1; fi
}

run_one "plain"  ""
run_one "asan"   "-fsanitize=address,undefined"
run_one "tsan"   "-fsanitize=thread"

echo
if [ "$fail" -eq 0 ]; then echo "ALL FLAVORS GREEN"; else echo "FLAVOR FAILURES DETECTED"; fi
exit "$fail"

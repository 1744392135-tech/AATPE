#!/usr/bin/env bash
# Host regression for v4.3.6 touch per-thread uclamp wiring.
# Builds ONLY the pure decision core + controller (no production daemon deps)
# and runs under normal / ASan+UBSan / TSan.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

CCXX=${CXX:-g++}
COMMON="-std=c++17 -Wall -Wextra -Werror -O1 -g -I. -I$ROOT"
SRCS="touch_uclamp.c touch_uclamp_act.cpp tests/touch_act/test_touch_act.cpp"

build_and_run () {
    local mode="$1"; shift
    local out="$HERE/bin_$mode"
    mkdir -p "$HERE"
    echo "=== [$mode] building ==="
    $CCXX $COMMON "$@" $SRCS -o "$out" -lm
    echo "=== [$mode] running ==="
    "$out"
}

build_and_run normal
build_and_run asan -fsanitize=address,undefined -fno-omit-frame-pointer
build_and_run tsan  -fsanitize=thread -fno-omit-frame-pointer
echo "ALL SANITIZER RUNS PASSED"

#!/bin/bash
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SRC_DIR"

echo "=== integrity manifest layout self-check ==="

mapfile -t PATHS < <(
    awk '
        /kCriticalRelPaths\[\] *=/ {grab=1}
        grab && /^[[:space:]]*"[^"]+",?[[:space:]]*$/ {
            line=$0
            if (match(line, /"[^"]+"/)) print substr(line, RSTART+1, RLENGTH-2)
        }
        grab && /nullptr/ {grab=0}
    ' integrity.cpp
)
echo "extracted ${#PATHS[@]} critical rel paths"
[ "${#PATHS[@]}" -eq 17 ] || { echo "[FAIL] expected 17 critical paths, got ${#PATHS[@]}"; exit 1; }

bad=0
for x in "customize.sh" "system/bin/aatpe-temp-probe.sh"; do
    for p in "${PATHS[@]}"; do
        [ "$p" = "$x" ] && { echo "[FAIL] forbidden entry still in chain: $x"; bad=1; }
    done
done
[ "$bad" -eq 0 ] && echo "[ok] customize.sh and aatpe-temp-probe.sh are NOT in the chain"

missing=0
for p in "${PATHS[@]}"; do
    case "$p" in
        system/*) f="$SRC_DIR/$p" ;;
        *)        f="$SRC_DIR/$p" ;;
    esac
    if [ -f "$f" ]; then
        echo "[ok]   present: $p"
    else
        echo "[FAIL] chained but MISSING in layout: $p"
        missing=$((missing+1))
    fi
done
[ "$missing" -eq 0 ] || { echo "=== FAIL: $missing chained file(s) absent from install layout ==="; exit 1; }

echo "=== PASS: integrity manifest layout matches shipped files ==="

echo
echo "=== P3: trust-anchor dir permission enforcement ==="
perm_fail=0

need() {
    if grep -qE "$2" "$1"; then
        echo "[ok]   $1 enforces: $2"
    else
        echo "[FAIL] $1 MISSING enforcement: $2"
        perm_fail=1
    fi
}

need post-fs-data.sh 'chmod 0700 "\$PERSIST_DIR"'
need customize.sh   'chmod 0700 /data/adb/aatpe'
need service.sh     'chmod 0700 /data/adb/aatpe'

need customize.sh   'chmod 0600 "\$REANCHOR_MARKER"'
need service.sh     'chmod 0600 "\$REANCHOR_MARKER"'

if grep -qE 'O_(WRONLY|CREAT)[^)]*,\s*0600' integrity.cpp; then
    echo "[ok]   integrity.cpp writes manifest.sha256 with mode 0600"
else
    echo "[FAIL] integrity.cpp manifest write is not mode 0600"
    perm_fail=1
fi

[ "$perm_fail" -eq 0 ] && echo "=== PASS: trust-anchor dir/marker permission enforcement present ===" \
    || { echo "=== FAIL: permission enforcement regression ==="; exit 1; }

echo
echo "=== v6.0.0 B2: release layout hard gate ==="
layout_fail=0

_bpf="$SRC_DIR/system/etc/aatpe/ebpf/sched_events.bpf.o"
_bpf_ok=0
if [ -f "$_bpf" ] && [ -s "$_bpf" ]; then
    _magic=$(od -An -tx1 -N4 "$_bpf" 2>/dev/null | tr -d ' \n')
    if [ "$_magic" = "7f454c46" ]; then
        echo "[ok]   sched_events.bpf.o present, non-empty, ELF magic ($_magic)"
        _bpf_ok=1
    else
        echo "[FAIL] sched_events.bpf.o is not an ELF (magic=$_magic, want 7f454c46)"
    fi
else
    echo "[FAIL] sched_events.bpf.o MISSING or EMPTY"
fi
[ "$_bpf_ok" -eq 1 ] || layout_fail=1

pyc_hits="$(find "$SRC_DIR" \( -path '*/build*' -o -path '*/.git*' \) -prune -o \( -name '__pycache__' -o -name '*.pyc' \) -print 2>/dev/null | grep -v '/build/' || true)"
if [ -z "$pyc_hits" ]; then
    echo "[ok]   no __pycache__/*.pyc in module tree"
else
    echo "[FAIL] Python bytecode cache present in module tree:"
    echo "$pyc_hits" | sed 's/^/         /'
    layout_fail=1
fi

stray="$(find "$SRC_DIR" \
    \( -path '*/build*' -o -path '*/_install*' -o -path '*/.git*' \) -prune -o \
    \( -name '*.o' -o -name '*.d' -o -name 'aatpe_integration_test' \) -print 2>/dev/null \
    | grep -v -E '/ebpf/sched_events\.bpf\.o$' || true)"
if [ -z "$stray" ]; then
    echo "[ok]   no stray build artifacts (*.o/*.d/integration binary; ebpf/sched_events.bpf.o whitelisted)"
else
    echo "[FAIL] stray build artifacts present:"
    echo "$stray" | sed 's/^/         /'
    layout_fail=1
fi

[ "$layout_fail" -eq 0 ] && echo "=== PASS: release layout hard gate ===" \
    || { echo "=== FAIL: release layout hard gate ==="; exit 1; }

#!/bin/bash
# ROUND-002 (G10): pre-release integrity-manifest layout self-check.
#
# Long-lived guard against the v3.4.7 regression: kCriticalRelPaths in
# integrity.cpp must exactly match the files actually shipped in the module.
# Anything in the chain that is not present in the installed layout gets baked
# as a permanent "missing" sentinel -> runtime verify reports TAMPER forever and
# the fail-closed gate blocks eBPF. Conversely, shipped critical files that are
# missing from the chain would go unverified.
#
# Run AFTER `make install` (which materializes the ./system/ mirror). Fails the
# release (non-zero) on any mismatch.
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SRC_DIR"

echo "=== integrity manifest layout self-check ==="

# --- 1) extract the critical rel paths from integrity.cpp --------------------
# Only take quoted strings on lines that begin (after whitespace) with a quote —
# i.e. the real array initializers — so prose inside // comments is ignored.
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

# --- 2) forbid the two known-bad entries --------------------------------------
bad=0
for x in "customize.sh" "system/bin/aatpe-temp-probe.sh"; do
    for p in "${PATHS[@]}"; do
        [ "$p" = "$x" ] && { echo "[FAIL] forbidden entry still in chain: $x"; bad=1; }
    done
done
[ "$bad" -eq 0 ] && echo "[ok] customize.sh and aatpe-temp-probe.sh are NOT in the chain"

# --- 3) every chained path must exist in the installed layout -----------------
# Paths under system/ resolve against the ./system install mirror; everything
# else (root scripts, module.prop, webroot/) resolves against the module root.
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

# ============================================================================
# P3 hardening: persistent trust-anchor dir must be root-private.
# /data/adb/aatpe/ holds manifest.sha256, the re-anchor marker, install_id/boot_id
# and other trust anchors. We assert at the SOURCE level that every script that
# creates/uses it converges it to 0700 root:root and the marker to 0600. (At build
# time the dir does not exist yet, so we guard the enforcement lines, not a live
# mode.) A regression that drops a chmod line fails the release.
# ============================================================================
echo
echo "=== P3: trust-anchor dir permission enforcement ==="
perm_fail=0

need() { # $1 = file, $2 = grep -E pattern that MUST be present
    if grep -qE "$2" "$1"; then
        echo "[ok]   $1 enforces: $2"
    else
        echo "[FAIL] $1 MISSING enforcement: $2"
        perm_fail=1
    fi
}

# dir must be converged to 0700 in all three lifecycle scripts
need post-fs-data.sh 'chmod 0700 "\$PERSIST_DIR"'
need customize.sh   'chmod 0700 /data/adb/aatpe'
need service.sh     'chmod 0700 /data/adb/aatpe'

# marker must be 0600 on creation (customize) and converged (service)
need customize.sh   'chmod 0600 "\$REANCHOR_MARKER"'
need service.sh     'chmod 0600 "\$REANCHOR_MARKER"'

# manifest file itself must stay 0600 (C++ side); assert the C++ open mode.
if grep -qE 'O_(WRONLY|CREAT)[^)]*,\s*0600' integrity.cpp; then
    echo "[ok]   integrity.cpp writes manifest.sha256 with mode 0600"
else
    echo "[FAIL] integrity.cpp manifest write is not mode 0600"
    perm_fail=1
fi

[ "$perm_fail" -eq 0 ] && echo "=== PASS: trust-anchor dir/marker permission enforcement present ===" \
    || { echo "=== FAIL: permission enforcement regression ==="; exit 1; }

# ============================================================================
# v6.0.0 RT-B B2: release-layout hard gate.
#   (a) the precompiled eBPF object MUST ship (system/etc/aatpe/ebpf/sched_events.bpf.o)
#   (b) zero Python bytecode cache (__pycache__/*.pyc) anywhere in the module
#   (c) zero stray non-ELF build artifacts (old .o, logs, integration test binary)
# Missing/stray -> FAIL.
# ============================================================================
echo
echo "=== v6.0.0 B2: release layout hard gate ==="
layout_fail=0

# (a) shipped eBPF object must exist, be NON-EMPTY, and be an ELF (BPF object is
# still an ELF: magic bytes 0x7f 'E' 'L' 'F'). RTC-03: a 0-byte placeholder or a
# text file would otherwise pass "file exists".
_bpf="$SRC_DIR/system/etc/aatpe/ebpf/sched_events.bpf.o"
_bpf_ok=0
if [ -f "$_bpf" ] && [ -s "$_bpf" ]; then
    # read first 4 bytes; BPF/ELF magic = 7f 45 4c 46.
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

# (b) no __pycache__ / .pyc anywhere under the module tree (excluding this script's own logs)
pyc_hits="$(find "$SRC_DIR" \( -path '*/build*' -o -path '*/.git*' \) -prune -o \( -name '__pycache__' -o -name '*.pyc' \) -print 2>/dev/null | grep -v '/build/' || true)"
if [ -z "$pyc_hits" ]; then
    echo "[ok]   no __pycache__/*.pyc in module tree"
else
    echo "[FAIL] Python bytecode cache present in module tree:"
    echo "$pyc_hits" | sed 's/^/         /'
    layout_fail=1
fi

# (c) no stray build artifacts. RTC-04: the old `find -maxdepth 2 -name '*.o'`
# false-positived on the LEGITIMATE ebpf/sched_events.bpf.o (depth 2, appears on
# dev machines after `make ebpf-prepare`). Prune build/_install/.git dirs, and
# whitelist the expected ebpf/sched_events.bpf.o. Cover all depths (not just 2).
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

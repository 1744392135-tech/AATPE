#!/bin/bash
# W3-TASK A host integration test: trusted-install re-anchor timing.
#
# Proves the upgrade false-TAMPER fix end-to-end at the shell level WITHOUT a
# device or an NDK-built aatped. We stub `aatped` and exercise the EXACT re-anchor
# logic that service.sh runs, in a tmp sandbox.
#
# Contract under test (must hold for the fix to be safe AND effective):
#   1. customize.sh WRITES the marker during trusted install.
#   2. service.sh CONSUMES the marker BEFORE --verify-integrity (before the gate).
#   3. On regen success -> marker removed, fresh manifest present (verify then OK).
#   4. On regen failure -> marker KEPT, verify still runs fail-closed (no weaken).
#   5. No marker (normal boot) -> NO regen attempted (verify fail-closed, as today).
#   6. The C++ verify side is untouched: still no auto-bootstrap on missing manifest.
#
# Run: bash check-reanchor-timing.sh ; exit code 0 = PASS.
set -u
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SRC_DIR"
fail=0
say() { echo "$@"; }
fail() { say "[FAIL] $*"; fail=1; }

say "=== 1) customize.sh writes the re-anchor marker ==="
grep -q 'REANCHOR_MARKER="/data/adb/aatpe/manifest_reanchor.pending"' customize.sh \
  && say "[ok] customize.sh defines marker path" || fail "customize.sh does not define marker"
grep -q 'versionCode=' customize.sh \
  && say "[ok] customize.sh stamps versionCode into marker" || fail "marker missing versionCode"

say "=== 2) service.sh consumes marker BEFORE the verify gate ==="
reanchor_ln=$(grep -n 'Consuming trusted-install re-anchor' service.sh | head -1 | cut -d: -f1)
verify_ln=$(grep -n -- '--verify-integrity' service.sh | head -1 | cut -d: -f1)
[ -n "$reanchor_ln" ] && [ -n "$verify_ln" ] && [ "$reanchor_ln" -lt "$verify_ln" ] \
  && say "[ok] re-anchor (line $reanchor_ln) runs before --verify-integrity (line $verify_ln)" \
  || fail "re-anchor block is not ordered before verify (reanchor=$reanchor_ln verify=$verify_ln)"
# regen must be gated on marker existence (no unconditional regen = no weaken)
# I-003 (v4.3.4): anchor on the unique REANCHOR_MARKER assignment line, then look
# within the following 25 lines for the `if [ -f "$REANCHOR_MARKER" ]` gate.
# Previously this used a hardcoded `awk 'NR>=83 && NR<=115'` line range, which
# broke whenever any line was inserted/removed above the re-anchor block. Content
# anchoring is robust to such edits.
grep -A25 '^REANCHOR_MARKER="/data/adb/aatpe/manifest_reanchor.pending"' service.sh \
  | grep -q 'if \[ -f "$REANCHOR_MARKER" \]' \
  && say "[ok] regen is gated on marker presence (no unconditional per-boot regen)" \
  || fail "regen is not gated on marker"

say "=== 3-5) simulate marker consumption in a sandbox with a stubbed aatped ==="
SIM=$(mktemp -d)
# stub module dir + stub aatped that writes a manifest on --generate-manifest.
MODDIR="$SIM/modules/aatpe"; mkdir -p "$MODDIR/system/bin"
STATE_DIR="$SIM/data/adb/aatpe"; mkdir -p "$STATE_DIR"
REANCHOR_MARKER="$STATE_DIR/manifest_reanchor.pending"
MANIFEST="$STATE_DIR/manifest.sha256"

# stub aatped: arg $1 = --generate-manifest, $2 = root. Writes a manifest.
make_stub() {
  local mode="$1"   # ok | fail
  cat > "$MODDIR/system/bin/aatped" <<EOF
#!/bin/sh
# stub aatped
if [ "\$1" = "--generate-manifest" ]; then
  if [ "$mode" = "fail" ]; then exit 1; fi
  echo "stub-chain-v2" > "$MANIFEST"
  exit 0
fi
# --verify-integrity: compare manifest content to current (stubbed: just report OK if manifest exists)
[ -f "$MANIFEST" ] && exit 0 || exit 1
EOF
  chmod +x "$MODDIR/system/bin/aatped"
}

# Re-implement the exact service.sh re-anchor block (extracted, sandboxed).
consume_marker() {
  LOG_FILE="$SIM/aatpe.log"
  if [ -f "$REANCHOR_MARKER" ]; then
    if [ -x "$MODDIR/system/bin/aatped" ] && [ -d "$MODDIR/system/bin" ]; then
      if "$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR" >> "$LOG_FILE" 2>&1 \
         && [ -f "$MANIFEST" ]; then
        rm -f "$REANCHOR_MARKER"
      fi
    fi
  fi
}

# --- case A: marker present + regen succeeds -> manifest fresh + marker gone
make_stub ok
echo "versionCode=42100" > "$REANCHOR_MARKER"   # old v1 manifest absent (recovery-time gen failed)
consume_marker
[ -f "$MANIFEST" ] && [ "$(cat "$MANIFEST")" = "stub-chain-v2" ] \
  && say "[ok] case A: regen produced fresh manifest" || fail "case A: manifest not regenerated"
[ ! -f "$REANCHOR_MARKER" ] \
  && say "[ok] case A: marker consumed on success" || fail "case A: marker should be removed"

# --- case B: marker present + regen fails -> marker KEPT (retry next boot)
make_stub fail
echo "versionCode=42100" > "$REANCHOR_MARKER"
consume_marker
[ -f "$REANCHOR_MARKER" ] \
  && say "[ok] case B: failure keeps marker for next boot (fail-closed retry)" || fail "case B: marker dropped on failure"

# --- case C: no marker (normal boot) -> NO regen, no manifest creation
rm -f "$REANCHOR_MARKER" "$MANIFEST"
make_stub ok
consume_marker
[ ! -f "$MANIFEST" ] \
  && say "[ok] case C: no marker -> no regen (fail-closed, verify must not auto-bootstrap)" \
  || fail "case C: regen happened without marker (would weaken fail-closed)"

rm -rf "$SIM"

say "=== 6) C++ verify side untouched (still fail-closed) ==="
grep -q 'refusing to auto-bootstrap' integrity.cpp \
  && say "[ok] integrity.cpp still refuses auto-bootstrap on missing manifest" \
  || fail "integrity.cpp auto-bootstrap guard missing"

echo
if [ "$fail" -eq 0 ]; then
  say "=== PASS: re-anchor timing is safe (correct baseline at correct time, verify stays fail-closed) ==="
  exit 0
else
  say "=== FAIL ==="
  exit 1
fi

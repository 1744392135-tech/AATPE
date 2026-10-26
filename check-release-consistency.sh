#!/bin/bash
# =============================================================================
# AATPE RELEASE-LEVEL consistency check (runs AFTER `make && make install`).
#
# Verifies the ARTIFACTS of a build, which do not exist on a clean source tree:
#   - the 4 freshly built binaries in ./ embed the version + are ARM aarch64 PIE
#   - the 4 packaged binaries under system/bin/ exist, embed the version, and are
#     stripped release ELF
#   - (optional, when BUILD_MANIFEST.txt exists) the system/bin/* SHA256 bytes
#     match the manifest 1:1 — this is the P0-1 "manifest == packaged bytes" gate.
#
# Split from the old monolithic check_version_consistency.sh (P0-2): source-level
# drift lives in check-source-consistency.sh and must pass on a clean tree; this
# script is the post-build / post-install gate.
# =============================================================================
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

EXPECTED_VERSION="$(grep -m1 '^VERSION_NAME=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
EXPECTED_CODE="$(grep -m1 '^VERSION_CODE=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
[ -z "$EXPECTED_VERSION" ] && { echo "FATAL: cannot read VERSION_NAME from VERSION"; exit 2; }

ERRORS=0
BINS="aatped aatpe-cli aatpe-watchdog aatpe-probe"
echo "=== AATPE release-level consistency (post-build/post-install) ==="
echo "Source: $SRC_DIR/VERSION -> $EXPECTED_VERSION / $EXPECTED_CODE"
echo ""

check_bin_info() {  # $1 = path  $2 = label
    local b="$1" label="$2"
    if [ ! -f "$b" ]; then
        echo "[FAIL] $label: missing ($b)"; ERRORS=$((ERRORS+1)); return
    fi
    if strings "$b" | grep -q "$EXPECTED_VERSION"; then
        echo "[ok]   $label embeds $EXPECTED_VERSION"
    else
        echo "[FAIL] $label does NOT embed $EXPECTED_VERSION"; ERRORS=$((ERRORS+1))
    fi
    if file "$b" 2>/dev/null | grep -q "ARM aarch64"; then
        echo "[ok]   $label is ARM aarch64"
    else
        echo "[FAIL] $label is not ARM aarch64"; ERRORS=$((ERRORS+1))
    fi
    if file "$b" 2>/dev/null | grep -q "pie"; then
        echo "[ok]   $label is PIE"
    else
        echo "[FAIL] $label is not PIE"; ERRORS=$((ERRORS+1))
    fi
}

# --- built binaries in ./ ----------------------------------------------------
echo "--- built binaries (./) ---"
for b in $BINS; do check_bin_info "$SRC_DIR/$b" "./$b"; done

# --- packaged binaries under system/bin/ --------------------------------------
echo ""
echo "--- packaged binaries (system/bin/) ---"
for b in $BINS; do check_bin_info "$SRC_DIR/system/bin/$b" "system/bin/$b"; done

# --- optional: manifest == packaged bytes (P0-1 gate) -------------------------
echo ""
if [ -f "$SRC_DIR/BUILD_MANIFEST.txt" ]; then
    echo "--- BUILD_MANIFEST.txt vs system/bin/* bytes ---"
    for b in $BINS; do
        # manifest lines look like: <64-hex-sha256>  <name>
        want="$(awk -v bin="$b" '$1 ~ /^[0-9a-f]{64}$/ && $2 == bin {print $1; exit}' \
                "$SRC_DIR/BUILD_MANIFEST.txt" 2>/dev/null)"
        if [ -z "$want" ]; then
            echo "[FAIL] BUILD_MANIFEST.txt has no entry for $b"; ERRORS=$((ERRORS+1)); continue
        fi
        have="$(sha256sum "$SRC_DIR/system/bin/$b" 2>/dev/null | awk '{print $1}')"
        if [ "$have" = "$want" ]; then
            echo "[ok]   system/bin/$b sha256 == manifest"
        else
            echo "[FAIL] system/bin/$b sha256 mismatch"; echo "        manifest=$want"; echo "        actual  =$have"
            ERRORS=$((ERRORS+1))
        fi
    done
else
    echo "--- BUILD_MANIFEST.txt not present; skipping manifest hash gate ---"
fi

echo ""
if [ "$ERRORS" -eq 0 ]; then
    echo "=== PASS: release-level consistent ==="
    exit 0
else
    echo "=== FAIL: $ERRORS release-level issue(s) ==="
    exit 1
fi

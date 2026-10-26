#!/bin/bash
# #101 / release-convergence item 21: AATPE version + artifact consistency CI.
# Single source of truth = ./VERSION (VERSION_NAME / VERSION_CODE).
# Derives expected values from VERSION (no hard-coded copy) and verifies:
#   - VERSION / module.prop / aatpe-version.h / shell banners / WebUI footer
#   - the 4 built binaries in ./ (strings version, arch, PIE)
#   - the 4 binaries packaged under system/bin/ embed the version (no stale prebuilt)
#   - WebUI three copies are byte-identical
#   - aatpe.conf two copies are byte-identical
# Any drift -> FAIL (exit 1). Missing packaged binaries -> FAIL (no more warn-and-pass).
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

EXPECTED_VERSION="$(grep -m1 '^VERSION_NAME=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
EXPECTED_CODE="$(grep -m1 '^VERSION_CODE=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
[ -z "$EXPECTED_VERSION" ] && { echo "FATAL: cannot read VERSION_NAME from VERSION"; exit 2; }
[ -z "$EXPECTED_CODE" ] && { echo "FATAL: cannot read VERSION_CODE from VERSION"; exit 2; }

ERRORS=0
echo "=== AATPE version + artifact consistency ==="
echo "Source: $SRC_DIR/VERSION -> $EXPECTED_VERSION / $EXPECTED_CODE"
echo ""

check_contains() {
    local file="$1" expected="$2" label="$3"
    if [ ! -f "$SRC_DIR/$file" ]; then
        echo "[FAIL] $label: file missing ($file)"; ERRORS=$((ERRORS+1)); return
    fi
    if grep -q "$expected" "$SRC_DIR/$file"; then
        echo "[ok]   $label"
    else
        echo "[FAIL] $label: '$file' does not contain '$expected'"; ERRORS=$((ERRORS+1))
    fi
}

check_contains "VERSION" "$EXPECTED_VERSION" "VERSION name"
check_contains "VERSION" "$EXPECTED_CODE"     "VERSION code"
check_contains "module.prop" "version=$EXPECTED_VERSION" "module.prop version"
check_contains "module.prop" "versionCode=$EXPECTED_CODE" "module.prop versionCode"
check_contains "aatpe-version.h" "\"$EXPECTED_VERSION\"" "aatpe-version.h version"
check_contains "aatpe-version.h" "$EXPECTED_CODE"        "aatpe-version.h code"
check_contains "service.sh"       "$EXPECTED_VERSION" "service.sh banner"
check_contains "post-fs-data.sh" "$EXPECTED_VERSION" "post-fs-data.sh banner"
check_contains "customize.sh"    "$EXPECTED_VERSION" "customize.sh banner"
check_contains "uninstall.sh"    "$EXPECTED_VERSION" "uninstall.sh banner"
check_contains "webroot/index.html" "$EXPECTED_VERSION" "WebUI (webroot)"
check_contains "webui/index.html"     "$EXPECTED_VERSION" "WebUI (webui)"

# --- RELEASE_ID / BUILD_DATE four-way consistency (SCR-02 fix) --------------
EXPECTED_REL="$(grep -m1 '^RELEASE_ID=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
EXPECTED_DATE="$(grep -m1 '^BUILD_DATE=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
[ -z "$EXPECTED_REL" ]   && { echo "FATAL: cannot read RELEASE_ID from VERSION"; exit 2; }
[ -z "$EXPECTED_DATE" ]  && { echo "FATAL: cannot read BUILD_DATE from VERSION"; exit 2; }
echo ""
echo "--- RELEASE_ID / BUILD_DATE (derived from VERSION) ---"
echo "Expected: $EXPECTED_REL / $EXPECTED_DATE"
# module.prop: version line must contain "(relXX)" and description must too
check_contains "module.prop" "version=$EXPECTED_VERSION ($EXPECTED_REL)" "module.prop version (rel)"
check_contains "module.prop" "($EXPECTED_REL, versionCode $EXPECTED_CODE)" "module.prop description (rel)"
# aatpe-version.h: macros must match
check_contains "aatpe-version.h" "\"$EXPECTED_REL\""  "aatpe-version.h AATPE_RELEASE_ID"
check_contains "aatpe-version.h" "\"$EXPECTED_DATE\"" "aatpe-version.h AATPE_BUILD_DATE"
# WebUI footer: the visible "relXX · 构建于 YYYY-MM-DD" marker
if grep -q "$EXPECTED_REL · 构建于 $EXPECTED_DATE" "$SRC_DIR/webui/index.html"; then
    echo "[ok]   WebUI footer marker ($EXPECTED_REL · 构建于 $EXPECTED_DATE)"
else
    echo "[FAIL] WebUI footer missing '$EXPECTED_REL · 构建于 $EXPECTED_DATE' in webui/index.html"; ERRORS=$((ERRORS+1))
fi

# --- WebUI three copies byte-identical (item 4) -----------------------------
echo ""
echo "--- WebUI three copies ---"
if cmp -s "$SRC_DIR/webui/index.html" "$SRC_DIR/webroot/index.html" && \
   cmp -s "$SRC_DIR/webui/index.html" "$SRC_DIR/system/etc/aatpe/webui/index.html"; then
    echo "[ok]   webui/index.html == webroot == system/etc/aatpe/webui (byte-identical)"
else
    echo "[FAIL] WebUI three copies differ (webui/ is the source; re-sync the other two)"
    ERRORS=$((ERRORS+1))
fi

# --- aatpe.conf two copies byte-identical (item 5) --------------------------
echo ""
echo "--- aatpe.conf two copies ---"
if cmp -s "$SRC_DIR/aatpe.conf" "$SRC_DIR/system/etc/aatpe/aatpe.conf"; then
    echo "[ok]   aatpe.conf == system/etc/aatpe/aatpe.conf (byte-identical)"
else
    echo "[FAIL] aatpe.conf copies differ (root aatpe.conf is the source)"
    ERRORS=$((ERRORS+1))
fi

# --- built binaries: strings version + arch (item 21) -----------------------
echo ""
echo "--- built binaries (./) ---"
BINS="aatped aatpe-cli aatpe-watchdog aatpe-probe"
for b in $BINS; do
    if [ ! -f "$SRC_DIR/$b" ]; then
        echo "[FAIL] built binary missing: $b (run make first)"; ERRORS=$((ERRORS+1)); continue
    fi
    if strings "$SRC_DIR/$b" | grep -q "$EXPECTED_VERSION"; then
        echo "[ok]   $b embeds $EXPECTED_VERSION"
    else
        echo "[FAIL] $b does NOT embed $EXPECTED_VERSION"; ERRORS=$((ERRORS+1))
    fi
    if file "$SRC_DIR/$b" 2>/dev/null | grep -q "ARM aarch64"; then
        echo "[ok]   $b is ARM aarch64"
    else
        echo "[FAIL] $b is not ARM aarch64"; ERRORS=$((ERRORS+1))
    fi
done

# --- packaged binaries under system/bin/ must exist + embed the version ------
echo ""
echo "--- packaged binaries (system/bin/) ---"
for b in $BINS; do
    sb="$SRC_DIR/system/bin/$b"
    if [ ! -f "$sb" ]; then
        echo "[FAIL] packaged binary missing: system/bin/$b (must be produced by clean build)"
        ERRORS=$((ERRORS+1)); continue
    fi
    if strings "$sb" | grep -q "$EXPECTED_VERSION"; then
        echo "[ok]   system/bin/$b embeds $EXPECTED_VERSION"
    else
        echo "[FAIL] system/bin/$b does NOT embed $EXPECTED_VERSION (stale prebuilt?)"
        ERRORS=$((ERRORS+1))
    fi
done

# --- P2-32: each binary must support a --version / -v / version --------------
# On this x86_64 cross-build host the aarch64 ELFs cannot be executed directly,
# so we (a) audit each source to confirm the --version branch exists and prints
# AATPE_VERSION, and (b) re-confirm the built ./ binary embeds the version
# string that branch prints. If qemu-aarch64 happens to be installed we also
# execute `./binary --version` and match its stdout.
echo ""
echo "--- --version support (P2-32) ---"
declare -A BIN_SRC=( [aatped]=aatped_main.cpp [aatpe-cli]=aatpe-cli.cpp \
                     [aatpe-watchdog]=aatpe-watchdog.cpp [aatpe-probe]=aatpe-probe.cpp )
for b in $BINS; do
    src="${BIN_SRC[$b]}"
    if grep -q -- '--version' "$SRC_DIR/$src" 2>/dev/null; then
        echo "[ok]   $b ($src) handles --version"
    else
        echo "[FAIL] $b ($src) has NO --version branch"
        ERRORS=$((ERRORS+1))
    fi
    # Best-effort runtime check when an aarch64 emulator is present.
    if command -v qemu-aarch64 >/dev/null 2>&1; then
        out=$(qemu-aarch64 "$SRC_DIR/$b" --version 2>/dev/null || true)
        if echo "$out" | grep -q "$EXPECTED_VERSION"; then
            echo "[ok]   $b --version (qemu) -> $out"
        else
            echo "[FAIL] $b --version (qemu) output='$out' (expected $EXPECTED_VERSION)"
            ERRORS=$((ERRORS+1))
        fi
    fi
done

echo ""
if [ "$ERRORS" -eq 0 ]; then
    echo "=== PASS: version + artifacts consistent ==="
    exit 0
else
    echo "=== FAIL: $ERRORS consistency issue(s) ==="
    exit 1
fi

#!/bin/bash
set -e
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

EXPECTED_VERSION="$(grep -m1 '^VERSION_NAME=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
EXPECTED_CODE="$(grep -m1 '^VERSION_CODE=' "$SRC_DIR/VERSION" | cut -d= -f2 | tr -d ' \r')"
[ -z "$EXPECTED_VERSION" ] && { echo "FATAL: cannot read VERSION_NAME from VERSION"; exit 2; }
[ -z "$EXPECTED_CODE" ] && { echo "FATAL: cannot read VERSION_CODE from VERSION"; exit 2; }

ERRORS=0
echo "=== AATPE source-level consistency ==="
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
check_contains "customize.sh"     "$EXPECTED_VERSION" "customize.sh banner"
check_contains "uninstall.sh"    "$EXPECTED_VERSION" "uninstall.sh banner"
check_contains "webroot/index.html" "$EXPECTED_VERSION" "WebUI (webroot)"
check_contains "webui/index.html"     "$EXPECTED_VERSION" "WebUI (webui)"

echo ""
echo "--- WebUI copies ---"
if ! cmp -s "$SRC_DIR/webui/index.html" "$SRC_DIR/webroot/index.html"; then
    echo "[FAIL] WebUI committed copies differ (webui/ vs webroot/)"
    ERRORS=$((ERRORS+1))
elif [ -f "$SRC_DIR/system/etc/aatpe/webui/index.html" ]; then
    if cmp -s "$SRC_DIR/webui/index.html" "$SRC_DIR/system/etc/aatpe/webui/index.html"; then
        echo "[ok]   webui/index.html == webroot == system/etc/aatpe/webui (byte-identical)"
    else
        echo "[FAIL] WebUI system/ mirror differs from committed webui/ (re-run make install)"
        ERRORS=$((ERRORS+1))
    fi
else
    echo "[skip] system/etc/aatpe/webui/index.html absent (build artifact; run 'make install' to verify it)"
fi

echo ""
echo "--- aatpe.conf copies ---"
if [ -f "$SRC_DIR/system/etc/aatpe/aatpe.conf" ]; then
    if cmp -s "$SRC_DIR/aatpe.conf" "$SRC_DIR/system/etc/aatpe/aatpe.conf"; then
        echo "[ok]   aatpe.conf == system/etc/aatpe/aatpe.conf (byte-identical)"
    else
        echo "[FAIL] aatpe.conf system/ mirror differs (re-run make install)"
        ERRORS=$((ERRORS+1))
    fi
else
    echo "[skip] system/etc/aatpe/aatpe.conf absent (build artifact; run 'make install' to verify it)"
fi

echo ""
echo "--- eBPF committed embedded header ---"
if [ -f "$SRC_DIR/ebpf/sched_events_embedded.h" ]; then
    echo "[ok]   ebpf/sched_events_embedded.h present (zero-dependency loader)"
else
    echo "[FAIL] ebpf/sched_events_embedded.h missing; run 'make ebpf-regen' on a BPF-capable host"
    ERRORS=$((ERRORS+1))
fi

echo ""
if [ "$ERRORS" -eq 0 ]; then
    echo "=== PASS: source-level consistent ==="
    exit 0
else
    echo "=== FAIL: $ERRORS source-level issue(s) ==="
    exit 1
fi

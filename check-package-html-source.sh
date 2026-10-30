#!/usr/bin/env bash
set -u
ZIP="${1:-}"; SRC="${2:-.}"
[ -f "$ZIP" ] || { echo "usage: $0 <module.zip> [source-root]" >&2; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
unzip -q "$ZIP" 'webroot/index.html' 'webui/index.html' \
     'system/etc/aatpe/webui/index.html' -d "$T" || { echo "[FAIL] cannot extract html from zip" >&2; exit 1; }
FAIL=0
check() {
  zp="$1"; sp="$2"
  [ -f "$T/$zp" ] || { echo "[FAIL] zip missing $zp"; FAIL=1; return; }
  [ -f "$SRC/$sp" ] || { echo "[FAIL] source missing $sp"; FAIL=1; return; }
  a=$(sha256sum "$T/$zp"  | cut -d' ' -f1)
  b=$(sha256sum "$SRC/$sp" | cut -d' ' -f1)
  if [ "$a" = "$b" ]; then echo "[ok] $zp == source $sp ($a)";
  else echo "[FAIL] $zp=$a != source $sp=$b"; FAIL=1; fi
}
check webroot/index.html webroot/index.html
check webui/index.html webui/index.html
check system/etc/aatpe/webui/index.html webui/index.html
if [ "$FAIL" -eq 0 ]; then echo "package-html-source PASS"; exit 0; fi
echo "package-html-source FAIL" >&2; exit 1

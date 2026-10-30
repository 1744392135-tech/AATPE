#!/system/bin/sh
set -e
cd "$(dirname "$0")"
F1=webroot/index.html
F2=webui/index.html
F3=_install/system/etc/aatpe/webui/index.html
for f in "$F1" "$F2" "$F3"; do
  [ -f "$f" ] || { echo "HTML-sync FAIL: missing $f" >&2; exit 1; }
done
UNIQ=$(sha256sum "$F1" "$F2" "$F3" | awk '{print $1}' | sort -u | wc -l)
echo "HTML 3-copy sha256 set count: $UNIQ"
sha256sum "$F1" "$F2" "$F3"
if [ "$UNIQ" -ne 1 ]; then
  echo "HTML-sync FAIL: three copies diverge" >&2
  exit 1
fi
echo "HTML-sync PASS"

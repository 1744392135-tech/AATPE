#!/system/bin/sh
# ROUND-014 G10: HTML three-copy sha256 sync gate.
# The WebUI index.html exists in three copies that must stay byte-identical:
#   webroot/index.html                                   (KSU webroot source)
#   webui/index.html                                      (module webui source)
#   _install/system/etc/aatpe/webui/index.html           (installed mirror)
# If the copies drift, the on-device panel silently serves a stale UI.
# Exit non-zero (release fail) when the sha256 set is not exactly one unique value.
# I-007 (v4.3.4): md5sum -> sha256sum to match the rest of the project's
# integrity tooling (check-release-consistency.sh, release_ci.sh, BUILD_MANIFEST).
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

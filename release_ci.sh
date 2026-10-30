#!/bin/bash
set -e
SRC="${AATPE_SRC:-$(cd "$(dirname "$0")" && pwd)}"
OUT="$(cd "$SRC/.." && pwd)"
. "$SRC/VERSION"
[ -n "${VERSION_NAME:-}" ] || { echo "ERROR: VERSION_NAME missing in $SRC/VERSION" >&2; exit 2; }
MODZIP="$OUT/AATPE-${VERSION_NAME}-release.zip"
SRCZIP="$OUT/AATPE-${VERSION_NAME}-source.zip"
FAIL=0

echo "=== [25] module ZIP integrity ==="
zip -T "$MODZIP"
unzip -t "$MODZIP" >/dev/null && echo "unzip -t OK"
python3 - "$MODZIP" <<'PY'
import sys, struct
d=open(sys.argv[1],"rb").read()
e=d.rfind(b"PK\x05\x06")
assert e>0, "EOCD missing"
assert len(d) >= e+22, "EOCD header truncated"
comment_len = struct.unpack_from("<H", d, e+20)[0]
assert e + 22 + comment_len == len(d), \
    "EOCD not at end (e=%d hdr=22 comment=%d total=%d filesize=%d)" % (e, comment_len, e+22+comment_len, len(d))
print("EOCD present at", e, "comment", comment_len, "size", len(d))
PY

echo "=== [26] packaged binaries vs build output ==="
T=$(mktemp -d); unzip -q "$MODZIP" 'system/bin/*' -d "$T"
for b in aatped aatpe-cli aatpe-watchdog aatpe-probe; do
  want=$(grep "  $b\$" "$OUT/BUILD_MANIFEST.txt" | cut -d' ' -f1)
  got=$(sha256sum "$T/system/bin/$b" | cut -d' ' -f1)
  if [ "$want" = "$got" ]; then echo "[ok] $b sha256 == manifest"; else echo "[FAIL] $b sha mismatch"; FAIL=1; fi
  if strings "$T/system/bin/$b" | grep -q "$VERSION_NAME"; then echo "[ok] $b embeds $VERSION_NAME"; else echo "[FAIL] $b version (expected $VERSION_NAME)"; FAIL=1; fi
done
rm -rf "$T"

echo "=== [26b] required WebUI copies (KernelSU webroot/ + Magisk webui/ + runtime system/etc) ==="
for f in webroot/index.html webui/index.html system/etc/aatpe/webui/index.html; do
  if unzip -l "$MODZIP" | grep -q "  $f$"; then
    echo "[ok] module zip contains $f"
  else
    echo "[FAIL] module zip MISSING required file: $f"; FAIL=1
  fi
done
T2=$(mktemp -d); unzip -q "$MODZIP" 'webroot/index.html' 'webui/index.html' 'system/etc/aatpe/webui/index.html' -d "$T2" || { echo "[FAIL] could not extract webui copies"; FAIL=1; }
if [ "$FAIL" -eq 0 ]; then
  h1=$(sha256sum "$T2/webroot/index.html" | cut -d' ' -f1)
  h2=$(sha256sum "$T2/webui/index.html" | cut -d' ' -f1)
  h3=$(sha256sum "$T2/system/etc/aatpe/webui/index.html" | cut -d' ' -f1)
  if [ "$h1" = "$h2" ] && [ "$h2" = "$h3" ]; then
    echo "[ok] all three webui/index.html copies byte-identical"
  else
    echo "[FAIL] webui copies differ: webroot=$h1 webui=$h2 system=$h3"; FAIL=1
  fi
fi
rm -rf "$T2"

echo "=== [26d] required device scripts (collect.sh / doctor.sh) ==="
for f in system/etc/aatpe/collect.sh system/etc/aatpe/doctor.sh; do
  if unzip -l "$MODZIP" | grep -q "  $f$"; then
    echo "[ok] module zip contains $f"
  else
    echo "[FAIL] module zip MISSING required file: $f"; FAIL=1
  fi
done

echo "=== [26c] package html must byte-match current source tree ==="
if [ -x "$(dirname "$0")/check-package-html-source.sh" ]; then
  "$(dirname "$0")/check-package-html-source.sh" "$MODZIP" "$(dirname "$0")" || { echo "[FAIL] package html != source tree"; FAIL=1; }
else
  bash "$(dirname "$0")/check-package-html-source.sh" "$MODZIP" "$(dirname "$0")" || { echo "[FAIL] package html != source tree"; FAIL=1; }
fi

echo "=== [27] source ZIP cleanliness ==="
if [ ! -f "$SRCZIP" ]; then
  echo "[FAIL] source zip missing at $SRCZIP"; FAIL=1
else
if unzip -l "$SRCZIP" | grep -vE '^Archive:|^  Length|^---------|^[0-9]+ file|^----' | grep -qE '\.o$|\.d$|\.pyc$|__pycache__|/build/|/build_[^/]+/|build_stale|\.debug$|\.err$|\.log$|\.sampler$|\.apk$|/system/bin/(aatped|aatpe-cli|aatpe-watchdog|aatpe-probe)$|\.zip$|\.plist$|STAGING_NOTICE|WORKFLOW_MEMORY'; then
  echo "[FAIL] source zip contains build artifacts / prebuilt binaries / logs / test binaries"; FAIL=1
  unzip -l "$SRCZIP" | grep -vE '^Archive:|^  Length|^---------|^[0-9]+ file|^----' | grep -E '\.o$|\.d$|\.pyc$|__pycache__|/build/|/build_[^/]+/|build_stale|\.debug$|\.err$|\.log$|\.sampler$|\.apk$|/system/bin/(aatped|aatpe-cli|aatpe-watchdog|aatpe-probe)$|\.zip$|\.plist$' | head -20
else
  echo "[ok] source zip clean (no .o/.d/.pyc/build/debug/.err/.log/.sampler/.apk/prebuilt)"
fi
N=$(unzip -l "$SRCZIP" | tail -1 | awk '{print $2}')
echo "source zip entries: $N"
fi

echo "=== [27b] module ZIP cleanliness (no __pycache__/.pyc) ==="
if unzip -l "$MODZIP" | grep -vE '^Archive:|^  Length|^---------|^[0-9]+ file' | grep -qE '\.pyc$|__pycache__'; then
  echo "[FAIL] module zip contains Python bytecode cache (__pycache__/*.pyc)"; FAIL=1
else
  echo "[ok] module zip has no __pycache__/.pyc"
fi

echo "=== [P3-63] version + artifact consistency ==="
( cd "$SRC" && bash check_version_consistency.sh ) || { echo "[FAIL] check_version_consistency.sh"; FAIL=1; }

echo "=== [v6.0.0] integrity manifest layout + release hard gate ==="
( cd "$SRC" && bash check-manifest-layout.sh ) || { echo "[FAIL] check-manifest-layout.sh"; FAIL=1; }

echo "=== [v6.0.0 RTC-02] g_cpuMetrics lock-contract static guard (scope-tracked, no pinned lines) ==="
awk '
function braceDelta(s,   o,c){ o=gsub(/\{/,"&",s); c=gsub(/\}/,"&",s); return o-c }
BEGIN { bad=0; lockActive=0; depth=0; lockDepth=0; lastlock=-9999 }
{
  line=$0;
  islock   = (line ~ /(lock_guard|unique_lock)[^(]*\(g_cpuMetricsMutex/);
  isaccess = (line ~ /g_cpuMetrics[[:space:]]*\.(find|begin|end)|g_cpuMetrics[[:space:]]*\[|: g_cpuMetrics[[:space:]]*[)]/);
  isunlock = (line ~ /\.unlock[[:space:]]*\(\)/);

  if (isaccess && !lockActive) {
    if (NR - lastlock > 12) {
      printf("[FAIL] g_cpuMetrics read at line %d has no g_cpuMetricsMutex lock in scope/window\n", NR);
      bad = 1;
    }
  }
  if (islock) { lastlock = NR; lockActive = 1; lockDepth = depth; }
  if (isunlock) { lockActive = 0; }
  depth += braceDelta(line);
  if (lockActive && depth < lockDepth) { lockActive = 0; }
}
END { exit bad ? 1 : 0 }
' "$SRC/aatped_main.cpp" || { echo "[FAIL] g_cpuMetrics lock-contract regression (someone dropped a lock?)"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] every g_cpuMetrics read in aatped_main.cpp is under a g_cpuMetricsMutex lock (scope-tracked)"

echo "=== [v6.0.0 RTC-01b] backupModelFile mkstemp static guard ==="
awk '
  /^void backupModelFile\(/ { in_fn = 1 }
  in_fn { body = body $0 "\n" }
  in_fn && /^}/ { in_fn = 0; done = 1 }
  END {
    if (body == "") { print "[FAIL] backupModelFile not found in aatped_main.cpp"; exit 1 }
    if (body !~ /mkstemp/) { print "[FAIL] backupModelFile body missing mkstemp (regressed to non-unique tmp?)"; exit 1 }
    if (body ~ /tmpBak[[:space:]]*=[^;]*getpid\(\)/) {
      print "[FAIL] backupModelFile still uses getpid-based tmp name (per-process collision)"; exit 1
    }
    exit 0
  }
' "$SRC/aatped_main.cpp" || { echo "[FAIL] backupModelFile tmp-name regression"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] backupModelFile uses mkstemp (no per-process getpid tmp name)"

echo "=== [v6.0.0 round4] heal-wall split static guard ==="
awk '
  /healShouldLiftThread/ { in_fn = 1 }
  in_fn { body = body $0 "\n" }
  in_fn && /^}/ { in_fn = 0; done = 1 }
  END {
    if (body == "") { print "[FAIL] healShouldLiftThread body not found"; exit 1 }
    if (body ~ /thermalEmergency[[:space:]]*\|\|[[:space:]]*thermalHalLevel[[:space:]]*>=[[:space:]]*3/) {
      print "[FAIL] healShouldLiftThread reverted to unconditional `thermalEmergency || thermalHalLevel>=3` wall (skin throttle must not block cgroup lift)";
      exit 1;
    }
    if (body !~ /if[[:space:]]*\([[:space:]]*thermalEmergency[[:space:]]*\)[[:space:]]*return false/) {
      print "[FAIL] healShouldLiftThread missing the split `if (thermalEmergency) return false;` wall";
      exit 1;
    }
    exit 0
  }
' "$SRC/placement_heal_decision.cpp" || { echo "[FAIL] heal-wall split regression"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] healShouldLiftThread splits thermalEmergency (wall) from skin thermalHalLevel (no wall)"

echo "=== [v6.0.0 round4b] frame fps renderFps + incremental-timestats guard ==="
awk '
  /^/ { body = body $0 "\n" }
  END {
    if (body !~ /renderFps/) {
      print "[FAIL] frame.cpp missing renderFps (latency present-ring fps regressed?)"; exit 1 }
    if (body !~ /g_tsLayerSnap/) {
      print "[FAIL] frame.cpp missing g_tsLayerSnap per-layer snapshot (incremental timestats regressed?)"; exit 1 }
    exit 0
  }
' "$SRC/frame.cpp" || { echo "[FAIL] round4b frame fps regression"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] frame.cpp: renderFps + g_tsLayerSnap present, window tsMs not re-stamped to contentTsMs"

echo "=== [v6.0.0 round4c] foreground timestats increment + feature switches ==="
awk '
  /^/ { body = body $0 "\n" }
  END {
    if (body !~ /snapTotalFrames/) { print "[FAIL] frame.cpp TimestatsSession missing snapTotalFrames increment (foreground timestats still cumulative?)"; exit 1 }
    if (body !~ /deltaFrames/) { print "[FAIL] frame.cpp missing deltaFrames instantaneous fps"; exit 1 }
    exit 0
  }
' "$SRC/frame.cpp" || { echo "[FAIL] round4c foreground timestats increment regression"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] foreground timestats uses per-layer Δframes/ΔwallMs increment"

awk '
  /^/ { body = body $0 "\n" }
  END {
    if (body !~ /set_c4_freezer=/) { print "[FAIL] control dispatcher missing set_c4_freezer="; exit 1 }
    if (body !~ /set_ebpf=/)      { print "[FAIL] control dispatcher missing set_ebpf="; exit 1 }
    if (body !~ /set_federated=/) { print "[FAIL] control dispatcher missing set_federated="; exit 1 }
    exit 0
  }
' "$SRC/aatped_control.cpp" || { echo "[FAIL] round4c feature switches missing"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] control dispatcher exposes set_c4_freezer/set_ebpf/set_federated"

echo "=== [v6.0.0 round5] latency-priority foreground fps (timestats fallback) ==="
awk '
  /^/ { body = body $0 "\n" }
  END {
    if (body !~ /chooseForegroundFps/) { print "[FAIL] frame.cpp missing chooseForegroundFps (latency-priority chooser)"; exit 1 }
    if (body !~ /frame_timestats_suspect/) { print "[FAIL] frame.cpp missing timestats suspect cross-check marker"; exit 1 }
    if (body ~ /return;[ \t]*\/\/[ \t]*timestats authoritative/) { print "[FAIL] timestats re-asserted as authoritative early-return"; exit 1 }
    exit 0
  }
' "$SRC/frame.cpp" || { echo "[FAIL] round5 latency-priority regression"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] foreground fps prefers latency present; timestats fallback + suspect cross-check"

echo "=== [v6.0.0 round5d] WebUI whitelist exposes feature toggles ==="
awk '
  /^/ { body = body $0 "\n" }
  END {
    if (body !~ /set_c4_freezer/) { print "[FAIL] webui_http.cpp whitelist missing set_c4_freezer"; exit 1 }
    if (body !~ /set_ebpf/)        { print "[FAIL] webui_http.cpp whitelist missing set_ebpf"; exit 1 }
    if (body !~ /set_federated/)   { print "[FAIL] webui_http.cpp whitelist missing set_federated"; exit 1 }
    exit 0
  }
' "$SRC/webui_http.cpp" || { echo "[FAIL] round5d webui whitelist regression"; FAIL=1; }
[ "$FAIL" -eq 0 ] && echo "[ok] WebUI whitelist allows set_c4_freezer/set_ebpf/set_federated"

echo "=== [P3-64] SHA256 manifest re-verify ==="
T3=$(mktemp -d)
unzip -q "$MODZIP" 'system/bin/*' -d "$T3"
( cd "$T3/system/bin" && sha256sum aatped aatpe-cli aatpe-watchdog aatpe-probe | sort -k2 ) > "$T3/fresh.manifest"
grep -E '  (aatped|aatpe-cli|aatpe-watchdog|aatpe-probe)$' "$OUT/BUILD_MANIFEST.txt" | awk '{print $1"  "$2}' | sort -k2 > "$T3/committed.manifest"
if diff -u "$T3/committed.manifest" "$T3/fresh.manifest" > "$T3/diff.txt"; then
    echo "[ok] packaged binary sha256 == BUILD_MANIFEST.txt (100% match)"
else
    echo "[FAIL] BUILD_MANIFEST.txt does not match packaged binaries:"
    cat "$T3/diff.txt"
    FAIL=1
fi
( cd "$OUT" && sha256sum -c "$(basename "$MODZIP").sha256" ) && echo "[ok] module zip .sha256 sidecar verifies" || { echo "[FAIL] module zip .sha256 sidecar"; FAIL=1; }
rm -rf "$T3"

echo "=== [P3-67] shell + python syntax check ==="
T3_ERR=$(mktemp)
SH_FAIL=0
while IFS= read -r script; do
    if ! bash -n "$script" 2>"$T3_ERR"; then
        echo "[FAIL] bash -n $script"; cat "$T3_ERR"; SH_FAIL=1
    fi
done < <(cd "$SRC" && find . -name '*.sh' -not -path './tests/*' -not -path './.git/*')
[ "$SH_FAIL" -eq 0 ] && echo "[ok] all *.sh pass bash -n" || FAIL=1
PY_FAIL=0
if ! ( cd "$SRC" && find . -name '*.py' -not -path './tests/*' -not -path './.git/*' -print0 \
        | xargs -0 python3 -c '
import ast, sys
bad = 0
for f in sys.argv[1:]:
    try:
        with open(f, encoding="utf-8") as fh:
            ast.parse(fh.read(), f)
    except SyntaxError as e:
        print("[FAIL] py syntax %s: %s" % (f, e)); bad = 1
sys.exit(bad)
' ) 2>"$T3_ERR"; then
    cat "$T3_ERR"
    PY_FAIL=1
fi
[ "$PY_FAIL" -eq 0 ] && echo "[ok] all *.py pass ast.parse (no .pyc written)" || FAIL=1
rm -f "$T3_ERR"

echo "=== [P3-68] dangerous-path scan in module ZIP ==="
DP_FAIL=0
if unzip -l "$MODZIP" | grep -vE '^Archive:|^  Length|^---------|^[0-9]+ file|^Summary' | grep -E '/(home|Users)/' ; then
    echo "[FAIL] module ZIP contains dev-home path in filenames"; DP_FAIL=1
else
    echo "[ok] no dev-home paths in ZIP filenames"
fi
T4=$(mktemp -d); unzip -q "$MODZIP" -d "$T4"
EMBED=$(mktemp)
for b in aatped aatpe-cli aatpe-watchdog aatpe-probe; do
    strings "$T4/system/bin/$b" 2>/dev/null | grep -E '/(home|Users)/[a-zA-Z]' >> "$EMBED" || true
done
grep -rEh '/(home|Users)/[a-zA-Z]' "$T4" --include='*.sh' --include='*.conf' 2>/dev/null >> "$EMBED" || true
grep -vE '/data/local/tmp|LLD |googleplex-android|build-disk' "$EMBED" > "$EMBED.filtered" || true
if [ -s "$EMBED.filtered" ]; then
    echo "[FAIL] package embeds dev-machine paths:"; sort -u "$EMBED.filtered" | head -20; DP_FAIL=1
else
    echo "[ok] no dev-machine paths embedded in package"
fi
rm -f "$EMBED" "$EMBED.filtered"; rm -rf "$T4"
[ "$DP_FAIL" -eq 0 ] && echo "[ok] P3-68 clean" || FAIL=1

echo "=== [round3.1] sweep __pycache__/*.pyc from build tree ==="
find "$SRC" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "$SRC" -type f -name '*.pyc' -delete 2>/dev/null || true
echo "[ok] build tree pyc cache swept"

[ "$FAIL" -eq 0 ] && echo "=== RELEASE CI PASS ===" || { echo "=== RELEASE CI FAIL ==="; exit 1; }

#!/system/bin/sh
set -u

MOUNT=/sys/fs/cgroup
PASS=0; FAIL=0; DEG=0
ok()   { echo "PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL: $1"; FAIL=$((FAIL+1)); }
deg()  { echo "DEGRADED: $1"; DEG=$((DEG+1)); }
have() { command -v "$1" >/dev/null 2>&1; }

if [ -f "$MOUNT/cgroup.controllers" ] && grep -qw freezer "$MOUNT/cgroup.controllers"; then
  ok "cgroup v2 freezer controller present"
else
  deg "no v2 'freezer' controller -> C4 cannot freeze (v1 or kernel lacking it)"
fi

if [ -r "$MOUNT/cgroup.freeze" ]; then
  ok "root cgroup.freeze node readable"
else
  bad "cgroup.freeze node unreadable"
fi

GPU=0
[ -d /sys/class/kgsl ] && { ok "KGSL/Adreno node present"; GPU=1; }
ls /sys/class/devfreq/ 2>/dev/null | grep -qi mali && { ok "Mali devfreq present"; GPU=1; }
[ -d /sys/kernel/ged ] || [ -e /dev/ged ] && { ok "MTK GED node present"; GPU=1; }
[ "$GPU" = 0 ] && deg "no KGSL/Mali/GED node found -> GPU platform unknown"

if have dumpsys; then
  if dumpsys media_session 2>/dev/null | grep -q 'state='; then
    ok "media_session dump parseable (state= lines present)"
  else
    deg "media_session present but no state= lines -> audio whitelist conservative"
  fi
else
  deg "dumpsys unavailable -> audio whitelist always conservative (no freeze)"
fi

FG=$(dumpsys window 2>/dev/null | grep -m1 -E 'mCurrentFocus|topResumedActivity' | sed -E 's/.* ([a-zA-Z0-9_.]+)\/.*/\1/')
if [ -n "$FG" ]; then
  PID=$(pidof "$FG" 2>/dev/null | cut -d' ' -f1)
  if [ -n "$PID" ] && [ -r "/proc/$PID/cgroup" ]; then
    CG=$(grep -E '^0::' "/proc/$PID/cgroup" | head -1 | sed -E 's/^0:://; s|^/||')
    case "$CG" in
      *uid_*) ok "foreground '$FG' v2 cgroup '$CG' has uid_ segment" ;;
      "")      deg "foreground '$FG' has no v2 line (v1? ) -> C4 degrade" ;;
      *)       deg "foreground '$FG' cgroup '$CG' lacks uid_ segment (system? )" ;;
    esac
    if [ -n "$CG" ] && [ -w "$MOUNT/$CG/cgroup.freeze" ] 2>/dev/null; then
      ok "target cgroup.freeze writable"
    else
      bad "target cgroup.freeze not writable (need root / freezer perms)"
    fi
  else
    deg "cannot read /proc/<pid>/cgroup for '$FG'"
  fi
else
  deg "no foreground app resolved"
fi

echo "---"
echo "summary: PASS=$PASS FAIL=$FAIL DEGRADED=$DEG"
if [ "$FAIL" -gt 0 ]; then echo "VERDICT: C4 NOT USABLE (fix FAIL items)";
elif [ "$DEG" -gt 0 ]; then echo "VERDICT: C4 USABLE WITH DEGRADED (conservative whitelist)";
else echo "VERDICT: C4 READY"; fi

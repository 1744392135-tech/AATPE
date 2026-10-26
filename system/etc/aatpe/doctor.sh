#!/system/bin/sh
# AATPE v4.4.5 device/doctor.sh — on-device readiness check for C4 freezer + GPU.
# Prints PASS / FAIL / DEGRADED per item and a verdict "C4 on this device".
# Pure POSIX sh. All checks are read-only (except a probe write to a temp we
# immediately restore). Safe to run as root/shell.
set -u

MOUNT=/sys/fs/cgroup
PASS=0; FAIL=0; DEG=0
ok()   { echo "PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL: $1"; FAIL=$((FAIL+1)); }
deg()  { echo "DEGRADED: $1"; DEG=$((DEG+1)); }
# ISSUE-004 (A7): have() is called below but was never defined in this file (it
# lives in collect.sh), so under `set -u` the call errored / always fell through
# to the else branch and doctor forever reported "dumpsys unavailable". Define it
# here: command-exists probe.
have() { command -v "$1" >/dev/null 2>&1; }

# 1. freezer controller on v2 unified hierarchy
if [ -f "$MOUNT/cgroup.controllers" ] && grep -qw freezer "$MOUNT/cgroup.controllers"; then
  ok "cgroup v2 freezer controller present"
else
  deg "no v2 'freezer' controller -> C4 cannot freeze (v1 or kernel lacking it)"
fi

# 2. root cgroup.freeze node readable
if [ -r "$MOUNT/cgroup.freeze" ]; then
  ok "root cgroup.freeze node readable"
else
  bad "cgroup.freeze node unreadable"
fi

# 3. GPU detection coverage
GPU=0
[ -d /sys/class/kgsl ] && { ok "KGSL/Adreno node present"; GPU=1; }
ls /sys/class/devfreq/ 2>/dev/null | grep -qi mali && { ok "Mali devfreq present"; GPU=1; }
[ -d /sys/kernel/ged ] || [ -e /dev/ged ] && { ok "MTK GED node present"; GPU=1; }
[ "$GPU" = 0 ] && deg "no KGSL/Mali/GED node found -> GPU platform unknown"

# 4. audio session attribution (whitelist input)
if have dumpsys; then
  if dumpsys media_session 2>/dev/null | grep -q 'state='; then
    ok "media_session dump parseable (state= lines present)"
  else
    deg "media_session present but no state= lines -> audio whitelist conservative"
  fi
else
  deg "dumpsys unavailable -> audio whitelist always conservative (no freeze)"
fi

# 5. target app cgroup: pick foreground, confirm a uid_ cgroup + writable freeze
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

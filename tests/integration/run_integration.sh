#!/bin/bash
# AATPE v94 Integration Test runner.
# Builds a fake /sys tree and two fake /proc files (stat, loadavg), then enters
# an unprivileged user+mount namespace. We bind-mock /sys wholesale, but for
# /proc we OVERLAY ONLY the business nodes (/proc/stat, /proc/loadavg) on top of
# the REAL /proc -- we never replace /proc with a fake tree, because ASan/UBSan
# depend on /proc/self/maps + /proc/self/exe for shadow bookkeeping; a wholesale
# fake /proc (no self) made ASan mis-report innocent stack string ops as
# stack-buffer-overflow (W1-AU-004, environment false positive).
# Set AATPE_IT_PROC_MOCK=0 to skip even the per-file proc overlay (fully real
# /proc) when running under a sanitizer; default =1 overlays stat+loadavg only.
set -euo pipefail

SRC="${1:?usage: run_integration.sh SRC_DIR TEST_BIN}"
BIN="${2:?missing test binary}"

export AATPE_IT_ROOT=$(mktemp -d /tmp/aatpe_it_root.XXXXXX)
export AATPE_SRC_DIR="$SRC"
MOCK="$AATPE_IT_ROOT/_mockfs"
mkdir -p "$MOCK/sys/devices/system/cpu"
mkdir -p "$MOCK/sys/class/thermal"
mkdir -p "$MOCK/sys/class/power_supply/battery"
mkdir -p "$MOCK/proc"

# ---- fake /proc baseline ----
echo "cpu  200 0 100 700 0 0 0 0 0 0" > "$MOCK/proc/stat"
echo "0.50 0.40 0.30 1/123 4567" > "$MOCK/proc/loadavg"

# ---- fake CPU topology: 8 cores (MTK-like 4L+2M+2B) ----
# cpu0-2 little (cap 307), cpu3-4 mid (cap 640), cpu5-7 big (cap 1024)
echo "0-7" > "$MOCK/sys/devices/system/cpu/online"
declare -A CAP=( [0]=307 [1]=307 [2]=307 [3]=640 [4]=640 [5]=1024 [6]=1024 [7]=1024 )
declare -A MAXF=( [0]=1800000 [1]=1800000 [2]=1800000 [3]=2000000 [4]=2000000 [5]=2600000 [6]=2600000 [7]=2600000 )
for c in 0 1 2 3 4 5 6 7; do
  d="$MOCK/sys/devices/system/cpu/cpu$c"
  mkdir -p "$d/cpufreq"
  echo "${CAP[$c]}" > "$d/cpu_capacity"
  echo "${MAXF[$c]}" > "$d/cpufreq/scaling_max_freq"
  echo "${MAXF[$c]}" > "$d/cpufreq/scaling_cur_freq"
  echo "1" > "$d/online"
done

# ---- fake thermal zones ----
# zone0 = cpu, zone1 = skin, zone2 = battery
mkdir -p "$MOCK/sys/class/thermal/thermal_zone0"
echo "cpu-thermal" > "$MOCK/sys/class/thermal/thermal_zone0/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone0/temp"
mkdir -p "$MOCK/sys/class/thermal/thermal_zone1"
echo "skin" > "$MOCK/sys/class/thermal/thermal_zone1/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone1/temp"
mkdir -p "$MOCK/sys/class/thermal/thermal_zone2"
echo "battery" > "$MOCK/sys/class/thermal/thermal_zone2/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone2/temp"
# battery temp (0.1 C scale): 300 = 30.0C
echo "300" > "$MOCK/sys/class/power_supply/battery/temp"

# ---- fake DRM refresh-rate nodes (R-W4) ----
# S1 main loop calls updateRefreshRate()->aatpeDetectRefreshRate(). The detector's
# FIRST choice is /sys/class/drm/cardN-X/modes: it trusts only the line marked
# with a leading '*' (active mode) and parses the 2nd column as Hz. If that line
# parses to a rate in [kMinRate,kMaxRate] it returns immediately and NEVER falls
# through to the dumpsys-fork fallback. Materialize a connected connector whose
# active mode is 120Hz so the refresh probe resolves from sysfs with no shell.
mkdir -p "$MOCK/sys/class/drm/card0-DSI-1"
echo "connected" > "$MOCK/sys/class/drm/card0-DSI-1/status"
echo "*1920x1080 120" > "$MOCK/sys/class/drm/card0-DSI-1/modes"

# ---- fake /system/bin/sh (R-W4) ----
# aatpeExec() forks and execl("/system/bin/sh","sh","-c",cmd). On the host / in
# the unshare namespace there is no /system/bin/sh; under ASan the failed-exec
# child does not release the pipe cleanly and the parent blocks in
# read()/futex (proven soak: S1 hangs only inside the user+mount ns). Bind a
# shim that exits 0 immediately (host /bin/true) so every aatpeExec child
# exits at once, the pipe returns clean EOF, and the parent never blocks.
mkdir -p "$MOCK/system/bin"
if [ -x /bin/true ]; then cp /bin/true  "$MOCK/system/bin/sh"; fi
if [ -x /usr/bin/true ]; then cp /usr/bin/true "$MOCK/system/bin/sh"; fi
chmod +x "$MOCK/system/bin/sh"

# ---- materialize rules + profiles into the mock conf dir ----
mkdir -p "$AATPE_IT_ROOT/conf/rules" "$AATPE_IT_ROOT/conf/profiles" "$AATPE_IT_ROOT/ml"
cp "$SRC/rules/thread_roles.conf" "$AATPE_IT_ROOT/conf/rules/"
cp "$SRC/profiles/"*.conf "$AATPE_IT_ROOT/conf/profiles/"

# /proc/self is NOT mocked: we keep the host /proc (with self/maps, self/exe,
# self/status...) and only overlay /proc/stat + /proc/loadavg below, so ASan's
# shadow bookkeeping stays correct.

echo "IT_ROOT=$AATPE_IT_ROOT"
echo "Entering namespace and executing $BIN"

# Enter user+mount namespace. Bind-mock /sys wholesale; overlay ONLY the two
# business /proc nodes on the real /proc (preserve /proc/self for ASan).
export AATPE_IT_PROC_MOCK="${AATPE_IT_PROC_MOCK:-1}"
unshare --map-root-user --mount bash -c '
  set -e
  mount --bind "'"$MOCK"'/sys" /sys
  if [ "${AATPE_IT_PROC_MOCK:-1}" != "0" ]; then
    mount --bind "'"$MOCK"'/proc/stat" /proc/stat
    mount --bind "'"$MOCK"'/proc/loadavg" /proc/loadavg
  fi
  # R-W4: provide /system/bin/sh as an exit-0 shim so aatpeExec() children
  # terminate immediately and the pipe returns clean EOF (no ASan pipe_read
  # hang). The mountpoint /system may not be creatable on a read-only overlay
  # root inside the userns; if so this is best-effort and the drm sysfs nodes
  # above short-circuit the refresh-rate probe before it ever forks sh.
  if mkdir -p /system 2>/dev/null && mount --bind "'"$MOCK"'/system" /system 2>/dev/null; then
    echo "  ns: /system/bin/sh shim mounted"
  else
    echo "  ns: /system bind skipped (rely on drm sysfs short-circuit)"
  fi
  # sanity
  echo "  ns: cpu online = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
  echo "  ns: /proc/self/maps readable = $([ -r /proc/self/maps ] && echo yes || echo NO)  (proc_mock=${AATPE_IT_PROC_MOCK})"
  exec "'"$BIN"'"
'
rc=$?
echo "Test binary exit code: $rc"
# preserve a copy of the on-device mock tree for post-mortem if requested
if [ "${AATPE_IT_KEEP:-0}" = "1" ]; then
  echo "Kept IT_ROOT=$AATPE_IT_ROOT"
else
  rm -rf "$AATPE_IT_ROOT"
fi
exit $rc

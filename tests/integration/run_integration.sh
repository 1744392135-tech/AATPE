#!/bin/bash
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

echo "cpu  200 0 100 700 0 0 0 0 0 0" > "$MOCK/proc/stat"
echo "0.50 0.40 0.30 1/123 4567" > "$MOCK/proc/loadavg"

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

mkdir -p "$MOCK/sys/class/thermal/thermal_zone0"
echo "cpu-thermal" > "$MOCK/sys/class/thermal/thermal_zone0/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone0/temp"
mkdir -p "$MOCK/sys/class/thermal/thermal_zone1"
echo "skin" > "$MOCK/sys/class/thermal/thermal_zone1/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone1/temp"
mkdir -p "$MOCK/sys/class/thermal/thermal_zone2"
echo "battery" > "$MOCK/sys/class/thermal/thermal_zone2/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone2/temp"
echo "300" > "$MOCK/sys/class/power_supply/battery/temp"

mkdir -p "$MOCK/sys/class/drm/card0-DSI-1"
echo "connected" > "$MOCK/sys/class/drm/card0-DSI-1/status"
echo "*1920x1080 120" > "$MOCK/sys/class/drm/card0-DSI-1/modes"

mkdir -p "$MOCK/system/bin"
if [ -x /bin/true ]; then cp /bin/true  "$MOCK/system/bin/sh"; fi
if [ -x /usr/bin/true ]; then cp /usr/bin/true "$MOCK/system/bin/sh"; fi
chmod +x "$MOCK/system/bin/sh"

mkdir -p "$AATPE_IT_ROOT/conf/rules" "$AATPE_IT_ROOT/conf/profiles" "$AATPE_IT_ROOT/ml"
cp "$SRC/rules/thread_roles.conf" "$AATPE_IT_ROOT/conf/rules/"
cp "$SRC/profiles/"*.conf "$AATPE_IT_ROOT/conf/profiles/"

echo "IT_ROOT=$AATPE_IT_ROOT"
echo "Entering namespace and executing $BIN"

export AATPE_IT_PROC_MOCK="${AATPE_IT_PROC_MOCK:-1}"
unshare --map-root-user --mount bash -c '
  set -e
  mount --bind "'"$MOCK"'/sys" /sys
  if [ "${AATPE_IT_PROC_MOCK:-1}" != "0" ]; then
    mount --bind "'"$MOCK"'/proc/stat" /proc/stat
    mount --bind "'"$MOCK"'/proc/loadavg" /proc/loadavg
  fi
  if mkdir -p /system 2>/dev/null && mount --bind "'"$MOCK"'/system" /system 2>/dev/null; then
    echo "  ns: /system/bin/sh shim mounted"
  else
    echo "  ns: /system bind skipped (rely on drm sysfs short-circuit)"
  fi
  echo "  ns: cpu online = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
  echo "  ns: /proc/self/maps readable = $([ -r /proc/self/maps ] && echo yes || echo NO)  (proc_mock=${AATPE_IT_PROC_MOCK})"
  exec "'"$BIN"'"
'
rc=$?
echo "Test binary exit code: $rc"
if [ "${AATPE_IT_KEEP:-0}" = "1" ]; then
  echo "Kept IT_ROOT=$AATPE_IT_ROOT"
else
  rm -rf "$AATPE_IT_ROOT"
fi
exit $rc

#!/bin/bash
# AATPE W1 REAL soak runner. Builds a fake /sys+/proc tree, enters an unprivileged
# user+mount namespace, bind-mocks /sys and /proc, and execs the soak binary that
# links the REAL production objects.
#
# Env overrides:
#   AATPE_SOAK_TICKS       fast simulated ticks (default 260000 ~= 72h @1Hz)
#   AATPE_WALL_SECONDS     real wall-clock seconds (default 1800 = 30 min)
#   AATPE_SOAK_LOGDIR      where soak_samples.csv + log go (default STAGE/logs)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE="$(cd "$HERE/../.." && pwd)"
BIN="$HERE/build/aatpe_soak"
SRC="$STAGE"

export AATPE_IT_ROOT=$(mktemp -d /tmp/aatpe_soak_root.XXXXXX)
MOCK="$AATPE_IT_ROOT/_mockfs"
mkdir -p "$MOCK/sys/devices/system/cpu"
mkdir -p "$MOCK/sys/class/thermal"
mkdir -p "$MOCK/sys/class/power_supply/battery"
mkdir -p "$MOCK/proc"

echo "cpu  200 0 100 700 0 0 0 0 0 0" > "$MOCK/proc/stat"
echo "0.50 0.40 0.30 1/123 4567" > "$MOCK/proc/loadavg"

# 8-core MTK-like topology
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

# thermal zones
for z in 0 1 2; do
  mkdir -p "$MOCK/sys/class/thermal/thermal_zone$z"
done
echo "cpu-thermal" > "$MOCK/sys/class/thermal/thermal_zone0/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone0/temp"
echo "skin" > "$MOCK/sys/class/thermal/thermal_zone1/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone1/temp"
echo "battery" > "$MOCK/sys/class/thermal/thermal_zone2/type"
echo "30000" > "$MOCK/sys/class/thermal/thermal_zone2/temp"
echo "300" > "$MOCK/sys/class/power_supply/battery/temp"

# rules + profiles into mock conf dir
mkdir -p "$AATPE_IT_ROOT/conf/rules" "$AATPE_IT_ROOT/conf/profiles" "$AATPE_IT_ROOT/ml"
cp "$SRC/rules/thread_roles.conf" "$AATPE_IT_ROOT/conf/rules/" 2>/dev/null || true
cp "$SRC/profiles/"*.conf "$AATPE_IT_ROOT/conf/profiles/" 2>/dev/null || true

# log dir for the harness
LOGDIR="${AATPE_SOAK_LOGDIR:-$STAGE/logs}"
mkdir -p "$LOGDIR"
export AATPE_SOAK_LOGDIR="$LOGDIR"

echo "SOAK_ROOT=$AATPE_IT_ROOT  LOGDIR=$LOGDIR"
echo "TICKS=${AATPE_SOAK_TICKS:-260000}  WALL=${AATPE_WALL_SECONDS:-1800}"

unshare --map-root-user --mount bash -c '
  set -e
  mount --bind "'"$MOCK"'/sys" /sys
  # Override ONLY the specific proc files the daemon reads for load, leaving the
  # rest of /proc REAL so /proc/self/fd and /proc/self/status work for resource
  # sampling. (A whole-fake /proc bind hid /proc/self and broke fd/RSS accounting.)
  mount --bind "'"$MOCK"'/proc/stat" /proc/stat
  mount --bind "'"$MOCK"'/proc/loadavg" /proc/loadavg
  # Fault-injection mount points (valid inside the userns+mountns):
  #  ro   : read-only tmpfs  -> open(O_WRONLY) must fail EACCES (fail-closed).
  #  full : tiny 60KB tmpfs -> large writes must hit ENOSPC (fail-closed).
  mkdir -p "'"$AATPE_IT_ROOT"'/ro" "'"$AATPE_IT_ROOT"'/full"
  mount -t tmpfs -o size=1m,ro,mode=0755 tmpfs "'"$AATPE_IT_ROOT"'/ro"
  mount -t tmpfs -o size=60k,mode=0755 tmpfs "'"$AATPE_IT_ROOT"'/full"
  export AATPE_RO_DIR="'"$AATPE_IT_ROOT"'/ro"
  export AATPE_FULL_DIR="'"$AATPE_IT_ROOT"'/full"
  unset AATPE_REAL_PROC
  echo "  ns: cpu online = $(cat /sys/devices/system/cpu/online 2>/dev/null)"
  exec "'"$BIN"'"
'
rc=$?
echo "soak exit code: $rc"
if [ "${AATPE_SOAK_KEEP:-0}" = "1" ]; then echo "Kept SOAK_ROOT=$AATPE_IT_ROOT"; else rm -rf "$AATPE_IT_ROOT"; fi
exit $rc

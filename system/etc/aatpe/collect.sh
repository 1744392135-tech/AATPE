#!/system/bin/sh
set -u

OUT="${1:-/sdcard/aatpe_collect_$$}"
mkdir -p "$OUT" 2>/dev/null
echo "[collect] out=$OUT"

have() { command -v "$1" >/dev/null 2>&1; }
dump() {
  f="$1"; shift
  echo "### $*" > "$OUT/$f" 2>>"$OUT/errors.txt"
  "$@" >> "$OUT/$f" 2>>"$OUT/errors.txt" || echo "[rc=$?] $*" >> "$OUT/$f"
}

dump build.txt        getprop ro.build.version.release
dump build.txt        getprop ro.build.version.sdk
dump build.txt        getprop ro.product.model
dump build.txt        uname -a

dump cpu_topology.txt cat /proc/cpuinfo
for c in /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq; do
  echo "$c = $(cat $c 2>/dev/null)" >> "$OUT/cpu_topology.txt"
done
dump cpuset.txt       cat /sys/fs/cgroup/cpuset/cpuset.cpus.effective 2>/dev/null || true

dump cgroup_root.txt  cat /sys/fs/cgroup/cgroup.controllers
dump cgroup_root.txt  cat /sys/fs/cgroup/cgroup.freeze 2>/dev/null || echo "(no root cgroup.freeze)"

FG=$(dumpsys window 2>/dev/null | grep -m1 -E 'mCurrentFocus|topResumedActivity' | sed -E 's/.* ([a-zA-Z0-9_.]+)\/.*/\1/')
echo "foreground=$FG" > "$OUT/target.txt"
if [ -n "$FG" ]; then
  for pid in $(pidof "$FG" 2>/dev/null); do
    echo "--- pid $pid ---" >> "$OUT/app_cgroup.txt"
    cat "/proc/$pid/cgroup" >> "$OUT/app_cgroup.txt" 2>/dev/null
    grep -E '^Uid:' "/proc/$pid/status" >> "$OUT/app_cgroup.txt" 2>/dev/null
  done
  UIDLINE=$(grep -m1 '^Uid:' /proc/$(pidof "$FG" 2>/dev/null | cut -d' ' -f1)/status 2>/dev/null)
  echo "uidline=$UIDLINE" >> "$OUT/target.txt"
fi

dump media_session.txt dumpsys media_session

dump gpu_nodes.txt    sh -c 'for f in /sys/class/kgsl/kgsl-3d0/gpubusy /sys/kernel/ged/hal/gpu_utilization /dev/ged; do [ -e "$f" ] && echo "present $f" || echo "absent $f"; done'
ls /sys/class/devfreq/ > "$OUT/gpu_devfreq.txt" 2>/dev/null || echo "(no devfreq)" >> "$OUT/gpu_devfreq.txt"

dump psi.txt          cat /sys/fs/cgroup/pressure/io 2>/dev/null || true
ls /sys/class/thermal/ > "$OUT/thermal_zones.txt" 2>/dev/null || true

echo "[collect] done -> $OUT"
echo "[collect] tar: tar -czf ${OUT}.tgz -C $(dirname $OUT) $(basename $OUT)"

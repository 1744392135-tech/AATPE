#!/system/bin/sh
# AATPE device/collect.sh — on-device evidence collector (version-neutral header;
# tracks the module VERSION at runtime, do not pin a release string here).
# Captures everything needed to validate C4 cgroup v2 freezer + GPU detection on a
# REAL device (the sandbox has no /dev/kvm / no phone). POSIX sh; runs under
# Magisk/KSU/APatch. No writes except into the output dir.
#
# Usage: collect.sh [OUTDIR]
#   OUTDIR defaults to /sdcard/aatpe_collect_<pid>/
set -u

OUT="${1:-/sdcard/aatpe_collect_$$}"
mkdir -p "$OUT" 2>/dev/null
echo "[collect] out=$OUT"

have() { command -v "$1" >/dev/null 2>&1; }
dump() { # file  cmd...
  f="$1"; shift
  echo "### $*" > "$OUT/$f" 2>>"$OUT/errors.txt"
  "$@" >> "$OUT/$f" 2>>"$OUT/errors.txt" || echo "[rc=$?] $*" >> "$OUT/$f"
}

# 1. Build / kernel / SDK
dump build.txt        getprop ro.build.version.release
dump build.txt        getprop ro.build.version.sdk
dump build.txt        getprop ro.product.model
dump build.txt        uname -a

# 2. CPU topology (cluster / affinity)
dump cpu_topology.txt cat /proc/cpuinfo
for c in /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq; do
  echo "$c = $(cat $c 2>/dev/null)" >> "$OUT/cpu_topology.txt"
done
dump cpuset.txt       cat /sys/fs/cgroup/cpuset/cpuset.cpus.effective 2>/dev/null || true

# 3. Cgroup v2 unified + freezer controller
dump cgroup_root.txt  cat /sys/fs/cgroup/cgroup.controllers
dump cgroup_root.txt  cat /sys/fs/cgroup/cgroup.freeze 2>/dev/null || echo "(no root cgroup.freeze)"

# 4. Foreground app + its per-process cgroup samples
FG=$(dumpsys window 2>/dev/null | grep -m1 -E 'mCurrentFocus|topResumedActivity' | sed -E 's/.* ([a-zA-Z0-9_.]+)\/.*/\1/')
echo "foreground=$FG" > "$OUT/target.txt"
if [ -n "$FG" ]; then
  for pid in $(pidof "$FG" 2>/dev/null); do
    echo "--- pid $pid ---" >> "$OUT/app_cgroup.txt"
    cat "/proc/$pid/cgroup" >> "$OUT/app_cgroup.txt" 2>/dev/null
    grep -E '^Uid:' "/proc/$pid/status" >> "$OUT/app_cgroup.txt" 2>/dev/null
  done
  # target cgroup.freeze writability (best-effort guess of its uid_ path)
  UIDLINE=$(grep -m1 '^Uid:' /proc/$(pidof "$FG" 2>/dev/null | cut -d' ' -f1)/status 2>/dev/null)
  echo "uidline=$UIDLINE" >> "$OUT/target.txt"
fi

# 5. Audio session attribution (for the C4 audio whitelist)
dump media_session.txt dumpsys media_session

# 6. GPU nodes: kgsl / mali / ged + devfreq
dump gpu_nodes.txt    sh -c 'for f in /sys/class/kgsl/kgsl-3d0/gpubusy /sys/kernel/ged/hal/gpu_utilization /dev/ged; do [ -e "$f" ] && echo "present $f" || echo "absent $f"; done'
ls /sys/class/devfreq/ > "$OUT/gpu_devfreq.txt" 2>/dev/null || echo "(no devfreq)" >> "$OUT/gpu_devfreq.txt"

# 7. Scene-threshold related nodes (PSI / thermal zone names)
dump psi.txt          cat /sys/fs/cgroup/pressure/io 2>/dev/null || true
ls /sys/class/thermal/ > "$OUT/thermal_zones.txt" 2>/dev/null || true

echo "[collect] done -> $OUT"
echo "[collect] tar: tar -czf ${OUT}.tgz -C $(dirname $OUT) $(basename $OUT)"

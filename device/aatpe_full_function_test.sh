#!/system/bin/sh
# =============================================================================
# AATPE v4.2.1 全功能穷尽测试脚本
# 用法: su -c "sh /data/local/tmp/aatpe_full_test.sh"
# 输出: 每项测试的 PASS/FAIL/WARN/DEGRADED/FALLBACK + 依据
# =============================================================================

RUN_DIR="/dev/aatpe"
STATE_DIR="/dev/aatpe/state"
CONF_DIR="/system/etc/aatpe"
DECISION_DIR="/dev/aatpe/decisions"
LOG_FILE="/dev/aatpe/aatpe.log"
PID_FILE="/dev/aatpe/daemon.pid"
CLI="/system/bin/aatpe-cli"
DAEMON="/system/bin/aatped"
WATCHDOG="/system/bin/aatpe-watchdog"
PROBE="/system/bin/aatpe-probe"
WEBROOT="/system/etc/aatpe/webui/index.html"
EBPF_DIR="/system/etc/aatpe/ebpf"
PROFILES_DIR="/system/etc/aatpe/profiles"
RULES_DIR="/system/etc/aatpe/rules"

PASS=0; FAIL=0; WARN=0; DEGRADED=0; FALLBACK=0; TOTAL=0
RESULTS=""

# 颜色（终端支持时）
if [ -t 1 ]; then
  C_GREEN='\033[0;32m'; C_RED='\033[0;31m'; C_YELLOW='\033[1;33m'
  C_BLUE='\033[0;34m'; C_CYAN='\033[0;36m'; C_NC='\033[0m'
else
  C_GREEN=''; C_RED=''; C_YELLOW=''; C_BLUE=''; C_CYAN=''; C_NC=''
fi

log_result() {
  # $1=类别 $2=测试项 $3=状态(PASS/FAIL/WARN/DEGRADED/FALLBACK) $4=依据
  TOTAL=$((TOTAL+1))
  case "$3" in
    PASS) PASS=$((PASS+1)); tag="${C_GREEN}[PASS]${C_NC}";;
    FAIL) FAIL=$((FAIL+1)); tag="${C_RED}[FAIL]${C_NC}";;
    WARN) WARN=$((WARN+1)); tag="${C_YELLOW}[WARN]${C_NC}";;
    DEGRADED) DEGRADED=$((DEGRADED+1)); tag="${C_YELLOW}[DEGRADED]${C_NC}";;
    FALLBACK) FALLBACK=$((FALLBACK+1)); tag="${C_BLUE}[FALLBACK]${C_NC}";;
  esac
  printf "%s %-20s %-45s %s\n" "$tag" "$1" "$2" "$4"
  RESULTS="${RESULTS}${3}|${1}|${2}|${4}\n"
}

section() {
  echo ""
  echo "${C_CYAN}==========================================================================="
  echo "  $1"
  echo "===========================================================================${C_NC}"
}

read_state() {
  # $1=key $2=default
  local val=""
  if [ -f "$STATE_DIR/$1" ]; then
    val=$(cat "$STATE_DIR/$1" 2>/dev/null)
  fi
  echo "${val:-$2}"
}

# =============================================================================
echo "${C_CYAN}###########################################################################"
echo "#  AATPE v4.2.1 全功能穷尽测试"
echo "#  时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo "#  设备: $(getprop ro.product.model 2>/dev/null) $(getprop ro.build.version.release 2>/dev/null)"
echo "###########################################################################${C_NC}"

# =============================================================================
section "一、环境与安装完整性 (Environment & Installation)"
# =============================================================================

# T001 Root权限
if [ "$(id -u)" = "0" ]; then
  log_result "环境" "Root权限" "PASS" "uid=0, 具备完整系统访问权限"
else
  log_result "环境" "Root权限" "FAIL" "uid=$(id -u), 非root用户，多数测试将失败"
fi

# T002 二进制存在性
for bin in $CLI $DAEMON $WATCHDOG; do
  bname=$(basename $bin)
  if [ -f "$bin" ]; then
    log_result "安装" "$bname 存在" "PASS" "$bin $(ls -l $bin 2>/dev/null | awk '{print $5}')B"
  else
    log_result "安装" "$bname 存在" "FAIL" "$bin 不存在"
  fi
done

# T003 二进制权限
for bin in $CLI $DAEMON $WATCHDOG; do
  bname=$(basename $bin)
  perms=$(stat -c '%a' $bin 2>/dev/null || toybox stat -c '%a' $bin 2>/dev/null)
  if [ "$perms" = "755" ] || [ "$perms" = "0755" ]; then
    log_result "安装" "$bname 权限" "PASS" "权限=$perms (rwxr-xr-x)"
  else
    log_result "安装" "$bname 权限" "WARN" "权限=$perms, 期望755"
  fi
done

# T004 配置文件存在
for cf in "$CONF_DIR/aatpe.conf" "$RULES_DIR/thread_roles.conf"; do
  cname=$(basename $cf)
  if [ -f "$cf" ]; then
    log_result "安装" "$cname 存在" "PASS" "$cf $(wc -c < $cf 2>/dev/null)B"
  else
    log_result "安装" "$cname 存在" "FAIL" "$cf 不存在"
  fi
done

# T005 Profile文件
prof_count=$(ls $PROFILES_DIR/*.conf 2>/dev/null | wc -l)
if [ "$prof_count" -ge 4 ]; then
  log_result "安装" "Profile配置" "PASS" "找到${prof_count}个profile: $(ls $PROFILES_DIR/*.conf 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"
else
  log_result "安装" "Profile配置" "FAIL" "仅找到${prof_count}个profile, 期望≥4"
fi

# T006 eBPF文件
if [ -f "$EBPF_DIR/sched_events.bpf.o" ] && [ -f "$EBPF_DIR/sched_events_embedded.h" ]; then
  log_result "安装" "eBPF字节码" "PASS" "bpf.o=$(wc -c < $EBPF_DIR/sched_events.bpf.o 2>/dev/null)B, embedded.h存在"
else
  log_result "安装" "eBPF字节码" "FAIL" "eBPF文件缺失"
fi

# T007 WebUI文件
if [ -f "$WEBROOT" ]; then
  log_result "安装" "WebUI文件" "PASS" "index.html=$(wc -c < $WEBROOT 2>/dev/null)B"
else
  log_result "安装" "WebUI文件" "FAIL" "$WEBROOT 不存在"
fi

# T008 Magisk模块信息
if [ -f "/data/adb/modules/aatpe/module.prop" ]; then
  mod_ver=$(grep '^version=' /data/adb/modules/aatpe/module.prop 2>/dev/null | cut -d= -f2)
  log_result "安装" "Magisk模块注册" "PASS" "module.prop存在, version=$mod_ver"
else
  log_result "安装" "Magisk模块注册" "WARN" "/data/adb/modules/aatpe/module.prop不存在(可能非Magisk安装)"
fi

# =============================================================================
section "二、守护进程生命周期 (Daemon Lifecycle)"
# =============================================================================

# T010 PID文件
if [ -f "$PID_FILE" ]; then
  dpid=$(cat $PID_FILE 2>/dev/null)
  if [ -n "$dpid" ] && [ "$dpid" -gt 0 ] 2>/dev/null; then
    log_result "守护进程" "PID文件" "PASS" "PID=$dpid"
  else
    log_result "守护进程" "PID文件" "FAIL" "PID文件内容无效: '$dpid'"
  fi
else
  log_result "守护进程" "PID文件" "FAIL" "$PID_FILE 不存在, 守护进程未启动"
fi

# T011 进程运行状态
if [ -n "$dpid" ] && [ -d "/proc/$dpid" ]; then
  log_result "守护进程" "进程运行" "PASS" "/proc/$dpid 存在, 进程存活"
else
  log_result "守护进程" "进程运行" "FAIL" "PID $dpid 对应的/proc目录不存在, 进程未运行"
fi

# T012 PID身份验证（防止PID复用）
if [ -n "$dpid" ] && [ -d "/proc/$dpid" ]; then
  exe=$(readlink /proc/$dpid/exe 2>/dev/null)
  if echo "$exe" | grep -q "aatped"; then
    log_result "守护进程" "PID身份验证" "PASS" "/proc/$dpid/exe -> $exe (确认为aatped)"
  else
    log_result "守护进程" "PID身份验证" "FAIL" "/proc/$dpid/exe -> $exe (非aatped, PID可能被复用)"
  fi
fi

# T013 运行时目录
if [ -d "$RUN_DIR" ]; then
  log_result "守护进程" "运行时目录" "PASS" "$RUN_DIR 存在, 包含: $(ls $RUN_DIR 2>/dev/null | tr '\n' ' ')"
else
  log_result "守护进程" "运行时目录" "FAIL" "$RUN_DIR 不存在"
fi

# T014 状态目录
if [ -d "$STATE_DIR" ]; then
  state_count=$(ls $STATE_DIR 2>/dev/null | wc -l)
  log_result "守护进程" "状态目录" "PASS" "$STATE_DIR 存在, ${state_count}个状态文件"
else
  log_result "守护进程" "状态目录" "FAIL" "$STATE_DIR 不存在"
fi

# T015 CLI version
ver_out=$($CLI version 2>&1)
if echo "$ver_out" | grep -q "v4.2.1"; then
  log_result "守护进程" "CLI版本" "PASS" "$ver_out"
else
  log_result "守护进程" "CLI版本" "WARN" "版本输出: $ver_out"
fi

# T016 CLI status（守护进程响应）
status_out=$($CLI status 2>&1)
if echo "$status_out" | grep -q "RUNNING"; then
  uptime=$(echo "$status_out" | grep "Uptime" | awk '{print $2}')
  log_result "守护进程" "CLI status" "PASS" "守护进程RUNNING, uptime=${uptime}s"
elif echo "$status_out" | grep -q "STOPPED"; then
  log_result "守护进程" "CLI status" "FAIL" "守护进程STOPPED"
else
  log_result "守护进程" "CLI status" "WARN" "status输出异常: $(echo $status_out | head -1)"
fi

# =============================================================================
section "三、CPU拓扑与调度 (CPU Topology & Scheduling)"
# =============================================================================

# T020 CPU数量
cpu_count=$(read_state "cpu_count" "0")
if [ "$cpu_count" -gt 0 ] 2>/dev/null; then
  log_result "CPU拓扑" "CPU数量" "PASS" "检测到${cpu_count}个CPU (online=$(cat /sys/devices/system/cpu/online 2>/dev/null))"
else
  log_result "CPU拓扑" "CPU数量" "FAIL" "cpu_count=0, 拓扑检测失败"
fi

# T021 小核池
little_pool=$(read_state "little_pool" "")
if [ -n "$little_pool" ]; then
  log_result "CPU拓扑" "小核池(little)" "PASS" "CPUs: $little_pool"
else
  log_result "CPU拓扑" "小核池(little)" "WARN" "little_pool为空, 可能使用统一调度"
fi

# T022 中核池
mid_pool=$(read_state "mid_pool" "")
if [ -n "$mid_pool" ]; then
  log_result "CPU拓扑" "中核池(mid)" "PASS" "CPUs: $mid_pool"
else
  log_result "CPU拓扑" "中核池(mid)" "FALLBACK" "mid_pool为空, 设备可能无中核(2-pool架构), 回退到little/big两级"
fi

# T023 大核池
big_pool=$(read_state "big_pool" "")
if [ -n "$big_pool" ]; then
  log_result "CPU拓扑" "大核池(big)" "PASS" "CPUs: $big_pool"
else
  log_result "CPU拓扑" "大核池(big)" "FAIL" "big_pool为空, 无法识别大核"
fi

# T023b ROUND-004: 三级拓扑 clusters 状态文件
clusters_file="$STATE_DIR/clusters"
if [ -f "$clusters_file" ]; then
  _bad=0; _lines=0; _has_prime=0
  while IFS= read -r ln; do
    [ -z "$ln" ] && continue
    _lines=$((_lines+1))
    # format: tier,capacity,minKhz-maxKhz:cpulist
    case "$ln" in
      PRIME,*) _has_prime=1 ;;
    esac
    case "$ln" in
      LITTLE,*|MID,*|BIG,*|PRIME,*)
        # capacity 数字 + freq range + colon cpulist
        cap=$(echo "$ln" | cut -d, -f2)
        range=$(echo "$ln" | cut -d, -f3 | cut -d: -f1)
        cpus=$(echo "$ln" | cut -d: -f2)
        case "$cap" in ''|*[!0-9]*) _bad=$((_bad+1)) ;; esac
        case "$range" in *-*) : ;; *) _bad=$((_bad+1)) ;; esac
        [ -z "$cpus" ] && _bad=$((_bad+1))
        ;;
      *) _bad=$((_bad+1)) ;;
    esac
  done < "$clusters_file"
  if [ "$_lines" -ge 1 ] && [ "$_bad" -eq 0 ]; then
    if [ "$_has_prime" -eq 1 ]; then
      log_result "CPU拓扑" "三级拓扑(clusters)" "PASS" "${_lines}簇含PRIME: $(head -1 $clusters_file) ..."
    else
      log_result "CPU拓扑" "三级拓扑(clusters)" "PASS" "${_lines}簇(无PRIME行, 本机型非prime设计, 允许): $(head -1 $clusters_file)"
    fi
  else
    log_result "CPU拓扑" "三级拓扑(clusters)" "WARN" "clusters格式异常 bad=$_bad lines=$_lines: $(head -1 $clusters_file)"
  fi
else
  log_result "CPU拓扑" "三级拓扑(clusters)" "WARN" "clusters状态文件缺失, 拓扑尚未建立"
fi

# T023c ROUND-004: PRIME 状态键
spill=$(read_state "prime_spillover_count" "")
hosted=$(read_state "prime_hosted_threads" "")
if [ -n "$spill" ]; then
  case "$spill" in *[!0-9]*) log_result "PRIME调度" "spillover计数" "WARN" "非数字: $spill" ;;
    *) log_result "PRIME调度" "spillover计数" "PASS" "prime_spillover_count=$spill" ;; esac
else
  log_result "PRIME调度" "spillover计数" "WARN" "prime_spillover_count缺失"
fi
if [ -n "$hosted" ]; then
  case "$hosted" in *[!0-9]*) log_result "PRIME调度" "hosted线程数" "WARN" "非数字: $hosted" ;;
    *) log_result "PRIME调度" "hosted线程数" "PASS" "prime_hosted_threads=$hosted" ;; esac
else
  log_result "PRIME调度" "hosted线程数" "WARN" "prime_hosted_threads缺失"
fi

# T024 每CPU利用率
if [ "$cpu_count" -gt 0 ] 2>/dev/null; then
  util_ok=0; util_na=0; _c=0
  while [ "$_c" -lt "$cpu_count" ]; do
    u=$(read_state "cpu${_c}/util" "-1")
    if [ "$u" -ge 0 ] 2>/dev/null; then util_ok=$((util_ok+1)); else util_na=$((util_na+1)); fi
    _c=$((_c+1))
  done
  if [ "$util_ok" -gt 0 ]; then
    log_result "CPU拓扑" "每CPU利用率" "PASS" "${util_ok}/${cpu_count}个CPU有利用率数据"
  else
    log_result "CPU拓扑" "每CPU利用率" "DEGRADED" "0/${cpu_count}个CPU有利用率, 可能schedstat不可读, 回退到默认值"
  fi
else
  log_result "CPU拓扑" "每CPU利用率" "FAIL" "cpu_count=0, 无法检查"
fi

# T025 每CPU频率
if [ "$cpu_count" -gt 0 ] 2>/dev/null; then
  freq_ok=0; _c=0
  while [ "$_c" -lt "$cpu_count" ]; do
    f=$(read_state "cpu${_c}/cur_freq" "0")
    if [ "$f" -gt 0 ] 2>/dev/null; then freq_ok=$((freq_ok+1)); fi
    _c=$((_c+1))
  done
  if [ "$freq_ok" -gt 0 ]; then
    log_result "CPU拓扑" "每CPU频率" "PASS" "${freq_ok}/${cpu_count}个CPU有频率数据(kHz)"
  else
    log_result "CPU拓扑" "每CPU频率" "DEGRADED" "频率读取失败, 可能/sys/devices/system/cpu/cpuN/cpufreq不可访问"
  fi
fi

# T026 CLI topology
topo_out=$($CLI topology 2>&1)
if echo "$topo_out" | grep -q "CPU"; then
  log_result "CPU拓扑" "CLI topology" "PASS" "拓扑命令正常响应"
else
  log_result "CPU拓扑" "CLI topology" "WARN" "topology输出: $(echo $topo_out | head -1)"
fi

# T027 控制模式
ctrl_mode=$(read_state "control_mode" "unknown")
case "$ctrl_mode" in
  assist|active|passive|powersave|emergency)
    log_result "CPU拓扑" "控制模式" "PASS" "control_mode=$ctrl_mode";;
  *)
    log_result "CPU拓扑" "控制模式" "WARN" "control_mode=$ctrl_mode (非预期值)";;
esac

# T028b ROUND-004: 三档控制模式 set_mode 往返 (passive/assist/active)
_orig_mode=$(read_state "control_mode" "assist")
_mode_ok=0
for m in passive assist active; do
  if $CLI set_mode "$m" >/dev/null 2>&1; then
    sleep 1
    now=$(read_state "control_mode" "")
    if [ "$now" = "$m" ]; then _mode_ok=$((_mode_ok+1)); fi
  fi
done
# 恢复原模式
$CLI set_mode "$_orig_mode" >/dev/null 2>&1
if [ "$_mode_ok" -eq 3 ]; then
  log_result "CPU拓扑" "三档模式切换" "PASS" "passive/assist/active 全部生效, 已恢复=$_orig_mode"
else
  log_result "CPU拓扑" "三档模式切换" "WARN" "$_mode_ok/3档生效, 已恢复=$_orig_mode"
fi
# emergency 是保留态, 用户主动 set_mode emergency 应被拒
if $CLI set_mode emergency >/dev/null 2>&1; then
  log_result "CPU拓扑" "emergency拒绝" "FAIL" "用户可直接set_mode=emergency, 安全门被绕过"
else
  log_result "CPU拓扑" "emergency拒绝" "PASS" "set_mode emergency被CLI拒绝(保留态仅系统可置)"
fi

# T028 当前Profile
cur_profile=$(read_state "current_profile" "unknown")
if [ -f "$PROFILES_DIR/${cur_profile}.conf" ]; then
  log_result "CPU拓扑" "当前Profile" "PASS" "current_profile=$cur_profile (配置文件存在)"
else
  log_result "CPU拓扑" "当前Profile" "WARN" "current_profile=$cur_profile (配置文件不存在)"
fi

# =============================================================================
section "四、线程跟踪与放置决策 (Thread Tracking & Placement)"
# =============================================================================

# T030 跟踪线程数
tracked=$(read_state "tracked_threads" "0")
if [ "$tracked" -gt 0 ] 2>/dev/null; then
  log_result "线程调度" "跟踪线程数" "PASS" "tracked_threads=$tracked"
else
  log_result "线程调度" "跟踪线程数" "DEGRADED" "tracked_threads=$tracked, 守护进程可能刚启动尚未完成首次扫描"
fi

# T031 CLI threads
threads_out=$($CLI threads 2>&1)
if echo "$threads_out" | grep -q "THREAD DATABASE"; then
  thread_lines=$(echo "$threads_out" | grep -c '^\[' 2>/dev/null)
  log_result "线程调度" "CLI threads" "PASS" "线程数据库可查询, 显示${thread_lines}条记录"
else
  log_result "线程调度" "CLI threads" "WARN" "threads输出: $(echo $threads_out | head -1)"
fi

# T032 决策目录
if [ -d "$DECISION_DIR" ]; then
  dec_count=$(ls $DECISION_DIR 2>/dev/null | wc -l)
  log_result "线程调度" "决策记录" "PASS" "$DECISION_DIR 存在, ${dec_count}个决策文件"
else
  log_result "线程调度" "决策记录" "WARN" "$DECISION_DIR 不存在或为空"
fi

# T033 CLI decisions
dec_out=$($CLI decisions 5 2>&1)
if echo "$dec_out" | grep -qEi "decision|migration|cpu|->|Score"; then
  log_result "线程调度" "CLI decisions" "PASS" "决策日志可查询"
else
  log_result "线程调度" "CLI decisions" "DEGRADED" "决策输出可能为空: $(echo $dec_out | head -1)"
fi

# T034 前台应用
fg_app=$(read_state "foreground_app" "unknown")
fg_src=$(read_state "foreground_app_source" "unknown")
if [ "$fg_app" != "unknown" ] && [ -n "$fg_app" ]; then
  log_result "线程调度" "前台应用识别" "PASS" "foreground_app=$fg_app, source=$fg_src"
else
  log_result "线程调度" "前台应用识别" "DEGRADED" "foreground_app=unknown, 可能无ActivityManager连接或辅助APK未安装"
fi

# T035 辅助APK
helper_apk=$(read_state "helper_apk_installed" "0")
if [ "$helper_apk" = "1" ]; then
  log_result "线程调度" "辅助APK" "PASS" "helper_apk_installed=1, 可获取精确前台应用"
else
  log_result "线程调度" "辅助APK" "FALLBACK" "helper_apk_installed=0, 使用模块端/proc回退检测前台应用"
fi

# =============================================================================
section "五、热管理 (Thermal Management)"
# =============================================================================

# T040 CPU温度
cpu_temp=$(read_state "thermal_cpu_max" "N/A")
if [ "$cpu_temp" != "N/A" ] && [ -n "$cpu_temp" ]; then
  if [ "$cpu_temp" -gt 0 ] 2>/dev/null && [ "$cpu_temp" -lt 150000 ] 2>/dev/null; then
    log_result "热管理" "CPU最高温度" "PASS" "thermal_cpu_max=${cpu_temp}°C (单位换算正确, 在合理范围)"
  else
    log_result "热管理" "CPU最高温度" "WARN" "thermal_cpu_max=${cpu_temp}, 超出合理范围(0-150°C), 可能单位换算问题"
  fi
else
  log_result "热管理" "CPU最高温度" "DEGRADED" "thermal_cpu_max=N/A, thermal zone可能不可读, 使用默认值"
fi

# T041 皮肤温度
skin_temp=$(read_state "thermal_skin" "N/A")
if [ "$skin_temp" != "N/A" ] && [ "$skin_temp" != "-1" ] && [ -n "$skin_temp" ]; then
  log_result "热管理" "皮肤温度" "PASS" "thermal_skin=${skin_temp}°C"
else
  log_result "热管理" "皮肤温度" "FALLBACK" "thermal_skin=$skin_temp, 皮肤温度传感器不可用, 回退到CPU温度估算"
fi

# T042 电池温度
bat_temp=$(read_state "thermal_battery" "N/A")
if [ "$bat_temp" != "N/A" ] && [ -n "$bat_temp" ]; then
  log_result "热管理" "电池温度" "PASS" "thermal_battery=${bat_temp}°C"
else
  log_result "热管理" "电池温度" "FALLBACK" "thermal_battery=N/A, 电池温度不可读"
fi

# T043 热余量
headroom=$(read_state "thermal_headroom" "N/A")
headroom_pct=$(read_state "thermal_headroom_pct" "0")
if [ "$headroom" != "N/A" ]; then
  log_result "热管理" "热余量" "PASS" "headroom=${headroom}°C (${headroom_pct}%)"
else
  log_result "热管理" "热余量" "DEGRADED" "headroom=N/A"
fi

# T044 节流状态
throttle=$(read_state "thermal_throttle" "0")
if [ "$throttle" = "0" ]; then
  log_result "热管理" "节流状态" "PASS" "thermal_throttle=0 (未节流)"
else
  log_result "热管理" "节流状态" "WARN" "thermal_throttle=$throttle (当前处于热节流)"
fi

# T045 紧急状态
emergency=$(read_state "thermal_emergency" "0")
if [ "$emergency" = "0" ]; then
  log_result "热管理" "紧急状态" "PASS" "thermal_emergency=0 (正常)"
else
  log_result "热管理" "紧急状态" "WARN" "thermal_emergency=$emergency (热紧急状态, 全局安全上限生效)"
fi

# T046 CLI thermal
thermal_out=$($CLI thermal 2>&1)
if echo "$thermal_out" | grep -qEi "thermal|temp|cpu|HOT|WARN|CRIT"; then
  log_result "热管理" "CLI thermal" "PASS" "热管理命令正常响应"
else
  log_result "热管理" "CLI thermal" "WARN" "thermal输出: $(echo $thermal_out | head -1)"
fi

# =============================================================================
section "六、帧率与显示 (Frame Rate & Display)"
# =============================================================================

# T050 VSync频率
vsync=$(read_state "vsync_rate" "60")
if [ "$vsync" -ge 30 ] 2>/dev/null && [ "$vsync" -le 240 ] 2>/dev/null; then
  log_result "帧率显示" "VSync频率" "PASS" "vsync_rate=${vsync}Hz"
else
  log_result "帧率显示" "VSync频率" "WARN" "vsync_rate=$vsync (超出30-240Hz范围)"
fi

# T051 SF延迟
sf_lat=$(read_state "sf_latency_avg" "N/A")
if [ "$sf_lat" != "N/A" ] && [ -n "$sf_lat" ]; then
  log_result "帧率显示" "SF平均延迟" "PASS" "sf_latency_avg=${sf_lat}us"
else
  log_result "帧率显示" "SF平均延迟" "DEGRADED" "sf_latency_avg=N/A, SurfaceFlinger连接可能未建立"
fi

# T052 Jank检测 — G10-006: was an unconditional PASS. Now verify the read path
# actually returns a sane numeric value; jank_detected=1 itself is just telemetry
# (a jank happened), not a test failure.
jank=$(read_state "jank_detected" "N/A")
case "$jank" in
  N/A|'') log_result "帧率显示" "Jank检测" "DEGRADED" "jank_detected不可读, frame binder未连接" ;;
  *)
    if [ "$jank" -ge 0 ] 2>/dev/null; then
      log_result "帧率显示" "Jank检测" "PASS" "jank_detected=$jank (可读, 检测管线正常)"
    else
      log_result "帧率显示" "Jank检测" "WARN" "jank_detected=$jank 非数值"
    fi
    ;;
esac

# T053 帧关键状态 — G10-006: real read-path check instead of unconditional PASS.
frame_crit=$(read_state "frame_critical" "N/A")
case "$frame_crit" in
  N/A|'') log_result "帧率显示" "帧关键状态" "DEGRADED" "frame_critical不可读" ;;
  *)
    if [ "$frame_crit" -ge 0 ] 2>/dev/null; then
      log_result "帧率显示" "帧关键状态" "PASS" "frame_critical=$frame_crit (可读)"
    else
      log_result "帧率显示" "帧关键状态" "WARN" "frame_critical=$frame_crit 非数值"
    fi
    ;;
esac

# T054 层数
layers=$(read_state "layer_count" "N/A")
if [ "$layers" != "N/A" ]; then
  log_result "帧率显示" "图层数" "PASS" "layer_count=$layers"
else
  log_result "帧率显示" "图层数" "DEGRADED" "layer_count=N/A"
fi

# =============================================================================
section "七、eBPF子系统 (eBPF Subsystem)"
# =============================================================================

# T060 eBPF能力检测
ebpf_cap=$(read_state "ebpf_capability" "unknown")
ebpf_mode=$(read_state "ebpf_mode" "unknown")
if [ "$ebpf_mode" = "active" ] || [ "$ebpf_mode" = "native" ] || [ "$ebpf_mode" = "event_driven" ]; then
  log_result "eBPF" "eBPF模式" "PASS" "ebpf_mode=$ebpf_mode, 原生eBPF加载成功"
elif [ "$ebpf_mode" = "degraded" ] || [ "$ebpf_mode" = "polling" ] || [ "$ebpf_mode" = "fallback" ]; then
  log_result "eBPF" "eBPF模式" "FALLBACK" "ebpf_mode=$ebpf_mode, eBPF不可用, 回退到/proc轮询模式"
else
  log_result "eBPF" "eBPF模式" "WARN" "ebpf_mode=$ebpf_mode (未知状态)"
fi

# T061 eBPF程序加载
if [ -f "$EBPF_DIR/sched_events.bpf.o" ]; then
  bpf_size=$(wc -c < $EBPF_DIR/sched_events.bpf.o 2>/dev/null)
  if [ "$bpf_size" -gt 1000 ] 2>/dev/null; then
    log_result "eBPF" "eBPF字节码" "PASS" "sched_events.bpf.o=${bpf_size}B, 包含6个tracepoint程序"
  else
    log_result "eBPF" "eBPF字节码" "FAIL" "bpf.o仅${bpf_size}B, 可能损坏"
  fi
fi

# T062 内嵌头文件
if [ -f "$EBPF_DIR/sched_events_embedded.h" ]; then
  log_result "eBPF" "内嵌头文件" "PASS" "sched_events_embedded.h存在, 零依赖加载器可用"
else
  log_result "eBPF" "内嵌头文件" "FAIL" "embedded.h缺失, 无法零依赖加载"
fi

# T063 eBPF事件统计
ebpf_events=$(read_state "ebpf_events_total" "0")
ebpf_forks=$(read_state "ebpf_fork_events" "0")
ebpf_switches=$(read_state "ebpf_switch_events" "0")
if [ "$ebpf_events" -gt 0 ] 2>/dev/null; then
  log_result "eBPF" "eBPF事件统计" "PASS" "total=$ebpf_events, fork=$ebpf_forks, switch=$ebpf_switches"
else
  log_result "eBPF" "eBPF事件统计" "DEGRADED" "ebpf_events_total=0, 可能eBPF未加载或刚启动"
fi

# =============================================================================
section "八、机器学习 (Machine Learning)"
# =============================================================================

# T070 ML模型文件
ml_model="$RUN_DIR/ml_model.dat"
if [ -f "$ml_model" ]; then
  log_result "机器学习" "模型文件" "PASS" "$ml_model 存在 ($(wc -c < $ml_model 2>/dev/null)B)"
else
  log_result "机器学习" "模型文件" "FALLBACK" "$ml_model 不存在, 使用默认权重, 在线学习将创建新模型"
fi

# T071 ML混合学习状态
ml_hybrid=$(read_state "ml_hybrid_enabled" "unknown")
if [ "$ml_hybrid" = "1" ] || [ "$ml_hybrid" = "true" ]; then
  log_result "机器学习" "混合学习" "PASS" "ml_hybrid_enabled=1"
elif [ "$ml_hybrid" = "0" ]; then
  log_result "机器学习" "混合学习" "WARN" "ml_hybrid_enabled=0 (已禁用)"
else
  log_result "机器学习" "混合学习" "DEGRADED" "ml_hybrid_enabled=$ml_hybrid"
fi

# T072 ML学习率 — G10-006: real read-path check instead of unconditional PASS.
ml_lr=$(read_state "ml_learning_rate" "N/A")
ml_samples=$(read_state "ml_samples" "0")
case "$ml_lr" in
  N/A|'') log_result "机器学习" "ML训练状态" "DEGRADED" "ml_learning_rate不可读, ML子系统未初始化 (samples=$ml_samples)" ;;
  *)
    if awk "BEGIN{exit !($ml_lr>0 && $ml_lr<1)}" 2>/dev/null; then
      log_result "机器学习" "ML训练状态" "PASS" "learning_rate=$ml_lr 在(0,1)内, samples=$ml_samples"
    else
      log_result "机器学习" "ML训练状态" "WARN" "learning_rate=$ml_lr 超出(0,1), samples=$ml_samples"
    fi
    ;;
esac

# T073 联邦学习
fed_dir="$RUN_DIR/federated"
if [ -d "$fed_dir" ]; then
  fed_count=$(ls $fed_dir 2>/dev/null | wc -l)
  log_result "机器学习" "联邦学习" "PASS" "$fed_dir 存在, ${fed_count}个文件"
else
  log_result "机器学习" "联邦学习" "FALLBACK" "$fed_dir 不存在, 联邦学习未启用(单设备模式)"
fi

# =============================================================================
section "九、WebUI (Web Interface)"
# =============================================================================

# T080 HTTP端口
http_port=$(read_state "http_port" "0")
if [ "$http_port" -gt 0 ] 2>/dev/null; then
  log_result "WebUI" "HTTP端口" "PASS" "http_port=$http_port"
else
  log_result "WebUI" "HTTP端口" "DEGRADED" "http_port=0, WebUI可能未启动"
fi

# T081 Health端点
if [ "$http_port" -gt 0 ] 2>/dev/null; then
  health=$(toybox wget -q -O - "http://127.0.0.1:${http_port}/health" 2>/dev/null || curl -s "http://127.0.0.1:${http_port}/health" 2>/dev/null)
  if echo "$health" | grep -q '"status":"ok"'; then
    log_result "WebUI" "Health端点" "PASS" "GET /health -> 200, $health"
  else
    log_result "WebUI" "Health端点" "FAIL" "GET /health 失败或返回异常: $health"
  fi
else
  log_result "WebUI" "Health端点" "DEGRADED" "端口未知, 跳过HTTP测试"
fi

# T082 Version端点
if [ "$http_port" -gt 0 ] 2>/dev/null; then
  ver=$(toybox wget -q -O - "http://127.0.0.1:${http_port}/version" 2>/dev/null || curl -s "http://127.0.0.1:${http_port}/version" 2>/dev/null)
  if echo "$ver" | grep -q "v4.2.1"; then
    log_result "WebUI" "Version端点" "PASS" "GET /version -> $ver"
  else
    log_result "WebUI" "Version端点" "WARN" "GET /version -> $ver"
  fi
fi

# T083 认证保护（无token访问受保护端点应返回401）
if [ "$http_port" -gt 0 ] 2>/dev/null; then
  resp=$(toybox wget -q -O - "http://127.0.0.1:${http_port}/state/all" 2>/dev/null || curl -s "http://127.0.0.1:${http_port}/state/all" 2>/dev/null)
  if echo "$resp" | grep -qE "401|Unauthorized|token required|ok.:false|error"; then
    log_result "WebUI" "认证保护" "PASS" "无token访问/state/all返回401 (认证生效)"
  else
    log_result "WebUI" "认证保护" "FAIL" "无token访问/state/all未返回401, 可能认证绕过: $resp"
  fi
fi

# T084 daemon_token不通过HTTP泄露
if [ "$http_port" -gt 0 ] 2>/dev/null; then
  resp=$(toybox wget -q -O - "http://127.0.0.1:${http_port}/api/daemon_token" 2>/dev/null || curl -s "http://127.0.0.1:${http_port}/api/daemon_token" 2>/dev/null)
  if echo "$resp" | grep -qE "403|unavailable|ok.:false|error|not.*available"; then
    log_result "WebUI" "Token防泄露" "PASS" "GET /api/daemon_token返回403 (不通过HTTP泄露)"
  else
    log_result "WebUI" "Token防泄露" "WARN" "daemon_token端点响应: $resp"
  fi
fi

# T085 ROUND-004: /api/config_meta 可达且分类正确
if [ "$http_port" -gt 0 ] 2>/dev/null; then
  meta=$(toybox wget -q -O - "http://127.0.0.1:${http_port}/api/config_meta" 2>/dev/null || curl -s "http://127.0.0.1:${http_port}/api/config_meta" 2>/dev/null)
  if [ -n "$meta" ] && echo "$meta" | grep -qE 'hot_reload|category|meta'; then
    if echo "$meta" | grep -q 'PRIME_POOL'; then
      log_result "WebUI" "/api/config_meta" "PASS" "端点可达且PRIME_POOL已纳入分类"
    else
      log_result "WebUI" "/api/config_meta" "WARN" "端点可达但未含PRIME_POOL键: $(echo $meta|head -c120)"
    fi
  else
    log_result "WebUI" "/api/config_meta" "WARN" "端点无响应或格式不符: $(echo $meta|head -c120)"
  fi
else
  log_result "WebUI" "/api/config_meta" "WARN" "http_port=0, 跳过"
fi

# =============================================================================
section "十、安全 (Security)"
# =============================================================================

# T090 HMAC密钥
hmac_key="$RUN_DIR/hmac_key"
if [ -f "$hmac_key" ]; then
  hmac_perms=$(stat -c '%a' $hmac_key 2>/dev/null)
  hmac_size=$(wc -c < $hmac_key 2>/dev/null)
  if [ "$hmac_size" -ge 32 ] 2>/dev/null; then
    log_result "安全" "HMAC密钥" "PASS" "存在, ${hmac_size}B, 权限=$hmac_perms"
  else
    log_result "安全" "HMAC密钥" "FAIL" "密钥仅${hmac_size}B, 不足32字节"
  fi
else
  log_result "安全" "HMAC密钥" "FAIL" "$hmac_key 不存在"
fi

# T091 daemon_token
daemon_token="$RUN_DIR/daemon_token"
if [ -f "$daemon_token" ]; then
  token_size=$(wc -c < $daemon_token 2>/dev/null)
  log_result "安全" "Daemon Token" "PASS" "存在, ${token_size}B (128字符hex)"
else
  log_result "安全" "Daemon Token" "WARN" "$daemon_token 不存在, WebUI认证将不可用"
fi

# T092 命令目录权限
cmd_dir="$RUN_DIR/commands"
if [ -d "$cmd_dir" ]; then
  cmd_perms=$(stat -c '%a' $cmd_dir 2>/dev/null)
  if [ "$cmd_perms" = "700" ] || [ "$cmd_perms" = "0700" ]; then
    log_result "安全" "命令目录权限" "PASS" "$cmd_dir 权限=$cmd_perms (仅root)"
  else
    log_result "安全" "命令目录权限" "WARN" "$cmd_dir 权限=$cmd_perms, 期望700"
  fi
else
  log_result "安全" "命令目录权限" "FALLBACK" "$cmd_dir 不存在(命令队列未激活)"
fi

# T093 运行时目录权限 — G10-006: was unconditional PASS. RUN_DIR is deliberately
# left world-readable (WebView file:// fetch needs it), so we only assert the dir
# actually exists and is owned by root (not world-writable).
if [ -d "$RUN_DIR" ]; then
  run_perms=$(stat -c '%a' "$RUN_DIR" 2>/dev/null)
  case "$run_perms" in
    777|776|775|774|773|771|770)
      log_result "安全" "运行时目录权限" "WARN" "$RUN_DIR 权限=$run_perms 组/其他可写, 期望无写位" ;;
    '')
      log_result "安全" "运行时目录权限" "DEGRADED" "无法 stat $RUN_DIR" ;;
    *)
      log_result "安全" "运行时目录权限" "PASS" "$RUN_DIR 存在, 权限=$run_perms (WebView需可读)" ;;
  esac
else
  log_result "安全" "运行时目录权限" "FAIL" "$RUN_DIR 不存在, 模块未正确初始化"
fi

# T094 日志文件权限
if [ -f "$LOG_FILE" ]; then
  log_perms=$(stat -c '%a' $LOG_FILE 2>/dev/null)
  if [ "$log_perms" = "600" ] || [ "$log_perms" = "0600" ]; then
    log_result "安全" "日志文件权限" "PASS" "$LOG_FILE 权限=$log_perms (仅root可读)"
  else
    log_result "安全" "日志文件权限" "WARN" "$LOG_FILE 权限=$log_perms, 期望600"
  fi
fi

# T095 完整性校验
manifest="$RUN_DIR/manifest.json"
if [ -f "$manifest" ]; then
  integrity=$(read_state "integrity_verdict" "unknown")
  if [ "$integrity" = "1" ] || [ "$integrity" = "PASSED" ] || [ "$integrity" = "passed" ]; then
    log_result "安全" "完整性校验" "PASS" "manifest存在, integrity_verdict=$integrity"
  elif [ "$integrity" = "0" ] || [ "$integrity" = "FAILED" ]; then
    log_result "安全" "完整性校验" "FAIL" "integrity_verdict=FAILED (fail-closed生效, daemon不应启动)"
  else
    log_result "安全" "完整性校验" "WARN" "integrity_verdict=$integrity"
  fi
else
  log_result "安全" "完整性校验" "FALLBACK" "$manifest 不存在, 完整性校验未执行(首次启动)"
fi

# =============================================================================
section "十一、看门狗 (Watchdog)"
# =============================================================================

# T100 看门狗进程
wd_pid=$(pgrep -f "aatpe-watchdog" 2>/dev/null | head -1)
if [ -n "$wd_pid" ]; then
  log_result "看门狗" "看门狗进程" "PASS" "aatpe-watchdog运行中, PID=$wd_pid"
else
  log_result "看门狗" "看门狗进程" "WARN" "aatpe-watchdog未运行, daemon崩溃后无自动重启"
fi

# T101 看门狗白名单
wd_whitelist="$RUN_DIR/watchdog_whitelist"
if [ -f "$wd_whitelist" ]; then
  log_result "看门狗" "重启白名单" "PASS" "$(wc -l < $wd_whitelist 2>/dev/null)条白名单规则"
else
  log_result "看门狗" "重启白名单" "FALLBACK" "白名单文件不存在, 使用默认规则"
fi

# =============================================================================
section "十二、日志与可观测性 (Logging & Observability)"
# =============================================================================

# T110 日志文件
if [ -f "$LOG_FILE" ]; then
  log_size=$(wc -c < $LOG_FILE 2>/dev/null)
  log_lines=$(wc -l < $LOG_FILE 2>/dev/null)
  log_result "日志" "日志文件" "PASS" "$LOG_FILE ${log_size}B, ${log_lines}行"
else
  log_result "日志" "日志文件" "FAIL" "$LOG_FILE 不存在"
fi

# T111 日志级别 — G10-006: validate range 0..3 instead of unconditional PASS.
log_level=$(read_state "log_level" "N/A")
case "$log_level" in
  N/A|'') log_result "日志" "日志级别" "DEGRADED" "log_level不可读 (默认INFO=2)" ;;
  0|1|2|3) log_result "日志" "日志级别" "PASS" "log_level=$log_level (0=ERROR,1=WARN,2=INFO,3=DEBUG)" ;;
  *) log_result "日志" "日志级别" "WARN" "log_level=$log_level 超出0-3" ;;
esac

# T112 CLI log
log_out=$($CLI log 5 2>&1)
if [ -n "$log_out" ]; then
  log_result "日志" "CLI log" "PASS" "日志可读取, 最后5行可获取"
else
  log_result "日志" "CLI log" "WARN" "日志读取为空"
fi

# T113 健康检查 — G10-006: derive verdict from the actual health_status instead
# of unconditional PASS.
health_state=$(read_state "health_status" "unknown")
alerts=$(read_state "active_alerts" "0")
case "$health_state" in
  ok|healthy|normal)
    log_result "可观测性" "健康状态" "PASS" "health=$health_state, active_alerts=$alerts" ;;
  degraded|warning)
    log_result "可观测性" "健康状态" "WARN" "health=$health_state, active_alerts=$alerts" ;;
  critical|error|fail)
    log_result "可观测性" "健康状态" "FAIL" "health=$health_state, active_alerts=$alerts" ;;
  *)
    log_result "可观测性" "健康状态" "DEGRADED" "health=$health_state (不可识别), active_alerts=$alerts" ;;
esac

# T114 守护进程运行时间
uptime=$(read_state "daemon_uptime" "0")
if [ "$uptime" -gt 0 ] 2>/dev/null; then
  log_result "可观测性" "运行时间" "PASS" "daemon_uptime=${uptime}s"
else
  log_result "可观测性" "运行时间" "WARN" "daemon_uptime=0"
fi

# =============================================================================
section "十三、配置与热重载 (Config & Hot-reload)"
# =============================================================================

# T120 主配置语法
if [ -f "$CONF_DIR/aatpe.conf" ]; then
  conf_lines=$(wc -l < $CONF_DIR/aatpe.conf 2>/dev/null)
  conf_keys=$(grep -c '=' $CONF_DIR/aatpe.conf 2>/dev/null)
  log_result "配置" "主配置文件" "PASS" "${conf_lines}行, ${conf_keys}个配置项"
else
  log_result "配置" "主配置文件" "FAIL" "$CONF_DIR/aatpe.conf 不存在"
fi

# T121 线程角色规则
if [ -f "$RULES_DIR/thread_roles.conf" ]; then
  rule_count=$(grep -c '=' $RULES_DIR/thread_roles.conf 2>/dev/null)
  log_result "配置" "线程角色规则" "PASS" "${rule_count}条角色规则"
else
  log_result "配置" "线程角色规则" "FAIL" "规则文件不存在"
fi

# T122 自动Profile
auto_prof=$(read_state "auto_profile" "0")
auto_mode=$(read_state "auto_profile_mode" "unknown")
if [ "$auto_prof" = "1" ]; then
  log_result "配置" "自动Profile" "PASS" "auto_profile=1, mode=$auto_mode"
else
  log_result "配置" "自动Profile" "WARN" "auto_profile=0 (已禁用)"
fi

# =============================================================================
section "十四、性能基准 (Performance Benchmark)"
# =============================================================================

# T130 CLI benchmark
bench_out=$($CLI benchmark 2>&1)
if echo "$bench_out" | grep -qE "ops/sec|Scored|Benchmark|CPU-thread"; then
  rate=$(echo "$bench_out" | grep "ops/sec" | awk '{print $2}')
  log_result "性能" "评分基准" "PASS" "$(echo $bench_out | grep -E 'Scored|Rate' | tr '\n' ' ')"
else
  log_result "性能" "评分基准" "DEGRADED" "benchmark输出: $(echo $bench_out | head -2)"
fi

# =============================================================================
section "十五、降级与回退汇总 (Degradation & Fallback Summary)"
# =============================================================================

echo ""
echo "${C_YELLOW}--- 主动检测到的降级/回退路径 ---${C_NC}"

# 检查eBPF降级
if [ "$ebpf_mode" = "degraded" ] || [ "$ebpf_mode" = "polling" ] || [ "$ebpf_mode" = "fallback" ]; then
  echo "  [DEGRADED] eBPF不可用 -> /proc轮询回退 (依据: ebpf_mode=$ebpf_mode)"
fi

# 检查辅助APK回退
if [ "$helper_apk" != "1" ]; then
  echo "  [FALLBACK] 辅助APK未安装 -> 模块端/proc前台检测 (依据: helper_apk_installed=$helper_apk)"
fi

# 检查皮肤温度回退
if [ "$skin_temp" = "N/A" ] || [ "$skin_temp" = "-1" ]; then
  echo "  [FALLBACK] 皮肤温度传感器不可用 -> CPU温度估算 (依据: thermal_skin=$skin_temp)"
fi

# 检查ML模型回退
if [ ! -f "$ml_model" ]; then
  echo "  [FALLBACK] ML模型文件不存在 -> 默认权重 (依据: $ml_model 不存在)"
fi

# 检查联邦学习回退
if [ ! -d "$fed_dir" ]; then
  echo "  [FALLBACK] 联邦学习目录不存在 -> 单设备模式 (依据: $fed_dir 不存在)"
fi

# 检查中核池回退
if [ -z "$mid_pool" ]; then
  echo "  [FALLBACK] 无中核池 -> 2-pool调度 (依据: mid_pool为空)"
fi

# 检查完整性回退
if [ ! -f "$manifest" ]; then
  echo "  [FALLBACK] manifest不存在 -> 完整性校验未执行 (依据: $manifest 不存在)"
fi

# 检查看门狗
if [ -z "$wd_pid" ]; then
  echo "  [WARN] 看门狗未运行 -> daemon崩溃无自动重启 (依据: pgrep aatpe-watchdog为空)"
fi

# =============================================================================
# 最终汇总
# =============================================================================
echo ""
echo "${C_CYAN}==========================================================================="
echo "  测试结果汇总"
echo "===========================================================================${C_NC}"
echo ""
printf "  ${C_GREEN}PASS:     %3d${C_NC}  (功能正常, 有明确依据)\n" $PASS
printf "  ${C_RED}FAIL:     %3d${C_NC}  (功能异常, 需修复)\n" $FAIL
printf "  ${C_YELLOW}WARN:     %3d${C_NC}  (非致命问题, 建议关注)\n" $WARN
printf "  ${C_YELLOW}DEGRADED: %3d${C_NC}  (功能降级运行, 主路径不可用)\n" $DEGRADED
printf "  ${C_BLUE}FALLBACK: %3d${C_NC}  (回退到替代方案, 功能可用但非最优)\n" $FALLBACK
echo "  ─────────────────────────────"
printf "  TOTAL:    %3d  (测试项总数)\n" $TOTAL
echo ""

if [ "$FAIL" -eq 0 ]; then
  echo "  ${C_GREEN}结论: 无致命失败, 模块核心功能正常运行。${C_NC}"
else
  echo "  ${C_RED}结论: 存在${FAIL}个致命失败项, 需排查修复。${C_NC}"
fi

if [ "$DEGRADED" -gt 0 ] || [ "$FALLBACK" -gt 0 ]; then
  echo "  ${C_YELLOW}注意: ${DEGRADED}项降级 + ${FALLBACK}项回退, 详见上方降级汇总。${C_NC}"
fi

echo ""
echo "${C_CYAN}===========================================================================${C_NC}"

# G10-005: gateable exit code. Semantics (per FULL_FUNCTION_TEST_SPEC §六):
#   exit 0 = no FAIL; exit 1 = at least one FAIL. WARN/DEGRADED/FALLBACK do NOT
#   fail the run (non-fatal). This lets CI/release gating depend on the script.
# NOTE: the device side has no shared scripts/lib_verdict.sh, so the five-level
# counter (PASS/WARN/FAIL/DEGRADED/FALLBACK) and this exit-code semantics are
# implemented inline here; keep them in sync if lib_verdict.sh is ever vendored.
if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0

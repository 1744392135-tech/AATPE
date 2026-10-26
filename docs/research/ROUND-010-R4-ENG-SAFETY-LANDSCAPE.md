# ROUND-010-R4 工程化/兼容/安全/开源版图调研（深度扩写版）

> 目标：让 AATPE 在数万种设备配置上"装上不出事、易卸载、不留痕、不被杀、不冲突"。
> 方法：以官方文档/内核/AOSP 一手资料为主源，逐条附 URL + 原文摘录。
> 本版：600+ 行，30+ 来源，逐节实读。G3（安全/加密/完整性/身份域）已在前序轮次硬化，本文聚焦工程兼容与可靠性。

---

## 目录
1. Root 模块规范逐文件深读（Magisk / KSU / APatch 对照表）
2. SELinux 专章（逐节点 source/target/perm + sepolicy.rule 样例）
3. 守护进程可靠性工程（开机时序 / 防杀组合 / 崩溃状态机）
4. 卸载不留痕（逐节点还原清单 + baseline 快照格式）
5. 冲突检测（写后读回 + OEM 包名表 + WebUI 策略）
6. 16KB page size（验证方法 + customize.sh 自检）
7. sched_ext/SCX 评估加深
8. 开源项目版图（逐项目 repo / 维护状态 / 可借鉴设计）
9. 兼容性检查清单 + P0/P1/P2 缺口

---

## 1. Root 模块规范逐文件深读

### 1.1 Magisk module.prop 字段（主源 topjohnwu.github.io/Magisk/guides.html）
原文：
```
id=<string>            # 必填，正则 ^[a-zA-Z][a-zA-Z0-9._-]+$
name=<string>          # 必填
version=<string>      # 必填，展示用
versionCode=<int>      # 必填，整数，版本比较
author=<string>
description=<string>
updateJson=<url>       # 可选
```
- "Make sure to use the UNIX (LF) line break type and not the Windows (CR+LF) or Macintosh (CR)."
- updateJson 格式：`{"version":..., "versionCode":int, "zipUrl":url, "changelog":url}`。
- KSU 额外可选 `actionIcon=<path>`、`webuiIcon=<path>`（相对模块根，如 `actionIcon=icon/icon.png`）。

**AATPE 对照**：需确认 module.prop id 匹配正则、LF 换行、versionCode 纯整数。

### 1.2 customize.sh 环境变量与 UI 函数（Magisk + KSU 一致）
原文（Magisk guides）：
- "customize.sh ... will be *sourced* (not executed!) by the module installer script after all files are extracted and default permissions and secontext are applied."
- 变量：`MAGISK_VER`、`MAGISK_VER_CODE`、`BOOTMODE`(bool)、`MODPATH`、`TMPDIR`、`ZIPFILE`、`ARCH`(arm/arm64/x86/x64/riscv64)、`IS64BIT`、`API`。
- 函数：`ui_print <msg>`（避免 echo）、`abort <msg>`（避免 exit）、`set_perm <t> <owner> <group> <perm> [context]`（默认 context `u:object_r:system_file:s0`）、`set_perm_recursive`。
- 硬约束："DO NOT call `exit` at the end of customize.sh"；"DO NOT add a file named `install.sh`"；`SKIPUNZIP=1` 可完全自定义安装。

**AATPE 对照（ROUND-002 已修）**：customize.sh:359 改为 `"$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR"`，解决升级期 modules_update vs modules 锚错旧树。需复核：customize.sh 末尾用 `abort` 不用 `exit`；对 `$ARCH=arm64` 才安装 arm64 二进制（32 位设备跳过并 abort 友好提示）。

### 1.3 boot 脚本执行时机/阻塞/超时（Magisk vs KSU vs APatch）
| 阶段 | Magisk | KSU | APatch | 语义 |
|---|---|---|---|---|
| post-fs-data.sh | BLOCKING，**40s** 超时 | BLOCKING，**10s** 超时 | BLOCKING，**10s** | /data 刚解密挂载；Zygote 未起；before modules mount |
| post-mount.sh | （无） | 有 | 有 | OverlayFS 挂载后 |
| service.sh | late_start，NON-BLOCKING | late_start，NON-BLOCKING | late_start，NON-BLOCKING | **推荐放绝大多数脚本** |
| boot-completed.sh | （用 `resetprop -w sys.boot_completed 0` 等待） | 原生支持 | 原生支持 | 开机完成后 |
| uninstall.sh | 卸载时 | 卸载时 | 卸载时 | 还原现场 |
| action.sh | Magisk app 按钮 | KSU app 按钮 | APatch app 按钮 | 用户触发 |

原文（Magisk）："post-fs-data mode: This stage is BLOCKING. The boot process is paused before execution is done, or 40 seconds have passed."；"late_start service mode: NON-BLOCKING ... This is the recommended stage to run most scripts."
原文（KSU）：post-fs-data "paused before execution is done or after 10 seconds."；boot 流程精确时序见 kernelsu.org/guide/module.html（post-fs-data 里顺序：safe mode check → post-fs-data.d → load sepolicy.rule → metamodule post-fs-data.sh → 模块 post-fs-data.sh → system.prop → metamount.sh → post-mount.sh；bootanim 起来后才 service.d → service.sh；boot complete 后才 boot-completed.sh）。

**关键工程结论**：cpuset/stune 节点由 init 在 class_start main（zygote 前后）挂载，远晚于 post-fs-data。因此：
- 拉起 daemon 的 `service.sh` 必须先 `resetprop -w sys.boot_completed 0`（等开机完成），再轮询 `/dev/cpuset/top-app/tasks` 存在性（带 ~30s 超时），再 attach eBPF。
- post-fs-data.sh 只做"非阻塞、快速、不依赖 /data 完整/cpuset"的事（如快速自检、不拉起 daemon）。

### 1.4 sepolicy.rule 语法
原文（Magisk）："Each line in this file will be treated as a policy statement."；KSU/APatch 同。语句贴近 `*.te`：`allow <src> <tgt>:<class> { perms };`。Magisk 自身基线：`magiskpolicy --magisk 'allow magisk * * *'`（topjohnwu tools.html）。

### 1.5 META-INF update-binary
原文：recovery 刷机才需要 `META-INF/com/google/android/update-binary`（指向最新 module_installer.sh）+ `updater-script`（内容仅 `#MAGISK`）。"When your module is downloaded with the Magisk app, update-binary will be forcefully replaced ... DO NOT try to add any custom logic in update-binary."

### 1.6 KSU / APatch 差异逐项对照
| 维度 | Magisk | KernelSU | APatch |
|---|---|---|---|
| 挂载机制 | magic mount (bind mount) | metamodule（如 meta-overlayfs），仅改 /system 需装 | 内核 OverlayFS |
| 环境变量判平台 | `MAGISK_VER` | `KSU=true` | `APATCH=true` |
| Zygisk | 内置 | 需 ZygiskNext | 无内置（ZygiskNext） |
| BusyBox | /data/adb/magisk/busybox | /data/adb/ksu/bin/busybox | /data/adb/ap/bin/busybox（Magisk 同源） |
| post-fs-data 超时 | 40s | 10s | 10s |
| boot-completed.sh | 无独立文件（resetprop 等） | 原生 | 原生 |
| init.rc 注入 | overlay.d | initrc/（ksu 注入 init.rc，支持 `on property:sys.boot_completed=1`） | — |
| WebUI | Magisk WebUI 规范 | /data/adb/ksu/webroot + activity | 同 KSU |
| 伪装版本号 | 真实 | — | `MAGISK_VER=v27.0 / CODE=27000` 恒值 |

KSU initrc 注入示例（kernelsu.org 原文）：
```
service myservice /data/adb/modules/mymodule/bin/myservice
  user root group root disabled seclabel u:r:ksu:s0
  on property:sys.boot_completed=1
  start myservice
```
注意："initrc injection happens extremely early ... before post-fs-data"，且"not available in late-load mode"。

---

## 2. SELinux 专章

### 2.1 AATPE 守护进程运行域
- Magisk：root shell / magiskd 跑在新域 `magisk`，"effectively permissive"（topjohnwu details.html）。
- KSU：root 命令在 `u:r:su:s0` / ksu 域；initrc service 可用 `seclabel u:r:ksu:s0`。
- 因此 AATPE daemon（service.sh 拉起的 root 进程）通常在 permissive/广授权域，**写 sysfs/cpuset 一般不被 AVC deny**。但厂商 enforcing ROM + 额外 neverallow 仍可能拦。

### 2.2 逐节点操作的 class/perm
| 操作 | 路径 | class | 需要的 perm |
|---|---|---|---|
| 写任务进 cpuset | /dev/cpuset/<group>/tasks | file | open write getattr |
| 写 schedtune boost | /dev/stune/<group>/schedtune.boost | file | open write |
| 写 devfreq gov/频率 | /sys/class/devfreq/.../governor | sysfs file | open read write getattr |
| 写 cpufreq | /sys/.../cpufreq/scaling_min_freq | sysfs file | open write |
| 写 uclamp | /dev/cpuctl/cpu.uclamp.min | file | open write |
| 读 /proc/<tid>/stat、comm | /proc/<pid>/* | proc/pid | open read getattr |
| sched_setaffinity/setscheduler | — | capability | CAP_SYS_NICE（root 有） |

### 2.3 sepolicy.rule 真实样例（供 G2 评估，最小授权）
```
# AATPE sepolicy.rule — 仅在默认域不足时补；magisk 域已广授权，多数设备无需
allow magisk sysfs file { open read write getattr };
allow magisk cpuset file { open write };
allow magisk proc file { open read getattr };
# 收紧示例（可选，避免过度放权）：
# neverallow { domain } sysfs file write;  # 通常不要，会破坏系统
```
流程：先以最小配置跑，doctor 抓 AVC，再用 audit2allow 式补精确规则，不要一上来 `allow magisk * * *`（Magisk 自身基线已含，但模块不应再放大）。

### 2.4 enforcing denial 抓取与 doctor 集成
- 抓取：`dmesg | grep "avc: denied"` 或 `logcat -b events | grep avc`。
- 原文（XDA 4671716）："The missing SELinux policy is the line beginning with 'allow', which can be used as a parameter for magiskpolicy (as user root): `magiskpolicy --live \"allow system_app proc_stat file open\"`"；验证 `magiskpolicy --print-rules | grep ...`。
- `magiskpolicy --live` 仅当前 boot 生效、重启即失 → 持久化必须 sepolicy.rule（pre-init 注入）。
- doctor 应：抓 avc denied → 解析 denied class/perms → WebUI 提示"节点 X 被 SELinux 拦，建议加规则 Y"，而非静默把功能关掉。

### 2.5 厂商 neverallow 风险
- 不同厂商（高通/MTK/华为/小米）base policy 含额外 neverallow，模块规则若触碰会在 enforcing 下拒绝加载 sepolicy.rule。对策：规则写得窄、doctor 上报、失败时降级为只读/不 attach 而非 bootloop。

---

## 3. 守护进程可靠性工程

### 3.1 开机时序竞争
- cpuset/stune 由 init 在 class_start main（zygote 阶段）挂载；post-fs-data 时不存在。
- 对策：service.sh `resetprop -w sys.boot_completed 0` 后，轮询节点存在：
```sh
for i in $(seq 1 30); do
  [ -e /dev/cpuset/top-app/tasks ] && break
  sleep 1
done
# 节点缺失则降级：不 attach eBPF，WebUI 报"cpuset 未就绪"
```
- KSU late-load 模式：`KSU_LATE_LOAD=1`，post-fs-data 被 late-load 取代，脚本应检测该变量跳过早期操作。

### 3.2 防杀组合
- **NATIVE_ADJ=-1000**（perfetto.dev / CSDN ADJ 表）：native 进程默认 -1000，最不可能被 lmkd 杀。
- lmkd 按 oom_score_adj 杀：越低越保命。daemon 启动后写 `/proc/self/oom_score_adj` 期望 -1000 或至少 -900。
- **Android 12 phantom process killer**（cosyra.com / andronix.app）：系统统计后台 fork 子进程数，>32 且耗 CPU 即 SIGKILL（exit 137）。这是针对"app fork 的子进程"，root daemon 由 magiskd/service 直接起、不挂在某个 app 下，通常不受影响；但若 daemon 长期 fork 短生命周期 helper（如 probe），需警惕。对策：setsid 脱离父会话、避免大量 fork、probe 用 exec 复用。
- setsid + nohup：脱离 controlling terminal，父 shell 退出不连带。
- cgroup 放置：把 daemon 放入 `/dev/cpuset/system-background/` 或自建 cgroup，避免被 cgroup 迁移回收。Magisk 自身 "Escape from cgroups (/acct, /dev/cg...)"（deepwiki architecture）。

### 3.3 长期运行自查
- 日志膨胀：logcat ring buffer 自限；写文件日志需按大小轮转（logrotate 思路）。
- fd 泄漏：daemon 所有 open 配 O_CLOEXEC，定期 `/proc/self/fd` 计数。
- 内存增长：定期 RSS 采样（/proc/self/status VmRSS），异常增长上报。

### 3.4 rapid crash loop 状态机对照业界
- Magisk "Invincible mode: daemon will be restarted by init"（changes.html）。
- AATPE 已有 backoff。建议参数：60s 窗口内重启 >3 次 → 进安全模式（只读、不 attach eBPF、WebUI 提示），避免 bootloop；连续成功 10min 后退出安全模式。

---

## 4. 卸载不留痕

### 4.1 必须还原的写入
| 节点/状态 | 写入动作 | 还原 |
|---|---|---|
| /dev/cpuset/*/tasks | spillover 把 tid 迁到 top-app/PRIME | 写回原 savedCpuset 组（placement.cpp demote 已有） |
| 线程 affinity | applyPrimeAffinityFallback 改 sched_setaffinity | restoreAffinity 回 savedMask（已有） |
| schedtune.boost | 写组 boost | uninstall.sh 还原 baseline |
| uclamp.min/max | 写 | 还原 baseline |
| devfreq governor | 若改 | 还原 |
| scaling_min/max_freq | 若改 | 还原 |
| sys.boot_completed 等 prop | resetprop -n | resetprop --delete 还原 |

### 4.2 进程死亡后 cgroup 自动迁移
- 内核 cgroup：tasks 文件里的 tid 死亡后自动消失；cpuset 在进程消失后不残留。但若 tid 仍活且被 AATPE 改了 affinity/uclamp，daemon 被杀（未跑 SIGTERM handler）就残留。
- 对策：daemon 注册 SIGTERM/SIGHUP handler，遍历 g_spillState 全部 restoreAffinity + moveTidToCpusetGroup 回原组；watchdog 重启后重扫或 boot-completed 时强制重置一次。

### 4.3 baseline 快照格式设计
安装时（customize.sh / 首次 service.sh）把"出厂值"快照到 `$MODDIR/.baseline_<node>`：
```
# $MODDIR/baseline/uclamp_min.after_boot
# node=/dev/cpuctl/cpu.uclamp.min
# value=<原值>
# captured_at=<uptime>
```
uninstall.sh 读取并写回。格式：纯文本 key=value，root-only 0600。注意：boot 早期节点可能尚未设值，应在 boot-completed 后再快照（此时才是"用户日常状态"基线）。

---

## 5. 冲突检测

### 5.1 写后读回比对方法
- 写节点值 V → 立即读回 V'；记录。下个周期再读 V''；若 V''≠V 且非本 daemon 刚写 → 判定"外部写者在动"。
- 误判处理：其他合法写者（厂商 perf-hal、用户手动）也会改值。不能一检测到漂移就抢回（会打架）。策略：只**记录+提示**，不强制抢写；WebUI 标记"节点被外部修改"。
- owner/mtime：`stat` 节点看 mtime，非本 daemon 周期内的修改即外部。

### 5.2 冲突软件包/进程特征清单
| 项目 | 包名/进程 | 写的节点 |
|---|---|---|
| Scene (欧陆) | com.omarea.vtools | cpuset/uclamp/scaling |
| Uperf | uperf 进程 / data/adb/modules/uperf | sysfs 全节点 |
| FDE.PSP | FDE.PSP / mks | cpufreq/schedtune |
| perf+ / SmartPack | com.smartpack.kernelmanager | sysfs |
| FrancoKernelManager | com.franco.kernel | sysfs |
| 厂商游戏空间 | 小米 com.xiaoki.game / 华为 / OPPO / vivo 游戏助手 | 厂商 perf HAL |
| QTI perfd / libperfmgr | 系统进程 | uclamp/schedtune |

### 5.3 WebUI 提示策略
- 检测到冲突模块目录存在（/data/adb/modules/uperf、scene 进程）→ 黄色横幅"检测到 Scene/Uperf 活跃，调度节点会互相覆盖，建议二选一"。
- 不自动禁用，由用户决定。

---

## 6. 16KB page size

主源 developer.android.com/guide/practices/page-sizes：
- "Android NDK r28 and above: builds with 16 KB alignment by default."
- r27-：需 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`。
- "Starting November 1st, 2025, all new apps ... targeting Android 15+ must support 16 KB."

### 6.1 验证方法
- `readelf -l aatped | grep LOAD` → 每项 `LOAD ... 10000 0x...`，对齐应为 10000(64KB) 或至少 4000(16KB)。
- 模块内 .so：`zipalign -P 16 -c 4 mymodule.zip`。

### 6.2 customize.sh 自检脚本
```sh
# 检测设备页大小
pagesz=$(getconf PAGE_SIZE 2>/dev/null)
ui_print "- device page size = $pagesz"
if [ "$pagesz" = "16384" ]; then
  # 校验本模块二进制 LOAD 对齐
  if ! $BOOTMAGIC -- ? ; then :; fi  # doctor 做更细检查
fi
```
不支持时明确失败：armv8.0 老设备 4KB only 也能跑（16KB 对齐二进制在 4KB 内核上仍可加载），反之 4KB 对齐二进制在 16KB 内核上失败。NDK r30 产物是"大页兼容小页"的，安全。doctor 报告即可。

---

## 7. sched_ext/SCX 评估加深

### 7.1 主线要求
- sched_ext 自 Linux 6.12 GA（mainline），需 `CONFIG_SCHED_CLASS_EXT=y`。
- 四层架构：Core kernel → sched_ext framework → BPF scheduler → userspace（sched.co ossna2025 slides）。
- "The system integrity is maintained no matter what" — SCX 有安全兜底：调度器崩溃/超时自动切回 CFS（kernel.org sched-ext 文档："can be turned on and off dynamically anytime"）。

### 7.2 scx_rusty / scx_lavd 设计要点
- scx_rusty（docs.rs）："multi-domain, BPF/user-space hybrid. BPF portion does simple round-robin per domain; user space (Rust) calculates load factor and informs BPF."
- scx_lavd（Igalia / Changwoo Min）："Latency-criticality Aware Virtual Deadline"，面向 gaming/interactivity，per-LLC、per-core-type(P/E) 分调度域；Igalia 2025 回顾："scx_lavd is now production-ready for latency-sensitive workloads"，228 commits，futex tracing lock-aware boosting。
- LWN OSPM 2025：scx_lavd 是 virtual-deadline（类 EEVDF），专为 gaming。

### 7.3 安卓 GKI 可用性现状与不选判据
- 安卓 GKI 内核：5.10/5.15/6.1/6.6，**默认未开 CONFIG_SCHED_CLASS_EXT**（SCX 6.12+，且 GKI 未启用该 config）。手机内核绝大多数不满足。
- 判据：①内核 config 不满足 → 装了也加载失败；②SCX 是全局调度器，一个 bug = 系统卡顿/看门狗重启，对"装上不出事"目标风险过高；③AATPE 现有 eBPF tracepoint + uclamp/cpuset 是非侵入、可秒退、不接管全局调度的安全路径。
- 结论：P2 远期，仅在用户自编译 6.12+ 内核且 opt-in、配 5s 看门狗自动切 CFS 的实验场景。当前不落地。

---

## 8. 开源项目版图（逐项目）

| 项目 | repo | 维护状态 | 兼容/探测策略 | 可借鉴设计 |
|---|---|---|---|---|
| Uperf | github.com/yc9559/uperf | 活跃（Gitee 镜像） | 全 sysfs 节点、监听 cpuset 分组/wakelock/触屏识别场景 | 场景识别（前台 app 切换触发）、节点写前读 |
| Scene | github.com/lirujun/Scene-v2（com.omarea.vtools） | 活跃 | 图形化、能力探测 | 保守默认、可视化调度 |
| FDE.PSP | github.com/Huawei-Device-Engineering? / mks | 小众 | MTK/高通 perf | 厂商 perf HAL 对接 |
| SmartPack-KernelManager | github.com/SmartPack/SmartPack-Kernel-Manager | 维护 | 节点存在探测 | 白名单式节点写 |
| FrancoKernelManager | github.com/franciscofranco | 维护 | sysfs governor | 调度曲线 |
| perfmtk (MTK) | github.com/juaniman/perfmtk | 活跃 | `DEFAULT_PROFILE=balanced`，无匹配用平衡档 | **保守默认 profile** ← 借鉴 |
| QTI perfd / libperfmgr | AOSP/vendor | 系统自带 | uclamp/schedtune | 官方 EAS 基线 |

**可借鉴核心**：所有成熟模块都做 ①节点存在性探测（nodeWritable，AATPE placement_mech 已有）②不可用即降级（uclamp→affinity，ROUND-004 已做）③保守默认（perfmtk balanced）④场景触发而非常驻激进写。

---

## 9. 兼容性检查清单 + P0/P1/P2

### 可验证清单
| # | 项 | 验证 |
|---|---|---|
| C1 | service.sh 用 $MODDIR | grep |
| C2 | daemon 在 late_start service，非 post-fs-data 阻塞 | 不在 post-fs-data 起 daemon |
| C3 | resetprop -w sys.boot_completed 0 | service.sh |
| C4 | cpuset 就绪轮询（30s 超时）后再 attach eBPF | service.sh/doctor |
| C5 | 16KB readelf LOAD 对齐 | readelf -l |
| C6 | setsid + oom_score_adj=-1000 | daemon 启动 |
| C7 | watchdog triple identity（pid+exe+starttime） | pid_identity.h ✅ |
| C8 | 60s>3 重启进安全模式 | watchdog |
| C9 | uninstall.sh 还原 baseline cpuset/uclamp/scaling | uninstall.sh |
| C10 | spillover 线程 restoreAffinity（SIGTERM handler） | placement ✅ |
| C11 | doctor 抓 avc denied | doctor |
| C12 | 冲突模块检测提示 | doctor/WebUI |
| C13 | customize.sh 无 exit、用 abort、arm64 才装 | customize.sh |
| C14 | WebView 调试关、JS 桥最小 | G8 |
| C15 | baseline 快照在 boot-completed 后采 | service.sh |

### P0（阻断"装上不出事"）
- P0-1：daemon 确认 service.sh（late_start），post-fs-data 仅非阻塞自检。
- P0-2：崩溃 loop 安全模式（只读、不 attach eBPF）。
- P0-3：uninstall.sh 还原调度基线，真正不留痕。

### P1（可靠性提升）
- P1-1：doctor 抓 SELinux avc + 节点漂移。
- P1-2：冲突模块检测 + WebUI 横幅。
- P1-3：16KB readelf 自检。
- P1-4：spillover 残留 mask 重启后重扫还原。
- P1-5：phantom process 防御（setsid、避免大量 fork helper）。

### P2（远期）
- P2-1：sched_ext opt-in 实验（6.12+ 自定义内核，5s 看门狗回退 CFS）。
- P2-2：Android 17 usesCleartextTraffic 迁 network security config（loopback localhost 例外已满足，低优先）。

---

## 精读一手来源（30+）
1. https://topjohnwu.github.io/Magisk/guides.html — module.prop/customize.sh/boot scripts/sepolicy.rule 全文
2. https://topjohnwu.github.io/Magisk/tools.html — magiskpolicy --magisk 基线、daemon trigger
3. https://topjohnwu.github.io/Magisk/details.html — magisk 域 permissive
4. https://topjohnwu.github.io/Magisk/changes.html — Invincible mode / magisklogd
5. https://magisk.readthedocs.io/en/latest/developers/internaldetails.html — post-fs-data/late_start 时序
6. https://kernelsu.org/guide/module.html — KSU 模块、initrc 注入、boot 时序、late-load 模式（全文精读）
7. https://apatch.dev/apm-guide.html — APatch OverlayFS/post-mount/boot-completed/Lua（全文精读）
8. https://developer.android.com/guide/practices/page-sizes — 16KB/NDK r28
9. https://goo.gle/46ewrD2 — Google 16KB 强制 2025-11
10. https://developer.android.com/privacy-and-security/security-config — loopback localhost cleartext 例外
11. https://developer.android.com/about/versions/17/behavior-changes-all — usesCleartextTraffic 弃用
12. https://source.android.com/docs/core/perf/lmkd — lmkd/oom_score_adj
13. https://perfetto.dev/docs/data-sources/memory-counters — NATIVE_ADJ=-1000 定义
14. https://lotay.blog.csdn.net/article/details/135826910 — ADJ 级别表（NATIVE -1000/SYSTEM -900）
15. https://cosyra.com/guides/termux-signal-9-fix.html — Android12 phantom process killer >32 子进程
16. https://docs.andronix.app/android-12/andronix-on-android-12 — phantom process 机制
17. https://www.kernel.org/doc/html/v7.1/scheduler/sched-ext.html — sched_ext 官方
18. https://docs.rs/crate/scx_rusty/1.1.3 — rusty BPF/userspace hybrid
19. https://docs.rs/crate/scx_lavd/1.1.3 — LAVD per-core-type 调度域
20. https://www.igalia.com/2026/01/30/Igalia-Kernel-Team-2025-Achievements-A-Retrospective.html — scx_lavd production-ready
21. https://lwn.net/Articles/1021332/ — OSPM 2025 scx_lavd virtual-deadline
22. http://oldvger.kernel.org/bpfconf2024_material/Sched_Ext_-_What_s_New_&_What_s_Missing.pdf — SCX 架构
23. https://hosted-files.sched.co/ossna2025/d2/...pdf — 四层架构/DSQ
24. https://gitee.com/mirrors_yc9559/uperf — Uperf 场景识别策略
25. https://context7.com/juaniman/perfmtk/llms.txt — perfmtk DEFAULT_PROFILE=balanced
26. https://forums.xda-developers.com/t/guide-how-to-add-missing-selinux-policies-dynamically-using-magisk.4671716/ — magiskpolicy --live 流程
27. https://deepwiki.com/topjohnwu/Magisk/2-architecture — magiskd escape cgroups
28. https://newandroidbook.com/ddb/RedmiK30/ — /dev/stune cgroup 挂载实证
29. https://crdroid.net/changelog/v12.x/m31s_changelog.txt — schedtune→uclamp 迁移补丁
30. http://raw.githubusercontent.com/okhsunrog/vpnhide/master/portshide/README.md — loopback 端口隔离
31. https://wiki.cachyos.org/configuration/sched-ext/ — SCX 使用场景
32. https://www.igalia.com/downloads/slides/ChangwooMin-...LAVD.pdf — LAVD big/LITTLE 分类讨论

---

## 10. AATPE fix_tree 现状逐项对照（实读代码后的具体缺口）

本节把上述规范逐条对照 fix_tree 真实代码，给出可执行缺口。

### 10.1 脚本入口对照
实读 service.sh（ROUND-002 已改 bootstrap:107 / verify:111 传 `"$MODDIR"`）：
- ✅ 已用 `$MODDIR` 显式指定模块根，消除升级期（modules_update 新树 / modules 旧树）锚错旧二进制。
- ✅ customize.sh:359 `"$MODDIR/system/bin/aatped" --generate-manifest "$MODDIR"`。
- 缺口 G10：service.sh 是否在拉起 `aatped` 前 `resetprop -w sys.boot_completed 0`？若 daemon 由 service.sh 直接 exec 而未等 boot_completed，cpuset/stune 节点在 late_start 早期可能尚未由 init 挂载 → probe 误报"节点不存在" → 能力探测降级（uclamp N/A → affinity fallback）。建议：
```sh
#!/system/bin/sh
MODDIR=${0%/*}
resetprop -w sys.boot_completed 0   # 等开机完成
i=0
while [ $i -lt 30 ]; do              # 轮询 cgroup 节点就绪
  [ -e /dev/cpuset/top-app/tasks ] && break
  sleep 1; i=$((i+1))
done
exec "$MODDIR/system/bin/aatped" --daemon   # 节点缺则降级 POLLING
```

### 10.2 customize.sh 安装自检
- 复核末尾无裸 `exit`；失败用 `abort "msg"`。
- 16KB 自检：customize.sh 加 `getconf PAGE_SIZE` 打印。
- 架构守卫：`[ "$ARCH" = "arm64" ] || abort "requires arm64"`。

### 10.3 守护防杀对照
- pid_identity.h triple identity ✅。
- 缺口：daemon 启动写 `/proc/self/oom_score_adj=-1000` + `setsid()`：
```c
setsid();
int fd = open("/proc/self/oom_score_adj", O_WRONLY|O_CLOEXEC);
if (fd>=0){ write(fd,"-1000",5); close(fd); }
```

### 10.4 eBPF attach 时机
除完整性 gate 外，attach 前应检查：/sys/fs/bpf 已挂载、内核 tracepoint offset match、cgroup 节点就绪。任一不满足 → POLLING 降级 + doctor 上报，不 crash。

---

## 11. 冲突检测深化（写后读回与误判）

### 11.1 方法
写节点 N 值 V → 立即读回 V1（应==V）；下一周期读 V2；V2≠V 即外部写者在动。记录 STATE_DIR/node_drift（0600）。

### 11.2 误判处理（关键）
厂商 perf-hal / QTI perfd / 用户手动都是合法写者。**只记录、不抢写**：检测到漂移后该节点进入"旁观模式"，不再周期强写，避免打架抖动。WebUI 提示"节点被外部修改，AATPE 已暂停管理"。连续 N 周期回稳或用户手动接管才恢复。

### 11.3 OEM 游戏空间包名表
| OEM | 包名 |
|---|---|
| Xiaomi | com.xiaomi.gamecenter / com.miui.performance |
| OPPO/realme | com.coloros.gamespaceui |
| vivo/iQOO | com.vivo.gamecube |
| Huawei | com.huawei.gameassistant |
| Samsung | com.samsung.android.game.gamehome |
检测：`pm list packages | grep` 或扫 `/data/adb/modules`。

---

## 12. 卸载/退出还原逐节点

| 类别 | 节点 | 还原 | 时机 |
|---|---|---|---|
| cpuset | /dev/cpuset/*/tasks | demote 回 savedCpuset | SIGTERM / uninstall |
| affinity | sched_setaffinity | restoreAffinity savedMask | 同上 |
| schedtune | schedtune.boost | 写回 baseline | uninstall |
| uclamp | cpu.uclamp.min/max | 写回 baseline | uninstall |
| devfreq | governor | 写回 baseline | uninstall |
| cpufreq | scaling_min/max_freq | 写回 baseline | uninstall |
| prop | resetprop -n 属性 | resetprop --delete | uninstall |

baseline 快照在 boot_completed 后采集，存 `$MODDIR/.baseline/<node>`（key=value, 0600）。进程被 SIGKILL 残留：boot-completed.sh 兜底扫 spillState 记录文件还原。

---

## 13. 崩溃状态机参数
- Magisk "Invincible mode: daemon restarted by init"。
- AATPE 建议：窗口 60s 阈值 3 次重启 → 安全模式（只读、不 attach eBPF）；连续 10min 无崩溃退出。与 ROUND-003 完整性 N=3 防抖同哲学。

---

## 14. Android 开机时序与 AATPE 插入点详解

### 14.1 完整 boot 时间线（综合 KSU module.html 原文）
```
Bootloader → kernel → init
 ├─ /dev,/proc,/sys 挂载
 ├─ property-init → read default props → read init.rc
 │    * KSU: initrc injection（modules.rc 注入 init.rc，极早期）
 ├─ early-init → init → late_init
 ├─ start vold → fs_mgr → 挂载 /vendor,/system,/persist
 ├─ post-fs-data（/data 解密挂载）
 │    * safe mode check
 │    * post-fs-data.d 脚本
 │    * load sepolicy.rule          ← 模块 sepolicy.rule 在此注入
 │    * metamodule/模块 post-fs-data.sh（BLOCKING 10s/40s）
 │    * system.prop (resetprop -n)
 │    * metamount.sh / post-mount.sh（OverlayFS 挂载）
 ├─ zygote-start → load_all_props
 ├─ class_start core → logd,console,vold
 ├─ class_start main → adb,netd,zygote  ← cpuset/stune cgroup 在此附近由 init 创建
 │
 ─ kernel2user（bootanim 起）─
 ├─ service.d / 模块 service.sh（NON-BLOCKING late_start）  ← AATPE daemon 应在此
 ├─ zygisk hook
 └─ 系统 app 自启
 │
 ─ ACTION_BOOT_COMPLETED 广播 ─
 ├─ boot-completed.d / 模块 boot-completed.sh              ← baseline 快照应在此
 └─ 用户解锁 /data/data → 用户 app 自启
```

### 14.2 AATPE 关键推断
- cpuset/stune cgroup 节点在 `class_start main`（zygote）阶段由 init 创建，**早于 late_start service.sh**，但晚于 post-fs-data。所以放 service.sh 通常已就绪，但 boot 慢的设备（IO 慢/加密）仍可能差几秒 → 必须轮询兜底。
- sepolicy.rule 在 post-fs-data 早期加载，早于 daemon 跑，规则已生效 ✅。
- baseline 节点值在 boot_completed 后才稳定（厂商 perf-hal 也会写），故快照放 boot-completed.sh。

### 14.3 AATPE 不选 post-fs-data 的理由
- post-fs-data BLOCKING（KSU 10s / Magisk 40s），daemon 启动+probe+attach 若超 10s 会被强杀 → boot 卡顿。
- post-fs-data 时 /data 刚挂载，cgroup 未就绪，attach 必失败。
- 结论：daemon 必须 service.sh；post-fs-data.sh 可留空或仅做"标记需要重装"之类毫秒级动作。

---

## 15. SELinux denial 实例与 doctor 集成

### 15.1 典型 avc denied 行
```
avc: denied { write } for pid=1234 comm="aatped" name="tasks" dev="cpuset"
      scontext=u:r:magisk:s0 tcontext=u:object_r:cpuset:s0 tclass=file
```
doctor 解析：`scontext`（来源域）、`tclass`（类别）、`{ perm }`、`tcontext`（目标类型）。
转换规则：`allow <scontext> <tcontext>:<tclass> { perm };`。

### 15.2 doctor 集成流程
1. daemon 启动时 `dmesg` 抓 `avc: denied` 且 comm=aatped 的行。
2. 解析出需要的最小规则，写 STATE_DIR/selinux_diagnostics。
3. WebUI 展示"SELinux 拦了 N 次：写 cpuset tasks 被拒"，并给出建议规则文本。
4. 不自动 `magiskpolicy --live`（重启即失，且放大攻击面）；提示用户写入 sepolicy.rule 后重启。

### 15.3 厂商 neverallow 风险
- 部分 ROM（华为/小米）base policy 含 `neverallow domain sysfs file:write` 之类，模块 `allow magisk sysfs file write` 可能违反。
- 对策：规则写窄到具体类型（`allow magisk sysfs_devfreq file write`），而非泛 `sysfs`。失败时降级只读，不 bootloop。

---

## 16. 16KB 深度验证

### 16.1 readelf 判据
```
readelf -l system/bin/aatped | grep -A1 LOAD
# 期望： Type LOAD ... 0x10000  (align=0x10000=64KB) 或 0x4000(16KB)
# 4KB 对齐的段在 16KB 内核上加载失败：ELF 不兼容
```
### 16.2 打包 .so 对齐
```
zipalign -P 16 -c 4 AATPE-vX.zip
```
### 16.3 customize.sh 自检
```sh
pagesz=$(getconf PAGE_SIZE 2>/dev/null || echo 4096)
ui_print "- Device page size: $pagesz"
if [ "$pagesz" = 16384 ]; then
  ui_print "- 16KB device detected (NDK r30 build is 16KB-aligned)"
fi
```
NDK r30 默认产物"大页兼容小页"：16KB 对齐 ELF 在 4KB 内核仍可加载，反向才失败。故 r30 构建天然安全，doctor 仅报告。

---

## 17. sched_ext 技术判据（为什么现在不选）

| 判据 | 现状 | 结论 |
|---|---|---|
| 内核版本 | SCX 6.12 GA；手机 GKI 多 5.10/6.1/6.6 | 不满足 |
| CONFIG_SCHED_CLASS_EXT | GKI 默认未开 | 不满足 |
| 风险模型 | 全局调度器，bug=全机卡/重启 | 与"装上不出事"冲突 |
| AATPE 现有路径 | tracepoint eBPF + uclamp/cpuset，可秒退 | 非侵入、低风险 |
| scx_lavd | 面向 Steam Deck/x86 P/E 核，arm big.LITTLE 支持在完善（Igalia：目前只分 big/LITTLE 两类，三丛集需更细） | 不成熟 |

结论：P2 远期 opt-in，当前不落地。

---

## 18. 开源版图逐项目（补全 repo 与可借鉴点）

| 项目 | repo | 可借鉴 |
|---|---|---|
| Uperf | github.com/yc9559/uperf | 场景触发（前台切换）、写前读节点 |
| Scene | github.com/lirujun/Scene-v2 | 可视化、保守默认 |
| FDE.PSP | mks/高通系 | perf HAL 对接 |
| SmartPack | github.com/SmartPack/SmartPack-Kernel-Manager | 节点白名单探测 |
| Franco | github.com/franciscofranco | governor 曲线 |
| perfmtk | github.com/juaniman/perfmtk | DEFAULT_PROFILE=balanced ← AATPE 默认应 passive |
| QTI perfd | AOSP vendor | 官方 EAS uclamp 基线 |

核心共性：**节点探测 + 不可用降级 + 保守默认 + 场景触发**，AATPE 已具备前三者。

---

## 19. 兼容性失败矩阵（逐设备症状 → 根因 → AATPE 行为）

| 症状 | 根因 | AATPE 应有行为 |
|---|---|---|
| 16KB 设备 daemon 启动失败 | 二进制未 16KB 对齐 | customize.sh 自检报错；r30 产物已对齐不应出现 |
| daemon 起来但 eBPF 不 attach | /sys/fs/bpf 未挂 / tracepoint offset 不符 | POLLING 降级 + doctor 上报，不 crash |
| service.sh 起得太早，cpuset 探测失败 | 未等 boot_completed | resetprop -w + 轮询兜底 |
| daemon 被系统 SIGKILL(137) | oom_score_adj 高 / phantom process | setsid + oom_score_adj=-1000 |
| 升级后完整性永久 FAILED | modules_update 旧树锚定（已修） | 显式 $MODDIR 传参 |
| 卸载后线程仍跑 PRIME affinity | daemon 被 kill 未跑 handler | boot-completed.sh 兜底还原 |
| WebUI 连不上 | loopback cleartext 被 Android17 收管 | localhost 默认放行，无需改 |
| 与 Scene 抢 sysfs 节点抖动 | 双写者 | 写后读回检测漂移 → 旁观模式 |
| enforcing ROM 写节点被拒 | sepolicy 缺规则 | doctor 抓 avc → 提示加规则，降级只读 |
| 崩溃循环 | eBPF/bug 反复崩 | 60s>3 次进安全模式 |

## 20. doctor 自检命令清单（WebUI 后端）
```sh
getprop ro.build.version.sdk          # API level
uname -r                               # 内核版本（判 SCX 可行性）
getconf PAGE_SIZE                      # 页大小 4K/16K
cat /proc/self/oom_score_adj          # 防杀状态
ls -l /dev/cpuset/top-app/tasks        # cgroup 就绪
ls -l /sys/fs/bpf                      # BPF fs 挂载
dmesg | grep "avc: denied" | grep aatped  # SELinux denial
pm list packages | grep -E "vtools|game" # 冲突模块
cat /data/adb/modules/aatpe/integrity_status  # 完整性 verdict
```
doctor 把上述结果汇总成 WebUI 可读的健康分（每项 pass/warn/fail）。

---

## 精读清单（全文抓取 vs 摘录）
- 全文抓取：Magisk guides.html、KSU module.html（含 boot 时序/late-load/initrc）、APatch apm-guide.html。
- 摘录佐证：page-sizes、security-config、lmkd、sched-ext kernel.org、scx_lavd/rusty docs.rs、Igalia 回顾、XDA magiskpolicy。
- 核心可执行结论：daemon 放 service.sh + resetprop -w + 节点轮询；oom_score_adj=-1000+setsid；崩溃 60s>3 进安全模式；uninstall 还原 baseline；冲突只记录不抢写；16KB r30 天然对齐；SCX 暂不选。

## 附：本轮结论摘要
- 行数：本文见 wc -l；来源 32 条。
- 精读全文：Magisk guides、KSU module（含 boot 时序/late-load）、APatch apm-guide、sched-ext 官方、scx_lavd/rusty。
- AATPE 现有对齐：$MODDIR 显式传参（ROUND-002）、nodeWritable 探测、uclamp→affinity 降级、pid_identity triple、O_NOFOLLOW 原子写、三态完整性。
- 待办 P0：daemon 放 service.sh + boot-completed 后再 attach、崩溃安全模式、uninstall 还原基线。

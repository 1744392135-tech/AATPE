# AATPE SELinux 策略说明（v94 / 问题39、问题40）

> 本文件只做**文档**。问题39/40 的结论是：当前 SELinux 行为无法仅靠静态代码
> 保证，必须在真机上用 `avc: denied` 日志验证。本文记录现有策略、已知可能的
> denial 点、不同 ROM 的差异，以及真机验证方法。**未修改任何 sepolicy 规则。**

## 1. 当前 SELinux 策略（现状，不臆测）

模块中**没有**独立的 `.te` 策略文件（`find . -name '*.te'` 为空）。SELinux 放行
完全通过 `post-fs-data.sh` 在开机时用宿主的策略注入工具 `--live` 下发，分两套。
规则被刻意收紧为**最小自域（self）**——只允许模块自己所在的域访问 bpf /
perf_event，不再给 init / shell / zygote / rootfs / kernel 等额外域加权限（旧的
宽域规则已删除，见 post-fs-data.sh 中 `(dropped: ...)` 注释）：

- **Magisk 路径**（`magiskpolicy --live`）：
  - `allow magisk self bpf { read write map_create map_read map_write prog_load }`
  - `allow magisk self perf_event { open read write ioctl }`
- **KernelSU 路径**（`supolicy --live`）：
  - `allow init self bpf/perf_event ...`
  - `allow su self bpf/perf_event ...`
  - `allow magisk self bpf/perf_event ...`

关键安全姿态（与 `post-fs-data.sh` 注释一致）：

- **没有 `setenforce 0`**。全树 grep 仅在注释里出现 "NO setenforce 0"。
  daemon 对 `/sys/fs/selinux/enforce` 与 `/proc/sys/kernel/perf_event_paranoid`
  均为**只读探测（detect-only）**，绝不改值。
- **`chcon` / `restorecon` 用途受限**：`post-fs-data.sh` 仅对
  `$RUN_DIR/helper/`（WebUI 导出目录）设置 `u:object_r:system_data_file:s0`；
  `service.sh` 对该 helper 目录做同样 chcon；`uninstall.sh` 在清理阶段对
  `$RUN_DIR` 调用 `restorecon -R` 恢复上下文。这些都不放宽进程域，只对齐
  文件标签，且作用范围限于模块自己的运行时目录。
- eBPF 加载走原生 `bpf(BPF_PROG_LOAD)` 系统调用（见 `aatpe_ebpf.cpp:bpfSyscall`），
  不依赖 libbpf/动态加载，因此 SELinux 关注的就是这个 syscall 本身。

## 2. 已知可能的 denial 点（真机上最容易卡的地方）

静态代码无法证明这些点在所有 ROM 上都放行，按风险排序：

| 行为 | 代码位置 | 可能的 SELinux 类别 |
|------|----------|----------------------|
| BPF 程序加载 / map 创建 | `aatpe_ebpf.cpp` `bpf(BPF_PROG_LOAD)` | `bpf_prog` / `bpf` 域的 `prog_load` `map_create` |
| tracepoint 附加 / ring buffer | `attachTracepoint()` `perf_event_open` | `perf_event` 的 `open` `read` `write` `ioctl` |
| 读 debugfs/tracefs format 与 id | `readTracepointId()`、`verifyTracepointOffsets()` | `debugfs` / `tracefs` 读权限 |
| 读 `/sys/...` 调度节点 | watchdog `detectBigCorePool()`、thermal/frame | `sysfs` 读 |
| 读 `/proc/<pid>/task` 与 `/proc/stat` 等 | watchdog emergency migrate、thread_scan | `proc` 读 / `ptrace` 类 |
| 绑定 WebUI socket | `webui_http.cpp` | `unix_stream_socket` / `tcp_socket` 绑定 |

## 3. 不同 Android ROM 的 policy 差异（无法静态消除）

- **AOSP / 原生 GSI**：默认策略最严，`unprivileged_bpf_disabled` 常为 1，且无
  `magiskpolicy`/`supolicy` 时模块无法注入规则 → eBPF 可能直接拒绝，daemon 会
  优雅降级到 polling（见 `load()` 末尾）。
- **小米 / MIUI/HyperOS**：厂商已自带部分域策略差异，`shell`/`init` 域与 AOSP
  命名不完全一致；`--live` 注入可能因域不存在而静默失败（脚本已 `2>/dev/null`
  吞错，需看 `aatpe.log` 确认是否真的 applied）。
- **华为 / EMUI/HarmonyOS**：设备管理器/厂商签名策略较封闭，KernelSU/Magisk 域
  命名不同，`supolicy` 的 `rootfs`/`zygote` 假设可能不成立。
- **三星 / One UI**：Knox 内核加固可能额外限制 `perf_event_open`，与
  `perf_event_paranoid` 叠加。

**结论**：上表差异是 ROM 级、运行时的，静态改一份 `.te` 既覆盖不了也不该覆盖
（会与宿主策略冲突）。这正是问题39的本质。

## 4. 真机验证方法（强制）

在目标设备 root shell 下：

```sh
# 实时抓 BPF / perf_event / debugfs 相关 denial
dmesg | grep -E "avc: denied" | grep -iE "bpf|perf_event|debugfs|tracefs"
logcat | grep -E "avc: denied"

# 看模块自己记录的 eBPF 环境诊断（post-fs-data.sh 写入）
cat /dev/aatpe/aatpe.log | grep -iE "eBPF|SELinux|perf_event_paranoid|unprivileged_bpf"

# 确认 enforce 状态（只读，模块不改它）
cat /sys/fs/selinux/enforce        # 1=enforcing, 0=permissive

# 看 eBPF 实际走的加载方式与 offset 校验结果
cat /dev/aatpe/state/ebpf_offset_verified
```

判读：若 `dmesg` 出现 `avc: denied { prog_load } for comm="aatped"` 之类，即
对应第二节某个点被拦，需在该 ROM 上补对应 `magiskpolicy`/`supolicy` 规则，而不是
改模块代码。

## 5. 长期兼容性验证计划（问题40）

SELinux 规则较广（多域、`bpf`+`perf_event` 全开），长期兼容性不能一次验证完。
计划：

1. **按 ROM 建基线**：在 AOSP、MIUI、HarmonyOS、One UI 各保留一份
   `dmesg | grep avc` 快照，纳入 issue 库，作为回归对比。
2. **每次改 eBPF 路径必跑**：改 tracepoint/attach/读 `/sys` 后，在真机确认
   `aatpe.log` 无新增 `avc: denied`，且 `ebpf_offset_verified=true`。
3. **规则收敛而非扩张**：新能力优先复用已有 `bpf`/`perf_event` 类别，避免新增
   新的 SELinux 类别请求；必须新增时先在一台真机验证再合入。
4. **降级路径必须保持可用**：任何 SELinux 拦截都不应让 daemon 崩溃——`load()`
   已多级降级到 polling，验证时确认拦截后仍能起 polling 模式而非僵死。

# ROUND-005 报告 A：安卓任务放置机制（按内核代际）穷尽研究

> 研究对象：AATPE（Adaptive Android Task Placement Engine）——root 调度守护模块，按线程角色把任务引导到合适 CPU 簇。
> 目标设备：MediaTek 天玑 1100（MT6893），1×Cortex-A78@2.6 + 3×A78@2.0 + 4×A55@2.0，内核 **4.14.186 vendor 树**。
> 检索日期：2026-10-01。所有结论附 URL 与关键原文摘录。
>
> **一处重要事实校正**：任务背景假设"per-task uclamp mainline 合入 5.0"。经核对内核官方文档，
> **per-task uclamp 合入 mainline v5.3，cgroup 级 uclamp 合入 v5.4**（见下）。这直接决定了 AATPE 在 4.14 上的能力边界。

---

## 1. per-task uclamp（sched_setattr 的 sched_util_min/max）

### 1.1 mainline 合入版本（权威事实）
内核官方文档 `sched-util-clamp.rst` 原文：
> "Utilization clamping, also known as util clamp or uclamp … **was introduced in v5.3 release. The CGroup support was merged in v5.4.**"
> 来源：https://www.kernel.org/doc/html/v7.2-rc4/_sources/scheduler/sched-util-clamp.rst.txt （另见 https://docs.kernel.org/6.16/scheduler/sched-util-clamp.html ）

> "sched_setattr() syscall was extended to accept two new fields: **sched_util_min** … lower performance bound; **sched_util_max** … upper performance bound."
> 来源：https://dri.freedesktop.org/docs/drm/scheduler/sched-util-clamp.html

util-linux `uclampset(1)` 同样标注：
> "Utilization clamping is a new feature added in **v5.3**. It gives a hint to the scheduler about the allowed range of utilization the task should be operating at. … Only schedutil cpufreq governor understands handling util clamp hints."
> 来源：https://manpages.debian.org/trixie/util-linux/uclampset.1.en.html

**结论**：4.14 vendor 树默认**没有** mainline per-task uclamp；它只可能以 vendor backport 形式存在（见 1.4）。

### 1.2 ACK 各分支是否包含
- ACK 分支谱系（AOSP 官方）：common-android-4.9 / 4.14 / 4.19 / android11-5.4 / android12-5.10 / android13-6.1 GKI。
  来源：https://source.android.google.cn/docs/setup/reference/bazel-support
- uclamp 核心自 v5.3 才上游合入。因此：
  - **4.9 / 4.14 ACK 主干无 uclamp**；4.19 ACK 也无（4.19 < 5.3）。
  - **5.4 ACK 起**（GKI 早期）才有 per-task uclamp + cgroup v2 uclamp。
  - GKI（android12-5.10 / android13-6.1+）有完整 cgroup v2 `cpu.uclamp.min/max`。

### 1.3 sched_attr 结构在 arm64 的布局、size、E2BIG/EINVAL
`struct sched_attr`（`include/uapi/linux/sched/types.h`，内核 sched-deadline 文档示例）：
```c
struct sched_attr {
    __u32 size;
    __u32 sched_policy;
    __u64 sched_flags;
    /* SCHED_NORMAL / SCHED_BATCH */
    __s32 sched_nice;
    __u32 sched_priority;
    /* SCHED_FIFO / SCHED_RR / SCHED_DEADLINE */
    __u64 sched_runtime, sched_deadline, sched_period;
    /* uclamp (v5.3+) */
    __u32 sched_util_min;
    __u32 sched_util_max;
};
```
来源：https://docs.kernel.org/6.13/_sources/scheduler/sched-deadline.rst.txt 、https://docs.kernel.org/6.10/scheduler/sched-deadline.html

- **arm64 syscall 号**：`__NR_sched_setattr = 274`、`__NR_sched_getattr = 275`（注：内核文档示例里 `#ifdef __arm__` 用 380/381；arm64 用通用号表 274/275，AATPE 应优先用 glibc `sched_setattr()` 包装而非硬编码号）。
  来源：https://docs.kernel.org/6.13/_sources/scheduler/sched-deadline.rst.txt
- **size 语义**：`attr->size = sizeof(attr)`。内核按 size 决定接受哪些尾部字段。
  - **E2BIG**：`size` 大于内核 `sizeof(struct sched_attr)` 已知布局 → 表示"用户态传了比内核认识的更新的结构"，内核拒绝。
  - **EINVAL**：size=0、policy 非法、util 越界（>1024）等。
- **"接受但忽略"陷阱**：4.14 vendor 若**没有** uclamp backport，`sched_setattr()` 调用本身可能**成功返回 0**（因为它认识 size/policy/nice），只是 `sched_util_min/max` 两个字段被内核**静默忽略**。这就是为什么必须用 `sched_getattr()` **回读验证**，而不能只看 setattr 返回值。

### 1.4 vendor 4.14（MTK 天玑 / 高通）是否有 backport — 社区实证
- **高通**：QTI 文档明确暴露 UCLAMP 调参项，说明高通 CAF 4.14/4.19 树**backport 了 uclamp**：
  > "When the schedutil governor is used, **UCLAMP determines the CPU frequency selection. The UCLAMP value ranges from 0 to 1024.** … `sched_util_clamp_min`: sets the minimum acceptable performance level for individual tasks and task groups."
  来源：https://docs.qualcomm.com/bundle/publicresource/80-80021-10/topics/18-customize.md 、https://dragonwingdocs.qualcomm.com/System/Performance/customize-for-performance-tuning
- **MTK 4.14**：MTK Linux-4.14 调度 trace 实证（`sched_util: cid=… next=1624000`），说明 MTK 4.14 有 EAS/schedutil 与调度增强；但**公开材料里 MTK 4.14 的 /dev/stune 远多于 uclamp 节点**。天玑 1100 这类 MTK 4.14 **大概率走 schedtune 而非 per-task uclamp**，uclamp 是否 backport 需在设备上探测（见 §1.5）。
  来源（MTK 4.14 sched trace）：https://www.cnblogs.com/hellokitty2/p/15452157.html
- **LPC 2019 "UtilClamp usage on Android"** 直接对比两代机制：
  > "Features: SchedTune = out-of-tree custom cgroup controller; UtilClamp = core bits upstream since **v5.3**, cgroups support v5.4. … cgroup v1 only, single level hierarchy (schedtune)."
  来源：https://lpc.events/event/4/contributions/412/attachments/328/552/UtilClamp_usage_on_Android.pdf

### 1.5 AATPE 权威探测（含回读）建议
探测顺序（运行时只跑一次，缓存结果）：
1. `sched_setattr(tid, {size=sizeof, sched_util_min=512})` → 记录 errno。
2. 立即 `sched_getattr(tid, &attr2)` → 读回 `sched_util_min`。
   - 若 errno==0 且回读 ==512 → **uclamp 真生效**。
   - 若 errno==0 但回读 ==0 → **"接受但忽略"（假阳性）**，按不支持处理。
   - 若 errno==EINVAL/E2BIG → 明确不支持。
3. 同时探测 sysfs：`ls /dev/cpuctl/cpu.uclamp.min`、`/sys/fs/cgroup/cpu.uclamp.min`、`/dev/stune/` 是否存在。

---

## 2. cgroup 级 uclamp 节点路径

- **cgroup v1（Android 传统）**：cpu 控制器挂在 `/dev/cpuctl`（AOSP cgroup 抽象层原文）：
  > `"Controller": "cpu", "Path": "/dev/cpuctl"`；cgroup v2 挂在 `/sys/fs/cgroup`。
  来源：https://source.android.com/docs/core/perf/cgroups
  - 在 v1 + backport 内核上，典型出现 `/dev/cpuctl/cpu.uclamp.min`、`/dev/cpuctl/< subgroup >/cpu.uclamp.min`。
- **cgroup v2（GKI）**：`cpu.uclamp.min` / `cpu.uclamp.max`，v5.4 合入。内核文档原文：
  > "**cpu.uclamp.min / cpu.uclamp.max**. When a task is attached to a CPU controller, … cpu.uclamp.min is a protection."
  来源：https://www.kernel.org/doc/html/v7.2-rc2/_sources/scheduler/sched-util-clamp.rst.txt
- 出现版本：per-task v5.3 → cgroup v2 v5.4；GKI（5.10+）标准具备。

---

## 3. schedtune（/dev/stune/top-app/）机制

### 3.1 节点与语义
- 节点：`/dev/stune/top-app/{schedtune.boost,schedtune.prefer_high_cap,schedtune.prefer_idle,schedtune.colocate}`，另有 `/dev/stune/foreground`、`/dev/stune/background`、`/dev/stune/rt`、root `/dev/stune/schedtune.boost`。
- 官方/社区实证（AOSP boot-times 文档）：
  > "on init: write /dev/stune/schedtune.prefer_idle 1; write /dev/stune/schedtune.boost 100; on sys.boot_completed=1: reset … 0."
  来源：https://source.android.google.cn/docs/core/perf/boot-times
- Linaro 培训材料：
  > "write /dev/stune/foreground/schedtune.prefer_idle 1; prefer_idle: Prefer to place tasks on idle cpus, gives more responsiveness but costs power."
  来源：https://static.linaro.org/connect/las16/Presentations/Thursday/LAS16-400%20-%20Mini%20Conference%203%20AOSP%20%28Session%201%29%20%281%29.pdf

### 3.2 WALT vs PELT
- **PELT**（mainline EAS）：EWMA，每 1024µs 衰减，`y^32=0.5`（最近 32ms 贡献一半权重）。
  来源：https://www.kernel.org/doc/html/v6.19/_sources/scheduler/schedutil.rst.txt
- **WALT**（高通/MTK 4.14 常用）：窗口化负载追踪，默认窗口 **20ms**，跨 CPU 同步采样。
  来源（EAS Overview/WALT 文档）：见报告 B §5。
- schedtune 与 WALT/PELT 的关系：schedtune.boost 直接**放大任务利用率信号**，从而同时影响 schedutil 调频与 EAS 选核。

### 3.3 天玑 1100（MTK 4.14）是否有 stune/top-app — 实证
- MTK 4.14 vendor 树广泛使用 schedtune/EAS（cnblogs MTK trace 证据）。
- 中文社区调优脚本实证 MTK/AOSP 4.14 写 `/dev/stune/top-app/schedtune.boost 10`、`prefer_idle 1`：
  > "write /dev/stune/top-app/schedtune.boost 10; write /dev/stune/top-app/schedtune.prefer_idle 1; write /dev/stune/rt/schedtune.boost 30."
  来源：https://blog.csdn.net/liaochaoyun/article/details/122377605
- **判断**：天玑 1100 这类 MTK 4.14 **几乎必然有 /dev/stune/top-app**（但仍应在设备 `ls /dev/stune/` 实测确认）。

### 3.4 AOSP 如何用 top-app
- AOSP 通过 `task_profiles.json` + `SetTaskProfiles/SetProcessProfiles`（libprocessgroup）把应用进程放进 cgroup：
  > `"Controller": "schedtune", "Path": "top-app"`（JoinCgroup action）。
  来源：https://source.android.com/docs/core/perf/cgroups
- 即系统把前台/top-app 进程自动挂进 `top-app` cpuset + `top-app` schedtune。**AATPE 要抢超大核，本质是和这套既有分组竞争/叠加。**

---

## 4. cpusets / EAS / 三者叠加内耗

### 4.1 cpusets 惯例
- AOSP 惯例：`top-app`/`foreground` 用大核/全核，`background`/`system-background` 限小核。LWN 描述：
  > "cpusets … keep background tasks on the two slower CPUs; the top app and foreground share one fast CPU, the top app gets the other fast CPU to itself."
  来源：https://lwn.net/Articles/706125/
- 社区 root 脚本实证：`echo 0-3 > /dev/cpuset/system-background/cpus` 把后台按小核。
  来源：https://settingswizard.ru/android-kernel-scheduler-optimization-gaming/

### 4.2 EAS 选核要素
- EAS = energy model + capacity + util；uclamp 会 clamp `task_util()` 再参与选核：
  > "uclamp allows user space to specify min/max utilization via sched_setattr() or cgroup … can limit task_util()."
  来源：https://www.kernel.org/doc/html/v5.18/translations/zh_CN/scheduler/sched-capacity.html

### 4.3 硬 affinity / uclamp / schedtune 三者内耗
- **官方迁移方向**：prefer_idle 被 uclamp 取代。BKK19 材料：
  > "prefer_idle … will be going away when schedtune is removed in favour of util_clamp."
  来源：https://hosted-files.sched.co/bkk19/1f/HackingSession_AndroidPatches.pdf
- LPC 提议用 `cpu.uclamp_min>0` + `cpu.shares` 复现 prefer_idle。
- **内耗点**：schedtune.boost（放大 util）+ uclamp.min（抬升 util 下限）+ sched_setaffinity（硬绑核）同时用会互相打架——硬绑核绕过 EAS，boost 让小核看起来"够快"反而选不到大核。**官方建议：同代机制内只选一种，跨代用 uclamp 替代 schedtune。**

---

## 5. 天玑 1100 具体拓扑事实

- **SoC**：MT6893，6nm，4×Cortex-A78 + 4×Cortex-A55，Mali-G77 MC9。
  来源：https://www.mediatek.com/zh-cn/products/tablets/mediatek-dimensity-1100
- **实测拓扑（Geekbench 真机 OnePlus DN2103 = MT6893Z）**：
  > "Cluster 1: 4 Cores @ 2.00 GHz（A55）；Cluster 2: 3 Cores @ 2.60 GHz（A78）；另有 1×2.6 A78（prime）。"
  来源：https://browser.geekbench.com/v6/cpu/16739945
  - 即：A55 簇 = cpu0-3@2.0；A78 簇分 3+1，最大核 2.6GHz（与任务描述 1×2.6+3×2.0 一致）。
- **cpufreq policy 划分**：MTK 通常每簇一个 policy（A55 policy0=cpu0-3；A78 policy4=cpu4-7），`affected_cpus`/`related_cpus` 给出簇成员。
- **cpu_capacity**：需读 `/sys/devices/system/cpu/cpuN/cpu_capacity`（小核 ~239~308，大核 880~1024，依 OPP/温控动态）。
- **core_ctl / isolated / IPA**：MTK 4.14 常见 `/sys/devices/system/cpu/mt_cpufreq/`、core_ctl（大核上下线）、IPA 温控对 A78 限频。这些节点**非 mainline 标准**，必须设备探测。

---

## 6. 成熟工具的实际手段与教训

- **uperf（开源，国产 root 调度模块，与 AATPE 同类）**：
  > "根据识别场景动态设定 sysfs 节点；**动态绑定正在操作 APP 的 UI 相关线程到大核集群**；读触摸屏输入识别点击滑动；采样系统负载识别启动重负载；监听 cpuset 分组更新识别 app 切换。"
  来源：https://gitee.com/gongmi/uperf
  **教训**：靠 cpuset（把 UI 线程塞进大核 cpuset）+ sysfs boost，而非硬绑核。
- **Franco Kernel Manager / EX Kernel Manager**：支持 stune、CPU-Boost、input-boost、governor profiles。
  来源：见检索结果（FKManager 功能列表）。
- **FDE.AI / Scene / SmartPack**：主流手段是 schedtune.boost + cpuset + 调频 governor，而非 per-task uclamp（因为老设备无 uclamp）。
- **共性教训**：(a) 硬绑核（sched_setaffinity）暴力但伤 EAS/电源；(b) 首选"把线程挪进 top-app/大核 cpuset + stune boost"，最贴合系统既有机制；(c) 必须按设备探测能力分级降级。

---

## 7. 【AATPE 落地建议】分环境机制组合 + 探测顺序 + 回退链

| 环境 | 首选机制 | 次选 | 最后兜底 |
|---|---|---|---|
| **4.14 无 uclamp（天玑1100 大概率）** | 把目标线程 `echo tid > /dev/cpuset/top-app/tasks` + 写 `/dev/stune/top-app/schedtune.boost` | `sched_setaffinity` 绑 A78 簇（cpu4-6） | 仅 nice 提升（不保证选大核） |
| **4.19+ / 高通 CAF 有 uclamp backport** | `sched_setattr(tid, util_min=512~768)` + **sched_getattr 回读验证** | cgroup `/dev/cpuctl/cpu.uclamp.min` | stune boost |
| **5.4+ GKI cgroup v2** | cgroup v2 `/sys/fs/cgroup/<grp>/cpu.uclamp.min`（分组管理） | per-task sched_setattr | — |
| **三者都无（极端老内核）** | cpuset top-app + governor min_freq 抬升 | sched_setaffinity | 只记录日志不硬绑 |

**统一探测顺序（启动一次缓存）**：
1. `ls /dev/stune/top-app/` → 有无 stune；
2. `sched_setattr`+`sched_getattr` 回读 → uclamp 是否真生效；
3. `ls /dev/cpuctl/cpu.uclamp.min` 与 `/sys/fs/cgroup/cpu.uclamp.min` → cgroup uclamp；
4. 读 `cpuN/cpu_capacity` + `cpuN/cpufreq/affected_cpus` → 簇拓扑；
5. 结果写入能力位图，运行时只按位图选机制。

**失败回退链**：uclamp 回读假阳性 → 降 stune → stune 无 → 降 cpuset → cpuset 无 → 降 affinity → 仅 nice。每级降级记一行结构化日志，便于诊断。

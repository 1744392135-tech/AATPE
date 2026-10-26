# ROUND-006 深度研究 v2：逐子系统技术路线科学评估

> 研究对象：AATPE v3.4.6（Magisk/KernelSU root 调度守护，arm64 NDK C++17，目标天玑1100/MT6893，内核 4.14.186 vendor）。
> 目的：穷尽 2024-2026 公开资料，科学评估每个子系统实现是否最优，回答"哪些子系统换技术路线/语言会显著更好（非整体重写）"。
> 检索日期：2026-10-01。每条结论附 URL 与原文摘录。不碰任何代码。
> 承接 ROUND-005：uclamp v5.3/v5.4、PELT 32ms 半衰、WALT 20ms 窗、cpu_capacity 两路径、原子写四步。

---

## 1. 最新硬件架构现实：2024-2026 旗舰"无小核/全大核"

### 1.1 实测簇结构（权威厂商/媒体来源）
| SoC | 簇结构 | 原文摘录 | URL |
|---|---|---|---|
| 骁龙8 Elite (2024) | **2 Prime Oryon@4.32-4.47 + 6 Perf@3.53，无效率核** | "octa-core… two so-called prime cores and six performance cores. Yep, Qualcomm has finally ditched little or efficiency cores." | https://www.androidauthority.com/snapdragon-8-elite-3491805/ |
| 骁龙8 Elite Gen5 (2025) | 2 Prime@4.6 + 6 Perf@3.62 | "2 Prime core up to 4.6GHz; 6 Performance cores up to 3.62GHz." | https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/images/company/news-media/media-center/press-kits/snapdragon-summit-2025-press-kit/day-2-/documents/Snapdragon8EliteGen5_ProductBrief.pdf |
| 天玑9300 | **4×Cortex-X4@3.25 + 4×A720@2.0，全大核** | "world's first flagship smartphone chip to use an All Big Core design: four Cortex-X4 and four-A720." | https://www.mediatek.com/products/smartphones/mediatek-dimensity-9300 |
| 天玑9300+ | 1×X4@3.4 + 3×X4 + 4×A720 | "one Arm Cortex-X4 up to 3.4GHz, plus three Cortex-X4 cores and four Cortex-A720 cores." | https://www.mediatek.com/press-room/mediatek-boosts-flagship-smartphone-performance-with-dimensity-9300-soc |
| 天玑9400 | **1×X925@3.62 + 3×X4 + 4×A720（1+3+4）** | "1X Cortex-X925 (3.62GHz), 3X Cortex-X4 + 4X Cortex-A720." | https://www.mediatek.com/hubfs/MediaTek%20Assets/Pdfs/Infographics/MediaTek_D9400_Infographic.pdf |
| Exynos 2500 | **1×X925@3.3 + 2×A725@2.74 + 5×A725@2.36 + 2×A520@1.8（1+2+5+2，仍留2小核）** | "one large, seven mid-size, and two little cores." | https://semiconductor.samsung.com/us/processor/mobile-processor/exynos-2500/ |

### 1.2 调度含义
- **EAS 在全大核上**：全大核=同 ISA、容量差大幅缩小（天玑9300 里 A720 相对 X4 容量比从过去 ~30% 升到 ~60-70%）。此时 EAS 的"把小任务塞效率核省电"收益消失，调度重心从"跨簇选核"变成"**同簇内按容量/温度/负载均衡选核 + 频率管理**"。
- 骁龙 2+6 内部承认：
  > "Some might ask, is this just a renaming of big-little…? No."——微架构同源，差异在频率/容量档。
  来源：https://www.grandtruetech.com/news/7709/13890.html
- **对 AATPE（目标仍是 4.14 三簇 A55/A78×2）**：天玑1100 属于"旧三簇"范式，AATPE 的小/大/超大三档放置在本机完全成立；但代码必须假设未来用户可能跑在全大核机上，**簇数/档数要从 cpu_capacity 动态发现，不能硬编码"3 簇"**。

### 【AATPE 可执行建议】
- **收益：高｜成本：低**：簇/档数从 `cpuN/cpu_capacity` 聚类得出（容量差 <15% 视为同档），而非硬编码 small/big/prime 三档。这样 2+6/1+3+4/1+2+5+2 都能自适应。
- **收益：中｜成本：低**：在全大核机上，AATPE 的"送超大核"策略应退化为"同簇内优先空闲核 + 频率不封顶"，避免无意义地把线程挤唯一 prime 核造成热点。

---

## 2. 调度科学最新进展（mainline 6.6/6.12 LTS）

### 2.1 util_est / EWMA（6.12 schedutil 权威公式）
> "UTIL_EST drives an IIR EWMA with the 'running' value on dequeue… **filters to instantly increase and only decay on decrease**."
> 来源：https://www.kernel.org/doc/html/v6.12/_sources/scheduler/schedutil.rst.txt 、https://www.kernel.org/doc/html/v6.13/scheduler/schedutil.html
> 最终调频公式：`u_cfs = max(running, util_est)` → `clamp(u_cfs+u_rt, u_min, u_max)` → `u = u_clamp+u_irq+u_dl`。
> 来源：https://docs.kernel.org/scheduler/schedutil.html

**含义**：内核自己用"升即冲顶、降才衰减"的非线性滤波器防 DVFS 慢启动。AATPE 的评分窗口若做滑窗平均，会**反向**平滑掉突发——应借鉴"升快降慢"。

### 2.2 PELT vs WALT（厂用现状）
- **PELT**：EWMA，1024µs 周期，`y^32=0.5`（最近 32ms 占半）。来源：https://www.kernel.org/doc/html/v6.19/_sources/scheduler/schedutil.rst.txt
- **WALT**：20ms 窗，高通/MTK 历史使用；Linaro 实测：
  > "PELT takes more time to build up CPU utilization, thus delaying frequency ramp. **The frequency ramp up is 4 times faster on WALT**."
  来源：https://static.linaro.org/connect/sfo17/Presentations/SFO17-307%20WALT%20vs%20PELT.pdf
  > "WALT is approx 8000 lines; VA#1/VA#2 ~1200 lines."
  来源：https://static.linaro.org/connect/yvr18/presentations/yvr18-208.pdf
- **厂用现状（2024-2026）**：高通新文档已转 EEVDF+PELT：
  > "It uses the EEVDF CPU scheduler… EAS uses energy model (EM) and PELT signals."
  来源：https://docs.qualcomm.com/bundle/publicresource/80-70020-10/topics/2-performance-features.md
- **高通可调 PELT 半衰期**：
  > "sched_pelt_multiplier=[1,2,4]; default 1 (half life 32msec), 2/4 speeds up PELT clock."
  来源：https://docs.qualcomm.com/bundle/publicresource/80-70015-3/topics/customize.md

### 2.3 schedtune 在 AOSP 14/15 的现状：正在被 uclamp 取代
- 自定义内核 2024-2026 普遍：
  > "7. Migrate from Schedtune to Uclamp 8. Drop schedtune 9. Refactor task_profiles for Uclamp."
  来源：https://k00552684-spec.github.io/exyrom-site/
  > "Rework Uclamp Boosting… Set FP Boosting to Prime Cluster."
  来源：http://raw.githubusercontent.com/Voltage-Peridot/vendor_voltageota/refs/heads/15-qpr2/changelog_peridot.txt
- AOSP 官方仍在 NNAPI 文档保留 `writepid /dev/stune/nnapi-hal/tasks`（说明老设备仍有 stune）。
  来源：https://source.android.google.cn/docs/core/interaction/neural-networks/best-practices
- **结论**：schedtune 是"正在退役的 out-of-tree"机制，uclamp 是方向。AATPE 已按 ROUND-005 做 uclamp+stune 双探测是正确的。

### 2.4 sched_ext（6.12 LTS 新增）
> "Linux 6.12… mainline support for sched_ext, real-time PREEMPT_RT finally mainlined."
> 来源：https://www.phoronix.com/review/linux-66-612-lts
> "Extensible Scheduler class (sched_ext)… pluggable schedulers using BPF."
> 来源：https://hosted-files.sched.co/osseu2025/ed/ELCE-Status-of-Embedded-Linux-2025-08.pdf

### 【AATPE 可执行建议】
- **收益：中｜成本：低**：评分/突发检测借鉴 util_est"升即冲顶、降才衰减"的非线性，而非纯滑窗均值——缩短对 UI/输入突发的反应（WALT 比 PELT 快 4× 调频的教训）。
- **收益：低｜成本：高**：sched_ext/BPF 调度器插件——仅 6.12+ GKI 新设备可用，目标机 4.14 完全无。**不值得**现在投入。
- **收益：高｜成本：低**：能力位图把"uclamp 可用/stune 可用"写入，未来新设备自动优先 uclamp。

---

## 3. 逐子系统技术路线评估（现状是否最优 / 换路线判断）

> 语言判断三档：**不值得**（C++ 已最优，换语言纯增本）/ **局部值得**（仅某模块试点）/ **值得试点**。

| 子系统 | 现状机制 | 是否最优 | 更优路线 | 语言/路线判断 |
|---|---|---|---|---|
| **线程扫描 thread_scan** | 周期扫 /proc/<tid>，读 comm/stat | **基本最优**，但周期固定 | 周期自适应（前台活跃时 50ms，后台 500ms）；容忍 ESRCH（ROUND-005 已定） | **不值得换**：/proc 轮询在 root daemon 是唯一合规手段；Rust 无收益 |
| **身份采集** | comm 15 字节截断 | **需补**：UTF-8 校验+多字节截断出"？" | 身份=tid+stat comm+cmdline 组合（ROUND-005） | **不值得换**：C++ 按 ROUND-005 修即可 |
| **评分 scoring** | 滑窗特征加权 | **局部不优**：纯均值平滑掉突发 | 借鉴 util_est 升快降慢；特征含输入驻留时间 | **局部值得**：评分核心可考虑纯函数化（便于单测），但仍 C++ |
| **帧时监测 frame** | 读帧回调/choreographer 信号 | **最优**：已是信号驱动而非轮询 | 接 SurfaceFlinger 帧 trace（perfetto）做真机标定 | **不值得换** |
| **eBPF 事件链 + ring** | eBPF 追踪 sched/sched_wakeup 事件入 ring | **高价值但 4.14 受限**：4.14 eBPF 覆盖弱 | 4.14 上退化为 ftrace/eventpoll 读 trace_pipe | **局部值得**：eBPF 解析器/ring 消费可用 Rust 写独立 .a FFI（高安全面）；主体不换 |
| **spillover 阈值 + 滞回** | 固定阈值+dwell | **需标定**：无真机数据 | 用 §4 真机观测标定；dwell 32~100ms（PELT 半衰） | **不值得换** |
| **ML 在线学习 aatpe_ml*** | 在线特征→模型 | **冷启动风险** | 离线预训练权重打包 + 在线微调；冷启动用规则兜底 | **局部值得**：纯数值推理可 C++ 模板，不必 Rust；特征管道可考虑 |
| **温控归一化 thermal** | 读 thermal zone 归一化容量 | **最优**：与 EAS cpu_capacity 一致 | 读 IPA/thermal_emergency 节点降级 | **不值得换** |
| **watchdog** | 外部看门狗测 daemon 心跳 | **最优**：分离进程是正确模式 | 时间常数按崩溃恢复实测 | **不值得换** |
| **WebUI SSE / webui_http** | 内嵌 HTTP + SSE 推送 | **需补防缓存**（ROUND-005） | Cache-Control no-store + URL 带 ?v=VERSION | **不值得换**：C++ 内嵌 http 对 root 模块零依赖是优点 |
| **状态写/配置 reload** | 原子写（同目录 tmp+fsync+rename+fsync dir） | **最优**（ROUND-005 已定四步） | 单写者+锁序固定 | **不值得换** |

### 语言路线总结（诚实判断）
- **整体不重写 Rust/Go**。理由（承接 ROUND-005）：root 本地单写者、无网络面、4.14 老内核、已有 52 单测。
- **唯一"局部值得"**：**eBPF 事件字节流解析 / 用户 JSON 配置解析**这两个"不可信输入面"模块，可在下一代用 Rust 写独立静态库 FFI 链接，换取内存安全；其余子系统换语言都是负 ROI。
- **内核侧 vs 用户态**：AATPE 保持用户态（Magisk 模块兼容性、可热更新、免重刷）。把逻辑下沉内核模块/sched_ext 只对 6.12+ 新设备有意义，**不值得**为本 4.14 目标做。

### 【AATPE 可执行建议】
- **收益：高｜成本：中**：评分改"升快降慢"非线性 + 簇档数动态发现，是本轮两个最高性价比改动。
- **收益：低｜成本：高**：eBPF→Rust FFI 仅在下一代版本试点，不影响 v3.4.6。

---

## 4. 真机数据闭环方法论：30 分钟 ftrace 观测方案

### 4.1 权威采集手段（官方来源）
- **ftrace 节点**：`/sys/kernel/tracing`（或 `/sys/kernel/debug/tracing`，4.14 多为后者，需 `mount -t debugfs none /sys/kernel/debug`）。
  > "ftrace sysfs 节点位于 /sys/kernel/tracing；按事件：echo 1 > /sys/kernel/tracing/events/sched/sched_wakeup/enable."
  来源：https://source.android.google.cn/docs/core/tests/debug/ftrace?hl=zh-cn
  > Qualcomm：`echo 1 > .../events/sched/sched_switch/enable; cat trace_pipe`.
  来源：https://dragonwingdocs.qualcomm.com/System/Debug-Subsystem/trace-kernel-functions
- **Perfetto**（推荐，可脚本化）：
  > 可启用 `sched/sched_switch`、`sched/sched_wakeup`、`sched/sched_process_exec`。
  来源：https://developer.android.google.cn/tools/perfetto
  > Wattson（AOSP）：`ftrace_events: "sched/sched_switch"` + `cpuhz` 频率。
  来源：https://source.android.com/docs/core/power/wattson/how-to-wattson
- **trace-cmd**：`trace-cmd record -e sched_switch -e sched_wakeup ...`。
  来源：https://trace-cmd.org/Documentation/trace-cmd/trace-cmd-record.1.html
- **降开销**：`saved_cmdlines` 关闭可降追踪开销。
  来源：https://docs.kernel.org/6.13/trace/ftrace.html

### 4.2 30 分钟真机观测方案（天玑1100）
**准备（2 min）**：
1. `mount -t debugfs none /sys/kernel/debug`；
2. dump 静态拓扑快照：
   - `cat /sys/devices/system/cpu/possible /present /online`
   - `for n in 0..7: cat /sys/devices/system/cpu/cpuN/cpu_capacity; cat .../cpuN/cpufreq/affected_cpus`
   - `cat /sys/kernel/debug/sched_features`、`ls /dev/stune/ /dev/cpuctl/ /sys/fs/cgroup/`（确认能力位图）。

**采集（分段 25 min，每段独立 trace，避免 ring 溢出）**：
| 段 | 时长 | 场景 | 启用事件 |
|---|---|---|---|
| A 基线 | 5 min | 静置亮屏桌面 | sched_switch, sched_wakeup, cpu_frequency |
| B 输入突发 | 5 min | 连续滑动列表/打字 | 同上 + sched_waking, sched_stat_runtime |
| C 重负载 | 10 min | 打开大 app / 一局游戏 | 同上 + sched_process_exec, thermal_temperature |
| D 后台切换 | 5 min | 切后台/锁屏 | 同上，观察 cpuset/stune 分组迁移 |

**收尾（3 min）**：
3. `cat trace | gzip > round006_<seg>.trace.gz`（或 perfetto .pftrace）；
4. 关机停用所有事件：`echo 0 > .../events/enable; echo nop > current_tracer`。

**分析要点**：
- 统计 AATPE 关注线程（UI/RenderThread）落在哪个 cpuN、唤醒到上核延迟；
- 对照 `cpu_frequency` 事件看 spillover 后是否真的升频/上大核；
- 标定 dwell 与 spillover 阈值的真实驻留分布。

### 【AATPE 可执行建议】
- **收益：高｜成本：中**：把这套 ftrace/perfetto 方案做成 AATPE 自带的 `aatpe_probe --capture 30min`（root 下脚本化采集 + 自动停表防 ring 溢出），用真实数据标定 spillover/dwell/阈值——这是当前"凭经验写常量"的最大改进点。
- **收益：中｜成本：低**：采集后自动生成"线程-簇-频率"分布报告回 WebUI，形成闭环。

---

## 5. 结论速览
- **硬件**：2024-2026 旗舰走向全大核（骁龙2+6、天玑1+3+4），EAS 重心从跨簇选核转向同簇均衡；AATPE 目标机（天玑1100 三簇）仍是旧范式，但簇档数必须动态发现。
- **调度科学**：util_est"升快降慢"、PELT 32ms 半衰 vs WALT 4× 快调、schedtune 正被 uclamp 取代、sched_ext 仅 6.12+。
- **逐子系统**：11 个子系统中 8 个"不值得换路线"，2 个"局部值得"（评分非线性、eBPF/JSON 解析 Rust FFI 试点），无一个"值得整体重写"。
- **真机闭环**：ftrace/perfetto 30 分钟分段采集是标定所有经验常量的最高性价比下一步。

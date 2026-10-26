# ROUND-009 综合 Jank 诊断可行性研究：低负载低帧率根因分类

> 想法：让模块综合帧率、CPU/GPU 负载、实际使用等数据判断线程该放哪个核，并能判定"手机负载低但帧率低"的真实原因。
> 环境：root 用户态守护，天玑1100（4×A55 + 3×A78 + 1×A78 prime），内核 4.14.186 MTK vendor。
> 检索日期：2026-10-02。附 2024-2026 来源 URL 与原文摘录。不改代码。
> 承接 ROUND-008（簇压力算法）：触发信号=簇聚合 util，机制=uclamp>schedtune>affinity 逐级。

---

## 1. 数据源盘点（真机可读节点 + MTK 4.14 可用性）

| 信号 | 节点/命令 | MTK 4.14 可用性 | 替代/说明 |
|---|---|---|---|
| 帧率/帧耗时 | `dumpsys gfxinfo <pkg> framestats`（最近 120 帧 ns 时间戳） | ✅ 有 | "framestats … nanosecond timestamps from the **last 120 frames**"（https://iut-fbleau.fr/docs/android/training/testing/performance.html ）；`dumpsys SurfaceFlinger` 看 vsync/refresh |
| FrameTimeline/jank | Perfetto `android.FrameTimeline` | ✅（perfetto 可用） | GPU/Device composition 区分（https://perfetto.dev/docs/data-sources/frametimeline ） |
| GPU 利用率/频率 | `/sys/class/devfreq/<gpu>/{cur_freq,available_frequencies,governor}`；`dumpsys gfxinfo` | ✅ devfreq 有；MTK 另有 `/sys/class/misc/ged`、galcore | "lock freq: echo userspace > devfreq/<gpu>/governor"（https://blog.csdn.net/qq_27672101/article/details/143996392 ） |
| CPU 各簇 util/freq | `/sys/devices/system/cpu/cpuN/cpufreq/{scaling_cur_freq,scaling_max_freq,cpuinfo_max_freq}`；/proc/stat | ✅ 有 | max_freq 被压 = 热限频证据 |
| runnable 等待（rq 拥塞） | `/proc/<tid>/schedstat`：3 字段=运行 ns / **可运行但等待 ns** / 时间片数 | ✅（CONFIG_SCHEDSTATS） | 第 2 字段=线程在 rq 排队等待，是"小核簇拥塞"的直接证据（https://kernel-internals.org/sched/schedstat/ ） |
| CPU 饱和 | `/proc/stat` procs_running；vmstat `r` 列 | ✅ 有 | "r > CPU count = saturation"（Netflix 60s 法） |
| iowait | `/proc/stat` wa 列；vmstat `b`(D 态) | ✅ 有 | "r=running/waiting, b=uninterruptible(D)" |
| PSI（cpu/io/mem） | `/proc/pressure/{cpu,io,mem}`，格式 `some/full avg10/60/300 total` | **大概率有**（AOSP 已 backport 到 4.9/4.14） | "PSI patchset backported to **4.9 and 4.14**"（https://source.android.google.cn/docs/core/perf/lmkd ）；无则降级用 /proc/stat+vmstat 推断 |
| 内存回收 | MemAvailable、kswapd 活动、lmkd | ✅ 有 | PSI mem 高 + MemAvailable 低 = 回收压力 |
| 热限频 | 对比 `scaling_max_freq` vs `cpuinfo_max_freq` | ✅ 有 | 被压低 = thermal mitigation 生效 |
| 前台场景 | `dumpsys activity activities`/cpuset top-app/tasks | ✅ 有 | ROUND-008 已核验 top-app cpus |

---

## 2. "低负载低帧率"根因分类器（规则决策表）

> 前提：CPU 聚合低（<60%）但帧时 p95>16ms / jank 高。逐类判定证据组合 + 模块应做/不应做。

| 根因 | 证据组合 | 应采取动作 | 不应做 |
|---|---|---|---|
| **GPU bound** | GPU util/freq 高、CPU 闲、帧在 GPU composition 等待 | **提 CPU 无用**；记录为 GPU bound，仅必要时托 GPU freq（若模块有权） | ❌ 盲目把 UI 线程上 prime（浪费功耗） |
| **CPU 单线程瓶颈（关键线程在小核跑）** | 簇聚合低，但**某关键线程 schedstat 第2字段(wait)高** 或该线程在 cpu0-3 跑满 | 按 ROUND-008 簇压力+util Top-1，把**该关键线程**逐级提到大核/prime | ❌ 提升整个 app 所有线程 |
| **热限频** | `scaling_max_freq < cpuinfo_max_freq`（被压） | 不强行提频；等 thermal 恢复；EMERGENCY 档降级 | ❌ 逆 thermal 强提 prime/满频 |
| **I/O 卡顿** | PSI io 高 / iowait wa 高 / D 态 b 多 | 与 CPU 放置无关；记录，不 boost（boost 也救不了 IO） | ❌ 提核 |
| **内存回收** | PSI mem 高 + MemAvailable 低 + kswapd 活跃 | 不 boost；提示 LMK/杀后台 | ❌ 提核 |
| **vsync/后台干扰** | 帧等待 vsync、后台线程抢小核 | 把后台线程按门禁压回小核/降优先级 | ❌ 前台线程与后台抢大核 |
| **省电/降频模式** | governor=powersave、min_freq 被锁低 | 记录；是否提示用户切性能模式 | ❌ 强改全局 governor（与厂商冲突） |

**关键科学结论**：jank 不一定是"CPU 不够"。分类器的首要价值是**防止在 GPU bound / IO / 热 / 内存场景下盲目提 CPU**——这正是省电边界。

---

## 3. 综合放置策略（在 ROUND-008 簇压力之上融合）

### 3.1 信号权重与优先级仲裁（thermal 最高）
```
仲裁优先级（高→低）：
  1. thermal EMERGENCY  → 强制降级，禁止上 prime（最高）
  2. 根因分类命中 GPU/IO/内存 bound → 不做 CPU 放置干预
  3. 场景识别（冷启动/滑动/游戏/待机）→ 决定 boost 时长档位
  4. 簇聚合压力（ROUND-008）→ 决定提升级别
  5. 帧时 p95/p99 jank 投票 → 校正阈值
  6. util Top-N → 决定提哪些线程
```

### 3.2 融合伪逻辑
```
每周期(200ms)：
  jank = (frame p95 > 16ms) or (missed_vsync 增量>0)
  root = classify(psi_io, psi_mem, gpu_util, max_freq_vs_cpuinfo, schedstat_wait)
  if thermal==EMERGENCY: ramp_down_all(); return
  if root in {GPU_BOUND, IO, MEM}: 
       log(root); boost_cpu=false;   # 不盲目提
  if root==THERMAL_CAPPED: 
       hold(); wait_thermal()
  if root==SINGLE_THREAD_CPU:
       tid = argmax(schedstat.wait among key threads on cpu0-3)
       boost_cluster(tid, target=BIG->PRIME, mode=uclamp.min else stune)
  elif cluster_pressure(small)>80% sustained:   # ROUND-008
       boost_topN_levelwise()
  # 事件驱动+短时 boost+自动回落（标杆原则，ROUND-007）
  if jank and input_or_launch:
       short_boost(40-100ms); later auto_decay
  if idle low pressure sustained: symmetric_ramp_down()
```

### 3.3 与 ROUND-008 的关系
- ROUND-008 解决"小核簇饱和却无线程上大核"（簇级聚合压力）；
- ROUND-009 在其前加一层"**根因分类**"：先判断 jank 是不是 CPU 造成的，是才进 ROUND-008 放置；不是则记录不干预。两者是"诊断→处方"关系。

---

## 4. 边界、冲突与风险

### 4.1 用户态能做 / 做不到
| 能做 | 做不到（需内核/厂商） |
|---|---|
| 读 framestats/devfreq/schedstat/PSI/cpustat/thermal max_freq | 改 misfit 只对单任务超容量的内核逻辑 |
| 分类根因、按根因决定是否提线程 | 改 GPU 调度（GPU bound 提不了 CPU） |
| 抬线程 uclamp/schedtune、扩 cpuset | 改 thermal 限频决策 |
| 短时 boost+自动回落 | 强制逆 thermal 提频 |

### 4.2 与 Scene/厂商 HAL 冲突面
- Scene 写全局 governor/min_freq；AATPE 只做线程级放置+短时 boost，不长期写全局调频。
- 厂商 perfd/PowerHAL 已做 input/游戏 boost；AATPE 与其**叠加**时需避让（后写覆盖）。

### 4.3 误判风险与省电边界
- **误判 GPU bound 为 CPU bound** → 盲目上 prime 费电。对策：GPU util 高则一票否决提 CPU。
- **误判热限频为算力不足** → 逆 thermal 强提，加剧发热。对策：thermal 最高优先级。
- **省电边界**：只在"确认是 CPU 单线程/簇压力"且非 GPU/IO/热/内存时才 boost；短时(40-100ms)自动回落。

---

## 5. 改进项总表（按优先级）

| # | 现状 | 建议 | 收益 | 成本 | 风险 | 优先级 |
|---|---|---|---|---|---|---|
| 1 | 不区分根因，一律按簇压力提 CPU | 加**根因分类器**（GPU/IO/内存/热/单线程），非 CPU 根因不 boost | **极高（省电+防误判）** | 中 | 分类阈值需标定 | P0 |
| 2 | 无 PSI/GPU/schedstat 观测 | 接入 PSI(io/mem)+devfreq GPU+schedstat wait 信号 | 高 | 中 | PSI 4.14 需探测存在性 | P0 |
| 3 | 帧时未参与决策 | 用 framestats p95>16ms / missed_vsync 做 jank 投票校正阈值 | 高 | 低 | dumpsys 解析开销 | P1 |
| 4 | 热限频与放置未联动 | 比对 scaling_max vs cpuinfo_max，thermal 最高优先级仲裁 | 高（安全+省电） | 低 | 低 | P0 |
| 5 | 关键线程识别靠 util | 用 schedstat 第2字段(wait)找"rq 拥塞受害者"关键线程 | 高 | 低 | 需区分 idle 线程 | P1 |
| 6 | boost 时长/回落未对齐标杆 | 事件驱动短时 40-100ms + 自动衰减（ROUND-007） | 中高 | 低 | 过短致抖动 | P1 |
| 7 | 与厂商 HAL 叠加无避让 | 文档声明不写全局调频档，避让 perfd | 中 | 低 | 低 | P2 |

---

## 6. 结论
- **想法可行且科学**：在 root 用户态，framestats + devfreq GPU + schedstat + PSI(A CK 已 backport 4.14) + thermal max_freq + /proc/stat 已足够做"低负载低帧率"根因分类。
- **核心价值不是"更猛地提 CPU"，而是"先判断 jank 是不是 CPU 造成的"**——GPU/IO/热/内存 bound 时盲目提线程既无效又费电。
- **定位**：ROUND-009 根因分类 → ROUND-008 簇压力放置，构成"诊断→处方"闭环；thermal 一票否决，短时 boost 自动回落，与 Scene/厂商 HAL 避让。

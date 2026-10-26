# ROUND-008 簇级聚合压力算法设计：修复"小核饱和而 PRIME 闲置"

> 真机现象（天玑1100，4×A55[0-3] + 3×A78[4-6] + 1×A78 prime[7]，内核 4.14.186）：
> 打游戏时小核 0-3 全程 100% 顶满、大核 4-6 次之、**cpu7 全程几乎零出力（prime_spillover_count=0、托管线程=0）**。
> 现状策略：单关键线程利用率超阈值（active 65%/assist 75%/passive 85%）才把 Top-N 提升到 PRIME，dwell 60-150ms 回落；机制 stune_cpuset/uclamp/affinity 四选一。
> 检索日期：2026-10-02。附 URL 与原文摘录。不改代码。
> 承接 ROUND-005/006/007：uclamp v5.3/v5.4（4.14 MTK 多为 schedtune）、PELT 32ms、EAS capacity=1024、80% over-utilized。

---

## 1. 诊断：为什么"小核簇饱和而 PRIME 闲置"必然发生

### 1.1 根因一：misfit 迁移只对"单任务超容量"生效——这是本案的直接成因
内核 sched-capacity 权威文档：
> "Tasks … dubbed **'misfit'** tasks … **Misfit task migration leverages the CFS load balancer, more specifically the active load balance** … triggered **if a misfit task can be migrated to a CPU with more capacity**."
> 来源：https://www.kernel.org/doc/Documentation/scheduler/sched-capacity.rst

> "On a **partially busy system** (idle CPUs; busy CPUs have one task each), scheduling for asymmetric capacity ensures that **misfit tasks land on the big CPUs. The remaining tasks, misfit or not, run on the small CPUs**."
> 来源：https://patchew.org/linux/20260429-rneri-fix-cas-clusters-v2-0-cd787de35cc6@linux.intel.com/

**翻译到本案**：游戏负载是"多个中等负载线程，每个 30-60%"。
- 每个线程单独看 util(30-60%) < 小核 capacity(~308) → **没有任何一个线程是 misfit**；
- 于是 EAS 把它们全部留在小核（"remaining tasks run on the small CPUs"）；
- 小核 4 个被 8+ 个线程打满到 100%，但**因为没有单线程超容量，misfit 迁移永远不触发**，线程不会被向上推到大核/prime。
- 这就是 prime_spillover_count=0 的机制级原因：**AATPE 的 per-thread 阈值法和内核 misfit 是同一个盲区**——都只看单线程，不看簇聚合。

### 1.2 根因二：EAS 在"无 over-utilized"时禁用负载均衡、偏好省电小核
> "CPUs are flagged as **'over-utilized' as soon as used at more than 80%** of capacity. **As long as no CPUs are over-utilized in a root domain, load balancing is disabled and EAS overrides the wake-up balancing code. EAS is likely to load the most energy efficient CPUs** … if that can be done without harming throughput."
> 来源：https://www.kernel.org/doc/html/next/scheduler/sched-energy.html

**含义**：EAS 的设计目标就是"把任务尽量堆到最省电的小核"。只有当某 CPU >80% 被判 over-utilized，active balance 才介入。而 active balance 又受 misfit 逻辑约束（见 1.1）。结果：小核堆到 100% 也只是"勉强 over-utilized"，但缺乏把"聚合压力"翻译成"向上迁移若干线程"的机制。

### 1.3 根因三：cpuset top-app 在 MTK 4.14 是否含 prime(cpu7) 不确定
- AOSP 惯例：top-app cpuset 应给前台应用更多核，甚至"保留一个专属核"：
  > "Configure a cpuset containing only the top foreground app; **reserve one core (exclusive) for threads from this cpuset**."
  来源：https://source.android.google.cn/docs/core/power/performance?hl=id
- 但 vendor BSP 可能裁剪：
  > "Some vendors leave this disabled in their BSPs … cpusets are useful for ensuring load balancing reflects what the user is doing."
  来源：https://source.android.google.cn/docs/core/tests/debug/jank_capacity
- 同类工具 thread-opt 的实测分组（可作天玑1100 参照）：
  > "TOP_GROUP: analyzes cgroup for core '7', defaults to [7] or [6] → cpu6-7 or cpu7 only; BACKGROUND: cpu0-3; MIDDLE: all minus TOP/BACKGROUND."
  来源：https://deepwiki.com/reigadegr/thread-opt/1.1-key-concepts

**结论**：天玑1100 的 `/dev/cpuset/top-app/cpus` 必须真机实测——若它只含 0-6（不含 7），则 top-app 线程**根本无权上 prime**，AATPE 再怎么 per-thread 提权也无效（被 cpuset 屏蔽）。这是 cpu7 闲置的第二个可能成因。

### 1.4 根因四：AATPE per-thread 阈值法在该负载下必然漏触发
- active 65%/assist 75%/passive 85% 都是**单线程 util 阈值**；
- 游戏里每个线程 30-60%，**没有一个过 65%** → Top-N 提升永不触发 → prime_spillover_count=0。
- 即：阈值法度量的是"单线程重"，而本案瓶颈是"簇聚合重"。**度量对象错了。**

---

## 2. 最科学策略：簇级聚合压力驱动（cluster-pressure saturation）

### 2.1 核心思想
不再问"某个线程是否超阈值"，而是问"**小核簇是否持续饱和**"。饱和持续 dwell 后，按 util 排序把最该走的线程**逐级提升**（小核簇→大核簇→prime），大核簇也饱和再溢到 prime；对称回落。

### 2.2 阈值 / 滞后 / dwell / Top-N 经验区间（省电边界）
| 参数 | 建议区间 | 依据 |
|---|---|---|
| 小核簇聚合压力触发 | 平均 util > **75-80%** 持续 dwell | 对齐 EAS 80% over-utilized（ROUND-008 §1.2）；低于此 EAS 自己处理，不干预省电 |
| 触发 dwell | **100-200ms**（2-6×PELT 32ms） | 防止瞬时毛刺误触；ROUND-006/007：dwell 对齐 PELT 半衰 |
| 提升粒度 | 每次 Top-N=1~2 个 util 最高线程，**逐级**先到大核簇，不直接跳 prime | 省电边界：先消化大核簇空闲，最后才用 prime |
| 大核簇再溢 prime | 大核簇聚合 >75% 持续 dwell 后，再取 Top-1 上 prime | 对称层级溢 |
| 对称回落 | 簇聚合 <50% 持续 dwell(200-300ms) 后逐级降回 | 防止抖动；ROUND-007：突发后快速回落省电 |
| prime 驻留上限 | 单线程上 prime 后，非持续高 util 则 dwell 200-300ms 回落 | cpu7 是功耗黑洞，不长驻 |
| 温控 | EMERGENCY 档禁止上 prime，后台线程门禁照旧 | 不与 thermal 抢 |

### 2.3 uclamp.min 分档 vs 硬 affinity 的取舍
- **uclamp（首选，让 EAS 自己选核）**：
  > "For task placement, only EAS/CAS make use of uclamp … When a task wakes up, scheduler looks at **effective uclamp value of every rq** and compares … favoring the rq with most energy[-efficient feasible]."
  > 来源：https://www.kernel.org/doc/html/v7.2-rc3/_sources/scheduler/sched-util-clamp.rst.txt
  > "UCLAMP_MIN implies **boosting** a task."
  来源：https://docs.kernel.org/scheduler/sched-util-clamp.html
  > 高通："UCLAMP_MIN>0 → task demand always ≥ this value."
  来源：https://docs.qualcomm.com/bundle/publicresource/80-70023-3/topics/scheduler.md
- **取舍结论**：
  1. **有 uclamp（sched_getattr 回读确认）**：用 `uclamp.min` 分档抬升被提升线程（小档=384，大档=512，prime 候选=768），让内核 EAS 自己选核——**不硬绑**，保留迁移灵活性、最省电、不与 thermal 打架。
  2. **无 uclamp（天玑1100 大概率）**：fallback 到 schedtune.boost（/dev/stune/top-app）短时抬升；再不行才用 `sched_setaffinity` 硬绑——**硬绑只作最后兜底**，且绑大核簇不绑单 prime 核。
  3. **禁止**一上来就 sched_setaffinity(cpu7)：绕过 EAS、抗热差、与 Scene/厂商 HAL 冲突。

### 2.4 算法伪逻辑
```
每决策周期(200ms)：
  s_small = avg_util(cpu0-3)   # 小核簇聚合
  s_big   = avg_util(cpu4-6)
  if thermal==EMERGENCY: drop_to_baseline(); return
  if app not foreground/game: symmetric_ramp_down(); return

  # 小核簇饱和 → 逐级提升（先大核簇）
  if s_small > 80% sustained 150ms:
      cand = topN_threads(by util, n=2)   # 不直接上 prime
      boost_cluster(cand, target=BIG, mode=auto(uclamp.min=512 else stune))
  # 大核簇也饱和 → 溢到 prime
  if s_big > 75% sustained 200ms:
      p = top1_thread(by util, currently on BIG)
      boost_cluster(p, target=PRIME, mode=uclamp.min=768 else affinity{7})
  # 对称回落（防抖动/省电）
  if s_small < 50% sustained 250ms:
      ramp_down_one_level()
```

---

## 3. 真机核验清单（doctor 脚本固化）

| 检查项 | 节点/命令 | 判定 |
|---|---|---|
| top-app cpuset 是否含 cpu7 | `cat /dev/cpuset/top-app/cpus` | 输出含 `7`？不含则 AATPE 提权无效，需先扩 top-app cpus |
| top-app 任务当前归属 | `cat /dev/cpuset/top-app/tasks` | 游戏包名 tid 是否在内 |
| schedtune 是否存在 | `ls /dev/stune/top-app/` | 有 `schedtune.boost/prefer_idle`？4.14 MTK 大概率有 |
| uclamp 是否真生效 | `sched_setattr(tid, util_min=512)`→`sched_getattr` 回读 | 回读==512 才真生效（ROUND-005 权威探测） |
| cgroup uclamp 节点 | `cat /dev/cpuctl/cpu.uclamp.min` 或 `/sys/fs/cgroup/cpu.uclamp.min` | 存在即用 cgroup 档 |
| 线程实际可跑 CPU | `taskset -p <tid>` / `sched_getaffinity` | 确认 cpuset 是否把 tid 限制在 0-6 |
| cpu7 是否被隔离/下线 | `cat /sys/devices/system/cpu/online`、`.../cpu7/isolated` | 7 是否 online、是否 isolated |
| 簇 capacity | `cat /sys/devices/system/cpu/cpuN/cpu_capacity` | 确认三档容量标定 |

---

## 4. 责任划分：用户态能做 / 必须靠内核 / 与厂商冲突

| 能力 | 谁能做 | 说明 |
|---|---|---|
| 读簇聚合 util、按 util 排序选线程 | **AATPE 用户态** | /proc/stat、topology、cpu_capacity |
| 抬单线程 uclamp.min / schedtune.boost | **AATPE 用户态**（前提节点存在） | sched_setattr、写 /dev/stune |
| 扩 top-app cpuset 含 cpu7 | **AATPE 用户态可写**（需谨慎） | 直接改 /dev/cpuset/top-app/cpus，但与厂商冲突 |
| 内核 misfit/active balance 行为 | **必须靠内核** | AATPE 无法改 misfit 只对单任务超容量的逻辑 |
| uclamp 内核支持 | **必须靠内核**（4.14 MTK 多半无） | 无则只能 schedtune/affinity |
| 与 Scene/厂商 perfd 抢 sysfs 写 | **冲突区** | 后写覆盖；需文档声明 + 避让 |

---

## 5. 改进项总表（按性价比排序）

| # | 现状 | 标杆 | 差距 | 建议算法 | 收益 | 成本 | 风险 |
|---|---|---|---|---|---|---|---|
| 1 | per-thread 65/75/85% 阈值 | EAS misfit 只看单任务（同盲区） | 度量对象错：看单线程不看簇 | 改**簇聚合压力**触发（小核>80%持续150ms） | **极高** | 中 | 阈值需真机标定 |
| 2 | 直接提 Top-N 到 PRIME | 逐级溢（先大核再prime） | 一上来就上 prime，费电 | 逐级：小核簇饱和先提大核簇，大核簇再溢 prime Top-1 | **高（省电）** | 中 | 低 |
| 3 | 未核验 top-app cpus 含7 | AOSP top-app 可留专属核 | 若 top-app 不含7，提权全无效 | doctor 脚本先 `cat /dev/cpuset/top-app/cpus` | **极高** | 低 | 扩 cpuset 与厂商冲突 |
| 4 | 机制四选一未按能力选 | uclamp>schedtune>affinity | 无 uclamp 时误设 | sched_getattr 回读探测，无则 schedtune，兜底 affinity | 高 | 低 | 低 |
| 5 | 回落不对称/抖动 | PELT 32ms、对称滞回 | 突发后滞留费电 | 对称回落：簇<50%持续250ms逐级降回 | 中 | 低 | 过短致抖动 |
| 6 | dwell 60-150ms | 100-200ms 触发 | 偏短易毛刺 | 触发 dwell 提到 100-200ms | 中 | 低 | 低 |
| 7 | 硬绑 cpu7 倾向 | 让 EAS 选核（uclamp） | 绕过 EAS、抗热差 | uclamp.min 分档抬升，硬绑仅兜底 | 高 | 中 | 与 Scene 冲突 |

---

## 6. 结论
- **根因**：本案是"多中等负载线程、单线程都不超小核容量"→ 内核 misfit 与 AATPE per-thread 阈值**同一个盲区**（只看单任务），导致小核簇 100% 饱和却无线程被向上迁移；叠加 top-app cpuset 可能不含 cpu7。
- **解法**：把触发信号从"单线程 util"换成"**簇聚合压力**"，逐级（小核→大核→prime）按 util Top-N 提升，对称回落；机制优先 uclamp.min 分档（让 EAS 选核），其次 schedtune，硬绑兜底。
- **先决条件**：doctor 脚本必须先核验 `/dev/cpuset/top-app/cpus` 是否含 cpu7 + uclamp 是否 sched_getattr 回读真生效，否则提权白做。

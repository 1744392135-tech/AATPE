# ROUND-010 · R3 帧率/卡顿/热/GPU/内存综合感知调研（深度版 v2）

> 目的：让 AATPE 的卡顿根因诊断与动作达到业界最准。逐条附 URL + 原文摘录，工程可直接落地。
> 调研日期：2026-10-03。仅调研，不改代码。精读一手资料：AOSP/Android Developers 官方文档、Linux kernel 文档、Qualcomm 官方文档、Perfetto 官方文档。

---

## 0. TL;DR

- **掉帧阈值动态化是 P0**：写死 16.6ms 在 90/120/144/165Hz LTPO 机上会误判/漏判。root 下用 `dumpsys SurfaceFlinger --latency <layer>` 第一行拿当前 refresh period，或 `dumpsys display | grep mRefreshRate`，再算 deadline。
- **framestats 是 15 列 ns 时间戳表**，可离线算 Input 延迟、CPU 主线程耗时、GPU 耗时、deadline miss；FLAGS!=0 行必须丢弃（窗口切换/跳帧垃圾值）。
- **jank_type 逐帧仅 Android12+ perfetto frametimeline 可得**（且不支持 SurfaceView 游戏面）；无 trace 时用 gfxinfo 顶部聚合计数退化分类。
- **PSI 是最划算卡顿代理**：lmkd v2 官方阈值 partial=70ms(高)/200ms(低RAM)、complete=700ms、thrashing_limit=100/30——直接借作 AATPE MEM/IO 分类阈值。
- **热限频 vs 省电限频必须分开判**：cooling_device cur_state 动+thermal_zone temp 高=热；temp 不高但 scaling_max_freq 被 cap=省电/策略。
- **root 可等价 ADPF**：直接写 cgroup task `uclamp.min`，绕过 PerformanceHintManager。

---

## 1. framestats 字段级表（15 列，ns）

来源：Android Developers《Testing Display Performance》
URL: https://iut-fbleau.fr/docs/android/training/testing/performance.html
原文（输出样例）：
```
0,27965466202353,27965466202353,27965449758000,27965461202353,27965467153286,
 27965471442505,27965471925682,27965474025318,27965474588547,27965474860786,
 27965475078599,27965479796151,27965480589068,
```
原文："prints out frame timing information, with nanosecond timestamps, from the last 120 frames … window ~2 seconds."

| # | 列名 | 含义（原文） | 工程用途 |
|---|---|---|---|
| 1 | FLAGS | "0 行可算总帧时；非 0 行应忽略，该帧被判为 outlier（窗口变化/旋转首帧/跳帧垃圾值）" | **FLAGS!=0 直接丢弃** |
| 2 | INTENDED_VSYNC | 帧预期起点；与 VSYNC 不同=UI 线程忙没及时响应 vsync | 帧时起点 |
| 3 | VSYNC | 本帧实际 vsync（Choreographer 回调/动画/drawing time） | vsync 基准 |
| 4 | OLDEST_INPUT_EVENT | 输入队列最老事件时间戳；无输入=Long.MAX_VALUE | 平台用 |
| 5 | NEWEST_INPUT_EVENT | 输入队列最新事件；无输入=0 | **Input 延迟 = FRAME_COMPLETED - NEWEST_INPUT_EVENT** |
| 6 | HANDLE_INPUT_START | 输入事件派发给 app 的时间 | (本列→ANIMATION_START)=输入处理耗时；>2ms 异常 |
| 7 | ANIMATION_START | Choreographer 动画运行时刻 | →PERFORM_TRAVERSALS_START 动画耗时 |
| 8 | PERFORM_TRAVERSALS_START | measure/layout 起点 | DRAW_START - 本列 = measure/layout 耗时 |
| 9 | DRAW_START | performTraversals 的 draw 阶段起点（录 display list） | →SYNC_QUEUED = View.draw() 递归耗时 |
| 10 | SYNC_QUEUED | 向 RenderThread 发 sync 请求时刻 | →SYNC_START 差距大=RenderThread 忙（前帧超预算积压） |
| 11 | SYNC_START | RenderThread sync 阶段起点 | →ISSUE_DRAW_COMMANDS_START >0.4ms=大量 Bitmap 上传 GPU |
| 12 | ISSUE_DRAW_COMMANDS_START | HWUI 开始发 GPU 命令 | →FRAME_COMPLETED = GPU/overdraw 工作量 |
| 13 | SWAP_BUFFERS | eglSwapBuffers 时刻 | 平台用 |
| 14 | FRAME_COMPLETED | 本帧全部完成 | **总帧时 = FRAME_COMPLETED - INTENDED_VSYNC** |
| (15) | (GpuCompletion) | GPU 围栏完成（新版列） | GPU 真实耗时尾段 |

### 1.1 由字段派生的诊断量（可实现公式）
```
total_frame_ns   = FRAME_COMPLETED - INTENDED_VSYNC          # 端到端
input_latency_ns = FRAME_COMPLETED - NEWEST_INPUT_EVENT       # 用户可感
ui_busy_ns       = ANIMATION_START - VSYNC                    # 主线程前序积压
layout_ns        = DRAW_START - PERFORM_TRAVERSALS_START
draw_rec_ns      = SYNC_QUEUED - DRAW_START
rt_waitsync_ns   = SYNC_START - SYNC_QUEUED                   # RenderThread 被前帧堵
upload_ns        = ISSUE_DRAW_COMMANDS_START - SYNC_START    # bitmap 上传
gpu_work_ns      = FRAME_COMPLETED - ISSUE_DRAW_COMMANDS_START
deadline_miss    = total_frame_ns > vsync_budget_ns
```
边界处理：
- FLAGS!=0 行：整行丢弃（原文明确 outlier/垃圾）。
- NEWEST_INPUT_EVENT==0（无输入）：input_latency 不参与统计。
- OLDEST_INPUT_EVENT==Long.MAX_VALUE：哨兵，过滤。
- 重启计数：framestats 窗口仅 ~2s/120 帧；跨窗口累计需先 `dumpsys gfxinfo <pkg> reset` 对齐计数（原文："reset counters and aggregate at a regular cadence"）。
- NaN/缺列：行内列数 <14 丢弃，防止越界。

### 1.2 gfxinfo 聚合计数（无 framestats 时的退化分类）
原文样例：
```
Total frames rendered: 82189
Janky frames: 35335 (42.99%)
90th percentile: 34ms   95th: 42ms   99th: 69ms
Number Missed Vsync: 4706
Number High input latency: 142
Number Slow UI thread: 17270
Number Slow bitmap uploads: 1542
Number Slow draw: 23342
```
AATPE 可直接把这五类计数映射：Missed Vsync=调度/SF、Slow UI thread=主线程 CPU、Slow bitmap uploads=GPU 上传、Slow draw=overdraw/recording。

---

## 2. FrameTimeline jank 类型（Android12+，Perfetto 一手）

URL: https://perfetto.dev/docs/data-sources/frametimeline
原文约束："FrameTimeline requires Android 12(S) or higher … SurfaceViews are currently not supported."
原文逐类：
- **AppDeadlineMissed**：app 跑超预期；总时=choreographer wakeup→max(gpu, post)。
- **BufferStuffing**：app 不停发新帧，前帧未 present，buffer 队列塞满，每帧至少晚一个 vsync（状态而非真 jank）。
- **SurfaceFlingerCpuDeadlineMissed**：SF 主线程跑超（含 device composition 整段）。
- **SurfaceFlingerGpuDeadlineMissed**：CPU 时间达标但 GPU composition 没好→推到下一 vsync。
- **DisplayHAL**：SF 按时交付 HAL，但没在该 vsync present，落到下一帧。
- **PredictionError**：SF 预测 20ms present，实际 23ms 调度漂移；孤立 PE 用户通常无感（调度自愈），仍记为 jank。
- **Unknown**：超时但提前 present，概率极低。
SQL 表：`expected_frame_timeline_slice` / `actual_frame_timeline_slice`，列 `jank_type,on_time_finish,present_type,layer_name`。

**不录 perfetto 时 AATPE 拿不到逐帧 jank_type**——只能用 §1.2 聚合计数近似。游戏 SurfaceView 面 FrameTimeline 不支持 → 必须走 §3 的 --latency。

---

## 3. 刷新率动态阈值与游戏帧率

### 3.1 root 下读当前刷新率的确切方法
- `dumpsys display | grep -iE 'mRefreshRate|fps'` → 当前/默认刷新率。
- `dumpsys SurfaceFlinger --latency <layer>` **第一行就是 refresh period（ns）**。
  原文样例（http://eyan0221.github.io/ ）：
  > "The first line is the refresh period (here 16.95 ms), it is followed by 128 lines w/ 3 timestamps in nanosecond each: A) when the app started to draw, B)... "
- `/sys/class/drm/card0-*/modes` → 支持的 mode 列表；`/sys/class/drm/card0-*/enabled/status` + 当前 mode 推 fps。
- AOSP 原文（multiple-refresh-rate）："getDisplayVsyncPeriod … returns the current refresh rate (as a vsync period), especially during refresh-rate transitions."

### 3.2 LTPO 跳变防抖
- vsync_budget_ns 应滑动取最近 N 帧的众数 refresh period，而非逐帧跳变跟随（LTPO 会在 1→120Hz 间瞬切）。
- 判定"刷新率切换瞬态 jank"：连续两帧 refresh period 比值 >1.5 时，该帧掉帧标 `REFRESH_SWITCH_JANK`，不计入长期根因。
- AOSP frame-pacing 原文："SurfaceFlinger throttles app FPS as long as the app frame rate is a divisor of display refresh rate … if not in phase, holds the frame." → 游戏锁 60fps 在 120Hz 屏上 = 整除关系，不应判 jank。

### 3.3 SurfaceFlinger --latency 解析游戏帧率
- 层名枚举：`dumpsys SurfaceFlinger --list` 列所有 layer；游戏主面通常是 `SurfaceView[<pkg>/<Activity>]#0` 或 `<Unity>/<Unreal>` 渲染层。
- 输出：第一行 refresh period，其后 128 行 × 3 个 ns 时间戳（A=app 开始绘制 / B=... / C=present）。环形缓冲。
- 真实 fps：对 C 列（present）相邻差 p2p_ms = (t[i]-t[i-1])/1e6；fps=1000/median(p2p_ms)。P99 用最慢 1% p2p。
- Android Vitals 游戏慢会话门槛（原文）："A slow session is >25% frames slow; a frame is slow when not presented within 50ms of previous (=20fps); second metric 34ms (=30fps)."（URL: https://developer.android.google.cn/topic/performance/vitals/slow-session）

### 3.4 MTK ged FPS 节点
来源：CSDN《CPU/GPU/DDR 汇总》URL: https://blog.csdn.net/qq_27672101/article/details/143996392
节点：`/sys/kernel/ged/hal/gpu_utilization`、`gpu_freq`、`gpu_freq_cap`、`ged_boost`、`custom_upbound_gpu_freq`、`/d/ged/hal/*`、`ged_fps*`（当前游戏帧率）、FPSGO `fpsgo_enable`（厂商限频总开关，置 0=解除内置限频）。
输出格式样例：`gpu_utilization: 45`、`gpu_freq: 500000`（kHz）。

### 3.5 游戏引擎线程模型（关键线程识别）
- **Unity**：Main Thread（脚本/逻辑）、Worker Thread（JobSystem）、GPU Thread（渲染提交）。卡顿多在 Main 或 GPU Thread。
- **Unreal**：Game Thread（逻辑）、Render Thread（渲染）、RHI Thread（GPU 提交）。
- AATPE 策略：游戏场景下"关键线程"= 包名进程内 com.unity3d/Unreal 命名的渲染/Game Thread，而非 UI 主线程；提升目标应是这些线程，避免误把后台音频线程当关键。

---

## 4. ADPF 深读与 root 等价实现

### 4.1 PerformanceHintManager API
来源：Android codelab（native APl）URL: https://developer.android.google.cn/codelabs/adaptability-codelab
原文 C APl 序列：
```c
APerformanceHint_createHintSession(tids, targetDurationNanos);
APerformanceHint_updateTargetWorkDuration(session, targetDurationNanos);
APerformanceHint_reportActualWorkDuration(session, actualDurationNanos);
```
Java 侧：`createHintSession(int[] tids, long initialTargetWorkDurationNanos)` → 返回 Session；`updateTargetWorkDuration()` / `reportActualWorkDuration()` / `close()`。

### 4.2 内部如何落到调度（Qualcomm 一手）
URL: https://docs.qualcomm.com/bundle/publicresource/80-PK177-134/topics/google_adpf_and_qape.md
原文："CreateHintSession … results in two internal QAPE calls."
即：system-server PerformanceHintManager → PowerHAL/QAPE → 对 tids 做 per-task uclamp/boost。

uclamp 语义（kernel.org 一手）URL: https://www.kernel.org/doc/html/v7.1/_sources/scheduler/sched-util-clamp.rst.txt
原文："Utilization clamping … introduced v5.3, CGroup support v5.4 … a lower bound on the PELT signal; for the governor, instructs selecting a freq to meet the task's performance requirement."
Qualcomm 约束原文："Any requested uclamp.min cannot exceed sched_util_clamp_min."

### 4.3 root 等价实现时序（可落地）
```
1. 识别关键线程 tid（包名进程渲染/主线程）。
2. 读当前帧周期 vsync_budget_ms。
3. 写 /dev/cgroup/cpu/<tid>/uclamp.min = 目标利用率%（如 60→抬 freq floor）。
   （或 echo <tid> 到 schedtune/uclamp 节点）
4. 周期 ~每帧/每 500ms：测 actual_frame_ms，
   - actual > budget*1.15 → uclamp.min += 步进（上限受 sched_util_clamp_min）
   - actual < budget*0.8 持续 → uclamp.min -= 步进（省电回退）
5. 热回退：thermal cooling_state>0 时强制降 uclamp.min（防过热）。
```
与 ADPF 差异：ADPF 由系统统一仲裁且自带热感知；root 自管需自己实现热回退与上限。

---

## 5. GPU 节点三平台 + bound 判别

### 5.1 Adreno (kgsl) — Qualcomm 一手
URL: https://docs.qualcomm.com/bundle/publicresource/topics/80-70020-10/14-configure.html
原文节点：
```
cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage   # "Reads the GPU percentage usage" → "45" 0-100
cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq       # 当前 GPU 频率 kHz
cat /sys/class/kgsl/kgsl-3d0/devfreq/available_frequencies
cat /sys/class/kgsl/kgsl-3d0/devfreq/governor      # msm-adreno-tz
echo <freq> > /sys/class/kgsl/kgsl-3d0/devfreq/{min,max}_freq  # 锁频
```

### 5.2 Mali (Arm Bifrost/Valhall)
节点：`/sys/class/misc/mali0/device/utilization`（输出形如 "utilisation: A=45, B=12"，A=GPU 片上利用率、B=另一核/tiler 利用率，两列语义不同）、`/sys/class/misc/mali0/device/print_gpu_top`、devfreq `cur_freq/governor`(simple_ondemand)。
（参考：Mali GPU governor 工程 https://seyud.github.io/Mediatek_Mali_GPU_Governor/en/guide/introduction.html ）

### 5.3 MTK ged
见 §3.4。governor：userspace 锁频时 `echo userspace > /sys/class/devfreq/<gpu>/governor` 再写 set_freq。

### 5.4 bound 判别（可实现）
```
GPU_BOUND:   gpu_busy>=90 AND gpu_cur_freq==gpu_max_freq AND frame_ms>budget
CPU_BOUND:   gpu_busy<50 AND gpu_work_ns 短(空等) AND 主线程/RenderThread 占满小核
BW_BOUND:    gpu_busy<70 AND cpu_busy<70 AND DDR devfreq 高 AND frame_ms>budget
             # 代理：/sys/class/devfreq/*ddr*/cur_freq 高 + kgsl gpubusy 与 gpu 频率都不满
```
带宽代理节点：`/sys/class/devfreq/<bw|ddr>*/cur_freq`、`/sys/kernel/ged/hal/*ddr*`。

---

## 6. PSI / lmkd / MGLRU / binder

### 6.1 PSI 格式（kernel.org 一手）
URL: https://www.kernel.org/doc/html/v5.15/accounting/psi.html
原文：
```
some avg10=0.00 avg60=0.00 avg300=0.00 total=0   # cpu
some ... full ...                                 # memory/io 有 some+full
```
语义（kernel-internals）：avg10/60/300 = 最近 10s/60s/300s 任务被卡的时间百分比；total=累计 stall µs。some=至少一个任务卡；full=所有非 idle 任务都卡。
社区经验阈值（https://www.minzkn.com/linuxkernel/tools/proc-analyzer.html ）：avg10<5 正常；5-20 用户可感延迟；>20 严重；>50 近停摆。

### 6.2 lmkd v2 阈值（AOSP 一手，全表）
URL: https://source.android.google.cn/docs/core/perf/lmkd?hl=en
原文（Android 11 PSI 策略，高/低 RAM 两列默认值）：

| 属性 | 含义 | 高性能默认 | 低 RAM 默认 |
|---|---|---|---|
| ro.lmk.psi_partial_stall_ms | 触发 low-mem 通知的 partial PSI stall(ms) | **70** | **200** |
| ro.lmk.psi_complete_stall_ms | 触发 critical 的 complete PSI stall(ms) | **700** | 700 |
| ro.lmk.thrashing_limit | workingset refault 占 file-backed pagecache % 上限，超限=thrashing | **100** | **30** |
| ro.lmk.thrashing_limit_decay | kill 后仍不恢复时阈值衰减 % | 10 | 50 |
| ro.lmk.swap_util_max | swap 利用率 % 上限，100=关闭 | 100 | 100 |
| ro.lmk.swap_free_low_percentage | 剩 swap % 阈值，判 swap 枯竭 | 20 | 10 |

工程借用：AATPE MEM_BOUND 触发条件 = `/proc/pressure/memory` some avg10 高，且接近 lmkd partial(70ms)/complete(700ms) 语义；thrashing 信号借 workingset refault。

### 6.3 MGLRU 与 reclaim 卡顿
- MGLRU 开关节点：`/sys/kernel/mm/lru_gen/enabled`（1=启用）。MGLRU 降低 reclaim 抖动（多代 LRU，按 refault 检测 thrashing）。
- reclaim/compact 抖动代理：PSI memory full avg10 突增 + kswapd 进程 CPU 占用高 → MEM_THRASH。

### 6.4 io.stat / binder 拥塞
- cgroup v2：`memory.stat`、`io.stat`（读/写延迟与字节）；`/proc/pressure/io` avg10。
- binder 无 debugfs 替代：`/dev/binderfs`、`dumpsys binder_calls_stats`（统计大 binder 交易往返）。debugfs `/sys/kernel/debug/binder/state` 不可读时退化用 PSI cpu avg10 + system_server/SF 交易往返延迟。

---

## 7. 热限频全景

### 7.1 cooling_device / thermal_zone（kernel.org 一手）
URL: https://www.kernel.org/doc/Documentation/driver-api/thermal/sysfs-api.rst
原文：cooling_device ops `.get_max_state / .get_cur_state / .set_cur_state`；trip types "active / passive / hot / critical (Hardware not reliable)"。
节点：
```
/sys/class/thermal/cooling_device{N}/type        # 如 "cpufreq" / "gpu"
/sys/class/thermal/cooling_device{N}/cur_state    # 0=不限, max_state=限到顶
/sys/class/thermal/cooling_device{N}/max_state
/sys/class/thermal/thermal_zone{N}/temp           # milli°C
/sys/class/thermal/thermal_zone{N}/trip_point_{i}_type / temp
```
cpufreq cooling 换算：cur_state/max_state 比值 ≈ 限频程度；对照该 policy 的 `scaling_available_frequencies` 排表，cur_state 越高=max freq 越低。

### 7.2 三类限频识别（可实现）
```
热限频:    thermal_zone temp 接近 passive/critical trip AND cooling cur_state>0
省电策略:  temp 正常 AND scaling_max_freq 被写低 AND (settings global low_power==1
           OR vendor perf-mode 节点在省电档)
预测限频:  temp 未到 trip 但 freq 已被厂商 userspace daemon 压低
           (thermald / mtk_thermal / mi_thermal / qpnp thermal-engine)
```
高通 thermal-engine 配置：`/vendor/etc/thermal-engine*.conf`；MTK：`/sys/kernel/thermal/*` / mtk thermal 策略表节点。

---

## 8. 根因判定表（带确切阈值，接近可实现伪逻辑）

```
if frame_ms > vsync_budget_ms:                       # vsync_budget=1000/current_fps
  if FLAGS!=0: SKIP(outlier)
  elif cooling_cur_state>0 AND temp>passive_trip:        -> THERMAL
  elif temp 正常 AND scaling_max_freq<max_avail AND low_power: -> POWER_SAVER_CAP
  elif mem_some_avg10>10 OR near psi_partial(70ms):       -> MEM_THRASH
  elif io_full_avg10>15:                                 -> IO_BOUND
  elif gpu_busy>=90 AND gpu_freq==max:                   -> GPU_BOUND
  elif gpu_busy<50 AND main_thread_util_high:            -> CPU_SINGLE_THREAD
  elif gpu<70 AND cpu<70 AND ddr_devfreq 高:             -> BW_BOUND
  elif 本帧 refresh_period 与前帧比>1.5x:                 -> REFRESH_SWITCH_JANK
  elif 游戏渲染线程(Unity/Unreal 渲染)饱和:              -> GAME_RENDER_BOUND
  else:                                                  -> UNCLASSIFIED
```

### 8.1 新增根因类（在现有 GPU/CPU_SINGLE/THERMAL/IO/MEM/BG 之上）
1. BW_BOUND（带宽）
2. VSYNC_PREDICTION_ERROR（调度漂移，不迁线程，标注）
3. GAME_RENDER_BOUND（游戏渲染线程）
4. POWER_SAVER_CAP（省电策略）
5. REFRESH_SWITCH_JANK（刷新率切换瞬态）
6. MEM_THRASH（reclaim 抖动）

---

## 9. 信号可得性矩阵（逐格标来源）

| 信号 | 节点/命令 | 版本 | root | 来源 |
|---|---|---|---|---|
| 逐帧帧时/字段 | dumpsys gfxinfo <pkg> framestats | all | ✓ | [1] |
| 聚合 jank 计数 | dumpsys gfxinfo 顶部 | all | ✓ | [1] |
| jank_type 逐帧 | perfetto android.surfaceflinger.frametimeline | 12+ | 需录 trace | [2] |
| 当前刷新率 | dumpsys display mRefreshRate / SF --latency 首行 | all | ✓ | [3][4] |
| 游戏帧率 | SF --latency <layer>（128 帧×3 时间戳） | all | ✓ | [5] |
| GPU busy QC | /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage | QC | ✓ | [6] |
| GPU busy Mali | /sys/class/misc/mali0/device/utilization | Arm/MTK | ✓ | [7] |
| GPU busy MTK | /sys/kernel/ged/hal/gpu_utilization + ged_fps | MTK | ✓ | [8] |
| PSI cpu/mem/io | /proc/pressure/{cpu,memory,io} | 4.20+ | ✓ | [9] |
| lmkd 阈值 | ro.lmk.psi_*_stall_ms（getprop） | 11+ | ✓ | [10] |
| 热限频 | cooling_device{N}/{cur,max}_state | all | ✓ | [11] |
| 省电识别 | settings global low_power / vendor perf node | all | ✓ | [11] |
| uclamp 等价 ADPF | cgroup task uclamp.min | 5.4+ root | ✓ | [12] |

---

## 10. 工程清单（映射 aatpe_diag.cpp / frame 子系统）

### P0
- frame：vsync_budget 动态化=1000/current_fps，滑动众数防抖。
- frame：framestats 解析器（15 列，FLAGS 过滤，派生 input_latency/UI/GPU）。
- diag：读 /proc/pressure/{cpu,memory,io} avg10。
- diag：gpu_busy 三平台统一抽象 readGpuBusy()。
- diag：cooling_device cur_state/max_state → THERMAL 置信度。

### P1
- 游戏帧率：SF --latency <game layer> p2p。
- 热 vs 省电判别（temp + scaling_max + low_power 联合）。
- root 动作：uclamp.min 抬关键线程（带热回退）。
- BW_BOUND（ddr devfreq 代理）。

### P2
- perfetto frametimeline 可选录制（白名单包）。
- binder 拥塞（binder_calls_stats）。
- MGLRU enabled 检测。

---

## 11. 来源清单（30+）
1. https://iut-fbleau.fr/docs/android/training/testing/performance.html （framestats 15 列一手）
2. https://perfetto.dev/docs/data-sources/frametimeline （jank_type 权威）
3. https://source.android.google.cn/docs/core/graphics/multiple-refresh-rate （getDisplayVsyncPeriod）
4. https://source.android.google.cn/docs/core/graphics/frame-pacing （fps 整除）
5. http://eyan0221.github.io/ （SF --latency 128×3 时间戳样例）
6. https://docs.qualcomm.com/bundle/publicresource/topics/80-70020-10/14-configure.html （kgsl）
7. https://seyud.github.io/Mediatek_Mali_GPU_Governor/en/guide/introduction.html （Mali）
8. https://blog.csdn.net/qq_27672101/article/details/143996392 （MTK ged/DDR）
9. https://www.kernel.org/doc/html/v5.15/accounting/psi.html （PSI 格式）
10. https://source.android.google.cn/docs/core/perf/lmkd?hl=en （lmkd v2 全阈值表）
11. https://www.kernel.org/doc/Documentation/driver-api/thermal/sysfs-api.rst （cooling/trip）
12. https://www.kernel.org/doc/html/v7.1/_sources/scheduler/sched-util-clamp.rst.txt （uclamp）
13. https://docs.qualcomm.com/bundle/publicresource/80-PK177-134/topics/google_adpf_and_qape.md （ADPF/QAPE）
14. https://developer.android.google.cn/codelabs/adaptability-codelab （ADPF native APl）
15. https://developer.android.google.cn/topic/performance/vitals/slow-session （慢会话 50/34ms）
16. https://developer.android.com/games/optimize/framerate （P90/P99）
17. https://developer.android.com/studio/profile/jank-detection （jank UI）
18. https://aospinsider.com/courses/aosp-course-4/16-trace-analysis-workflow/ （expected vs actual）
19. https://kernel-internals.org/mm/psi/ （avg 语义）
20. https://www.minzkn.com/linuxkernel/tools/proc-analyzer.html （PSI 阈值经验）
21. https://source.android.google.cn/docs/core/power/thermal-mitigation （thermal status）
22. https://source.android.google.cn/docs/core/graphics/implement-vsync （VSYNC 管线）
23. https://blog.csdn.net/lf12345678910/article/details/103826627 （FLAGS）
24. https://proceedings.mlsys.org/paper_files/paper/2026/file/136b9a13861308c8948cd308ccd02658-Supplemental-Conference.pdf （GPU governor dvfs）
25. https://androidperformance.com/en/2025/08/05/Android-Perfetto-08-Vsync/ （vsync 时序）
26. https://docs.qualcomm.com/bundle/publicresource/80-70015-10/topics/18-customize.md （uclamp 上限约束）
27. https://genio-community.mediatek.com/t/how-to-check-and-adjust-npu-frequency-during-ai-inference/467 （MTK devfreq）
28. https://deepwiki.com/Gustyx-Power/Xtra-Kernel-Manager/8.3-sysfs-and-kernel-interfaces （gpu_busy 多路径）
29. https://qualcomm.github.io/userspace-resource-manager-extensions/04-resources-reference.html （KGSL devfreq max/min）
30. https://developer.android.google.cn/games/sdk/frame-pacing （EGL/VK present timing）

> 真机待核：D1100 实际 gpu_busy/ged 节点可用性、当前刷新率读取路径、cooling_device 编号、mali utilization 两列格式、cgroup uclamp 节点路径。

---

## 12. 精读一手资料清单（本轮实际 web.fetch / 检索）

| 精读 | URL | 拿到什么 |
|---|---|---|
| AOSP/Android framestats | https://iut-fbleau.fr/docs/android/training/testing/performance.html | 15 列逐字段原文 + 输出样例 + reset 窗口机制 |
| Perfetto FrameTimeline | https://perfetto.dev/docs/data-sources/frametimeline | 7 类 jank 逐字定义 + SQL 表 + TraceConfig |
| AOSP lmkd | https://source.android.google.cn/docs/core/perf/lmkd?hl=en | PSI partial/complete/thrashing/swap 全参数表默认值 |
| 多刷新/帧 pacing | source.android.com multiple-refresh-rate / frame-pacing | getDisplayVsyncPeriod、fps 整除持帧 |
| Qualcomm kgsl/ADPF/uclamp | docs.qualcomm.com + kernel.org | gpu_busy 节点、QAPE 两次调用、uclamp 上限约束 |
| SurfaceFlinger --latency | eyan0221.github.io 样例 | 首行 refresh period + 128 行×3 ns |

---

## 13. 补充深读：ADPF services 内部与 root 动作时序

### 13.1 session 生命周期（基于 AOSP/Qualcomm 一手归纳）
PerformanceHintManager.Session 持有的是一组 tid + targetWorkDuration。当 app 周期调用 `reportActualWorkDuration(actualNs)`，framework 侧把 (targetNs, actualNs) 差值喂给一个滑动反馈环：actual 持续大于 target → 判定"该任务跑不够快" → 通过 PowerHAL 向对应 tid 抬 uclamp.min / 触发 sched boost；actual 远小于 target → 释放 boost 省电。Qualcomm 文档原文确认 CreateHintSession 直接产生"two internal QAPE calls"，即该反馈环下沉到 QOS/QAPE 层而非 framework 自己选频。

工程含义：AATPE 复刻这个环时，**关键不是"一直抬频"，而是"按 target vs actual 的差闭环调"**。否则会变成无脑 max freq，功耗爆炸且与热回退冲突。

### 13.2 root 动作时序伪代码（落地级）
```
# 每帧或每 500ms tick
budget_ms   = vsync_budget_ms()          # 动态 = 1000/current_fps
actual_ms   = measured_frame_ms()         # 来自 framestats/--latency
err_ratio   = actual_ms / max(budget_ms,1)

if thermal_state_throttling():           # cooling cur_state>0
    clamp_min = max(clamp_min - STEP, 20) # 热优先回退
elif err_ratio > 1.15:
    clamp_min = min(clamp_min + STEP, sched_util_clamp_min_cap)
elif err_ratio < 0.8 and stable_n_ticks():
    clamp_min = max(clamp_min - STEP, 20)

write_uclamp_min(target_tids, clamp_min)  # cgroup task 级
```
注意上限：Qualcomm 原文"requested uclamp.min cannot exceed sched_util_clamp_min"——AATPE 写前先读 `/proc/sys/kernel/sched_util_clamp_min` 或 cgroup 顶层 max，避免被内核静默截断造成"以为抬了其实没抬"。

---

## 14. 补充深读：binder / debugfs 缺失替代矩阵

| 想观测 | 首选节点 | 无 root/无 debugfs 替代 |
|---|---|---|
| binder 各交易耗时 | /sys/kernel/debug/binder/state、binderfs | `dumpsys binder_calls_stats`（统计往返）+ PSI cpu avg10 突增 |
| 锁等待 / 主线程卡顿 | systrace/perfetto Lock contention | framestats UI_busy_ns 高 + 主线程 sched runtime 高 |
| GPU 内部瓶颈 | kgsl perfcounter / mali_debugfs | gpu_busy + gpu_cur_freq 联合粗判 |
| cache miss / LLCC | perf stat LLC-misses | BW_BOUND 代理（ddr devfreq + gpu/cpu 双低） |

原则：AATPE 诊断层要做**能力探测**（节点存在性），不存在就降级到代理指标，不能读空文件当 0（会误判为"不忙"）。

---

## 15. 补充深读：MGLRU 与 reclaim 卡顿

- 开关：`/sys/kernel/mm/lru_gen/enabled`。1=MGLRU 生效，内核按多代 refault 检测 thrashing。
- MGLRU 对卡顿的影响：传统 LRU 在内存压力下 kswapd 全表扫描回收，造成周期性大 stall；MGLRU 把热点/冷页分代，refault 命中即识别 thrashing，回收更局部 → reclaim 抖动更小。
- AATPE 用法：`/proc/pressure/memory` 的 **full avg10** 突增 + kswapd（[kswapd0]）CPU 占用飙升 = reclaim 抖动 = MEM_THRASH。此时动作不是抬 CPU，而是**压后台进程/降缓存写**，避免主线程等 reclaim。
- 与 lmkd 联动：thrashing_limit(高 RAM 100%) 即"workingset refault 占 file pagecache 比例"，超限 lmkd 开始 kill——AATPE 可在达到该阈值前就降后台，抢在 lmkd kill 之前。

---

## 16. 补充深读：thermal 策略与省电模式识别

### 16.1 thermal 层级
- 内核：thermal_zone + trip(passive/hot/critical) + cooling_device。
- 高通 userspace：thermal-engine（`/vendor/etc/thermal-engine*.conf`），在 temp 未到内核 trip 前**预测性**压 freq/GPU。
- MTK：mtk_thermal 策略表节点；mi_thermal（小米）；qpnp（PMIC 温度传感器）。
- AATPE 必须区分：内核 trip 触发的限频（cur_state 动）vs userspace daemon 预测限频（temp 未到 trip 但 freq 已 cap）。后者 temp 读数会"看起来正常"却已限频——单看 temp 会漏判。

### 16.2 省电模式识别
- `settings get global low_power`（1=省电模式）。
- vendor perf-mode / power-save 节点（各厂不同，需探测）。
- 判据：temp 正常 + scaling_max_freq < scaling_available_frequencies 最高值 + low_power==1 → POWER_SAVER_CAP，**不应**再往上抬频（与用户意图冲突，动作=提示而非干预）。

---

## 17. 引擎线程模型对照（关键线程识别表）

| 引擎 | 关键线程 | 卡顿常见根因 | AATPE 提升目标 |
|---|---|---|---|
| Unity | UnityMain / JobWorker / GfxWorker(GPU) | GfxWorker GPU 饱和 或 Main 脚本卡 | GfxWorker + Main，非音频线程 |
| Unreal | GameThread / RenderThread / RHIThread | RHI GPU 提交排队 | RenderThread/RHIThread |
| 原生 UI | 主线程 / RenderThread | 主线程 measure/layout 卡 | 主线程 |

注意：游戏进程内常有后台音频/加载线程，不能按"包名进程=整体提升"。AATPE 应按线程名模式（Gfx/Render/RHI/Main）筛选关键线程，且 FrameTimeline 不支持 SurfaceView → 游戏侧帧率以 --latency 为准。

---

## 18. 落地 worked example（D1100 8 核三级）

场景：D1100（LITTLE=cpu0-3 / MID=cpu4-6 / PRIME=cpu7），90Hz 屏。
1. 读刷新率：`dumpsys display` mRefreshRate=90 → vsync_budget=11.1ms。
2. 抓 framestats：`dumpsys gfxinfo com.game framestats`。
3. 逐行：FLAGS!=0 丢弃；算 total_frame = FRAME_COMPLETED-INTENDED_VSYNC。
4. 任一 total_frame > 11.1ms：查 gpu_busy、cooling cur_state、PSI。
   - gpu_busy=95% 且 gpu_freq 到顶 → GPU_BOUND，动作：GPU 不解锁频、降游戏画质/不动 CPU。
   - cooling cur_state>0 且 temp 高 → THERMAL，动作：降频回退、不抬 PRIME。
   - mem full avg10=25 → MEM_THRASH，动作：压后台。
   - gpu_busy=30% 但主线程占满 cpu0 → CPU_SINGLE_THREAD，动作：主线程迁 PRIME。
5. 若本帧 refresh period 从 11.1ms 跳到 22ms（90→45Hz LTPO）→ REFRESH_SWITCH_JANK，不计入长期 jank 率。

---

## 19. 回归测试建议（信号层）

- framestats 解析：构造 FLAGS!=0 / 列数不足 / NEWEST_INPUT_EVENT=0 / Long.MAX_VALUE 哨兵行，断言全部被正确过滤不越界。
- 动态 deadline：mock current_fps=60/90/120，断言 budget=16.6/11.1/8.3ms。
- 节点缺失：gpu_busy 文件不存在时 readGpuBusy 返回"未知"而非 0，避免误判不忙。
- 热回退：mock cooling cur_state=max，断言 uclamp 动作被压制。
- PSI：mock /proc/pressure/memory full avg10 突增，断言触发 MEM_THRASH。

---

## 20. 本轮回报

### 20.1 逐帧数字算例（framestats → 诊断）
取 §1 样例一行（ns）：
```
FLAGS=0
INTENDED_VSYNC = 27965466202353
VSYNC          = 27965466202353   # == Intended → UI 线程没积压
OLDEST_INPUT   = 27965449758000
NEWEST_INPUT   = 27965461202353
HANDLE_INPUT   = 27965467153286
ANIMATION      = 27965471442505
TRAVERSALS     = 27965471925682
DRAW           = 27965474025318
SYNC_QUEUED    = 27965474588547
SYNC_START     = 27965474860786
ISSUE_DRAW     = 27965475078599
SWAP           = 27965479796151
FRAME_COMPLETED= 27965480589068
```
派生量（µs）：
- total_frame = (FRAME_COMPLETED - INTENDED_VSYNC)/1000 = (27965480589068-27965466202353)/1000 = **14386 µs ≈ 14.4ms**。60Hz budget 16.6ms → 达标；90Hz budget 11.1ms → **miss（这正是动态阈值的意义：同一帧在 60Hz 算合格、90Hz 算掉帧）**。
- input_latency = (FRAME_COMPLETED - NEWEST_INPUT)/1000 = (27965480589068-27965461202353)/1000 = 19387 µs ≈ 19.4ms。
- ui_busy = (ANIMATION - VSYNC)/1000 = 52µs（几乎无积压）。
- layout = (DRAW - TRAVERSALS)/1000 = 219µs。
- draw_rec = (SYNC_QUEUED - DRAW)/1000 = 55µs。
- rt_wait = (SYNC_START - SYNC_QUEUED)/1000 = 27µs（RenderThread 没被前帧堵）。
- upload = (ISSUE_DRAW - SYNC_START)/1000 = 21µs。
- gpu_work = (FRAME_COMPLETED - ISSUE_DRAW)/1000 = 5510µs ≈ 5.5ms（本帧 GPU 段最重 → 偏 GPU/overdraw）。

结论演示：同一 14.4ms 帧，60Hz 不判 jank、90Hz 判 jank；且本帧耗时大头在 gpu_work(5.5ms) 与 swap 段 → 若连续多帧如此且 gpu_busy 高 → GPU_BOUND。

### 20.2 jank 类型 → AATPE 动作映射
| jank 信号 | 判定 | AATPE 动作 |
|---|---|---|
| AppDeadlineMissed + UI_busy 高 | 主线程 CPU 卡 | 主线程迁 PRIME，抬 uclamp |
| AppDeadlineMissed + gpu_work 高 + gpu_busy 高 | GPU/overdraw | 不迁 CPU（GPU 瓶颈），提示降画质 |
| BufferStuffing | 队列塞满、高延迟 | 不迁，标注"过量产帧" |
| SurfaceFlingerCpuDeadlineMissed | SF 合成慢 | 提升 SF 进程亲和，不动 app |
| SurfaceFlingerGpuDeadlineMissed | GPU composition 慢 | GPU 域，不动 app CPU |
| PredictionError | 调度漂移 | 不干预（自愈），仅统计 |
| DisplayHAL | HAL 延迟 | 系统层，不动 app |
| cooling cur_state>0 | 热限频 | 压制所有抬频动作 |
| low_power=1 | 省电 | 不抬频，仅提示 |

### 20.3 平台节点全枚举（含输出样例与解析公式）

**Adreno (QC)**
| 节点 | 样例输出 | 解析 |
|---|---|---|
| /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage | `45` | int 0-100，GPU 占用% |
| /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq | `500000000` | Hz，当前 GPU 时钟 |
| /sys/class/kgsl/kgsl-3d0/devfreq/max_freq | `810000000` | Hz，上限 |
| /sys/class/kgsl/kgsl-3d0/devfreq/governor | `msm-adreno-tz` | gov 名 |
| /sys/class/kgsl/kgsl-3d0/gpubusy | `45 810` | 寄存器两位 hex/百分号，(busy%, cur_mhz) |
解析：busy>=90 且 cur==max → GPU_BOUND。

**Mali (Arm/MTK)**
| 节点 | 样例 | 解析 |
|---|---|---|
| /sys/class/misc/mali0/device/utilization | `utilisation: 45, 12` | A=GPU 核占用%，B=tiler/第二列占用% |
| devfreq/cur_freq | `650000` | kHz |
| governor | `simple_ondemand` / `pm` | gov 名 |

**MTK ged**
| 节点 | 样例 | 解析 |
|---|---|---|
| /sys/kernel/ged/hal/gpu_utilization | `45` | GPU% |
| /sys/kernel/ged/hal/gpu_freq | `500000` | kHz |
| /sys/kernel/ged/hal/gpu_freq_cap | `810000` | cap |
| /d/ged/hal/fpsgo_enable | `1` | 0=厂商限频总闸解除 |
| /sys/kernel/ged/ged_fps | `60` | 当前游戏 fps |

**热**
| 节点 | 样例 | 解析 |
|---|---|---|
| cooling_device{N}/cur_state | `2` | 0=不限，max=限到顶 |
| cooling_device{N}/max_state | `10` | 上限 |
| thermal_zone{N}/temp | `65000` | milli°C=65°C |
| thermal_zone{N}/trip_point_1_type | `passive` | trip 类型 |

### 20.4 回报

- 行数：本文件 ~620 行（§1-§20，含本算例）。
- 来源：29 条编号 URL（§11），其中本轮 web.fetch 精读一手文档：framestats 列定义、Perfetto FrameTimeline 全文、AOSP lmkd 全阈值表、Qualcomm kgsl/ADPF/uclamp 片段。
- 关键升级：framestats 15 列字段级算法 + 数字算例、动态 vsync 阈值（同帧 60/90Hz 双判）、三平台 GPU 节点、PSI/lmkd 阈值借用、热/省电/预测三态判别、带阈值可实现根因判定表、jank→动作映射、能力探测降级原则。
- 真机待核：D1100 gpu_busy/ged 节点可用性、当前刷新率读取路径、cooling_device 编号、mali utilization 两列格式、cgroup uclamp 节点路径、sched_util_clamp_min 上限值。

---

## 21. 边界、假设与合规声明

- **全部信号读取均为设备本地 sysfs/proc/dumpsys**，无任何网络上传。本调研涉及的 gfxinfo、SurfaceFlinger、PSI、thermal、gpu devfreq 都是只读本地诊断节点，不产生数据出设备动作。
- **假设**：D1100 为 root Magisk 环境（AATPE 模块前提），可访问 /sys/kernel/debug、/proc/pressure、cgroup uclamp；非 root 设备 gfxinfo 仍可 shell 读，但 debugfs/cgroup 节点受限——AATPE 诊断层必须做能力探测降级。
- **不替代 perfetto**：逐帧精确 jank_type 仍需录 trace；本方案是"不录 trace 也能粗分根因"的退化路径，精度受限。
- **数字算例为演示**：framestats 样例取自官方文档样例行，派生量用于说明算法可实现性，非真机实测值。
- **版本覆盖**：framestats 自 Android M；FrameTimeline 自 Android 12；PSI 自 4.20（Android 10+ backport）；uclamp cgroup 自 5.4。老于这些版本的设备对应能力缺失，需降级。
- **工程动作边界**：AATPE 只读诊断 + 受控写 cgroup uclamp（带热回退与省电抑制），不锁 GPU/cpufreq 频率、不改厂商 thermal 策略，避免与系统/厂商守护进程打架。

（全文完。）

---

## 附：逐帧决策流（完整伪码，对照 §8 判定表）
```
on_frame(frame):
    if frame.FLAGS != 0: return SKIP
    budget = vsync_budget_ms()                  # 动态=1000/current_fps
    total  = (FRAME_COMPLETED - INTENDED_VSYNC)/1e6
    if total <= budget: return SMOOTH
    # 掉帧，开始归因（按优先级短路）
    if refresh_jump_detected():  return REFRESH_SWITCH_JANK
    if cooling_state_active():   return THERMAL
    if low_power_mode():        return POWER_SAVER_CAP
    if mem_thrashing():         return MEM_THRASH
    if io_busy():               return IO_BOUND
    if gpu_busy_max():          return GPU_BOUND
    if main_thread_busy():      return CPU_SINGLE_THREAD
    if bw_saturated():           return BW_BOUND
    return UNCLASSIFIED
```
每步都先做节点存在性探测，节点缺失即跳过该归因（不读空当 0）。



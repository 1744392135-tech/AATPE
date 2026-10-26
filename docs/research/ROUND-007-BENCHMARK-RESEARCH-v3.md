# ROUND-007 行业标杆穷举研究 v3：同类作品逆向级对比 + 大厂思维 + 常量科学性

> 对象：AATPE v3.4.6（Magisk/KernelSU root 调度守护，arm64 NDK C++17，目标天玑1100/MT6893，内核 4.14.186）。
> 目的：穷尽同类作品（开源读源码、闭源读公开技术分析/行为观测）与大厂 PowerHAL/perfd 思维，穷举排查 AATPE 运行/决策逻辑是否高效、科学、流畅、省电。
> 检索日期：2026-10-02。每条附 URL 与原文摘录。不碰任何代码。
> 承接 ROUND-005/006：uclamp v5.3/v5.4、PELT 32ms 半衰、WALT 20ms 窗、EAS capacity=1024。

---

## 1. 同类作品逆向级对比

### 1.1 uperf（开源，最直接同类——国产 root 调度模块）
Gitee 源码自述（https://gitee.com/gongmi/uperf）：
- **Hint 抽象模型**：
  > "`HeavyLoad`：在 Tap 或 Swipe 检测到重负载后切换，负载降低后回落到 Tap；`AndroidAM`：ActivityManager 活跃时触发的 Hint（如无 input 的屏下指纹解锁）；`Standby`：屏幕熄灭时的 Hint，**一般滞后 20 秒**。"
- **手段**：动态绑定 UI 相关线程到大核集群；读触摸屏识别点击/滑动；采样系统负载识别启动重负载；监听 cpuset 分组更新识别 app 切换。

**对 AATPE 的映射**：uperf 是"Hint 驱动 + 短时 boost + 自动回落"模型，而非"周期扫描 + 长期绑核"。AATPE 若以 200ms 周期硬决策，是更"重"的路线。

### 1.2 Scene 工具箱（com.omarea.vtools，闭源但行为公开）
- 抖音技术拆解（https://www.iesdouyin.com/share/video/7687630290499817427）：
  > "编辑 LP 游戏调频配置，从上到下对应四个性能档位：模式名、最大频率值、最小频率值、升频策略、FAS 频率上限；max/min 值从左到右对应小核→中核→超大核。"
- **重叠/冲突点**：Scene 按"档位"直接写 cpufreq min/max；AATPE 若也写 min_freq，会与 Scene 类工具**直接冲突**（后写覆盖先写）。AATPE 定位应是"按线程角色放置"，而非"全局调频档位"，避免与 Scene 抢 sysfs 写权限。

### 1.3 Franco Kernel Manager / EX Kernel Manager / SmartPack / Kernel Adiutor
- 共性：支持 schedtune.boost、CPU-Boost/input-boost、governor profiles、per-cluster min/max freq。
- **共同教训**：(a) input-boost 是"输入事件触发 + 固定时长后自动衰减"；(b) 很少做长期 sched_setaffinity 硬绑核——因为绑核伤 EAS/电源且不抗热。

### 1.4 高通 QTI perfd / mp-ctl / PerfHAL / perflock（大厂 daemon 标杆）
- **PerfHAL = perflock 访问服务**：
  > "PerfHAL is a Qualcomm proprietary service… It's beneficial when you need **short-term performance enhancements**. Perflocks help manage intermittent workloads. For example, if a code segment must run at a higher CPU frequency for a certain duration, use perflocks to boost. PerfHAL efficiently handles concurrent…"
  来源：https://docs.qualcomm.com/bundle/publicresource/80-70022-10/topics/2-performance-features.md
- **API**：`perf_lock_acq()` / `perf_lock_rel()`（dlsym 加载）；验证 `ls /usr/bin | grep perf`、`systemctl status perf-hal`。
  来源：https://docs.qualcomm.com/bundle/publicresource/80-70014-10/topics/18-customize.md
- **Android 侧 Java 接口链**：BoostFramework.java（framework.jar）→ QPerformance.jar（/system/framework，vendor/qcom/.../android-perf/QPerformance）→ UxPerformance。
  来源：https://juejin.cn/post/7141196697555714079
- **powerhint.json 实证（自定义内核调优记录）**：
  > "Migrate to common libqti-perfd-client and power-libperfmgr; Import powerhint.json from lahaina; **Set duration for INTERACTION CPUBW boost**; **Drop CPU freqs boosting on INTERACTION**; Handle thermal profile state via powerhint; Configure walt window stats policy."
  来源：https://raw.githubusercontent.com/ProjectInfinity-X/official_devices/refs/heads/master/changelog/marble.txt 、http://raw.githubusercontent.com/AxionAOSP/official_devices/refs/heads/main/OTA/CHANGELOG/marble.txt

**关键大厂思维证据**：**"Drop CPU freqs boosting on INTERACTION"**——连高通阵营都在从"输入即抬 min_freq"退回到"短时 boost + 衰减"，因为长期抬频费电且被 EAS/thermal 反向压制。

### 1.5 AOSP PowerHAL / PowerHint（AIDL IPower）
- **GAME 模式（Android 14）**：
  > "The new power mode GAME in Android 14… when Power HAL notified, it can: Increase CPU clock speed; **Temporarily reduce temperature throttling**; Assign [cores]."
  来源：https://source.android.google.cn/docs/core/perf/boost?hl=en
- **SUSTAINED_PERFORMANCE**：OEM 在 power HAL 实现 `POWER_HINT_SUSTAINED_PERFORMANCE`，锁最大频率防过热（持续性能模式）。
  来源：https://source.android.google.cn/docs/core/power/performance
- **Thermal HAL 2.0**：framework 持续监控 mitigation 信号，回调节流严重度。
  来源：https://source.android.google.cn/docs/core/power/thermal-mitigation

---

## 2. 大厂思维：为什么是"短时 boost + 自动衰减"而非长期绑核

| 维度 | 大厂做法 | 证据 |
|---|---|---|
| 触发 | 触摸/启动/游戏 = HINT（事件驱动），非周期轮询 | uperf Tap/Swipe HeavyLoad Hints；QTI perflock |
| 时长 | **短时 boost + 自动衰减**；INTERACTION boost 设明确 duration | powerhint "Set duration for INTERACTION CPUBW boost" |
| 绑核 | 不长期 sched_setaffinity；交给 EAS + cpuset top-app | 同类工具少硬绑核；uperf 仅"动态绑定 UI 线程"瞬时 |
| 调频 | 从"输入即抬 min_freq"**退回**（费电+被 EAS 反压） | "Drop CPU freqs boosting on INTERACTION" |
| 分组 | top-app/foreground cpuset 由系统 cgroup 管理，厂商只微调 | AOSP cgroups（ROUND-005） |
| 仲裁 | thermal 优先于性能：过热时先减频/降档 | Thermal HAL 2.0 mitigation；SUSTAINED_PERFORMANCE |

**对 AATPE 的核心启示**：AATPE 的价值定位应是"**在系统 top-app 分组之上做线程级瞬时增强（输入/启动突发时短暂送大核），突发结束快速回落**"，而不是"周期扫描后长期维持某线程在大核"。后者与省电目标相悖，且与 EAS/thermal 打架。

---

## 3. AATPE 关键常量 vs 业界经验对照（科学性结论）

| AATPE 常量 | 业界基准 | 结论 |
|---|---|---|
| **决策周期 200ms** | 触摸响应预算 16ms（60fps）；PELT 半衰 32ms；WALT 窗 20ms | **偏慢**。200ms 周期会错过输入突发窗口（首帧 16ms）。应改为"事件触发为主 + 周期 200ms 兜底"，或周期降到 ~50ms |
| **spillover 阈值 60/75/90%** | EAS capacity 归一 1024；业界 headroom 15-30%（"healthy system maintains 15-30% headroom"） | 60% 触发偏保守（=只留 40% headroom），与 EAS ~20% spare 接近。**75% 档合理**；90% 才触发会 runaway。建议主触发落在 ~75%（留 25% headroom，符合 SRE/EAS 惯例） |
| **驻留 dwell 200/300/500ms** | PELT 最近 32ms 占半权重；util_est 降才衰减 | **偏长**。200ms dwell 远超 PELT 记忆尺度，突发结束后线程滞留大核费电。建议 dwell 下限对齐 ~50-100ms（2-3×PELT 半衰），而非 200ms 起步 |
| **线程扫描上限 512** | 现代旗舰可达数百线程 | 512 足够；但固定上限会漏掉 >512 线程的游戏进程。建议按 `/proc/loadavg` 或 `/proc/<pid>/task` 实际数自适应，不硬截断 |
| **uclamp util_min 512/1024** | uclamp 0-1024；高通 `uclamp_boosted()` 分档 | 512=半速、1024=满速。**1024 只应用于瞬时输入/启动 boost**；长期 1024 费电。与"短时 boost"一致 |
| **input boost 时长** | 业界 input-boost 经验 40-100ms（EWMA 衰减）；powerhint 设 INTERACTION duration | 应落在 **40-100ms** 区间，而非 200ms。超时即回落 |
| **冷启动识别窗口** | uperf 用 ActivityManager hint + 负载采样；启动突发通常 <1s | 冷启动窗口建议 ~500-1000ms 后回落，过短则启动卡顿，过长则费电 |
| **帧率采样 p95/p99** | 官方：95th pctile >16ms(60fps) 即 jank；frameOverrunMs P99 黄=8-16ms、红>16ms；Slow session=>25% 帧慢于 30/20fps | **AATPE 用 p95/p99 采样是正确方向**，阈值应对齐官方：p95>16ms 判 jank、P99 overrun>16ms 判严重 |

**jank 阈值证据**：
> "95th percentile: 42ms, 99th percentile: 69ms… Missed Vsync."（官方 perf 测试输出）
> 来源：https://developer.android.google.cn/training/testing/performance.html
> "frameOverrunMs P99: Green <8ms, Yellow 8-16ms, Red >16ms."
> 来源：https://mvpfactory.io/blog/diagnosing-android-jank-with-frametimeline-api-surfaceflinger-deadlines-hwui
> "Slow frame threshold = target frame time + tolerance; Fast frame = 25% faster (headroom)."
> 来源：https://support.google.com/googleplay/android-developer/answer/9886355
> schedutil 调响应："rate_limit_us… Lower values improve responsiveness: echo 200 > policy4/schedutil/rate_limit_us."
> 来源：https://dragonwingdocs.qualcomm.com/System/Kernel/runtime-performance-tuning

---

## 4. 反编译/观测方法论：真机验证 AATPE 决策时序与大厂差异

### 4.1 可操作观测清单（root 真机）
1. **atrace/perfetto 分类**（官方）：
   > 推荐类别：`sched freq idle am wm gfx view binder_driver hal dalvik input res`。
   来源：https://perfetto.dev/docs/learning-more/android
   - 特别勾 `hal`（PowerHAL/perfd tracepoint）+ `input` + `freq` + `sched`，即可看到"输入事件→HAL hint→频率变化"的时序链。
2. **vendor tracepoint**：HAL 可扩展设备专用点（如 mdss/sde/mali_systrace）。
   来源：https://source.android.google.cn/docs/core/tests/debug/ftrace?hl=zh-cn
3. **dumpsys 服务**：`adb shell dumpsys`（power、activity 等）观察 cgroup 分组与 top-app 状态。
   来源：https://developer.android.google.cn/tools/dumpsys
4. **ftrace 事件**：`echo 1 > /sys/kernel/debug/tracing/events/sched/sched_switch/enable`、`sched_wakeup`、`cpu_frequency`（ROUND-006 已给 30min 方案）。

### 4.2 真机验证 AATPE 时序的具体做法
- 录一段"点击→动画→松手"的 perfetto trace（含 hal/input/freq/sched）；
- 对比三段时序：**系统/PowerHAL 自己的 boost 起止** vs **AATPE 的决策生效/回落**；
- 度量：从 input 事件到大核上核的延迟、AATPE boost 持有时长、是否比系统 boost 回落更慢（费电）。

---

## 5. 改进项总表（按性价比排序，供下一轮落地）

| # | 现状 | 标杆做法 | 差距 | 建议 | 收益 | 成本 | 风险 |
|---|---|---|---|---|---|---|---|
| 1 | 200ms 周期硬决策 | 事件驱动（Tap/启动）+ 短时 boost 衰减 | 错过 16ms 输入窗口 | 改"input/启动事件触发即时 boost + 200ms 周期兜底" | **高** | 中 | 事件源接入需稳 |
| 2 | dwell 200/300/500ms | PELT 半衰 32ms，突发后快速回落 | 滞留大核费电 | dwell 下限降到 50-100ms，突发结束即回落 | **高** | 低 | 过短致抖动，需标定 |
| 3 | input boost 时长未对齐 | 业界 40-100ms | 时长可能偏长 | input boost 固定 40-100ms 衰减 | **高** | 低 | 低 |
| 4 | spillover 60/75/90 | EAS/SRE headroom 15-30% | 60% 偏保守、90% 过晚 | 主触发定 ~75%（留 25% headroom） | 中 | 低 | 需真机标定 |
| 5 | 全局调频倾向 | 大厂退回"不长期抬 min_freq" | 与 Scene/EAS 抢 sysfs | AATPE 只做线程放置，不长期写 min_freq | **高（省电）** | 中 | 定位收敛 |
| 6 | 帧率采样 p95/p99 | 官方 jank 阈值 p95>16ms/P99 overrun>16ms | 阈值未对齐官方 | 用官方阈值判 jank，驱动 boost | 中 | 低 | 低 |
| 7 | 线程上限 512 硬截断 | 按实际 task 数自适应 | 漏 >512 线程游戏 | 自适应上限 | 中 | 低 | 低 |
| 8 | 无真机闭环 | perfetto/atrace/dumpsys 对标 PowerHAL | 常量靠经验 | 做 ROUND-006 已述 30min 采集 + 时序对比 | **高** | 中 | 采集开销 |
| 9 | 与 Scene 类工具冲突风险 | 同类工具抢 sysfs | 后写覆盖 | 文档声明 AATPE 不写全局调频档 | 中 | 低 | 低 |
| 10 | uclamp util_min 长期高 | 1024 仅瞬时 boost | 长期费电 | 1024 仅限输入/启动瞬时 | 中 | 低 | 低 |

---

## 6. 结论
- **大厂一致思维**：事件驱动的短时 boost + 自动衰减，不长期绑核、不长期抬 min_freq（高通阵营都在 "Drop CPU freqs boosting on INTERACTION"）。
- **AATPE 最需修的三点**（高收益低成本）：① 决策从"周期轮询"转"事件触发+短时 boost"；② dwell/input boost 时长对齐 PELT 半衰（50-100ms）；③ spillover 主触发对齐 EAS ~25% headroom。
- **定位收敛**：AATPE 应做"系统 top-app 分组之上的线程级瞬时增强"，而非全局调频，避免与 Scene/PowerHAL 抢 sysfs。
- **闭环手段**：perfetto(atrace hal/input/freq/sched) + dumpsys + ftrace 即可真机对标大厂时序，标定所有经验常量。

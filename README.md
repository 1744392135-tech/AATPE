# AATPE — Adaptive Android Task Placement Engine

> 面向移动端深度场景的自适应任务放置引擎。基于 eBPF 事件驱动观测、ADRC 自抗扰控制、在线强化学习与 cgroup 层级冻结，在无需解锁 Bootloader 之外任何侵入式 Hook 的前提下，为游戏、视频与高负载前台应用提供可观测、可干预、可回滚的系统级调度优化。

---

## 一、项目定位

AATPE（Adaptive Android Task Placement Engine）是一款面向 ARM64 Android 设备的系统级调度守护进程，以 Magisk/KernelSU/APatch 模块形态分发。它不修改 AOSP 源码、不替换系统库、不依赖 Zygisk Hook，而是通过 **cgroup 文件系统、sched_setattr、uclamp、eBPF 追踪点** 与用户态观测器的组合，在运行时对线程放置、频率请求、后台功耗与前台帧率进行闭环调节。

设计目标有三：

1. **可观测**：所有调度决策、温度事件、帧率波动、冻结状态均落盘 JSON 面板，WebUI 实时可视化；
2. **可干预**：每一项调节都有独立开关与回滚路径，关闭开关后系统行为与未安装模块逐位一致；
3. **可演进**：在线学习模型随设备真实负载持续迭代，并通过联邦学习在不泄露隐私的前提下汇总策略。

---

## 二、架构总览

```
┌──────────────────────────────────────────────────────────────┐
│                     WebUI (Web 面板 / HTTP)                    │
└──────────────────────────────────────────────────────────────┘
                              ▲
┌──────────────────────────────┴───────────────────────────────┐
│                    aatped (Arbitrator Daemon)                  │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌──────────┐ ┌───────┐ │
│  │ 场景推断 │ │ 帧率观测 │ │ 温控HAL │ │ 调度决策 │ │ C4冻结│ │
│  │ Scene   │ │ Frame   │ │ Thermal │ │ ADRC+ML  │ │Freezer│ │
│  └─────────┘ └─────────┘ └─────────┘ └──────────┘ └───────┘ │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌──────────┐ ┌───────┐ │
│  │ eBPF    │ │ 输入boost│ │ 联邦学习 │ │ 状态机   │ │看门狗│ │
│  │ 事件采样│ │ uclamp  │ │Federated│ │ StateTTL │ │Watchdog│
│  └─────────┘ └─────────┘ └─────────┘ └──────────┘ └───────┘ │
└──────────────────────────────────────────────────────────────┘
       │            │              │              │
       ▼            ▼              ▼              ▼
   cgroup.procs  sched_*      /sys/devices     /dev/cgroup
   freezer.state  uclamp.min   thermal/*       eBPF map
```

### 核心子系统

| 子系统 | 职责 | 关键技术 |
|---|---|---|
| **场景推断** | 识别前台应用类型（游戏/视频/阅读），选择对应调度档 | 包名白名单 + 帧率 + CPU 负载三因子融合 |
| **帧率观测** | 独立于 latency 通道的真实帧时间戳采集，支撑 ML 奖励 | SurfaceFlinger / timestats 双通道 |
| **温控 HAL** | 抽象 mtktscpu/tsens/battery 等多平台温度源，统一温度墙 | ADRC 状态空间 + 30s 防抖 |
| **调度决策** | ADRC 自抗扰控制器 + 在线 ML 双模型加权输出放置策略 | 扩张状态观测器、LEAR 增益调度 |
| **C4 后台冻结** | 双桶 freezer 探测、uid 枚举、属主复核、读回校验、回滚提交 | cgroup v1 freezer / v2 freezer |
| **eBPF 采样** | sched_switch / task_sched 事件驱动观测，替代轮询 | BPF ring buffer + 轮询降级 |
| **联邦学习** | 本地梯度差分隐私聚合，云端不接触原始数据 | 模型分片 + 安全聚合 |
| **看门狗** | 守护进程自监控、卡死自愈、重启窗口限流 | 时间窗 + 计数上限 |

---

## 三、核心特性

### 1. 事件驱动的零轮询观测
传统调度守护进程以固定间隔轮询 `/proc/stat`、`/proc/<pid>/stat`，在高负载下自身成为噪声源。AATPE 默认通过 eBPF `sched_switch` 与 `task_newtask` 追踪点采集线程切换事件，仅在 ring buffer 非空时唤醒决策循环；无权限挂载时自动降级到带退避的轮询路径，保证行为兼容。

### 2. 双桶 cgroup freezer（C4）
针对 cgroup v1 设备（内核 4.14~5.10）普遍存在的 **MIUI/HyperOS 预置共享桶**（`perf/frozen` / `perf/thawed`）而非标准 `uid_<uid>` 顶层目录的真机环境，AATPE 实现了：

- 双桶可用性探测（可写、state 校验、200 tick 周期重探）；
- 按 uid 遍历 `/proc` 枚举目标进程组全部 pid；
- 写入前逐 pid 复核 `/proc/<pid>/status` 属主，杜绝 pid 复用时误冻其他应用；
- 写入后读回 `/proc/<pid>/cgroup` 验证，失败自动回滚；
- 回滚失败的 pid 集合作"已冻结子集"持久化，保证后续解冻必然覆盖；
- 解冻恒重枚举，覆盖冻结期间 fork 的子进程；
- 只动本 uid 的 pid，从不清空共享桶。

### 3. ADRC 自抗扰温控
温控环路采用自抗扰控制（Active Disturbance Rejection Control）：扩张状态观测器实时估计"总扰动"（环境热辐射、后台负载、电池温升），并在前馈通道补偿，相比传统 PID 在温度超调与恢复时间上均有显著改善。30 秒防抖窗口消除 emergency 状态在阈值附近的振荡。

### 4. 在线 + 联邦双层学习
- **本地在线学习**：基于帧时间奖励的线性 bandit，学习率自适应衰减，权重钳制防止发散；
- **冷启动保护**：样本数低于阈值时由内置规则接管，避免初期乱决策；
- **联邦聚合**：多设备本地梯度安全聚合，模型参数差分隐私上传，原始数据不出设备。

### 5. 输入感知 boost
检测到屏幕输入事件（`/dev/input/event*`）时，按当前刷新率分档（60/120/144Hz）触发 uclamp 短时提升，时长随刷新率递减（80ms → 50ms → 40ms），兼顾跟手性与功耗。

### 6. WebUI 实时面板
内置 HTTP 服务器提供：
- 实时 FPS / CPU / 温度 / 内存；
- C4 冻结状态可视化（backend、frozen pid 列表、cgDir）；
- 调度决策日志、温控事件流；
- 配置在线热重载。

三副本 WebUI 资源（webroot / webui / 内置默认）字节一致，保证任意降级路径渲染相同。

---

## 四、技术栈

| 层 | 选型 |
|---|---|
| 语言 | C++17（核心守护）、C（mech/thermal_ff）、Shell（启动脚本） |
| 工具链 | Android NDK r30、clang-21、-O3 -flto=thin |
| 观测 | eBPF (BPF CO-RE 降级)、uclamp、sched_setattr |
| 存储 | JSON 状态文件、cgroup 文件系统 |
| 网络 | 内置 HTTP 服务器（libhv 风格轻量实现）|
| 测试 | GoogleTest、ASan/UBSan、TSan、libFuzzer、变异测试 |
| 模块格式 | Magisk / KernelSU / APatch 通用模板 |

---

## 五、安装

### 依赖
- Android 10+（ARM64）
- Magisk / KernelSU / APatch 任一 root 方案
- 已解锁的 root 环境（不需要 Bootloader 解锁，模块按系统方式加载）

### 步骤
1. 下载 `AATPE-v6.0.0-release.zip`；
2. 在 Magisk/KernelSU 中刷入，重启；
3. 打开 `http://localhost:8080`（或设备 IP）查看 WebUI；
4. 默认档位 `assist`（辅助模式），日常使用无需任何配置。

---

## 六、配置

主配置文件位于 `/system/etc/aatpe/aatpe.conf`。每一项配置均附中文注释（含义、单位、默认值、取值范围、调整建议），保存后守护进程自动热重载，无需重启。

常用调节示例：

```ini
# 从辅助模式切换为完全接管
CONTROL_MODE=off
# 目标帧率改为 120Hz（目标帧时间 8333us）
TARGET_FRAME_TIME=8333
# 自定义游戏包名
GAME_PACKAGES=com.tencent.tmgp.sgame,com.miHoYo.GenshinImpact
# 关闭 C4 后台冻结
C4_FREEZER_ENABLED=false
```

---

## 七、验证与质量

每个发布版本均通过以下门禁：

- **host 单元测试**：157 项全过，0 fail / 0 skip；
- **ASan + UBSan**：163 个目标，零内存错误 / 零未定义行为 / 零泄漏；
- **TSan**：25 个并发目标，零数据竞争 / 零死锁；
- **变异敏感度**：69 个变异测试，63 个被现有测试捕获；
- **干净重建**：NDK r30 clean build，四二进制 sha256 与 BUILD_MANIFEST 逐字节一致；
- **仿真端到端**：在 user+mount namespace 内用 LD_PRELOAD 拦截 fopen，驱动**未改一字节的权威源码**真实跑通冻结/解冻/回滚/重启全流程。

---

## 八、安全与隐私

- 所有决策均在本地完成，无遥测、无广告、无远程回传；
- 联邦学习上传仅聚合梯度统计量，差分隐私加噪；
- 控制命令支持 HMAC 签名，防止其他应用伪造调度指令；
- 关闭任一开关后，对应子系统完全退出，与未安装状态逐位一致。

---

## 九、许可

本项目以研究与个人使用为目的发布。源码可自由审阅、学习、二次开发（保留署名）。

---

## 十、版本

- 当前版本：**v6.0.0 (rel20 / versionCode 4414)**
- 发布日期：2026-10-30
- 详细变更见 [`CHANGELOG.md`](CHANGELOG.md)

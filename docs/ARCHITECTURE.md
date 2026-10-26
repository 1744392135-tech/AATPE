# AATPE 架构依赖说明（v94 / 问题41）

> 这是一个**架构观察文档**，不是 bug，也不要求重构（问题41 明确禁止重构）。
> 目的：把当前模块耦合度显性化，让后续改动知道"碰一处会波及哪里"。

## 1. 规模

约 139 个源文件。核心二进制 `aatped` 由 `Makefile` 的 `AATPED_SRCS` 一次性编译
链接（单进程，非多进程），把 eBPF、ML、热管理、帧调度、放置策略、WebUI 全部揉进
同一个地址空间：

```
aatped.cpp  scoring.cpp  event_monitor.cpp  uevent_monitor.cpp  thermal.cpp
frame.cpp   thread_scan.cpp  placement.cpp  webui_json.cpp  webui_http.cpp
aatpe_ml.cpp  aatpe_ml_hybrid.cpp  aatpe_federated.cpp  aatpe_ebpf.cpp
aatpe_observability.cpp
```

## 2. 模块依赖关系（数据流主链路）

```
                         ┌─────────────── daemon 主循环 (aatped.cpp) ───────────────┐
                         │                                                          │
  kernel sched tracepoint  ──►  aatpe_ebpf.cpp  ──►  ring buffer / poll thread      │
   (sched_switch/fork/exit/exec)   (BPF_PROG_LOAD, embedded bytecode)               │
                         │            │                                             │
                         │            ▼                                             │
                         │       g_eventConsumer (IEventConsumer)                   │
                         │            │                                             │
                         │            ▼                                             │
                         │     thread_scan.cpp  ──►  g_threads[] (线程表)           │
                         │            │                                             │
                         │            ▼                                             │
                         │     scoring.cpp / aatpe_ml.cpp (+hybrid/federated)       │
                         │            │  线程角色评分、ML 学习、池归属决策            │
                         │            ▼                                             │
                         │     placement.cpp  ──►  sched_setaffinity / cpuset        │
                         │            │                                             │
                         │     frame.cpp  ──►  帧时间 / 游戏场景 / audio 隔离        │
                         │            │                                             │
                         │     thermal.cpp ──►  温控 / 热紧急降级                    │
                         │            │                                             │
                         │            ▼                                             │
                         │     webui_http.cpp / webui_json.cpp ──► WebUI 状态/控制  │
                         │            │                                             │
                         │     event_monitor.cpp / uevent_monitor.cpp (uevent 兜底) │
                         └────────────┼─────────────────────────────────────────────┘
                                      │
                                      ▼
                        状态文件 /dev/aatpe/state/*  (heartbeat, big_pool,
                        control_mode, ebpf_offset_verified, watchdog_topology_source ...)
                                      ▲
                                      │ 读 daemon 的 big_pool；kill/restart daemon
                         aatpe-watchdog.cpp (独立进程)
```

## 3. 核心模块职责

| 模块 | 职责 | 关键产出 |
|------|------|----------|
| `aatped.cpp` | 主循环、配置加载、topology 分类、把各模块缝起来 | `g_cfg`, `g_bigPool`/`g_littlePool`, state 文件 |
| `aatpe_ebpf.cpp` | 内嵌 eBPF 加载、tracepoint 附加、ring buffer 消费 | fork/switch/exec/exit 事件 |
| `thread_scan.cpp` | 线程扫描、`/proc` 遍历 | `g_threads[]` |
| `scoring.cpp` | 线程角色/优先级评分 | 池归属建议 |
| `aatpe_ml*.cpp` | 池级 logistic 回归学习 | 迁移概率 |
| `placement.cpp` | 实际亲和性/cpuset 落地 | 调度动作 |
| `frame.cpp` | 帧时间、游戏场景识别 | 场景/目标帧时间 |
| `thermal.cpp` | 温度采样与热紧急 | thermal_emergency |
| `webui_*` | HTTP + JSON 状态/控制 | WebUI |
| `aatpe-watchdog.cpp` | 独立进程：心跳、重启、紧急迁移 | 读 `big_pool`、写 `control_mode` |
| `aatpe_federated*.cpp` | 本地模型权重的导出/导入与本地加权聚合 | `exchangeDir_/local_model.bin`、`import/` |

> **命名澄清（第三方审计反馈）**：`aatpe_federated_*` 模块**不是真正的 P2P 网络联邦学习**。
> 当前实现是 **cross-device model exchange via file transfer（基于文件传输的跨设备模型交换）**：
> 它只在本地读写 `exchangeDir_/local_model.bin`（导出）与 `exchangeDir_/import/`（导入对端模型），
> 进程内**没有任何 socket / 网络栈代码**，聚合也只发生在本地。模型文件如何从一台设备“搬”到另一台设备，
> **完全依赖外部文件传输机制**（如 `adb push`、共享存储、用户手动拷贝），本模块不负责也不感知这一过程。
> 文件自身的**完整性与真实性**由 PSK 预共享密钥做 **HMAC-SHA256 签名**校验、导入时再做 **CRC32** 校验双重保证；
> 没有签名或校验失败的模型仅作 CRC-only 的非信任导入（不参与聚合加权）。因此代码与文档中凡称“P2P / 对端交换”，
> 均应理解为“无中心服务器、靠外部文件搬运的设备间模型交换”，而非端到端网络联邦学习。

## 4. 已知高耦合点（改动前必读）

1. **`g_cfg` 被多模块读写**：`frame.cpp`、`placement.cpp`、`scoring.cpp`、
   `thermal.cpp`、`thread_scan.cpp`、`webui_http.cpp`、`webui_json.cpp` 都直接读
   全局 `g_cfg`。改配置字段/key 必须跨这些文件同步（v94 已为此统一过
   `MIN_SAMPLES_FOR_LEARNING`→`HISTORY_MIN_SAMPLES`）。
2. **`g_threads[]` 被多模块共享**：`aatped.cpp`、`placement.cpp`、`scoring.cpp`、
   `aatpe_ml*.h` 都访问。增删字段要同时改生产者（thread_scan）和消费者。
3. **CPU 池拓扑多份镜像**：`g_bigPool`/`g_littlePool`/`g_midPool` 在 daemon 内是
   真源，并通过 state 文件 `big_pool` 暴露给 watchdog；watchdog 另有一套**镜像
   一致**的本地推断（问题38）。ML 也通过 `setCpuPoolAssignment()` 收到一份拷贝。
   改池划分规则要三处一起看。
4. **eBPF 事件结构跨用户态/内核态一致**：`struct aatpe_*_event` 同时定义在
   `sched_events.bpf.c` 和 `aatpe_ebpf.h`，改一边必须同步另一边。
5. **state 文件是跨进程契约**：daemon 写、watchdog/WebUI/CLI 读，文件名即接口，
   改名等于破坏外部消费者。

## 5. 建议（不实施，仅记录）

当前阶段（见 `aatpe-version.h` 问题42）**不做解体重构**。高耦合点以文档化 + 明确
真源（`docs/SOURCE_OF_TRUTH.md`）来约束，避免再引入第二份分歧实现。未来若要拆，
优先方向是把 `g_cfg`/`g_threads` 收敛成显式上下文对象并显式传参，而非继续全局共享。

---

## 6. v3.5.0 / ROUND-004–005 增量补充

> 上文 §1–5 为 v94 基线观察；以下为 v3.5.0 起新增/改动的真实结构，避免与旧描述漂移。

- **通用多级 CPU 拓扑**：`aatped_topology.cpp` 引入 `CpuCluster` 聚类，切簇按相对比率判据（容量比≥1.20 / 频率比≥1.15），层级扩为 LITTLE/MID/BIG/**PRIME**（超大核）。运行时状态落盘 `state/clusters`（每行 `tier,capacity,minKHz-maxKHz:cpulist`，弱→强）。
- **boost 机制层**：`aatpe_placement_mech.{h,cpp}` 是独立编译单元（已入 Makefile `AATPED_SRCS`/`AATPED_HDRS`），封装 `SchedCaps` 探测与 `selectBoostMechanism`（UCLAMP / CGROUP_UCLAMP / STUNE_CPUSET / AFFINITY / NICE_ONLY 五选一）；`placement.cpp` 调用，不再硬编码 boost 落点。
- **spillover 调度**：大核饱和→超大核溢出，运行时状态 `prime_spillover_count` / `prime_hosted_threads`。
- **新运行时状态键**：`scheduler_capabilities`、`topology_diagnostics`、`clusters`、`prime_spillover_count`、`prime_hosted_threads`（均在 `/dev/aatpe/state/`，tmpfs，**不进模块包哈希链**）。
- **配置热重载**：`/api/config_meta` 端点返回全键 LIVE/BOOT 分类；profiles/*.conf 文件监视即时重载；BOOT 键需重启生效。
- **控制模式三档**：passive/assist/active（用户态）+ emergency（仅内部）。

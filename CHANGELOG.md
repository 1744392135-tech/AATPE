# AATPE Change Log

Single source of truth for the release version: see `./VERSION` and
`aatpe-version.h` (AATPE_VERSION / AATPE_VERSION_CODE). This file records the
historical fix series so it no longer has to live as scattered version tags in
the production source headers. Inline `fixNN` comments in the code are retained
for debugging; they are pointers to this log, not the version itself.

---

## v6.0.0 (2026-10-30) — rel20 / versionCode 4414：C4 双桶 freezer 真机根因修复 + 全树注释精简 + 四轮穷尽验证

> 真机发现 C4 后台冻结始终冻结不了。真机证据锁定根因：该设备（小米 chopin / 天玑1100 / 内核4.14 / cgroup v1 freezer）**没有 uid_xxx 目录**，只有 MIUI 预置的共享双桶 `/sys/fs/cgroup/freezer/perf/frozen`（state=FROZEN）与 `perf/thawed`（THAWED），旧代码硬找不存在的 uid_xxx 顶层目录而静默失效。

### C4 双桶后端重写
- **双桶探测**：probeV1Bucket() 探测 frozen/thawed 两桶是否存在、cgroup.procs 是否可写、state 是否就绪，结果缓存并每 200 tick 重探；探测顺序 v2 → v1 uid_xxx → v1-bucket。
- **按 uid 枚举 pid**：enumPidsForUid() 遍历 /proc 按 ruid 枚举目标 uid 全部 pid；**写 cgroup.procs 前用 pidOwnerMatches() 复核 /proc/<pid>/status 的 Uid 行**，属主不符（pid 被别的应用复用）直接丢弃，杜绝误冻别的 app。
- **读回 + 回滚 + 提交子集**：逐 pid 写入 frozen 桶后回读 /proc/<pid>/cgroup 验证；任一回读失败即把已写 pid 全部回滚到 thawed；**回滚也失败的 pid 集合作为 stuck 子集提交为已冻结状态并落盘**，保证后续前台释放一定能解冻它们，从根上杜绝"孤儿 pid 永久卡死"。
- **解冻恒重枚举**：解冻不盲信持久化 pid 列表，一律按当前 uid 重新枚举全部 live pid 移到 thawed（多解冻无害），覆盖冻结期 fork 出来、继承 FROZEN cgroup 的子进程；解冻只动本 uid 的 pid，**从不清空共享桶**。
- **桶自动 provision**：桶缺失但 v1 freezer 根可写时，自动 mkdir 双桶并写 freezer.state；provision 路径逐级 lstat 拒绝符号链接、文件用 O_NOFOLLOW|O_EXCL 创建，防止路径穿越。
- **音频白名单精确化**：mediaSessionMentionsPlaying() 重写——按 dumpsys session 头精确分块、包名精确 token 匹配（修掉 `com.game` 误配 `com.game.hack`）、`state=3` 精确分词（修掉 `flags=state=3` 子串误判）、包名截断 `:id` 后缀；分词器对逗号/分号分隔有界（修掉一个会导致 tick 线程永久挂起的死循环）。

### 注释精简（按用户口径）
- module.prop 的 description 从版本历史改为一句话模块简介；全树 C/C++/shell/conf/WebUI 注释按"只留许可证、其余全删"剥离（代码库无文件头许可证注释，故等同全删），并用编译器、node、归一化 token 对比验证零误伤。

### 四轮"修复 → 独立验证"收敛（本轮新增的穷尽流程）
- 第一轮四路独立验证（逐行审查 / 对抗红队 / 变异敏感度 / Sanitizer）发现 P0 孤儿冻结 + 2 个 P1 + P2/P3 共 5 项；
- 第二轮回归又发现解冻漏子进程、回滚失败残留孤儿、provision 符号链接 3 项；
- 第三轮发现音频 `mL` 子串误判、state 子串误配；
- 第四轮发现分词器死循环（逗号分隔）。每一轮修复后都重新独立验证，直到四路全部收口。
- 验证基线：host v94 **157 ok / 0 fail / 0 skip**；ASan+UBSan **163 目标零内存错误/UB/泄漏**；TSan **25 并发目标 0 race / 0 死锁**；变异电池 **69 个变异、63 个被测试捕获**（剩 4 个为 seam 能力边界低风险项）；release_ci 全 PASS。
- **自建仿真环境**：无真机/KVM 条件下，在 user+mount namespace（unshare -Urm）内用 LD_PRELOAD 拦截 fopen/fclose，把写 cgroup.procs 翻译成真实 SIGSTOP/SIGCONT，用**未改一字节的权威 c4_freezer.cpp** 端到端跑通冻结/解冻/回滚/重启/属主复核/子进程随父解冻全流程，真实观测进程 State=S↔T。

### 仍需真机回传（DEGRADED，不伪造）
刷入 rel20 后让目标游戏进后台，确认 c4.json 显示 `backend=v1-bucket`、`frozen=true`、cgDir 指向 perf/frozen，回前台或起音频后自动解冻。

---

## v6.0.0 (2026-10-29) — rel19 / versionCode 4413：四路全新对抗红队终审封版

> 在 rel18 基础上，派 4 名全新独立红队做终审对抗复查；修复查出的 3 个阻断项 + 全部同类建议项后复审清零。host v94=157/0、NDK 干净重编、三门全过、TSan race=0、反编译逐函数对照一致、源码干净重建复现一致。

---

## v6.0.0 (2026-10-28) — rel18：外部深度审查 H01–H04 独立复核 + 测试假通过根治

- H01 eBPF 软信任门控矛盾（锁定 ROM 零事件）、H03/H04 独立验证复核修复；
- 系统性根治"测试假通过"（零敏感度断言、恒真表达式、自抄副本）；
- 真机截图/帧率显示字段修复。

---

## v6.0.0 (2026-10-27) — rel16：调度核心 + 温控状态机 + C4 探测修复

- **大小核颠倒修复**（WP-A）：active 谓词重写 + emergency 强制重打分 + 主循环尾延迟优化；
- **温控状态机震荡修复**（WP-B）：温度墙统一 + emergency 30s 防抖窗口；
- **C4 freezer 探测与 percpu.json 生成修复**（WP-C）；
- **ML 就绪度空 MID 池误判 + 场景误判修复**（WP-D）；
- **前端 index.html 显示全修复**（WP-E）；
- 合并全量门禁 + 独立红队 + 干净重打包。

---

## v6.0.0 (2026-10-27) — rel17：真机两 bug 根因修复（含 rel16 全部内容）

- WebUI 首屏慢/刷新不及时/点按卡顿根因修复；守护进程每秒全量重打分导致 CPU 占用 43% 的根因修复；
- 前端数据对账与显示 bug 6 项修复；
- 上述 rel16 调度/温控/C4/ML/前端修复随本版合并封版。

---

## v6.0.0 (2026-10-26) — rel15：真机反馈第一批修复

- 真机"锁小核致游戏卡顿"、调度震荡、cgroup 空/freezer 不可用等反馈项的根因排查与修复；自建 arm64 模拟环境端到端复现钉小核（CONFIRMED）。

---

## v6.0.0 (2026-10-26) — rel14 四大根因修复封版

> 在 rel13 逐行审查基础上，修复四个导致真机行为异常的根因问题，而非症状层打补丁。host v94 146/0/0、NDK 普通门 rc=0、ASan+UBSan 15 行/801 断言/0 错误泄漏、TSan 5 目标/0 race。

### 根因修复
- **[C4 v1 freezer 回退]** 4.14 内核设备（天玑 1100 / chopin）不支持 cgroup v2 freezer，原代码直接写 `cgroup.freeze` 静默失败导致 C4 形同虚设。rel14 在检测到 v2 freezer 不可用时回退到 cgroup v1 `freezer.state= FROZEN`，覆盖旧内核设备。
- **[紧急迁移自激环消除]** 原紧急迁移在边界条件下反复触发→迁移→再触发，形成正反馈震荡。rel14 加 600ms 确认窗口、驻留要求、冷却期，消除自激环。
- **[对称迟滞消 bang-bang]** 大小核调度原用单向阈值，临界条件下在 LITTLE/BIG 间反复横跳。rel14 改为对称迟滞（上行/下行不同阈值），消除 bang-bang。
- **[帧率不被 latency 短路]** 原帧率测量在 latency 优先路径被短路，导致 FPS 数据恒为 0。rel14 分离帧率采集与 latency 决策，帧率数据独立可信。
- **[GPU boost 真温度门控]** 原 GPU boost 写死满频，不响应温度。rel14 改用真实温度门控 + 滞回 + 不锁满频，温度轴前移到 boost 决策前置。

---

## v6.0.0 (2026-10-26) — rel13 逐行非抽查审查 + 修复封版

> 在 rel11/rel12 基础上，派审查员对 shell/打包/构建/设备/机制全树逐行通读（禁止抽查），按 P0-P3 分级产出 ISSUE 卡，再由各模块修复至清零。本版修复 12 个 P1、约 70 个 P2、约 50 个功能性 P3，并补强打包完整性与版本一致性 CI 门。

### P1 修复（12 项）
- **[P1·打包] collect.sh / doctor.sh 漏打入 release.zip**：Makefile install 目标把两个设备脚本装进 `_install/`，但 mirror-to-`./system/` 块漏 cp，导致真机用户拿不到证据采集/就绪检查脚本。已补 mirror 行，release_ci 新增 [26d] 守门强制两者在包内。
- **[P1·版本分裂] RELEASE_ID 漂移**：webui 已含 rel12 性能诊断面板，但 VERSION/module.prop/aatpe-version.h/页脚仍写 rel11，CI 只 grep v6.0.0 不校验 RELEASE_ID。已 bump 至 rel13/2026-10-26，check_version_consistency.sh 新增 RELEASE_ID/BUILD_DATE 四处一致性校验。
- **[P1·C4 锁粒度/状态路径]** C4 冻结路径锁粒度与状态文件读写路径经逐行审查收紧。
- **[P1·eBPF 5 项]** eBPF 事件驱动路径 5 个缺陷修复。
- **[P1·配置热重载/持久化]** aatpe.conf 热重载与持久化路径修复。
- **[P1·daemon 自采样]** daemon 自采样路径修复。

### P2 修复（约 70 项，按模块）
- **placement / frame / main / webuihttp / ml / thermal / watchdog / gpu 拓扑 / security / 监控 / 前端 / state** 各模块的并发/边界/资源/错误处理/危险模式/输入校验问题。
- 典型：service.sh 在 integrity verify 前执行 aatped --init-topology（fail-closed 姿态修正）；promoted_tids/cg_uclamp_active/gpu_boost_active 路径字段补 `..` 拒绝；uninstall.sh legacy .uclamp/.affinity 恢复补 live==applied 守卫；customize.sh INSTALL_ID 熵兜底空指针修复。

### P3 修复（约 50 项）
- 跨 main/ebpf/watchdog/security/ml/gpu拓扑/webuihttp/frame/thermal/state/placement 11 个模块的功能性打磨。
- 构建期修复：frame.cpp:4978 unused parameter `now`（加 `(void)now;`）；aatped_battery.cpp:291 unused variable `online`（删除）。

### 验证结果
- **host v94**：143 测试 / 0 失败 / 0 未定义行为。
- **NDK 三门**：ASan 14/14 全通过（783 断言）、TSan 5/5 全通过（0 race）。
- **干净重建**：NDK r30 clean build，三向逐字节一致（webui 三副本 md5 相同、aatpe.conf 两副本相同、二进制 sha 与 BUILD_MANIFEST.txt 一致）。
- **仿真**：10 通过 / 0 失败 / 11 DEGRADED（无真机/KVM 环境限制）。

### 仍 DEGRADED 的 11 项（真机/内核依赖）
binder/SurfaceFlinger 真帧链路、eBPF 内核附着（4.14/5.10/6.x 各分支）、真温度读数（mtktscpu/tsens）、真帧率上屏（Choreographer/SF）、SELinux 实时策略（enforce 下 avc denied）、16KB 页设备对齐、C4 真冻结有效性、GPU boost 真频率、PSI 真实压力、fuzz 真崩溃、ML 真数据回归——均需真机或 KVM 环境验证，当前 host 仿真已通过但真机未回传。

---

## v6.0.0 (2026-10-17) — 终审红线收口（四路独立敌意终审 + 整改 + 复审清零）

> 在 v4.5.0 基础上，派 4 名全新第四方敌意审计员（安全/新代码正确性/通用不降级/证据打假）独立终审，再由原审计员对整改逐条复审至 **P0/P1/P2 清零**。本版修复一个会让头条交付物失效的真机/host 共性缺陷、补齐影子数据导出闭环、加固全部 root 写路径，并去除任何机型锁定观感。**当前构建（rel11，2026-10-23）`C4_FREEZER_ENABLED` / `FEDERATED_ENABLED` / eBPF 事件驱动均为默认 ON**（带前台/音频/IM 白名单与异常自动降级；该默认自 round4c 起翻转，下方 round4c 节有完整记录；更早 v4.4.x~v4.5.0 各节所述"仍默认 OFF"为对应历史时点的正确记录）；BPF 无权限/不支持时仍自动降级。

### 第三轮红队（并发/内存 + 生命周期/16KB，候选修订）
- **[P1·并发] `g_cpuMetrics` 数据竞态修复**：`g_cpuMetrics` 为 `std::map`，写侧在采集线程/topology/arbitrator 持 `g_cpuMetricsMutex`，但主循环有 5 处无锁读（loopAuxScan 遍历求平均负载、outcome 批处理 `find(curFreq/maxFreq)`、baseline duty、placement beforeFreq）——多线程下 `std::map` 并发读写是未定义行为。这 5 处改为短临界区（只把所需字段拷到局部量后立即释放锁）；锁序统一核实为 **threads+ml → cpuMetrics**，全树无反向获取（无 ABBA）。新增 host TSan 测试 `test_cpu_metrics_race_v600`（writer 线程镜像采集、reader 镜像 aux-scan/outcome 快照），**TSan 0 race**。
- **[P2·锁持有] shadow_dump / model_save 缩短 `g_mlMutex`**：shadow_dump 原在锁内做 512 次 snprintf + string 拼 JSON，改为锁内只 `drainShadowSamples` 拷出 vector、**释放锁后**再序列化 + `writeFileAtomic`；model_save 原在锁内 `backupModelFile()`（整文件拷贝），改为锁内只 `g_mlModel.save()`、锁外再 backup/stat/状态文件，与周期 save 路径一致。
- **[P2·真机安装阻断] 16KB 页设备对齐门修复**：`customize.sh` 原用 `readelf -l | grep -qE 'LOAD .* (10000|4000)\b'`，非宽输出下 readelf 把每个 LOAD 段拆两行、对齐值在第二行，导致 p_align=0x4000 的正确二进制被误判 bad → 16KB 设备直接 abort 装不上。改为 `readelf -lW`（宽输出，每段一行）+ awk 取 LOAD 末列 p_align，要求所有 LOAD 段 ∈ {0x4000, 0x10000}（绝不允许 0x1000），不依赖 `\b`（toybox/busybox 兼容）。实测：4 个真实 0x4000 ARM64 二进制全判 OK、构造的 0x1000 二进制正确判 BAD。
- **[门禁] release CI 新挂 check-manifest-layout 硬门**：`system/etc/aatpe/ebpf/sched_events.bpf.o` 必须随包、整模块 **0 个 `__pycache__/*.pyc`**、0 个 stray `.o/.d/integration` 产物；缺失即 FAIL，与 version 一致性检查同级。
- **[打包卫生] Python 字节码不落地**：release CI 的 Python 语法检查由 `py_compile`（会落地 .pyc）改为纯 `ast.parse`（内存编译、不写缓存，语法错误仍 FAIL）；`gen_embedded_header.py` 用 `python3 -B` 运行；release CI 末尾加幂等 `__pycache__/*.pyc` sweep。硬门在干净树连跑两次均 PASS（幂等）。
- **[新增·可选] `sepolicy.rule` 静态 SELinux 回退模板**：默认全部注释、不生效；仅当设备无 magiskpolicy/supolicy、live 策略跑不起来且 logcat 见 `avc: denied ... bpf/perf_event` 时，手动取消注释对应 loader（Magisk/KSU/APatch）块。规则最小化（self-domain only），与 live 策略一致。
- **[已知限制·DEGRADED]** outcome 批处理在持双锁内读 `/proc/<pid>/task/<tid>/schedstat`：该路径有 500ms 预算门控 + 睡眠指数退避兜底（延迟有界），真正的 UB（g_cpuMetrics 竞态）已在上条修复；把 schedstat 移出锁外涉及 TID-reuse 的 ML 学习正确性，无真机验证下未盲改，留待真机评估。

### 第三轮（续·RT-C 第六方回归对抗，round3.2 候选修订）
- **[P2·并发] 模型备份 backupModelFile 临时文件撞车修复**：原临时名 `<model.dat>.bak.tmp.<getpid()>` 在同进程内对所有线程相同。round3 把备份移出 `g_mlMutex` 后，控制线程 `model_save` 与周期 save 线程会并发 backup 同一 model.dat：同一 pid → 共用一个 tmp 文件，一个 `O_TRUNC` 把另一个正在写的流截断，再 `rename()` 发布一个 CRC 杂糅的 `model.dat.bak`（回滚备份）。主 `model.dat` 由模型内部 `mutex_` 串行、不受影响，坏的只是回滚副本。现改为 `mkstemp("<bak>.tmp.XXXXXX")`，每次调用生成唯一临时叶子（0600/O_EXCL，叶子不存在故无尾符号可跟随，原 O_NOFOLLOW 防护被原子创建取代），保留 read EINTR 硬化/fsync/失败 unlink/rename 语义。**穷尽同类**：其余 4 处 pid-based tmp——`aatpe_ml.cpp:534 LearningModel::save`（内部 recursive_mutex 串行）、`aatpe_ml_hybrid_persist.cpp:19 HybridLearner::save`（内部 mutex 串行）、`aatpe_ml.cpp:1866 saveTopologySidecar`（唯一生产调用点为周期 save 单线程）、`aatpe_command_queue.cpp:239`（命令 id 由原子计数器生成、每命令 finalPath 唯一）——均经代码证明由内部锁/单写者/唯一 id 串行保护，安全不改。新增 `test_backup_model_race_v600`（两线程并发 backup，最终 .bak 与源 byte-identical、无 .tmp.* 残留，TSan 0 race）。
- **[门禁] 两条静态回归守门**：release_ci 新增 (1) g_cpuMetrics 守门——aatped_main.cpp 每个 `g_cpuMetrics.find/[]/begin/end/range-for` 必须在 12 行窗口内有 `(lock_guard|unique_lock)(g_cpuMetricsMutex)`（写侧采集循环列已审计白名单），有人删锁即 FAIL；(2) backupModelFile 守门——函数体内必须出现 mkstemp、不得有 `tmpBak = ... getpid()` 赋值，回退 per-process 命名即 FAIL。双向演示：当前树 PASS，人为删锁/改回 getpid 的副本均 FAIL。
- **[打包] check-manifest-layout 收紧**：`sched_events.bpf.o` 由"仅存在"改为非空（`-s`）且校验 ELF 魔数 `7f454c46`；stray `.o` 检查改为全深度、prune build/_install/.git 目录，并对合法产物 `ebpf/sched_events.bpf.o` 显式白名单，避免开发机跑过 make ebpf-prepare 后被误杀。
- **[验证]** 终审三门 **136/0/0**（普通 / ASan+UBSan / TSan，TSan 0 race、无 ABBA、FI-23 40/40）；v6.0.0 新码 fuzz 非 web **37.58 亿** + web **21.81 亿**、**0 crash、0 鉴权绕过**；新码 **2h soak GREEN**、RSS 有界、无死锁。

### 真机验证（首份真机回传，round3.2 候选 rel5，2026-10-18）
> 权威依据：DC 落盘报告 `artifacts/v600_realdevice_report.md`（采集包 rdcollect_20261018_013306）。首台真机跑通，host 旧 DEGRADED 对账 **7 PASS / 0 FAIL / 1 仍 DEGRADED**，0 个模块级 bug。

- **真机环境**：Xiaomi M2104K10AC（chopin）/ MediaTek mt6893（天玑 1100 级）/ Android 12（SDK 31）/ 内核 4.14.186（aarch64）/ **页大小 4096**（16KB 设备未覆盖）/ arm64-v8a / 8 核三簇（LITTLE cpu0-3 cap=241、BIG cpu4-6 cap=917、PRIME cpu7 cap=1024）；root 框架线索推断为 **SukiSU Ultra**（logcat `com.sukisu.ultra` + 模块位于 `/data/adb/modules/aatpe` + supolicy minimal 规则 applied）；**SELinux Enforcing**，bpf/perf_event 相关拒绝 0 条。
- **版本核对**：四二进制 sha256 与 rel5 基线逐个完全一致（aatped 2,821,112 / aatpe-cli 818,800 / aatpe-watchdog 708,768 / aatpe-probe 552,960）；`aatpe-cli version` = `v6.0.0 (versionCode 4411)`、module.prop 4411；**无 integrity_degraded**；权限收口全部 root:root（`/dev/aatpe`=700、state/=700、aatpe.log=600、manifest.sha256=600、`/data/adb/aatpe`=700），无 /sdcard 写入。
- **eBPF 真附着 PASS**：`ebpf_active=1`、`embedded_syscall`/`event_driven`；created 4 maps（events=10 counters=11 wakeup_cache=12）；8 CPU per-cpu ring、event_counters total=17；**BPF self-test PASSED（delta=287 事件，重启二次 delta=156 同样 PASSED）**；ctx offset verify=true；**wakeup_cache 直接建成 LRU_HASH(type=9)，未触发 HASH(type=1) 回退**（4.14.186 直接接受 LRU_HASH，兜底路径本机未触发）；0 条 verifier reject、0 条 bpf/perf_event avc denied；ring 自适应降页（128→64）正常工作。
- **安全拦截 PASS（且比预期更严）**：无签名 `model_save` → **HTTP 403**（HMAC signature required）；无签名 `shadow_dump` → **HTTP 400**（command not in whitelist——影子导出本就只走 root control pipe，HTTP 面不存在该命令，纵深拦截更靠前）；`/health` → 200；root CLI 带签名 `shadow_dump=1` → auth=1 ok=1。
- **温控/PSI/FPS/限频 PASS**：PSI `cpu_some=19.14 / io_some=4.24 / mem_some=0`；**`cpu_throttled=1`（cap 2.6→2.0GHz，热限频真实生效）**；mtktscpu 56℃；28 个 thermal zone 按 type 动态遍历无写死；SurfaceFlinger 120Hz、frame p95=4ms。
- **拓扑/调度 PASS**：三簇自动识别 capFail=0 freqFail=0、无任何机型/频率/核数硬编码；快照决策 76,782 / 迁移 4,634 / 跟踪线程 511，decision_fusion 真实运行。
- **重启/基线 PASS**：`boot_fail_count=0`、两次 boot healthy；6 个跨天 install 快照（manifest total_entries=0，按 ROUND-013 设计不快照系统节点）；5 个 conf 已留 sha256 基线。
- **唯一仍 DEGRADED：passive 模式影子采样真数据**。本机为 active 模式，影子采样块入口仅在 `controlMode=="passive"` 时进入（设计如此），故 collected=0 / drained=0 / [SHADOW] total_lines=0 **非缺陷**。间接反证 extractMlFeatures 真机产出真实 14 维特征：主模型 samples=73、pool0 samples=45、14 维权重 `w1..w14=[+0.02 +0.00 +0.02 +0.03 +0.04 …]` 非零、ColdStart COMPLETE；shadow-dump 机制本身工作（auth=1 ok=1）。待补：切 `set_mode=passive` 重跑 shadow-dump 采一份非零 JSONL。
- **环境限制（非模块 bug，3 条）**：MTK GPU 无 freq 节点（仅 GED util，GPU 检测本身成功）；4.14 内核无 uclamp 自动退化 stune_cpuset；device_profile 名 "unknown" 但簇/capacity 分类全对。采集脚本自身 3 处工具修正（daemon.pid 路径、toybox 下 0 字节 manifest、embedded_syscall 不 pin 的误判），属我方脚本、非模块。
- **仍待补真机项**：16KB 页设备、passive 影子采样、配置原子写 fsync 断电耐久、更长 soak/高负载游戏（当前 uptime=110s 短时数据）、高通 KGSL 平台、C4 freezer 真实冻结（注：默认已于 round4c 翻转为 ON，此处"默认 OFF"为当时记录）。

### 真机卡顿修复 round4（锁小核/皮肤热限误判，2026-10-19）
> 依据：根因报告 `artifacts/v600_lock_little_core_rootcause.md` + G7 三门复跑。仍 v6.0.0/4411 候选修订。

- **根因（不是主动锁小核）**：AATPE 把 Android **皮肤热限**（thermal HAL level≥3，由外壳/握把温度触发，真机 CPU 仅 ~72℃、远未到 ~120℃ 温度墙）误当成"CPU 过热、停止一切干预"的硬墙，wall 死 cgroup 自救。AMS 把前台游戏任务组切到 background cpuset（cpus=0-3）后无人救回，叠加 ①cpuset pre-check 把"想迁大核"的请求全部 CPUSET_SKIP、②edge 恢复只救 AATPE 自己 pin 过（`affinityModified==true`）的线程，于是前台游戏线程永久困在 LITTLE 小核。面板上 FPS 0.1/0.2 是 Timestats 历史环形缓冲区被整段前台会话时间窗稀释的统计假象，与锁小核是两个独立问题（全局 GFXInfo 120fps / 0 丢帧才是当前真实状态）。
- **修复（区分两套热信号，不放宽阈值/不关热保护）**：
  1. `placement_heal_decision.cpp` 拆分：仅 `thermalEmergency`（CPU sysfs 真温度墙）维持 wall；**皮肤热限 `thermalHalLevel>=3` 但 `!thermalEmergency` 不再 wall**——cgroup lift 只改亲和性归属、不升频不增功耗，限频由 Android cpufreq 自管。其余条件（screenOn/非 passive/role∈UI·Render·Game/threadPkg==fg/非 top-app/cap）全保留。
  2. `placement.cpp releaseForegroundDownpins` edge 扩面：对新前台包 UI/Render/Game 线程，即使 `affinityModified==false`（从未被 AATPE pin、纯被 AMS 困 background）也读 live cgroup，非 top-app 则 lift 回 top-app（**只 lift cgroup、不写 affinity mask**，交正常 placement 循环）；周期 heal 注释同步更正。
  3. `frame.cpp`：App 最近 >5s 无新帧（ageMs>5000）时 fps 置 0、`reason=background_stale`，WebUI 显示 "--" 而非吓人的 0.1/0.2；保留 samples/dropped/lastSeen 原始数据。
  4. 可观测性：新增 `CpusetSkipSample` + `g_lastCpusetSkip`，cpuset pre-check 命中时快照最近被拒样本，状态目录写出 `apply_cpuset_skip_sample`（tid/pid/targetCpu/maskCount/pname），与原 `apply_cpuset_skip` 计数一起暴露。
  5. 门禁：release_ci 新增 heal-wall split 静态守门（禁止回退成无条件 `thermalEmergency || thermalHalLevel>=3`）；并把 g_cpuMetrics 锁守门从"钉死行号"改为**大括号深度作用域追踪**（锁作用域内访问自动视为受保护，删除行号 allowlist，不再随代码漂移误报）。
- **验证**：G7 独立三门 **136 ok / 0 fail / 0 skipped ×3**（普通 + ASan/UBSan + TSan），TSan 0 race、无 ABBA；rel7b 干净重编可复现（aatped sha 6797fce1…、p_align=0x4000、release_ci 全绿）；新守门双向验证（当前 PASS、删锁/作用域外注入无锁读均 FAIL）。
- **仍需真机验证（DEGRADED）**：刷 rel7b 后游戏中/切后台再回前台，观察线程是否回到 top-app 大核、`apply_cpuset_skip` 计数与最近样本是否符合预期；16KB 页设备、passive 影子、fsync 断电耐久、长 soak/高负载、高通 KGSL、C4 freezer（默认已于 round4c 翻转为 ON）仍待补。

### 真机帧统计口径修复 round4b（latency 真实渲染率，2026-10-20）
> 依据：真机现场包 `scratch/gamefps_chk/gamefps_20261020_011125/` 逐文件解包 + G7 rel8 三门复核。仍 v6.0.0/4411 候选修订；rel8 aatped sha 5c634827…（较 rel7b +4800B）。

- **真机根因（基于 gamefps_011125 实测，非臆测）**：①这台天玑1100/MIUI 上 SurfaceFlinger `--timestats` 通道**完全为空（0 字节，默认未启用）**——游戏帧统计的主通道本就无数据，旧代码却未正确降级，把空/历史累积直方图加权成瞬时 fps，落出被稀释的 0.1/0.2；②`--latency` 通道实测正常（真内容层 127 个非零帧、present 间隔 8.2–8.4ms、三次采样末帧单调推进），但容器/ActivityRecord 层全是 `0 0 0`、latency 环形缓冲仅 128 帧（120fps 只覆盖 ~1.07s）；③面板上的"全局 120fps"是当前前台 **View 应用 gfxinfo** 的渲染帧率（Total 3174 / p50=8ms / janky 0.88%），SurfaceView 游戏 gfxinfo 无帧行，被错误顶替成 120。
- **修复（latency 为主通道、timestats 降为可选增强，全架构动态不写死）**：
  1. `frame.cpp parseSfLatency` 新增 `renderFps=(presented−1)/(lastPresent−firstPresent)`——取 actual-present 列、在 128 帧环形内按真实 present 时间戳算瞬时渲染率（非面板 period）；INT64_MAX（未展示）行按 present 列有效性判定；全 0 容器行直接跳过。
  2. `frame.cpp sampleGameLayer`：最高分层若 0 有效帧（全 0 容器），遍历其他引用前台包的候选层，选中真正连续 present 的内容层（多内容层取 Δframes 最多者）。
  3. 上报 fps 由面板 `hz` 改为 `renderFps`（>1.0 时，否则回退 hz）。
  4. `frame.cpp processManualGameLayers`：timestats 走 per-layer 增量快照 `g_tsLayerSnap`（Δframes/ΔwallMs），窗口 tsMs 用最后活跃 present 时刻，停 present 后正常老化、ageMs>5s 落 `background_stale` 显示 "--"，不被累积 hist 稀释。
  5. WebUI 拆分两个字段：**屏幕刷新率（refresh-rate, Hz）**= 屏幕能力，**前台应用渲染 FPS**= 当前应用真实帧率（不再把前者误读成游戏帧率）；切通道/切前台包不跨包顶替上个 View UI 的 120。
  6. 采集脚本 `collect_game_fps.sh`：STATE_DIR 由误写 `/data/adb/aatpe/state` 修正为权威定义 `/dev/aatpe/state`（aatped_main.cpp:75，兼容回退模块目录）；timestats 读取前 best-effort `--timestats -enable`（拿不到就算，产品侧不依赖）。
- **验证**：G7 独立三门复核 普通 / ASan+UBSan / TSan **均 136 ok / 0 fail / 0 skipped**，TSan 0 race、无泄漏，峰值 RSS 0.43 / 0.66 / 1.47 GiB；release_ci round4b 静态守门双向（当前 PASS，sed 删 renderFps / g_tsLayerSnap 均 FAIL）；用真机 latency 数据复算 renderFps=121.4fps；rel8 干净重编可复现（release.zip 内 aatped=5c634827…，三处 webui 均含"前台应用渲染FPS(非屏幕刷新率)"、webroot==webui 一致、release_ci 全 PASS）。
- **仍 DEGRADED**：真机端到端待用户刷 rel8 复核（前台应用渲染FPS 是否显示游戏真实帧率、退后台是否 "--"）；timestats 在该机默认禁用故以 latency 为主通道；16KB 页设备、passive 影子、fsync 断电耐久、更长高负载、高通 KGSL、C4 freezer（默认已于 round4c 翻转为 ON）仍待补。

### 前台帧统计增量修复 + 功能开关开放 round4c（2026-10-21）
> 依据：用户刷 rel8 后真机复测（前台 SukiSU Ultra 单应用行 FPS=19.0 但 p50=6.8ms，自相矛盾）逐行定位 + G7 独立三门复核 GO。仍 v6.0.0/4411 候选修订；rel9 由 F7 重编。

- **前台 timestats 通道增量去稀释（核心漏修点）**：round4b 只在非前台手动游戏通道 `processManualGameLayers` 加了 `g_tsLayerSnap` 增量，**前台 timestats 通道 `TimestatsSession::tick` 仍直接用累积直方图加权 `avgFps=1000/加权平均间隔`**。这台设备 timestats 启用后，直方图是从启用时刻累积的：静态 UI 打开时有一批 6–8ms 快帧，静止期稀疏帧间隔 42–250ms 没被长间隔阈值过滤，混进 active 把平均间隔从 6.8ms 拉到 52.6ms → 显示 19fps（p50 却是 6.8ms）。本轮在 `TimestatsSession::tick` 为每个 layer/pkg 维护上一采样快照（`snapLayer/snapTotalFrames/snapDumpMs`），fps=ΔpresentFrames/Δ墙钟Ms（≥200ms 采样窗、封顶 300fps），只对本窗新增帧算间隔分布，历史累积帧不再稀释；本窗 0 新帧 → avgFps=0/activeFrames=0，前台帧稀疏或静止落"--"，与 latency renderFps 口径对齐。
- **active_ms 修正**：原 active_ms 只统计相邻窗口 gap≤4s，静态 UI 不 push 新窗口恒为 0ms；改为基于真实前台在前台时长累计。
- **功能区块开放（用户明确要求默认 ON，非默认关）**：
  1. **C4 后台冻结**：WebUI 由只读改为可切换，新增后端 `set_c4_freezer=0/1`，**默认 ON**（`c4FreezerEnabled=true`）。默认开带齐保护：前台/top-app 不冻结、音频播放/录音不冻结、IM/推送等核心白名单不冻结、解冻失败或杀后台导致消息异常时自动降级并写降级原因。安全（HMAC/鉴权/校验）与热保护（thermal_enabled）仍强制 ON，不因"开放"可关。
  2. **eBPF 事件驱动**：新增 `set_ebpf=0/1`，**默认 ON**（复用现有 `ebpfEnabled`）；仍走三种加载方式自动检测，设备不支持时如实降级回轮询，无副作用。
  3. **联邦学习**：新增 `set_federated=0/1`，**默认 ON**（复用已有 `federatedEnabled` 字段，本地聚合/导出）。
  - 切换统一走现有 /control 持久化 + 校验 + 操作日志；前端按钮实时反映状态；不写死机型/频率/阈值。
- **uclamp 测试脆弱性修复（seam 注入，不动生产动态逻辑）**：D8-1 原把期望 "87/100" 写死，但 `autocalBigRatio()`（aatpe_caps.cpp）动态读真实 sysfs cpu_capacity，在真机 MTK 上 ~917/1024≈0.87，在 host x86 云 VM 上 =0.6953——这是全架构动态的正确行为，不是 bug。新增 test seam `aatpe_caps.cpp autocalSetTestRatio(float r)`（r<0 复位 `g_autocalCache=-1` 回到自动探测，声明 `aatpe_caps.h`）。D8-1 开头 `autocalSetTestRatio(0.87f)` 注入；cgroup uclamp.min 是 max-merge floor（只升不降），故用与 P1-C 同款 asm extern seam 直接调纯函数 `uclampMinForTier_prod(BIG,0.87f,1)=622 > uclampMinForTier_prod(BIG,0.6953f,1)=497`，证明测试跟随注入比例而非写死；结尾复位。生产动态逻辑未改。
- **验证**：G7 独立三门复核 普通 / ASan+UBSan / TSan **均 136 ok / 0 fail / 0 skipped**，TSan 0 race（重点核查 per-layer 增量快照 map 与 g_perAppMutex 锁序，无 ABBA）、无泄漏，峰值 RSS 0.47 / 0.64 / 1.60 GiB；release_ci round4c 静态守门双向（前台 timestats 增量 `snapTotalFrames/Δframes` + 三个 set_ 命令，删即 FAIL）。
- **仍 DEGRADED**：gfxinfo 兜底窗口去稀释（latency 不可用时）列为下一步真机驱动项；真机 sgame SurfaceView 层 renderFps 选中与显示待用户用修好的前台探测脚本（优先读 frame_apps.json current）重采复核；热 HAL SEVERE（emergency=0）下是否仍在不必要限核待真机观察。

### FPS 终修：latency 优先 + 安全解耦 round4d/5/5b/5c/5d（2026-10-22）
> 依据：真机 136 层全图层普查（all_layers_fps 包）逐文件实测 + 独立逐行审查最终 P0/P1/P2=0。仍 v6.0.0/4411 候选修订。

- **round4d（WebUI 前端死按钮）**：主脚本是 `<script type="module">`，顶层函数不挂 window；round4c 新增的 toggleC4Freezer/toggleEbpf/toggleFederated 在文件尾 `window.xxx=xxx` 导出列表漏加，内联 onclick 找不到 → `ReferenceError`。补三行导出，并核对全量 `onclick="xxx()"` 又补出 `exportDiagLogs`（同样漏导）。C4 卡片去掉灰显"只读"、改"默认开·可切换"，按钮文案去"默认关"。
- **round5（FPS 根因终修，核心）**：136 层普查实测——前台内容层 `bin.mt.plus/l.…#0` 的 latency **present 列算出 101.1fps（n=125，span 1.23s）是真实渲染率**，但 AATPE 面板显示前台 12.4fps（来源 Timestats）。旧逻辑：前台通道 `if(!sfLatValid)` 里 timestats 一 `available` 就写值并 `return`，**根本不跑 latency**；该机 timestats 选层/统计本身不可靠（曾 enable 后 0 字节）。改为 latency 优先：前台先跑 binder/dumpsys latency 取 renderFps，latency 有效（>1）即作为前台 fps；两通道都有值且差异显著（**relGap>20% 或 absGap>15fps**——足够忽略 128 环形窗口抖动、又能抓到 101 vs 12.4 的 88% gap）时以 latency 为准并写 `frame_timestats_suspect=1`+reason；仅 latency 无有效 present 帧才回退 timestats。新增纯函数 `frameaware::chooseForegroundFps`（host 可测）。阈值全动态、不写死帧率/层名。
- **round5b（120°C 硬墙解耦，安全红线）**：root 经控制通道 `thermalEnabled=0` 本会连带撤销 120°C 紧急硬墙。新增 header-only 纯函数 `aatpeThermalEmergencyHardWall(temp,wall)`（wall≤0 兜底 120，**不接受 thermalEnabled 参数**），readThermal EMERGENCY 触发改调它；thermalEnabled=0 只关常规热感知调度/profile 自动降档/ML 冻结，**无法绕过 120°C die 硬墙**。核实 g_thermalEmergency 置位与全部 6 个消费方（arbitrator thermal_hard_wall、placement 迁移门、safemode、frame 停 IO、ML 硬墙 OR）均不被 thermalEnabled 门控。
- **round5c（P3 清理）**：set_thermal_enabled/set_c4_freezer/set_ebpf/set_federated 严格只接受 "0"/"1"（其它值报错，不再任意非0即开）；`watchdog::kDefaultLoadLimit` 由 120 对齐 Config 默认 95（缺省回退不再分叉）；删 aatped_main 冗余死前向声明；integrity.cpp OK 分支缩进；webui_http migration_cost 注释明确"WebUI 比 control 钳位更严"；WebUI 温控提示写明紧急墙 120°C 默认不触发（典型降频 95-105°C）、WARNING/CRITICAL 仍生效。
- **round5d（WebUI 白名单 P1）**：isCommandAllowed 漏列 round4c 三命令，HMAC WebUI 路径直接 400"command not in whitelist"（root 管道正常、唯独面板死）。补入 set_c4_freezer/set_ebpf/set_federated（值严格 0/1）；全量 diff 26 个控制命令，仅 set_control_mode/shadow_dump 不在白名单（WebUI 不调用，CLI-only，无第二处死按钮）。
- **验证**：独立审查最终 **P0=0 / P1=0 / P2=0**；三门 普通 / ASan+UBSan / TSan **136/0/0**（TSan 133/0/0、0 race），峰值 RSS 0.33 / 0.65 / 1.20 GiB；release_ci 新增守门双向（chooseForegroundFps/frame_timestats_suspect/三 set_ 命令/WebUI 白名单三 key，删即 FAIL）。
- **仍 DEGRADED**：真机最终端到端对照（刷 rel10 后前台滚动/动画 FPS 是否贴近真实 present、静止 "--"、三按钮可点）待用户实测；gfxinfo 兜底去稀释、sgame SurfaceView 真机选中仍列真机驱动项。

### 唯一构建标识 + 层名白名单放宽 + 配置默认开落地 round6（2026-10-23，rel11）
> 依据：真机 136 层普查遗留的阿拉伯语层名取证 + 用户要求"用户能一眼确认刷的是哪一版"。仍 v6.0.0/4411（RELEASE_ID=rel11，BUILD_DATE=2026-10-23）；rel 编号是构建标识、**不是版本 bump**。

- **现象→真相（isLayerNameSafe 误拒非 ASCII 层名）**：136 层普查中前台真实内容层 `bin.mt.plus/l.<db a8 db 98 d9 b0>#0`（阿拉伯语附加符号，6 字节多字节 UTF-8）被旧 ASCII 白名单 `[A-Za-z0-9_./()#@\[\]-]` 直接判不安全 → dumpsys 层名通道放弃 → 该层走了错误的统计通道。旧白名单是 v4.4.9 防 shell 注入时代写的；但层名早已经 `aatpeExecArgv`（fork+execvp、独立 argv 元素、**无 shell**）传递，非 ASCII 字节在理论上也无法注入 shell 元字符——白名单防得过头，把真机存在的合法多字节层名（CJK/阿拉伯语）误杀了。
- **修复（frame.cpp isLayerNameSafe）**：校验规则从"ASCII 白名单"改为"合法 UTF-8、无控制字符"——接受 ASCII 可打印（0x20-0x7e）与结构合法的多字节 UTF-8（CJK、阿拉伯语附加符号等）；拒绝 C0 控制字节（0x00-0x1f，含 \n \r \t \0）、DEL(0x7f)、非法 UTF-8（孤立续字节、overlong、截断前导、UTF-16 代理对、>0x10FFFF、0xff/0xfe）；长度上限由 256 字节放宽到 **512 字节**（非 ASCII 按字节膨胀）。纵深防御不变：即便将来误退回 shell 路径，伪造层名也夹带不了换行/空字节进日志 argv。测试 `tests/v94/test_frame.cpp` T6 组 13 断言：阿拉伯层名 `ACCEPTED`、中文层名 `SurfaceView[com.游戏/主界面]#0 ACCEPTED`、`line\nbreak`/NUL/0x7f/0xff/孤立 0x80/overlong `\xc0\x80` 全部 `REJECTED`。
- **唯一构建编号 rel11（VERSION / aatpe-version.h / module.prop / WebUI / state.json 五处同源）**：v6.0.0/4411 自 rel7 到 rel10 完全相同，用户无法区分自己刷的是哪一版。本轮新增 `RELEASE_ID=rel11` + `BUILD_DATE=2026-10-23`：`VERSION` → `aatpe-version.h`（`AATPE_RELEASE_ID`/`AATPE_BUILD_DATE`）→ `module.prop`（version=`v6.0.0 (rel11)`）→ WebUI 页脚黄标 `rel11 · 构建于 2026-10-23` → `aatped_state.cpp` state JSON 新增 `"release_id":"rel11"` / `"build_date":"2026-10-23"`。**用户核对方法：WebUI 顶部黄标应显示 rel11；或 `cat /dev/aatpe/state/*.json` 见 `release_id=="rel11"`。**
- **配置默认开落到 conf 级（round4c 承诺的收口）**：round4c 把 C4/eBPF/联邦三个开关在 WebUI 放开为默认 ON，但 `aatpe.conf` 里 `C4_FREEZER_ENABLED=false`、`FEDERATED_ENABLED=0` 仍写着旧值，`aatped_config.cpp` 的缺省兜底仍是 `false`——等于"WebUI 显示默认开、配置文件和解析缺省却默认关"的自相矛盾。本轮三处 conf/三个镜像副本全部翻转为 `C4_FREEZER_ENABLED=true` / `FEDERATED_ENABLED=1`；`aatped_config.cpp` C4 缺省兜底改为 `parseFlexBool(val, true, ...)`；`aatped_types.h` `c4FreezerEnabled=true`；`c4_freezer.h` 运行时原子 `g_enabled{true}`。安全（HMAC/鉴权/校验）与 120°C 紧急硬墙仍强制 ON，不受影响。
- **验证**：host 三门 普通 / ASan+UBSan / TSan 维持 **136/0/0**（T6 UTF-8 校验组随 frame 测试并入）；`check-source-consistency.sh` 验证 VERSION/version.h/module.prop/WebUI/state 五处 rel11 一致。
- **仍 DEGRADED**：rel11 发布物（release/source.zip 重打 + 新 sha）由后续构建统一产出；真机上阿拉伯语/CJK 层名是否被正确选中、`release_id` 字段是否落盘，待用户刷 rel11 包复核。

### 全量逐行复审 22 个 P1 修复 round7（2026-10-23，rel11 同版）
> 依据：9 个分片对 round4b~round6 全部改动文件做**逐行（非抽样）复审**，新发现并修复 22 个 P1。仍 v6.0.0/4411/rel11；全部为正确性/健壮性修复，无功能变更。

- **帧统计两路（frame.cpp）**：
  - **jankyPct 单位双缩放**：latency 通道写 per-app 指标时 `missRate` 已是 [0,1] 比值，旧代码又 `*100` → WebUI 丢帧百分比被放大 100 倍（1% 显示成 100% 量级）。改为直接写 `missRate`。
  - **binder 通道不产 renderFps**：renderFps 只在 dumpsys 解析路径计算，binder 直连路径 `smp.renderFps` 恒 0 → 走 binder 的前台应用 fps 退回面板刷新率（120Hz）而非真实渲染率。binder 路径补上同一 present 环形渲染率计算。
- **eBPF 两路（aatpe_ebpf.cpp/.h）**：
  - **只给 cpu0 建 ring**：事件环形缓冲只为 cpu0 map，其余 CPU 上的事件无处落地。改为为**每一个 online CPU** 建 ring。
  - **恢复状态机不匹配**：降级路径写 `DEGRADED_RING`，但恢复/自检路径只认别的枚举值 → 恢复判断永不命中、降级后无法自愈。恢复路径对齐 `DEGRADED_RING`。
- **看门狗主动温度扫描（aatpe-watchdog.cpp）**：`effThermal` 只在系统已报 EMERGENCY 时才看温度，属于"被动等上报"。改为**主动独立扫描温度**（不等系统 emergency 上报），扫描结果可强制生效，避免 HAL 延迟上报期间窗口失控。
- **placement 回拉（placement.cpp）**：前台切换后回拉线程时，未区分"该 tid 已被迁进 top-app cgroup"的状态——重复 lift 或漏 lift。补上对已在 top-app 的 tid 的正确分支。
- **ML 时间戳精度（aatpe_ml.cpp/.h）**：outcome 时间差原来只到秒级（wall clock），亚秒级调度结果算不出 Δt → ML 特征时间维度失真。改为**单调时钟纳秒 delta**（`before_mono_ns/after_mono_ns`），秒级时间戳降级为兜底。
- **拓扑 part 生产启用（aatped_topology.cpp）**：从 `/proc/cpuinfo` 读 MIDR part 的生产路径此前被 `#ifdef` 关掉（测试脚手架状态），真机拓扑识别缺一角。启用生产读取（动态、不写死 part 号）。
- **恢复状态机（aatpe_recovery.cpp）**：恢复 uclamp 时未置 `UTIL_CLAMP_MIN|MAX` flag，内核直接忽略写入；且 `return 0` 分支会在状态真正恢复前就删掉 `.rec` 文件。补上 flag 与返回序。
- **温控路径选择（thermal.cpp）**：thermal zone 温度路径用 `substr(12)` 硬切，目录名长度一变就切错。改为按 zone 名拼接 `/sys/class/thermal/<name>/temp`；ML 训练冻结阈值改为**从紧急硬墙派生**（配置驱动，不写死某芯片温度），固定迟滞恢复。
- **WebUI HTTP 头解析（webui_http.cpp）**：Content-Length 等搜索扫到了响应体，改为只搜 header 块（CRLFCRLF 之前）。
- **控制命令严格 0/1（aatped_control.cpp）**：set_* 开关此前"任意非 0 即开"，严格化为只接受 "0"/"1"（与 round5c 前台口径一致）。
- **写循环零写死循环（thread_scan.cpp / webui_json.cpp）**：`write()` 返回 0 时无 break → 死循环空转。补 `n==0 break`。
- **诊断取 MAX（aatpe_diag.cpp）**：温度/负载诊断此前按列表顺序取最后一个，改为**与顺序无关地取 MAX**（实测平台多 zone 时不再漏报最热）。
- **冷启动归属竞态（cold_start_rules.cpp）**：清冷启动包状态时未检查"前台是否已切换到别的包" → 会误清新前台的冷启动状态。仅在仍属自己时才清。
- **日志自死锁（aatped_logging.cpp）**：持 `g_logMutex` 时走 `LOG_WARN→logMsg` 会重入同一把锁。改为锁内不递归。
- **GPU boost 可写检测（gpu_boost.cpp）**：`access(W_OK)` 在 root（CAP_DAC_OVERRIDE）下被短路，永远"可写"。改用真实可写性探测。
- **mkstemp 临时文件清理（aatped_utils.cpp）**：清理逻辑按后缀匹配 `.tmp`，而 mkstemp 产物是 `<name>.tmp.XXXXXX`（`.tmp.` 在中间）→ 旧尾匹配永不命中、临时文件残留。改为匹配中间 `.tmp.`。
- **配置原子同步（aatped_config.cpp / aatped_main.cpp）**：运行时原子开关只更新了 `g_cfg` 副本、没同步到实时原子 → 控制面与生效面分叉。补同步；outcome 批处理 schedstat 读取补 `g_schedstatMutex`。
- **验证**：host 三门维持 **136/0/0**、TSan 0 race；改动均有对应 host 断言或既有门回归；release_ci 静态守门双向复跑 PASS。
- **仍 DEGRADED**：以上 22 项的真机行为（eBPF 全 CPU ring 在 4.14 内核的事件落地、看门狗主动扫描体感、拓扑 part 真机识别、ML 纳秒时间戳在真机 outcome 上的分布）待 rel11 真机包回传对照。

### 修复（终审发现）
- **[P1·影子特征] 被动模式影子样本特征恒为全 0**：`decidePlacement()` 在 passive 门提前 return（`placement.cpp`），早于 `d.mlFeatures` 唯一赋值点，导致每条 shadow 样本记录的都是 14 维全 0 向量，离线"特征→结果"数据集无统计价值。现被动采样器改用与正式评分同一纯观测函数 `extractMlFeatures(ti, ti.currentCpu)` 现取 counterfactual 特征（严格只读：短锁取 metrics + 读 /proc，**不触发任何 boost/freeze/sched_setaffinity/sysfs 写**），并复用正式冷启动全 0 过滤（指标未就绪本轮不记、不耗预算）。新增测试 `test_ml_shadow_passive_v600`（20 断言，含 util 变化被实时跟踪、冷启动不记录）。
- **[FEAT·影子数据闭环] 有界影子样本导出**：新增 `LearningModel::drainShadowSamples(maxN,clear)`（≤512、同锁、FIFO、无 OOB）、控制面只读命令 `shadow_dump=1` 与 `aatpe-cli shadow-dump [-o f]`，JSONL 落 **0700 私有 `state/diagnostic/`**（绝不 /sdcard）；经既有 root-0600/HMAC 通道，未签名/未授权一律 403（与 model_save 同门）；CLI 以发送前 mtime 为基准、2s 有界轮询，只认本次命令写出的文件，杜绝慢 daemon 下空 drain 覆盖。
- **[SECURITY·S-02] 四处 root 写路径加固**：crash_signal（`aatped_control.cpp`）、control 启动清空（`aatped_main.cpp`）、看门狗日志（`aatpe-watchdog.cpp`）补 `O_NOFOLLOW/O_CLOEXEC`；配置持久化（`aatped_config.cpp`）由固定名 `.tmp` 改为复用 canonical `writeFileAtomic()`（mkstemp 唯一叶子 + 全写循环 + **fsync 失败即 unlink 中止、保留旧配置** + regular-file 校验 + rename + 目录 fsync），消除 racing 写共享 inode 与尾符号链接跟随。FI-23 故障注入测试验证 fsync 失败时旧配置保留、无 tmp 残留。
- **[FIX·eBPF] 预编译回退路径 `event_counters` max_entries 11→12**：与 `sched_events.bpf.c`/`sched_events_embedded.h` 对齐为 12/12/12，使 `COUNTER_WAKEUP_NOT_SUCCESS=11` 在 embedded 加载失败回落 precompiled 时也能命中（原会 lookup 静默丢失该诊断计数）。
- **[FIX·遥测] 影子累计计数 64 位读取**（`readInt64`，防 21 亿截断）；影子样本 pool 越界丢弃计数（不再把 UNKNOWN(-1) 误标为 pool=0）；profile 段 `ML_SHADOW_SAMPLING` 改用 `parseStrictBool`（与主段一致，`=on` 不再被静默吞掉）。

### 通用架构 / 红线
- 去除 `aatpe-version.h` 注释中的具体机型（Dimensity 1100）与 `Makefile` 的 `-mtune=cortex-a78`，统一为通用 `-march=armv8.2-a`（增强通用性，非降级；PIE/stack-protector/FORTIFY/relro/-Werror 全保留）；`device/collect.sh` 版本头改为不随版本漂移的通用描述。拓扑/capacity/频率仍 100% 运行时自校准，无写死机型/频率/核数。

### 门禁（v6.0.0 源码）
- host 全量 **普通 134/0、ASan+UBSan 134/0（0/0/0）、TSan 134/0（生产+测试 race=0）**；新增/改动测试 `ml_shadow_passive_v600 20/0`、`ml_shadow_v450 38/0`（含 drain/512 精确上界/FIFO 淘汰/pool 越界）、config/cli/control/state/watchdog/utils/ebpf 等全绿。
- 性能门诚实复测：界定 7 条 Android 热路径（冷的 `configSchemaValidate` 配置解析单列、不卡 2% 门），同环境并排 9 轮，**0 条超 2%**；bench 所链生产 TU 在两树间 0 diff、两 benchmark 二进制 SHA-256 一致，Δ 全为沙箱噪声——无热路径回归。
- 证据打假更正：v4.4.9 fuzz 总量按 DONE 行重新求和，非 web 37.99 亿 + web **10.34 亿（原 v4.5.0 文案误记 11.23 亿）** = **约 48.3 亿（原误记约 49 亿）**，16 目标 crashes=0；覆盖率 80.8% 为圈选 32 个纯决策模块 + 人工归一的口径（原始 gcov 46%），本版起如实标注口径。

### 已知限制（DEGRADED，不硬跑）
无真机/KVM：影子样本真机特征分布、`shadow-dump` 端到端 HMAC 拒绝、LRU_HASH 真机建图/自动驱逐/verifier/SELinux、eBPF attach、真机 ML/FPS/Thermal HAL 闭环、mkstemp 断电语义均待 arm64 Android 10+ 真机验证；无 KVM 下 QEMU 软引导 Android 永久判死。

---

## v4.5.0 (2026-10-17) — ML 影子采样 + eBPF 唤醒缓存内核原生回收 + host 穷尽硬化分类

> 在 v4.4.9 全绿封版（六簇红队无 P0/P1、约 48.3 亿次 fuzz 零崩溃、覆盖率 80.8%、性能 P50≤2%、2h soak 无泄漏；fuzz 总量 v6.0.0 已按 DONE 行重新求和更正）基础上，对"已知限制/延后项"做一轮 host 可行性穷尽，落地两项真正可在 host 验证的增强，并把被工具链/真机阻塞的项如实标注。新增两个 host 测试（ml_shadow_v450、wakeup_reaper_v450）。`C4_FREEZER_ENABLED` 仍默认 OFF；BPF 仍在无权限/不支持时自动降级。

### 新增功能
- **ML 影子采样（observe-only/passive 也采样）**：只观测（accessibility/辅助）模式此前不施加调度、也不采集任何 ML 样本，模型在该模式下无法学习/验证。现在即使在"只观测、不做控制动作"的模式下，ML 也照常采集 特征→结果 样本（shadow），并满足：① 只观测模式安全语义不变（仍不 boost/freeze/写系统）；② 影子样本与正式样本物理隔离，**绝不**进入 `update()`/漂移检测/权重，不污染线上决策；③ 样本进有界 ring（512）+ pending 复用 1024 硬顶，达上限最旧淘汰、dropped 计数，不无界增长；④ 配置开关 `ML_SHADOW_SAMPLING`（默认开，支持热重载）；⑤ snapshot.json `ml` 块与 `[SHADOW]` 日志暴露 shadow collected/dropped 计数。采样在只观测路径按约 2s/≤2 个节流。
- **F-03 eBPF wakeup_cache 内核原生回收（LRU_HASH）**：唤醒缓存（tid→wakeup 记录，8192 项）此前为普通 HASH，满后 ENOSPC 仅计数，依赖外部清理；4.14 内核无 `bpf_for_each_map_elem`(5.13)/`bpf_timer`(5.15)/userspace get_next_key(4.15)，无法做 BPF 侧周期扫描。现改为 **`BPF_MAP_TYPE_LRU_HASH`（内核 UAPI type=9，内核 4.10+）**，map 将满时由内核自动驱逐冷/孤儿条目，从机制上杜绝无界增长；userspace 建图先发 LRU(9)，若内核 EINVAL（更老/异常内核）回退普通 HASH(1)，保持 v4.4.9 既有 fail-safe。
  - 已用 NDK r30 clang21（实测注册 `-target bpf`，纠正此前"主机无 BPF target"的陈旧结论）重编 `sched_events.bpf.c` 并再生 `sched_events_embedded.h`；diff 仅 wakeup_cache map type `1→9` 一行（key=4/value=24/max=8192 不变），6 个 tracepoint 程序指令不变，反汇编确认无 JMP32/ringbuf/CO-RE 等 4.15+ 特性。

### host 穷尽硬化分类（FIXED 1 / BLOCKED 4 / WONT 19）
- **FIXED**：F-03（见上）。
- **BLOCKED（附亲自验证证据，不硬跑）**：① 真机 eBPF attach / GKI verifier / ring 丢包（无真机/KVM，LRU 已留 HASH 回退兜底）；② ML/FPS/Thermal HAL 真机闭环（host 无法接 SF/FrameMetrics/HAL）；③ assist 模式"无因果 outcome"样本（assist 不发 MIGRATE，强行采样会违背安全语义或注入非因果噪声，需真机漏斗）；④ BPF 侧周期扫描本身（4.14 ABI 无 for_each/timer）。
- **WONT / NON-ISSUE**：toybox tar 跟 symlink（diag 已在 0700 STATE_DIR 用 O_EXCL|O_NOFOLLOW；collect.sh 仅 echo 不真执行；全树无 `tar -x` 解压）；日志 ELOOP（O_NOFOLLOW 遇符号链接环一次性降级到 stderr、孤儿缓冲 1MiB 封顶不无限重试）；其余 15 项在 v4.4.9 已收口。

### 工程质量 / 测试
- 新增 `tests/v94/test_ml_shadow_v450.cpp`（影子开关/有界 ring/不污染线上模型/关闭零采集，23 断言）与 `tests/v94/test_wakeup_reaper_v450.cpp`（LRU 决策矩阵 + UAPI 锁定断言：首选与 embedded 均 ==9 且 !=3(PROG_ARRAY)，17 断言）。
- 收口两处 v4.4.9 未纳入 TSan 92 套件、但在 v4.5.0 全量 TSan 下首次暴露的**测试侧**毛刺：`test_spillover_deadlock.cpp` detached 线程逃逸作用域写栈槽（改确定性 join）；`test_webui_cors.cpp` C8 客户端 3s 超时在 TSan 插桩下不够（放宽到 15s，断言仍严格要求 200）。生产代码零改动；两者在未改 v4.4.9 上原样复现，确认为既有测试侧问题。

### 门禁（host-side + arm64 交叉）
- 普通全量 133/0/0；ASan+UBSan 全量 133/0/0（ASan/UBSan/Leak=0）；TSan 全量 133/0/0（生产与测试 data race=0，含 4 线程各 20 万次 shadow ring/计数/pending 并发 hammer 0 race）。
- 被动模式有界 soak：shadow ring 推样 5000 即封 512 并恒定、pending 1.5 万全新 tid 压到 1024 封顶、RSS 一次性 +768kB warmup 后全程平、fds 4→4、零控制动作（不调 update()、alpha 不变）。
- BPF 字节码 4.14 自检通过（6 tracepoint、无 4.15+ 特性、map type=9）。v4.4.9 全部既有门（覆盖率 80.8%、性能 P50≤2%、2h soak、约 49 亿 fuzz）保持。

### 已知限制（DEGRADED）
- 无真机 / 无 KVM：BPF 未在真实内核加载，LRU 驱逐与 verifier 接受性、真机 ML 闭环、王者 FPS、Thermal HAL 真机、eBPF attach/SELinux 均待真机；不硬跑安卓虚拟机（QEMU 软引导 Android 永久判死）。v4.5.0 影子样本 features 部分为占位（outcome/pool/tid 为真实信号），真机填充率需读 `ml_shadow_*` 漏斗确认。

---

## v4.4.9 (2026-10-17) — 六簇独立敌意红队 + 全量修复收口 + eBPF fail-safe + 覆盖率达标

> v4.4.8 封版后，由 6 名与 v4.4.8 审查者不重复的独立红队（核心调度 / 线程场景 / eBPF 安全 / 帧与 ML / 热态温控 / 配置·Web·打包）敌意审查全树，结论无 P0/P1；全部 P2/P3 按"修前可复现 → 修后有回归"逐条闭环。另含 3 个 MINOR flaky 修复（fault 注入清理 35s、aatpe_ml 日志初始化空指针、pm-group restart 30s）。`C4_FREEZER_ENABLED` 仍默认 OFF；eBPF 在无 BPF/无权限时自动降级，不崩不卡。

### 安全收口（loopback/root 本地威胁模型）
- **签名命令降级分支防重放**：WebUI"不强制 HMAC"模式下，一个已签名写命令（切配置/清日志/改控制模式）此前可无限重放；现强制 ±300s 时间窗 + 写命令必须携带并一次性消费 nonce，裸签名（无 ts/nonce）不得授权写。
- **诊断包不再写世界可写目录**：diag 输出从 `/data/local/tmp/aatpe-diag.tgz` 改为 0700 私有 STATE_DIR，`O_EXCL|O_NOFOLLOW|O_CLOEXEC` 预建常规文件，失败 fail-closed，防本地 App 预建 symlink 诱导 root tar 跟写。
- **nonce 缓存改用服务端时钟**：缓存存活不再用客户端 reqTs（防未来时间戳投毒挤掉合法 nonce）。
- **统一防符号链接**：日志兜底、配置、看门狗、ML 拓扑旁路、state TTL 等路径统一 O_NOFOLLOW；配置改"打开 fd 后 fstat"、state TTL 改 `openat+fstat+unlinkat(dirfd)`，消除 stat→open / stat→unlink 的 TOCTOU。
- **命令队列入队二次白名单**：入队即按与 drain 相同字符白名单拒绝 shell 元字符/NUL。
- **日志注入剥离**：在 CR/LF/TAB 基础上剥离全部 C0 控制字符与 DEL（含终端转义 ESC）。
- **脚本输入校验**：service.sh 对 promoted 线程 id 做非负整数 + pid_max 范围校验；uninstall.sh 白名单恢复路径拒绝 `..`。
- **jsonEscape 单一实现**收敛到 webui_http.h；**HMAC 常量时间比较**定宽 64，session 8 槽恒做 64B 比较。

### 核心调度 / 并发正确性
- 虚拟机客户机进程名缓存（g_vmGuestCache）加锁，TSan 4 线程 75 万+ 次零 race；重启后 uclamp 账本重建（热降额不再静默跳过）；热墙覆盖 Thermal HAL≥3。
- c4_freezer 状态 JSON 补 s_stateMutex（生产模式 tick 经 TSan 插桩零 race，去锁对照复现 3 race）；预 boost 生产分支改 steady_clock（修复 15s 时间箱失效）；场景切换计数原子化；负载预测 min_samples=0 除零保护；uevent `/cpu` 收紧为必须跟数字。
- 配置整数解析统一 parseNonNegChecked（拒绝溢出）；capability 自动校准区分有效校准与保守默认（修复新设备首次校准永不触发）。
- **profile 仲裁"同名升源"一致性**（TSan 门发现的真实状态机缺陷）：profile 名不变但获胜来源变化（APP 应用 gaming 后 MANUAL 也投同名）时，仲裁早退不更新来源，导致 hasManualVote 与 effectiveSource 不一致；现同名不重载但同步来源，释放后正确逐级回落。

### 帧感知 / 机器学习
- 漂移检测器量纲契约对齐（生产喂 [0,1] 融合分、中性 0.5；旧实现按有符号中性 0 判，退化分支不触发、自愈被静默禁用而单测假绿）：<0.5 退化、>0.5 清零自愈、==0.5/NaN 不动。
- parseSfLatency 兼容 MIUI 2 列行（此前只收 3 列被整条跳过）；GfxInfo 无百分位时不再伪造 p50=16.67/valid，如实标记不可用。

### eBPF 调度器
- 新增 isEventDrivenLive() 真实存活探针，守护进程据其发布真实状态；加载失败按枚举收敛降级（修复降级枚举错配重复刷 WARN，现只告警一次）；ELF 解析改有界 NUL 读取（未终止损坏串立即拒绝，修前 ASan 复现 heap-buffer-overflow）；pid starttime 上界校验。

### 工程质量 / 验证
- 纯决策核 host 覆盖率提升至 **81.1%**（>70% 达标，新增 scoring 纯逻辑测试，生产零改动）；新增 20+ 个 v449 回归测试。
- 验证矩阵（host，无真机/KVM）：全量 v94、ASan+UBSan、TSan、qa 风暴（ASan/TSan 双变体 20904 项）、100 次重启泄漏、fault 注入、长时 fuzz、2h soak、热路径性能 P50 回归 ≤2%。

### 已知限制（无真机/KVM，如实列出）
- eBPF 真机 attach、内嵌 BPF 字节码 GKI verifier 接受性、SELinux、真机 ring 丢事件率未实测（主机无 clang BPF target；F-03 wakeup_cache 有界 8192、ENOSPC 已计数，BPF 侧 reaper 列入 v4.5.0）；ML 真机闭环、真机 FPS、Thermal HAL、toybox tar 跟 symlink、日志 ELOOP 未实测；辅助模式"只观测也采样"列入 v4.5.0。

---

## v4.4.8 (2026-10-16) — 3 MAJOR 并发/注入缝收口 + 日志孤儿缓冲内存上限

> 在 v4.4.7 基础上做一轮并发安全与注入面收口。`C4_FREEZER_ENABLED` 仍默认 OFF。

### MAJOR 修复
- **包名注入缝收口**：外部传入包名/标签的拼接路径与命令行参数全部按可信白名单转义，杜绝通过畸形包名注入额外 argv/路径。
- **热降额对已 boost 线程生效**：温度触发热降额时，此前只对"未来要 boost"的线程生效，已经 boost 过的高优先级线程不回退；现在热降额路径会显式回退已 boost 线程的 uclamp/优先级。
- **大核池 data race**：大核池（big-core pool）共享计数器在并发 acquire/release 下存在未加锁读写，TSan 上报 race；改为原子/临界区保护。

### MINOR（约 30 项）
- **日志孤儿缓冲内存上限**：当日志文件不可写/未配置时，日志路径会在内存里无界堆积孤儿缓冲；本版给孤儿缓冲加上限（满后丢弃最旧并计数），避免长跑/无日志文件场景下内存缓涨。
- 其余约 30 项 MINOR 硬化（断言加固、边界检查、错误路径 fail-safe、未使用返回值检查等）。

### 验证
- host v94 功能门禁 **109 / 0 / 0**（含 log_orphan、pkglist 8/8、thermal_derate 9/9、bigpool_race、a2_atomics 4/4）。
- ASan+UBSan **92 / 0 / 0**；TSan **92 / 0 / 0**（webui_http 经测试侧收口后 5/5、零 race）。
- qa storm **10452 / 10452**；restart 100 周期无泄漏。
- 16 个 fuzz 目标各 1500 s 长跑全部 PASS、0 crash。
- 六簇整模块逐行审计（154 个 .cpp/.h/.c、71,409 行）+ 3 MAJOR 独立复核 PASS + 版本终检 SHIP。
- 干净 NDK r30 重编：六件套加固 24/24 PASS、release 零测试符号。

### DEGRADED
- 无真机 / 无 /dev/kvm，全系统安卓 / QEMU 不硬跑；相关项以 DEGRADED 清单列出。

---

## v4.4.7 (2026-10-15) — clang-tidy 高价值缺陷硬化（开关仍默认 OFF，对 v4.4.6 字节等价）

> 在 v4.4.6 基础上按 R8 跑 clang-tidy 21 全模块（73 文件），只修真实缺陷、不做风格噪声。
> `C4_FREEZER_ENABLED` 仍默认 OFF；OFF 时 daemon 行为对 v4.4.6 逐字节等价。

### 缺陷硬化
- **concurrency-mt-unsafe**：生产日志/错误路径的 `strerror(errno)` 全部换线程安全 `aatpeErrStr()`；
  `aatpeStrError` 由 .cpp 改为 header inline（GNU strerror_r / XSI 双分支）。CLI/watchdog 单线程保留并说明。
- **bugprone-empty-catch（5 处）**：parsePoolStr、watchdog pool/probe、/proc status 解析——
  `catch(...){}` 改为 `catch(const std::exception&){/* malformed 留痕 */}`，不再静默吞。
- **bugprone-implicit-widening（15 处）**：乘法前显式转宽（`size_t()/off_t()/time_t()/double()`），
  覆盖 apklabel/control/hmac/utils/ml/session_token/watchdog/frame/webui_http。
- **bugprone-incorrect-roundings（17 处）**：`(int)(x+0.5[f])` → `lround/lroundf`（psi+frame）。
- **cert-err33-c（生产 fsync）**：arbitrator/config/utils/subsystem 的 5 处 fsync 加判断，fail-closed/日志。
- 必要 POD 成员默认初始化（决策/线程安全相关），不全量改 member-init。

### 验证
- host v94 全量 **104 ok / 0 failed / 0 skipped**；新增 test_tidy_hardening 6 断言
  （errnoStr 并发安全、未知 errno 非空、lround 等价）。

### DEGRADED / 已知限制
- 未真机/KVM 实测：含 C4 面板真机渲染、cgroup.freeze 写入、media_session 归属、freezer 控制器、
  GPU kgsl/ged；不宣称"后台必冻游戏"。

---

## v4.4.6 (2026-10-15) — NN-NIT 持久化 + C4 WebUI 可观测 + cppcheck 修复 + 性能基准（开关仍默认 OFF，对 v4.4.5 字节等价）

> 在 v4.4.5（C4 NN/MN 持久态、collect/doctor 真机验证包）基础上闭环 R6。
> `C4_FREEZER_ENABLED` 仍默认 OFF；OFF 时 daemon 行为对 v4.4.5 逐字节等价。

### 新增 / 闭环
- **NN-NIT 持久化**：把 lastGame（最后一个前台游戏）纳入 `c4_frozen_state`（可选 `lg=` 字段）；
  daemon 重启后跨回合恢复目标身份。旧 v4.4.5 文件无 `lg=` 仍向后兼容、安全置空。
- **C4 WebUI 可观测**：新增只读 `c4.json`（enabled/target/frozen/cgDir/lastAction/audioActive/
  freezerAvailable/degraded）与 WebUI 只读 C4 面板；OFF 灰态、不报错、无写操作。
- **cppcheck 真实项**：`SseSlotGuard.uid` 默认初始化（uninitvar 防御）；`aatpe-cli` 三处 substr→resize。
- **决策热路径基准**（单核、N=200k，host）：decideC4Freeze P50=21ns/P99=31ns、c4ChooseTarget
  P50=30ns/P99=41ns、c4StateJson P50=451ns/P99~630ns；立基线供后续防退化。

### DEGRADED / 已知限制
- 未真机/KVM 实测：含 C4 面板真机浏览器渲染、cgroup.freeze 真机写入、media_session 块归属、
  freezer 控制器、GPU kgsl/ged；collect/doctor 待真机跑；不宣称"后台必冻游戏"。

---

## v4.4.5 (2026-10-15) — C4 NN 状态持久化 + MN 目标跟随游戏 + 真机验证包（开关仍默认 OFF，对 v4.4.4 字节等价）

> 在 v4.4.4（C4 freezer + GPU 硬化，均默认关）基础上闭环 R5 遗留的 NN/MN 两项，并交付真机验证包。
> `C4_FREEZER_ENABLED` 仍默认 OFF；OFF 时 daemon 行为对 v4.4.4 逐字节等价。本版本不改任何默认行为。

### 新增 / 闭环
- **NN（C4 冻结状态持久化）**：冻结成功后把「cgroup 目录 + 目标包 + uid」经 `writeFileAtomic`
  原子落盘 `state/c4_frozen_state`；daemon 首 tick 读回对账——若目标已回前台则对称解冻并清态，
  仍在后台则恢复记账（不再因 daemon 重启丢失已冻 cgroup）。纯函数 `serializeC4Frozen`/`parseC4Frozen`
  覆盖正常往返、损坏/半写/缺字段判 invalid。
- **MN（目标跟随最后一个前台游戏）**：纯函数 `c4ChooseTarget(lastGame, fgPkg, fgIsGame)` + glue
  接入 `gamelist::isManualGame`。回桌面（launcher/非游戏前台）时不再把目标改成 launcher，而是继续冻结
  仍在后台的上一个游戏；游戏回前台、切游戏、或目标 cgroup 解析不到（进程退出）才解冻/换目标/清目标。
- **真机验证包** `device/collect.sh` / `device/doctor.sh`（POSIX sh，随包发布到 `system/etc/aatpe/`）：
  collect 采集内核/SDK/CPU 拓扑/目标 app `/proc/<pid>/cgroup`/`cgroup.controllers` 与 `cgroup.freeze`
  可写性/`dumpsys media_session`/GPU(kgsl/mali/ged)/PSI/thermal；doctor 逐项判 PASS/FAIL/DEGRADED 并
  给出"C4 在本机能否工作"结论。
- **integration 构建修复**：测试 TU 补 `-DAATPE_HOST_TEST`（`setIntegrityVerdictForTest` 原不可见），
  PROD 列表补齐 v4.4.x 新增 TU，glue 符号由 `tests/stubs/scenewire.cpp` 提供。
- **长稳 soak**：260000 fast ticks + 1800s wall，matrix/fault/signal/cred 全 0，exit code 0，RSS 无漂移。

### 已知限制 / NIT / DEGRADED
- **NN-NIT**：`s_lastGame`（最后前台游戏）仅内存、未持久化；daemon 重启后依赖前台边沿重建目标。
- **DEGRADED（未真机/KVM 实测）**：cgroup.freeze 真机写入、pm 取 uid、media_session 块归属、freezer
  控制器、GPU kgsl/ged 节点——均待真机跑 `device/collect.sh` + `device/doctor.sh` 回验。
- **待真机复核**：integration S1/S2（沙箱 namespace 假 `/proc/<pid>` 不可见）、half-open SSE 槽在真机的回收行为。
- 不宣称"后台必冻游戏"；C4 在 freezer 控制器缺失/非 v2/权限拒绝时 fail-safe 降级不冻。

---

## v4.4.4 (2026-10-15) — C4 cgroup v2 后台冻结 + GPU 全架构硬化 + thermal 崩溃修复（开关默认 OFF，对 v4.4.3 字节等价）

> 在 v4.4.3（scene_infer + C2 信封，均默认关）基础上新增 C4 后台冻结开关与若干硬化。
> 新开关 `C4_FREEZER_ENABLED=false` 为默认；OFF 时 daemon 行为对 v4.4.3 逐字节等价。
> 本版本不启用任何默认行为变更。

### 新增（默认 OFF 开关路径）
- **C4 cgroup v2 后台冻结**（`c4_freezer.h/.cpp`）：游戏进入后台时对其本 uid 的 cgroup v2 写
  `cgroup.freeze=1`；回前台对称解冻（任何前台边沿先确保解冻，fail-safe，不残留冻结）。
  - **音频白名单**：后台边沿 + 每 5 慢周期复检 `dumpsys media_session`，该包 state=3(PLAYING) 则
    不冻；已冻结后若音频中途开始则解冻。exec 失败/超时/无法判定 → 保守视为有音频（不冻）。
  - **uid 精确锁定**：`resolveAppUid`(pm list) + `/proc/<pid>/status` 按 uid 过滤 + 纯函数
    `cgroupPathBelongsToUid()` 段精确匹配 `uid_<uid>`（uid_10123 不误配 uid_1012/101230）；系统
    cgroup（top-app/init 等）永不被冻。
  - 纯决策与纯解析函数全部 host 单测覆盖（decideC4Freeze / parseV2CgroupPath / buildFreezeDir /
    mediaSessionMentionsPlaying / cgroupPathBelongsToUid / audioReevalDue）。

### 硬化
- **GPU 全架构探测**：`classifyGpuPlatform()` 纯分类器（header-only inline），probeGpu 在现有
  Mali 之外补 KGSL/Adreno（`/sys/class/kgsl/kgsl-3d0`、devfreq kgsl-3d0）与 GED/MTK（`/sys/kernel/ged/hal`、
  `/dev/ged`）。
- **thermal 崩溃修复**：`thermalBusyPctHook()` 的 `fopen("/proc/stat")` 未判空，EMFILE 时
  `fscanf(NULL)` 会 SIGSEGV 杀整个 daemon；现 fopen==NULL 直接 return -1（同文件唯一 fopen，已核）。

### 已知限制 / 方向保守（MN/NN）
- **MN**：按 Home 回桌面时目标 retarget 到 launcher，游戏 cgroup 不被冻（收益偏窄，方向保守）。
- **NN**：C4 状态仅内存，daemon 重启不持久化已冻 cgroup（靠前台边沿对称解冻兜底）。
- **DEGRADED**：C4 写冻、pm 取 uid、media_session 块归属、freezer 控制器、GPU 新探测均**未在真机/KVM
  实测**（沙箱无 /dev/kvm、无真机）。不得宣称"后台必冻游戏"。

---

## v4.4.3 (2026-10-15) — scene_infer 场景推断 + C2 信封（两开关默认 OFF，对 v4.4.2 字节等价）

> 在 v4.4.2（C1 预测预调度 + C3 热余量平滑，均默认关）基础上新增两条默认关的开关路径。
> 两开关 `SCENEINFER_ENABLED=false`、`C2_ENVELOPE_ENABLED=false` 为默认；OFF 时 daemon 行为对
> v4.4.2 逐字节等价（`detectGameScene` 实参、`preBoostTick` 调用均不变）。本版本不启用任何默认行为变更。

### 新增（默认 OFF 开关路径）
- **scene_infer 场景推断**（`scene_infer.h/.cpp`、`scene_infer_wiring.h/.cpp`）：22 字段输入、
  5 场景（MENU / COMBAT / TEAMFIGHT / BACKGROUND / UNKNOWN）、纯函数 host 可测。斜率/节拍用
  steady_clock 真实墙上时间（修旧硬编码 5s 误配 25s 一拍的虚高），可测逻辑下沉纯映射层。
- **C2 信封**（`c2_envelope.h/.cpp`）：20s 时间盒、热闸（接真实热 HAL 等级，≥1 立即清 boost）、
  限流；LOADING 态经 `schedtune.boost` 桥提频。对局态实际 boost 依赖 C1 `PREBOOST_ENABLED` 同开。

### 安全/正确性硬化（沿用 v4.4.2，OFF 行为不变）
- Cgroup uclamp 探测改只读（不再对共享节点写 "101"）；uclamp 账本死亡回收（TID 复用不误抬）；
  horizon 下溢 clamp；trusted 温度哨兵改 bool；watchdog 移除厂商硬编码；布尔解析统一 parseFlexBool；
  测试缝 *ForTest 统一 `#ifdef AATPE_HOST_TEST` 守卫（release 零符号）。

### DEGRADED（据实，未实现能力不宣称）
1. **C2 LOADING 仅经 schedtune.boost 桥提频**——无大核迁移（tierTarget=2）、无 devfreq min-freq floor。
2. **生产 dRunDelay 源未接**（schedstat per-app）——无 GPU 设备时 TEAMFIGHT 在生产侧不触发；纯函数已支持非 0 输入，待后续接线。
3. **classifyGpuTag 把"无 PSI"与"无 GPU"耦合**（ON-path 低风险，建议后续独立依据 gpuDevName）。

---

## v4.3.6 (2026-10-13) — 三路已验证接线合并：PSI 决策闭环 + 温度前馈 + 触摸 per-thread uclamp

> 本版本将三路在隔离 harness 中已验证的纯逻辑核，接线进同一份生产 daemon 头图。
> 版本号仍按 v4.3.5 封版流程 bump，本节记录本次生产接线的修正点，便于后续升级说明。

### 新接线（三路执行面互斥，不重复 actuation）
- **PSI 决策闭环**：`psi_decision.c/.h`（纯逻辑 C 核）+ `aatped_psi.cpp/.h`（生产 seam）。
  仅写**组级 cgroup uclamp**（背景降档）+ 暂停非关键迁移 + cold-start IO-pressure hook；
  `placement.cpp` 内调用统一用 **全局限定 `::aatpe::psi::*`**（placement.cpp 嵌套于
  `namespace aatpe_spillover`，非限定会被解析成 `aatpe_spillover::aatpe::psi`）。
- **温度前馈**：`thermal_ff/thermal_ff.c/.h` + `thermal_ff_adapter.h`，折进既有 `cpuPressure`
  做 **max 加法提载**；`thermal.cpp` 每 tick 调 `therm_ff_update()`。热 EMERGENCY 硬墙权威
  （执行端 `max(wall_gear, ff_gear)`）。
- **触摸 per-thread uclamp**：`touch_uclamp.c/.h`（纯逻辑 C 核）+ `touch_uclamp_act.cpp/.h`
  （`TouchUclampController`）+ `touch_uclamp_fgscan.cpp`（生产前台线程扫描，链 `g_threads`）。
  仅写 **per-thread uclamp**（aatpe_mech::aatpeUclampSetMin）+ 全局 schedtune "touch" producer（max 合并）。

### 生产接线修正（harness 绿 ≠ 生产链接绿）
1. **符号隔离改名**：touch 纯核原 `typedef enum{...} ThreadRole` 与 daemon
   `aatped_types.h::enum ThreadRole` 撞名。统一改名为 **`TouchUclampRole` + `TOUCH_UCLAMP_ROLE_*`**
   （波及 touch_uclamp.h/.c、touch_uclamp_act.cpp、touch_uclamp_fgscan.cpp、tests/touch_act；
   同步 mech_proto 主副本并复跑 touch 32 checks 仍 0 fail，保持单一真源）。daemon `ThreadRole` 未动。
2. **host 桩（`tests/stubs/touch_host_stub.cpp`）**：host 单测不链生产 `g_threads`，
   `touchUclampCollectForegroundThreads()` 返回 0 线程集（fail-safe：控制器只留低 floor）；
   `aatpe_mech::aatpeUclampSetMin()` no-op。**integration 链真实 fgscan，不用此桩**。
3. **链接配方补链**（`tests/v94/build.sh`、`tests/san/recipes.inc`、`tests/integration/build.sh`）：
   prod_obj 支持 `.c`（gcc -std=c11 -lm）；placement 组注入 `aatped_psi psi_decision`；
   thermal 组注入 `thermal_ff/thermal_ff`；main 注入 `aatped_psi psi_decision touch_uclamp touch_uclamp_act`
   + 桩。host 桩里 `coldStartSetIoPressure`/`readInt` 用 **`__attribute__((weak))`**，
   真链 cold_start_rules/aatped_utils 的配方（coldstart 等）取真定义，placement 族无真链时兜底，
   避免多重定义。
4. **补丁损坏修复**：`thermal_ff/thermal_ff.c` 三处注释被补丁生成误写为 `/* --- /dev/null`，
   已用 mech_proto 干净主副本覆盖（逻辑逐行 diff 一致，仅注释行）。
5. **fgscan include 顺序**：`touch_uclamp_fgscan.cpp` 改为先 `#include "aatped.h"`（伞头带
   aatpe_ml/ThreadInfo/<mutex>）再含 `aatped_utils.h`，否则 `ThreadRole`/`std::mutex` 未声明。

### 跨模块组合场景（过载 + 触摸 + 升温同时发生）
`tests/cross_module/cross_module_v436.cpp`（16 checks, 0 fail）：
- 三动作**叠加不互覆**：PSI 组级降背景+停迁移、touch per-thread 提前台+全局 schedtune、
  热前馈提 cpuPressure，作用于不同执行面，背景降档不取消前台 boost。
- **恢复各自归零**：只松触摸 → touch 窗口到期归零，PSI/热保持；只清过载 → PSI 回 OK，
  touch/热不受影响。
- **EMERGENCY 权威**：硬件报 THERM_HW_EMERGENCY 时 wall_gear 顶格，`max(wall,ff)` 不被低前馈覆盖。

### 门禁（全绿实证）
- v94 普通 ALL：94 ok / 0 failed / 0 skipped（2482 项断言）。
- ASan+UBSan ALL：84 ok / 0 failed / 0 skipped。
- TSan ALL：84 ok / 0 failed / 0 skipped。
- check-html-sync：PASS（三处 index.html sha256 一致）。
- integration BUILD：OK（链接真实 fgscan + 三模块，产出 `tests/integration/aatpe_integration_test`）。

---

## v4.2.1 (2026-10-05) — 国产 ROM 游戏帧识别修复 + 手动游戏名单

### 修复
- **游戏帧识别系统性失败（国产 ROM / MIUI / MTK，如 Redmi chopin）**：SurfaceFlinger 图层名兼容四种格式（`[pkg/act]`、`Name - pkg/act#n`、裸 `pkg/act`、方括号仅包名）；引擎白名单从硬门槛改为排序信号，前台包精确匹配的 SurfaceView 渲染层即按游戏采样（覆盖王者荣耀 / Unity / Cocos / libGDX / 自研；输入法 / 相机 / 视频 / 广告仍拉黑）。

### 新特性
- **手动游戏名单**：WebUI「游戏管理」卡片读取已安装第三方应用（`pm list packages -3`），搜索勾选或手输包名添加，存 `/data/adb/aatpe/game_overrides.list`（升级保留、热加载即时生效）；名单内应用强制按游戏取帧。本地接口仅回环 + 包名校验。
- **帧诊断键**：新增 `frame_debug_fg` / `frame_debug_candidates` 与细分原因（no_fg / sf_list_empty / no_layer_for_pkg / no_valid_frames），WebUI 帧行展开显示前台包与候选层名。

### 测试
- v94 target 77→78（game_layer 37 断言）。
- 门禁：v94 78、integration 10640、ASan/TSan/fuzz 0 crash、restart 200 0 泄漏、stress storm PASS。

---

## v4.2.0 (2026-10-05) — 渲染线程保底 + ML 新能力（默认关）+ 工程加固

### 热修（v4.1.2 回流）
1. **ML 热训练闸分级**：MODERATE 继续用 schedstat 相对量攒样本、仅抑制帧主导训练；SEVERE+ 全停。解决持续游戏时学习样本恒 0。
2. **三簇稀疏拓扑(0/2/3) pool 特征归一**：按实际非空簇数归一，回到 0.3/0.6/1.0，训练/推理一致。
3. **高负载降级迟滞**：需 jank 异常或持续 10s 才降级，恢复驻留 15s，消除 NORMAL/DEGRADED 秒级抖动。
4. **看门狗子机 rapid_crash_loop 锁存对账释放**：健康+心跳新鲜 30s 解锁。
5. **GPU/WebUI 显示**：GPU 频率空值不再显 0/0MHz；root 下节点可写探测纠正（本机 R1/W0/N16、ctrl=0，不真实写）；皮肤温度/热余量键名回退显示；SF 延迟按有效位门控（无实测显 --）；非游戏前台文案与热状态独立性说明。

### 新特性（默认安全/关闭）
- **A 渲染线程保底**：渲染线程白名单 uclamp/schedtune 保底（SF mode=3 约 20% 下限）；placement 经 gpu_boost 单写者桥做 schedtune passive20/assist50/critical60 事件对齐；autocal 容量自标定（无 autocal.json 时等同旧版）。
- **B ML 三新能力默认关**：ML_OUTCOME_MAD_FILTER（抗离群污染）、ML_DRIFT_RELEARN（漂移再学习）、ML_EPSILON_GREEDY（边缘探索 0–0.05）。
- **C 冷启动拒绝计数**与六状态键上屏（state.json/system.json/WebUI）。
- **D 恢复记录补 cgroup 成员还原、bpftool 硬化 argv、CPU 热插拔 ring 安全重建、故障注入测试台。

### 新增配置键（aatpe.conf，默认注释/关闭）
- SCHEDTUNE_ASSIST_BOOST=50
- ML_OUTCOME_MAD_FILTER=0
- ML_DRIFT_RELEARN=0
- ML_EPSILON_GREEDY=0.0

### 测试
- v94 target 66→77。
- 加长稳压：重启 500、集成 10×106400 断言、sanitizer 62712 点 0 race、fuzz ~28060 输入 0 crash、fault inject 14 项、stress 3 轮。

---

## v4.1.1 (2026-10-05) — 真机分诊修复 + 交叉审查 13 项

热修版。真机日志分诊 + ChatGPT 交叉审查确认项收口；v94 target 59→66，集成 26→27 场景（10631→10640 点）。

### 真机修复
1. **看门狗开机误判 + 降级死锁**：开机 120s 宽限（开机风暴期不计 rapid crash、心跳超时 3× 放宽）；DEGRADED 期间守护进程存活且心跳连续新鲜 30s 自愈回 NORMAL，不再永久卡 assist；pidfile 活实例按 /proc starttime 收养，开机不再双启动互撞。
2. **温度三连错**：开机传感器返回 0 按未就绪丢弃，不再种假基线导致真实 46–50°C 被当毛刺拒掉；恒温/负载窗口改 RAW 毫摄氏度序列（50m°C 死区，不再抹平 MTK 毫度抖动误判钳位）；CPU 温度源严格分类（mtktsAP/mtktspa 为二级 CPU die，charger/battery/pmic/lcd 等永不入选，无合法 die 显式未知）；源被判 invalid 后回改 thermal_source；throttle 每轮按 CPU 频率比+GPU+cooling 证据并集重建。
3. **WebUI/GPU 显示**：状态文件分号键解析修复（GPU 行不再显畸形残串、PSI IO/内存不再恒 --）；GPU 区分可调频(ctrl=1)/仅利用率(ctrl=0)；thermal invalid 区灰色"(恒定/无效)"与真 0°C 区分；删除无 writer 死行、tick mode 按 eBPF 状态推断。
4. **GPU 守护进程探测（MTK GED 机型）**：全量枚举 /sys/kernel/ged/hal 13 节点与 gpufreq/13000000.mali 等平台设备（全程只读，access(W_OK) 只判不写）；GED 频率量纲自动判据（OPP 索引不造假 MHz）；新增 gpu_probe 状态键；能力探测认真机拼写 current_freqency。
5. **timestats 定性**：新增 frame_timestats_reason（disabled/selinux_denied/parse_empty/no_layer/unknown），本机 dump 落空定性为 MTK SF 未被 enable 打开（非 SELinux）。

### 交叉审查修复（13 项）
6. **冷启动 RT 真释放**：新增不受 Safe Mode 门控的 aatpeRestoreThreadPriority，释放时真正回到 SCHED_OTHER+原 nice+撤销 uclamp.min，不再被 level<1 钳成 1 而残留 SCHED_RR；shutdown/热限冻结同走该通道。
7. **dumpsys 硬超时**：冷启动 activity 与温度 thermalservice 两处裸 fork 统一改用共享硬化 exec（3s 总时限+setpgid/killpg+WNOHANG 有界回收+冷却，argv 不经 shell）；frame.cpp 重复实现下沉 aatped_utils 去重。
8. **温度限频证据**：由固定 cpu0/scaling_max_freq+cooling_device0 改为遍历全部 CPU 簇与全部 cpufreq 类 cooling device。
9. **采样线程**：detach 分支 fail-safe，消除退出期对已析构条件变量的 notify UB。
10. **前台→后台→回前台仍按后台逻辑卡顿**：前台切换边沿对该包关键线程强制重钉 top-app（绕过稳定计数去抖）；cgroup/cpuset 通道新增 /proc/<tid>/cgroup 内核成员回查自愈（漂移即重写）；边沿瞬间对齐两层前台缓存（不再等 3s TTL）；回前台对称抬 schedtune top-app boost（2s 过渡后按投票释放）；降级前重读真实组成员，不把系统已迁走的线程错误拽回。
11. **UCLAMP 兜底记录**：last-resort uclamp 成功才回写 mechanism，释放路径四分支均可命中，杜绝 uclamp.min 残留。
12. **cgroup 迁回计数**：moved=false 不再提前 active--/还原 group floor，失败保留并重试。
13. **recovery 事务化**：sched/priority/uclamp 三路恢复全部成功才删除 .rec，部分失败保留供下次开机重试；修正半恢复被误判"外部接管"而丢弃。
14. **开机清扫**：service.sh cgroup v2 改逐线程 cgroup.threads（cgroup.procs 仅 TGID），修复纯 v2 机型 PROM 清扫 EINVAL/ESRCH。
15. **uclamp 刻度**：utilToCgroupPct 跟随探测到的 cgroupUclampScale（raw1024 等），不再硬编码标准百分比。
16. **GPU devfreq 恢复**：off 路径校验写字节数+读回 min_freq，失败保留 dirty 状态与 marker 并每 tick 重试，不再污染 savedMin 钉死高频。
17. **eBPF**：RAW_TRACEPOINT 改用正确的 BPF_RAW_TRACEPOINT_OPEN 挂载（失败诚实降级，状态键反映真实 attach 方法）；ring 建立与 per-cpu 聚合改按 online CPU mask 真实 ID（sparse 拓扑 0,1,4,5 不再漏核/误统 offline 槽）。
18. **ML 特征**：pool 特征按簇数归一化，修复四级拓扑最强核被当最弱核；三级及以下机型数值逐位不变。

### 测试与工程
- v94 target 59→66（新增 rt_restore/exec_timeout/thermal_clusters/coldstart_release/gpu_devfreq_recovery/ebpf_cpu_mask/ml_pool_feature；diag 30→54、frame 65→74、coldstart 27→28）。
- 集成 26→27 场景（新增 S27_FgBgFgRepromote 前后台切换回归，10631→10640 点）。
- F-11 冷启动断言由"level=0 被调用"改为验证真实 restore 通道。

### 已知延后（如实记录）
- .rec 记录原始 cgroup membership 字段（持久化 schema 变更，留后续版本）。
- fg_package 仅本机 loopback 可读（未出网，评估后保留）。
- 能力探测微秒级写 root cgroup 改临时子 cgroup（风险极低，延后）。
- ML 反事实探索/per-app 模型等留 v4.2 路线图。

---

## v4.0.3 (2026-10-03) — ROUND-013 外部审查 25 项裁决修复

- **sched ABI（P0）**：新建统一头 `aatpe_sched_abi.h`——UAPI 优先、host/旧内核完整兜底 struct（补 sched_period；static_assert sizeof==56、util_min offset==48）；修正全部 flag 常量（KEEP_POLICY 0x08/KEEP_PARAMS 0x10/UTIL_CLAMP_MIN 0x20/MAX 0x40，旧代码两处错位）；aatpeUclampSetMin flags=0x38。**影响：旧版任务级 uclamp 因 util_min 偏移错位在真机从未真正生效，本版起正确生效。**
- **cgroup v2 线程迁移（P0）**：单 TID 迁移从 cgroup.procs 改为 cgroup.threads；三态 0/-1/-2，内核拒绝线程粒度（EOPNOTSUPP/EBUSY/EINVAL）缓存结论并降级 task-uclamp/schedtune/affinity，绝不回退 procs。
- **cgroup uclamp 层级（P0/P1）**：承认组级 floor——首次 boost 读存 prior 原值、多 boost 取 max+引用计数、归零写回 prior、defaulted 才写 0；state/cg_uclamp_active 标记；shutdown restoreAll；service.sh boot 残留清扫。
- **能力探测拆分**：task-uclamp 只由对自身真实 sched_setattr 探针决定（rc=0/E2BIG/EPERM 三态），不再由节点存在推导；capabilities 新增 task_uclamp/cgroup_uclamp_v1/v2/cgroup_threads_v2 四键。
- **v2 原始组解析**：支持纯 v2 `0::/path` 与 v1 controllers 边界匹配，hybrid 优先 v1；savedGroup 空则跳过还原。
- **recovery 状态闭环（P1）**：orig 接管前记一次、applied 每次 AATPE 成功 mutation 实时刷新，修复多次迁移后 live≠applied 导致卸载误判、线程滞留 AATPE 掩码。
- **卸载快照收紧（P1）**：卸载只还原 AATPE 实际持有的线程级状态（.rec 多维比对），不再覆盖他组件/用户/ROM 合法变化；动态节点从不快照回写。
- **测试**：caps 38、placement 105、recovery 26 新增 hermetic；QA 六阶段 6/6。

---

## v4.0.2 (2026-10-03) — 日用 APP 切换卡顿 / 小核下钉修复（ROUND-012）

- **前台应用禁止小核降钉**：decidePlacement 新增 FG_NO_DOWNPIN 门控——当前前台包的线程一律阻止向低容量池（MID/LITTLE）迁移；静默判据改用 eBPF 真实最近 sched_switch 时间戳（≥5s 无活动），只有真正静默的 S5 后台线程才允许下钉，杜绝"累计开关数<50 就误判静默"；前台包未知的检测空窗按保守方向（活跃非 S5 不下钉）；热紧急 EMERGENCY/低开销降温旁路不受限。
- **APP 切换边沿立即解绑**：新增 `releaseForegroundDownpins()`，前台切换事件触发时一次性还原新前台应用所有被本守护下钉线程的原始亲和掩码（ESRCH 安全跳过），不再等评分门控缓慢回迁；新增状态计数 fg_downpin_blocked、fg_switch_releases。
- **线程识别扩展**：Flutter 引擎 1.ui/*.ui/1.platform→UI、1.raster/*.raster/*.gpu→渲染；WebView/Chromium CrRenderer*→UI；1.io 不提升。
- **测试**：placement 87→93、thread_scan 23→31（含切换解绑端到端、真假静默、fg 未知 fail-closed、Flutter/WebView 分类、mtkPowerAIDL 防误判回归）。
- **说明**：模块从不把线程移入 background cpuset、不写 uclamp.max；不存在绑单核 CPU0 的逻辑，掩码始终为整池；本版修的是"整小核池下钉 + 切换不解绑"。

---

## v4.0.1 (2026-10-03) — 温控卡死自愈补丁（ROUND-011）

- **温控采集线程化**：sysfs 扫描移出主循环到独立采样线程（1s 节奏、异常自吞、stop 3s 有界 join 超时 detach），单点内核态读卡住不再冻结整个守护/心跳/WebUI。
- **采样新鲜度状态机**：成功扫描盖 CLOCK_BOOTTIME 戳，主循环每秒判龄，>10s 写 `thermal_freshness=stale=N` 与 `thermal_degraded=stale_read`，恢复自动清零（新增状态文件 thermal_freshness、thermal_stamp）。
- **温控选源非粘性**：每轮从有效 CPU zone 重选（mtktscpu 优先，恒定失效 zone 不得当选），thermal_source 写实际路径或空，失效恢复后自动切回。
- **watchdog D 状态升级**：连续 3 周期 SIGKILL 杀不掉 → `watchdog_runtime_state=UnkillableDstate`、`watchdog_unkillable=1`（pid/等待秒数/心跳年龄）、校验 pgid 后进程组 SIGKILL 补刀、每周期重试不静默放弃、恢复后清零。
- **doctor 新增 T 段**：心跳年龄>30s FAIL、unkillable 红色提示重启、thermal_freshness stale WARN、watchdog 卡死日志原文。
- **测试**：thermal 8 场景 22 断言（freshness 翻转/线程启停 detach/选源切换/轻载恒定失效与恢复）、watchdog 53（+4）。
- **诚实声明**：本版为代码核查确认的残留卡死路径防御性收口，未真机复现；v3.7.2 轻载恒定死值的更可能成因在 v4.0.0 已修。

---

## v4.0.0 (2026-10-03) — 全设备兼容架构 + 逐线程 uclamp 能效闭环（主版本进位）

ROUND-010：12 小时原子级重构（3h 四路调研 R1–R4 共 2492 行/211 来源、3h 八路解剖 D1–D9、6h 落地）。

- **全设备兼容**：aatpe_caps 启动能力探测状态机（内核/cgroup v1v2 挂载/uclamp 任务级与 cgroup/schedtune/PSI/GPU/loader/page size，capabilities+cap.json）；拓扑容量多级降级链（cpu_capacity→vendor→DT→频率×MIDR 微架构推断表 inferCapacityFromPart）；簇按容量序动态定级（全大核 8Gen3/9300/8Elite、同构 835 单簇、2 簇、hotplug offline 过滤）；16KB page size（max-page-size=16384 + customize.sh 自检）。
- **能效核心**：boost 后端升级为逐线程 uclamp.min 分档闭环（sched_setattr，BIG≈容量比/PRIME=1024 短窗，复位 UINT32_MAX）；机制降级链 任务级 uclamp→cgroup uclamp(v1/v2 自动路径与百分比换算)→schedtune→affinity，单一机制不叠加；EPERM/组消失重试降级；轻载让位 EAS、同容簇不跨簇、逐级不跳级。
- **正确性 P0**：veto 释放移入锁内 + mask/cpuset/uclamp 全对称还原（修线程永久滞留 top-app）；前台门控 fail-closed（堵 comm 伪造窗口）；promoted_tids（tid:starttime:orig_affinity:mechanism）+ service.sh boot 残留清扫（TID 复用校验）。
- **游戏帧**：SurfaceFlinger --latency 按 SurfaceView 层采样（frame_binder 直连+exec 兜底）、动态刷新率 deadline、帧数据不可用 fail-closed。
- **温控**：低负载恒定钳位第二判据、/proc/stat jiffies 回绕校正、byPolicy[-1] 不混组、电池缺失显 --。
- **ML**：指纹剔除容量/频率瞬时值（杜绝反复冷启动清零）、hotplug 池数增量迁移保权重、旧模型池数按 assignment 派生、损坏文件安全拒绝、nr_switches rate 归一。
- **加固**：setsid+oom_score_adj=-1000 防杀、7 个 map 有界驱逐、caps/diag 状态文件原子写 0600、service.sh watchdog 监督循环+rapid-crash 上限、watchdog.pid 原子写、uninstall 对称还原、sepolicy.rule 模板、doctor 新增 AVC(P)/能力(Q) 段并纠偏 M 段温度判据。
- **WebUI**：簇压力多行 tr 桥+base64 兜底、token 鉴权收回、严格 JSON、负温/0 值/NaN 修复、全设备动态簇渲染、系统能力段读 capabilities。
- **测试**：v94 52→54 目标（新 test_caps 29、test_diag 17）、placement 86、topology 78、v343_ml 61、frame 16；integration 166；QA 6/6。

---

## v3.7.2 (2026-10-03) — 温度判据科学性重构

- **废弃 v3.7.1 "CPU 温度不得高于电池+30°C"规则**：该规则有两个硬伤——电池温度节点本身可能是假值/缺省常量；CPU 结温真实可达 90–100°C，固定温差会误杀真实高负载。
- **新判据不依赖任何其他温度锚点**：
  1. **硬无效**：读数等于该传感器任意跳闸点、超出 15–115°C、读错。
  2. **负载相关性自证**：60s 滑动窗口内 CPU 聚合利用率（守护进程直接读 /proc/stat 计算）大幅摆动而某温度传感器读数纹丝不动，判定为厂商常量钳位并弃用；随负载同向变化的传感器才被选为 CPU 温度源。
  3. 补充 `/sys/class/hwmon` 候选源。
  4. 开机窗口未满（约 60s）温度显"校验中"而非发假值；所有候选无效则显 `--`。
- **hermetic 测试已验证**：恒 92°C 假传感器在负载 10→90% 摆动时被拒绝；随负载 50→75°C 变化的真实传感器入选；假电池温度不影响判定。
- WebUI"温度传感器"列表展示每颗传感器状态。

---

## v3.7.1 (2026-10-03) — 真机温度/GPU 两问题补丁

- **温度恒显 92°C**：上轮 trip_point_0 单点比对没拦住（真机该 zone 返回 92000 钳位常量但不等于 trip_point_0，且 MTK 热策略关闭时 zone 返回钳位值）。本轮：遍历全部 `trip_point_N` 比对、与电池温度交叉合理性（CPU 超电池 +30°C 判钳位）、双采样变化性判据（候选两次完全不变而皮肤/电池在变即丢弃）；全部 CPU zone 无效则显 `--` 绝不显示假值。新增 `thermal_zones` 状态文件（全部传感器 idx:type:temp:invalid）与 `thermal_source`，WebUI 新增可折叠"温度传感器"列表。说明：D1100 硬件只有一颗 CPU die 温度传感器，不存在逐核独立温度。
- **GPU 数据全空**：扩充 GPU 节点发现（devfreq 全条目含 fde00000.gpu 类命名、MTK GED hal gpu_utilization/gpu_freq、Mali mali0 节点、厂商格式解析），`gpu_load` 增 source 来源字段；PSI 零值前端正确显 0%（之前 0 被吞成 `--`）。
- **doctor 新增 M/N/O 段**：全部温控区双采样 + 钳位 WARN、GPU 节点逐项枚举、PSI 存在性。

---

## v3.7.0 (2026-10-02) — 卡顿根因综合诊断（新功能）

- **系统压力采集 aatpe_diag**：PSI cpu/io/mem 压力、GPU devfreq 利用率/频率、CPU/GPU 热限频状态、线程 runqueue 等待，2s 周期；状态文件 `pressure` / `gpu_load` / `throttle_state` / `rq_wait_top`。
- **调度 tick 根因分类器**：卡顿时先判根因——温控限频 / GPU 瓶颈 / IO 卡顿 / 内存回收四类**一票否决 CPU 盲目提升并释放已提升线程**（GPU 瓶颈时提 CPU 纯费电）；只有"单线程 CPU 瓶颈"（关键线程在小核排队、帧时超标的真实 CPU 病因）才走 ROUND-008 逐级提升；后台干扰只记录；2 周期防抖。
- **WebUI**：监控页新增"卡顿根因诊断"卡片（根因中文 + 证据 + GPU/PSI/限频状态）。
- 依据：`docs/research/ROUND-009-HOLISTIC-JANK-DIAGNOSIS.md`（AOSP/内核/Perfetto 15+ 来源）。
- **同包含 ROUND-009a 两修复**：①在线学习 CPU7 样本归因改用模型自身 `getCpuPoolAssignment`（池3 不再恒 0）；②温度读数与 `trip_point_0_temp` 比对，阈值常量 95°C 误读自动跳过。

真机验证方法：游戏卡顿时看 WebUI 根因卡片判定是 GPU/温控/IO/内存还是真 CPU 瓶颈；非 CPU 病因时确认 CPU 未被盲目提升、已提升线程被对称释放。

---

## v3.6.1 (2026-10-02) — 真机两 P0 修复

- **在线学习 CPU7 样本数恒 0**：采样归因函数 `getPoolForCpu(7)` 把 PRIME 折叠成 BIG 池2，而模型 `cpuToPool` 把 cpu7 映射到池3，导致 cpu7 样本全进池2、池3 永远 0。主训练循环与 hybrid tick 改用模型自身 `getCpuPoolAssignment` 归因（v343_ml 49 测试）。
- **温度恒显 95°C**：真机 MTK 某 CPU zone 的 temp 返回跳闸阈值常量 95000（95°C）被当实时温度。新增与 `trip_point_0_temp` 比对，阈值读数自动跳过换下一个真实 CPU zone，读不到显 `--`；前端移除全部温度硬编码兜底并显示温度来源 zone。

---

## v3.6.0 (2026-10-02) — 簇级聚合压力调度（次要版本进位）

根治真机"游戏时小核簇顶满、超大核全程闲置"。

- **盲区**：旧策略与内核 misfit 同一盲区——只看单线程利用率，多个 30–60% 中载线程谁都不触发提升，导致小核簇顶满、超大核全程闲置。
- **新策略 cluster-pressure**：
  1. 每 tick 计算各簇聚合利用率；小核簇聚合压力（active 75% / assist 80% / passive 85% 持续 dwell）即触发。
  2. **逐级提升不跳级**——先把最重前台关键线程提到大核簇（affinity 兜底 mask=bigPool 4–6、uclamp 分档、stune top-app）；大核簇也饱和且超大核有余量才溢 Top-1 到 PRIME。
  3. **对称回落**（簇聚合 <50% 持续 250ms 逐级降回）。
  4. STUNE 路径运行时核验 `/dev/cpuset/top-app/cpus` 含 PRIME，不含则自动降级 affinity/uclamp。
  5. 新增 `cluster_pressure` 状态文件逐簇压力可观测；EMERGENCY 压制 / 前台门禁 / 空载零提升照旧。
- **doctor**：新增 top-app 含超大核 FAIL 检查、机制节点前提、簇压力段、cpu7 isolated 检查。
- 依据：`docs/research/ROUND-008-CLUSTER-PRESSURE-ALGORITHM.md`（内核 EAS misfit / uclamp / AOSP / 同类工具 15+ 来源）。

真机验证方法：游戏中观察 `cluster_pressure` 逐簇压力、WebUI Per-CPU 不再退化为 4+4、`aatpe-doctor.sh` top-app 含超大核检查通过。

---

## v3.5.9 (2026-10-02)

Per-CPU 超大核误判根治 + mutex 自死锁 release blocker。

- **Per-CPU 超大核 CPU7 误入"未识别核心"**：守护端新增 `prime_pool` 扁平状态文件（两处拓扑写出点）；前端 Per-CPU 分组事实源改 `topoMap`（clusters→cpu_topology→prime_pool），未识别角标不再误显小核。
- **release blocker（教训）**：`primePoolLocked()` 拆锁修复了一个会导致真机启动挂死的非递归 mutex 自死锁——持锁路径再次取同一非递归锁即自死锁，v94 topology 测试 72 项须不再挂死方可发布。

---

## v3.5.8 (2026-10-02)

KSU WebView exec 桥多行输出根治（纯前端）。

- **根因**：该用户 KernelSU WebView 中守护进程 HTTP 聚合通道不可达（fetch loopback 被拦），所有数据走 KSU exec 桥；而 exec 对多行输出只回传最后一行——导致拓扑面板只显 PRIME 单核、日志只显一行、Per-CPU 退化为 4+4。守护进程检测本身一直健康。
- **修复**：多行状态文件（clusters/clusters_raw_debug/topology_diagnostics）exec 读取经 `tr` 单行化 + JS 按 0x1e 还原；加入 exec 降级聚合白名单；日志 tail 200 行同样处理；Per-CPU 严格按 cpu_topology 四池分组。

---

## v3.5.7 (2026-10-02)

拓扑多行状态改聚合通道，根治面板只显单核。

- **根因**：clusters/topology_diagnostics 多行文本经单文件通道（KSU exec cat 桥）只回传尾部最后一行，WebUI 拓扑面板因此只显示 PRIME 单核。
- **修复**：clusters 与 clusters_raw_debug 完整多行并入 `/state/all` 聚合快照（G1）；前端拓扑面板/Per-CPU 全部改读聚合通道，调试块标注数据来源（agg/http/exec）并展示 raw_debug 全文（G8）。

---

## v3.5.6 (2026-10-02)

在线学习权重表对外序列化补 PRIME 池。

- **修复在线学习权重表仍缺 CPU7**：`model.json` 序列化器硬编码三池（poolIdx>=3 跳过、无 PRIME 池名/CPU 列表）。已补 PRIME 池（cpu7 输出池3 权重），D1100 权重表现为 8 行、pool_count=4。
- 说明：v3.5.5 的模型内部映射修复已在守护进程内，本轮补齐对外数据序列化的遗漏。

---

## v3.5.5 (2026-10-02)

ML 目标宇宙漏 PRIME（本轮最深 bug）+ clusters 发布守卫。

- **ML 目标宇宙漏 PRIME**：PRIME cpu7 重分类后 cpuToPool=-1，ML 从不给超大核打分、权重表缺 CPU7 行。已映射 PRIME→池3、mlPoolCount=maxTier+1、cpu_topology.prime JSON 补全；旧权重按拓扑指纹自动冷启动。
- **clusters 发布守卫**：残缺拓扑（<2 簇/行格式非法）不再覆盖好文件，避免"只显示 1 核"；新增 `clusters_raw_debug` 原始对照。
- **WebUI**：拓扑面板加原始 clusters 逐行调试块；Per-CPU 利用率按新三簇分组（小核/中核/超大核不再合并）；温度标注"共享 die"；主动迁移次数开机显 0。
- 启动即写 total_decisions/total_migrations/prediction_proactive_migrations=0。

---

## v3.5.4 (2026-10-02)

线程追踪自适应预算 + jank 官方阈值闭环。

- **线程追踪自适应预算**：默认不再死锁 512，按上轮观测线程数 ×1.15 自适应（512–2048 区间）；config 显式 `MAX_TRACKED_THREADS` 优先。表满时前台游戏线程不再被跳过（优先淘汰 BACKGROUND 腾位）。
- **jank 官方阈值闭环**：连续 2 帧 p95>16ms 触发有界 boost 投票，提升最高权重前台关键线程；jank 消失即撤，EMERGENCY 压制，不全局抬 min_freq；新增 `jank_boost_vote` 状态文件。

---

## v3.5.3 (2026-10-02)

真机 doctor 数据驱动修复 + 标杆对标调参。

### PRIME 识别
- 独立 cpufreq policy + 容量严格更大即判 PRIME（修复天玑1100 两档 A78 max_freq 报成相同导致 cpu7 误标 BIG）。
- clusters 行格式锁死；MTK CPU 温控 zone 命名扩充（thermal_cpu 不再为空）。

### WebUI
- 拓扑面板容错 + 核数完整性告警（修复只显示一颗核）；温控按钮初始化重试 5 次常驻显示。
- PRIME 计数 "0" 不再显 "--"；新增 `GET /logs?tail=200` 日志端点，前端追加式 200 行持续滚动。
- 5 处 "?" 裸占位改 `[tid N]`。

### 线程名
- eBPF onFork 空名占位线程输出 `[tid N]`；进程名兜底补 UTF-8 修复。

### 模式状态机
- 用户意图模式与安全生效模式分离，崩溃保护窗口到期自动恢复用户选择（修复点激进被 rapid_crash_loop 永久切回）。

### 机制探测
- 启动后无条件首探 scheduler_capabilities（不再依赖 spillover 触发）。

### 标杆对标（ROUND-007 研究）
- 事件驱动即时 boost（触摸/冷启动 onInputEvent，40ms 节流、80ms 窗）。
- spillover 驻留 500/300/200→150/100/60ms（对齐 PELT 32ms）；阈值 90/75/60→85/75/65（EAS headroom）；不全局抬 min_freq。

### 主干
- total_decisions/total_migrations/version 状态文件写出；watchdog pid 原子写。

---

## v3.5.2 (2026-10-01)

全架构拓扑自动识别 + 全机型兼容。

### 拓扑识别全架构化重构
- cpufreq policy（affected_cpus）升为首要簇边界（硬件电压域真相），组内再按容量/频率细分。
- PRIME 判据支持无小核旗舰（2+6 骁龙8 Elite 顶簇 2 核判 PRIME）；policy 缺失回退纯比率。
- 新增 8+ SoC 合成矩阵（2+6 / 全大核 / 1+7 / 独立 policy lead 等）；诊断 dump 含 policy 证据；topology 测试 56→64。

### WebUI
- 拓扑面板渲染全部簇与"共 N 核/M 簇"徽章，读失败显示明确错误而非只渲染部分；日志面板 60vh/上滑暂停/200 行环形缓冲。

### 线程名根治补漏
- 空 tname（eBPF onFork 占位未 hydration）JSON 输出确定性占位 `[tid N]`；进程名 comm 兜底路径补 UTF-8 修复。

### 配置清理
- aatpe.conf 与 profiles 删除全部历史版本/修复标记注释（v66/fix33 等 31 处），注释规范化；校正 DECISION_INTERVAL(10-5000)/MAX_MIGRATIONS(1-1000) 注释漂移；KV 解析零变化。

### 文档
- 新增 docs/research/ROUND-006-DEEP-RESEARCH-v2.md（2024-2026 旗舰架构现状、逐子系统技术路线科学评估）。

---

## v3.5.1 (2026-10-01)

跨内核代际调度机制兼容 + 真机拓扑根治（天玑1100 实测驱动）。

### 真机严重问题修复（天玑1100 实测）
- **"激进"档形同虚设**：active 曾被错误映射为 assist。control_mode 现严格保留 passive/assist/active 原值，并分离 `system_state`/`control_mode_effective`。
- **WebUI JS ReferenceError**：`toggleThermalEnabled` 未挂 window 导致按钮失效，修复并加 onclick→window 导出静态自检。
- **新增 `GET /state/<name>` 单文件路由**（严格白名单 + token + 防路径穿越）：修复温控开关/拓扑/PRIME 计数全部 404 显 "--"、拓扑面板回退旧字段的问题。
- **拓扑误判根因根治**：部分核 capacity 读失败被默认值 380 污染。改为 capacity 双 sysfs 路径、证据层整层判废、禁用 scaling_cur_freq、启动 1/3/10/30s 退避重试、hotplug 重检；新增 `topology_diagnostics` 诊断转储与 `aatpe-cli topology` 子命令。
- 线程名 15 字节 UTF-8 截断显示"？"修复；日志面板多行/60vh/上滑暂停；KSU 打包目录第三份 HTML 副本陈旧同步（三份 md5 一致）。

### 跨内核代际调度机制统一层（新增 aatpe_placement_mech）
- 能力探测位图（uclamp / schedtune / cgroup uclamp / top-app cpuset）写入 `scheduler_capabilities`。MTK 4.14（天玑）走 top-app cpuset 入会 + EAS（对称恢复原 cpuset 组），4.19+/CAF 走 uclamp，GKI 走 cgroup，全缺才 affinity/nice。
- uclamp 权威探测增加 sched_getattr 回读 util，识别"返回 0 但静默忽略"的 policy-only(-3)/KEEP_POLICY 缺失(-2)，杜绝假阳性。
- spillover 候选叠加前台真源门禁防 comm 伪造；watchdog emergency 本地回退排除 PRIME 顶簇；温控频率单核读失败改 fail-neutral；修 spillover debounce 每轮被清零导致提升永不触发的状态机 bug。

### ML / 主干
- hybrid f[2] 训练/推理偏差修复并改为"切换速率"特征（累计计数饱和）；模型持久化 v3→v4，旧权重拒绝；联邦 PSK 不可用 fail-closed（不再静默降级 CRC-only）。
- `daemon_pid`/`daemon_starttime`/`http_port` 改原子写；`scheduler_capabilities`/`topology_diagnostics` 进 /state/all。

### 测试与工程
- Makefile 补 aatpe_placement_mech（干净构建链接断裂修复）；v94 52/52、integration 156/156（新增 S15 机制矩阵/S16 拓扑韧性/S17 状态契约/S18 线程 UTF-8）；QA 六阶段全 PASS；device 全功能脚本新增 T023d–g。
- 文档：新增 WORKFLOW_MEMORY.txt 与 docs/research/ 两份外部技术研究。

---

## v3.5.0 (2026-09-30)

通用多级 CPU 拓扑（新增 PRIME 超大核层）+ spillover 溢出调度 + 配置热重载 + WebUI 三端对齐。

### 调度 / 拓扑（核心）
- **通用多级 CPU 拓扑**：新增超大核 PRIME 层级与 `CpuCluster` 聚类；切簇改相对比率判据（容量比≥1.20 或频率比≥1.15），PRIME 需 ≥3 簇且最高簇 ≤2 核等软信号；1/2/3/4 簇全通用。12 款 SoC 合成矩阵验证（天玑1100 正确识别 4小/3大/1超大；845/835/天玑900 等不误造超大核）。capacity/频率缺失三级降级。
- **PRIME_POOL 手动覆盖键**：非法 cpulist 回退自动、去重、与大核重叠拒绝。
- **大核饱和→超大核溢出(spillover)调度**：关键线程 Top-N 提升、debounce+驻留滞回（升快降慢）、三档参数矩阵（省电 90%/正常 75%/激进 60% 阈值等）真实接入周期迁移回路；温控紧急态零提升并立即撤出；线程退出自动回收。
- **EAS 协作**：默认 uclamp 提示不长期钉核；uclamp 不支持的内核（如 MTK 4.14/天玑1100）自动降级为池级 affinity 落点并在撤出时恢复；uclamp 支持探测由一次性 latch 改 60s 重探。
- **评分科学性**：修复 UI 线程池偏好常量倒挂（超大核得分反低于中核）；容量归一接入实测标定并随拓扑代际重标；空闲核不再被罚分；漂移检测改分池隔离。
- **ML**：池维度按实际簇数初始化（hotplug 安全重 init）；hybrid 推理 4 个特征（priority/nice/thermal/迁移数）由硬编码改真值，训练/推理归一化契约对齐并加测试锁定；模型持久化版本升至 v3（旧分布权重拒绝加载、重新冷学习）。

### 接口 / 热重载
- 删除死配置键 `EBPF_CAPABILITY_DETECTION`。
- **配置重载回调注册机制**：eBPF 使能/ring 页数/poll 回退、HTTP 服务开关全部支持运行时热生效（含安全 unload/reload 与同值防抖）；`profiles/*.conf` 纳入文件监视，手改即时重载；损坏配置/profile 行不再崩溃或打回默认（保留运行值并告警）。
- 配置文件增加属主/可写权限纵深校验；全键 LIVE/BOOT 分类，新增 `/api/config_meta` 端点，BOOT 键注释明确"重启生效"。
- **控制模式三端对齐**：被动/辅助/激进用户三档 + emergency 仅内部；WebUI 补齐激进按钮与紧急覆盖横幅/拒切提示、模式与手感档位文案分区、温控总开关、日志等级补到 0-5、uptime 公共字段修正、删除死 POST 分支、修正多处范围注释。
- WebUI 拓扑面板四级展示（含超大核与溢出计数）、自定义 profile 名 XSS 转义加固、手机窄屏布局。
- 新增状态输出 `clusters`、`prime_spillover_count`、`prime_hosted_threads`；device 全功能脚本同步覆盖（三级拓扑可解析、无 PRIME 机型不误判、三档 set_mode 往返、emergency 被拒、config_meta 可达）。

### 健壮性
- 修复 eBPF 热重载与主循环 retry/ring 动态调整并发的 unload/load 竞态（生命周期锁，防 double-close/UAF）。
- 修复 spillover debounce 计数被每轮清零导致提升永不触发的状态机缺陷。
- 修复损坏 profile 行异常穿透崩溃；集成测试构建对象 mtime 缓存防陈旧二进制。

### 测试
v94 单测 52 目标全绿（拓扑 53、placement 38、ebpf 42、integrity 44 等）；集成场景 99→133 验证点（新增 reload hook 链/三级拓扑/三模式/PRIME_POOL 覆盖）；QA 六阶段（fuzz/ASan/UBSan/LSan/TSan/coverage/stress/restart×100）通过。

---

## v3.4.9 (2026-09-30)

"误伤型回退"修复轮：把上几轮为了 fail-closed 而误伤到健康设备的降级/拒绝路径改为条件化、可自愈。

1. **完整性校验瞬时 I/O 防抖**：读错误（EIO/EINTR/EBUSY/EAGAIN/ENOMEM/EACCES/EPERM）三态化为 Clean / 确定篡改 / 瞬时(Transient)，瞬时错误不再判篡改；确定篡改连续 3 次复核才进只读；FAILED→OK 对称解除只读（hmacClearReadonly + restore hook），无需重启自愈。
2. **eBPF 偏移**：已知内核族（4.14.x）tracefs 不可读时不再永久闩锁轮询，改为信任内置偏移保持事件驱动（TRUSTED_BUILTIN 软标记+告警）；读到但布局不符（MISMATCH）或异族仍保守降级。
3. **GPU/CPU boost 节点去单次探测熔断**：首探失败后 30s 节流重探至成功、连续 5 次写失败触发重探，消除首帧 SELinux 瞬态导致 boost 整会话静默灭活。
4. **CRASH_LOOP 安全模式条件自愈**：闩锁后观察 300s 无新崩溃自动恢复（最多 2 次自愈，第 3 次才永久闩到重启）；helper 目录审计收紧（.conf/.json 资产白名单永不删、子串匹配仅限点前缀敏感名）。
5. **bootloop 自禁状态机重写**（boot_id 区分开机轮次 + self_disabled_latch）：自禁后手动 enable 并完成一次健康开机即恢复，不再立即重新自禁；真 bootloop（连续 3 次不完整开机）仍自禁。
6. **配置解析**：schema 数字键容忍单位尾缀（500ms→500，LOG_WARN）；热加载单键笔误保留上一份运行值而非静默回落编译默认。
7. **WebUI**：SSE 共享桶 4→6、429 返回 sse_limit 结构化原因、重连 1s；只读 403 返回 read_only/reason，前端一次性 toast 说明只读原因与恢复方法。
8. **watchdog EMERGENCY 独立温度复核兜底**：daemon 温度标志卡死时，watchdog 自读 thermal_zone 连续 30s ≤85°C 即放行恢复；节点全读不到则保守保持 EMERGENCY。
9. **工程加固**：recovery 跨维记录 RMW 原子性（读-改-写收进锁、提交内核确保目录存在，消除并发丢维度）；测试构建对象缓存改为 mtime 判断（消除陈旧对象假绿/假红）。

测试：v94 52/52、integration 99/99、QA fuzz/ASan+UBSan+LSan/TSan/stress/restart(100x) 全 PASS、coverage 8/8。

---

## v3.4.8 (2026-09-30)

热修：修复 v3.4.7 升级后完整性永久 FAILED、fail-closed 门禁把 eBPF 挡在 POLLING 的回归。

- **根因**：完整性哈希链误含两项本不该被运行时校验的文件——安装期-only 的 `customize.sh`（活跃模块目录中从不存在）与从未发布的幽灵文件 `system/bin/aatpe-temp-probe.sh`。运行时 verify 把这两个"缺失"文件固化进基线哨兵，导致每次升级后完整性恒为 TAMPER。
- **修复**：
  - `integrity.cpp` 清单剔除上述 2 项；
  - 基线生成/校验改为显式模块根锚定（`--generate-manifest`/`--verify-integrity` 接受显式 root 与 `AATPE_MODULE_ROOT`，优先级 argv > env > 探测，非法 root 非零退出、不静默回退）；
  - 安装/服务脚本均传 `"$MODDIR"`，升级不再锚到旧树；
  - 锚定前关键文件缺失即拒绝生成基线。
- 测试：integrity 用例 33/33。

---

## v3.4.7 (2026-09-30)

穷尽式代码审查修复轮（12 组审查约 4.7 万行生产代码；去重后修复 1 Critical、22 High、46 Medium、80 Low）。

**Security / integrity**
- 完整性校验 fail-closed 真正落地：校验失败即撤销对系统节点的写权限并拒绝挂载 eBPF，不再以"降级可用"继续写。
- 联邦学习开关失效修复（默认 opt-in，开关可真正关停）。
- HMAC 关闭模式下零凭证铸令牌的封堵。

**Stability / correctness**
- 温控传感器全失效时进入 fail-safe，EMERGENCY 保持 120°C 硬天花板。
- offline CPU 误判修复；Mali GPU 频率单位 1000 倍错误修复；input 位图 arm64 栈越界修复。
- AB 分组稳定化；前台串无锁读 SIGSEGV 修复；日志旋转丢条修复。
- watchdog 状态机补边并支持显式停服标志（重启自动清除）；CLI 退出码修正。

**Build / release engineering**
- 干净树 `make all` 引导死锁修复（镜像缺失即 skip，不再硬失败）。
- 安装前补 ABI 白名单、Android SDK 下限、versionCode 降级拒绝、关键文件缺失 abort、升级保留用户配置。
- bootloop 自禁计数（连续失败自动 disable）+ 新增 boot-completed.sh。
- 全功能测试脚本回归源码树并由 Makefile 安装；修复永真断言与 `seq` 可移植性。
- 死代码清理约 325 行；归档历史测试约 11,412 行；可复现构建自动重生成 BUILD_MANIFEST。

**Verification:** v94 单测 52/52、集成测试 99/99、ASan+UBSan 通过、fuzz 约 8027 输入零崩溃、`make all` -Werror 零警告。

---

## v3.4.6 (current)

Security / integrity hardening + bug-fix rollup.

- **Integrity manifest trusted-install design.** `customize.sh` generates the
  SHA-256 manifest during install (trusted environment); `service.sh` verifies
  every shipped binary/config against it BEFORE forking the daemon. A mismatch
  logs `integrity_degraded` and starts the daemon in read-only mode rather than
  silently trusting on-disk bytes.
- **Placement / eBPF offset verification.** On-device tracepoint offsets are
  checked against `/sys/kernel/debug/tracing` format at load; mismatch degrades
  to polling rather than trusting hardcoded 4.14 offsets.
- **ADRC PID clamp / derivative kick.** Anti-windup clamp and bounded derivative
  prevent runaway affinity churn under sustained jank.
- **PID-identity guard.** Thread starttime is read from `/proc/<tid>/stat` to
  detect TID reuse across the `pid,comm` netlink uevent boundary.
- **setsockopt / fopen return-value checks.** The WebUI HTTP server and config
  logger now check every syscall result; silent failures no longer leave the
  socket/logger in an inconsistent state.
- **Control command dispatcher.** Whitelist `[a-z0-9_=.-]` validated before any
  HMAC check; signed replay uses HMAC-SHA256 with timestamp window.
- **Build system.** `system/etc/aatpe/` mirrors committed; `make install` ships
  eBPF `.o`/`.c`/`.h`; `release_ci.sh` derives SRC from script location.

### v3.4.6 穷尽审查修复轮（ROUND-001，2026-09-30，G12 文档/卫生类）

> 本小节为骨架，具体修复清单与最终测试数字由编排者在闭环后填入 `rounds/ROUND-001.md`。
> 本轮不改动版本号（版本号统一由 G10 在第二轮处理）。

- **死代码清理**：删除根目录注释墓碑 `aatpe_federated.cpp`、`aatpe_ml_hybrid.cpp`
  （代码早已拆分为 main/aggregate/exchange/security 与 main/train/predict/persist/alpha 等 TU）。
- **历史测试归档**：11 个根目录 `aatpe_test*.cpp`（v66–v71）+ 8 个 `tests/test_*.cpp`
  （v66–v74）+ v94 AB harness 统一移入 `tests/legacy/`，不参与 `make test`，保留考古用途；
  详见 `tests/legacy/README.md`。
- **文档不实声称订正**：`docs/UNIT_TEST_REPORT.md`、`docs/COMPREHENSIVE_TEST_REPORT.md`
  标注为 versionCode 3263 时代的历史快照，删除"0 失败/全绿"作为当前版本承诺的表述；
  当前真实测试状态以 `make test` 实测与 `rounds/ROUND-001.md` 为准。
- **已知遗留（待 G10/G11 闭环）**：6 个零引用孤儿头（cpu_topology.h / daemon_core.h /
  hmac_manager.h / placement_interface.h / event_coordinator.h / state_manager.h）仍列在
  `Makefile` 的 `AATPED_HDRS` 中，删除须与构建依赖列表同步，留待构建组统一处理。

## v3.4.4 — crash hardening

- **Static destruction-order fiasco fixed.** `~EbpfScheduler()` no longer calls
  `LOG_INFO()` after `g_logMutex` has been destroyed (see
  `ROOT_CAUSE_ANALYSIS_v3.4.4_crash.md`).
- **Control file truncated at startup.** `O_TRUNC` instead of `O_APPEND` so a
  stale `stop` from a SIGKILL'd prior run cannot exit the new daemon on its
  first loop.

## v3.4.2 — on-device migration fixes

- Emergency migrations bypass Safe Mode / thermal latch / hysteresis gates but
  still honor kernel threads, empty cpusets, and the rate limiter.
- eBPF ring 64→128 page upgrade vote also runs from the DEGRADED/HYBRID path.
- Thread-role confidence threshold lowered 50→30; foreground S1/S2/S3 bypass the
  low-confidence veto.
- Medium-event cooldown 500→1000 ms; `safe_mode_reason` string no longer
  truncated; `migrate_reject_*` state files actually written.

## v3.3.0 — compute-first placement

- "算力优先分配" / intelligent CPU core binding: big-pool placement favors the
  highest-capacity cluster for foreground critical threads; mid-pool (A78) score
  uplift and cross-pool penalty recalibration.

## v3.2.0

### Engineering / maintainability hardening (this round)

- **#9 Per-app config wired into the production chain.** `loadPerAppConfigs()`
  now runs at the end of `loadConfig()` (initial load + every hot-reload), and
  `checkAndApplyPerAppConfig(foregroundApp)` is called every decision cycle for
  the current foreground package. Previously the functions existed but were dead
  code, so `g_perAppConfigs` stayed empty and per-app overrides never applied.
- **#10 `loadThreadRules()` is idempotent.** Rules are now built into a local
  vector and `swap()`ed into `g_rules` once, instead of appending on every
  reload (which duplicated the rule set N times).
- **#11 TID-reuse protection extended to all auxiliary state tables.**
  `g_lastMigrationTime`, `g_migFailCooldown/Count/Cpu` and
  `g_unmigratableThreads` were keyed by the bare int tid. A recycled tid could
  inherit stale migration history/cooldown/"unmigratable" marks. A new
  `invalidateAuxThreadState(tid)` clears them on every (re)insertion in
  `scanThreads()` (both the REUSE path and the fresh-thread path).
- **#12 WebUI state-cache delete sync.** Added `stateCacheNoteRemove()` and
  unified the cache path validation. `stateCacheSnapshot()` now also self-heals
  by dropping entries whose backing file no longer exists on disk, so an
  unlink can never be served as stale data.
- **#13 Tightened runtime permissions.** Runtime data dirs (`state/`,
  `metrics/`, `decisions/`, `history/`, `thread_db/`, RUN_DIR) are 0700 and the
  generic `writeFile()`/`writeFileAtomic()` create files 0600 (they carry
  pid/tid/pname/tname/role/priority/CPU/placement info). Only the static webroot
  stays 0755.
- **#14 Daemon token fails closed.** The weak `rand_r(time^pid^const)` fallback
  when `/dev/urandom` fails was removed. Any CSPRNG failure now returns an empty
  token; `startHttpServer()` refuses to bind if it cannot mint a strong token.
- **#15 SOURCE_DATE_EPOCH actually fixes the build timestamp.** The Makefile now
  derives a UTC `Mmm dd yyyy` / `HH:MM:SS` from a non-zero
  `SOURCE_DATE_EPOCH` and injects it as `-D__DATE__=... -D__TIME__=...`, instead
  of only mapping build paths while claiming the timestamp was pinned.
- **#16 Version consolidation.** Stray release banners (`v3.1.0-thermal120`,
  module `v66`/`v94` banners) were updated to the single `v3.2.0` line.
  Meaningful fix/audit comments were kept.
- **#17 Historical fix series consolidated into this file.** No large-scale
  refactor; inline `fixNN` debug tags stay.

---

## Historical fix series (pre-v3.2.0, inline tags retained in source)

These are the fix numbers that appear as `// fixNN ...` comments across the
production tree. They are grouped by theme (not a detailed per-fix spec):

- **fix5, fix15–fix34** — early config validation, self-healing and parsing
  hardening (doubled-prefix healing, range clamping, HMAC active sync).
- **fix35–fix38** — migration cooldown logic (failure-counted cooldown,
  CPU-change early release, floor bypass, no permanent block).
- **fix39, fix40** — ML save-state diagnostic files; APK detection state files.
- **fix45** — mechanical split of the monolithic `aatped.cpp` into per-module
  translation units (this file layout).
- **fix46, fix47, fix49, fix50** — foreground detection source, control path,
  model reset.
- **fix52, fix54–fix59** — topology refresh on hotplug, TID-reuse guard,
  unmigratable-thread reprobe, decision-dir lazy cleanup.
- **fix65, fix66** — TID-reuse starttime guard; the v66 audit pass (thermal
  unit auto-detection, observability mutexes, hybrid learning).
- **fix71, fix72, fix73** — mtime-based hot-reload, control-command parsing,
  HMAC key persistence.
- **fix84–fix93** — proactive migration, auxiliary scans restored,
  self-pipe wakeup, eBPF degraded path.
- **fix94** — per-app config, learnable scoring weights, thread-role cleanups.
- **fix95** — federated learning TUs; decision-dir batching; per-thread score
  cache.
- **fix96** — adaptive decision interval; GPU-bottleneck detection.
- **fix97, fix98** — SSE/cache and observability refinements.
- **fix100** — eBPF scheduler init; real inotify hot-reload.
- **fix101** — HMAC key load-or-generate (0600).
- **fix103** — atomic flag for config inotify thread.
- **fix104** — auxiliary task scans; GPU bottleneck interval.
- **fix105** — (this round) engineering hardening #9–#17.
- **fix107** — ancillary stability follow-up.

If a specific `fixNN` needs its original design intent, grep the tree for it;
the inline comment is the authoritative note.

# ROUND-005 报告 B：工程严谨性最佳实践（root 调度守护 daemon）

> 对象：AATPE——arm64 NDK C++17 root 守护（Magisk/KernelSU），按线程角色做 CPU 簇放置。
> 检索日期：2026-10-01。每条附 URL 与关键原文摘录。
> 文末给"对 AATPE 的具体可执行建议"。

---

## 1. 语言选型诚实评估：C++ vs Rust（NDK root daemon）

### 现状事实
- Rust 官方 Tier：`aarch64-linux-android` 为一等支持 target，构建需 NDK；支持目标列表：
  > "Supported Android targets: aarch64-linux-android, armv7-linux-androideabi, x86_64-linux-android … riscv64 Tier-2."
  来源：https://doc.rust-lang.org/rustc/platform-support/android.html
- 工具链：`rustup target add aarch64-linux-android` + `cargo-ndk`（或手动 `.cargo/config` 指定 ar/linker = NDK llvm）。
  > "export ANDROID_NDK_HOME=…; [target.aarch64-linux-android] ar=…/llvm-ar linker=…/aarch64-linux-android29-clang."
  来源：https://users.rust-lang.org/t/target-older-android-ndk-with-armv7-linux-androideabi/134791 、https://www.davthecoder.com/blog/rust-android-ndk-toolchain-setup-cargo-ndk
- Magisk 本体已用 Rust（ONDK 自带 Rust 工具链）：
  > "The Magisk NDK package ONDK bundles a complete Rust toolchain."
  来源：https://topjohnwu.github.io/Magisk/build.html

### 工程判断（不迎合）
**结论：AATPE 现阶段保持 C++，不要整体重写为 Rust。** 理由：
1. **重写成本/风险不对称**：AATPE 已有 55 个 C++ TU、eBPF、thrift 风格状态机、v94 52 单测。Rust 重写意味着全部测试重写、FFI 边界重做，而收益主要在内存安全——但本守护是 **root 本地单写者、无网络面、解析的是 /proc 与 sysfs**，内存安全暴露面远小于网络服务。
2. **生态兼容**：Magisk/KernelSU 模块用 C++ NDK 二进制零摩擦；Rust 需额外维护 cargo-ndk 工具链、strip、libc 依赖（Android bionic），CI 复杂度上升。
3. **Rust 的真正价值点**：可在**新写的、高风险的解析模块**（如 eBPF 字节码解析、用户输入/JSON 解析、配置文件解析）用 Rust 写独立 `.a`/`.o` 通过 FFI 链接，或在下一代重写时引入。**不必为了安全把整个 daemon 推翻。**
4. **诚实结论**：Rust 适合从零新写、有网络/不可信输入面的守护；AATPE 是演进中的本地 root 工具，C++ + -Werror + 现有测试网已足够，重写的 ROI 为负。

---

## 2. /proc/<tid>/comm、stat、schedstat、cpufreq 正确读取姿势

### 关键事实
- **comm 截断**：`TASK_COMM_LEN = 16`（15 字符 + NUL）。
  > "max process name length … 15 characters, enforced by kernel TASK_COMM_LEN (16 bytes incl null)."
  来源：https://linuxvox.com/blog/what-is-the-maximum-allowed-limit-on-the-length-of-a-process-name/
  > "current->comm: char[TASK_COMM_LEN], max 15 chars + null."
  来源：https://embeddedpathashala.com/how-does-the-current-macro-access-execution-context/
- **comm 可被进程自己改**（`prctl(PR_SET_NAME)`），不可做身份/安全依据：
  > "Never trust current->comm for security decisions; it can be changed by the process itself."（同上）
- **瞬时退出 ESRCH**：内核文档明确——读 /proc/<pid> 期间进程可能已退出，操作返回 ESRCH：
  > "operations on these FDs usually fail with ESRCH."
  来源：https://origin.kernel.org/doc/html/v6.15/_sources/filesystems/proc.rst.txt
- **多字节 UTF-8**：comm 是 15 字节硬截断，UTF-8 中文会在多字节中间被切断 → 显示乱码/"？"。
- **内核线程命名**：`kworker/u:…`、`[nfsd]` 方括号风格，与用户态线程不同。

### 监控里出现"？"的原因与工业级做法
- "？"= comm 截断点落在 UTF-8 多字节中间，形成非法 UTF-8 序列，渲染为替换字符。
- 做法：
  1. comm 仅作**辅助提示**，身份判定用 `tid` + `/proc/<tid>/stat` 的 comm 字段 + `/proc/<tid>/cmdline` 组合，不依赖 comm 唯一性。
  2. 读 comm 后做 **UTF-8 合法性校验**，非法则替换为 `?`（而不是把半个字符当合法名）。
  3. 读 stat/comm 必须**容忍 ESRCH**：open 失败/读失败不视为错误，标记线程已退出并从表中删除。
  4. hotplug 竞态：读 `online` cpumask 与读线程负载之间 CPU 可能下线，用 `sched_getaffinity` 结果做最终仲裁，不缓存"某 CPU 一定在线"。

---

## 3. 跨厂商 CPU 拓扑发现权威方法

### 权威 sysfs 路径
- 掩码：`/sys/devices/system/cpu/possible|present|online`：
  > "online: cpus online and being scheduled; possible: allocated resources, can be brought online; present: identified as present."
  来源：https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-devices-system-cpu 、https://www.kernel.org/doc/html/v6.0/admin-guide/cputopology.html
- 拓扑：`/sys/devices/system/cpu/cpuX/topology/{physical_package_id,core_id,thread_siblings_lists,cluster_id}`。
  来源：https://www.kernel.org/doc/html/latest/translations/zh_CN/admin-guide/cputopology.html
- cpufreq policy：`cpuX/cpufreq/affected_cpus`（需协同调频的在线 CPU）、`related_cpus`（同 policy 全部）。
  来源：https://lenovopress.lenovo.com/lp0826.pdf
- **cpu_capacity 两种路径**：
  1. `/sys/devices/system/cpu/cpuN/cpu_capacity`（每 CPU，EAS 用）；
  2. `/sys/devices/system/cpu/cpu_capacity/cpuN_capacity`（部分 vendor 的汇总视图）。
  两路径都要试，取能读到的。

### 降级证据排序（容量缺失/SELinux 拒读时）
1. 先读 `cpuN/cpu_capacity`；
2. 失败 → 读 `cpu_capacity/cpuN_capacity`；
3. 失败 → 用 `cpuinfo_max_freq` 归一化估算相对容量（大核 freq 高 → 容量大）；
4. 再失败 → 用 `topology/cluster_id` 按簇分档（A78 簇 > A55 簇）；
5. 全失败 → 默认小/大两档保守模型，并记日志。

### 为什么不能用 scaling_cur_freq 做拓扑
- `scaling_cur_freq` 是**瞬时调频值**，随负载/温控/HMP 上下抖动，**不是**静态能力。用它判大核会把"此刻刚好高频的小核"误判为大核。拓扑必须用 `cpu_capacity`（静态/准静态）或 `topology/` 兄弟列表。

---

## 4. KernelSU/Magisk webroot 提供与缓存语义

### 事实
- KSU 官方：web 资源放模块根 `webroot/`，**KSU 安装时自动设置权限与 SELinux context**，不要手动改：
  > "Web 资源文件应放置在模块根目录的 webroot 子目录中。安装模块时 KernelSU 会自动设置 webroot 目录的权限和 SELinux context。"
  来源：https://kernelsu.org/zh_CN/guide/module-webui.html
- KSU vs Magisk 替换机制不同：
  > "The method for replacing or deleting files in KernelSU modules is completely different from Magisk."
  来源：https://kernelsu.org/guide/difference-with-magisk
- 模块生命周期里 webroot 是符号链接/挂载替换，升级后若浏览器/WebView 缓存了旧 index.html，页面不更新。

### 正确刷新/防缓存手段
1. **版本化 URL**：`index.html?v=3460`（带 VERSION），升级即变 URL 绕缓存。
2. **响应头**：webroot 提供方返回 `Cache-Control: no-cache, no-store, must-revalidate`（AATPE 内嵌 webui_http 应加此头）。
3. **资源指纹**：CSS/JS 文件名带内容 hash，升级即破缓存。
4. 不要在 webroot 自行 chmod/chcon（KSU 接管，自己改反而破坏）。

---

## 5. 功耗/流畅度科学：EAS、PELT、滞回、Android Vitals

- **PELT 负载追踪**：EWMA，每 1024µs 衰减，`y^32=0.5`（最近 32ms 占一半权重）。
  > "EWMA, each period (1024us) decayed such that y^32=0.5; the most recent 32ms contribute half."
  来源：https://www.kernel.org/doc/html/v6.19/_sources/scheduler/schedutil.rst.txt
- **WALT**（高通/MTK 4.14）：20ms 窗口，跨 CPU 同步采样任务 demand。
  来源：EAS Overview / WALT 文档。
- **工程经验值**：因为 PELT 32ms 才占一半权重，**任务放置决策应带 ~32~100ms 滞回/驻留时间**，避免短任务在大小核间反复迁移（迁移成本 > 收益）。这与 AATPE 已有的 dwell 配置化方向一致。
- **Android Vitals 流畅度**：系统以"UI 工作负载所需容量 vs 簇选择"判 jank——
  > "if scheduler judges UI workload needs 310 units of throughput, best to avoid jank is running big cluster at lowest freq (costs power)."
  来源：https://source.android.google.cn/docs/core/tests/debug/jank_capacity
  即：流畅 ≠ 盲目拉满大核，而是用足够容量的簇以较低频跑——AATPE 应"按角色匹配容量档"，而非一律送超大核。

---

## 6. root daemon 可靠性模式

### 原子状态写（已验证的工业模式）
> "Write to a temp file in the **same directory**, fsync it, fsync the **directory**, then rename over target. Cross-dir rename isn't atomic on POSIX."
> 来源：https://nalar.dev/write-files-atomically-on-linux-with-rename-and-fsync/ 、https://www.cclabs.dev/blog/linux-atomic-filesystems-security
> 关键坑："after rename(), the parent directory must also be fsync'd"。
> 来源：https://kernel-internals.org/io/war-stories/
- **AATPE 对应**：state/decision 文件必须 `open(tmp 同目录) → write → fsync(fd) → rename → fsync(dirfd)`，禁止直接 `O_TRUNC` 写正式文件（正是 v3.4.4 修过的 O_TRUNC fiasco）。

### 其他模式
- **单写者**：状态文件只由 daemon 主循环写，CLI 通过命令队列（已有 aatpe_command_queue）而非直接写文件。
- **锁序**：固定全局锁顺序（config → state → scoring），lockdep 思路，避免反向获取死锁。
- **配置 reload 回调隔离**：reload 只生成新 config 实例，交换指针，不持锁做慢解析。
- **崩溃看门狗**：外部 watchdog（已有 aatpe-watchdog）检测 daemon 心跳，僵死则重启；启动期 integrity verify 先于 daemon（fail-closed）。

---

## 7. 【对 AATPE 的具体可执行建议】
1. **语言**：保持 C++；仅对"配置解析 / eBPF 解析"等新高风险模块试点 Rust FFI，不整体重写。
2. **comm 读取**：读 `/proc/<tid>/comm` 后做 UTF-8 合法性校验（截断补 `?`），身份用 tid+stat+cmdline，comm 不做安全依据；所有 /proc 读容忍 ESRCH 并删表。
3. **拓扑发现**：优先 `cpuN/cpu_capacity`（回退 `cpu_capacity/cpuN_capacity` → max_freq 归一 → cluster_id 分档）；用 `possible/present/online` 掩码 + `topology/` 兄弟表，**严禁用 scaling_cur_freq 判大核**。
4. **webui 防缓存**：webroot/内嵌 HTTP 返回 `Cache-Control: no-store`，URL 带 `?v=VERSION`，资源名带内容 hash；不手动改 webroot 的 chmod/chcon。
5. **放置策略**：按角色匹配容量档（PELT 32ms 权重 → dwell 32~100ms 滞回），避免短任务跨核反复迁移；流畅度目标=够用容量簇低频跑，而非一律超大核。
6. **状态写**：所有状态文件走"同目录临时文件 + fsync(fd) + rename + fsync(dir)"原子路径；CLI 只经命令队列、不直写状态；锁序固定。

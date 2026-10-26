# AATPE 单一真源（Single Source of Truth）约定（v94 / 问题47）

> 问题47：源码、embedded bytecode、测试、文档四者此前没有完全形成单一真源，
> 容易出现"改了一处忘了另一处"。本文件明确**每类内容的唯一权威位置**与修改流程。
> 只做流程整理，不改代码逻辑。

## 1. 各类内容的权威真源

| 类别 | 真源（唯一可改处） | 生成物 / 镜像（**不可手改**） |
|------|--------------------|-------------------------------|
| eBPF 源码 | `ebpf/sched_events.bpf.c` | `ebpf/sched_events.bpf.o`（编译产物） |
| eBPF 内嵌字节码 | `ebpf/sched_events_embedded.h` | 由 `ebpf/gen_embedded_header.py` 从 `.bpf.o` 生成；**禁止手改** |
| 设备镜像 | — | `system/etc/aatpe/ebpf/` 下的 `.c`/`.h`/`gen_*.py` 是构建时从 `ebpf/` **同步**的镜像，不可手改 |
| 版本号 | `VERSION`（`VERSION_NAME` / `VERSION_CODE`，唯一人工修改处） | `aatpe-version.h`、`module.prop`、WebUI footer、shell banner 均为派生物；`check_version_consistency.sh` 在每次 build 后强制它们与 `VERSION` 一致，漂移即 FAIL |
| 运行时配置 | `aatpe.conf` | daemon 启动时加载为 `g_cfg`；设备侧 `system/etc/aatpe/aatpe.conf` 是其镜像 |
| 权威测试 | `tests/v94/` | `tests/aatpe_v74_test_suite.py` 是**历史遗留说明**，不是权威 |
| 文档 | `docs/` 与 `BUILD.md` | 代码内注释是考古保留（见 `HISTORY.md`），不替代文档 |

## 2. 修改 eBPF 的标准流程（强约束）

任何 eBPF 改动必须按顺序，否则字节码与源码漂移：

1. **改源码**：只编辑 `ebpf/sched_events.bpf.c`（offset/事件结构在这里定义）。
2. **重新编译 .o**：
   ```sh
   clang --target bpf -O2 -g -c ebpf/sched_events.bpf.c -o ebpf/sched_events.bpf.o
   ```
3. **重新生成 embedded header**：
   ```sh
   python3 ebpf/gen_embedded_header.py ebpf/sched_events.bpf.o ebpf/sched_events_embedded.h
   ```
4. **同步镜像**：把 `ebpf/` 下的 `.c`/`.h`/`gen_*.py` 拷到
   `system/etc/aatpe/ebpf/`（构建时由 `make install`/打包流程完成）。
5. **重新编译 daemon**：`make`（`aatped` 链接 `aatpe_ebpf.cpp`，它 `#include`
   了 embedded header）。
6. **跑测试 + 真机验证**：`make check-toolchain && make`，真机上确认
   `/dev/aatpe/state/ebpf_offset_verified=true` 且各事件计数正常。

## 3. 构建依赖关系（对应 Makefile 注释）

- `aatped` 的源文件列表见 `Makefile` `AATPED_SRCS`；它**强依赖**
  `ebpf/sched_events_embedded.h`（经 `aatpe_ebpf.cpp` include）。
- 因此：改 `sched_events.bpf.c` → 必须先重跑步骤 2/3 再 `make`，否则 daemon 链到
  的是旧字节码。
- `make install` 负责把 `aatpe.conf`、`profiles/`、`rules/`、`webui/` 拷进设备树；
  eBPF 镜像文件由模块打包（`customize.sh`/目录同步）处理。

## 4. 真源反模式（禁止）

- ❌ 直接改 `system/etc/aatpe/ebpf/sched_events_embedded.h`（它是镜像）。
- ❌ 在代码里硬编码版本字符串（一律用 `AATPE_VERSION`）。
- ❌ 把 `tests/aatpe_v74_test_suite.py` 当回归基线（它是历史遗留说明；新测试进
  `tests/v94/`）。
- ❌ 在设备侧直接编辑 `aatpe.conf` 当源（它是 `aatpe.conf` 根目录真源的镜像）。

---

## 5. v3.5.0 增量真源约束

- **新增源文件必须登记 Makefile**：`aatpe_placement_mech.{h,cpp}`（boost 机制层）已加入 `AATPED_SRCS`/`AATPED_HDRS`。新增任何 `.cpp` 若定义外部符号被 `placement.cpp`/`aatped_main.cpp` 调用，必须同步进这两个列表，否则干净 `make` 链接报 undefined symbol（v3.5.0 曾因漏登记而链接失败、旧二进制为陈旧产物）。
- **运行时状态文件不进包**：`state/clusters`、`scheduler_capabilities`、`topology_diagnostics`、`prime_spillover_count`、`prime_hosted_threads` 都是 daemon 运行期在 `/dev/aatpe/state/`（tmpfs）生成，**既不是源文件也不进模块包、不进完整性哈希链**。完整性链仍为 `check-manifest-layout.sh` 提取的 17 项。
- **`webui/index.html` 三副本**：源 = `webui/` 与 `webroot/`（两份手工真源）；`system/etc/aatpe/webui/index.html` 由 `make install` 生成，三副本由 `check_version_consistency.sh` 强制 byte-identical。

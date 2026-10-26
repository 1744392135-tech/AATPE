# tests/legacy/ — 历史测试快照（归档，不参与构建）

> 本目录由 G12 穷尽审查 ROUND-001 建立。这里的文件是 **v66–v94 时代的历史测试快照**，
> **不被任何 Makefile / build.sh 编译或运行**，不随 `make test` 跑。保留仅为考古用途。

## 为什么在这里
这些测试在 v94 之前编码了"当时的源码长相"（硬编码 eBPF 字节码偏移、map 数量、
文件 md5、版本字符串、旧的全局变量类型）。随着生产代码演进，它们：
- 不再被任何构建脚本引用（grep `Makefile`/`*.sh` 均为空）；
- 多数无法在当前源码树上独立链接（缺 stub、或生产类型已从 `int` 改为 `std::atomic`）；
- 其失败是"金值漂移"，**不是**生产回归。

权威测试面是 `tests/v94/` + `tests/integration/`，由 `make test` 驱动。
判定一个测试失败是否代表真 bug，见 `tests/README.md` §4。

## 目录结构
| 子目录 | 来源 | 文件数 | 行数 | 对应版本 |
|---|---|---:|---:|---|
| `root/` | 原源码根目录 `aatpe_test*.cpp` | 11 | 6286 | v66–v71 |
| `tests/` | 原 `tests/test_{v66,v68,v69,p0,p1,scoring,http_server,ml_funnel}.cpp` | 8 | 3830 | v66–v74 |
| `ab/` | 原 `tests/ab/aatpe_test_v94_comprehensive_ab.cpp` | 1 | 1296 | v94 A/B harness |

## 重要说明
- `ab/aatpe_test_v94_comprehensive_ab.cpp`：曾被旧 README 标为"权威"，但
  **在当前源码树上主机编译失败**（它声明 `int g_thermalCpu/Skin/Battery`，而
  `aatped_state.h` 现已改为 `std::atomic<int>`，结构漂移）。它未接入 `make test`，
  归档于此；如需恢复为 A/B 门禁，须先按当前生产类型修复其全局变量声明。
- `tests/aatpe_v74_test_suite.py` **未归档**：它仍被 `make test-legacy` 引用
  （Makefile 固定路径），保留在 `tests/`。其失败按 `[TEST_STALE_GOLD]` 分类。
- 不要在本目录新增测试。新测试一律进 `tests/v94/`（行为驱动，非金值断言）。

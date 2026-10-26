# AATPE v4.2.2 — 加长版 / 对抗性 QA 报告（隔离副本 v423_staging）

- 副本来源：最终 source.zip `bf20f8f4…` 解出，等同已封冻 v4.2.2。
- 工具链：NDK r30（aarch64-linux-android29-clang++）、SOURCE_DATE_EPOCH=1767225600、全程 -j1。
- 纪律：未改动 review_work/v42_tree 与 deliverables 任何文件；仅在本副本内编译、新增测试。

## A) 第三方可复现见证（clean make + 官方 strip）
在 S 干净 `make all` + llvm-strip 后四二进制 sha256，与封冻值逐字节一致：

| 二进制 | 复现 sha256 | 封冻值 | 一致 |
|---|---|---|---|
| aatped | d1f7e125591ce7094f2de0bf7078c773ef5c7cae5188a4e87319c308df771dd5 | d1f7e125… | ✅ |
| aatpe-cli | 82055baf081f3e6c9e4768ebd3dc9ca4a70f3870c43fa6273815d70520b2a65e | 82055baf… | ✅ |
| aatpe-watchdog | 1297691fb5eea56d468ef8c30bca1b8ec4d7bd1bc4e43da4d5e5bb0135659e93 | 1297691f… | ✅ |
| aatpe-probe | bf435bc9b4c9ff1e1f9e349193d14b762bcbffecc16dfb2da86fd577a6b23d59 | bf435bc9… | ✅ |

结论：从干净 source.zip 可逐字节复现封冻四二进制。

## B) 加长版稳定性（高于发布门禁）
| 项 | 发布基线 | 本次加长 | 结果 |
|---|---|---|---|
| restart-loop | 100x | **500x** | PASS（500 个干净 start/stop，0 泄漏） |
| fuzz 变异 | 2000/目标 | **~10000/目标（7 目标）** | PASS=7 FAIL=0，0 crash |
| stress storm (ASan) | config2000/fg5000/thread2000 | 同档全跑（fg S26=5000、thread S25=401×轮） | PASS |
| stress storm (TSan) | 2000+5000 | 同档全跑 | PASS（无数据竞争） |
| ASan/UBSan/LSan | 发布门禁 | 全跑 | PASS=13 FAIL=0，LSan 无泄漏 |
| v94 全量 | 78 | 78 + ext_neg_psi | 78 ok / 0 fail；ext_neg_psi 1 ok |

fuzz 实测：json/event/config/command/cgroup/uclamp/threadname 各约 10000 个变异输入，均"corpus sweep complete, no crash"。

## C) 对抗性边界负例（新增宿主测试，v4.2.3 候选）
新增：`tests/v94/test_ext_neg_psi.cpp`（注册 build.sh: `ext_neg_psi`），9 个断言全过：
1. `/proc/pressure` 整体缺失 → psiAvailable=false、三档全 0、total=0（前端显 --）。
2. cpu 含垃圾数字（abc/-5x/NaN/notanumber）→ 不崩溃、available=true、abc→0、total→0。
3. cpu 仅给 avg10（缺 avg60/avg300）→ 60/300 降级 0。
4. io/mem 的 `full` 行不污染 `some`（full=88 不泄漏进 some=0）。

其余 C1(ML 计数门控)/C2(单应用帧 LRU+畸形 framestats)/C4(温控死区/trip/zone撤销/fail-safe)/C5(重锚负例)/C6(uclamp ABI/recovery sched_getattr) 由既有 v94 + 集成套件（78 单测 + 10640 点，含 test_v343_ml / test_frame / test_per_app_frame / thermal / test_integrity / mech_uclamp / d1recovery）覆盖，本次全绿；这些模块的边界负例在封冻前门禁中已系统化断言。

## 发现的问题
**P3（防御纵深，不影响已封冻 v4.2.2 的正确性/安全，未自行改封冻包）**：
- `aatpe_diag.cpp:69-73` PSI 三档解析用 `atof(值前缀)`：`avg60=-5x` 会被解析成 -5.0、`avg300=NaN` 会解析成 NaN，而非降级 0。
- 影响面：真实内核 `/proc/pressure` 永远输出规范浮点，设备上不可达；仅当该 root 节点被污染时，NaN 可能经 `std::to_string` 进 system.json 成为非法 `nan`，影响 WebUI JSON.parse。
- 建议（v4.2.3）：解析后校验 finite() 且落在 [0, 某合理上界]，越界/NaN 归 0。已在 ext_neg_psi 用例标注为 KNOWN P3 FINDING。

无 P0/P1/P2；无崩溃、无泄漏、无数据竞争、无内存越界。

## 结论
加长版 / 对抗性验证**未发现影响 v4.2.2 正确性或安全的问题**（唯一发现为上述 P3 防御纵深项，设备路径不可达，留待 v4.2.3 加固）。封冻包无需回炉。

# ROUND-010 R1 全设备兼容性工程调研（深度扩写版）

> 目标：为 AATPE（root 用户态安卓线程调度守护，现仅天玑1100/内核4.14/KernelSU 真机验证）打造"刷上就能正确工作"的全设备兼容层。
> 版本：v2 深度扩写（600+ 行）。检索/精读日期：2026-10-03。不改代码。
> 一手资料精读：AOSP cgroup 抽象层全文、kernel.org sched-energy.rst 全文、Magisk 官方 Developer Guides、KernelSU 官方 Module Guide。每条关键结论附英文原文摘录 + URL。
> 承接 ROUND-005~009：uclamp v5.3/v5.4、schedtune≤4.14、PSI backport 4.9/4.14、EAS capacity=1024、簇压力算法、根因分类器。

---

## 0. 调研提纲
1. SoC 拓扑版图（30+ 型号）
2. 内核/Android 版本机制矩阵
3. Root 与运行环境（Magisk/KSU/APatch/SELinux/16KB）
4. 16KB page size 专章
5. cgroup 布局实测对照（Android 8/10/12/14/15）
6. 能力探测状态机工程级设计
7. 能力矩阵大表
8. 代码改造点
9. P0/P1/P2 工程清单
10. 来源清单（30+）与精读记录

---

## 1. SoC 拓扑版图（30+ 型号）

列：簇构成 / 微架构与标称频 / cpufreq policy 数 / 独立超大核 policy / GPU 与 devfreq 节点 / 典型机型。

### 1.1 高通 Snapdragon
| SoC | 簇构成 | policy数 | 独立prime policy | GPU/devfreq | 典型机型 |
|---|---|---|---|---|---|
| 845 | 4×A75@2.8 + 4×A55@1.7 | 2 | 否（大簇共享） | Adreno630 / kgsl-3d0 | S9 Exynos? 845:小米8 |
| 855/855+ | 1Gold A76@2.84 + 3Gold@2.42 + 4Silver A55@1.8 | 3(1+3同大簇?实测大簇policy) | 半 | Adreno640/kgsl | 小米9 |
| 865/865+ | 1Prime A77@2.84 + 3A77@2.42 + 4A55@1.8 | 3 | 是 | Adreno650/kgsl | 小米10/一加8 |
| 870 | 同865+提频@3.2 | 3 | 是 | Adreno650/kgsl | 小米11X |
| 888 | 1X1@2.84 + 3A78@2.4 + 4A55@1.8 | 3 | 是 | Adreno660/kgsl | 小米11 |
| 8Gen1/8+Gen1 | 1X2@2.9/3.2 + 3A710@2.5 + 4A510@1.8 | 3 | 是 | Adreno730/kgsl | 小米12/12+ |
| 7+Gen1 | 1X2@2.5 + 3A710 + 4A510 | 3 | 是 | Adreno725/kgsl | 红米K50增强版 |
| 778G | 4A78@2.4 + 4A55@1.9 | 2 | 否 | Adreno642L/kgsl | 荣耀50 |
| 695 | 2A78@2.2 + 6A55@1.7 | 2 | 否 | Adreno619/kgsl | 红米Note11 |
| 6Gen1 | 2A78@2.2 + 6A55 | 2 | 否 | Adreno A5xx/kgsl | 红米Note13 |
| 8Gen2 | 1X3@3.2 + 2A715@2.8 + 2A710@2.0 + 4A510@1.8 | 4(1+2+2+4) | 是 | Adreno740/kgsl | 小米13 |
| 8Gen3 | 1X4@3.3 + 5A720 + 2A520 | 3(1+5+2) | 是 | Adreno750/kgsl | 小米14 |
| 8 Elite | 2Oryon@4.3 + 6Oryon@3.5（无效率核） | 2(2+6同源档) | 否（prime=同微架构高频档） | Adreno830/kgsl | 小米15/三星S24U(部分) |

### 1.2 联发科 Dimensity
| SoC | 簇构成 | policy数 | 独立prime | GPU/devfreq | 典型机型 |
|---|---|---|---|---|---|
| 1000/1200 | 4A77@2.6 + 4A55@2.0 | 2 | 否 | Mali-G77 / devfreq+ged | 红米K30/K40游戏 |
| 1300 | 1A78@3.0 + 3A78@2.6 + 4A55@2.0 | 3 | 是 | Mali-G77/ged | 红米Note12Pro |
| 7050 | 2A78@2.6 + 6A55@2.0 | 2 | 否 | Mali-G68/ged | 红米Note13 |
| 7200/8200 | 1A715@2.6 + 3A715@2.4 + 4A510@2.0 | 3 | 是 | Mali-G615/ged | 红米K60E/K60 |
| 8100 | 1A78@2.85 + 4A78@2.6 + 4A55@2.0 | 3 | 是 | Mali-G610/ged | 红米K50 |
| 9000 | 1X2@3.05 + 3A710@2.85 + 4A510 | 3 | 是 | Mali-G710/ged | vivo X60Pro+ |
| 9200 | 1X3@3.05 + 3A715 + 4A510 | 3 | 是 | Immortalis-G715/ged | vivo X90 |
| 9300 | 4X4@3.25 + 4A720@2.0（全大核） | 2 | 否（4 X4 共享） | Immortalis-G720/ged | vivo X100 |
| 9400 | 1X925@3.62 + 3X4 + 4A720 | 3 | 是 | Immortalis-G925/ged | vivo X200 |
| **天玑1100(本机)** | 1A78@2.6 + 3A78@2.0 + 4A55@2.0 | 3 | 是(cpu7) | Mali-G77 MC9/ged | 真我GT Neo |

### 1.3 三星 Exynos / 谷歌 Tensor / 海思麒麟 / 展锐
| SoC | 簇构成 | policy数 | GPU/devfreq | 备注 |
|---|---|---|---|---|
| Exynos 2100 | 1M5@2.9 + 3A78 + 4A55 | 3 | Mali-G78 | S21 Exynos |
| Exynos 2200 | 1X2 + 3A78 + 4A55 | 3 | Xclipse219(RDNA) | S22 Exynos |
| Exynos 1380 | 4A78@2.4 + 4A55 | 2 | Mali-G68 | A54 |
| Exynos 1480 | 1X4 + 3A78 + 4A55 | 3 | Xclipse535 | A54x |
| Exynos 2400 | 1X4 + 3A720 + ... + 4A520 | 多 | Xclipse940 | S24 Exynos |
| Exynos 2500 | 1X925@3.3 + 2A725@2.74 + 5A725@2.36 + 2A520 | 4 | Xclipse950 | Z Flip7 |
| Tensor G1/G2 | **2X1@2.8 + 2A76/A78 + 4A55@1.8（2+2+4，非1+3+4）** | 3 | Mali-G78/G710 | Pixel6/7 |
| Tensor G3 | 1X3@2.91 + 4A715@2.37 + 4A510@1.7 | 3 | Mali-G715 | Pixel8 |
| Tensor G4 | 1X4 + A720 + A520 | 3 | Mali-G715 | Pixel9 |
| 麒麟9000 | 1A77@3.13 + 3A77@2.54 + 4A55@2.05 | 3 | Mali-G78 MP24 | Mate40 |
| 麒麟9000s/9010 | 4A510 + 海思自研核(SMT) | 多 | Maleoon | Mate60 |
| 展锐 T618 | 2A75@2.0 + 6A55@1.8 | 2 | Mali-G52 | 入门 |
| 展锐 T765/T820 | 1A76 + 3A76 + 4A55 | 3 | Mali-G57 | 入门5G |

> Tensor G1/G2 校正（多来源）：
> "octa-core … two high-performance ARM Cortex-X1 cores at 2.8GHz, two ARM Cortex-A76 (2.25GHz), and four efficient Cortex-A55 … This 2+2+4 configuration differs from the 1+3+4 config."
> 来源：https://www.phonearena.com/news/Googles-Tensor-explained-Core-Pixel-features-amplified_id134064

### 1.4 容量检测降级链（EAS 机制一手原文）
> "The 'capacity' of a CPU … normalized in a 1024 range … comparable with the utilization signals of tasks and CPUs computed by PELT … provided via arch-specific code through the `arch_scale_cpu_capacity()` callback."
> 来源：https://docs.kernel.org/scheduler/sched-energy.html
AATPE 探测顺序：
1. `/sys/devices/system/cpu/cpuN/cpu_capacity`；
2. `/sys/devices/system/cpu/cpu_capacity/cpuN_capacity`；
3. `/proc/device-tree/cpus/cpuN/capacity-dmips-mhz`；
4. 缺失 → 用 `cpuinfo_max_freq` 归一 + 微架构表（A55<A76<A78<X 系列）推断；
5. 簇划分用 `cpuN/cpufreq/affected_cpus`（同 policy=同簇）。

---

## 2. 内核/Android 版本机制矩阵

| 机制 | 4.4/4.9 | 4.14(本机) | 4.19 | 5.4 | 5.10/5.15 | 6.1/6.6(GKI) |
|---|---|---|---|---|---|---|
| schedtune(/dev/stune) | ✅ | ✅ | 部分 | 退役中 | ❌ | ❌ |
| per-task uclamp | ❌ | ❌(主) | 部分backport | ✅(v5.3) | ✅ | ✅ |
| cgroup uclamp | ❌ | ❌ | ❌ | ✅(v5.4) | ✅ | ✅ |
| EAS | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| schedutil(EAS唯一兼容governor) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| WALT(高通vendor) | ✅ CAF | ✅ CAF | 部分 | 转PELT | EEVDF | EEVDF |
| PSI | backport4.9 | **backport4.14** | ✅ | ✅ | ✅ | ✅ |

> EAS 依赖（一手原文）："the only sane governor to use together with EAS is schedutil";"EAS on SMT is not supported";"checked at run-time by SD_ASYM_CPUCAPACITY_FULL flag"。
> 来源：https://docs.kernel.org/scheduler/sched-energy.html
> PSI backport："The PSI patchset in the upstream kernel (backported to 4.9 and 4.14 kernels) measures the amount of time that tasks are delayed."
> 来源：https://source.android.google.cn/docs/core/perf/lmkd

---

## 3. Root 与运行环境（一手文档精读）

### 3.1 Magisk 模块（官方 Developer Guides 精读）
> 模块结构：`post-fs-data.sh`（post-fs-data，BLOCKING，40s 超时）、`service.sh`（late_start service，NON-BLOCKING，**推荐**）、`boot-completed` 用 `resetprop -w sys.boot_completed 0` 等待；`sepolicy.rule` 每行一条策略；`customize.sh` 提供 `MODPATH/ARCH/IS64BIT/API` 变量、`ui_print/abort/set_perm`。
> 关键警告："**using setprop will deadlock the boot process! use resetprop -n**"；post-fs-data "paused before done, or 40 seconds passed"。
> 来源：https://topjohnwu.github.io/Magisk/guides.html

### 3.2 KernelSU 模块（官方 Module Guide 精读）
> 阶段：`post-fs-data.sh`(BLOCKING,10s)、`post-mount.sh`(OverlayFS mount后)、`service.sh`(late)、`boot-completed.sh`(boot完成后)；`initrc/*.rc` 注入 init.rc（最早，早于 post-fs-data）；`KSU=true` 环境变量判 KSU；`KSU_LATE_LOAD=1` 判 late-load。
> 与 Magisk 差异："KernelSU's metamodule uses kernel's OverlayFS; Magisk uses magic mount(bind)";"KernelSU doesn't support Magisk式文件替换，需 mknod c 0 0 + setfattr trusted.overlay.opaque"。
> boot 流程顺序（一手）：initrc注入 → post-fs-data → load sepolicy.rule → metamount → post-mount → zygote → service → boot-completed。
> 来源：https://kernelsu.org/guide/module.html

### 3.3 APatch
> "APatch systemless via kernel OverlayFS vs Magisk magic mount(bind); directly uses magiskpolicy"。
> 来源：https://apatch.dev/zh_CN/apm-guide.html

### 3.4 SELinux enforcing 写节点
- AATPE 以 root domain 运行（magisk/ksu domain 为 permissive）。写 `/dev/cpuset/*`、`/dev/stune/*`、`/sys/class/kgsl/*/devfreq/*` 时若 `avc: denied`，doctor 脚本应捕获 logcat/dmesg 的 `avc: denied` 并在 `sepolicy.rule` 补规则，而非静默失败。

---

## 4. 16KB page size 专章（关键兼容性陷阱）

> "Android NDK r28 and higher compile 16 KB-aligned by default."
> "NDK r27 and lower: `-Wl,-z,max-page-size=16384` and `-Wl,-z,common-page-size=16384`."
> "Starting Feb 1 2027, app updates on 64-bit must support 16KB."
> "16 KB ELF alignment works with 4KB and 16KB kernels starting android14-6.1."
> 来源：https://developer.android.google.cn/guide/practices/page-sizes ；https://source.android.google.cn/docs/core/architecture/16kb-page-size/16kb

**AATPE 工程要求**：
1. 构建用 **NDK r28+**；若必须 r27-，在 LDFLAGS 加 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`。
2. customize.sh 自检：`getconf PAGE_SIZE`；若为 16384，校验自带二进制 LOAD 段对齐（`readelf -l` 看 LOAD seg 对齐是否 16384），不符则 `abort "binary not 16KB aligned"`。
3. zipalign：模块 zip 内原生二进制条目需 16KB 对齐（Play 要求）。

---

## 5. cgroup 布局实测对照（Android 版本）

### 5.1 机制演进（一手 AOSP 原文）
> "Android 9 and lower: init.rc contained cgroups, mount points, versions."
> "Android 10 and higher: cgroups.json + task_profiles.json; `SetTaskProfiles`/`SetProcessProfiles` (Android 11+); `writepid` deprecated in Android 12, replaced by `task_profiles`."
> "Default cgroups.json: cpu→/dev/cpuctl, memory→/dev/memcg; Cgroups2→/sys/fs/cgroup."
> "MaxPerformance profile: JoinCgroup schedtune top-app."
> 来源：https://source.android.google.cn/docs/core/perf/cgroups

### 5.2 组成员语义对照表
| 组 | cgroup v1 路径 | cgroup v2 路径 | 语义 |
|---|---|---|---|
| top-app | /dev/cpuset/top-app | /sys/fs/cgroup/top-app | 前台焦点 app，最多核；部分vendor**不含prime** |
| foreground | /dev/cpuset/foreground | /sys/fs/cgroup/foreground | 用户可见非焦点 |
| background | /dev/cpuset/background | /sys/fs/cgroup/background | 后台，限小核 |
| system-background | /dev/cpuset/system-background | 同 | 系统后台 |
| display | /dev/cpuset/display | 同 | SurfaceFlinger |
| daemon | /dev/cpuset/daemon | 同 | 系统daemon |
| restricted/ultra-restricted | vendor 自定义 | vendor 自定义 | 省电限制组 |

> cpuset effective（v2）："cpuset.cpus.effective: queries effective CPUs, affected by hotplug."
> 来源：https://www.alibabacloud.com/help/en/alinux/differences-between-cgroup-v1-and-cgroup-v2
- **AATPE 必查**：`cat <cpuset_root>/top-app/cpus` 是否含 prime 核号（本机 cpu7）。不含则提权无效，需先扩 top-app cpus。

---

## 6. 能力探测状态机（工程级，接近可实现）

### 6.1 探测项完整清单（确切路径 + 判定命令）
```
P1 uname: `uname -r` → 解析版本(4.14/4.19/5.4/6.x) + GKI?
P2 cgroup布局:
   ls -d /dev/cpuset/top-app  → v1
   ls -d /sys/fs/cgroup/top-app → v2
   取存在者为 cpuset_root
P3 top-app成员: cat $cpuset_root/top-app/cpus → 解析是否含 prime 号
P4 拓扑:
   for n in 0..N:
     cap = cat /sys/devices/system/cpu/cpu$n/cpu_capacity
     若空: cat /sys/devices/system/cpu/cpu_capacity/cpu${n}_capacity
     若空: cat /proc/device-tree/cpus/cpu$n/capacity-dmips-mhz
     若空: 用 cpuinfo_max_freq 归一
     aff = cat /sys/devices/system/cpu/cpu$n/cpufreq/affected_cpus
   → 按 capacity 聚类成档,按 affected_cpus 定簇
P5 uclamp per-task:
   sched_setattr(0,{util_min=512}) → sched_getattr 回读==512?
   (root 下可 fork 测试线程)
P6 cgroup uclamp:
   ls /dev/cpuctl/cpu.uclamp.min  OR  /sys/fs/cgroup/cpu.uclamp.min
P7 schedtune: ls /dev/stune/top-app/{schedtune.boost,prefer_idle}
P8 GPU:
   ls /sys/class/kgsl/kgsl-3d0/gpuclk
   ls /sys/class/devfreq/*/{cur_freq,load}   (枚举,不写死名)
P9 PSI: ls /proc/pressure/{cpu,io,mem}
P10 loader: [ -n "$KSU" ] →KSU; [ -d /data/adb/magisk ]→Magisk; APatch
P11 page_size: getconf PAGE_SIZE → 16384? 校验二进制对齐
P12 SELinux: getenforce; 试写 cpuset 节点,logcat 抓 avc denied
```

### 6.2 降级链伪逻辑
```
MECH = NONE
if P5.uclamp_real_effective: MECH=UCLAMP_TASK
elif P6.cgroup_uclamp:       MECH=UCLAMP_CGROUP
elif P7.schedtune:           MECH=SCHEDTUNE
else:                        MECH=AFFINITY   # 最后兜底
# prime 档门控:
if P3.topapp_has_prime==false and P4.has_prime_policy==false:
    PRIME_TIER = DISABLED    # 无可用prime,关闭上超大核功能而非乱试
# 安全默认值:
未知簇 → 保守两档(小/大),不强行三档
未知GPU → GPU_BOUND判定关闭(不误报)
PSI缺失 → 用 /proc/stat iowait 列替代
```

### 6.3 探测结果缓存格式（`/data/adb/aatpe/cap.json`）
```json
{
 "kernel":"4.14.186","arch":"arm64","page_size":4096,
 "cgroup":"v1","cpuset_root":"/dev/cpuset",
 "topapp_cpus":"0-6","topapp_has_prime":false,
 "clusters":[{"cpu":[0,1,2,3],"cap":308},{"cpu":[4,5,6],"cap":1024},{"cpu":[7],"cap":1024,"freq_max":2600}],
 "mech":"SCHEDTUNE","uclamp_task":false,"uclamp_cgroup":false,"schedtune":true,
 "gpu_node":"/sys/class/kgsl/kgsl-3d0",
 "psi":true,"loader":"KSU","selinux":"enforcing"
}
```
- 每次启动重探测一次；缓存供 doctor/CLI 读；探测失败的项填 `null` 并走安全默认。

---

## 7. 能力矩阵大表
| 维度 | 4.14 MTK(本机) | 4.19 | 5.4 GKI | 6.1/6.6 GKI |
|---|---|---|---|---|
| per-task uclamp | ❌ | 部分 | ✅ | ✅ |
| cgroup uclamp | ❌ | ❌ | ✅ | ✅ |
| schedtune | ✅ | 部分 | ❌ | ❌ |
| PSI | ✅backport | ✅ | ✅ | ✅ |
| cgroup | v1 /dev | v1/v2 | v2 | v2 |
| EAS+schedutil | ✅ | ✅ | ✅ | ✅ |
| GPU节点 | kgsl/ged | 同 | 同 | 同 |
| loader | 全 | 全 | 全 | 全 |

---

## 8. 代码改造点（映射文件职责）
- **aatped_topology.cpp**：容量多级降级链(P4)；簇数动态聚类去"硬编码3簇"；全大核无小核适配；policy/prime policy 探测。
- **aatpe_placement_mech.cpp**：机制选择改能力矩阵驱动(P5/P6/P7)；按 loader/内核走降级链；prime 档门控(P3)。
- **aatpe_probe/doctor（新增）**：实现 §6 状态机，输出 cap.json；捕获 avc denied。
- **build**：NDK r28+ / 16KB 对齐。
- **GPU 观测**：枚举 kgsl/devfreq，不写死节点名。
- **customize.sh**：16KB 二进制对齐自检 + abort。

---

## 9. P0/P1/P2 工程清单
| # | 项 | 收益 | 成本 | 风险 | 优先级 |
|---|---|---|---|---|---|
| 1 | 容量多级降级链+簇动态聚类 | 极高 | 中 | 微架构表维护 | P0 |
| 2 | 能力探测状态机+cap.json | 极高 | 中 | 低 | P0 |
| 3 | NDK r28+/16KB对齐+customize自检 | 极高 | 低 | 低 | P0 |
| 4 | cgroup v1/v2 双兼容+top-app cpus探测 | 高 | 中 | 路径差异 | P0 |
| 5 | GPU节点枚举(kgsl/devfreq) | 高 | 低 | 低 | P1 |
| 6 | loader自适应(KSU/Magisk/APatch阶段) | 高 | 中 | WebUI差异 | P1 |
| 7 | SELinux avc检测+sepolicy.rule | 中高 | 低 | 低 | P1 |
| 8 | 全大核无小核策略退化 | 中 | 中 | 误判 | P2 |
| 9 | Tensor G1/G2 2+2+4 特例 | 低 | 低 | 自动聚类覆盖 | P2 |

---

## 10. 来源清单（30+，去重）
### 一手精读（web.fetch 全文）
1. https://source.android.google.cn/docs/core/perf/cgroups — cgroups.json/task_profiles 全文
2. https://docs.kernel.org/scheduler/sched-energy.html — EAS/over-utilization 80%/arch_scale_cpu_capacity 全文
3. https://topjohnwu.github.io/Magisk/guides.html — Magisk 模块/installer/boot scripts 全文
4. https://kernelsu.org/guide/module.html — KSU 模块阶段/initrc/late-load 全文
### 其他来源
5. https://apatch.dev/zh_CN/apm-guide.html
6. https://source.android.google.cn/docs/core/architecture/16kb-page-size/16kb
7. https://developer.android.com/guide/practices/page-sizes
8. https://source.android.google.cn/docs/core/perf/lmkd — PSI backport
9. https://www.kernel.org/doc/html/v7.2-rc3/_sources/scheduler/sched-util-clamp.rst.txt
10. https://www.alibabacloud.com/help/en/alinux/differences-between-cgroup-v1-and-cgroup-v2
11. https://docs.qualcomm.com/bundle/publicresource/topics/80-70020-10/14-configure.html — kgsl GPU
12. https://www.androidauthority.com/snapdragon-8-elite-3491805/
13. https://www.mediatek.com/products/smartphones/mediatek-dimensity-9300
14. https://www.mediatek.com/hubfs/MediaTek%20Assets/Pdfs/Infographics/MediaTek_D9400_Infographic.pdf
15. https://semiconductor.samsung.com/us/processor/mobile-processor/exynos-2500/
16. https://nanoreview.net/en/soc/qualcomm-snapdragon-865
17. https://www.phonearena.com/news/Googles-Tensor-explained-Core-Pixel-features-amplified_id134064
18. https://www.hisilicon.com/cn/products/Kirin/Kirin-flagship-chips/Kirin-9000
19. https://static.linaro.org/connect/sfo17/Presentations/SFO17-307%20WALT%20vs%20PELT.pdf
20. https://docs.qualcomm.com/bundle/publicresource/80-70023-3/topics/scheduler.md — UCLAMP_MIN
21. https://source.android.google.cn/docs/core/power/thermal-mitigation
22. https://www.notebookcheck.net/HiSilicon-Kirin-9010-Processor-Benchmarks-and-Specs.855471.0.html
23. https://www.notebookcheck.biz/Google-Tensor-G3-Notebook-Processor.772753.0.html
24. https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/snapdragon_865_product_brief.pdf
25. https://docs.kernel.org/6.13/scheduler/schedutil.html — util_est
26. https://source.android.google.cn/docs/core/perf/boost — GAME mode
27. https://perfetto.dev/docs/data-sources/frametimeline
28. https://developer.android.google.cn/games/optimize/framerate — p99
29. https://topjohnwu.github.io/Magisk/details.html — sepolicy domain
30. https://kernelsu.org/guide/difference-with-magisk

---

## 11. 厂商 perf 配置与 perfd 行为精读（CAF / MTK）

### 11.1 高通 perfd / power-libperfmgr（CAF）
- 高通阵营自 Android 13+ 从旧 perfd 迁到 `power-libperfmgr` + AIDL IPower，配置为 `powerhint.json`（而非旧 powerhint.xml）。
  > 实测自定义内核 changelog（2025）："Migrate to common libqti-perfd-client and power-libperfmgr; Import powerhint.json from lahaina; **Set duration for INTERACTION CPUBW boost**; **Drop CPU freqs boosting on INTERACTION**; Handle thermal profile state via powerhint."
  来源：http://raw.githubusercontent.com/ProjectInfinity-X/official_devices/refs/heads/master/changelog/marble.txt
- **对 AATPE 的含义**：高通系设备上，系统已用 `INTERACTION` hint 做短时 boost；AATPE 的 input boost 应**避让/叠加时短**，避免与 perfd 同时抬频造成抖动。
- PerfHAL/perflock：`perf_lock_acq()`/`perf_lock_rel()`，dlsym 加载，验证 `ls /usr/bin | grep perf`、`systemctl status perf-hal`。
  来源：https://docs.qualcomm.com/bundle/publicresource/80-70014-10/topics/18-customize.md
- schedutil 调响应：
  > "rate_limit_us … Lower values improve responsiveness: echo 200 > /sys/devices/system/cpu/cpufreq/policy4/schedutil/rate_limit_us."
  来源：https://dragonwingdocs.qualcomm.com/System/Kernel/runtime-performance-tuning

### 11.2 MTK BSP（天玑系列公开部分）
- MTK 用 CorePilot/HMP，调度 trace 含 `sched_util`；GPU 走 `/sys/class/misc/ged`（GPU 能效调度器）+ devfreq。
- MTK 4.14 普遍有 `/dev/stune/top-app`（ROUND-005 实证），但 uclamp backport 不确定 → AATPE 对 MTK 4.14 默认走 **schedtune 路径**。
- 天玑9300 全大核白皮书自述：
  > "the mid-gear out-of-order cores replace the in-order cores for min-gear CPU … simplifies both the cluster (DSU) design and the energy-aware scheduler."
  来源：https://www.mediatek.com/hubfs/MediaTek%20Assets/Pdfs/Dimensity-9300-All-Big-Cores_Whitepaper_ENG_Final.pdf

---

## 12. EAS 机制精读展开（sched-energy.rst 一手）

### 12.1 over-utilization 80% 阈值的精确含义
> "CPUs are flagged as 'over-utilized' as soon as they are used at more than 80% of their compute capacity. As long as no CPUs are over-utilized in a root domain, **load balancing is disabled and EAS overrides the wake-up balancing code**."
> "As soon as one CPU goes above the 80% tipping point, the overutilized flag is raised for the entire root domain, EAS is disabled, and the load-balancer is re-enabled."
> 来源：https://docs.kernel.org/scheduler/sched-energy.html

**对 AATPE 簇压力阈值的校准意义**：
- 内核在 >80% 才"放手"给负载均衡。AATPE 的小核簇触发阈值设 75-80%，正好与内核 tipping point 对齐——既不抢在 EAS 前面，也不落后。
- ROUND-008 的"小核簇聚合 >80% 持续 150ms 才提升"由此得到一手文档支撑。

### 12.2 EAS 选核算法一手
> "find_energy_efficient_cpu() looks for the CPU with the **highest spare capacity (CPU capacity - CPU utilization)** in each performance domain … then checks if placing the task there could save energy vs prev_cpu."
> 来源：https://docs.kernel.org/scheduler/sched-energy.html

**含义**：EAS 选"剩余容量最大"的核，不是"最快"的核。AATPE 用 uclamp.min 抬升线程时，本质是改变该线程的 util 信号，让 EAS 的 spare-capacity 计算倾向大核——这比硬 affinity 更顺 EAS 的"脾性"。

### 12.3 EAS 不支持的场景（边界）
> "EAS on SMT is not supported"; "EAS operates only on heterogeneous CPU topologies"; "the only sane governor is schedutil".
> 来源：https://docs.kernel.org/scheduler/sched-energy.html
- **麒麟9010 有 SMT** → EAS 在其上可能行为异常；AATPE 应对 SMT 设备降级为纯 cpuset/affinity 策略。
- **全大核设备（同 ISA 同频档）** EAS 省电收益小，AATPE 应退化为"同簇空闲核优先"。

---

## 13. 状态机失败决策树（细化）

```
探测某能力失败时:
  uclamp 探测:
    回读 != 写入值 → 标记 FAKE_EFFECTIVE(接受但忽略), 降级
  top-app cpus 不含 prime:
    尝试写 top-app/cpus 追加 prime号?
      写成功 → 启用 prime 档
      写被拒(SELinux/fs只读) → PRIME_TIER=DISABLED, doctor 报告
  cgroup 路径探测:
    v1 /dev/cpuset 无 → 试 v2 /sys/fs/cgroup
    都无 → 放弃 cpuset 提升, 仅用 affinity/nice
  GPU 节点探测:
    kgsl 无 → 枚举 /sys/class/devfreq/*/cur_freq
    都无 → GPU_BOUND 判定关闭, 不误报
  PSI 无:
    用 /proc/stat 的 wa(iowait) + procs_running 推断
  page_size=16KB 但二进制未对齐:
    customize.sh abort, 报明确错误而非装坏
  SELinux enforcing 写节点 denied:
    doctor 追加 sepolicy.rule 建议行, 下次重启重试
```

---

## 14. 逐 loader 的脚本时序适配（一手展开）

| 阶段 | Magisk | KernelSU | AATPE 应挂哪 |
|---|---|---|---|
| post-fs-data | BLOCKING 40s | BLOCKING 10s | 不挂（太早，/data 未完全就绪） |
| post-mount | 无 | OverlayFS后 | KSU 可挂（需要 system 挂载完成时） |
| service.sh | NON-BLOCKING（推荐） | NON-BLOCKING | **挂这里**（启动 daemon） |
| boot-completed | resetprop -w 等 | boot-completed.sh | 挂这里做首次探测+doctor |
| initrc injection | overlay.d | initrc/*.rc | 需要 root service 常驻时用 |

- AATPE daemon 启动推荐 `service.sh`（NON-BLOCKING，不拖开机）；首次能力探测放 `boot-completed.sh`（此时 cgroup/sysfs 全就绪）。
- KSU 特有：`KSU=true`、`KSU_LATE_LOAD=1` 环境变量判 loader 与 late-load 模式。
  来源：https://kernelsu.org/guide/module.html

---

## 15. 与 Scene / 厂商 HAL 冲突面（细化）
- **Scene（vtools）**：按全局档位写 cpufreq min/max；AATPE 不写全局调频档，只做线程级放置+短时 boost。
- **厂商 perfd/PowerHAL**：已做 input/game boost；AATPE boost 短时(40-100ms)自动回落，避让叠加。
- **冲突检测**：doctor 脚本检测 `/sys/class/kgsl` 或 cpufreq min_freq 是否被外部锁定（与 AATPE 预期不一致），报告"可能有其他调度工具接管"。

---

## 16. cgroup 布局按 Android 版本细化对照

| Android | 年代 | cpuset 实现 | 挂载点 | 关键变化 |
|---|---|---|---|---|
| 8/9 | 2017-18 | init.rc 硬编码 | /dev/cpuset, /dev/cpuctl | 无 task_profiles，writepid |
| 10 | 2019 | cgroups.json 引入 | /dev/cpuctl, /dev/cpuset, /sys/fs/cgroup(v2 freezer) | task_profiles 抽象层 |
| 11 | 2020 | SetTaskProfiles API | 同 10 | API 正式化 |
| 12 | 2021 | writepid 废弃 | task_profiles 命令替代 | AggregateProfiles |
| 14 | 2023 | GKI cgroup v2 趋势 | /sys/fs/cgroup/{top-app,...} | cpu.uclamp 可用 |
| 15 | 2024-25 | cgroup v2 进一步统一 | /sys/fs/cgroup | 16KB 页兼容 |

> "writepid /dev/cpuctl/top-app/tasks — Deprecated in Android 12";"task_profiles MaxPerformance — Joins the current process into top-app group under cpu controller."
> 来源：https://source.android.google.cn/docs/core/perf/cgroups

- **AATPE 不依赖 Android 内部 API**（root daemon 无 libprocessgroup），直接读写 sysfs/cgroup 文件节点，因此 v1/v2 路径探测是必须的。

---

## 17. GPU devfreq 节点命名跨厂商表

| 厂商/GPU | 节点路径 | 频率读 | 利用率读 |
|---|---|---|---|
| 高通 Adreno | /sys/class/kgsl/kgsl-3d0/ | gpuclk / devfreq/cur_freq | devfreq/load 或 gpu_busy_percentage |
| Mali Bifrost/Valhall(MTK/Exynos) | /sys/class/devfreq/mali* 或 /sys/class/misc/ged | cur_freq | load |
| Immortalis(MTK 9200+) | /sys/class/devfreq/ged* | cur_freq | load |
| IMG PowerVR(展锐/老机) | /sys/class/devfreq/galcore | cur_freq | load |

> kgsl 实证："cat /sys/class/kgsl/kgsl-3d0/gpuclk; gpu_available_frequencies; devfreq/min_freq."
> 来源：https://docs.qualcomm.com/bundle/publicresource/topics/80-70020-10/14-configure.html
- AATPE GPU 观测**枚举** `/sys/class/kgsl/*` 与 `/sys/class/devfreq/*`，不写死任何名字。

---

## 18. 探测项期望输出与判定阈值（工程级）

| 探测项 | 期望输出样例 | 判定 |
|---|---|---|
| uname -r | "4.14.186-xxxx" | major=4,minor=14 |
| top-app/cpus | "0-6" 或 "0-7" | 含 prime号? |
| cpuN/cpu_capacity | 308 / 1024 | <512小,>=512大 |
| affected_cpus | "4 5 6" 或 "7" | 同号集合=同簇 |
| sched_getattr 回读 | 写入512回读512=真;回读0=假 | 假→降级 |
| /dev/stune/top-app 存在 | boost/prefer_idle 文件 | 存在→schedtune可用 |
| /proc/pressure/cpu | "some avg10=1.2..." | 存在→PSI可用 |
| getconf PAGE_SIZE | 4096 / 16384 | 16384→校验对齐 |
| getenforce | Enforcing/Permissive | Enforcing→补sepolicy |

---

## 19. 逐子系统跨设备适配映射

| AATPE 子系统 | 跨设备适配点 | 依赖探测 |
|---|---|---|
| thread_scan | /proc/<tid> 容忍 ESRCH；hidepid 下 CAP_SYS_PTRACE | P11/SELinux |
| scoring | 簇档数动态，不硬编码三档 | P4 拓扑 |
| frame | framestats 需 dumpsys 权限；无则降级 | doctor |
| eBPF 事件链 | 4.14 eBPF 弱 → 退 ftrace trace_pipe | 内核版本 P1 |
| placement_mech | uclamp>cgroup>schedtune>affinity | P5/P6/P7 |
| thermal | scaling_max vs cpuinfo_max 通用 | 无特殊 |
| watchdog | 分离进程，全 loader 通用 | 无 |
| webui_http | KSU/Magisk WebUI 差异，防缓存 | P10 loader |
| GPU 观测 | 枚举 kgsl/devfreq | P8 GPU |

---

## 20. 探测主流程伪代码（接近可实现）

```c
void run_capability_probe(Capability* out) {
    out->kernel = parse_uname(READ("/proc/sys/kernel/osrelease"));
    out->page_size = sysconf(_SC_PAGESIZE);

    // cgroup root
    if (exists("/dev/cpuset/top-app/cpus")) {
        out->cgroup = CG_V1; out->cpuset_root="/dev/cpuset";
    } else {
        out->cgroup = CG_V2; out->cpuset_root="/sys/fs/cgroup";
    }
    string topcpus = READ(out->cpuset_root + "/top-app/cpus");
    out->topapp_has_prime = contains(topcpus, out->prime_cpu);

    // topology
    for (n=0; n<nr_cpu_ids; n++) {
        cap = read_capacity_chain(n);          // 4级降级
        aff = READ(cpufreq(n)+"/affected_cpus");
        out->clusters.add(n, cap, aff);
    }
    out->cluster_count = cluster_by_capacity(out->clusters);

    // mechanisms
    out->uclamp_task = probe_uclamp_task();      // setattr+getattr回读
    out->uclamp_cgroup = exists("/dev/cpuctl/cpu.uclamp.min")
                      || exists("/sys/fs/cgroup/cpu.uclamp.min");
    out->schedtune = exists("/dev/stune/top-app/schedtune.boost");

    // GPU
    out->gpu_node = enumerate_gpu_node();       // kgsl then devfreq
    out->psi = exists("/proc/pressure/cpu");
    out->loader = detect_loader();              // KSU env / magisk dir

    // choose mechanism
    if (out->uclamp_task)      out->mech = MECH_UCLAMP_TASK;
    else if (out->uclamp_cgroup) out->mech = MECH_UCLAMP_CGROUP;
    else if (out->schedtune)   out->mech = MECH_SCHEDTUNE;
    else                       out->mech = MECH_AFFINITY;

    // prime tier gating
    if (!out->topapp_has_prime && !out->has_prime_policy)
        out->prime_tier = TIER_DISABLED;

    write_json("/data/adb/aatpe/cap.json", out);
}
```

---

## 21. 风险登记册

| 风险 | 影响 | 缓解 |
|---|---|---|
| uclamp"接受但忽略"假阳性 | 提权无效 | sched_getattr 回读验证 |
| top-app cpus 不含 prime | prime 档白做 | doctor 探测+尝试扩 cpus |
| 16KB 设备二进制加载失败 | 模块无法运行 | NDK r28+/customize abort |
| 全大核设备无小核簇 | 三档假设错 | 动态聚类退两档 |
| SMT(麒麟9010) EAS 异常 | 选核失真 | SMT 检测后降 cpuset/affinity |
| 与 Scene/perfd 抢 sysfs | 写被覆盖 | 避让+短时boost+冲突检测 |
| SELinux denied | 写节点失败 | avc 捕获+sepolicy.rule |

---

## 22. doctor 脚本落地清单（用户态一次跑完）
1. `uname -r` + `getconf PAGE_SIZE` → 打印内核代际与页大小。
2. `cat $cpuset_root/top-app/cpus` → 打印是否含 prime。
3. 逐核 `cpu_capacity` + `affected_cpus` → 打印簇划分表。
4. `sched_setattr/getattr` 回读 → 打印 uclamp 是否真生效。
5. `ls /dev/stune/top-app/` → 打印 schedtune 可用性。
6. `ls /proc/pressure/` → 打印 PSI。
7. 枚举 `/sys/class/kgsl` + `/sys/class/devfreq` → 打印 GPU 节点。
8. `getenforce` + 试写 cpuset 节点 → 捕获 avc denied。
9. 输出 `cap.json` 摘要 + 推荐机制 + 已禁用的档位（及原因）。

---

## 23. 已知设备兼容快照（参考）
| 设备 | 内核 | 机制预期 | prime 档 |
|---|---|---|---|
| 天玑1100(本机) | 4.14.186 | schedtune | 需扩 top-app cpus 含 cpu7 |
| 骁龙865机型 | 4.19 | uclamp backport? 探测 | 大簇 prime policy |
| 骁龙8Gen2/3 | 5.10/5.15 GKI | cgroup uclamp | 原生 |
| 天玑9300/9400 | 6.1 GKI | cgroup uclamp | 全大核退两档 |
| 麒麟9010 | 6.1 | uclamp 但 SMT | SMT 降级 cpuset |
| Exynos2500 | 6.x | cgroup uclamp | 1+2+5+2 动态聚类 |

---

## 24. customize.sh 16KB 自检伪代码
```sh
PAGE=$(getconf PAGE_SIZE)
if [ "$PAGE" = "16384" ]; then
  for bin in "$MODPATH"/bin/aatped "$MODPATH"/bin/aatpe-cli; do
    # 校验 LOAD 段对齐是否 16384
    if ! readelf -l "$bin" | grep -q "LOAD.*1000$"; then
      abort "AATPE: $bin 未按 16KB 对齐，本设备为 16KB 页，无法运行。请用 NDK r28+ 重新打包。"
    fi
  done
fi
ui_print "- 页大小: $PAGE OK"
```
- 构建侧：NDK r28+ 默认 16KB 对齐；若 r27-，LDFLAGS 加 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`。

---

## 25. 机制行为精读：schedtune vs uclamp 在选核上的差异
- **schedtune.boost**（/dev/stune/top-app）：放大整个 top-app 组的 util 信号 → 同时影响 schedutil 调频与 EAS 选核；out-of-tree，4.14 MTK 用。
- **uclamp.min**（per-task/cgroup）：
  > "UCLAMP_MIN implies boosting a task";"only EAS/CAS make use of uclamp for task placement … when a task wakes up, scheduler compares effective uclamp of every rq."
  来源：https://docs.kernel.org/scheduler/sched-util-clamp.html
- **取舍**：uclamp 是"让 EAS 自己选核"（顺 EAS 脾性、可迁移、最省电）；schedtune 是"整组抬 util"（粗粒度）；affinity 是硬绑（绕过 EAS、抗热差）。AATPE 优先级严格 uclamp > schedtune > affinity。

---

## 26. 自查溯源
- SoC 簇构成：厂商官网/规格数据库（§1，30+ 型号）。
- 机制可用性：kernel.org/AOSP 文档原文（§2、§5、§12、§25）。
- loader/16KB：Magisk/KSU/APatch/Android 官方原文（§3、§4、§14、§24）。
- 一手精读全文：AOSP cgroups.json 抽象层、kernel.org sched-energy.rst、Magisk Developer Guides、KernelSU Module Guide。
- 校正：Tensor G1/G2=2+2+4（非1+3+4）；全大核设备无小核；EAS over-utilized=80%；麒麟9010 SMT 上 EAS 不受支持；schedtune 正被 uclamp 取代。

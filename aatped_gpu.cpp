// AATPE - GPU state monitoring, Qualcomm KGSL + ARM Mali dual platform.
//
// Detection order (deterministic, cached after first run):
//   KGSL kgsl-3d0  ->  Mali mali0[/device] / /sys/kernel/gpu  ->  devfreq gpu node
//   ->  /proc/cpuinfo Hardware  ->  unknown.
// Frequencies are read as Hz and normalized to MHz. KGSL gpu_busy_percentage is
// already 0..100; the Mali DDK "load" node is parts-per-thousand (0..1000) and is
// normalized to 0..100. Temperature is millidegrees C -> whole degrees C.
// All shared state is guarded by g_gpuDualMutex; sysfs reads self-throttle to
// kGpuSampleMinMs so high-frequency decision loops do not hammer sysfs.
#include "aatped.h"

// GPU bottleneck implementation (legacy scoring knobs)
// C-010: changed to atomic for thread-safe cross-thread access.
std::atomic<int> g_gpuBusyPercent{0};
std::atomic<bool> g_gpuBottleneck{false};

// ============================================================================
// Dual-platform GPU state
// ============================================================================
GpuDualState g_gpuDual;
std::mutex   g_gpuDualMutex;
const long long kGpuSampleMinMs = 1000;   // min gap between real sysfs reads

namespace {

// Steady-clock wall ms (CLOCK_MONOTONIC-like; monotonic, immune to NTP jumps).
long long nowMs(void) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Probe a list of candidate paths, return the first that exists, is a regular
// file/symlink, and reads back non-empty. stat() follows symlinks, so sysfs
// class symlinks resolve transparently.
std::string pickFirstExisting(const std::vector<std::string>& candidates) {
    for (const auto& p : candidates) {
        if (p.empty()) continue;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::string v = readFile(p.c_str());
            if (!v.empty()) return p;
        }
    }
    return std::string();
}

// Parse the leading integer out of a sysfs node (Hz / millideg / pct / count).
// Returns -1 when the node is missing or unparseable.
long long readNodeLl(const std::string& path) {
    if (path.empty()) return -1;
    std::string v = readFile(path.c_str());
    if (v.empty()) return -1;
    try {
        size_t pos = 0;
        long long val = std::stoll(v, &pos);
        (void)pos;
        return val;
    } catch (...) {
        return -1;
    }
}

std::string readNodeStr(const std::string& path) {
    if (path.empty()) return std::string();
    return readFile(path.c_str());
}

// Normalize a raw busy/load reading to 0..100.
//   KGSL gpu_busy_percentage : already 0..100.
//   Mali DDK "load"          : 0..1000 parts-per-thousand -> /10.
//   Anything > 1000           : treated as a bad outlier, clamp to 100.
int normalizeBusyPct(long long raw) {
    if (raw < 0) return -1;
    long long u = raw;
    if (u > 1000) u = 100;
    else if (u > 100) u = u / 10;     // parts-per-thousand -> percent
    if (u < 0) u = 0;
    if (u > 100) u = 100;
    return (int)u;
}

// KGSL node table. kgslRoot is e.g. /sys/class/kgsl/kgsl-3d0.
void fillKgslNodes(const std::string& root) {
    g_gpuDual.nodeRoot    = root;
    g_gpuDual.nodeCurFreq  = pickFirstExisting({
        root + "/cur_freq",
        root + "/gpu_clock",
        root + "/gpuclk",
    });
    g_gpuDual.nodeMaxFreq  = pickFirstExisting({
        root + "/max_freq",
        root + "/gpu_max_clock",
    });
    g_gpuDual.nodeMinFreq  = pickFirstExisting({
        root + "/min_freq",
    });
    g_gpuDual.nodeUtil     = pickFirstExisting({
        root + "/gpu_busy_percentage",
        root + "/gpu_busy",
        root + "/gpu_gpu_busy_percentage",
    });
    g_gpuDual.nodeTemp     = pickFirstExisting({
        root + "/gpu_temp",
        root + "/temp",
    });
    g_gpuDual.nodeVoltage = pickFirstExisting({
        root + "/gpu_gfxrail_voltage",
        root + "/gpu_voltage",
    });
    g_gpuDual.nodeModel   = pickFirstExisting({
        root + "/gpu_model",
        root + "/gpu_model_name",
    });
}

// Mali node table. root is one of the mali0[/device] / /sys/kernel/gpu paths.
// For each node, try root/<node> first, then root/device/<node> — MediaTek
// Mali DDKs often expose the real sysfs under device/ while the class root
// only has a symlink + uevent. This fixes freq/util reading -1 on MTK Mali.
static std::string pickMaliNode(const std::string& root,
                                 const std::vector<std::string>& names) {
    std::vector<std::string> candidates;
    candidates.reserve(names.size() * 2);
    for (const auto& n : names) candidates.push_back(root + "/" + n);
    for (const auto& n : names) candidates.push_back(root + "/device/" + n);
    return pickFirstExisting(candidates);
}

void fillMaliNodes(const std::string& root) {
    g_gpuDual.nodeRoot    = root;
    g_gpuDual.nodeCurFreq  = pickMaliNode(root, {
        "cur_freq", "gpu_freq", "mali_freq", "clock", "frequency",
        // MTK Mali-G77 (MT6893 / Dimensity 1100) extras.
        "gpu_cur_freq", "clk_freq", "mali_clk", "actual_freq",
    });
    g_gpuDual.nodeMaxFreq  = pickMaliNode(root, {
        "max_freq", "gpu_max_freq", "mali_max_freq", "max_clock",
    });
    g_gpuDual.nodeMinFreq  = pickMaliNode(root, {
        "min_freq", "gpu_min_freq", "min_clock",
    });
    g_gpuDual.nodeUtil     = pickMaliNode(root, {
        "gpu_busy", "gpu_utilization", "mali_utilization", "utilization", "load",
        // MTK / newer Mali DDK load node names.
        "gpu_loading", "loading", "mali_util", "gpu_util", "busy",
    });
    g_gpuDual.nodeTemp     = pickMaliNode(root, {
        "gpu_temp", "temp", "gpu_temperature", "temperature",
    });
    g_gpuDual.nodeVoltage = pickMaliNode(root, {
        "gpu_voltage", "mali_voltage", "volt_now",
    });
    g_gpuDual.nodeModel   = pickMaliNode(root, {
        "gpu_model", "gpu_name", "mali_model",
    });
}

// absolute /proc/mali fallback. Some MTK Mali DDK builds
// expose freq/util ONLY under /proc/mali (not sysfs). Probe these absolute
// paths directly; returns true if at least the freq node was resolved.
static bool fillMaliProcNodes(void) {
    std::string freq = pickFirstExisting({
        "/proc/mali/mali_freq",
        "/proc/mali/gpu_freq",
        "/proc/mali/clock",
    });
    std::string util = pickFirstExisting({
        "/proc/mali/mali_util",
        "/proc/mali/utilization",
        "/proc/mali/loading",
    });
    if (freq.empty() && util.empty()) return false;
    if (!freq.empty()) g_gpuDual.nodeCurFreq = freq;
    if (!util.empty()) g_gpuDual.nodeUtil = util;
    if (g_gpuDual.nodeRoot.empty()) g_gpuDual.nodeRoot = "/proc/mali";
    return true;
}

// Bonus layer: scan /sys/class/devfreq/* and pick the first governor node whose
// directory name hints a GPU (gpu / mali / kgsl / 3d0 / gpubw). Returns the
// resolved root or empty. Used only when neither kgsl-3d0 nor mali0 exists.
std::string findDevfreqGpuRoot(void) {
    DIR* d = opendir("/sys/class/devfreq");
    if (!d) return std::string();
    std::string best;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string name(de->d_name);
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        bool looksGpu =
            lower.find("gpu")   != std::string::npos ||
            lower.find("mali")  != std::string::npos ||
            lower.find("kgsl")  != std::string::npos ||
            lower.find("3d0")   != std::string::npos ||
            lower.find("gfx")   != std::string::npos;
        if (!looksGpu) continue;
        std::string root = std::string("/sys/class/devfreq/") + name;
        // Must actually expose a frequency node to count as a GPU devfreq.
        std::string cur = pickFirstExisting({
            root + "/cur_freq",
            root + "/gpu_cur_freq",
        });
        if (!cur.empty()) { best = root; break; }
    }
    closedir(d);
    return best;
}

// /proc/cpuinfo "Hardware :" fallback. Maps SoC family name to a coarse
// platform tag when neither KGSL nor Mali sysfs nodes are present.
GpuPlatform detectFromCpuinfo(std::string& outModel) {
    std::ifstream f("/proc/cpuinfo");
    if (!f) return GPU_PLATFORM_UNKNOWN;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("Hardware") == std::string::npos) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string hw = line.substr(colon + 1);
        trim(hw);
        outModel = hw;
        if (hw.find("Qualcomm")   != std::string::npos ||
            hw.find("Snapdragon") != std::string::npos ||
            hw.find("SM")         != std::string::npos ||
            hw.find("MSM")        != std::string::npos) {
            return GPU_PLATFORM_KGSL;
        }
        if (hw.find("MT")       != std::string::npos ||
            hw.find("Mediatek") != std::string::npos ||
            hw.find("MediaTek") != std::string::npos) {
            return GPU_PLATFORM_MALI;
        }
        break;
    }
    return GPU_PLATFORM_UNKNOWN;
}

} // namespace

// ============================================================================
// Platform detection: KGSL -> Mali -> devfreq -> cpuinfo Hardware -> unknown.
// Runs once; node paths are cached in g_gpuDual.
// ============================================================================
void gpuDetectPlatform(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (g_gpuDual.detected) return;

    // ---- Layer 1: Qualcomm Adreno / KGSL ----
    const char* kgslRoots[] = {
        "/sys/class/kgsl/kgsl-3d0",
        "/sys/class/kgsl/kgsl-3d0/device",
    };
    for (const char* root : kgslRoots) {
        std::string cur = pickFirstExisting({
            std::string(root) + "/cur_freq",
            std::string(root) + "/gpu_clock",
        });
        if (cur.empty()) continue;
        fillKgslNodes(root);
        g_gpuDual.platform   = GPU_PLATFORM_KGSL;
        g_gpuDual.platformTag = "kgsl";
        g_gpuDual.detected    = true;
        g_gpuDual.available   = true;
        LOG_INFO("GPU: detected Qualcomm KGSL (%s), cur=%s util=%s",
                 root, g_gpuDual.nodeCurFreq.c_str(),
                 g_gpuDual.nodeUtil.c_str());
        break;
    }

    // ---- Layer 2: ARM Mali ----
    if (!g_gpuDual.detected) {
        // expanded root list. MediaTek Mali-G77 (MT6893 /
        // Dimensity 1100) exposes devfreq as /sys/class/devfreq/<addr>.mali,
        // e.g. 13000000.mali; some builds also keep /sys/kernel/gpu alive.
        const char* maliRoots[] = {
            "/sys/class/misc/mali0",
            "/sys/class/misc/mali0/device",
            "/sys/kernel/gpu",
            "/sys/class/devfreq/mali0",
            "/sys/class/devfreq/mali-gpu",
            "/sys/class/devfreq/13000000.mali",   // MT6893 / MTK common base addr
            "/sys/class/devfreq/13040000.mali",
            // additional MTK Mali devfreq base-address variants.
            // Different MTK SoCs expose the GPU devfreq node under different
            // physical base addresses; scan them all at detection time.
            "/sys/class/devfreq/130a0000.mali",
            "/sys/class/devfreq/130b0000.mali",
            "/sys/class/devfreq/mali",
        };
        LOG_INFO("GPU: probing Mali roots: misc/mali0, misc/mali0/device, "
                 "/sys/kernel/gpu, devfreq/mali0, devfreq/13000000.mali ...");
        for (const char* root : maliRoots) {
            std::string cur = pickFirstExisting({
                std::string(root) + "/cur_freq",
                std::string(root) + "/gpu_freq",
                std::string(root) + "/gpu_cur_freq",
                std::string(root) + "/mali_freq",
                std::string(root) + "/clock",
            });
            if (cur.empty()) continue;
            fillMaliNodes(root);
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mali";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: selected root=%s freq_node=%s util_node=%s",
                     root, g_gpuDual.nodeCurFreq.c_str(),
                     g_gpuDual.nodeUtil.c_str());
            break;
        }
        // ---- Layer 2b: MTK GED (no devfreq on D1100/MT6893) ----
    // Redmi chopin / Dimensity 1100 exposes GPU DVFS through MTK's GED driver,
    // NOT through standard /sys/class/devfreq. The devfreq directory is empty
    // on this device ("no devfreq devices"). Probe MTK-specific nodes:
    //   /sys/class/devfreq/13fbb000.mali_dvfs_hint  (MTK Mali DVFS hint)
    //   /sys/class/devfreq/1100b000.eemgpu_fsm      (EEM GPU FSM)
    //   /proc/gpufreq/*                              (platform gpufreq, GED control)
    if (!g_gpuDual.detected) {
        const char* mtkRoots[] = {
            "/sys/class/devfreq/13fbb000.mali_dvfs_hint",
            "/sys/class/devfreq/1100b000.eemgpu_fsm",
            "/sys/class/devfreq/13000000.mali",
        };
        for (const char* root : mtkRoots) {
            std::string cur = pickFirstExisting({
                std::string(root) + "/cur_freq",
                std::string(root) + "/gpu_freq",
                std::string(root) + "/load",
                std::string(root) + "/utilization",
            });
            if (cur.empty()) continue;
            fillMaliNodes(root);
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mtk_ged";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: MTK GED root=%s freq_node=%s", root, cur.c_str());
            break;
        }
        // /proc/gpufreq fallback (MTK GED sysfs-less control plane).
        if (!g_gpuDual.detected) {
            std::string oppIdx = pickFirstExisting({
                "/proc/gpufreq/gpufreq_opp_index",
                "/proc/gpufreq/gpufreq_cur_opp",
            });
            std::string oppFreq = pickFirstExisting({
                "/proc/gpufreq/gpufreq_opp_freq",
                "/proc/gpufreq/gpufreq_cur_freq",
            });
            // P3-a: only report as freq if we have a REAL freq node. If only
            // opp_index exists, do NOT report OPP index as MHz (unit mismatch).
            // Mark current_freq unavailable; index is level-only, not Hz.
            if (!oppFreq.empty()) {
                g_gpuDual.platform   = GPU_PLATFORM_MALI;
                g_gpuDual.platformTag = "mtk_gpufreq";
                g_gpuDual.nodeCurFreq = oppFreq;
                g_gpuDual.nodeGovernor = "/proc/gpufreq/gpufreq_governor";
                g_gpuDual.nodeMaxFreq  = "/proc/gpufreq/gpufreq_max_opp";
                g_gpuDual.nodeMinFreq  = "/proc/gpufreq/gpufreq_min_opp";
                g_gpuDual.detected    = true;
                g_gpuDual.available   = true;
                LOG_INFO("GPU: MTK /proc/gpufreq freq node=%s", oppFreq.c_str());
            } else if (!oppIdx.empty()) {
                // Index-only: detect but mark freq unavailable. No fake MHz.
                g_gpuDual.platform   = GPU_PLATFORM_MALI;
                g_gpuDual.platformTag = "mtk_gpufreq_index";
                g_gpuDual.nodeCurFreq = "";   // unknown, do not fake
                g_gpuDual.nodeUtil = oppIdx;  // level info, not utilization
                g_gpuDual.detected    = true;
                g_gpuDual.available   = false;  // read-only level, no freq control
                LOG_INFO("GPU: MTK /proc/gpufreq index-only (no real freq node); "
                         "current_freq unavailable");
            }
        }
    }

    // absolute /proc/mali fallback when no sysfs root
        // yielded a freq node but the DDK exposes /proc/mali/*.
        if (!g_gpuDual.detected && fillMaliProcNodes()) {
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mali";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: selected /proc/mali freq_node=%s util_node=%s",
                     g_gpuDual.nodeCurFreq.c_str(),
                     g_gpuDual.nodeUtil.c_str());
        }
    }

    // ---- Layer 3: generic devfreq GPU node ----
    if (!g_gpuDual.detected) {
        std::string root = findDevfreqGpuRoot();
        if (!root.empty()) {
            fillMaliNodes(root);   // devfreq node names match Mali schema
            g_gpuDual.platform   = GPU_PLATFORM_MALI;
            g_gpuDual.platformTag = "mali";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = true;
            LOG_INFO("GPU: detected GPU via devfreq (%s)", root.c_str());
        }
    }

    // ---- Layer 4: /proc/cpuinfo Hardware ----
    if (!g_gpuDual.detected) {
        std::string hw;
        GpuPlatform fb = detectFromCpuinfo(hw);
        if (fb != GPU_PLATFORM_UNKNOWN) {
            g_gpuDual.platform   = fb;
            g_gpuDual.platformTag = (fb == GPU_PLATFORM_KGSL) ? "kgsl" : "mali";
            g_gpuDual.model       = hw;
            g_gpuDual.detected    = true;
            g_gpuDual.available   = false; // no usable sysfs nodes
            LOG_INFO("GPU: cpuinfo fallback platform=%s model=%s (no sysfs nodes)",
                     g_gpuDual.platformTag.c_str(), hw.c_str());
        } else {
            g_gpuDual.platform   = GPU_PLATFORM_UNKNOWN;
            g_gpuDual.platformTag = "unknown";
            g_gpuDual.detected    = true;
            g_gpuDual.available   = false;
            LOG_WARN("GPU: no KGSL/Mali/devfreq nodes found, platform=unknown");
        }
    }

    // Read model string from sysfs when available (overrides cpuinfo guess).
    if (!g_gpuDual.nodeModel.empty()) {
        std::string m = readNodeStr(g_gpuDual.nodeModel);
        if (!m.empty()) g_gpuDual.model = m;
    }
    if (g_gpuDual.model.empty()) {
        g_gpuDual.model = g_gpuDual.platformTag;
    }
}

// ============================================================================
// Periodic sample. Reads cached node paths, normalizes units. Self-throttled.
// ============================================================================
void gpuSampleAll(void) {
    gpuDetectPlatform();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (!g_gpuDual.detected) return;

    long long now = nowMs();
    if (g_gpuDual.lastSampleMs != 0 &&
        now - g_gpuDual.lastSampleMs < kGpuSampleMinMs) {
        return;   // too soon; reuse cached values
    }
    g_gpuDual.lastSampleMs = now;

    // Frequencies: sysfs Hz -> MHz.
    long long curHz = readNodeLl(g_gpuDual.nodeCurFreq);
    long long maxHz = readNodeLl(g_gpuDual.nodeMaxFreq);
    long long minHz = readNodeLl(g_gpuDual.nodeMinFreq);
    g_gpuDual.curFreqMhz = (curHz >= 0) ? (int)(curHz / 1000000LL) : -1;
    g_gpuDual.maxFreqMhz = (maxHz >= 0) ? (int)(maxHz / 1000000LL) : -1;
    g_gpuDual.minFreqMhz = (minHz >= 0) ? (int)(minHz / 1000000LL) : -1;

    // P1-7 / P3-23: kHz-vs-Hz heuristic. Some devfreq nodes (notably Mali on
    // certain MTK kernels, including Dimensity 1100 / MT6893) report frequency
    // in kHz, not Hz. After /1e6 that collapses to <10 MHz, which is obviously
    // wrong for a GPU. The detection is deliberately conservative:
    //   - raw must be non-negative
    //   - after /1e6 the result must still look tiny (<10 MHz)
    //   - raw must sit in a plausible kHz window [100, 20000] kHz (= 0.1..2 GHz)
    // Only then do we rescale by /1000 (kHz -> MHz). A node that reports Hz
    // correctly (e.g. 650000000 Hz -> 650 MHz) never matches because 650 is
    // not <10 after /1e6. This is a heuristic, not a unit query: some exotic
    // nodes could still mis-report, but the false-positive rate is low because
    // real GPU Hz readings are always >= ~50,000,000 Hz (-> 50 MHz).
    // G6-009: kHz window corrected. A GPU reporting kHz:
    //   - 650 MHz Mali (MT6893 / Dimensity 1100) reads raw=650000 kHz;
    //   - after /1e6 that is 0 (<10), but the old upper bound 20000 (=20 MHz)
    //     rejected the very target part, so nodeFreqIsKhz stayed false and the
    //     write path emitted mhz*1e6 (~1000x too large) to a kHz node.
    // Valid kHz range now covers 0.1 MHz .. 5 GHz => [100, 5_000_000]. A real
    // Hz-reporting node reads >= ~50,000,000 Hz (>=50 MHz => afterHz>=50), so it
    // never satisfies afterHz<10.
    auto rescaleKhz = [](long long raw, int mhz) -> int {
        if (raw >= 0 && mhz < 10 && raw >= 100 && raw <= 5000000LL) {
            return (int)(raw / 1000LL);
        }
        return mhz;
    };
    // LED-048 (R4-004): detect once whether the freq nodes report kHz. The same
    // heuristic that rescales the read side gates the write-side unit conversion.
    {
        int afterHz = (maxHz >= 0) ? (int)(maxHz / 1000000LL) : -1;
        bool looksLikeKhz = (maxHz >= 0 && afterHz >= 0 && afterHz < 10 &&
                             maxHz >= 100 && maxHz <= 5000000LL);
        if (looksLikeKhz) g_gpuDual.nodeFreqIsKhz = true;
    }
    g_gpuDual.curFreqMhz = rescaleKhz(curHz, g_gpuDual.curFreqMhz);
    g_gpuDual.maxFreqMhz = rescaleKhz(maxHz, g_gpuDual.maxFreqMhz);
    g_gpuDual.minFreqMhz = rescaleKhz(minHz, g_gpuDual.minFreqMhz);

    // Remember the stock max once so freq cap can be restored.
    if (g_gpuDual.savedDefaultMaxHz < 0 && maxHz > 0) {
        g_gpuDual.savedDefaultMaxHz = maxHz;
    }

    // Utilization: KGSL is already 0..100; Mali load is 0..1000 (parts-per-thousand).
    long long util = readNodeLl(g_gpuDual.nodeUtil);
    g_gpuDual.utilPct = normalizeBusyPct(util);

    // Temperature: KGSL gpu_temp / Mali temp are millidegrees C -> degrees C.
    // P2 : unit boundary. If the raw reading is small (<1000) it is
    // already in whole degrees C (some Mali nodes report °C directly, not
    // millidegrees); dividing by 1000 would collapse it to 0.
    // GPUTOP-09: after conversion, sanity-cap to [0, 150] °C. A hostile or
    // mis-wired node reporting 950 (which is either 950 °C or 950 mdeg = 0.95 °C)
    // would previously be treated as 950 °C with no range check; now anything
    // outside [0, 150] is flagged as -1 (invalid) rather than trusted.
    long long t = readNodeLl(g_gpuDual.nodeTemp);
    if (t < 0) {
        g_gpuDual.tempC = -1;
    } else {
        int tc;
        if (t >= 1000) {
            tc = (int)(t / 1000LL);   // millidegrees -> degrees C
        } else {
            tc = (int)t;              // already degrees C
        }
        if (tc < 0 || tc > 150) {
            LOG_WARN("GPU: temp reading %lld out of range (tc=%d) -> invalid", t, tc);
            g_gpuDual.tempC = -1;
        } else {
            g_gpuDual.tempC = tc;
        }
    }

    // Voltage: raw microvolt as reported by the rail node.
    // P3-24: clamp. A hostile/buggy sysfs node can report values outside the
    // plausible GPU rail range (0..2000000 µV = 0..2.0 V); without clamping
    // those values get published to state files and CSV as-is, confusing the
    // WebUI/APK. -1 means "no reading".
    long long v = readNodeLl(g_gpuDual.nodeVoltage);
    if (v < 0) {
        g_gpuDual.voltageUv = -1;
    } else {
        // Plausible GPU rail range: 0 .. 2.0 V (0 .. 2,000,000 µV).
        if (v > 2000000LL) v = 2000000LL;
        g_gpuDual.voltageUv = (int)v;
    }

    // Mark usable once any of freq/util is actually readable.
    if (g_gpuDual.curFreqMhz >= 0 || g_gpuDual.utilPct >= 0) {
        g_gpuDual.available = true;
    }

    // periodic diagnostic dump every 30 seconds. Previously the
    // GPU detection only logged at startup (gpuDetectPlatform), so on-device
    // logs taken later never showed why freq/util read as 0. This self-throttled
    // block prints the resolved root, node paths, and current values so the
    // operator can see exactly what was probed and what was read back.
    {
        static long long s_lastDiagLogMs = 0;
        if (s_lastDiagLogMs == 0 || (now - s_lastDiagLogMs) >= 30000) {
            s_lastDiagLogMs = now;
            LOG_INFO("GPU-DIAG: platform=%s root=%s freq_node=%s util_node=%s "
                     "cur=%dMHz util=%d%% temp=%dC avail=%d",
                     g_gpuDual.platformTag.c_str(),
                     g_gpuDual.nodeRoot.c_str(),
                     g_gpuDual.nodeCurFreq.c_str(),
                     g_gpuDual.nodeUtil.c_str(),
                     g_gpuDual.curFreqMhz, g_gpuDual.utilPct,
                     g_gpuDual.tempC, (int)g_gpuDual.available);
        }
    }
}

// ============================================================================
// Publish normalized state files for WebUI / APK consumers.
// ============================================================================
// v4.2.7: parse GED freq from text content (handles "curfreq=836000" or raw "836000").
// Returns MHz value (KHz→MHz auto-conversion for values >100000).
int aatpeParseGedFreqMhz(const std::string& content) {
    // Find first digit in the buffer.
    size_t pos = 0;
    while (pos < content.size() && !isdigit((unsigned char)content[pos])) pos++;
    if (pos >= content.size()) return 0;
    long val = strtol(content.c_str() + pos, nullptr, 10);
    if (val <= 0) return 0;
    return (val > 100000) ? (int)(val / 1000) : (int)val;  // KHz→MHz
}

void gpuWriteStateFiles(void) {
    // GPUTOP-10: snapshot g_gpuDual under the lock, then release it before doing
    // the slow GED fopen/fgets sysfs reads. State-file writes are independent of
    // g_gpuDual and do not need the mutex.
    std::string platformTag, model, nodeRoot;
    int pubFreq, pubMax, pubMin, pubUtil, pubTemp, pubVolt;
    {
        std::lock_guard<std::mutex> lk(g_gpuDualMutex);
        std::string dir = STATE_DIR;
        (void)dir;  // dir used below after the lock release

        // publish 0 (no-data) instead of -1 when a node could
        // not be read. The legacy -1 is an internal "unknown" sentinel, but the APK
        // rendered it as a literal "-1 MHz" / "-1%" error. 0 means "no reading yet".
        pubFreq  = (g_gpuDual.curFreqMhz  >= 0) ? g_gpuDual.curFreqMhz  : 0;
        pubMax   = (g_gpuDual.maxFreqMhz  >= 0) ? g_gpuDual.maxFreqMhz  : 0;
        pubMin   = (g_gpuDual.minFreqMhz  >= 0) ? g_gpuDual.minFreqMhz  : 0;
        pubUtil  = (g_gpuDual.utilPct     >= 0) ? g_gpuDual.utilPct     : 0;
        pubTemp  = (g_gpuDual.tempC       >= 0) ? g_gpuDual.tempC       : 0;
        pubVolt  = (g_gpuDual.voltageUv   >= 0) ? g_gpuDual.voltageUv   : 0;
        platformTag = g_gpuDual.platformTag;
        model       = g_gpuDual.model;
        nodeRoot    = g_gpuDual.nodeRoot;
    }  // lock released here

    std::string dir = STATE_DIR;

    // P2-3: GED fallback for MTK devices where devfreq nodes are absent.
    // When devfreq path yields 0, try GED HAL gpu_freq node as fallback.
    // v4.2.8 B9: 1s cache for GED reads (GPU freq doesn't need 50ms precision).
    static int s_gedCachedFreq = 0;
    static int s_gedCachedMax = 0;
    static long long s_gedLastReadMs = 0;
    auto readGedFreqNode = [](const char* path) -> int {
        FILE* f = fopen(path, "r");
        if (!f) return 0;
        char buf[256] = {0};
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
        fclose(f);
        return aatpeParseGedFreqMhz(buf);
    };
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
    bool gedCacheValid = (nowMs - s_gedLastReadMs) < 1000;  // 1s TTL
    if (pubFreq == 0) {
        if (gedCacheValid && s_gedCachedFreq > 0) {
            pubFreq = s_gedCachedFreq;
        } else {
            pubFreq = readGedFreqNode("/sys/kernel/ged/hal/gpu_freq");
            s_gedCachedFreq = pubFreq;
            s_gedLastReadMs = nowMs;
        }
    }
    if (pubMax == 0) {
        if (gedCacheValid && s_gedCachedMax > 0) {
            pubMax = s_gedCachedMax;
        } else {
            pubMax = readGedFreqNode("/sys/kernel/ged/hal/gpu_max_freq");
            s_gedCachedMax = pubMax;
        }
    }

    writeFile((dir + "/gpu_platform").c_str(),  platformTag);
    writeFile((dir + "/gpu_model").c_str(),     model);
    writeFile((dir + "/gpu_root").c_str(),      nodeRoot);
    writeFile((dir + "/gpu_freq").c_str(),      pubFreq);
    writeFile((dir + "/gpu_max_freq").c_str(),  pubMax);
    writeFile((dir + "/gpu_min_freq").c_str(),  pubMin);
    writeFile((dir + "/gpu_util").c_str(),      pubUtil);
    writeFile((dir + "/gpu_temp").c_str(),     pubTemp);
    writeFile((dir + "/gpu_voltage").c_str(),  pubVolt);
}

// ============================================================================
// History record under METRICS_DIR/gpu_metrics.csv. Appends one line per
// sample; rolls the file back to its header when it exceeds ~256 KiB so it
// cannot grow without bound on an always-on device.
// ============================================================================
void gpuAppendMetrics(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (!g_gpuDual.available) return;

    std::string path = std::string(METRICS_DIR) + "/gpu_metrics.csv";
    const char* header =
        "epoch,platform,cur_mhz,max_mhz,min_mhz,util_pct,temp_c,voltage_uv\n";

    // LED-051 (R4-007): stat() then open() is a TOCTOU — between the two, an
    // attacker could replace path with a symlink. Open with O_NOFOLLOW first,
    // then fstat the fd so the size check is on the same file we write to.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return;
    struct stat st;
    bool needHeader = (fstat(fd, &st) != 0 || st.st_size == 0);
    bool roll = (!needHeader && st.st_size > (256L * 1024L));

    if (roll) {
        // G6-011: reopen with O_NOFOLLOW|O_TRUNC instead of ofstream (which has no
        // O_NOFOLLOW), closing the TOCTOU window between close(fd) and reopen.
        close(fd);
        int rfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (rfd < 0) return;
        dprintf(rfd, "%s", header);
        dprintf(rfd, "%lld,%s,%d,%d,%d,%d,%d,%d\n",
            (long long)time(nullptr), g_gpuDual.platformTag.c_str(),
            g_gpuDual.curFreqMhz, g_gpuDual.maxFreqMhz,
            g_gpuDual.minFreqMhz, g_gpuDual.utilPct,
            g_gpuDual.tempC, g_gpuDual.voltageUv);
        ::close(rfd);
        return;
    }

    // Append path: keep the O_NOFOLLOW fd; wrap it in a FILE* (takes ownership).
    FILE* af = ::fdopen(fd, "a");
    if (!af) { ::close(fd); return; }
    if (needHeader) std::fputs(header, af);
    fprintf(af, "%lld,%s,%d,%d,%d,%d,%d,%d\n",
        (long long)time(nullptr), g_gpuDual.platformTag.c_str(),
        g_gpuDual.curFreqMhz, g_gpuDual.maxFreqMhz,
        g_gpuDual.minFreqMhz, g_gpuDual.utilPct,
        g_gpuDual.tempC, g_gpuDual.voltageUv);
    std::fclose(af);
    (void)0;
}

// ============================================================================
// Frequency cap control.
// ============================================================================
bool gpuSetMaxFreqMhz(int mhz) {
    gpuDetectPlatform();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (g_gpuDual.nodeMaxFreq.empty()) {
        LOG_WARN("GPU: setMaxFreq no writable max_freq node (platform=%s)",
                 g_gpuDual.platformTag.c_str());
        return false;
    }
    if (g_gpuDual.maxFreqMhz > 0 && mhz > g_gpuDual.maxFreqMhz) {
        mhz = g_gpuDual.maxFreqMhz;   // never exceed the hardware ceiling
    }
    if (g_gpuDual.minFreqMhz > 0 && mhz < g_gpuDual.minFreqMhz) {
        mhz = g_gpuDual.minFreqMhz;   // never go below the floor
    }
    // LED-048 (R4-004): write in the node's native unit. The read side detected
    // kHz-reporting nodes via rescaleKhz; without this, setMaxFreqMhz wrote
    // mhz*1e6 (Hz) to a kHz node, which the driver clamped/rejected (the value
    // was ~1000x too large). savedDefaultMaxHz is stored raw, so restore is
    // already unit-consistent; only this write needed the fix.
    long long outVal = g_gpuDual.nodeFreqIsKhz
        ? (long long)mhz * 1000LL
        : (long long)mhz * 1000000LL;
    std::string s = std::to_string(outVal);
    if (!writeFile(g_gpuDual.nodeMaxFreq.c_str(), s)) {
        LOG_WARN("GPU: setMaxFreq write failed (%s = %lld %s) errno=%d (%s)",
                 g_gpuDual.nodeMaxFreq.c_str(), outVal,
                 g_gpuDual.nodeFreqIsKhz ? "kHz" : "Hz", errno, aatpeErrStr(errno));
        return false;
    }
    LOG_INFO("GPU: max freq capped to %d MHz (%lld %s) via %s",
             mhz, outVal, g_gpuDual.nodeFreqIsKhz ? "kHz" : "Hz",
             g_gpuDual.nodeMaxFreq.c_str());
    return true;
}

bool gpuRestoreDefaultFreq(void) {
    gpuDetectPlatform();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    if (g_gpuDual.savedDefaultMaxHz < 0 || g_gpuDual.nodeMaxFreq.empty()) {
        LOG_WARN("GPU: restoreFreq no saved default / node unavailable");
        return false;
    }
    std::string s = std::to_string(g_gpuDual.savedDefaultMaxHz);
    if (!writeFile(g_gpuDual.nodeMaxFreq.c_str(), s)) {
        LOG_WARN("GPU: restoreFreq write failed errno=%d (%s)",
                 errno, aatpeErrStr(errno));
        return false;
    }
    LOG_INFO("GPU: restored default max freq to %lld Hz",
             g_gpuDual.savedDefaultMaxHz);
    return true;
}

int gpuGetPlatform(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    return (int)g_gpuDual.platform;
}

std::string gpuGetPlatformTag(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    return g_gpuDual.platformTag;
}

std::string gpuGetModel(void) {
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    return g_gpuDual.model;
}

// LED-049 (R4-005): g_gpuType is a `const char*` global read by other threads
// (placement/scoring/WebUI) without g_gpuDualMutex. The previous approach set
//   g_gpuType = s_gpuTypeOwner.c_str();
// every sample tick. When s_gpuTypeOwner is reassigned, std::string reallocates
// its internal buffer, freeing the old bytes that g_gpuType still points at —
// a reader on another thread can dereference freed memory (UAF) or see a torn
// pointer. Fix: publish via a heap-allocated buffer that is NEVER freed (platform
// tags are short and there are at most 2-3 distinct values per process life, so
// the intentional leak is negligible), and only re-publish when the value
// actually changes. The pointer store itself is atomic on aligned word-sized
// platforms; readers get either the old or new stable pointer.
static std::string s_gpuTypeOwner = "unknown";

// ============================================================================
// Legacy entry point called by the daemon main loop every ~5s. Drives the full
// dual-platform sample + state-file publication + metrics append, and keeps the
// legacy bottleneck globals / g_gpuState in sync for the scoring path.
// ============================================================================
void readGpuUtilization(void) {
    gpuSampleAll();
    gpuWriteStateFiles();
    gpuAppendMetrics();

    // Mirror into legacy globals consumed by placement scoring + state JSON.
    {
        std::lock_guard<std::mutex> lk(g_gpuDualMutex);
        int u = g_gpuDual.utilPct;
        if (u < 0) u = 0;
        g_gpuBusyPercent = u;
        g_gpuUtilization.store((float)u, std::memory_order_relaxed);
        // LED-049: only (re)publish when the tag actually changes. strdup the new
        // tag into a permanent heap buffer; the old buffer is intentionally leaked
        // (it is tiny and readers may still hold it).
        if (g_gpuDual.platformTag != s_gpuTypeOwner) {
            s_gpuTypeOwner = g_gpuDual.platformTag;
            char* published = strdup(s_gpuTypeOwner.c_str());
            g_gpuType.store(published, std::memory_order_relaxed);  // atomic pointer publish
        }
        g_gpuState.available   = g_gpuDual.available;
        g_gpuState.utilization = u;
        g_gpuState.type        = g_gpuDual.platformTag;
    }
}

bool isGpuBottleneck(float cpuUtilization) {
    // P3-22: g_gpuBusyPercent is std::atomic<int> initialized to 0 and
    // readGpuUtilization() clamps any negative read to 0 before storing, so
    // the old `if (g_gpuBusyPercent < 0) return false;` branch was dead code.
    g_gpuBottleneck = (g_gpuBusyPercent.load(std::memory_order_relaxed) > 80 &&
                       cpuUtilization < 50);
    return g_gpuBottleneck;
}

// F-07 (v6.0.0 thermal拔河 fix): gpuBoostTick() runs every ~50ms but used to
// read g_gpuUtilization, which is only refreshed by readGpuUtilization() once
// every ~5s (driven by the daemon main loop). On a Mali node that coarsely
// samples busy%, a 5s-stale util reading means the boost state machine reacts
// to a 5-second-old picture: util spikes at frame start can keep boost latched
// long after the load dropped, and the 85% threshold is judged against a frozen
// value. We now expose a lightweight refresh that gpuBoostTick() calls each
// cycle. gpuSampleAll() self-throttles to kGpuSampleMinMs(=1s), so calling it at
// 50ms cadence is cheap (no-op inside the window).
void aatpeGpuRefreshUtilForBoost(void) {
    gpuSampleAll();
    std::lock_guard<std::mutex> lk(g_gpuDualMutex);
    int u = g_gpuDual.utilPct;
    // Only publish when a real reading is available. On a host test root (or
    // early boot before nodes are discovered) utilPct is -1; we must NOT clobber
    // the test-set / previously-valid g_gpuUtilization with 0.
    if (u >= 0) {
        g_gpuUtilization.store((float)u, std::memory_order_relaxed);
    }
}

int getGpuBottleneckDecisionInterval(void) {
    // GPU bottleneck: double decision interval to reduce CPU overhead
    return g_gpuBottleneck ? 200 : 100;
}

// ROUND-014 P3: GPU node availability probe + throttle state keys.
// GPUTOP-04: s_gpuPeakMax -> atomic, and the tick is self-throttled to 1s so
// the main loop does not hammer sysfs every cycle.
static std::atomic<long long> s_gpuPeakMax{0};
static std::atomic<long long> s_gpuThrottleLastMs{0};
static constexpr long long kGpuThrottleMinMs = 1000;

bool aatpeGpuNodeAvailable(int vendor) {
    // vendor 0=Mali, 1=Adreno
    if (vendor == 0) {
        return access("/sys/class/misc/mali0/device/devfreq/cur_freq", F_OK) == 0;
    } else {
        return access("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", F_OK) == 0;
    }
}

// ROUND-014 P3: resolve Mali devfreq path (two vendor layouts).
static std::string resolveMaliDevfreq() {
    static std::mutex cachedMtx;
    static std::string cached;
    std::lock_guard<std::mutex> lk(cachedMtx);
    if (!cached.empty()) return cached;
    // Layout 1: /sys/class/misc/mali0/device/devfreq
    if (access("/sys/class/misc/mali0/device/devfreq/cur_freq", F_OK) == 0) {
        cached = "/sys/class/misc/mali0/device/devfreq"; return cached;
    }
    // Layout 2: /sys/class/devfreq/*.mali/
    DIR* d = opendir("/sys/class/devfreq");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (strstr(e->d_name, "mali")) {
                std::string p = std::string("/sys/class/devfreq/") + e->d_name + "/cur_freq";
                if (access(p.c_str(), F_OK) == 0) {
                    cached = std::string("/sys/class/devfreq/") + e->d_name;
                    closedir(d); return cached;
                }
            }
        }
        closedir(d);
    }
    return cached;
}

void aatpeGpuThrottleTick() {
    // GPUTOP-04: self-throttle to >=1s between real sysfs reads.
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = s_gpuThrottleLastMs.load(std::memory_order_relaxed);
    if (last != 0 && (now - last) < kGpuThrottleMinMs) return;
    s_gpuThrottleLastMs.store(now, std::memory_order_relaxed);

    std::string mali = resolveMaliDevfreq();
    if (!mali.empty()) {
        int cur = readInt((mali + "/cur_freq").c_str(), -1);
        int mx = readInt((mali + "/max_freq").c_str(), -1);
        if (cur >= 0 && mx >= 0) {
            long long peak = s_gpuPeakMax.load(std::memory_order_relaxed);
            if (mx > peak) s_gpuPeakMax.store(mx, std::memory_order_relaxed);
            peak = s_gpuPeakMax.load(std::memory_order_relaxed);
            float util = g_gpuUtilization.load();
            bool gpuBusy = (util >= 50.0f);
            writeFileAtomic((std::string(STATE_DIR) + "/gpu_hard_throttle").c_str(),
                (gpuBusy && cur < (int)(mx*0.95)) ? "1" : "0");
            writeFileAtomic((std::string(STATE_DIR) + "/gpu_thermal_cap").c_str(),
                (peak>0 && mx < (int)(peak*0.95)) ? "1" : "0");
            return;
        }
    }
    // Adreno kgsl fallback.
    int cur = readInt("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", -1);
    int mx = readInt("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", -1);
    if (cur >= 0 && mx >= 0) {
        long long peak = s_gpuPeakMax.load(std::memory_order_relaxed);
        if (mx > peak) s_gpuPeakMax.store(mx, std::memory_order_relaxed);
        peak = s_gpuPeakMax.load(std::memory_order_relaxed);
        float util = g_gpuUtilization.load();
        bool gpuBusy = (util >= 50.0f);
        writeFileAtomic((std::string(STATE_DIR) + "/gpu_hard_throttle").c_str(),
            (gpuBusy && cur < (int)(mx*0.95)) ? "1" : "0");
        writeFileAtomic((std::string(STATE_DIR) + "/gpu_thermal_cap").c_str(),
            (peak>0 && mx < (int)(peak*0.95)) ? "1" : "0");
    }
}

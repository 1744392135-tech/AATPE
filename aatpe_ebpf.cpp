// AATPE eBPF Event-Driven Architecture — comprehensive implementation
// Multi-strategy loader (embedded_syscall / syscall_precompiled / bpftool_precompiled / fallback)
// + per-CPU perf ring buffer + event aggregation + degradation.
#include "aatpe_ebpf.h"
#include "aatped.h"
#include "aatpe_subsystem.h" // round-5: lifecycle/safe-mode hooks
#include "integrity.h"        // G3: privilegedOpsAllowed() fail-closed gate
#include "ebpf/sched_events_embedded.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>

namespace aatpe {
// ROUND-014 P3: single source of truth mapping EbpfState -> ebpf_state string.
// Old internal states are folded into the seven-state ladder per SPEC §2.5.
const char* ebpfStateToString(int s) {
    using S = aatpe_ebpf::EbpfState;
    switch ((S)s) {
        case S::EVENT_DRIVEN:  return "event_driven";
        case S::ACTIVE:        return "event_driven";   // legacy alias
        case S::DEGRADED_RING: return "degraded_ring";
        case S::POLLING:       return "polling";
        case S::DEGRADED:      return "polling";       // legacy fallback
        case S::DENIED_SELINUX:return "denied_selinux";
        case S::NO_CAP:        return "no_cap";
        case S::KERNEL_UNSUPPORTED: return "kernel_unsupported";
        case S::UNVERIFIED:    return "unverified";
        case S::LOAD_TIMEOUT:  return "kernel_unsupported";
        case S::LOADING: case S::UNINITIALIZED: return "unverified";
        default:               return "unverified";
    }
}
}  // namespace aatpe

// ROUND-014 P3: errno -> seven-state classification (pure, host-testable).
namespace aatpe {
aatpe_ebpf::EbpfState classifyEbpfFailure(int err, bool progLoaded, bool ringReadable) {
    using S = aatpe_ebpf::EbpfState;
    if (progLoaded && ringReadable) return S::EVENT_DRIVEN;
    if (progLoaded && !ringReadable) return S::DEGRADED_RING;
    switch (err) {
        case EACCES: case EPERM:  return S::DENIED_SELINUX;
        case ENOSYS: case EOPNOTSUPP: return S::KERNEL_UNSUPPORTED;
        case ESRCH: case ENOENT:  return S::KERNEL_UNSUPPORTED;
        default:                  return S::POLLING;
    }
}
}  // namespace aatpe
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <sched.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#include <ctime>
#include <map>

namespace aatpe_ebpf {

// ============================================================================
// Logging
// ============================================================================

// ============================================================================
// BPF constants (explicit for old NDK headers)
// ============================================================================
#ifndef BPF_PROG_TYPE_KPROBE
#define BPF_PROG_TYPE_KPROBE 2
#endif
#ifndef BPF_PROG_TYPE_TRACEPOINT
#define BPF_PROG_TYPE_TRACEPOINT 5
#endif
#ifndef BPF_PROG_TYPE_RAW_TRACEPOINT
#define BPF_PROG_TYPE_RAW_TRACEPOINT 17
#endif
#ifndef BPF_PROG_TYPE_RAW_TRACEPOINT_ALT
#define BPF_PROG_TYPE_RAW_TRACEPOINT_ALT 24
#endif
// #21: BPF_RAW_TRACEPOINT_OPEN command (kernel 4.17+). Opens a raw tracepoint
// link fd; closing the fd detaches. This is the CORRECT attach method for
// BPF_PROG_TYPE_RAW_TRACEPOINT programs — perf_event_open+SET_BPF always fails
// with EINVAL/EOPNOTSUPP for raw_tp progs.
#ifndef BPF_RAW_TRACEPOINT_OPEN
#define BPF_RAW_TRACEPOINT_OPEN 28
#endif
#ifndef BPF_PROG_TYPE_PERF_EVENT
#define BPF_PROG_TYPE_PERF_EVENT 7
#endif
#ifndef BPF_MAP_TYPE_PERCPU_ARRAY
#define BPF_MAP_TYPE_PERCPU_ARRAY 6
#endif
#ifndef BPF_MAP_TYPE_ARRAY
#define BPF_MAP_TYPE_ARRAY 2
#endif
#ifndef BPF_MAP_TYPE_PERF_EVENT_ARRAY
#define BPF_MAP_TYPE_PERF_EVENT_ARRAY 4
#endif
#ifndef BPF_MAP_TYPE_HASH
#define BPF_MAP_TYPE_HASH 1
#endif
/* v4.5.0 F-03: LRU_HASH (type 9 per kernel UAPI <linux/bpf.h>: ...PROG_ARRAY=3,
 * PERCPU_ARRAY=6,STACK_TRACE=7,CGROUP_ARRAY=8,LRU_HASH=9). Kernel 4.10+, and
 * the module targets Android 10+ (4.14+) so always available. Used for
 * wakeup_cache so the kernel auto-evicts the coldest (orphaned) entry near full
 * instead of an 8192 HASH silently filling up. Loader creates it with a
 * plain-HASH(1) fallback. NOTE: 3 is PROG_ARRAY, NOT LRU_HASH — do not revert. */
#ifndef BPF_MAP_TYPE_LRU_HASH
#define BPF_MAP_TYPE_LRU_HASH 9
#endif

// ENOTSUPP is a kernel-internal errno (524) defined in include/linux/errno.h.
// It is NOT a standard POSIX errno and is not exposed by bionic libc headers.
// Some Android 4.14 kernels return -ENOTSUPP from BPF_MAP_LOOKUP_ELEM on
// BPF_MAP_TYPE_PERF_EVENT_ARRAY because their perf_event_array_map_ops does
// not implement .map_lookup_elem (callback is NULL). This is NOT a sign that
// the update failed — the update callback works fine and stores the perf
// event pointer correctly. User-space lookup of PERF_EVENT_ARRAY is also
// semantically useless (it returns a kernel virtual address), so we must
// NOT use it to verify updates.
#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif

// PERF_COUNT_SW_BPF_OUTPUT (10) is the software event type designed
// specifically for BPF perf event output. Defined in linux/perf_event.h
// since kernel 4.2, but some old NDK bionic headers may lack it.
#ifndef PERF_COUNT_SW_BPF_OUTPUT
#define PERF_COUNT_SW_BPF_OUTPUT 10
#endif

static const int kLoadTimeoutMs = 2000;
// P2-6: verifier log buffer. The kernel writes up to log_size bytes into
// log_buf; if it fills the whole buffer there is NO trailing NUL and the
// LOG_WARN("%s", bpfLogBuf) would read out of bounds. Allocate 65537 bytes,
// tell the kernel log_size=65536, and keep byte[65536] as a permanent NUL
// (memset zeros the whole buffer before each PROG_LOAD).
static const size_t kBpfLogSize = 65536;
static thread_local char bpfLogBuf[kBpfLogSize + 1];

// ---------------------------------------------------------------------------
// Compute the running kernel's LINUX_VERSION_CODE in the format expected by
// the BPF_PROG_LOAD attr.kern_version field: (major << 16) | (minor << 8) | patch.
// On kernel 4.14, tracing program types (KPROBE/TRACEPOINT/PERF_EVENT) compare
// attr.kern_version against LINUX_VERSION_CODE when non-zero. Some Android 4.14
// builds reject loads with EACCES when kern_version is left at zero.
// ---------------------------------------------------------------------------
static uint32_t computeKernelVersionCode() {
    struct utsname buf;
    if (uname(&buf) != 0) return 0;
    int major = 0, minor = 0, patch = 0;
    if (sscanf(buf.release, "%d.%d.%d", &major, &minor, &patch) >= 2)
        return (uint32_t)((major << 16) | (minor << 8) | (patch & 0xFF));
    return 0;
}

// ============================================================================
// #24: Parse /sys/devices/system/cpu/online mask (e.g. "0-1,4-5") into the
// sorted list of physical CPU IDs. PERCPU_ARRAY slots and perf_event_open(2)
// are indexed by physical CPU id, NOT by online ordinal. On sparse online sets
// iterating 0..ncpus-1 opens rings for offline cores and misses online
// high-numbered cores.
//
// This is a PURE function (no sysfs reads) so it can be unit-tested on host.
// Returns the list in ascending order. Empty string / unparseable -> empty
// vector (caller falls back to 0..n-1).
// ============================================================================
std::vector<int> parseCpuOnlineMask(const std::string& text) {
    std::vector<int> out;
    if (text.empty()) return out;
    // The sysfs line may have trailing whitespace/newline; strtok_r handles it.
    char* work = strdup(text.c_str());
    if (!work) return out;
    char* saveptr = nullptr;
    for (char* tok = strtok_r(work, ",", &saveptr); tok;
         tok = strtok_r(nullptr, ",", &saveptr)) {
        // skip leading spaces
        while (*tok == ' ' || *tok == '\t') tok++;
        int lo = 0, hi = 0;
        if (strchr(tok, '-')) {
            if (sscanf(tok, "%d-%d", &lo, &hi) == 2) {
                if (lo > hi) { int t = lo; lo = hi; hi = t; }
                for (int c = lo; c <= hi; c++) out.push_back(c);
            }
        } else {
            if (sscanf(tok, "%d", &lo) == 1) out.push_back(lo);
        }
    }
    free(work);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Read /sys/devices/system/cpu/online and return the online CPU id list.
// Falls back to 0..ncpus-1 on sysfs read failure.
std::vector<int> readOnlineCpuIds() {
    std::vector<int> fallback;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 4;
    for (long i = 0; i < n; i++) fallback.push_back((int)i);

    FILE* f = fopen("/sys/devices/system/cpu/online", "re");
    if (!f) return fallback;
    char line[128] = {0};
    if (!fgets(line, sizeof(line), f)) { fclose(f); return fallback; }
    fclose(f);
    std::vector<int> ids = parseCpuOnlineMask(line);
    if (ids.empty()) return fallback;
    return ids;
}

// ============================================================================
// Environment detection
// ============================================================================
static void printSelinuxContext() {
    char ctx[256] = {0};
    int fd = open("/proc/self/attr/current", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, ctx, sizeof(ctx) - 1);
        close(fd);
        if (n > 0) {
            ctx[n] = '\0';
            while (n > 0 && (ctx[n-1] == '\n' || ctx[n-1] == '\0')) n--;
            ctx[n] = '\0';
            LOG_INFO("SELinux context: %s", ctx);
        }
    }
    fd = open("/sys/fs/selinux/enforce", O_RDONLY);
    if (fd >= 0) {
        char buf[4] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) LOG_INFO("SELinux enforce=%c (detect-only)", buf[0]);
    }
}

static void printPerfEventParanoid() {
    std::ifstream f("/proc/sys/kernel/perf_event_paranoid");
    if (f.is_open()) {
        int val = -99; f >> val; f.close();
        LOG_INFO("perf_event_paranoid=%d", val);
    }
    std::ifstream f2("/proc/sys/kernel/unprivileged_bpf_disabled");
    if (f2.is_open()) {
        int val = -99; f2 >> val; f2.close();
        LOG_INFO("unprivileged_bpf_disabled=%d", val);
    }
}

static void printCapabilities() {
    std::ifstream f("/proc/self/status");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("CapEff:", 0) == 0 || line.rfind("CapPrm:", 0) == 0)
            LOG_INFO("%s", line.c_str());
    }
    f.close();
    LOG_INFO("uid=%d euid=%d gid=%d egid=%d",
                getuid(), geteuid(), getgid(), getegid());
}

// ============================================================================
// #48/#49 eBPF least-privilege capability detection (round-2)
// ----------------------------------------------------------------------------
// Instead of assuming the daemon can do everything (load tracepoint progs,
// kprobes, ringbuf maps, perf-event attach), we PROBE each operation once and
// record the actually-working set. The loader then only attempts the subset it
// detected as available — this is the userspace mirror of "only open the BPF
// permissions you need". Results are written to STATE_DIR/ebpf_capabilities
// (EBPF-14: STATE_DIR global, not a hardcoded /dev/aatpe/state literal)
// so WebUI/SELinux audit can see the minimal required set on this device.
// ============================================================================
#define ATPE_CAP_BIT(x) (1ULL << (x))
static unsigned long long readCapEff() {
    std::ifstream f("/proc/self/status");
    if (!f.is_open()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("CapEff:", 0) == 0) {
            return strtoull(line.substr(7).c_str(), nullptr, 16);
        }
    }
    return 0;
}

static bool probeBpfMapCreateRingbuf() {
    // BPF_MAP_TYPE_RINGBUF = 13 (kernel 5.8+). On the target 4.14 kernel this
    // will fail; we then fall back to perf-event arrays. Probing tells us which
    // path to use rather than assuming.
    union bpf_attr attr; memset(&attr, 0, sizeof(attr));
    attr.map_type = 13;
    attr.key_size = 4;
    attr.value_size = 4;
    attr.max_entries = 4096;
    int fd = (int)syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof(attr));
    if (fd >= 0) { close(fd); return true; }
    return false;
}

void EbpfScheduler::runCapabilityDetection() {
    unsigned long long capEff = readCapEff();
    // Which named capabilities matter for BPF on Android:
    //   CAP_SYS_ADMIN(21) -> BPF_PROG_LOAD / map create on locked-down kernels
    //   CAP_PERFMON(38)   -> perf_event_open attach (5.8+ split cap)
    //   CAP_SYS_PTRACE(19)-> some kprobe attach paths
    bool hasSysAdmin   = (capEff & ATPE_CAP_BIT(21)) != 0;
    bool hasPerfmon    = (capEff & ATPE_CAP_BIT(38)) != 0;
    bool hasSysPtrace  = (capEff & ATPE_CAP_BIT(19)) != 0;
    bool ringbufOk     = probeBpfMapCreateRingbuf();

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"cap_eff\":\"0x%llx\",\"sys_admin\":%d,\"perfmon\":%d,\"sys_ptrace\":%d,"
        "\"ringbuf_map\":%d,\"minimal_set\":\"%s\"}",
        capEff, hasSysAdmin?1:0, hasPerfmon?1:0, hasSysPtrace?1:0, ringbufOk?1:0,
        ringbufOk ? "BPF_MAP_CREATE(RINGBUF),PROG_LOAD,PERF_EVENT"
                  : "BPF_MAP_CREATE(PERF_ARRAY),PROG_LOAD,PERF_EVENT");
    // Best-effort state file. Use the daemon's STATE_DIR global + the atomic
    // tmp+fsync+rename writer so a crash mid-write can never leave a torn
    // ebpf_capabilities file (P3-2). If it is not writable (host test / read-only
    // mount) writeFileAtomic returns false and we silently skip — the LOG_INFO
    // line still reports the detected set.
    writeFileAtomic((std::string(STATE_DIR) + "/ebpf_capabilities").c_str(), buf);
    LOG_INFO("eBPF capability detection: capEff=0x%llx sys_admin=%d perfmon=%d ringbuf=%d",
             capEff, hasSysAdmin?1:0, hasPerfmon?1:0, ringbufOk?1:0);
}

static bool loadTimeoutExceeded(
        const std::chrono::steady_clock::time_point& deadline) {
    return std::chrono::steady_clock::now() >= deadline;
}

int EbpfScheduler::readKernelVersion() {
    struct utsname buf;
    if (uname(&buf) != 0) return 0;
    int major = 0, minor = 0;
    if (sscanf(buf.release, "%d.%d", &major, &minor) == 2)
        return major * 100 + minor;
    return 0;
}

bool EbpfScheduler::checkDebugfsTracefs() {
    static const char* paths[] = {
        "/sys/kernel/debug/tracing",
        "/sys/kernel/tracing",
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            LOG_INFO("tracefs available at %s", paths[i]);
            return true;
        }
    }
    LOG_WARN("no tracefs/debugfs tracing mount found");
    return false;
}

void EbpfScheduler::detectEnvironment() {
    int kv = readKernelVersion();
    LOG_INFO("kernel version: %d.%d (code=%d)", kv / 100, kv % 100, kv);
    checkDebugfsTracefs();
    printSelinuxContext();
    printPerfEventParanoid();
    printCapabilities();
    // #48/#49: least-privilege capability probe + state file.
    runCapabilityDetection();
}

// ============================================================================
// BPF syscall wrapper
// ============================================================================
// --- G2 (v4.4.9) host fault-injection seam ---------------------------------
// On a host without real BPF we still must exercise the loader's failure
// branches (prog/map create succeeds, then attach/ringbuf fails; or bpf()
// returns ENOSYS/EPERM). These flags ONLY take effect when AATPE_HOST_TEST is
// defined. Release builds (NDK, no such macro) compile the entire block and the
// call sites inside bpfSyscall() out, so no seam symbol survives in the .so.
#ifdef AATPE_HOST_TEST
// bit0: fake BPF_PROG_LOAD/BPF_MAP_CREATE success (proceed past host probe)
// bit1: force every bpf() to fail with ENOSYS
// bit2: force every bpf() to fail with EPERM
// bit3: report caps present (skip capget preflight)
// bit4: force startPollThread() to return false (EBPF-04 test seam)
static std::atomic<uint32_t>& hostBpfInjectFlags() {
    static std::atomic<uint32_t> f{0};
    return f;
}
// G2: on a non-root host the capget preflight reports no caps and bails at
// NO_CAP before the bpf() probe runs. This host-only flag makes the capget
// capability check report "caps present" so the test can reach the probe / map
// / attach / ringbuf failure branches. Release compiles it out.
static std::atomic<bool> hostTestFakeCaps{false};
static std::atomic<bool> hostTestForcePollThreadFail{false};
extern "C" void aatpe_host_test_set_bpf_inject(uint32_t flags) {
    hostBpfInjectFlags().store(flags, std::memory_order_relaxed);
    hostTestFakeCaps.store((flags & 0x8u) != 0, std::memory_order_relaxed);
    hostTestForcePollThreadFail.store((flags & 0x10u) != 0, std::memory_order_relaxed);
}
#endif

int EbpfScheduler::bpfSyscall(int cmd, void* attr, unsigned int size) {
#ifdef AATPE_HOST_TEST
    uint32_t inj = hostBpfInjectFlags().load(std::memory_order_relaxed);
    if (inj & 0x2u) { errno = ENOSYS; return -1; }
    if (inj & 0x4u) { errno = EPERM;  return -1; }
    if (inj & 0x1u) {
        if (cmd == BPF_PROG_LOAD || cmd == BPF_MAP_CREATE) return 196608;
    }
#endif
    return (int)syscall(__NR_bpf, cmd, attr, size);
}

int EbpfScheduler::createMap(uint32_t type, uint32_t keySize,
                              uint32_t valueSize, uint32_t maxEntries) {
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = type;
    attr.key_size = keySize;
    attr.value_size = valueSize;
    attr.max_entries = maxEntries;
    return bpfSyscall(BPF_MAP_CREATE, &attr, sizeof(attr));
}

// F-03 v4.5.0: pure decision for the wakeup_cache map-type fallback.
// lruFd    = result of createMap(BPF_MAP_TYPE_LRU_HASH, ...)
// lruErrno = errno observed at that point.
//   returns  0 -> accept the LRU fd (kernel supports LRU_HASH; use the
//                 in-kernel auto-reaper).
//   returns  1 -> retry plain BPF_MAP_TYPE_HASH (this kernel rejected the LRU
//                 map type; HASH restores the v4.4.9 bounded-8192 behavior).
//   returns -1 -> hard fail; retrying HASH cannot help (bpf() disabled / no
//                 caps / out of memory).
// Kept free of syscalls so it is host-unit-testable without a real BPF map.
static int wakeupCacheMapDecision(int lruFd, int lruErrno) {
    if (lruFd >= 0) return 0;
    if (lruErrno == EINVAL) return 1;   // "invalid map type" -> HASH may exist
    return -1;                          // ENOSYS/EPERM/ENOMEM -> HASH fails too
}
#ifdef AATPE_HOST_TEST
extern "C" int aatpe_wakeup_map_decision_for_test(int lruFd, int lruErrno) {
    return wakeupCacheMapDecision(lruFd, lruErrno);
}
// F-03 v4.5.0 regression lock: expose the map-type constants so the host test
// can assert LRU_HASH==9 (kernel UAPI) and PROG_ARRAY(3) is NOT used, and that
// the regenerated embedded header agrees. Release compiles these out.
extern "C" int aatpe_wakeup_preferred_type_for_test() { return BPF_MAP_TYPE_LRU_HASH; }
extern "C" int aatpe_wakeup_embedded_type_for_test()   { return kEmbeddedMaps[2].type; }
#endif

// ============================================================================
// Load BPF program from raw bytes with multi-map FD patching
// ============================================================================
int EbpfScheduler::loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                                         const char* license,
                                         const std::vector<int>& mapFds,
                                         int progType) {
    if (!bytes || len == 0 || len % 8 != 0) return -1;

    // v94 fix(P42): local vector — no shared state between nested calls.
    std::vector<uint8_t> patched;
    patched.assign(bytes, bytes + len);

    // Patch all LD_IMM64 PSEUDO_MAP_FD (code=0x18, src_reg=1) instructions.
    // Sequential patching: mapFds[0], mapFds[1], ... in order of appearance.
    // NOTE: This is only safe when the caller guarantees mapFds order matches
    // the actual reference order. For precompiled ELF, use the overload with
    // explicit mapIndices (see parseElfPrograms relocation parsing).
    size_t mapRefIdx = 0;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint8_t code = patched[i];
        uint8_t src_reg = (patched[i + 1] >> 4) & 0x0F;
        if (code == 0x18 && src_reg == 1) {
            // G4-004: never reuse mapFds[0] for an out-of-range reference. A
            // mismatched PSEUDO_MAP_FD makes the verifier reject on a confusing
            // path; fail the load so the caller falls through to the next phase.
            int fd = (mapRefIdx < mapFds.size()) ? mapFds[mapRefIdx] : -1;
            if (fd < 0) {
                LOG_WARN("loadBpfProgFromBytes(sequential): unresolved map reference "
                         "#%zu (mapFds=%zu) — refusing to patch", mapRefIdx, mapFds.size());
                return -1;
            }
            {
                // G4-005: memcpy instead of a misaligned int32_t* write (vector<uint8_t>
                // has alignment 1; direct int32 deref is UB on a strict compiler).
                int32_t imm = fd;
                std::memcpy(patched.data() + i + 4, &imm, sizeof(imm));
                // Clear high 32 bits (defensive: map FDs fit in 32 bits).
                if (i + 16 <= len) {
                    int32_t immHi = 0;
                    std::memcpy(patched.data() + i + 12, &immHi, sizeof(immHi));
                }
            }
            mapRefIdx++;
            i += 8;
            if (i + 8 > len) break;
        }
    }

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = (uint32_t)progType;
    attr.insn_cnt = (uint32_t)(len / sizeof(struct bpf_insn));
    attr.insns = (uint64_t)(unsigned long)patched.data();
    attr.license = (uint64_t)(unsigned long)license;
    attr.kern_version = computeKernelVersionCode();
    // Clear log buffer before each load to avoid stale verifier output from
    // previous attempts (critical for accurate error diagnosis on 4.14).
    memset(bpfLogBuf, 0, sizeof(bpfLogBuf));
    attr.log_buf = (uint64_t)(unsigned long)bpfLogBuf;
    attr.log_size = kBpfLogSize;  // P2-6: kernel writes <=65536 bytes; byte[65536] stays NUL
    attr.log_level = 1;
    int fd = bpfSyscall(BPF_PROG_LOAD, &attr, sizeof(attr));
    if (fd < 0) {
        if (bpfLogBuf[0] != '\0') {
            LOG_WARN("BPF_PROG_LOAD failed (type=%d) errno=%d (%s): %s", progType, errno, aatpeErrStr(errno), bpfLogBuf);
        } else {
            LOG_WARN("BPF_PROG_LOAD failed (type=%d) errno=%d (%s) [no verifier log — kernel rejected before verifier, likely unsupported prog_type or missing CONFIG_BPF_EVENTS]",
                     progType, errno, aatpeErrStr(errno));
        }
    }
    return fd;
}

// Load BPF program with explicit per-reference map indices.
// mapIndices[i] = index into mapFds for the i-th PSEUDO_MAP_FD reference.
// This correctly handles programs where references are not in map order
// (e.g. [events, counters, counters] vs sequential [events, counters, cache]).
int EbpfScheduler::loadBpfProgFromBytesWithRefs(const uint8_t* bytes, size_t len,
                                                  const char* license,
                                                  const std::vector<int>& mapFds,
                                                  const std::vector<int>& mapIndices,
                                                  int progType) {
    if (!bytes || len == 0 || len % 8 != 0) return -1;

    // v94 fix(P42): local vector — no shared state between nested calls.
    std::vector<uint8_t> patched;
    patched.assign(bytes, bytes + len);

    size_t mapRefIdx = 0;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint8_t code = patched[i];
        uint8_t src_reg = (patched[i + 1] >> 4) & 0x0F;
        if (code == 0x18 && src_reg == 1) {
            int fd = -1;
            if (mapRefIdx < mapIndices.size()) {
                int idx = mapIndices[mapRefIdx];
                if (idx >= 0 && idx < (int)mapFds.size()) fd = mapFds[idx];
            }
            // LED-017 (R2-002): never patch an unknown/unresolved map reference to
            // mapFds[0] (the events_map, PERF_EVENT_ARRAY). Doing so makes the
            // verifier reject the program (map type/value mismatch) on a confusing
            // path. Instead fail the load so the caller can try the next objPath
            // or fall back to polling.
            if (fd < 0) {
                LOG_WARN("loadBpfProgFromBytesWithRefs: unresolved map reference #%zu (idx list size=%zu, mapFds=%zu) — refusing to patch to events_map",
                         mapRefIdx, mapIndices.size(), mapFds.size());
                return -1;
            }
            {
                // G4-005: memcpy instead of a misaligned int32_t* write.
                int32_t imm = fd;
                std::memcpy(patched.data() + i + 4, &imm, sizeof(imm));
                // Clear high 32 bits (defensive: map FDs fit in 32 bits).
                if (i + 16 <= len) {
                    int32_t immHi = 0;
                    std::memcpy(patched.data() + i + 12, &immHi, sizeof(immHi));
                }
            }
            mapRefIdx++;
            i += 8;
            if (i + 8 > len) break;
        }
    }

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = (uint32_t)progType;
    attr.insn_cnt = (uint32_t)(len / sizeof(struct bpf_insn));
    attr.insns = (uint64_t)(unsigned long)patched.data();
    attr.license = (uint64_t)(unsigned long)license;
    attr.kern_version = computeKernelVersionCode();
    memset(bpfLogBuf, 0, sizeof(bpfLogBuf));
    attr.log_buf = (uint64_t)(unsigned long)bpfLogBuf;
    attr.log_size = kBpfLogSize;  // P2-6: kernel writes <=65536 bytes; byte[65536] stays NUL
    attr.log_level = 1;
    int fd = bpfSyscall(BPF_PROG_LOAD, &attr, sizeof(attr));
    if (fd < 0) {
        if (bpfLogBuf[0] != '\0') {
            LOG_WARN("BPF_PROG_LOAD (refs) failed (type=%d) errno=%d (%s): %s", progType, errno, aatpeErrStr(errno), bpfLogBuf);
        } else {
            LOG_WARN("BPF_PROG_LOAD (refs) failed (type=%d) errno=%d (%s) [no verifier log]",
                     progType, errno, aatpeErrStr(errno));
        }
    }
    return fd;
}

int EbpfScheduler::loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                                         const char* license, int mapFd,
                                         int progType) {
    std::vector<int> fds;
    if (mapFd >= 0) fds.push_back(mapFd);
    return loadBpfProgFromBytes(bytes, len, license, fds, progType);
}

int EbpfScheduler::loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                                         const char* license, int mapFd) {
    int progType = (mapFd < 0) ? BPF_PROG_TYPE_SOCKET_FILTER : BPF_PROG_TYPE_TRACEPOINT;
    return loadBpfProgFromBytes(bytes, len, license, mapFd, progType);
}

// ============================================================================
// Load BPF program with explicit (byte_offset, map_index) relocation pairs.
// This is REQUIRED for legacy bpf_map_def bytecode where the compiler
// generates LD_IMM (0x18) with src_reg=0 instead of PSEUDO_MAP_FD
// (src_reg=1). The sequential scanner in loadBpfProgFromBytes only matches
// src_reg=1, so it silently fails to patch legacy bytecode — the map FD
// stays at 0 and the verifier rejects with "R2 type=inv expected=map_ptr".
//
// Here we patch BOTH the src_reg field (set to 1 = BPF_PSEUDO_MAP_FD) AND
// the 32-bit immediate field at each ref's byte offset. Without setting
// src_reg=1, the kernel verifier treats the loaded register as an ordinary
// immediate (inv) rather than a map pointer, causing "R2 type=inv
// expected=map_ptr" even when the FD value is correct.
//
// BPF instruction byte layout (8 bytes per word, LD_IMM uses 2 words = 16B):
//   word1: [code(1)][regs(1)][off(2)][imm_lo(4)]
//   word2: [0...][imm_hi(4 at +12)]
//   regs byte: dst_reg(lo 4 bits) | src_reg(hi 4 bits)
// We preserve dst_reg, set src_reg=1, write imm_lo=fd, clear imm_hi=0.
// ============================================================================
int EbpfScheduler::loadBpfProgFromBytesWithExplicitRefs(
    const uint8_t* bytes, size_t len, const char* license,
    const int* refOffsets, const int* refMapIndices, int refCount,
    const std::vector<int>& mapFds, int progType) {
    if (!bytes || len == 0 || len % 8 != 0) return -1;

    // v94 fix(P42): local vector — no shared state between nested calls.
    std::vector<uint8_t> patched;
    patched.assign(bytes, bytes + len);

    int patchedCount = 0;
    for (int ri = 0; ri < refCount; ri++) {
        int off = refOffsets[ri];
        int mapIdx = refMapIndices[ri];
        if (off < 0 || (size_t)(off + 8) > len) {
            LOG_WARN("explicit-ref: ref[%d] offset=%d out of range (len=%zu), skipping",
                     ri, off, len);
            continue;
        }
        if (mapIdx < 0 || mapIdx >= (int)mapFds.size()) {
            LOG_WARN("explicit-ref: ref[%d] mapIdx=%d out of range (mapFds.size=%zu), skipping",
                     ri, mapIdx, mapFds.size());
            continue;
        }
        int fd = mapFds[mapIdx];
        if (fd < 0) {
            LOG_WARN("explicit-ref: ref[%d] mapIdx=%d has invalid fd=%d, skipping",
                     ri, mapIdx, fd);
            continue;
        }
        // Verify this is an LD_IMM (0x18) instruction before patching.
        uint8_t op = patched[off];
        if (op != 0x18) {
            LOG_WARN("explicit-ref: ref[%d] offset=%d expected op=0x18 but got 0x%02x, skipping",
                     ri, off, op);
            continue;
        }
        // CRITICAL: Set src_reg = BPF_PSEUDO_MAP_FD (1) so the verifier
        // treats the destination register as a map_ptr, not an inv.
        // Legacy bpf_map_def bytecode has src_reg=0; without this fix the
        // verifier rejects with "R2 type=inv expected=map_ptr".
        uint8_t regs = patched[off + 1];
        uint8_t dst_reg = regs & 0x0F;
        patched[off + 1] = static_cast<uint8_t>(dst_reg | (1u << 4));

        // Patch the low 32-bit immediate at offset + 4 (map FD value).
        // G4-005: memcpy instead of a misaligned int32_t* write.
        {
            int32_t imm = fd;
            std::memcpy(patched.data() + off + 4, &imm, sizeof(imm));
        }

        // Clear the high 32 bits (second instruction word at +12).
        // Map FDs are positive 32-bit integers, so imm_hi must be 0.
        if ((size_t)(off + 16) <= len) {
            int32_t immHi = 0;
            std::memcpy(patched.data() + off + 12, &immHi, sizeof(immHi));
        }
        patchedCount++;
    }
    LOG_INFO("explicit-ref: patched %d/%d map references (prog_type=%d)",
             patchedCount, refCount, progType);

    // Load with empty mapFds so the sequential scanner doesn't double-patch
    // (it won't find src_reg=1 anyway in legacy bytecode).
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = (uint32_t)progType;
    attr.insn_cnt = (uint32_t)(len / sizeof(struct bpf_insn));
    attr.insns = (uint64_t)(unsigned long)patched.data();
    attr.license = (uint64_t)(unsigned long)license;
    attr.kern_version = computeKernelVersionCode();
    memset(bpfLogBuf, 0, sizeof(bpfLogBuf));
    attr.log_buf = (uint64_t)(unsigned long)bpfLogBuf;
    attr.log_size = kBpfLogSize;  // P2-6: kernel writes <=65536 bytes; byte[65536] stays NUL
    attr.log_level = 1;
    int fd = bpfSyscall(BPF_PROG_LOAD, &attr, sizeof(attr));
    if (fd < 0) {
        if (bpfLogBuf[0] != '\0') {
            LOG_WARN("BPF_PROG_LOAD (explicit-refs) failed (type=%d) errno=%d (%s): %s",
                     progType, errno, aatpeErrStr(errno), bpfLogBuf);
        } else {
            LOG_WARN("BPF_PROG_LOAD (explicit-refs) failed (type=%d) errno=%d (%s) [no verifier log]",
                     progType, errno, aatpeErrStr(errno));
        }
    }
    return fd;
}

// ============================================================================
// Tracepoint ID lookup
// ============================================================================
int EbpfScheduler::readTracepointId(const char* tpPath) {
    // tpPath format: "tracepoint/sched/sched_switch"
    // Convert to: /sys/kernel/tracing/events/sched/sched_switch/id
    std::string path(tpPath);
    // Remove "tracepoint/" prefix
    if (path.rfind("tracepoint/", 0) == 0)
        path = path.substr(11);
    // Replace first '/' with "/events/" — actually path is "sched/sched_switch"
    static const char* bases[] = {
        "/sys/kernel/debug/tracing/events/",
        "/sys/kernel/tracing/events/",
        nullptr
    };
    for (int i = 0; bases[i]; i++) {
        std::string full = std::string(bases[i]) + path + "/id";
        std::ifstream f(full);
        if (f.is_open()) {
            int id = 0;
            f >> id;
            f.close();
            if (id > 0) return id;
        }
    }
    return -1;
}

// ============================================================================
// Attach tracepoint with multi-strategy fallback
// ============================================================================
struct AttachStrategy {
    uint32_t size;
    uint64_t sample_period;
    uint32_t wakeup_events;
    uint64_t sample_type;
    bool enable_first;
};

// EBPF-19: strategy 0 passes the COMPILE-TIME native sizeof(struct
// perf_event_attr). On a modern host / modern kernel this is the correct value
// and succeeds immediately (one syscall instead of probing the legacy matrix).
// Old 4.14 kernels reject attr->size LARGER than their own struct with E2BIG,
// in which case the loop simply falls through to the conservative legacy
// matrix below (48/64/80/96/128) — the old hardcoded pe.size=64 for every
// attempt wasted syscalls on modern kernels and relied on sizeof matching 64.
// Trying a smaller size than the kernel knows is always forward-compatible
// (kernel reads only that many bytes, rest stays zero), so the matrix remains
// the 4.14 safety net.
static const AttachStrategy kAttachStrategies[] = {
    { (uint32_t)sizeof(struct perf_event_attr), 1, 1, PERF_SAMPLE_RAW, true },
    { 64, 1, 1, PERF_SAMPLE_RAW, true },
    { 64, 1, 1, 0, true },
    { 64, 1000, 1, PERF_SAMPLE_RAW, true },
    { 64, 0, 1, PERF_SAMPLE_RAW, true },
    { 48, 1, 1, PERF_SAMPLE_RAW, true },
    { 80, 1, 1, PERF_SAMPLE_RAW, true },
    { 96, 1, 1, PERF_SAMPLE_RAW, true },
    { 128, 1, 1, PERF_SAMPLE_RAW, true },
    { 64, 1, 1, PERF_SAMPLE_RAW, false },
    { 64, 1000, 1, PERF_SAMPLE_RAW, false },
    { 64, 1, 0, PERF_SAMPLE_RAW, true },
};
static const int kNumAttachStrategies =
    (int)(sizeof(kAttachStrategies) / sizeof(kAttachStrategies[0]));

bool EbpfScheduler::attachTracepoint(int progFd, int tpId, int cpu, int& outPerfFd) {
    int lastErrno = 0;
    int lastFailStage = 0;  // 0=perf_event_open, 1=ENABLE, 2=SET_BPF, 3=ENABLE(after)
    for (int s = 0; s < kNumAttachStrategies; s++) {
        const AttachStrategy& st = kAttachStrategies[s];
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.type = PERF_TYPE_TRACEPOINT;
        pe.size = st.size;
        pe.config = (uint64_t)tpId;
        pe.sample_period = st.sample_period;
        pe.sample_type = st.sample_type;
        pe.wakeup_events = st.wakeup_events;
        pe.disabled = 1;

        int fd = syscall(__NR_perf_event_open, &pe, -1, cpu, -1, 0);
        if (fd < 0) { lastErrno = errno; lastFailStage = 0; continue; }

        if (st.enable_first) {
            if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
                lastErrno = errno; lastFailStage = 1; close(fd); continue;
            }
            if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, progFd) < 0) {
                lastErrno = errno; lastFailStage = 2;
                ioctl(fd, PERF_EVENT_IOC_DISABLE, 0); close(fd); continue;
            }
        } else {
            if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, progFd) < 0) {
                lastErrno = errno; lastFailStage = 2; close(fd); continue;
            }
            if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
                lastErrno = errno; lastFailStage = 3; close(fd); continue;
            }
        }
        outPerfFd = fd;
        return true;
    }
    const char* stageNames[] = {"perf_event_open", "PERF_EVENT_IOC_ENABLE",
                                "PERF_EVENT_IOC_SET_BPF", "PERF_EVENT_IOC_ENABLE(after)"};
    LOG_WARN("attachTracepoint: ALL %d strategies failed tpId=%d cpu=%d progFd=%d "
             "last_stage=%s errno=%d (%s)",
             kNumAttachStrategies, tpId, cpu, progFd,
             stageNames[lastFailStage], lastErrno, aatpeErrStr(lastErrno));
    return false;
}

// ============================================================================
// v82 PERF-OUTPUT FIX: open a ring-buffer-only perf event using
// PERF_TYPE_SOFTWARE / PERF_COUNT_SW_BPF_OUTPUT.
//
// This is the STANDARD design for BPF perf event output: a software event
// of type PERF_COUNT_SW_BPF_OUTPUT exists SPECIFICALLY so that BPF programs
// can write to a ring buffer without needing a hardware/tracepoint event.
//
// The old v81 code used PERF_TYPE_TRACEPOINT for ring-buffer-only events,
// which on 4.14 kernels causes bpf_perf_event_output() to silently fail
// (all CPU ring buffer heads stay at 0) because the kernel's perf output
// path for tracepoint-type events has issues in that kernel version.
//
// PERF_TYPE_SOFTWARE / PERF_COUNT_SW_BPF_OUTPUT events:
//   - Are designed exclusively for BPF output (config=10)
//   - Do NOT generate samples on their own (only BPF writes to them)
//   - Work correctly on 4.14+ kernels
//   - Must be enabled via PERF_EVENT_IOC_ENABLE before BPF can write
// ============================================================================
bool EbpfScheduler::openRingBufferOnlyEvent(int tpId, int cpu, int& outPerfFd) {
    (void)tpId;  // tpId no longer needed — software events don't use tracepoint config
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_SOFTWARE;
    // EBPF-19: deliberately 64, NOT sizeof(struct perf_event_attr). This is a
    // single-shot open with NO fallback matrix (unlike attachTracepoint()).
    // On the 4.14 target, a compile-time sizeof (128 on modern UAPI headers)
    // would E2BIG against a kernel whose struct is smaller, and there is no
    // second attempt to recover. A 64-byte size is forward-compatible on every
    // kernel: the kernel reads only the first 64 bytes and treats the rest as
    // zero — all fields we care about (type/config/sample_type/wakeup_events)
    // live within those 64 bytes.
    pe.size = 64;  // conservative size for 4.14 compat (see EBPF-19 note above)
    pe.config = PERF_COUNT_SW_BPF_OUTPUT;  // 10 — software event for BPF output
    pe.sample_period = 1;
    pe.sample_type = PERF_SAMPLE_RAW;
    pe.wakeup_events = 1;
    pe.disabled = 1;  // keep disabled until after mmap + events_map update

    int fd = syscall(__NR_perf_event_open, &pe, -1, cpu, -1, 0);
    if (fd < 0) {
        LOG_WARN("openRingBufferOnlyEvent: perf_event_open(PERF_TYPE_SOFTWARE/BPF_OUTPUT) "
                 "failed cpu=%d errno=%d (%s)",
                 cpu, errno, aatpeErrStr(errno));
        return false;
    }
    outPerfFd = fd;
    return true;
}

// ============================================================================
// v82 PERF-OUTPUT FIX: Set up per-CPU ring buffers using PERF_TYPE_SOFTWARE /
// PERF_COUNT_SW_BPF_OUTPUT events for ALL CPUs (including attachCpu).
//
// The tracepoint attach event (attachFd) is used ONLY for SET_BPF attachment —
// it is NOT used as a ring buffer for output. This avoids the 4.14 kernel bug
// where PERF_TYPE_TRACEPOINT events silently fail to receive BPF perf output.
//
// For every CPU:
//   1. Open a PERF_TYPE_SOFTWARE / PERF_COUNT_SW_BPF_OUTPUT event
//   2. mmap the ring buffer
//   3. Update events_map[cpu] = perf_fd
//   4. PERF_EVENT_IOC_ENABLE the event
//
// Only called for progIndex==0 (the first program) because all programs
// share the same events_map.
// ============================================================================
int EbpfScheduler::setupPerCpuRingBuffers(int tpId, int attachFd, int attachCpu,
                                           int ncpus, int progIndex) {
    if (progIndex != 0) return 0;
    int ringsMapped = 0;

    // v82 DIAGNOSTIC: Log events_map FD for map-patching verification
    LOG_INFO("setupPerCpuRingBuffers: v82 PERF_TYPE_SOFTWARE/BPF_OUTPUT mode — "
             "eventsMapFd_=%d attachFd=%d attachCpu=%d ncpus=%d",
             eventsMapFd_, attachFd, attachCpu, ncpus);

    // v82: Create a SEPARATE software output event for EVERY online CPU,
    // including attachCpu. The tracepoint attach event is only for SET_BPF.
    // #24: iterate over real online CPU ids (from /sys/.../cpu/online), not
    // 0..ncpus-1 — on sparse online sets ("0-1,4-5") the latter opens rings for
    // offline cores 2,3 and never reaches online cores 4,5.
    std::vector<int> fallbackCpus;
    if (onlineCpuIds_.empty()) {
        for (int i = 0; i < ncpus; i++) fallbackCpus.push_back(i);
    }
    const std::vector<int>& cpusToUse = onlineCpuIds_.empty() ? fallbackCpus : onlineCpuIds_;
    int totalRings = (int)cpusToUse.size();

    for (int cpu : cpusToUse) {
        int ringFd = -1;
        if (!openRingBufferOnlyEvent(tpId, cpu, ringFd)) {
            LOG_WARN("setupPerCpuRingBuffers: failed to open software output event for cpu=%d", cpu);
            continue;
        }
        tracepointFds_.push_back(ringFd);

        // mmap the ring buffer (this also updates events_map[cpu] = ringFd)
        if (!mmapPerfRingForCpu(cpu, ringFd, ringPages_)) {
            LOG_WARN("setupPerCpuRingBuffers: mmap failed for cpu=%d fd=%d", cpu, ringFd);
            // P2-9: on mmap failure the fd is unusable. Drop it from the tracked
            // list and close it so it is not leaked (and not double-closed later
            // in unload()).
            tracepointFds_.pop_back();
            close(ringFd);
            continue;
        }
        ringsMapped++;

        // Enable the software event AFTER mmap and events_map update.
        // PERF_COUNT_SW_BPF_OUTPUT events must be enabled for BPF to write.
        if (ioctl(ringFd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
            LOG_WARN("setupPerCpuRingBuffers: ENABLE software output event failed "
                     "cpu=%d fd=%d errno=%d (%s)",
                     cpu, ringFd, errno, aatpeErrStr(errno));
        } else {
            LOG_INFO("setupPerCpuRingBuffers: enabled software output event cpu=%d fd=%d",
                     cpu, ringFd);
        }
    }

    LOG_INFO("setupPerCpuRingBuffers: mapped %d/%d CPU ring buffers via PERF_TYPE_SOFTWARE/BPF_OUTPUT "
             "(attachCpu=%d uses independent software event, not tracepoint attach fd)",
             ringsMapped, totalRings, attachCpu);

    // LED-020 (R2-005): publish coverage so logDegradeSummary / EventStats can
    // surface partial ring setup instead of leaving it as a one-off WARN.
    ringsMappedCount_.store(ringsMapped, std::memory_order_relaxed);
    ringsTotalCount_.store(totalRings, std::memory_order_relaxed);
    if (ringsMapped < totalRings) {
        LOG_WARN("setupPerCpuRingBuffers: PARTIAL ring coverage %d/%d — events on %d CPU(s) will be silently dropped by the kernel",
                 ringsMapped, totalRings, totalRings - ringsMapped);
    }

    return ringsMapped;
}

// ============================================================================
// KPROBE event creation: write to kprobe_events
// ============================================================================
int EbpfScheduler::createKprobeEvent(const char* funcName, bool isKretprobe,
                                       std::string& outEventName) {
    static std::atomic<int> s_eventCounter{0};
    int idx = s_eventCounter.fetch_add(1);
    char eventName[128];
    snprintf(eventName, sizeof(eventName), "aatpe_%s_%d", funcName, idx);
    outEventName = eventName;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s:%s %s",
             isKretprobe ? "r" : "p", eventName, funcName);

    static const char* kprobePaths[] = {
        "/sys/kernel/debug/tracing/kprobe_events",
        "/sys/kernel/tracing/kprobe_events",
        nullptr
    };

    for (int i = 0; kprobePaths[i]; i++) {
        int fd = open(kprobePaths[i], O_WRONLY | O_APPEND);
        if (fd < 0) {
            LOG_INFO("kprobe: %s not accessible (errno=%d %s), trying next",
                     kprobePaths[i], errno, aatpeErrStr(errno));
            continue;
        }
        ssize_t n = write(fd, cmd, strlen(cmd));
        int savedErrno = errno;
        close(fd);
        if (n > 0) {
            LOG_INFO("kprobe: created event '%s' via %s (cmd='%s')",
                     eventName, kprobePaths[i], cmd);
            return 0;
        }
        LOG_WARN("kprobe: write to %s failed errno=%d (%s) cmd='%s'",
                 kprobePaths[i], savedErrno, aatpeErrStr(savedErrno), cmd);
    }
    LOG_ERROR("kprobe: failed to create event for func=%s (all paths exhausted)", funcName);
    return -1;
}

// ============================================================================
// Read kprobe event ID from tracefs
// ============================================================================
int EbpfScheduler::readKprobeEventId(const std::string& eventName) {
    static const char* bases[] = {
        "/sys/kernel/debug/tracing/events/kprobes/",
        "/sys/kernel/tracing/events/kprobes/",
        nullptr
    };
    for (int i = 0; bases[i]; i++) {
        std::string path = std::string(bases[i]) + eventName + "/id";
        std::ifstream f(path);
        if (f.is_open()) {
            int id = 0;
            f >> id;
            f.close();
            if (id > 0) {
                LOG_INFO("kprobe: event '%s' id=%d (%s)", eventName.c_str(), id, path.c_str());
                return id;
            }
            LOG_WARN("kprobe: event '%s' id file read 0/invalid (%s)", eventName.c_str(), path.c_str());
        }
    }
    LOG_WARN("kprobe: event id not found for '%s' errno=%d (%s)",
             eventName.c_str(), errno, aatpeErrStr(errno));
    return -1;
}

// ============================================================================
// P2-4: Ensure the kprobe debugfs event for (funcName, isKretprobe) exists and
// resolve its id ONCE. Per-CPU attachKprobe() calls reuse the cached kpId via
// attachTracepoint() instead of re-creating a duplicate kprobe entry per CPU
// (the old code created N identical kprobe_events rows, one per online CPU).
// ============================================================================
int EbpfScheduler::ensureKprobeEvent(const char* funcName, bool isKretprobe,
                                     std::string& outEventName) {
    std::string key = (isKretprobe ? std::string("r:") : std::string("p:")) + funcName;
    auto it = kprobeIdCache_.find(key);
    if (it != kprobeIdCache_.end()) {
        outEventName = it->second.first;
        return it->second.second;
    }

    std::string eventName;
    if (createKprobeEvent(funcName, isKretprobe, eventName) != 0) {
        LOG_WARN("kprobe ensure: create event failed for func=%s", funcName);
        return -1;
    }
    int kpId = readKprobeEventId(eventName);
    if (kpId <= 0) {
        LOG_WARN("kprobe ensure: event id not found for %s", eventName.c_str());
        return -1;
    }
    // Track the debugfs name for cleanupKprobeEvents() (once, not per-CPU).
    kprobeEventNames_.push_back(eventName);
    kprobeIdCache_[key] = {eventName, kpId};
    outEventName = eventName;
    LOG_FORCE("kprobe ensure: created event=%s kpId=%d (shared across all CPUs)",
              eventName.c_str(), kpId);
    return kpId;
}

// ============================================================================
// Attach KPROBE: ensure event (cached, once) -> perf_event_open + SET_BPF -> ENABLE
// ============================================================================
bool EbpfScheduler::attachKprobe(int progFd, const char* funcName, bool isKretprobe,
                                  int cpu, int& outPerfFd, std::string& outEventName) {
    LOG_FORCE("kprobe attach: func=%s is_ret=%d cpu=%d progFd=%d",
             funcName, isKretprobe, cpu, progFd);

    // Step 1: ensure the kprobe debugfs event exists (created once per funcName,
    // deduped in kprobeIdCache_). Per-CPU callers reuse the same kpId.
    int kpId = ensureKprobeEvent(funcName, isKretprobe, outEventName);
    if (kpId <= 0) {
        LOG_WARN("kprobe attach: ensure event failed for func=%s cpu=%d", funcName, cpu);
        return false;
    }

    // Step 2: perf_event_open + SET_BPF + enable on the shared kpId.
    if (!attachTracepoint(progFd, kpId, cpu, outPerfFd)) {
        LOG_WARN("kprobe attach: perf_event_open/SET_BPF failed for %s cpu=%d (kpId=%d)",
                 outEventName.c_str(), cpu, kpId);
        return false;
    }

    LOG_FORCE("kprobe attach: SUCCESS func=%s event=%s cpu=%d perfFd=%d kpId=%d",
             funcName, outEventName.c_str(), cpu, outPerfFd, kpId);
    return true;
}

// ============================================================================
// Cleanup all created kprobe events
// ============================================================================
void EbpfScheduler::cleanupKprobeEvents() {
    if (kprobeEventNames_.empty()) return;
    static const char* kprobePaths[] = {
        "/sys/kernel/debug/tracing/kprobe_events",
        "/sys/kernel/tracing/kprobe_events",
        nullptr
    };
    for (const auto& name : kprobeEventNames_) {
        std::string cmd = "-:" + name;
        for (int i = 0; kprobePaths[i]; i++) {
            int fd = open(kprobePaths[i], O_WRONLY | O_APPEND);
            if (fd < 0) continue;
            // P3: check the short-write / failure return so we don't silently
            // leave a kprobe armed on a path that rejected the "-:" detach.
            ssize_t w = write(fd, cmd.c_str(), cmd.size());
            if (w < 0) {
                LOG_WARN("kprobe cleanup: detach write to %s failed errno=%d (%s)",
                         kprobePaths[i], errno, aatpeErrStr(errno));
            }
            close(fd);
            break;
        }
    }
    LOG_INFO("kprobe: cleaned up %zu events", kprobeEventNames_.size());
    kprobeEventNames_.clear();
}
// ============================================================================
// Update events_map (BPF_MAP_TYPE_PERF_EVENT_ARRAY) with retry-on-update-failure.
//
// ROOT CAUSE OF PREVIOUS BUG (v80 lookup-verify false positive):
//   On some Android 4.14 kernels, BPF_MAP_LOOKUP_ELEM on a PERF_EVENT_ARRAY
//   returns -ENOTSUPP (errno=524) because the kernel's perf_event_array_map_ops
//   does NOT implement .map_lookup_elem (callback is NULL). This is NOT a sign
//   that the update failed — the .map_update_elem callback works fine and stores
//   the perf event pointer correctly. User-space lookup of PERF_EVENT_ARRAY is
//   also semantically useless (it would return a kernel virtual address that
//   means nothing to user space).
//
//   The v80 code treated "lookup returns ENOTSUPP" as "kernel silently dropped
//   the entry", retried 3 times, and reported FAILED. This was a false positive:
//   the map was actually updated successfully, and bpf_perf_event_output() could
//   write events. The misleading "kernel PERF_EVENT_ARRAY bug" error caused
//   downstream confusion.
//
// CORRECT BEHAVIOR:
//   - Trust BPF_MAP_UPDATE_ELEM's return value. If it returns 0, the entry is
//     stored. Retry only if update itself fails (returns -1).
//   - Do NOT call BPF_MAP_LOOKUP_ELEM on PERF_EVENT_ARRAY for verification.
//   - If a lookup is ever attempted for diagnostics and returns ENOTSUPP, log
//     it as "expected on this kernel" not as an error.
//   - The real diagnostic for "are BPF programs firing?" is the event_counters
//     PERCPU_ARRAY map, which DOES support user-space lookup.
// ============================================================================
bool EbpfScheduler::updateEventsMapWithVerify(int cpu, int perfFd) {
    if (eventsMapFd_ < 0) {
        LOG_WARN("updateEventsMap: eventsMapFd_ invalid, cannot update cpu=%d", cpu);
        return false;
    }
    if (perfFd < 0) {
        LOG_WARN("updateEventsMap: invalid perfFd=%d for cpu=%d", perfFd, cpu);
        return false;
    }

    const int kMaxRetries = 3;
    for (int attempt = 0; attempt < kMaxRetries; attempt++) {
        int key = cpu;
        union bpf_attr ma;
        memset(&ma, 0, sizeof(ma));
        ma.map_fd = (uint32_t)eventsMapFd_;
        ma.key = (uint64_t)(unsigned long)&key;
        ma.value = (uint64_t)(unsigned long)&perfFd;
        int ret = bpfSyscall(BPF_MAP_UPDATE_ELEM, &ma, sizeof(ma));
        if (ret == 0) {
            // Update succeeded. The entry is stored. We deliberately do NOT
            // verify via BPF_MAP_LOOKUP_ELEM because PERF_EVENT_ARRAY may not
            // support user-space lookup (ENOTSUPP / errno=524 on some 4.14
            // kernels), and even when supported the returned value is a
            // kernel virtual address that is meaningless to user space.
            if (attempt > 0) {
                LOG_INFO("updateEventsMap: events_map[%d]=%d OK (after %d retries)",
                         cpu, perfFd, attempt);
            }
            return true;
        }

        int savedErrno = errno;
        LOG_WARN("updateEventsMap: update FAILED cpu=%d fd=%d attempt=%d errno=%d (%s) — retrying",
                 cpu, perfFd, attempt, savedErrno, aatpeErrStr(savedErrno));
        // Brief backoff before retry
        if (attempt < kMaxRetries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    LOG_ERROR("updateEventsMap: events_map[%d]=%d FAILED after %d update attempts "
              "(bpf_perf_event_output will drop events for this CPU)",
              cpu, perfFd, kMaxRetries);
    return false;
}

bool EbpfScheduler::mmapPerfRingForCpu(int cpu, int perfFd, size_t pageCount) {
    if (perfFd < 0) return false;
    if (pageCount == 0) pageCount = ringPages_;
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;
    size_t mmapSize = (pageCount + 1) * (size_t)pageSize;

    void* base = mmap(nullptr, mmapSize, PROT_READ | PROT_WRITE, MAP_SHARED, perfFd, 0);
    if (base == MAP_FAILED) {
        LOG_WARN("mmapPerfRing cpu=%d: mmap failed errno=%d (%s)", cpu, errno, aatpeErrStr(errno));
        return false;
    }

    PerfRing ring;
    ring.perfFd = perfFd;
    ring.base = base;
    ring.mmapSize = mmapSize;
    ring.cpu = cpu;
    rings_.push_back(ring);

    // Update the events_map with this perf fd for the CPU key.
    // Uses retry-on-update-failure. We do NOT verify via user-space lookup
    // because PERF_EVENT_ARRAY may not support it (ENOTSUPP/errno=524 on some
    // 4.14 kernels), and the returned kernel pointer is meaningless to user space.
    if (eventsMapFd_ >= 0) {
        if (!updateEventsMapWithVerify(cpu, perfFd)) {
            LOG_WARN("mmapPerfRing: events_map[%d]=%d update FAILED "
                     "(bpf_perf_event_output will drop events for this CPU)", cpu, perfFd);
        } else {
            LOG_INFO("mmapPerfRing: events_map[%d]=%d updated OK", cpu, perfFd);
        }
    } else {
        LOG_WARN("mmapPerfRing: eventsMapFd_ invalid, cannot update map for cpu=%d", cpu);
    }

    LOG_INFO("mmapPerfRing: cpu=%d fd=%d mapped %zu bytes (%zu pages)",
                cpu, perfFd, mmapSize, pageCount);
    return true;
}

bool EbpfScheduler::mmapPerfRing(int perfFd, size_t pageCount) {
    return mmapPerfRingForCpu(0, perfFd, pageCount);
}

void EbpfScheduler::unmapPerfRing() {
    if (perfRingBase_ != nullptr && perfMmapSize_ > 0) {
        munmap(perfRingBase_, (size_t)perfMmapSize_);
    }
    perfRingBase_ = nullptr;
    perfMmapSize_ = 0;
    perfFd_ = -1;
}

void EbpfScheduler::unmapAllRings() {
    // P1-1 fd double-close fix: every ring.perfFd is ALSO recorded in
    // tracepointFds_ (the per-CPU software ring events pushed in
    // setupPerCpuRingBuffers / mmapPerfRingForCpu). Previously this loop closed
    // ring.perfFd here, and unload() / the failure-cleanup loop then closed the
    // SAME fd again from tracepointFds_ — a double-close that could close a
    // recycled fd belonging to an unrelated later open().
    //
    // Policy: unmapAllRings() ONLY munmap()s the ring buffers and resets the
    // local fd bookkeeping; it NEVER close()s the perf fds. The single ownership
    // of every perf fd is tracepointFds_, which is closed exactly once:
    //   * on success-path teardown: by unload()'s `for (fd : tracepointFds_)` loop
    //   * on load-failure paths: by the matching `for (fd : tracepointFds_)` loop
    // after unmapAllRings(). The DISABLE ioctl is kept (harmless on an fd about
    // to be closed) but the close() is removed and ring.perfFd is invalidated so
    // no later code in this function can act on a stale fd.
    for (auto& ring : rings_) {
        if (ring.base != nullptr && ring.mmapSize > 0) {
            munmap(ring.base, ring.mmapSize);
        }
        if (ring.perfFd >= 0) {
            ioctl(ring.perfFd, PERF_EVENT_IOC_DISABLE, 0);
            // do NOT close(ring.perfFd) — owned by tracepointFds_
            ring.perfFd = -1;
        }
        ring.base = nullptr;
        ring.mmapSize = 0;
    }
    rings_.clear();
    unmapPerfRing();
}

// v94 fix(P43): readPerfRing() removed — was an empty legacy stub.
// Actual ring buffer reading is done by processRingData().

// ============================================================================
// Process events from a single CPU ring buffer
// ============================================================================
int EbpfScheduler::processRingData(PerfRing& ring, int maxEvents) {
    if (!ring.base || ring.mmapSize == 0) return 0;

    // v83: cache page size to avoid repeated sysconf() in hot path
    static const long s_pageSize = []() {
        long ps = sysconf(_SC_PAGESIZE);
        return (ps > 0) ? ps : 4096L;
    }();
    const long pageSize = s_pageSize;

    // perf ring layout: [perf_event_mmap_page (1 page)][data pages]
    auto* header = reinterpret_cast<volatile struct perf_event_mmap_page*>(ring.base);
    // P1: arm64 weak memory order. The kernel publishes record data before
    // writing data_head with release semantics; we must ACQUIRE data_head so
    // the record payload below is not reordered ahead of the head read. We own
    // data_tail, but when we finally publish our consumption we must RELEASE it
    // so the kernel (which reads it) sees the record reads completed first.
    uint64_t dataTail = __atomic_load_n(&header->data_tail, __ATOMIC_RELAXED);
    uint64_t dataHead = __atomic_load_n(&header->data_head, __ATOMIC_ACQUIRE);
    uint64_t dataSize = ring.mmapSize - (size_t)pageSize;

    if (dataHead == dataTail) return 0;

    static std::atomic<uint64_t> s_diagLogCounter{0};
    if ((s_diagLogCounter.fetch_add(1) % 64) == 0) {
        LOG_DEBUG("[eBPF-DIAG] processRingData cpu=%d head=%llu tail=%llu diff=%llu dataSize=%llu",
                 ring.cpu, (unsigned long long)dataHead, (unsigned long long)dataTail,
                 (unsigned long long)(dataHead - dataTail), (unsigned long long)dataSize);
    }

    // Check for overflow (lost events)
    // v89 FIX: Previously this computed lost = (dataHead - dataTail) - dataSize
    // which is in BYTES, but ringLost_ was treated as an event count by WebUI.
    // With ~68-byte records this inflated the loss rate by ~68x (e.g. 190%).
    // P2-6: PERF_RECORD_LOST records (parsed in the event loop below) are the
    // PRIMARY source of lost-event accounting — the kernel tells us exactly how
    // many events were dropped. This byte-based overflow estimate is SECONDARY:
    // it catches the case where the kernel's lost counter hasn't been read yet
    // but we can see the head/tail pointer has wrapped. The estimated event
    // count uses the running average record size observed from consumed events.
    // P3-18 (reviewed, kept): when the ring has wrapped we jump dataTail forward
    // to dataHead - dataSize and DO NOT parse the PERF_RECORD_LOST records in
    // the overwritten region. That is intentional: the kernel's own LOST record
    // accounting below is the authoritative source, and the overwritten bytes are
    // already gone. The byte-based estimate above backstops the case where the
    // LOST record itself was overwritten. No change.
    if (dataHead - dataTail > dataSize) {
        uint64_t lostBytes = (dataHead - dataTail) - dataSize;
        uint64_t consumed = eventCountTotal_.load(std::memory_order_relaxed);
        uint64_t avgSize = (consumed > 0)
            ? (ringConsumedBytes_.load(std::memory_order_relaxed) / consumed)
            : 64ULL;  // fallback estimate before any events processed
        if (avgSize < 16) avgSize = 16;  // sanity floor
        uint64_t lostEvents = lostBytes / avgSize;
        // G4-007: do NOT feed the byte-based estimate into the cumulative
        // ringLost_ (the driver of shouldDegrade). The kernel's authoritative
        // PERF_RECORD_LOST record (parsed in the event loop) already accounts the
        // dropped events; adding the estimate there double-counts loss and makes
        // shouldDegrade flap. The estimate is kept only as a windowed diagnostic
        // for the dynamic ring-sizing logic and to stamp lastRingLossMs_.
        windowLost_.fetch_add(lostEvents, std::memory_order_relaxed);
        lastRingLossMs_.store((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        dataTail = dataHead - dataSize;
    }

    int processed = 0;
    uint8_t* dataBase = static_cast<uint8_t*>(ring.base) + pageSize;

    // opt#4: batch SWITCH aggregation. The old code locked
    // switchAggMutex_ ONCE PER sched_switch event. At game-load switch rates
    // (thousands/sec across 8 rings) that is a per-event lock/unlock storm. We now
    // accumulate per-nextPid counts into a thread-local map WITHOUT the lock, then
    // acquire the lock a SINGLE time at the end of this ring drain to merge +
    // decide deliveries. Delivery itself still runs outside the lock (queue push).
    struct LocalSwitchAcc {
        uint32_t count = 0;
        uint64_t lastTs = 0;
        SwitchEvent firstEv;
    };
    std::unordered_map<uint32_t, LocalSwitchAcc> localSwitchBatch;
    localSwitchBatch.reserve(64);  // P3: avoid rehash churn per ring drain
    std::vector<SwitchEvent> pendingSwitchDeliver;

    while (dataTail < dataHead && processed < maxEvents) {
        uint64_t offset = dataTail % dataSize;
        // Ensure we can read the perf_event_header
        if (offset + sizeof(struct perf_event_header) > dataSize) {
            // LED-021 (R2-006): a record that straddles the ring tail in a way that
            // cannot be resolved by the kernel's 8-byte tail padding means the ring
            // content is corrupted (or we have desynced). Previously we advanced
            // only 8 bytes and continued parsing at a bogus offset, which could
            // misinterpret garbage as record sizes and loop badly. Treat this as
            // an unrecoverable desync and drop to dataHead.
            LOG_WARN("processRingData: ring desync at tail offset=%llu size=%llu — dropping to dataHead",
                     (unsigned long long)offset, (unsigned long long)dataSize);
            dataTail = dataHead;
            break;
        }
        auto* eh = reinterpret_cast<struct perf_event_header*>(dataBase + offset);
        uint16_t type = eh->type;
        uint16_t misc = eh->misc;
        uint32_t size = eh->size;

        if (size == 0 || size > dataSize) {
            dataTail = dataHead;
            break;
        }

        if (type == PERF_RECORD_SAMPLE) {
            // Sample layout: [perf_event_header][sample_id (if set)][raw data]
            // With PERF_SAMPLE_RAW: after header comes size(4) + data
            uint64_t sampleOffset = offset + sizeof(struct perf_event_header);
            if (sampleOffset + 4 <= dataSize) {
                uint32_t rawSize = *reinterpret_cast<volatile uint32_t*>(dataBase + sampleOffset);
                uint64_t rawOffset = sampleOffset + 4;
                if (rawSize >= sizeof(EventHeader) && rawOffset + rawSize <= dataSize) {
                    // v4.4.9: the raw record's EventHeader may sit at an offset
                    // that is not 8-byte aligned (the ring layout only guarantees
                    // 4-byte alignment here). Casting to `const EventHeader*` and
                    // dereferencing is misaligned UB on arm64 (UBSan flagged this);
                    // read the type field via memcpy into an aligned local. The
                    // per-event structs below are already memcpy'd the same way.
                    uint32_t evtType = 0;
                    memcpy(&evtType, dataBase + rawOffset, sizeof(evtType));
                    bool validBpfEvent = (evtType >= 1 && evtType <= 5);  // P1-1: RUNQ=5
                    if (!validBpfEvent) {
                        // Not one of our BPF events (could be a regular perf sample
                        // from an enabled ring-buffer-only event). Skip without counting.
                        dataTail += size;
                        continue;
                    }
                    switch (evtType) {
                        case (uint32_t)EventType::FORK: {
                            // P3-8: only count the event when the raw record is
                            // actually large enough to hold the struct; previously
                            // the counter bumped even on truncated/undersized records.
                            if (rawSize >= sizeof(ForkEvent)) {
                                ForkEvent ev;
                                memcpy(&ev, dataBase + rawOffset, sizeof(ForkEvent));
                                // v94 P1-2: route through the single-consumer queue
                                // when the poll thread owns the ring; direct deliver
                                // only in the legacy fallback path.
                                deliverForkEvent(ev);
                                eventCountFork_.fetch_add(1, std::memory_order_relaxed);
                                eventCountTotal_.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case (uint32_t)EventType::EXIT: {
                            if (rawSize >= sizeof(ExitEvent)) {
                                ExitEvent ev;
                                memcpy(&ev, dataBase + rawOffset, sizeof(ExitEvent));
                                deliverExitEvent(ev);
                                eventCountExit_.fetch_add(1, std::memory_order_relaxed);
                                eventCountTotal_.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case (uint32_t)EventType::SWITCH: {
                            if (rawSize >= sizeof(SwitchEvent)) {
                                SwitchEvent ev;
                                memcpy(&ev, dataBase + rawOffset, sizeof(SwitchEvent));
                                // opt#4: accumulate locally (no lock); the
                                // merge + delivery decision happens once per ring drain.
                                auto& acc = localSwitchBatch[ev.nextPid];
                                if (acc.count == 0) acc.firstEv = ev;
                                acc.count++;
                                acc.lastTs = ev.hdr.timestampNs;
                                eventCountSwitch_.fetch_add(1, std::memory_order_relaxed);
                                eventCountTotal_.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case (uint32_t)EventType::EXEC: {
                            if (rawSize >= sizeof(ExecEvent)) {
                                ExecEvent ev;
                                memcpy(&ev, dataBase + rawOffset, sizeof(ExecEvent));
                                deliverExecEvent(ev);
                                eventCountExec_.fetch_add(1, std::memory_order_relaxed);
                                eventCountTotal_.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case (uint32_t)EventType::RUNQ: {
                            // P1-1: runqueue latency. Cheap stat-only delivery
                            // (no /proc I/O, no g_threadsMutex) so we dispatch
                            // directly from the poll thread.
                            if (rawSize >= sizeof(RunqEvent)) {
                                RunqEvent ev;
                                memcpy(&ev, dataBase + rawOffset, sizeof(RunqEvent));
                                deliverRunqEvent(ev);
                                eventCountRunq_.fetch_add(1, std::memory_order_relaxed);
                                eventCountTotal_.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                        default:
                            break;
                    }
                    processed++;
                }
            }
        } else if (type == PERF_RECORD_LOST) {
            // Lost events record
            if (offset + sizeof(struct perf_event_header) + 16 <= dataSize) {
                uint64_t lost = *reinterpret_cast<volatile uint64_t*>(
                    dataBase + offset + sizeof(struct perf_event_header) + 8);
                ringLost_.fetch_add(lost, std::memory_order_relaxed);
                windowLost_.fetch_add(lost, std::memory_order_relaxed);
                lastRingLossMs_.store((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
            }
        }

        dataTail += size;
        // v89: track consumed bytes for accurate average-event-size calculation
        ringConsumedBytes_.fetch_add(size, std::memory_order_relaxed);
        (void)misc;
    }

    // opt#4: merge the accumulated SWITCH batch under ONE lock, then deliver
    // outside the lock (identical semantics to the old per-event path: first
    // occurrence delivered immediately; threshold crossings delivered when count
    // crosses kSwitchAggregateThreshold; lazy eviction preserved).
    if (!localSwitchBatch.empty()) {
        {
            std::lock_guard<std::mutex> aggLock(switchAggMutex_);
            for (auto& kv : localSwitchBatch) {
                uint32_t nextPid = kv.first;
                LocalSwitchAcc& acc = kv.second;
                auto it = switchAgg_.find(nextPid);
                bool deliverNow = false;
                if (it == switchAgg_.end()) {
                    SwitchAggregate agg;
                    agg.nextPid = nextPid;
                    agg.count = acc.count;
                    agg.lastTs = acc.lastTs;
                    memcpy(agg.nextComm, acc.firstEv.nextComm, 16);
                    switchAgg_[nextPid] = agg;
                    deliverNow = true;  // first occurrence
                    if (++switchAggInsertCount_ >= kSwitchAggLazyEvictInserts) {
                        switchAggInsertCount_ = 0;
                        struct timespec lazyTs;
                        clock_gettime(CLOCK_MONOTONIC, &lazyTs);
                        uint64_t lazyNowMs =
                            (uint64_t)lazyTs.tv_sec * 1000ULL +
                            (uint64_t)lazyTs.tv_nsec / 1000000ULL;
                        evictStaleSwitchAggLocked(lazyNowMs);
                    }
                } else {
                    it->second.count += acc.count;
                    it->second.lastTs = acc.lastTs;
                    if (it->second.count >= kSwitchAggregateThreshold) {
                        deliverNow = true;
                        it->second.count = 0;
                    }
                }
                if (deliverNow) pendingSwitchDeliver.push_back(acc.firstEv);
            }
        }
        for (const auto& ev : pendingSwitchDeliver) {
            deliverSwitchEvent(ev);
        }
    }

    // P1: RELEASE store so the kernel sees our record reads before it observes
    // the new data_tail (prevents the kernel overwriting records we just read).
    __atomic_store_n(&header->data_tail, dataTail, __ATOMIC_RELEASE);
    // v90: count processed events toward the current loss-rate window
    if (processed > 0) {
        windowEvents_.fetch_add((uint64_t)processed, std::memory_order_relaxed);
    }
    return processed;
}

#ifdef AATPE_HOST_TEST
// G2 host seam: synthesize a one-page-perf-header + data-page ring in memory and
// drive the REAL processRingData() parser over a caller-supplied blob. No perf fd,
// no tracefs. Marks offsets NO_CTX (trusted) so deliver*Event reaches the consumer.
int EbpfScheduler::hostTestParseRingBlob(const void* data, size_t dataBytes) {
    static const long kPage = 4096;
    if (dataBytes > (size_t(1) << 20)) return -1;  // sanity cap (1 MiB ring)
    offsetStatus_fork_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_switch_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_exec_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_exit_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_wakeup_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);

    std::vector<uint8_t> buf(kPage + dataBytes, 0);
    auto* hdr = reinterpret_cast<struct perf_event_mmap_page*>(buf.data());
    hdr->data_tail = 0;
    hdr->data_head = dataBytes;  // host: single-threaded, plain store is fine
    if (dataBytes && data) memcpy(buf.data() + kPage, data, dataBytes);

    PerfRing ring;
    ring.base = buf.data();
    ring.mmapSize = kPage + dataBytes;
    ring.cpu = 0;
    return processRingData(ring, 1 << 20);
}
#endif

// ============================================================================
// v94 P1-2 / v95 issues 6/7/8/9: event delivery routing (poll-thread side)
//
// When the bounded queues are enabled (poll thread owns the ring), events are
// routed by type:
//   FORK/EXIT/EXEC -> criticalQueue_ (256 slots, high priority) + wake hook
//   SWITCH         -> normalQueue_   (8192 slots, no wake)
// The daemon main loop later runs dispatchQueuedEvents() to invoke the
// consumer callbacks. This keeps the perf ring mmap region and its data_tail
// pointer touched by exactly ONE thread (the poll thread), removing the
// previous dual-consumer race. When the queues are disabled (poll thread
// unavailable / POLLING fallback), we fall back to direct delivery, preserving
// the legacy single-thread behavior.
// ============================================================================
namespace {
// Invoke the critical-event wake hook if the daemon installed one. Called on
// the poll thread immediately after a FORK/EXIT/EXEC is enqueued.
inline void wakeCriticalIfSet(const std::function<void()>& hook) {
    if (!hook) return;
    // v95-CPU-FIX2: throttle main-loop wakeups to max once per 50ms.
    // Fork/exec/exit events can burst (hundreds/sec during game startup),
    // and each wakeup interrupts the main loop's poll() sleep. 50ms = 20Hz
    // is plenty for lifecycle event responsiveness; the event queue itself
    // is unbounded so no events are lost, only the wakeup is coalesced.
    static std::atomic<long long> s_lastWakeMs{0};
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = s_lastWakeMs.load(std::memory_order_relaxed);
    if (nowMs - last < 50) return;
    if (s_lastWakeMs.compare_exchange_strong(last, nowMs, std::memory_order_relaxed)) {
        hook();
    }
}
} // namespace

// v94-r3 fix: per-tracepoint offset-gate for event delivery.
//
// When verifyTracepointOffsets() detects that a tracepoint's hardcoded ctx
// byte offsets do not match the on-device kernel layout, the BPF program would
// still be reading ctx at the wrong bytes and the resulting event would carry
// garbage fields (child_pid, next_pid, prev_comm, ...). The BPF side cannot be
// reattached with corrected offsets (the offsets are baked into the embedded
// bytecode), so we filter in user-space here: events from a DEGRADED
// tracepoint are dropped before they reach the queue or the consumer.
//
// The first time we drop an event for a given tracepoint we emit a single
// WARN so the log shows WHY lifecycle/switch events stopped flowing; after
// that we only bump offsetFilteredCount_ (WebUI "ebpf_offset_filtered").
// P2-12: UNKNOWN verdicts ALSO filter (we cannot verify the offsets, so we
// do not trust them — only VERIFIED and NO_CTX are trusted).
namespace {
// One-shot "we just filtered the first event for this tracepoint" flags.
// Plain function-local atomics — no class state needed, no re-entrancy issue.
std::atomic<bool> g_forkWarned{false};
std::atomic<bool> g_exitWarned{false};
std::atomic<bool> g_switchWarned{false};
std::atomic<bool> g_execWarned{false};
inline bool firstDrop(std::atomic<bool>& flag) {
    bool expected = false;
    return flag.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}
} // namespace

#ifdef AATPE_HOST_TEST
// forward decl: defined near readEmbeddedCpuCounts (EBPF-13 sizing helper).
static int embeddedCpuBufferSlots(long possibleCpus);
// EBPF-16 host seam: read the one-shot offset-filter WARN flags (defined above).
bool EbpfScheduler::hostTestOffsetWarnFired(char kind) const {
    switch (kind) {
        case 'f': return g_forkWarned.load(std::memory_order_relaxed);
        case 'x': return g_exitWarned.load(std::memory_order_relaxed);
        case 's': return g_switchWarned.load(std::memory_order_relaxed);
        case 'e': return g_execWarned.load(std::memory_order_relaxed);
        default:  return false;
    }
}

// EBPF-20 host seam: return parseElfPrograms' mapIndices output verbatim.
bool EbpfScheduler::hostTestParseElfMapIndices(const std::string& path,
                                               std::vector<std::vector<int>>& outMapIndices) {
    std::vector<std::vector<uint8_t>> progs;
    std::vector<std::string> tps;
    std::string lic;
    outMapIndices.clear();
    return parseElfPrograms(path, progs, tps, outMapIndices, lic);
}

// EBPF-13 host seam: buffer slot sizing (pure arithmetic, no bpf fd).
int EbpfScheduler::hostTestEmbeddedCpuBufferSlots(long possibleCpus) {
    return embeddedCpuBufferSlots(possibleCpus);
}

// EBPF-16 host seam: force one tracepoint kind into DEGRADED.
void EbpfScheduler::hostTestSetOffsetDegraded(char kind) {
    switch (kind) {
        case 'f': offsetStatus_fork_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
        case 'x': offsetStatus_exit_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
        case 's': offsetStatus_switch_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
        case 'e': offsetStatus_exec_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
    }
}

// H01 host seam: set any ctx-reading tracepoint's offset verdict to an arbitrary
// value so the UNKNOWN soft-trust vs UNKNOWN-no-soft-trust delivery split can be
// exercised on the host (kind: 'f'=fork 's'=switch 'w'=wakeup).
void EbpfScheduler::hostTestSetOffsetStatus(char kind, TpOffsetStatus s) {
    switch (kind) {
        case 'f': offsetStatus_fork_.store(s, std::memory_order_relaxed); break;
        case 's': offsetStatus_switch_.store(s, std::memory_order_relaxed); break;
        case 'w': offsetStatus_wakeup_.store(s, std::memory_order_relaxed); break;
    }
}
#endif

// ============================================================================
// Event storm protection — per-type rate limiting for critical events.
//
// During app launches or boot, hundreds of fork/exit/exec events can fire per
// second. The critical queue (256 slots) can overflow, and each onExec opens
// /proc/<pid>/cmdline — a flood of these starves the main loop. This rate
// limiter caps critical events to kMaxCriticalPerSec (200/sec). Excess events
// are dropped; the periodic /proc scanThreads() (every 5s) is the backstop that
// reconciles missed lifecycle events.
//
// Returns true if the event should be DROPPED (rate limit exceeded).
// ============================================================================
bool EbpfScheduler::rateLimitCriticalEvent(void) {
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t winStart = criticalWindowStartMs_.load(std::memory_order_relaxed);
    if (winStart == 0 || (nowMs - winStart) >= 1000) {
        // New window
        criticalWindowStartMs_.store(nowMs, std::memory_order_relaxed);
        criticalEventsInWindow_.store(1, std::memory_order_relaxed);
        return false;  // first event of the window always passes
    }
    uint64_t count = criticalEventsInWindow_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > (uint64_t)kMaxCriticalPerSec) {
        criticalDroppedByRate_.fetch_add(1, std::memory_order_relaxed);
        return true;  // rate limit exceeded — drop this event
    }
    return false;
}

void EbpfScheduler::deliverForkEvent(const ForkEvent& ev) {
    // v94-r3: if child_pid/child_tgid offsets were detected as wrong on this
    // kernel, every fork event we push downstream would carry a garbage
    // child_pid. Drop it here instead of poisoning the scheduler.
    if (!isForkOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_forkWarned)) {
            LOG_WARN("offset-gate: sched_process_fork offsets mismatch — "
                     "filtering fork events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    // rate limit critical events to prevent app-launch storms
    if (rateLimitCriticalEvent()) return;
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.fork = ev;
        criticalQueue_.push(a);   // v95 issue 6: lifecycle -> critical ring
        // v95 issues 8/9: lifecycle event enqueued -> wake the daemon main loop
        // out of its DECISION_INTERVAL sleep. SWITCH does NOT do this.
        wakeCriticalIfSet(criticalEventWakeHook_);
    } else if (eventConsumer_) {
        eventConsumer_->onFork(ev);
        // (分级事件唤醒): even in the non-queue degraded path, a
        // FORK (thread add) is an important lifecycle event — wake the main loop
        // via self-pipe so it runs a full placement decision immediately instead
        // of waiting for the next idle poll timeout. sched_switch never wakes.
        wakeCriticalIfSet(criticalEventWakeHook_);
    }
}

void EbpfScheduler::deliverExitEvent(const ExitEvent& ev) {
    // v94-r3: exit does not read ctx (uses bpf_get_current_pid_tgid), so
    // offsetStatus_exit_ is almost always NO_CTX. The gate is still here for
    // symmetry and in case a future BPF revision starts reading ctx.
    if (!isExitOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_exitWarned)) {
            LOG_WARN("offset-gate: sched_process_exit offsets mismatch — "
                     "filtering exit events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    // rate limit critical events to prevent app-launch storms
    if (rateLimitCriticalEvent()) return;
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.exit = ev;
        criticalQueue_.push(a);
        wakeCriticalIfSet(criticalEventWakeHook_);  // v95 issues 8/9
    } else if (eventConsumer_) {
        eventConsumer_->onExit(ev);
        // EXIT (thread removal) is important — wake main loop.
        wakeCriticalIfSet(criticalEventWakeHook_);
    }
}

void EbpfScheduler::deliverSwitchEvent(const SwitchEvent& ev) {
    // v94-r3: switch is load-bearing. If its offsets mismatch the whole eBPF
    // subsystem should already have been degraded to polling by load(), but
    // this gate is the second line of defense — even if a stale switch event
    // sneaks through before degrade() runs, we drop it rather than feed
    // garbage next_pid/prev_comm to the scheduler.
    if (!isSwitchOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_switchWarned)) {
            LOG_WARN("offset-gate: sched_switch offsets mismatch — "
                     "filtering switch events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    // P1-6: removed userspace 1/10 sampling block. The three-layer sampling
    // (BPF-side runq 1/8 + consumer-side 1/10) was over-aggressive. The BPF
    // side already aggregates high-frequency events; dropping another 90% here
    // made switch events nearly invisible. All switch events now flow through.
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.sw = ev;
        normalQueue_.push(a);    // v95 issue 6: high-frequency -> normal ring
        // Intentionally NO wake hook: SWITCH events are too frequent.
    } else if (eventConsumer_) {
        eventConsumer_->onSwitch(ev);
    }
}

void EbpfScheduler::deliverExecEvent(const ExecEvent& ev) {
    // v94-r3: exec does not read ctx (uses bpf_get_current_comm), so
    // offsetStatus_exec_ is almost always NO_CTX. Gate retained for symmetry.
    if (!isExecOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_execWarned)) {
            LOG_WARN("offset-gate: sched_process_exec offsets mismatch — "
                     "filtering exec events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    // rate limit critical events to prevent app-launch storms
    if (rateLimitCriticalEvent()) return;
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.exec = ev;
        criticalQueue_.push(a);
        wakeCriticalIfSet(criticalEventWakeHook_);  // v95 issues 8/9
    } else if (eventConsumer_) {
        eventConsumer_->onExec(ev);
        // EXEC (new app/component launch) is important — wake main loop.
        wakeCriticalIfSet(criticalEventWakeHook_);
    }
}

// P1-1: deliver a runqueue-latency sample. This is stat-only (no /proc I/O,
// no g_threadsMutex, no queue push): the BPF side already sampled long samples
// fully and short samples at 1/8, so the volume is low. We track the worst
// observed delay as an atomic max and forward to the consumer for optional
// decision-loop use.
void EbpfScheduler::deliverRunqEvent(const RunqEvent& ev) {
    // LED-026 (R2-011): runq latency samples are derived from sched_wakeup
    // (pid/wakerTid/targetCpu/delayNs) joined against sched_switch. When either
    // tracepoint's ctx offsets are UNKNOWN/DEGRADED, the sample fields are garbage.
    // deliverFork/Exit/Switch/Exec all gate on is*OffsetTrusted(); runq was the
    // only path missing the gate. Drop untrusted samples instead of feeding them
    // to onRunqLatency / runqMaxDelayNs_.
    if (!isWakeupOffsetTrusted() || !isSwitchOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t prevMax = runqMaxDelayNs_.load(std::memory_order_relaxed);
    while (ev.delayNs > prevMax &&
           !runqMaxDelayNs_.compare_exchange_weak(
               prevMax, ev.delayNs, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
        // CAS retry loop; prevMax re-read by compare_exchange_weak.
    }
    if (eventConsumer_) {
        eventConsumer_->onRunqLatency(ev);
    }
}

// ============================================================================
// v94 P1-2: daemon main-loop consumption
// Drains the event queue (filled by the poll thread) and dispatches each event
// to the consumer on the daemon main thread. Called from the daemon main loop in
// place of the old direct pollEvents(256) ring read.
// ============================================================================
// ============================================================================
// v94 P1-2 / v95 issues 6/7/10/11: daemon main-loop consumption
// Drains the event queues (filled by the poll thread) and dispatches each
// event to the consumer on the daemon main thread. Called from the daemon main
// loop in place of the old direct pollEvents(256) ring read.
//
// v95 dual-queue + budget:
//   1. criticalQueue_ (FORK/EXIT/EXEC) is drained UNBOUNDED first — lifecycle
//      events must never wait behind SWITCH traffic (issues 6/7).
//   2. normalQueue_ (SWITCH) is drained up to kDispatchBudgetPerCycle (256)
//      events per cycle; the remainder stays for the next cycle. This prevents
//      an extreme backlog from being processed all at once, which previously
//      produced a CPU spike / decision-loop jitter (issues 10/11).
//   2b. criticalQueue_ (FORK/EXEC/EXIT) is ALSO bounded now, at
//      kCriticalDispatchBudgetPerCycle (1024). Previously it was unbounded,
//      which caused a startup kill-loop: the poll thread backlogs tens of
//      thousands of lifecycle events during daemonInit, then the first
//      dispatchQueuedEvents() tries to drain them all at once. Each onExec
//      opens /proc/<pid>/cmdline (getPackageByPid), so tens of thousands of
//      events take tens of seconds — exceeding the watchdog 30s heartbeat
//      timeout → daemon is killed → restart → backlog rebuilds → loop.
//      Bounding to 1024/cycle lets the main loop keep advancing (heartbeat
//      stays fresh) while steadily working through the backlog.
//   3. eventsProcessedThisCycle is published via lastCycleEventsProcessed_.
// ============================================================================
size_t EbpfScheduler::dispatchQueuedEvents() {
    if (!eventConsumer_) return 0;
    // v95 issue 13: reuse a thread-local batch buffer across cycles — no
    // reallocation on the hot path (the SPSC ring itself already avoids any
    // per-event heap allocation; this only avoids growing the consumer-side
    // vector repeatedly).
    static thread_local std::vector<AnyEvent> s_batch;
    s_batch.clear();

    size_t total = 0;

    // Phase 1: critical (lifecycle) queue — bounded at 1024/cycle.
    // See header comment for why this is no longer unbounded.
    size_t nCritical = criticalQueue_.popBatch(s_batch, kCriticalDispatchBudgetPerCycle);
    for (size_t i = 0; i < nCritical; ++i) {
        const AnyEvent& a = s_batch[i];
        // L-fix: memcpy the POD out of the union storage instead of naming
        // a.data.fork/.exit/.exec directly. The producer activated exactly one
        // member; reading its object representation via memcpy from the union is
        // well-defined and does not depend on the compiler's active-member view.
        switch ((uint32_t)a.hdr.type) {
            case (uint32_t)EventType::FORK: {
                ForkEvent ev; std::memcpy(&ev, &a.data, sizeof(ev)); eventConsumer_->onFork(ev); break;
            }
            case (uint32_t)EventType::EXIT: {
                ExitEvent ev; std::memcpy(&ev, &a.data, sizeof(ev)); eventConsumer_->onExit(ev); break;
            }
            case (uint32_t)EventType::EXEC: {
                ExecEvent ev; std::memcpy(&ev, &a.data, sizeof(ev)); eventConsumer_->onExec(ev); break;
            }
            default: break;  // SWITCH never lands here
        }
    }
    total += nCritical;

    // Phase 2: normal (SWITCH) queue — bounded by per-cycle budget.
    s_batch.clear();
    size_t nNormal = normalQueue_.popBatch(s_batch, kDispatchBudgetPerCycle);
    for (size_t i = 0; i < nNormal; ++i) {
        const AnyEvent& a = s_batch[i];
        // Only SWITCH events are routed to normalQueue_; guard defensively.
        if ((uint32_t)a.hdr.type == (uint32_t)EventType::SWITCH) {
            SwitchEvent ev; std::memcpy(&ev, &a.data, sizeof(ev)); eventConsumer_->onSwitch(ev);
        }
    }
    total += nNormal;

    lastCycleEventsProcessed_.store((uint64_t)total, std::memory_order_relaxed);
    return total;
}

uint64_t EbpfScheduler::getQueueDropped() const {
    // v95: sum of both rings' overflow-drop counters.
    return criticalQueue_.droppedCount() + normalQueue_.droppedCount();
}

size_t EbpfScheduler::getQueueSize() const {
    return criticalQueue_.sizeApprox() + normalQueue_.sizeApprox();
}

// v94 fix / 问题46: consolidated event stats for state JSON / WebUI.
EbpfScheduler::EventStats EbpfScheduler::getEventStats() const {
    EventStats s;
    s.total         = eventCountTotal_.load(std::memory_order_relaxed);
    s.processed     = (eventCountFork_.load(std::memory_order_relaxed) +
                       eventCountExit_.load(std::memory_order_relaxed) +
                       eventCountSwitch_.load(std::memory_order_relaxed) +
                       eventCountExec_.load(std::memory_order_relaxed));
    s.dropped       = criticalQueue_.droppedCount() + normalQueue_.droppedCount();
    s.ringOverflow  = ringLost_.load(std::memory_order_relaxed);
    s.fallbackCount = fallbackCount_.load(std::memory_order_relaxed);
    s.consumerLag   = criticalQueue_.sizeApprox() + normalQueue_.sizeApprox();
    s.windowLost    = windowLost_.load(std::memory_order_relaxed);
    s.eventRatePerSec = getEventRatePerSec();
    // v95 issues 6/10/11: per-queue breakdown + last-cycle throughput.
    s.criticalDropped   = criticalQueue_.droppedCount();
    s.normalDropped     = normalQueue_.droppedCount();
    s.criticalBacklog   = criticalQueue_.sizeApprox();
    s.eventsProcessedLastCycle = lastCycleEventsProcessed_.load(std::memory_order_relaxed);
    // events dropped by the per-second rate limiter
    // (already counted in s.dropped via the queue, but tracked separately here
    // so WebUI can distinguish queue-overflow drops from rate-limit drops).
    s.dropped += criticalDroppedByRate_.load(std::memory_order_relaxed);
    return s;
}

// ============================================================================
// pollEvents — non-blocking event pump (RING CONSUMER ONLY: poll thread calls)
// ============================================================================
int EbpfScheduler::pollEvents(int maxEvents) {
    if (!active_) return 0;

    // v91 fix(P5): run window rotation and switchAgg_ cleanup in ALL modes
    // (event_driven, hybrid, and POLLING). In POLLING mode these are
    // effectively no-ops — no ring buffer means no lost events, and no
    // switch events means switchAgg_ stays empty — but running them keeps
    // window timers alive and ensures stale entries are purged if the mode
    // is later switched from POLLING to event-driven.
    rotateWindowIfNeeded();
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t nowMs = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
        uint64_t lastCleanup = lastSwitchAggCleanupMs_.load(std::memory_order_relaxed);
        if (nowMs - lastCleanup >= kSwitchAggCleanupIntervalMs) {
            lastSwitchAggCleanupMs_.store(nowMs, std::memory_order_relaxed);
            cleanupSwitchAgg(nowMs);
        }
    }

    // POLLING mode has no eBPF ring buffers to drain — maintenance above
    // is the only work needed.
    if (mode_ == EbpfMode::POLLING) return 0;
    if (rings_.empty()) return 0;

    // v90: cache page size once — calling sysconf() per ring per poll call
    // (8 rings × every poll) was a measurable syscall overhead in the hot path.
    static const long s_pageSizeCached = []() {
        long ps = sysconf(_SC_PAGESIZE);
        return (ps > 0) ? ps : 4096L;
    }();

    // Use poll() to check which rings have data (non-blocking)
    // v95 issue 16: hoist the pollfd vector out of the call path. pollEvents()
    // is called in a tight loop from the poll thread (and rarely from the daemon
    // main loop in the legacy fallback); rebuilding the vector every call did a
    // heap allocation per call. thread_local keeps one vector per calling
    // thread (no cross-thread sharing) that grows to rings_.size() once and is
    // then reused with only clear() + refills.
    static thread_local std::vector<struct pollfd> s_pfds;
    s_pfds.clear();
    s_pfds.reserve(rings_.size());
    for (auto& ring : rings_) {
        if (ring.perfFd >= 0) {
            struct pollfd pfd;
            pfd.fd = ring.perfFd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            s_pfds.push_back(pfd);
        }
    }
    if (s_pfds.empty()) return 0;

    int pollRet = poll(s_pfds.data(), s_pfds.size(), 0);  // non-blocking
    (void)pollRet;  // #24: result only consumed by compile-trimmed debug logging

    // CRITICAL 4.14 FIX: On kernel 4.14, disabled perf events (ring-buffer-only
    // events for cpu!=attachCpu) may never trigger poll() wakeups, even when
    // bpf_perf_event_output() writes data to their ring buffers. Even the enabled
    // event may not reliably wake poll. Therefore we MUST check the ring buffer
    // data_head/data_tail directly as a fallback, regardless of poll() result.
    static std::atomic<uint64_t> s_pollDiagCounter{0};
    bool logThisCall = ((s_pollDiagCounter.fetch_add(1) % 256) == 0);

    int total = 0;

    // Phase 1: Process rings that poll() reported as readable
    size_t pfdIdx = 0;
    for (auto& ring : rings_) {
        // P3-9: rings with perfFd < 0 were NOT added to s_pfds (built above only
        // from perfFd >= 0 entries), so we must NOT advance pfdIdx for them —
        // otherwise pfdIdx drifts out of alignment with s_pfds[] and we read the
        // wrong pollfd's revents. Such rings are skipped entirely.
        if (ring.perfFd < 0) { continue; }
        if (pfdIdx < s_pfds.size() && (s_pfds[pfdIdx].revents & POLLIN)) {
            int perRingMax = maxEvents - total;
            if (perRingMax > 0) {
                total += processRingData(ring, perRingMax);
            }
        }
        pfdIdx++;
        if (total >= maxEvents) break;
    }

    // Phase 2: FALLBACK — directly check data_head != data_tail for ALL rings.
    // This catches events that didn't trigger poll() wakeups (common on 4.14
    // with disabled ring-buffer-only events). Only run if we haven't exhausted
    // the event budget and poll didn't report everything.
    // v94-fix (发热修复): 加 50ms 时间门控 — 原逻辑每次 pollEvents 都遍历所有 ring
    // 的 mmap 内存，在 8 个 ring buffer 上累积开销不小。50ms 门控既保证 4.14 fallback
    // 的可靠性（事件最多延迟 50ms 被消费，可接受），又减少空闲时的 mmap 读取开销。
    {
        static std::atomic<uint64_t> s_lastFallbackCheckMs{0};
        struct timespec nowTs;
        clock_gettime(CLOCK_MONOTONIC, &nowTs);
        uint64_t nowFbMs = (uint64_t)nowTs.tv_sec * 1000ULL + (uint64_t)nowTs.tv_nsec / 1000000ULL;
        uint64_t lastFb = s_lastFallbackCheckMs.load(std::memory_order_relaxed);
        bool runFallback = (nowFbMs - lastFb >= 50);
        if (runFallback) {
            s_lastFallbackCheckMs.store(nowFbMs, std::memory_order_relaxed);
            if (total < maxEvents) {
                for (auto& ring : rings_) {
                    if (ring.perfFd < 0) continue;
                    if (total >= maxEvents) break;
                    // Directly check if there's unread data in the ring buffer
                    // v90: use static-cached page size to compute data region size
                    uint64_t dataSize = (ring.mmapSize > (size_t)s_pageSizeCached)
                        ? (uint64_t)(ring.mmapSize - (size_t)s_pageSizeCached) : 0;
                    (void)dataSize;  // #24: only consumed by compile-trimmed debug logging
                    auto* hdr = reinterpret_cast<volatile struct perf_event_mmap_page*>(ring.base);
                    // P1: ACQUIRE head / RELAXED tail for the fallback has-data probe
                    // (processRingData() below re-reads with the proper barriers).
                    // P3-13: hdr == ring.base is always non-null for rings in rings_
                    // (mmapPerfRingForCpu only pushes after a successful mmap, and
                    // unmapAllRines clears rings_ on teardown), so the old `hdr &&`
                    // null check was dead. ring.base is dereferenced above regardless.
                    uint64_t fbHead = __atomic_load_n(&hdr->data_head, __ATOMIC_ACQUIRE);
                    uint64_t fbTail = __atomic_load_n(&hdr->data_tail, __ATOMIC_RELAXED);
                    if (fbHead != fbTail) {
                        int perRingMax = maxEvents - total;
                        if (perRingMax > 0) {
                            if (logThisCall) {
                                // v94-P6-fix: LOG_DEBUG (default off). These diagnostics
                                // ran at LOG_INFO inside pollEvents(); even with 1-in-256
                                // throttling they surfaced as ~8 lines/sec at default log
                                // level, burning CPU in the hot poll path. Enable only when
                                // logLevel>=DEBUG to diagnose ring/poll issues.
                                LOG_DEBUG("[eBPF-DIAG] pollEvents fallback: cpu=%d has data (head=%llu tail=%llu diff=%llu dataSize=%llu) "
                                         "but pollRet=%d (poll did not wake)",
                                         ring.cpu,
                                         (unsigned long long)hdr->data_head,
                                         (unsigned long long)hdr->data_tail,
                                         (unsigned long long)(hdr->data_head - hdr->data_tail),
                                         (unsigned long long)dataSize,
                                         pollRet);
                            }
                            total += processRingData(ring, perRingMax);
                        }
                    }
                }
            }
        }
    }

    if (logThisCall) {
        // v94-P6-fix: LOG_DEBUG (default off) — was LOG_INFO, surfaced every poll
        // cycle at default log level. The ALL-ZERO failure below stays LOG_WARN.
        LOG_DEBUG("[eBPF-DIAG] pollEvents: pollRet=%d rings=%zu processed=%d totalEvents=%llu "
                 "fork=%llu exit=%llu switch=%llu exec=%llu",
                 pollRet, rings_.size(), total,
                 (unsigned long long)eventCountTotal_.load(),
                 (unsigned long long)eventCountFork_.load(),
                 (unsigned long long)eventCountExit_.load(),
                 (unsigned long long)eventCountSwitch_.load(),
                 (unsigned long long)eventCountExec_.load());

        // CRITICAL DIAGNOSTIC: Read BPF-side event_counters map to determine if
        // BPF programs are actually firing. If BPF counters > 0 but user-space
        // events == 0, the problem is in the ring buffer write/read path.
        // If BPF counters == 0, the BPF programs are not running at all.
        if (countersMapFd_ >= 0) {
            std::vector<uint64_t> bpfCounts;
            if (readEmbeddedCpuCounts(bpfCounts)) {
                uint64_t bpfTotal = 0;
                for (uint64_t v : bpfCounts) bpfTotal += v;
                // v94-P6-fix: LOG_DEBUG (default off).
                LOG_DEBUG("[eBPF-DIAG] BPF-side counters: total=%llu (per-CPU: %s)",
                         (unsigned long long)bpfTotal,
                         bpfCounts.empty() ? "none" : "see per-CPU values");
                for (size_t i = 0; i < bpfCounts.size(); i++) {
                    if (bpfCounts[i] > 0) {
                        LOG_DEBUG("[eBPF-DIAG]   BPF counter cpu=%zu: %llu",
                                 i, (unsigned long long)bpfCounts[i]);
                    }
                }
                if (bpfTotal == 0) {
                    LOG_WARN("[eBPF-DIAG] BPF-side counters are ALL ZERO — BPF programs are NOT firing! "
                             "(tracepoint attach may not be triggering on any CPU)");
                }
            }
        }
    }

    // Update event rate calculation
    uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastCheck = lastRateCheckNs_.load();
    if (lastCheck > 0) {
        uint64_t elapsedNs = now - lastCheck;
        if (elapsedNs >= 1000000000ULL) {  // 1 second
            uint64_t curEvents = eventCountTotal_.load();
            uint64_t delta = curEvents - eventsAtLastCheck_.load();
            double rate = (double)delta * 1e9 / (double)elapsedNs;
            eventRate_.store(rate);
            eventsAtLastCheck_.store(curEvents);
            lastRateCheckNs_.store(now);
        }
    } else {
        lastRateCheckNs_.store(now);
        eventsAtLastCheck_.store(eventCountTotal_.load());
    }

    return total;
}

double EbpfScheduler::getEventRatePerSec() const {
    return eventRate_.load();
}

// ============================================================================
// v90: Windowed loss-rate rotation
// Resets windowLost_/windowEvents_ every 60s so the displayed loss rate
// reflects recent activity instead of a cumulative value that only grows.
// ============================================================================
void EbpfScheduler::rotateWindowIfNeeded() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t nowMs = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    uint64_t startMs = windowStartMs_.load(std::memory_order_relaxed);
    if (startMs == 0) {
        windowStartMs_.store(nowMs, std::memory_order_relaxed);
        return;
    }
    if (nowMs - startMs >= kWindowRotateMs) {
        windowLost_.store(0, std::memory_order_relaxed);
        windowEvents_.store(0, std::memory_order_relaxed);
        windowStartMs_.store(nowMs, std::memory_order_relaxed);
    }
}

// ============================================================================
// v90 / v95 issues 14/15: switchAgg_ cleanup
// Evicts entries for processes that haven't switched in >5min, and hard-caps
// the map at 10000 entries to prevent hash-lookup latency from degrading
// over long uptimes (which was part of the loss-rate positive-feedback loop).
//
// v95 issues 14/15: the periodic sweep ran every 1 HOUR while the entry TTL
// is 5 minutes, so stale entries lingered for up to ~65 minutes. The sweep
// interval is now matched to the TTL (5 min). evictStaleSwitchAggLocked() is
// split out so the SWITCH aggregation path can ALSO call it inline (lazy
// eviction, every kSwitchAggLazyEvictInserts inserts) without re-locking
// switchAggMutex_, ensuring expired records are purged promptly rather than
// lingering until the next periodic sweep.
// ============================================================================
void EbpfScheduler::evictStaleSwitchAggLocked(uint64_t nowMs) {
    // Caller MUST hold switchAggMutex_.
    if (switchAgg_.empty()) return;
    // Convert nowMs to ns for comparison with lastTs (which is in ns from BPF)
    uint64_t nowNs = nowMs * 1000000ULL;
    uint64_t cutoffNs = (nowNs > kSwitchAggEntryTtlMs * 1000000ULL)
        ? nowNs - kSwitchAggEntryTtlMs * 1000000ULL : 0;

    // Phase 1: evict stale entries (no switch in > TTL)
    for (auto it = switchAgg_.begin(); it != switchAgg_.end(); ) {
        if (it->second.lastTs < cutoffNs) {
            it = switchAgg_.erase(it);
        } else {
            ++it;
        }
    }
    // Phase 2: if still over cap, evict oldest entries by lastTs
    if (switchAgg_.size() > kMaxSwitchAggEntries) {
        // Collect entries sorted by lastTs ascending
        std::vector<std::pair<uint64_t, uint32_t>> sorted;
        sorted.reserve(switchAgg_.size());
        for (const auto& kv : switchAgg_) {
            sorted.push_back({kv.second.lastTs, kv.first});
        }
        std::sort(sorted.begin(), sorted.end());
        size_t toRemove = switchAgg_.size() - kMaxSwitchAggEntries;
        for (size_t i = 0; i < toRemove && i < sorted.size(); i++) {
            switchAgg_.erase(sorted[i].second);
        }
    }
}

void EbpfScheduler::cleanupSwitchAgg(uint64_t nowMs) {
    // v91 fix(P4): hold switchAggMutex_ for the entire cleanup — it does
    // find/erase iteration which races with processRingData's find/insert.
    std::lock_guard<std::mutex> aggLock(switchAggMutex_);
    evictStaleSwitchAggLocked(nowMs);
}

// ============================================================================
// v86: Poll-based adaptive poll thread
// Uses poll() to block on ring-buffer FDs + wakeup pipe instead of
// sleep+check spinning. Adaptive backoff: 1→2→5→10→20→50ms when idle,
// resets to 1ms on event activity. Batch-drains all rings up to 4096 events
// per wakeup. Wakeup pipe ensures prompt thread exit.
// ============================================================================
void EbpfScheduler::pollThreadFunc() {
    // v89: elevate poll thread priority so ring-buffer draining is not
    // preempted by ordinary workload threads under high load.
    // nice -10 is safe (no RT priority, no CAP_SYS_NICE needed on Android
    // for lowering nice value when running as root).
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
        LOG_WARN("[eBPF-v89] pollThreadFunc: setpriority(-10) failed: %s",
                 aatpeErrStr(errno));
    }

    LOG_INFO("[eBPF-v89] pollThreadFunc: started (poll-based adaptive, nice=-10)");

    // v95-OPT2 (CPU 压降): timeout levels 从 {5,10,20,50,100} 改为 {20,50,100,200,500}。
    // 根因: 旧最小 5ms 超时导致 poll 线程活跃时每秒最多唤醒 200 次, 即使空闲时
    // 也要 100ms 一次, 阻止 CPU 进入 deep idle (ARM WFI 以上 idle 状态需要更长不活跃窗口)。
    // 新值: 活跃时 20ms(50次/秒, 仍远低于旧 200次/秒), 空闲时 500ms(2次/秒,
    // 让 CPU 有机会进入 deeper idle 状态, 显著降低待机功耗)。每秒最大唤醒次数
    // 从 200 次降到 20 次, poll 线程自身 CPU 占用降一个数量级。
    // 调度效果保留: 20ms 延迟对 sched_switch 批处理足够(批处理窗口), FORK/EXEC/EXIT
    // 关键事件由 criticalEventWakeHook 即时唤醒主循环, 不依赖 poll 周期。
    static const int kTimeoutLevels[] = {20, 50, 100, 200, 500};
    static const int kNumLevels = (int)(sizeof(kTimeoutLevels) / sizeof(kTimeoutLevels[0]));
    int timeoutIdx = 0;

    // v95 issue 16: hoist the pollfd vector OUT of the adaptive while loop.
    // Previously a fresh vector was constructed (and heap-allocated) on every
    // poll iteration — up to thousands of allocations per second under active
    // draining. Now it grows once to (rings_.size()+1) and is only cleared +
    // refilled each iteration. rings_ is fixed after load(), so the capacity
    // never changes after the first fill.
    std::vector<struct pollfd> pfds;
    pfds.reserve(16);

    while (!pollThreadStop_.load(std::memory_order_acquire)) {
        // E-005: explicit acquire loads for the cross-thread status flags, matching
        // the rest of the file's ordering policy (was implicit operator bool()/==
        // which relied on seq_cst conversion). rings_ is owned by the load/poll
        // lifecycle and is not an atomic.
        if (!active_.load(std::memory_order_acquire) ||
            mode_.load(std::memory_order_acquire) == EbpfMode::POLLING ||
            rings_.empty()) {
            // Inactive: block on wakeup pipe with 200ms timeout (v95-OPT2: was 50ms).
            // 200ms 仍能在 stopPollThread() 后 200ms 内退出, 但空闲时少 4x 唤醒。
            struct pollfd wfd;
            wfd.fd = pollWakeupReadFd_;
            wfd.events = POLLIN;
            wfd.revents = 0;
            if (pollWakeupReadFd_ >= 0) {
                poll(&wfd, 1, 200);
                // Drain wakeup pipe
                char drainBuf[32];
                while (read(pollWakeupReadFd_, drainBuf, sizeof(drainBuf)) > 0) {}
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            timeoutIdx = 0;
            continue;
        }

        // Build pollfd list: all ring buffer FDs + wakeup FD
        // v95 issue 16: reuse the hoisted vector (clear, no reallocation).
        pfds.clear();
        pfds.reserve(rings_.size() + 1);
        for (auto& ring : rings_) {
            if (ring.perfFd >= 0) {
                struct pollfd pfd;
                pfd.fd = ring.perfFd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                pfds.push_back(pfd);
            }
        }
        if (pollWakeupReadFd_ >= 0) {
            struct pollfd wpfd;
            wpfd.fd = pollWakeupReadFd_;
            wpfd.events = POLLIN;
            wpfd.revents = 0;
            pfds.push_back(wpfd);
        }

        if (pfds.empty()) {
            // v95-OPT2: 10ms -> 50ms, 空闲时少唤醒。rings_ 为空(极端加载失败场景)时
            // 不需要 100Hz 轮询, 50ms 足够发现 wakeup。
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        int timeoutMs = kTimeoutLevels[timeoutIdx];
        int pollRet = poll(pfds.data(), pfds.size(), timeoutMs);

        // P3: EINTR (signal) must not be counted as a timeout/backoff step.
        // Data still lives in the ring buffers; just re-enter the poll immediately.
        if (pollRet < 0 && errno == EINTR) {
            continue;
        }

        // Check wakeup pipe (always last entry if present)
        bool wokenByStop = false;
        if (pollWakeupReadFd_ >= 0 && !pfds.empty()) {
            const struct pollfd& lastFd = pfds.back();
            if (lastFd.fd == pollWakeupReadFd_ && (lastFd.revents & POLLIN)) {
                char drainBuf[32];
                while (read(pollWakeupReadFd_, drainBuf, sizeof(drainBuf)) > 0) {}
                if (pollThreadStop_.load(std::memory_order_acquire)) {
                    wokenByStop = true;
                }
            }
        }
        if (wokenByStop) break;

        // Drain events: pollEvents already handles poll+fallback(4.14 direct check)
        // Batch loop: keep draining until no more data or 4096 event budget exhausted
        int totalProcessed = 0;
        while (totalProcessed < 4096) {
            int processed = pollEvents(4096 - totalProcessed);
            if (processed <= 0) break;
            totalProcessed += processed;
        }

        // bugfix: when the ring is continuously saturated (VMOS/game storm,
        // 100k events/s), poll() returns immediately and this loop re-enters at
        // once, pinning the poll thread (nice=-10) at 43% CPU as TID=15924. If we
        // drained a FULL 4096 batch, the ring is still backed up — yield ~5ms so
        // the kernel can run other threads and we stop busy-spinning. Events keep
        // queueing in the ring buffer; they are processed on the next pass.
        if (totalProcessed >= 4096) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (totalProcessed > 0) {
            // Activity — reset to fastest timeout
            timeoutIdx = 0;
        } else if (pollRet > 0) {
            // poll woke but pollEvents found 0 (spurious wake / already drained)
            timeoutIdx = 0;
        } else {
            // Timeout with no events — adaptive backoff
            if (timeoutIdx < kNumLevels - 1) {
                timeoutIdx++;
            }
        }
    }

    LOG_INFO("[eBPF-v86] pollThreadFunc: stopped");
}

bool EbpfScheduler::startPollThread() {
    if (pollThreadRunning_.load()) {
        LOG_WARN("[eBPF-v86] startPollThread: already running");
        return true;
    }
    // Create wakeup pipe for poll() interruption
    pollWakeupReadFd_ = -1;
    pollWakeupWriteFd_ = -1;
    int pipeFds[2];
    if (pipe(pipeFds) == 0) {
        pollWakeupReadFd_ = pipeFds[0];
        pollWakeupWriteFd_ = pipeFds[1];
        // Set read end non-blocking for draining
        int fl = fcntl(pollWakeupReadFd_, F_GETFL, 0);
        if (fl >= 0) fcntl(pollWakeupReadFd_, F_SETFL, fl | O_NONBLOCK);
    } else {
        LOG_WARN("[eBPF-v86] startPollThread: pipe() failed (%s), wakeup may be delayed",
                 aatpeErrStr(errno));
    }

    pollThreadStop_.store(false, std::memory_order_release);
    // v94 P1-2: the poll thread is now the SINGLE ring consumer. Route all
    // consumed events through the bounded queue; the daemon main loop drains it
    // via dispatchQueuedEvents() instead of reading the ring itself.
    setEventQueueEnabled(true);
#ifdef AATPE_HOST_TEST
    // EBPF-04 test seam: force thread creation failure to exercise the
    // tear-down-and-return-false path. Release compiles this out.
    if (hostTestForcePollThreadFail.load(std::memory_order_relaxed)) {
        LOG_WARN("[eBPF] startPollThread: host injection forcing failure");
        setEventQueueEnabled(false);
        pollThreadRunning_.store(false);
        if (pollWakeupReadFd_ >= 0) { close(pollWakeupReadFd_); pollWakeupReadFd_ = -1; }
        if (pollWakeupWriteFd_ >= 0) { close(pollWakeupWriteFd_); pollWakeupWriteFd_ = -1; }
        return false;
    }
#endif
    try {
        pollThread_ = std::thread(&EbpfScheduler::pollThreadFunc, this);
        pollThreadRunning_.store(true);
        LOG_INFO("[eBPF-v86] startPollThread: poll-based adaptive thread started (queue single-consumer mode)");
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("[eBPF-v86] startPollThread: failed to create thread: %s", e.what());
        setEventQueueEnabled(false);  // v94 P1-2: fall back to direct delivery
        pollThreadRunning_.store(false);
        if (pollWakeupReadFd_ >= 0) { close(pollWakeupReadFd_); pollWakeupReadFd_ = -1; }
        if (pollWakeupWriteFd_ >= 0) { close(pollWakeupWriteFd_); pollWakeupWriteFd_ = -1; }
        return false;
    }
}

void EbpfScheduler::stopPollThread() {
    if (!pollThreadRunning_.load()) return;
    pollThreadStop_.store(true, std::memory_order_release);
    // Signal wakeup pipe to interrupt poll() immediately
    if (pollWakeupWriteFd_ >= 0) {
        char one = 1;
        ssize_t w = write(pollWakeupWriteFd_, &one, 1);
        (void)w;
    }
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
    pollThreadRunning_.store(false);
    // v94 P1-2: poll thread is gone; it is no longer the ring consumer.
    // Disable the queue so any residual processRingData call delivers directly
    // (legacy safe path) rather than enqueueing into an undrained queue.
    setEventQueueEnabled(false);
    // Cleanup wakeup pipe
    if (pollWakeupReadFd_ >= 0) { close(pollWakeupReadFd_); pollWakeupReadFd_ = -1; }
    if (pollWakeupWriteFd_ >= 0) { close(pollWakeupWriteFd_); pollWakeupWriteFd_ = -1; }
    LOG_INFO("[eBPF-v86] stopPollThread: poll thread stopped");
}

// ============================================================================
// Method 1: embedded_syscall — load from embedded bytecode header
// ============================================================================
bool EbpfScheduler::tryLoadEmbeddedSyscall() {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kLoadTimeoutMs);
    LOG_FORCE("=== tryLoadEmbeddedSyscall start ===");

    // ROUND-014 P3: real capget pre-flight (CAP_BPF=39 / CAP_PERFMON=38,
    // fallback CAP_SYS_ADMIN=21 on old kernels). Missing -> NO_CAP.
    {
        struct cap_hdr { uint32_t version, pid; } hdr{0x20080522, 0};
        struct cap_data { uint32_t eff, perm, inh; } data[2]{};
        if (syscall(SYS_capget, &hdr, data) == 0) {
            auto has = [&](int cap)->bool {
#ifdef AATPE_HOST_TEST
                if (hostTestFakeCaps.load(std::memory_order_relaxed)) return true;
#endif
                int idx = cap / 32, bit = cap % 32;
                return (data[idx].eff >> bit) & 1;
            };
            if (!has(39) && !has(38) && !has(21)) { state_ = EbpfState::NO_CAP; return false; }
        }
        // capget failed -> conservative: continue to probeFd for real errno.
    }

    // Step 1: Minimal probe to verify BPF_PROG_LOAD
    static const uint8_t kMinimalProg[] = {
        0xb7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    if (loadTimeoutExceeded(deadline)) { state_ = aatpe::classifyEbpfFailure(EACCES, false, false); return false; }
    int probeFd = loadBpfProgFromBytes(kMinimalProg, sizeof(kMinimalProg),
                                        kEmbeddedLicense, -1, BPF_PROG_TYPE_SOCKET_FILTER);
    if (probeFd < 0) {
        LOG_FORCE("embedded: minimal probe FAILED errno=%d (%s) — BPF syscall may be blocked or unavailable",
                  errno, aatpeErrStr(errno));
        state_ = aatpe::classifyEbpfFailure(errno, false, false);   // ROUND-014 P3
        return false;
    }
    LOG_FORCE("embedded: minimal probe OK (BPF syscall available)");
    close(probeFd);

    // Step 1b: Probe KPROBE type support on this kernel (critical for 4.14).
    // A minimal KPROBE program (r0=0; exit) verifies that BPF_PROG_TYPE_KPROBE
    // is accepted by the kernel before we attempt full program loads.
    static const uint8_t kMinimalKprobeProg[] = {
        0xb7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x95, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    int kprobeTypeSupported = 0;
    int kprobeProbeFd = loadBpfProgFromBytes(kMinimalKprobeProg,
                                              sizeof(kMinimalKprobeProg),
                                              kEmbeddedLicense, -1, BPF_PROG_TYPE_KPROBE);
    if (kprobeProbeFd >= 0) {
        kprobeTypeSupported = 1;
        LOG_INFO("embedded: KPROBE type=2 supported by kernel (minimal probe OK)");
        close(kprobeProbeFd);
    } else {
        LOG_WARN("embedded: KPROBE type=2 minimal probe FAILED errno=%d (%s) — will try TRACEPOINT first",
                 errno, aatpeErrStr(errno));
    }

    // Step 2: Create all maps from embedded definitions
    if (loadTimeoutExceeded(deadline)) { state_ = aatpe::classifyEbpfFailure(EACCES, false, false); return false; }
    mapFds_.clear();
    for (int i = 0; i < kEmbeddedMapCount; i++) {
        const auto& m = kEmbeddedMaps[i];
        int fd = createMap(m.type, m.keySize, m.valueSize, m.maxEntries);
        /* F-03 v4.5.0: wakeup_cache (map idx 2) now asks for LRU_HASH so the
         * kernel auto-reaps cold/orphaned wakeup entries. If this kernel rejects
         * the LRU type (EINVAL -> "invalid map type"), transparently retry plain
         * HASH: same key/value/max, restores the v4.4.9 bounded behavior. */
        if (fd < 0 && i == 2 && m.type == BPF_MAP_TYPE_LRU_HASH &&
            wakeupCacheMapDecision(fd, errno) == 1) {
            LOG_WARN("embedded: wakeup_cache LRU_HASH rejected errno=%d (%s); "
                     "falling back to plain HASH", errno, aatpeErrStr(errno));
            fd = createMap(BPF_MAP_TYPE_HASH, m.keySize, m.valueSize, m.maxEntries);
        }
        if (fd < 0) {
            LOG_WARN("embedded: map[%d] create failed type=%u errno=%d (%s)", i, m.type, errno, aatpeErrStr(errno));
            /* P2-4: do NOT silently fallback PERCPU_ARRAY to ARRAY. The BPF
             * programs use direct read-modify-write on percpu values assuming
             * per-CPU isolation; an ARRAY fallback would introduce cross-CPU
             * data races on every counter increment. Instead, fail loudly and
             * let the caller degrade to polling mode. */
            if (m.type == BPF_MAP_TYPE_PERCPU_ARRAY) {
                LOG_FORCE("embedded: PERCPU_ARRAY map[%d] unavailable — cannot safely fallback to ARRAY (would cause counter races). Degrading to polling.", i);
            }
            // P3-11: removed the redundant inner `if (fd < 0)` — we are already
            // inside the fd<0 block. Fail loudly and roll back the maps created
            // so far.
            LOG_FORCE("embedded: map[%d] create FAILED type=%u errno=%d (%s) — all maps exhausted",
                      i, m.type, errno, aatpeErrStr(errno));
            for (int existing : mapFds_) close(existing);
            mapFds_.clear();
            state_ = aatpe::classifyEbpfFailure(errno, false, false);   // ROUND-014 P3
            return false;
        }
        mapFds_.push_back(fd);
    }
    eventsMapFd_ = (mapFds_.size() > 0) ? mapFds_[0] : -1;
    countersMapFd_ = (mapFds_.size() > 1) ? mapFds_[1] : -1;
    /* P2-1: thread_cache removed; mapFds_[2] is now wakeup_cache.
     * wakeupCacheMapFd_ is not exposed as a named member (BPF manages it
     * internally); we keep threadCacheMapFd_ = -1 for back-compat. */
    threadCacheMapFd_ = -1;
    LOG_INFO("embedded: created %d maps (events=%d counters=%d wakeup_cache=%d)",
                (int)mapFds_.size(), eventsMapFd_, countersMapFd_,
                (mapFds_.size() > 2) ? mapFds_[2] : -1);

    // Step 3: Load each program and attach (KPROBE优先, TRACEPOINT备选)
    if (loadTimeoutExceeded(deadline)) { state_ = aatpe::classifyEbpfFailure(EACCES, false, false); return false; }

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus <= 0) ncpus = 8;
    // #24: parse real online CPU ids (not just count) for sparse topology.
    onlineCpuIds_ = readOnlineCpuIds();
    LOG_INFO("embedded: online CPU ids: %zu (ncpus count=%ld)",
             onlineCpuIds_.size(), ncpus);

    LOG_INFO("embedded: loading %d programs on %ld CPUs (prog_type from embedded header)",
             kEmbeddedProgramCount, ncpus);
    LOG_INFO("embedded: license=\"%s\" kern_version=0x%08x",
             kEmbeddedLicense, computeKernelVersionCode());

    int progsLoaded = 0;
    int firstProgType = -1;
    for (int pi = 0; pi < kEmbeddedProgramCount; pi++) {
        if (loadTimeoutExceeded(deadline)) break;
        const auto& info = kEmbeddedPrograms[pi];

        // Determine program type: use embedded header's progType, but try
        // KPROBE first (if supported), then TRACEPOINT as fallback.
        int desiredType = info.progType;
        int altType = (desiredType == BPF_PROG_TYPE_KPROBE) ?
                       BPF_PROG_TYPE_TRACEPOINT : BPF_PROG_TYPE_KPROBE;

        // If minimal KPROBE probe failed, skip KPROBE and go straight to
        // TRACEPOINT to avoid wasting time on known-bad type.
        if (desiredType == BPF_PROG_TYPE_KPROBE && !kprobeTypeSupported) {
            LOG_INFO("embedded: prog[%d] KPROBE type known-unsupported, trying TRACEPOINT first", pi);
            desiredType = BPF_PROG_TYPE_TRACEPOINT;
            altType = BPF_PROG_TYPE_KPROBE;
        }

        LOG_INFO("embedded: prog[%d] section=%s func=%s is_ret=%d desired_type=%d alt_type=%d",
                 pi, info.sectionName, info.kprobeFunc, info.isKretprobe,
                 desiredType, altType);

        // Build explicit relocation arrays from embedded header refs.
        // These give exact byte offsets and map indices for LD_IMM patching.
        // This is REQUIRED because legacy bpf_map_def bytecode uses src_reg=0
        // (not PSEUDO_MAP_FD src_reg=1), so the sequential scanner cannot
        // auto-detect map references.
        std::vector<int> refOffsets;
        std::vector<int> refMapIndices;
        refOffsets.reserve(info.refCount);
        refMapIndices.reserve(info.refCount);
        for (int ri = 0; ri < info.refCount; ri++) {
            refOffsets.push_back(info.refs[ri].insnOffset);
            refMapIndices.push_back(info.refs[ri].mapIndex);
        }

        // Try desired type first, then alt type, then RAW_TRACEPOINT.
        // All loads use explicit-offset map FD patching.
        // License comes from the embedded header (extracted from the .o's
        // license section) — MUST be GPL for 4.14 when using bpf_perf_event_output.
        int progFd = loadBpfProgFromBytesWithExplicitRefs(
            info.bytecode, info.bytecodeSize, kEmbeddedLicense,
            refOffsets.data(), refMapIndices.data(), info.refCount,
            mapFds_, desiredType);
        int loadedType = desiredType;
        if (progFd < 0) {
            LOG_WARN("embedded: prog[%d] load with type=%d failed errno=%d (%s), trying type=%d",
                     pi, desiredType, errno, aatpeErrStr(errno), altType);
            progFd = loadBpfProgFromBytesWithExplicitRefs(
                info.bytecode, info.bytecodeSize, kEmbeddedLicense,
                refOffsets.data(), refMapIndices.data(), info.refCount,
                mapFds_, altType);
            loadedType = altType;
        }
        if (progFd < 0) {
            // Last resort: RAW_TRACEPOINT (type=17) — supported on 4.17+
            progFd = loadBpfProgFromBytesWithExplicitRefs(
                info.bytecode, info.bytecodeSize, kEmbeddedLicense,
                refOffsets.data(), refMapIndices.data(), info.refCount,
                mapFds_, BPF_PROG_TYPE_RAW_TRACEPOINT);
            loadedType = BPF_PROG_TYPE_RAW_TRACEPOINT;
        }
        if (progFd < 0) {
            LOG_ERROR("embedded: prog[%d] (%s) ALL load types failed (last errno=%d %s)",
                      pi, info.sectionName, errno, aatpeErrStr(errno));
            continue;
        }
        if (firstProgType < 0) firstProgType = loadedType;
        LOG_INFO("embedded: prog[%d] loaded successfully type=%d (fd=%d, %d insns)",
                 pi, loadedType, progFd, info.bytecodeSize / 8);
        progFds_.push_back(progFd);

        // Attach based on loaded program type
        int attached = 0;
        bool useKprobe = (loadedType == BPF_PROG_TYPE_KPROBE);
        // #21: RAW_TRACEPOINT programs must be attached via BPF_RAW_TRACEPOINT_OPEN,
        // NOT perf_event_open+SET_BPF (which always returns EINVAL/EOPNOTSUPP for
        // raw_tp prog fds). The old code fell through to the tracepoint branch and
        // silently failed all per-CPU attaches, then reported overall failure.
        bool useRawTp = (loadedType == BPF_PROG_TYPE_RAW_TRACEPOINT);

        if (useRawTp) {
            // #21: correct BPF_RAW_TRACEPOINT_OPEN attach path.
            // Extract the raw tracepoint event name from sectionName
            // ("tracepoint/sched/sched_switch" -> "sched_switch").
            std::string sec(info.sectionName);
            std::string tpName;
            size_t slash = sec.find_last_of('/');
            if (slash != std::string::npos) tpName = sec.substr(slash + 1);
            else tpName = sec;

            union bpf_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.raw_tracepoint.name = (uint64_t)(unsigned long)tpName.c_str();
            attr.raw_tracepoint.prog_fd = (uint32_t)progFd;
            int linkFd = bpfSyscall(BPF_RAW_TRACEPOINT_OPEN, &attr, sizeof(attr));
            if (linkFd < 0) {
                LOG_WARN("embedded: prog[%d] RAW_TRACEPOINT_OPEN(name=%s) failed errno=%d (%s)",
                         pi, tpName.c_str(), errno, aatpeErrStr(errno));
                close(progFd);
                progFds_.pop_back();
                continue;
            }
            rawTracepointLinks_.push_back(linkFd);
            LOG_INFO("embedded: prog[%d] RAW_TRACEPOINT attached name=%s linkFd=%d",
                     pi, tpName.c_str(), linkFd);

            // Set up per-CPU ring buffers (independent software perf events).
            if (pi == 0) {
                // tpId=0, attachFd=-1, attachCpu=-1: raw_tp is global, no perf event.
                setupPerCpuRingBuffers(0, -1, -1, (int)ncpus, pi);
            }
            attached = 1;
        } else if (useKprobe) {
            // KPROBE attach — LED-022 (R2-007): single SET_BPF attach + software
            // rings, same model as tracepoints. Previously opened per-CPU perf
            // events for every program but only mmapped pi==0; the rest were
            // enabled-but-unconsumed.
            LOG_FORCE("embedded: prog[%d] using KPROBE attach (func=%s is_ret=%d)",
                     pi, info.kprobeFunc, info.isKretprobe);
            std::string eventName;
            int kpId = ensureKprobeEvent(info.kprobeFunc, info.isKretprobe, eventName);
            if (kpId <= 0) {
                LOG_FORCE("embedded: prog[%d] KPROBE ensure event FAILED func=%s", pi, info.kprobeFunc);
            } else {
                int attachFd = -1;
                int attachCpu = -1;
                for (int cpu = 0; cpu < ncpus; cpu++) {
                    if (loadTimeoutExceeded(deadline)) break;
                    int perfFd = -1;
                    if (attachTracepoint(progFd, kpId, cpu, perfFd)) {
                        attachFd = perfFd;
                        attachCpu = cpu;
                        break;
                    }
                }
                if (attachFd >= 0) {
                    tracepointFds_.push_back(attachFd);
                    if (pi == 0) {
                        setupPerCpuRingBuffers(kpId, attachFd, attachCpu, (int)ncpus, pi);
                    }
                    attached = 1;
                } else {
                    LOG_FORCE("embedded: prog[%d] KPROBE attach FAILED (kpId=%d)", pi, kpId);
                }
            }
        } else {
            // TRACEPOINT attach — 4.14-safe single-attach + per-CPU rings.
            // On 4.14, SET_BPF on the same tracepoint twice returns EEXIST,
            // so we attach once and open ring-buffer-only events for other CPUs.
            int tpId = readTracepointId(info.sectionName);
            if (tpId <= 0) {
                LOG_WARN("embedded: tracepoint id not found for %s errno=%d (%s)",
                         info.sectionName, errno, aatpeErrStr(errno));
                continue;
            }
            // Try to attach on each CPU until one succeeds (SET_BPF works once)
            int attachFd = -1;
            int attachCpu = -1;
            for (int cpu = 0; cpu < ncpus; cpu++) {
                if (loadTimeoutExceeded(deadline)) break;
                int perfFd = -1;
                if (attachTracepoint(progFd, tpId, cpu, perfFd)) {
                    attachFd = perfFd;
                    attachCpu = cpu;
                    break;
                }
            }
            if (attachFd < 0) {
                LOG_WARN("embedded: prog[%d] tracepoint attach FAILED on all CPUs (tpId=%d)",
                         pi, tpId);
                continue;
            }
            tracepointFds_.push_back(attachFd);
            // Set up per-CPU ring buffers (only first program owns the rings)
            if (pi == 0) {
                setupPerCpuRingBuffers(tpId, attachFd, attachCpu, (int)ncpus, pi);
            }
            attached = 1;  // single global attach covers all CPUs
        }

        if (attached > 0) progsLoaded++;
        LOG_INFO("embedded: prog[%d] %s attached on %d/%ld CPUs (method=%s)",
                 pi, info.sectionName, attached, ncpus,
                 useKprobe ? "kprobe" : "tracepoint");
    }

    if (progsLoaded == 0 || rings_.empty()) {
        LOG_FORCE("embedded: FAILED — no programs attached or no rings mapped (progsLoaded=%d rings=%zu)",
                  progsLoaded, rings_.size());
        return false;
    }

    activeProgType_ = firstProgType;
    // #21: report the ACTUAL attach method used, not a lie.
    if (firstProgType == BPF_PROG_TYPE_KPROBE) activeAttachMethod_ = "kprobe";
    else if (firstProgType == BPF_PROG_TYPE_RAW_TRACEPOINT) activeAttachMethod_ = "raw_tracepoint";
    else activeAttachMethod_ = "tracepoint";
    loaderType_ = EbpfLoaderType::EMBEDDED_SYSCALL;
    mode_ = EbpfMode::EVENT_DRIVEN;
    active_ = true;
    state_ = EbpfState::EVENT_DRIVEN;   // ROUND-014 P3: seven-state ladder
    // final-review fix (postLoadOffsetDegrade timing): run the offset verdict
    // BEFORE the poll thread starts. Previously postLoadOffsetDegradeDecision()
    // ran in load() AFTER startPollThread(), so deliverSwitchEvent()'s
    // second-layer offset filter observed offsetDegraded_ /
    // offsetUnverifiedButTrusted_ only with a delay — early UNKNOWN events were
    // conservatively dropped in that gap. The decision now runs here and gates
    // thread start.
    postLoadOffsetDegradeDecision();
    if (offsetDegraded_.load(std::memory_order_relaxed)) {
        // The decision already latched mode_=POLLING / state_=DEGRADED_RING and
        // called stopPollThread() (no-op here: the thread has not started).
        // Rings stay mapped but undrained — starting a consumer for proven-wrong
        // ctx offsets would only drain garbage, so skip the thread entirely.
        unloaded_.store(false, std::memory_order_relaxed);  // EBPF-04 guard: loaded (degraded)
        LOG_WARN("eBPF: offset-degraded after attach (embedded); poll thread NOT "
                 "started (early-UNKNOWN event-drop window eliminated)");
        return true;
    }
    // EBPF-04 fix: if the poll thread cannot start, rings_ cannot be drained.
    // Previously we called degrade() and returned true, leaving rings mapped
    // with no consumer (silent overflow). Now: tear down everything we just
    // built and return false so the outer load() tries the next phase; if all
    // phases fail, the caller lands in pure POLLING.
    if (!startPollThread()) {
        LOG_ERROR("eBPF: poll thread start failed (embedded) — tearing down, "
                  "falling through to next load phase");
        for (int f : progFds_) close(f);
        progFds_.clear();
        for (int f : mapFds_) close(f);
        mapFds_.clear();
        unmapAllRings();
        for (int f : tracepointFds_) close(f);
        tracepointFds_.clear();
        eventsMapFd_ = -1;
        countersMapFd_ = -1;
        threadCacheMapFd_ = -1;
        active_ = false;
        state_ = EbpfState::LOAD_FAILED;
        return false;
    }

    // DIAGNOSTIC: Read BPF event_counters map immediately after load to verify
    // map access works. The caller's main loop will read counters periodically
    // to verify BPF progs are actually firing.
    if (countersMapFd_ >= 0) {
        std::vector<uint64_t> initialCounts;
        if (readEmbeddedCpuCounts(initialCounts)) {
            uint64_t sum = 0;
            for (uint64_t v : initialCounts) sum += v;
            LOG_INFO("[eBPF-DIAG] event_counters map read OK: %zu CPUs, total=%llu (expected ~0 at load)",
                     initialCounts.size(), (unsigned long long)sum);
        } else {
            LOG_WARN("[eBPF-DIAG] event_counters map read FAILED (countersMapFd=%d)", countersMapFd_);
        }
    }

    // DIAGNOSTIC: Log ring buffer states immediately after setup
    for (const auto& ring : rings_) {
        if (ring.base) {
            auto* hdr = reinterpret_cast<volatile struct perf_event_mmap_page*>(ring.base);
            LOG_INFO("[eBPF-DIAG] ring cpu=%d fd=%d base=%p head=%llu tail=%llu",
                     ring.cpu, ring.perfFd, ring.base,
                     (unsigned long long)hdr->data_head,
                     (unsigned long long)hdr->data_tail);
        }
    }

    // CRITICAL DIAGNOSTIC: Actively trigger scheduling events and read BPF-side
    // event_counters to verify the BPF programs are actually firing. This is the
    // definitive test for "events_map works but events_total=0":
    //   - If BPF counters increase after sched_yield, the BPF prog is attached
    //     and bpf_perf_event_output is being called. If user-space still sees 0
    //     events, the ring buffer read path is broken.
    //   - If BPF counters stay at 0, the BPF prog is NOT firing (attach issue,
    //     tracepoint not enabled, or wrong tracepoint ID).
    if (countersMapFd_ >= 0) {
        std::vector<uint64_t> countsBefore;
        if (readEmbeddedCpuCounts(countsBefore)) {
            uint64_t beforeTotal = 0;
            for (uint64_t v : countsBefore) beforeTotal += v;

            // Trigger scheduling switches on multiple CPUs
            for (int i = 0; i < 32; i++) {
                sched_yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::vector<uint64_t> countsAfter;
            if (readEmbeddedCpuCounts(countsAfter)) {
                uint64_t afterTotal = 0;
                for (uint64_t v : countsAfter) afterTotal += v;
                uint64_t delta = afterTotal - beforeTotal;
                LOG_INFO("[eBPF-DIAG] BPF self-test: sched_yield x32, counters before=%llu after=%llu delta=%llu",
                         (unsigned long long)beforeTotal,
                         (unsigned long long)afterTotal,
                         (unsigned long long)delta);
                if (delta > 0) {
                    LOG_INFO("[eBPF-DIAG] BPF self-test PASSED: BPF programs are firing (delta=%llu events)",
                             (unsigned long long)delta);
                } else {
                    LOG_WARN("[eBPF-DIAG] BPF self-test FAILED: counters did not increase after sched_yield x32. "
                             "BPF programs may not be attached/firing. Check tracepoint enable state.");
                }
            }
        }
    }

    LOG_FORCE("=== tryLoadEmbeddedSyscall SUCCESS (%d progs, %zu rings, prog_type=%d, attach=%s) ===",
                progsLoaded, rings_.size(), activeProgType_, activeAttachMethod_.c_str());
    unloaded_.store(false, std::memory_order_relaxed);  // EBPF-04: clear between-phase unload() guard
    return true;
}

// ============================================================================
// Method 2: syscall_precompiled — parse .o ELF, load via bpf() syscall
// ============================================================================
bool EbpfScheduler::parseElfPrograms(const std::string& path,
                                      std::vector<std::vector<uint8_t>>& outPrograms,
                                      std::vector<std::string>& outTpPaths,
                                      std::vector<std::vector<int>>& outMapIndices,
                                      std::string& outLicense) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    // LED-023 (R2-008): check gcount() — a truncated .o leaves trailing zeros
    // that could satisfy offset checks and lead to deeper parsing.
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (f.gcount() != (std::streamsize)size) {
        LOG_WARN("parseElfPrograms: short read on %s — expected %zu bytes, got %ld",
                 path.c_str(), size, (long)f.gcount());
        f.close();
        return false;
    }
    f.close();

    if (size < 64 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        return false;

    // Parse ELF64 header. P2-7: the input is a byte vector that is NOT guaranteed
    // aligned; reinterpret_cast<uintXX_t*> + deref is UB (strict aliasing +
    // unaligned access). Use memcpy into properly aligned locals.
    auto rd16 = [&](size_t off) -> uint16_t { uint16_t v; memcpy(&v, data.data()+off, 2); return v; };
    auto rd32 = [&](size_t off) -> uint32_t { uint32_t v; memcpy(&v, data.data()+off, 4); return v; };
    auto rd64 = [&](size_t off) -> uint64_t { uint64_t v; memcpy(&v, data.data()+off, 8); return v; };

    // v4.4.9 R3-F02: bounded C-string reader for ELF string tables (shstrtab /
    // .strtab). A corrupt/tampered .o can hand us an index that lies inside the
    // declared table size but whose bytes run to the END of the table with no
    // NUL terminator. The old code did std::string((const char*)ptr), which
    // keeps walking PAST the table — and potentially past the whole `data`
    // vector — until it happens to find a NUL (heap OOB read). Scan at most
    // `rem` bytes; return the NUL offset (<=rem) when terminated, or
    // npos when the run is unterminated within the table (caller rejects).
    static const size_t kNoNul = static_cast<size_t>(-1);
    auto cstrLenWithin = [&](const uint8_t* p, size_t rem) -> size_t {
        for (size_t i = 0; i < rem; i++) {
            if (p[i] == '\0') return i;
        }
        return kNoNul;
    };

    uint64_t e_shoff = rd64(40);
    uint16_t e_shentsize = rd16(58);
    uint16_t e_shnum = rd16(60);
    uint16_t e_shstrndx = rd16(62);

    if (e_shoff == 0 || e_shnum == 0 || e_shentsize < 64 ||
        e_shoff + (uint64_t)e_shnum * e_shentsize > size)
        return false;
    // P2-8: e_shstrndx must be a valid section index. SHN_UNDEF(0) / out-of-range
    // means there is no string table section -> reject before dereferencing it.
    if (e_shstrndx == 0 || e_shstrndx >= e_shnum)
        return false;

    // Section header string table
    uint64_t shstrHdrOff = e_shoff + (uint64_t)e_shstrndx * e_shentsize;
    if (shstrHdrOff + 64 > size) return false;
    uint64_t shstrOff = rd64(shstrHdrOff + 24);
    uint64_t shstrSize = rd64(shstrHdrOff + 32);
    if (shstrOff + shstrSize > size) return false;
    const uint8_t* shstr = data.data() + shstrOff;

    // First pass: find .symtab and .strtab, and all section info
    int symtabIdx = -1, strtabIdx = -1;
    struct SectionInfo {
        std::string name;
        uint32_t type;
        uint64_t offset;
        uint64_t size;
        uint32_t link;       // sh_link
        uint32_t info;       // sh_info
        uint64_t entsize;
    };
    std::vector<SectionInfo> sections;
    sections.reserve(e_shnum);

    for (int i = 0; i < e_shnum; i++) {
        uint64_t off = e_shoff + (uint64_t)i * e_shentsize;
        if (off + 64 > size) break;
        SectionInfo si;
        uint32_t sh_name = rd32(off);
        si.type = rd32(off + 4);
        si.offset = rd64(off + 24);
        si.size = rd64(off + 32);
        si.link = rd32(off + 40);
        si.info = rd32(off + 44);
        si.entsize = rd64(off + 56);
        // P1-2 bounds check: sh_name is a byte offset INTO the shstr section.
        // A corrupt ELF can point it past the end of the string table; reading
        // shstr + sh_name then walks out of the mapped file buffer. Reject the
        // section (empty name) and continue rather than OOB-read.
        if (sh_name >= shstrSize) {
            si.name.clear();
            sections.push_back(si);
            continue;
        }
        // v4.4.9 R3-F02: sh_name is in-bounds of the shstr table, but the name
        // must be NUL-terminated WITHIN the table. If it runs to the end of the
        // table with no NUL, the old std::string((const char*)) walked past the
        // table — and potentially past the whole `data` vector — (heap OOB
        // read). Bound the read to the remaining table bytes and reject the
        // object when no NUL is found in bounds.
        size_t shRem = shstrSize - sh_name;
        size_t shLen = cstrLenWithin(shstr + sh_name, shRem);
        if (shLen == kNoNul) {
            LOG_WARN("parseElfPrograms: shstrtab name at offset %u not NUL-terminated within %zu bytes — reject corrupt .o",
                     (unsigned)sh_name, (size_t)shstrSize);
            return false;
        }
        si.name.assign(reinterpret_cast<const char*>(shstr + sh_name), shLen);
        sections.push_back(si);
        if (si.type == 2 /*SHT_SYMTAB*/) symtabIdx = i;
        if (si.name == ".strtab") strtabIdx = i;
    }

    // Build symbol name lookup: symbol index -> name string
    // Map symbol name -> map index (0=events_map, 1=event_counters, 2=wakeup_cache)
    // P2-1: thread_cache removed (dead map).
    auto mapNameToIndex = [](const std::string& symName) -> int {
        if (symName == "events_map") return 0;
        if (symName == "event_counters") return 1;
        if (symName == "wakeup_cache") return 2;
        return -1;
    };

    std::vector<int> symMapIndex;  // sym idx -> map index (-1 if not a map)
    if (symtabIdx >= 0 && strtabIdx >= 0 &&
        symtabIdx < (int)sections.size() && strtabIdx < (int)sections.size()) {
        const SectionInfo& symtab = sections[symtabIdx];
        const SectionInfo& strtab = sections[strtabIdx];
        if (symtab.offset + symtab.size <= size && strtab.offset + strtab.size <= size) {
            size_t symEntSize = symtab.entsize ? symtab.entsize : 24;
            size_t numSyms = symtab.size / symEntSize;
            symMapIndex.resize(numSyms, -1);
            for (size_t s = 0; s < numSyms; s++) {
                uint64_t symOff = symtab.offset + s * symEntSize;
                if (symOff + 24 > size) break;
                uint32_t st_name = rd32(symOff);  // P3: memcpy, no unaligned deref
                if (st_name < strtab.size) {
                    // v4.4.9 R3-F02: bound the symbol name to the .strtab table.
                    // strtab.offset+strtab.size<=size is already checked, so
                    // `sp` is inside `data`; require a NUL within [st_name,size)
                    // and reject the object otherwise (old std::string could walk
                    // past the table / past the data vector — heap OOB read).
                    size_t stRem = strtab.size - st_name;
                    const uint8_t* sp = data.data() + strtab.offset + st_name;
                    size_t stLen = cstrLenWithin(sp, stRem);
                    if (stLen == kNoNul) {
                        LOG_WARN("parseElfPrograms: .strtab symbol name at st_name=%u not NUL-terminated within %zu bytes — reject corrupt .o",
                                 (unsigned)st_name, (size_t)strtab.size);
                        return false;
                    }
                    symMapIndex[s] = mapNameToIndex(
                        std::string(reinterpret_cast<const char*>(sp), stLen));
                }
            }
        }
    }

    outLicense = "GPL";
    outPrograms.clear();
    outTpPaths.clear();
    outMapIndices.clear();

    // Second pass: extract programs and parse relocations
    for (int i = 0; i < (int)sections.size(); i++) {
        const SectionInfo& si = sections[i];
        bool isBpfProgSection = (si.name.rfind("tracepoint/", 0) == 0 ||
                                  si.name.rfind("kprobe/", 0) == 0 ||
                                  si.name.rfind("kretprobe/", 0) == 0);
        if (si.type == 1 /*SHT_PROGBITS*/ && isBpfProgSection
            && si.size > 0 && si.offset + si.size <= size) {
            std::vector<uint8_t> prog(data.data() + si.offset,
                                       data.data() + si.offset + si.size);
            // EBPF-20: count PSEUDO_MAP_FD refs BEFORE std::move(prog) empties
            // the local — this count feeds the no-.rel fallback below.
            size_t pseudoRefCount = 0;
            for (size_t off = 0; off + 8 <= prog.size(); off += 8) {
                if (prog[off] == 0x18 && ((prog[off + 1] >> 4) & 0x0F) == 1) {
                    pseudoRefCount++;
                }
            }
            outPrograms.push_back(std::move(prog));
            outTpPaths.push_back(si.name);

            // Parse corresponding .rel<section> section for map relocations
            std::vector<int> mapIndices;
            std::string relSecName = ".rel" + si.name;
            for (int j = 0; j < (int)sections.size(); j++) {
                if (sections[j].name == relSecName &&
                    sections[j].type == 9 /*SHT_REL*/ &&
                    sections[j].offset + sections[j].size <= size) {
                    const SectionInfo& rel = sections[j];
                    size_t relEntSize = rel.entsize ? rel.entsize : 16;
                    size_t numRels = rel.size / relEntSize;
                    for (size_t r = 0; r < numRels; r++) {
                        uint64_t relOff = rel.offset + r * relEntSize;
                        if (relOff + 16 > size) break;
                        // Elf64_Rel: r_offset (8), r_info (8) — memcpy, no unaligned deref
                        uint64_t r_info = rd64(relOff + 8);
                        uint32_t symIdx = (uint32_t)(r_info >> 32);
                        if (symIdx < symMapIndex.size()) {
                            mapIndices.push_back(symMapIndex[symIdx]);
                        } else {
                            mapIndices.push_back(-1);
                        }
                    }
                    break;
                }
            }
            // If no relocation section found, fall back to sequential (0,1,2,...)
            // EBPF-20 fix: count the ACTUAL number of PSEUDO_MAP_FD instructions
            // (BPF_LD class 0x18 with src_reg==1) in this program section instead
            // of unconditionally pushing 0..7. The old hardcoded 8 was wrong in
            // both directions: a program with >8 map refs ran off the end of the
            // list and failed load with "unresolved map reference", while one
            // with 0 refs carried 8 bogus entries. Without .rel we cannot know
            // WHICH map each reference wants (only symbols in a .rel tell us),
            // so sequential 0..N-1 remains a best-effort guess for corrupt /
            // hand-built objects only; production .o files always ship .rel.
            if (mapIndices.empty()) {
                // pseudoRefCount was captured above, before std::move(prog).
                for (size_t k = 0; k < pseudoRefCount; k++) mapIndices.push_back((int)k);
            }
            outMapIndices.push_back(std::move(mapIndices));
        }
        if (si.name == "license" && si.size > 0 && si.offset + si.size <= size) {
            outLicense = std::string(reinterpret_cast<const char*>(data.data() + si.offset),
                                      si.size);
            while (!outLicense.empty() && outLicense.back() == '\0')
                outLicense.pop_back();
        }
    }
    return !outPrograms.empty();
}

bool EbpfScheduler::tryLoadSyscallPrecompiled() {
    // fix-v96 #8: resolve the module root ONCE via getModuleDir() (Magisk /
    // KernelSU / APatch) instead of hardcoding /data/adb/modules/aatpe.
    std::string modRoot = getModuleDir();
    std::vector<std::string> pathVec = {
        "/system/etc/aatpe/ebpf/sched_events.bpf.o",
        modRoot + "/system/etc/aatpe/ebpf/sched_events.bpf.o",
        "/system/etc/aatpe/sched_events.bpf.o",
        modRoot + "/sched_events.bpf.o",
    };
    std::vector<const char*> paths;
    for (const auto& p : pathVec) paths.push_back(p.c_str());
    paths.push_back(nullptr);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kLoadTimeoutMs);

    for (int pi = 0; paths[pi]; pi++) {
        struct stat st;
        if (stat(paths[pi], &st) != 0) continue;
        LOG_INFO("syscall_precompiled: trying %s", paths[pi]);

        std::vector<std::vector<uint8_t>> programs;
        std::vector<std::string> tpPaths;
        std::vector<std::vector<int>> progMapIndices;
        std::string license;
        if (!parseElfPrograms(paths[pi], programs, tpPaths, progMapIndices, license)) {
            LOG_WARN("syscall_precompiled: ELF parse failed for %s errno=%d (%s)", paths[pi], errno, aatpeErrStr(errno));
            continue;
        }

        // Create maps (same layout as embedded: events, counters, cache, switch_sample)
        // LED-017 (R2-002): precompiled path must match the 4-map layout embedded in
        // sched_events_embedded.h. Previously it hardcoded only 3 maps with wrong
        // max_entries (counters=10 vs 11, wakeup_cache=4096 vs 8192) and omitted the
        // 4th PERCPU_ARRAY switch_sample_counter (max=1). Unknown map references were
        // silently patched to mapFds[0] (events_map, PERF_EVENT_ARRAY), which the
        // verifier rejects because the program does map_lookup+write on it.
        mapFds_.clear();
        int fd;
        fd = createMap(BPF_MAP_TYPE_PERF_EVENT_ARRAY, 4, 4, 256);
        if (fd < 0) continue;
        mapFds_.push_back(fd);
        eventsMapFd_ = fd;

        // v6.0.0 P3-4: max_entries MUST be 12, matching the other two source-of-
        // truth sites (ebpf/sched_events.bpf.c:206 and
        // ebpf/sched_events_embedded.h kEmbeddedMaps[1]). The precompiled fallback
        // had 11 while the BPF side grew 11->12 for COUNTER_WAKEUP_NOT_SUCCESS=11;
        // an out-of-range map_lookup_elem on index 11 would fail inside the kernel
        // and silently drop the not-success counter on the fallback path.
        fd = createMap(BPF_MAP_TYPE_PERCPU_ARRAY, 4, 8, 12);  /* event_counters: 12 counters (matches bpf.c + embedded.h) */
        if (fd < 0) {
            /* P2-4: no silent ARRAY fallback for PERCPU_ARRAY */
            LOG_FORCE("precompiled: PERCPU_ARRAY counters map create FAILED, cannot safely fallback");
            close(eventsMapFd_); mapFds_.clear(); continue;
        }
        mapFds_.push_back(fd);
        countersMapFd_ = fd;

        /* P2-1: was thread_cache (dead). Now creates wakeup_cache:
         * key=tid(u32), value=wakeup_rec(24 bytes: ktime+waker_tid+target_cpu+pad)
         * v4.5.0 F-03: prefer LRU_HASH for the in-kernel auto-reaper; fall back to
         * plain HASH on kernels that reject the LRU map type (EINVAL). */
        fd = createMap(BPF_MAP_TYPE_LRU_HASH, 4, 24, 8192);
        if (fd < 0 && wakeupCacheMapDecision(fd, errno) == 1) {
            LOG_WARN("syscall_precompiled: wakeup_cache LRU_HASH rejected errno=%d "
                     "(%s); falling back to plain HASH", errno, aatpeErrStr(errno));
            fd = createMap(BPF_MAP_TYPE_HASH, 4, 24, 8192);
        }
        if (fd < 0) { for (int f : mapFds_) close(f); mapFds_.clear(); continue; }
        mapFds_.push_back(fd);
        threadCacheMapFd_ = fd;  /* repurposed: fd for wakeup_cache at index 2 */

        /* LED-017: switch_sample_counter (PERCPU_ARRAY, key=4, value=8, max=1).
         * The sched_switch BPF program does a 1/10 sampling gate on this map.
         * Without it, relocation for map index 3 was patched to events_map fd
         * and the verifier rejected the load. */
        fd = createMap(BPF_MAP_TYPE_PERCPU_ARRAY, 4, 8, 1);
        if (fd < 0) { for (int f : mapFds_) close(f); mapFds_.clear(); continue; }
        mapFds_.push_back(fd);

        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpus <= 0) ncpus = 8;
        onlineCpuIds_ = readOnlineCpuIds();  // #24
        int progsLoaded = 0;
        for (size_t i = 0; i < programs.size() && i < tpPaths.size(); i++) {
            if (loadTimeoutExceeded(deadline)) break;
            const std::vector<int>& mapIdx = (i < progMapIndices.size()) ?
                progMapIndices[i] : std::vector<int>();

            // Determine program type from section name
            const std::string& secName = tpPaths[i];
            bool isKprobe = (secName.rfind("kprobe/", 0) == 0 ||
                             secName.rfind("kretprobe/", 0) == 0);
            bool isKretprobe = (secName.rfind("kretprobe/", 0) == 0);
            int progType = isKprobe ? BPF_PROG_TYPE_KPROBE : BPF_PROG_TYPE_TRACEPOINT;

            // Extract kernel function name from section
            std::string funcName;
            if (isKretprobe) funcName = secName.substr(strlen("kretprobe/"));
            else if (isKprobe) funcName = secName.substr(strlen("kprobe/"));

            LOG_INFO("syscall_precompiled: prog[%zu] section=%s type=%d func=%s is_ret=%d",
                     i, secName.c_str(), progType, funcName.c_str(), isKretprobe);

            int progFd = loadBpfProgFromBytesWithRefs(programs[i].data(), programs[i].size(),
                                                       license.c_str(), mapFds_, mapIdx,
                                                       progType);
            if (progFd < 0) {
                // Try alt type
                int altType = isKprobe ? BPF_PROG_TYPE_TRACEPOINT : BPF_PROG_TYPE_KPROBE;
                LOG_WARN("syscall_precompiled: prog[%zu] type=%d load failed errno=%d, trying type=%d",
                         i, progType, errno, altType);
                progFd = loadBpfProgFromBytesWithRefs(programs[i].data(), programs[i].size(),
                                                       license.c_str(), mapFds_, mapIdx,
                                                       altType);
                if (progFd >= 0) {
                    isKprobe = (altType == BPF_PROG_TYPE_KPROBE);
                    progType = altType;
                }
            }
            if (progFd < 0) {
                LOG_WARN("syscall_precompiled: prog[%zu] (%s) load failed errno=%d (%s)",
                         i, secName.c_str(), errno, aatpeErrStr(errno));
                continue;
            }
            progFds_.push_back(progFd);

            int attached = 0;
            if (isKprobe && !funcName.empty()) {
                // KPROBE attach — LED-022 (R2-007): converge to the same
                // single-attach + software-ring model tracepoints use. Previously
                // we opened perf_event_open+SET_BPF+ENABLE for EVERY program × EVERY
                // CPU, but only pi==0's fds were mmapped and written to events_map.
                // The other N×(ncpus-1) perf events were enabled but never consumed,
                // wasting fds and kernel perf context. kprobes (like tracepoints on
                // 4.14) are globally attached by a single SET_BPF; the per-CPU
                // software output rings are set up by setupPerCpuRingBuffers.
                int kpId = -1;
                {
                    std::string eventName;
                    kpId = ensureKprobeEvent(funcName.c_str(), isKretprobe, eventName);
                }
                if (kpId <= 0) {
                    LOG_WARN("syscall_precompiled: kprobe ensure event failed for %s", funcName.c_str());
                    continue;
                }
                int attachFd = -1;
                int attachCpu = -1;
                for (int cpu = 0; cpu < ncpus; cpu++) {
                    if (loadTimeoutExceeded(deadline)) break;
                    int perfFd = -1;
                    if (attachTracepoint(progFd, kpId, cpu, perfFd)) {
                        attachFd = perfFd;
                        attachCpu = cpu;
                        break;
                    }
                }
                if (attachFd < 0) {
                    LOG_WARN("syscall_precompiled: kprobe attach FAILED func=%s kpId=%d",
                             funcName.c_str(), kpId);
                    continue;
                }
                tracepointFds_.push_back(attachFd);
                if (i == 0) {
                    setupPerCpuRingBuffers(kpId, attachFd, attachCpu, (int)ncpus, (int)i);
                }
                attached = 1;
            } else {
                // TRACEPOINT attach — 4.14-safe single-attach + per-CPU rings
                int tpId = readTracepointId(secName.c_str());
                if (tpId <= 0) {
                    LOG_WARN("syscall_precompiled: tracepoint id not found for %s", secName.c_str());
                    continue;
                }
                int attachFd = -1;
                int attachCpu = -1;
                for (int cpu = 0; cpu < ncpus; cpu++) {
                    if (loadTimeoutExceeded(deadline)) break;
                    int perfFd = -1;
                    if (attachTracepoint(progFd, tpId, cpu, perfFd)) {
                        attachFd = perfFd;
                        attachCpu = cpu;
                        break;
                    }
                }
                if (attachFd < 0) {
                    LOG_WARN("syscall_precompiled: prog[%zu] tracepoint attach FAILED (tpId=%d)",
                             i, tpId);
                    continue;
                }
                tracepointFds_.push_back(attachFd);
                if (i == 0) {
                    setupPerCpuRingBuffers(tpId, attachFd, attachCpu, (int)ncpus, (int)i);
                }
                attached = 1;  // single global attach covers all CPUs
            }
            if (attached > 0) progsLoaded++;
            LOG_INFO("syscall_precompiled: prog[%zu] %s attached on %d/%ld CPUs (%s)",
                     i, secName.c_str(), attached, ncpus,
                     isKprobe ? "kprobe" : "tracepoint");
        }

        if (progsLoaded > 0 && !rings_.empty()) {
            loaderType_ = EbpfLoaderType::SYSCALL_PRECOMPILED;
            mode_ = EbpfMode::EVENT_DRIVEN;
            active_ = true;
            state_ = EbpfState::EVENT_DRIVEN;
            // final-review fix: offset verdict BEFORE startPollThread() (see the
            // embedded path for the rationale). Degraded verdict latches
            // POLLING and we skip the poll thread instead of starting one.
            postLoadOffsetDegradeDecision();
            if (offsetDegraded_.load(std::memory_order_relaxed)) {
                unloaded_.store(false, std::memory_order_relaxed);
                LOG_WARN("eBPF: offset-degraded after attach (syscall_precompiled); "
                         "poll thread NOT started");
                return true;
            }
            // EBPF-04 fix: same as embedded path — if poll thread cannot start,
            // tear down and return false so load() tries the next phase.
            if (!startPollThread()) {
                LOG_ERROR("eBPF: poll thread start failed (syscall_precompiled) — "
                          "tearing down, falling through to next load phase");
                for (int f : progFds_) close(f);
                progFds_.clear();
                for (int f : mapFds_) close(f);
                mapFds_.clear();
                unmapAllRings();
                for (int f : tracepointFds_) close(f);
                tracepointFds_.clear();
                eventsMapFd_ = -1;
                countersMapFd_ = -1;
                threadCacheMapFd_ = -1;
                active_ = false;
                state_ = EbpfState::LOAD_FAILED;
                return false;
            }
            LOG_INFO("syscall_precompiled: SUCCESS (%d progs, %zu rings)",
                        progsLoaded, rings_.size());
            unloaded_.store(false, std::memory_order_relaxed);  // EBPF-04: clear between-phase unload() guard
            return true;
        }
        // Cleanup on failure
        for (int f : progFds_) close(f);
        progFds_.clear();
        for (int f : mapFds_) close(f);
        mapFds_.clear();
        unmapAllRings();
        for (int f : tracepointFds_) close(f);
        tracepointFds_.clear();
    }
    return false;
}

// ============================================================================
// Method 3: bpftool_precompiled — use bpftool command to load .o
// ============================================================================

// P2-22: find bpftool at a known absolute path instead of relying on PATH
// lookup through /bin/sh (which is what system("command -v bpftool") did).
// Returns the absolute path, or empty string if not found.
static std::string findBpftoolPath() {
    static const char* kBpftoolPaths[] = {
        "/system/bin/bpftool",
        "/vendor/bin/bpftool",
        "/data/adb/ksu/bin/bpftool",
        nullptr
    };
    for (int i = 0; kBpftoolPaths[i]; i++) {
        if (access(kBpftoolPaths[i], X_OK) == 0) return kBpftoolPaths[i];
    }
    return "";
}

// R1 (v4.2.3): resolve `rm` to a fixed absolute path instead of passing the bare
// token "rm" through execvp (which would PATH-lookup a possibly-poisoned PATH).
// Same access(X_OK) probe style as findBpftoolPath. Returns "" if no known rm
// exists — callers then skip the exec entirely (cleanup is best-effort anyway and
// never blocks the bpftool->POLLING degrade chain).
static std::string findRmPath() {
    static const char* kRmPaths[] = {
        "/system/bin/rm",
        "/vendor/bin/rm",
        nullptr
    };
    for (int i = 0; kRmPaths[i]; i++) {
        if (access(kRmPaths[i], X_OK) == 0) return kRmPaths[i];
    }
    return "";
}

// D2 (v4.2): run a fixed argv through the SHARED hardened exec helper.
// Previously this block forked `sh -c "<string-concatenated command>"`. That went
// through a shell (command substitution / metacharacter surface) and reimplemented
// its own timeout/reap. Now every bpftool/rm invocation is a fixed argv passed
// verbatim to execvp (NO shell, NO quoting, NO glob, NO `2>/dev/null` — stderr is
// already captured by aatpeExecArgv), with the shared 3s wall-clock deadline +
// setpgid + killpg + bounded WNOHANG reap. bypassCooldown=true so an unrelated
// dumpsys stall never suppresses eBPF load.
//
// Returns 0 on a clean child exit, the raw exit code otherwise, -1 on timeout /
// launch failure. Callers branch on != 0 exactly as before.
static int runBpftoolArgv(const std::vector<std::string>& argv) {
    int status = -1;
    (void)aatpeExecArgv(argv, "bpftool", &status, /*bypassCooldown=*/true);
    if (status < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;  // terminated by signal / not a clean exit
}

// One `bpftool prog loadall <obj> <pinBase> [type T] [pinmaps <dir>]` invocation.
// `type` null => auto-detect; `pinmapsDir` empty => no pinmaps (attempt 4).
static int runBpftoolLoadall(const std::string& bin, const std::string& obj,
                             const std::string& pinBase, const std::string& pinmapsDir,
                             const char* type) {
    std::vector<std::string> argv = { bin, "prog", "loadall", obj, pinBase };
    if (type) { argv.push_back("type"); argv.push_back(type); }
    if (!pinmapsDir.empty()) { argv.push_back("pinmaps"); argv.push_back(pinmapsDir); }
    return runBpftoolArgv(argv);
}

// H02 fix: the bpftool fallback attach table. sched_events.bpf.c is a
// TRACEPOINT-ONLY object: all six programs live in a tracepoint/sched/* section
// (fork/exit/switch/exec/wakeup/wakeup_new). bpftool `prog loadall` loads and
// pins every program; this table lists, for each pinned program (by its ELF
// function name handle_*), the tracepoint id path we resolve from tracefs to
// attach it.
//
// H02 bug being fixed: the old table listed only 4 programs (handle_fork/
// exit/switch/exec) and paired them with kprobe guesses (do_fork/do_exit/
// finish_task_switch/search_binary_handler). Two problems:
//   1. sched_wakeup / sched_wakeup_new were never opened or attached, so the
//      wakeup_cache map those two programs write was never populated and the
//      run-queue latency derived from it was silently dead on the bpftool path.
//   2. The .o has NO kprobe sections, so the kprobe guesses were always a type
//      mismatch on a tracepoint program fd; they only wasted an attach attempt
//      and leaked kprobe_events rows. They are removed -- every program here is
//      attached straight to its tracepoint.
struct BpftoolAttachRow { const char* progName; const char* tpPath; };
static const BpftoolAttachRow kBpftoolAttachTable[] = {
    {"handle_fork",       "tracepoint/sched/sched_process_fork"},
    {"handle_exit",       "tracepoint/sched/sched_process_exit"},
    {"handle_switch",     "tracepoint/sched/sched_switch"},
    {"handle_exec",       "tracepoint/sched/sched_process_exec"},
    {"handle_wakeup",     "tracepoint/sched/sched_wakeup"},
    {"handle_wakeup_new",  "tracepoint/sched/sched_wakeup_new"},
};
#ifdef AATPE_HOST_TEST
const std::vector<std::pair<std::string,std::string>>&
EbpfScheduler::hostTestBpftoolAttachTable(void) {
    static const std::vector<std::pair<std::string,std::string>> tbl = {
        {kBpftoolAttachTable[0].progName, kBpftoolAttachTable[0].tpPath},
        {kBpftoolAttachTable[1].progName, kBpftoolAttachTable[1].tpPath},
        {kBpftoolAttachTable[2].progName, kBpftoolAttachTable[2].tpPath},
        {kBpftoolAttachTable[3].progName, kBpftoolAttachTable[3].tpPath},
        {kBpftoolAttachTable[4].progName, kBpftoolAttachTable[4].tpPath},
        {kBpftoolAttachTable[5].progName, kBpftoolAttachTable[5].tpPath},
    };
    return tbl;
}
#endif

bool EbpfScheduler::tryLoadBpftoolPrecompiled() {
    // P2-22: fixed absolute path check instead of "command -v bpftool".
    std::string bpftoolBin = findBpftoolPath();
    if (bpftoolBin.empty()) {
        LOG_WARN("bpftool_precompiled: bpftool not available at known paths");
        return false;
    }

    // fix-v96 #8: module root resolved via getModuleDir() (Magisk/KSU/APatch).
    std::string modRoot2 = getModuleDir();
    std::vector<std::string> objPathVec = {
        "/system/etc/aatpe/ebpf/sched_events.bpf.o",
        modRoot2 + "/system/etc/aatpe/ebpf/sched_events.bpf.o",
    };
    std::vector<const char*> objPaths;
    for (const auto& p : objPathVec) objPaths.push_back(p.c_str());
    objPaths.push_back(nullptr);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kLoadTimeoutMs);

    for (int i = 0; objPaths[i]; i++) {
        struct stat st;
        if (stat(objPaths[i], &st) != 0) continue;
        if (loadTimeoutExceeded(deadline)) break;

        LOG_INFO("bpftool_precompiled: trying %s", objPaths[i]);

        // Load all programs with maps pinned.
        // Priority: auto-detect (no type) -> type kprobe -> type tracepoint
        std::string pinBase = "/sys/fs/bpf/aatpe_";
        std::string mapPinDir = "/sys/fs/bpf/aatpe_maps";
        // D2 cleanup (NO shell, NO glob): unlink the deterministic prog pin files
        // directly, rm -rf the (possibly non-empty) maps dir via hardened argv,
        // then recreate it. The old shell line was `rm -rf <pinBase>* <mapPinDir>;
        // mkdir -p <mapPinDir>` — glob expansion is impossible without a shell, so
        // we unlink the known pin names explicitly (stale extras are harmless:
        // loadall re-pins to the same paths).
        for (size_t pi = 0; pi < (sizeof(kBpftoolAttachTable)/sizeof(kBpftoolAttachTable[0])); pi++)
            ::unlink((pinBase + kBpftoolAttachTable[pi].progName).c_str());
        for (int pi = 0; pi < (int)(sizeof(kBpftoolAttachTable)/sizeof(kBpftoolAttachTable[0])); pi++)
            ::unlink((pinBase + "prog_" + std::to_string(pi)).c_str());
        // R1 (v4.2.3): use absolute-path rm (no PATH lookup). If none found, skip
        // the exec — rm cleanup is best-effort; mkdirIfNotExist below re-creates
        // the dir, and a leftover stale pin dir is harmless (loadall re-pins).
        std::string rmBin = findRmPath();
        if (rmBin.empty()) {
            LOG_WARN("bpftool_precompiled: no absolute rm found, skipping pinmaps cleanup (continuing)");
        } else {
            int cleanupRet = runBpftoolArgv({rmBin, "-rf", mapPinDir});
            if (cleanupRet != 0) {
                LOG_WARN("bpftool_precompiled: cleanup rm returned %d (continuing anyway)", cleanupRet);
            }
        }
        mkdirIfNotExist(mapPinDir.c_str(), 0755);

        const std::string& obj = objPaths[i];  // argv passes it verbatim; no quoting

        // Attempt 1: auto-detect program type from section names
        int ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, mapPinDir, nullptr);
        LOG_INFO("bpftool: auto-detect loadall ret=%d", ret);

        // Attempt 2: explicit type=kprobe
        if (ret != 0) {
            ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, mapPinDir, "kprobe");
            LOG_INFO("bpftool: type=kprobe loadall ret=%d", ret);
        }

        // Attempt 3: explicit type=tracepoint (legacy fallback)
        if (ret != 0) {
            ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, mapPinDir, "tracepoint");
            LOG_INFO("bpftool: type=tracepoint loadall ret=%d", ret);
        }

        // Attempt 4: without pinmaps
        if (ret != 0) {
            ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, "", nullptr);
            LOG_INFO("bpftool: no-pinmaps loadall ret=%d", ret);
        }
        if (ret != 0) continue;

        // Open each pinned program and attach it to its tracepoint. The object is
        // tracepoint-only (see kBpftoolAttachTable), so there is no kprobe step:
        // bpftool loaded these as tracepoint programs and we attach them by
        // resolving the tracepoint id from tracefs. (H02: this table now covers
        // all six programs, incl. sched_wakeup / sched_wakeup_new whose
        // wakeup_cache writes are what feed run-queue latency.)
        const int kProgCount =
            (int)(sizeof(kBpftoolAttachTable)/sizeof(kBpftoolAttachTable[0]));

        // Bug 4 fix: Open the program's internal events_map from pinned bpffs path,
        // NOT create a new separate map. This ensures perf ring buffer updates go
        // to the same map the BPF program writes events to.
        std::string eventsMapPin = mapPinDir + "/events_map";
        eventsMapFd_ = open(eventsMapPin.c_str(), O_RDONLY | O_CLOEXEC);
        if (eventsMapFd_ >= 0) {
            LOG_INFO("bpftool: opened pinned events_map fd=%d", eventsMapFd_);
            mapFds_.push_back(eventsMapFd_);
            int cfd = open((mapPinDir + "/event_counters").c_str(), O_RDONLY | O_CLOEXEC);
            if (cfd >= 0) { countersMapFd_ = cfd; mapFds_.push_back(cfd); }
            /* P2-1: was thread_cache pin; now wakeup_cache. */
            int tfd = open((mapPinDir + "/wakeup_cache").c_str(), O_RDONLY | O_CLOEXEC);
            if (tfd >= 0) { threadCacheMapFd_ = tfd; mapFds_.push_back(tfd); }
        } else {
            LOG_WARN("bpftool: WARNING: pinned events_map not found, creating separate map");
            eventsMapFd_ = createMap(BPF_MAP_TYPE_PERF_EVENT_ARRAY, 4, 4, 256);
            if (eventsMapFd_ < 0) continue;
            mapFds_.push_back(eventsMapFd_);
        }

        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpus <= 0) ncpus = 8;
        onlineCpuIds_ = readOnlineCpuIds();  // #24
        int progsLoaded = 0;
        for (int p = 0; p < kProgCount; p++) {
            if (loadTimeoutExceeded(deadline)) break;
            const char* progName = kBpftoolAttachTable[p].progName;
            std::string pinPath = pinBase + progName;
            int pfd = open(pinPath.c_str(), O_RDONLY | O_CLOEXEC);
            if (pfd < 0) {
                pinPath = pinBase + "prog_" + std::to_string(p);
                pfd = open(pinPath.c_str(), O_RDONLY | O_CLOEXEC);
            }
            if (pfd < 0) continue;
            progFds_.push_back(pfd);

            // Tracepoint-only object: attach each program straight to its
            // tracepoint by resolving the tracepoint id from tracefs (single,
            // 4.14-safe attach). The wakeup programs (p==4,5) do not emit to the
            // ring -- they only write wakeup_cache -- so they need no ring setup;
            // attaching them is what makes their map updates fire.
            int attached = 0;
            const char* tpPath = kBpftoolAttachTable[p].tpPath;
            if (tpPath) {
                int tpId = readTracepointId(tpPath);
                if (tpId > 0) {
                    int attachFd = -1;
                    int attachCpu = -1;
                    for (int cpu = 0; cpu < ncpus; cpu++) {
                        if (loadTimeoutExceeded(deadline)) break;
                        int perfFd = -1;
                        if (attachTracepoint(pfd, tpId, cpu, perfFd)) {
                            attachFd = perfFd;
                            attachCpu = cpu;
                            break;
                        }
                    }
                    if (attachFd >= 0) {
                        tracepointFds_.push_back(attachFd);
                        if (p == 0) {
                            setupPerCpuRingBuffers(tpId, attachFd, attachCpu, (int)ncpus, p);
                        }
                        attached = 1;
                    }
                }
            }
            if (attached > 0) progsLoaded++;
            LOG_INFO("bpftool: prog[%d] %s attached=%d", p, progName, attached);
        }

        if (progsLoaded > 0 && !rings_.empty()) {
            loaderType_ = EbpfLoaderType::BPFTOOL_PRECOMPILED;
            mode_ = EbpfMode::EVENT_DRIVEN;
            active_ = true;
            state_ = EbpfState::EVENT_DRIVEN;
            // final-review fix: offset verdict BEFORE startPollThread() (same
            // rationale as the embedded path).
            postLoadOffsetDegradeDecision();
            if (offsetDegraded_.load(std::memory_order_relaxed)) {
                unloaded_.store(false, std::memory_order_relaxed);
                LOG_WARN("eBPF: offset-degraded after attach (bpftool_precompiled); "
                         "poll thread NOT started");
                return true;
            }
            // EBPF-04 fix: same as embedded/syscall_precompiled paths.
            if (!startPollThread()) {
                LOG_ERROR("eBPF: poll thread start failed (bpftool_precompiled) — "
                          "tearing down, falling through to pure POLLING");
                for (int f : progFds_) close(f);
                progFds_.clear();
                for (int f : mapFds_) close(f);
                mapFds_.clear();
                unmapAllRings();
                for (int f : tracepointFds_) close(f);
                tracepointFds_.clear();
                eventsMapFd_ = -1;
                countersMapFd_ = -1;
                threadCacheMapFd_ = -1;
                cleanupKprobeEvents();
                kprobeIdCache_.clear();
                active_ = false;
                state_ = EbpfState::LOAD_FAILED;
                return false;
            }
            LOG_INFO("bpftool_precompiled: SUCCESS (%d progs)", progsLoaded);
            unloaded_.store(false, std::memory_order_relaxed);  // EBPF-04: clear between-phase unload() guard
            return true;
        }
        // Cleanup
        for (int f : progFds_) close(f);
        progFds_.clear();
        for (int f : mapFds_) close(f);
        mapFds_.clear();
        unmapAllRings();
        for (int f : tracepointFds_) close(f);
        tracepointFds_.clear();
        eventsMapFd_ = -1;
        countersMapFd_ = -1;
        threadCacheMapFd_ = -1;
        // LED-018 (R2-003): bpftool attachKprobe() writes "p:aatpe_..." rows to
        // /sys/kernel/tracing/kprobe_events. On this failure path those rows were
        // not cleaned up, leaking debugfs entries between objPath attempts (and
        // until the outer load() eventually calls unload()). Mirror unload() here.
        cleanupKprobeEvents();
        kprobeIdCache_.clear();
    }
    return false;
}

// ============================================================================
// Legacy methods (preserved for backward compatibility)
// ============================================================================
bool EbpfScheduler::extractBpfTextFromElf(const std::string& path,
                                            std::vector<uint8_t>& outText,
                                            std::string& outLicense) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    f.seekg(0, std::ios::beg);
    outText.resize(size);
    f.read(reinterpret_cast<char*>(outText.data()), size);
    outLicense = "GPL";
    return true;
}

// ============================================================================
// EBPF-15: the three legacy methods (tryLoadPrecompiled / loadEmbeddedBytecode /
// attachPerfEvent) were DEAD LEGACY wrappers/stubs with zero callers anywhere
// in the tree (verified by grep over all *.cpp/*.h including tests/). They are
// now REMOVED (declarations + definitions) instead of being kept as fake no-op
// implementations — a stub that always returns false is exactly the kind of
// dead code future readers can mistake for a live entry point. The real entry
// points they forwarded to (tryLoadBpftoolPrecompiled / tryLoadEmbeddedSyscall)
// are untouched and keep their own production callers.
// v94 fix(P44): tryCompileAndLoad() removed — was dead code (always returned
// false; runtime BPF compilation is not feasible on Android).
// ============================================================================

// EBPF-13: PERCPU_ARRAY lookup buffer sizing. The kernel writes
// value_size * nr_cpu_ids bytes into the userspace buffer; nr_cpu_ids can
// exceed the parsed /sys/.../cpu/possible count on drifted kernels, so we
// round up to a multiple of 8 AND add an explicit 8-slot margin. Kept as a
// file-scope pure function so the host test can verify the margin arithmetic
// without a bpf fd.
static int embeddedCpuBufferSlots(long possibleCpus) {
    if (possibleCpus < 1) possibleCpus = 1;
    return (int)(((possibleCpus + 7) / 8) * 8) + 8;
}

bool EbpfScheduler::readEmbeddedCpuCounts(std::vector<uint64_t>& outCounts) {
    outCounts.clear();
    if (!active_ || countersMapFd_ < 0) return false;
    int key = 0;  // total counter
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (uint32_t)countersMapFd_;
    attr.key = (uint64_t)(unsigned long)&key;
    // L-fix: a PERCPU_ARRAY lookup copies value_size * nr_cpu_ids bytes into the
    // user buffer, where nr_cpu_ids is the number of POSSIBLE cpus (cores that
    // may be offline/firmware-disabled but still carry a slot). Previously the
    // buffer was sized from _SC_NPROCESSORS_CONF (present cpus) rounded up by 8,
    // which could be SMALLER than what the kernel writes -> heap overflow. Size
    // the buffer from /sys/devices/system/cpu/possible (e.g. "0-3,7" -> max idx
    // 7 -> 8 possible slots), falling back to sysconf if the sysfs file is gone.
    long possibleCpus = 0;
    {
        FILE* pf = fopen("/sys/devices/system/cpu/possible", "re");
        if (pf) {
            char line[128] = {0};
            if (fgets(line, sizeof(line), pf)) {
                int maxIdx = -1;
                // Walk comma-separated ranges like "0-3,7".
                char* saveptr = nullptr;
                for (char* tok = strtok_r(line, ",", &saveptr); tok; tok = strtok_r(nullptr, ",", &saveptr)) {
                    int lo = 0, hi = 0;
                    if (strchr(tok, '-')) {
                        if (sscanf(tok, "%d-%d", &lo, &hi) == 2) {
                            if (hi > maxIdx) maxIdx = hi;
                        }
                    } else {
                        if (sscanf(tok, "%d", &lo) == 1) {
                            if (lo > maxIdx) maxIdx = lo;
                        }
                    }
                }
                if (maxIdx >= 0) possibleCpus = (long)maxIdx + 1;
            }
            fclose(pf);
        }
    }
    if (possibleCpus <= 0) possibleCpus = sysconf(_SC_NPROCESSORS_CONF);
    if (possibleCpus <= 0) possibleCpus = 8;
    // G4-006: round up to a multiple of 8 AND add an explicit 8-slot defensive
    // margin. nr_cpu_ids should equal the possible-CPU count, but on kernels
    // where the two drift the kernel writes value_size*nr_cpu_ids bytes; the
    // extra margin prevents a heap overrun of the userspace buffer.
    // EBPF-13 fix: the old formula ((possibleCpus+15)/8)*8 ONLY rounded up to a
    // multiple of 8, i.e. added at most 7 slots — the comment claimed a +8-slot
    // margin that the arithmetic never actually delivered. The margin is now
    // explicit (+8), not implied by the rounding.
    int roundedCpus = embeddedCpuBufferSlots(possibleCpus);
    size_t bufSize = (size_t)roundedCpus * sizeof(uint64_t);
    std::vector<uint8_t> buffer(bufSize, 0);
    attr.value = (uint64_t)(unsigned long)buffer.data();
    if (bpfSyscall(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr)) != 0) return false;
    uint64_t* counts = reinterpret_cast<uint64_t*>(buffer.data());
    // #24: PERCPU_ARRAY slots are indexed by PHYSICAL CPU id, not online ordinal.
    // On sparse online ("0-1,4-5") the old loop took counts[0..3] which included
    // offline cores 2,3 (zeros) and dropped online cores 4,5. Now iterate the
    // real online id list and only push counts[cpuId] for online cores.
    if (!onlineCpuIds_.empty()) {
        for (int cpuId : onlineCpuIds_) {
            if (cpuId < 0 || cpuId >= roundedCpus) continue;  // out of allocated buffer
            outCounts.push_back(counts[cpuId]);
        }
    } else {
        // Fallback: onlineCpuIds_ not populated (e.g. called before load).
        long onlineCpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (onlineCpus <= 0) onlineCpus = possibleCpus;
        int actualCount = (int)std::min((long)roundedCpus, onlineCpus);
        for (int i = 0; i < actualCount; i++) outCounts.push_back(counts[i]);
    }
    return true;
}

const uint8_t* EbpfScheduler::getEmbeddedBytecode(size_t* outSize) {
    if (outSize) *outSize = kEmbeddedPrograms[0].bytecodeSize;
    return kEmbeddedPrograms[0].bytecode;
}
size_t EbpfScheduler::getEmbeddedBytecodeSize(void) {
    return kEmbeddedPrograms[0].bytecodeSize;
}

// ============================================================================
// Degradation
// ============================================================================
void EbpfScheduler::degrade(const char* reason) {
    fallbackCount_.fetch_add(1, std::memory_order_relaxed);
    degradeEventCount_.fetch_add(1, std::memory_order_relaxed);

    // state dedup. The cumulative ringLost_/eventCountTotal_ counters
    // keep shouldDegrade() returning true forever after the first overflow, so
    // the main loop calls degrade() every iteration.  If we are ALREADY in the
    // ring-loss degraded state we must NOT re-log, re-stop the poll thread, or
    // re-set mode_ — just count the event for the 30s summary and return.
    //
    // v4.4.9 R3-F01: production degrade() writes EbpfState::DEGRADED_RING(9)
    // below, never the legacy EbpfState::DEGRADED(6) (which is only touched by
    // host test seams). The old check compared against DEGRADED(6), so after the
    // first transition it never matched: every loop iteration re-logged the
    // WARN and re-invoked stopPollThread() (idempotent, but a log storm /
    // wasted syscalls). Match the state we actually write, exactly like the
    // sibling retryLoadIfDue() gate (E-001) already does.
    if (state_ == EbpfState::DEGRADED_RING) return;
    if (mode_ == EbpfMode::POLLING) return;

    // First transition to degraded — log exactly once.
    LOG_WARN("eBPF DEGRADED: %s (mode -> HYBRID/POLLING)", reason);
#ifdef AATPE_HOST_TEST
    // v4.4.9 R3-F01 host seam: count how many times the one-time WARN branch was
    // taken. Release (no AATPE_HOST_TEST) compiles this out — zero effect/zero
    // extra symbols in the NDK binary. Lets the host test prove repeated
    // degrade() calls hit the dedup early-return instead of re-logging.
    degradeWarnCount_.fetch_add(1, std::memory_order_relaxed);
#endif
    stopPollThread();
    if (pollFallbackEnabled_.load(std::memory_order_relaxed)) {
        mode_ = EbpfMode::HYBRID;
    } else {
        mode_ = EbpfMode::POLLING;
    }
    state_ = EbpfState::DEGRADED_RING;
}

// throttled degrade-event summary. Called from the main loop every
// iteration but internally prints at most one WARN per 30 seconds.  This
// replaces the old per-event log storm with a single aggregate line.
void EbpfScheduler::logDegradeSummaryIfDue(void) {
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last = degradeSummaryLastMs_.load(std::memory_order_relaxed);
    if (nowMs - last < 30000) return;

    // CAS: only one caller (main loop) wins, resets the window atomically.
    if (!degradeSummaryLastMs_.compare_exchange_strong(last, nowMs,
            std::memory_order_relaxed, std::memory_order_relaxed)) return;

    uint64_t events = degradeEventCount_.exchange(0, std::memory_order_relaxed);
    if (events > 0 && state_ == EbpfState::DEGRADED_RING) {
        LOG_WARN("eBPF DEGRADED: %lu events in last 30s, mode=%s",
                 (unsigned long)events, getModeString());
    }
}

bool EbpfScheduler::shouldDegrade(void) const {
    if (!active_) return false;
    // recovery grace. After recoverIfHealthy() flips back to
    // EVENT_DRIVEN, the CUMULATIVE lost/total ratio is still >10% for a long
    // time (the old burst is baked into the counters). Without this grace the
    // very next loop iteration would re-degrade immediately, flapping
    // HYBRID<->EVENT_DRIVEN. Give the ring a 60s clean window to prove the
    // burst was transient before allowing another degrade.
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t rec = lastRecoverMs_.load(std::memory_order_relaxed);
    if (rec != 0 && (nowMs - rec) < 60000) return false;
    // Degrade if ring lost events exceed 10% of total
    uint64_t total = eventCountTotal_.load();
    uint64_t lost = ringLost_.load();
    if (total > 1000 && lost > total / 10) return true;
    return false;
}

// auto-recover from a transient ring-loss degrade.
// degrade() used to be one-way: state_ stayed DEGRADED forever even after the
// ring stabilized, because shouldDegrade() reads cumulative counters. Now, when
// we degraded to HYBRID (ring overflow, NOT a fatal load/offset failure) and
// the ring has had zero new losses for >=30s, we flip back to EVENT_DRIVEN and
// restart the dedicated poll thread. Fatal POLLING degradation (load failed,
// bad tracepoint offsets) is intentionally left untouched.
// ROUND-003 (F2误伤修复): post-load offset verdict decision, shared by the
// production load() lambda and the host test seam. Distinguish
//   VERIFIED           -> healthy, clear all offset flags.
//   UNKNOWN + known family  -> tracefs format unreadable on a known 4.14 layout
//                              family; we cannot falsify built-in offsets. Trust
//                              them (v3.4.6 behavior), stay EVENT_DRIVEN, set only a
//                              soft diagnostic flag. Do NOT latch POLLING.
//   DEGRADED (readable but != built-in) -> evidence of a different layout: latch
//                              POLLING/DEGRADED (fail-safe, no auto-recover).
//   UNKNOWN + non-known family -> no evidence either way; stay conservative.
void EbpfScheduler::postLoadOffsetDegradeDecision(void) {
    const auto sw = offsetStatus_switch_.load(std::memory_order_relaxed);
    if (sw == TpOffsetStatus::VERIFIED) {
        offsetDegraded_.store(false, std::memory_order_relaxed);
        offsetUnverifiedButTrusted_.store(false, std::memory_order_relaxed);
        return;
    }
    if (sw == TpOffsetStatus::UNKNOWN && knownKernelFamily_.load(std::memory_order_relaxed)) {
        offsetUnverifiedButTrusted_.store(true, std::memory_order_relaxed);
        offsetDegraded_.store(false, std::memory_order_relaxed);
        LOG_WARN("offset-verify: tracefs format unreadable on known 4.14 kernel "
                 "family; trusting built-in offsets (EVENT_DRIVEN, soft-trust)");
        return;  // stay event-driven; no latch
    }
    // sw == DEGRADED (evidence mismatch) OR UNKNOWN on a non-known family.
    LOG_ERROR("offset-verify: sched_switch offsets not usable (status=%s, "
              "knownFamily=%d); entering offset-degraded POLLING (no auto-recover)",
              tpOffsetStatusToString(sw), (int)knownKernelFamily_.load(std::memory_order_relaxed));
    offsetUnverifiedButTrusted_.store(false, std::memory_order_relaxed);
    offsetDegraded_.store(true, std::memory_order_relaxed);
    stopPollThread();
    mode_ = EbpfMode::POLLING;
    state_ = EbpfState::DEGRADED_RING;
}

void EbpfScheduler::recoverIfHealthy(void) {
    if (state_ != EbpfState::DEGRADED_RING) return;  // P1 round7: match degrade() which writes DEGRADED_RING
    if (mode_ != EbpfMode::HYBRID) return;  // only recover ring-loss fallback
    // G4-001: offset-caused degradation (offsetDegraded_=true, mode=POLLING) is
    // never auto-recovered here. The kernel layout does not self-correct; the
    // verdict can only flip back via a fresh load() that re-runs
    // verifyTracepointOffsets() and re-validates sched_switch against tracefs.
    if (offsetDegraded_.load(std::memory_order_relaxed)) return;

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastLoss = lastRingLossMs_.load(std::memory_order_relaxed);
    if (lastLoss == 0) {
        // No loss stamp recorded (degraded for a non-ring reason). Seed the
        // clock so we don't recover on the very first call.
        lastRingLossMs_.store(nowMs, std::memory_order_relaxed);
        return;
    }
    if ((nowMs - lastLoss) < 30000) return;  // require 30s loss-free

    startPollThread();               // resume the dedicated ring consumer
    state_ = EbpfState::EVENT_DRIVEN;
    mode_ = EbpfMode::EVENT_DRIVEN;
    lastRecoverMs_.store(nowMs, std::memory_order_relaxed);  // start degrade grace
    LOG_WARN("eBPF recovered: ring buffer loss-free for 30s, mode -> EVENT_DRIVEN");
    // round-5: report recovery to the unified registry.
    aatpe::subsys::Registry::instance().setState("ebpf", aatpe::subsys::SubsystemState::ACTIVE);
    aatpe::subsys::Registry::instance().markRecovery("ebpf");
}

// ============================================================================
// Dynamic ring buffer sizing.
//
// Monitors the windowed loss rate every 30s and adjusts ringPages_ between
// kRingPagesNormal (64) and kRingPagesHigh (128). When the loss rate exceeds
// kRingHighLossThreshold (5%), we upgrade to 128 pages. When the loss rate is
// below kRingLowLossThreshold (0.1%) for 3 consecutive windows, we downgrade
// back to 64 pages to save memory.
//
// Resizing requires an unload+reload cycle (the perf ring mmap size is fixed
// at mmap time). This is done atomically: stopPollThread -> unload -> set new
// ringPages_ -> load -> startPollThread. The reload re-runs the full detection
// chain (embedded -> syscall -> bpftool), which is ~10-50ms.
//
// Safety: only adjusts when eBPF is ACTIVE in EVENT_DRIVEN mode. Does not
// touch POLLING or HYBRID states.
// ============================================================================
// ============================================================================
// D3 (v4.2): CPU hotplug online-mask -> eBPF ring rebuild.
//
// onlineCpuIds_ was only read at load(); on a runtime core on/offline the per-cpu
// perf rings and the PERCPU_ARRAY aggregation indices were stale. This method
// re-reads /sys/devices/system/cpu/online and, when the mask differs from the
// loaded list, runs a guarded unload()+load() (same proven path as ringPages
// resize) so the programs re-attach against the new online set.
//
// Concurrency: all unload()/load() work is serialized under lifecycleMutex_
// (shared with onConfigReload / retryLoadIfDue). stopPollThread() is called
// FIRST and joined, so no poll-thread deliver*Event() races the teardown.
//
// Failure fallback: if the reload fails, we do NOT leave a half-built ring live —
// the scheduler cleanly enters POLLING and the existing retryLoadIfDue backoff
// recovers. We stamp ebpf_hotplug_rebuild_failed for diagnostics. The old ring is
// torn down by the deliberate unload() (a true atomic program-swap would require
// loading a second set into fresh fds, too invasive for P2); in practice a mask
// change on a previously-working kernel almost always reloads cleanly.
// ============================================================================
bool EbpfScheduler::onlineMaskSetChanged(const std::vector<int>& oldIds,
                                          const std::string& newText) {
    std::vector<int> newIds = parseCpuOnlineMask(newText);
    if (newIds.empty()) return false;   // unreadable / garbage -> no action
    return newIds != oldIds;
}

std::vector<int> EbpfScheduler::hotplugRebuildIfDue(void) {
    // Only rebuild a live event-driven scheduler. POLLING/degraded is owned by
    // retryLoadIfDue; HYBRID ring-fallback has no per-cpu ring to reindex.
    if (!active_) return {};
    if (mode_ != EbpfMode::EVENT_DRIVEN) return {};
    if (onlineCpuIds_.empty()) return {};   // load() not settled; don't act

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last = lastHotplugRebuildMs_.load(std::memory_order_relaxed);
    if (last != 0 && (nowMs - last) < kHotplugRebuildMinIntervalMs) return {};

    // Read the raw sysfs mask text and compare via the pure decision helper.
    FILE* f = fopen("/sys/devices/system/cpu/online", "re");
    char line[128] = {0};
    std::string newText;
    if (f) { if (fgets(line, sizeof(line), f)) newText = line; fclose(f); }
    if (newText.empty()) return {};
    if (!onlineMaskSetChanged(onlineCpuIds_, newText)) return {};

    std::vector<int> newMask = parseCpuOnlineMask(newText);

    LOG_WARN("eBPF hotplug: online mask changed, rebuilding rings for new set "
             "(old %zu cpus, new %zu cpus)", onlineCpuIds_.size(), newMask.size());
    lastHotplugRebuildMs_.store(nowMs, std::memory_order_relaxed);
    hotplugRebuildAttempts_.fetch_add(1, std::memory_order_relaxed);

    stopPollThread();
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        unload();                       // clears onlineCpuIds_; load() re-reads it
        bool ok = load();
        if (ok && state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN) {
            startPollThread();
            LOG_FORCE("eBPF hotplug: rebuild OK, now tracking %zu online cpus",
                      onlineCpuIds_.size());
            // P3-2: atomic write via STATE_DIR (was non-atomic fopen + hardcoded dir).
            writeFileAtomic((std::string(STATE_DIR) + "/ebpf_hotplug_rebuild_ok").c_str(), "1\n");
            unlink((std::string(STATE_DIR) + "/ebpf_hotplug_rebuild_failed").c_str());
        } else {
            hotplugRebuildFailures_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("eBPF hotplug: rebuild FAILED — falling back to POLLING, "
                     "retryLoadIfDue will recover");
            writeFileAtomic((std::string(STATE_DIR) + "/ebpf_hotplug_rebuild_failed").c_str(), "1\n");
        }
    }
    return newMask;
}

void EbpfScheduler::dynamicRingAdjustIfDue(void) {
    if (!active_) {
        // D3: even when idle-for-ring-adjust, still service the hotplug rebuild so
        // a mask change during a non-EVENT_DRIVEN phase is not silently dropped.
        hotplugRebuildIfDue();
        return;
    }
    // D3: check the online-mask first (cheap, throttled). If it changed we rebuild
    // rings and bail out of the page-size logic for this window.
    if (!hotplugRebuildIfDue().empty()) return;
    // Only ACTIVE/EVENT_DRIVEN (healthy) or DEGRADED/HYBRID (ring-loss fallback)
    // carry a drainable ring. POLLING (fatal load failure / bad offsets) has no
    // ring to resize and must NOT be touched.
    //
    // 核心问题2 fix: previously this required state_==ACTIVE &&
    // mode_==EVENT_DRIVEN. Under SUSTAINED ring overflow shouldDegrade() flips the
    // scheduler to DEGRADED/HYBRID (poll thread stopped), and recoverIfHealthy()
    // needs a 30s loss-free window to flip back — which never arrives while the
    // ring keeps overflowing. Result: the 64->128 upgrade vote could never run and
    // the ring stayed at 64 forever (ebpf_ring_pages stuck, RING_OVERFLOW
    // persistent). Now the UPGRADE vote is allowed from the HYBRID ring-loss
    // fallback too; the reload re-establishes ACTIVE/EVENT_DRIVEN on the bigger
// bugfix: ring sizing is a CAPACITY question, not a mode question.
    // The old early-return below skipped adjustment unless we were healthy
    // (ACTIVE+EVENT_DRIVEN) or ring-fallback (DEGRADED+HYBRID), AND required
    // eventsDelta >= 100. Under a bad overflow the poll thread starves, so
    // consumed events (eventCountTotal_) stop growing while ringLost_ keeps
    // climbing -> eventsDelta < 100 -> we returned before upgrading, forever
    // stuck at 64 pages. We now evaluate every 30s in any state, and upgrade on
    // EITHER a >5% windowed loss rate OR an absolute loss burst, with no minimum
    // event-count requirement on the upgrade path.
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastAdj = lastRingAdjustMs_.load(std::memory_order_relaxed);
    if (lastAdj == 0) {
        // First call: seed baseline
        lastRingAdjustMs_.store(nowMs, std::memory_order_relaxed);
        lostAtLastAdjust_.store(ringLost_.load(), std::memory_order_relaxed);
        eventsAtLastAdjust_.store(eventCountTotal_.load(), std::memory_order_relaxed);
        return;
    }
    if ((nowMs - lastAdj) < kRingAdjustIntervalMs) return;  // not time yet

    // Compute delta-based loss rate since last adjustment
    uint64_t totalLost = ringLost_.load(std::memory_order_relaxed);
    uint64_t totalEvents = eventCountTotal_.load(std::memory_order_relaxed);
    uint64_t lostDelta = totalLost - lostAtLastAdjust_.load(std::memory_order_relaxed);
    uint64_t eventsDelta = totalEvents - eventsAtLastAdjust_.load(std::memory_order_relaxed);
    double lossRate = (eventsDelta > 0) ? ((double)lostDelta / (double)eventsDelta) : 0.0;

    // UPGRADE: 64 -> 128 pages. Allowed in ANY state/mode (see header comment).
    // Trigger on EITHER >5% loss rate OR an absolute lost-event burst; the burst
    // trigger catches the starvation case where consumption collapses.
    bool upgrade = (ringPages_ < kRingPagesHigh) &&
                   (lossRate > kRingHighLossThreshold ||
                    lostDelta >= kRingHighLostBurst);
    if (upgrade) {
        LOG_WARN("eBPF dynamic ring: overflow (lossRate=%.1f%% lostDelta=%llu eventsDelta=%llu) "
                 "-> upgrading ring %d->%d pages (state=%d mode=%d)",
                 lossRate * 100, (unsigned long long)lostDelta, (unsigned long long)eventsDelta,
                 ringPages_, kRingPagesHigh,
                 (int)state_.load(std::memory_order_relaxed),
                 (int)mode_.load(std::memory_order_relaxed));
        stopPollThread();
        // ROUND-004 adversarial: serialize reload against config-thread onConfigReload.
        {
            std::lock_guard<std::mutex> lk(lifecycleMutex_);
            unload();
            ringPages_ = kRingPagesHigh;
            lowLossStreak_.store(0, std::memory_order_relaxed);
            // bugfix: reset the cumulative ring_lost counter after upgrade so
            // the new larger ring starts clean and degraded_reason clears. Re-baseline.
            ringLost_.store(0, std::memory_order_relaxed);
            load();
            if (state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN) {
                startPollThread();
            }
        }
        lastRingAdjustMs_.store(nowMs, std::memory_order_relaxed);
        lostAtLastAdjust_.store(0, std::memory_order_relaxed);
        eventsAtLastAdjust_.store(eventCountTotal_.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        return;
    }

    // DOWNGRADE: very low loss rate for 3 consecutive windows -> reduce to 64.
    // Healthy EVENT_DRIVEN only (never tear down the fallback ring). Need enough
    // events consumed to trust the low-loss verdict.
    bool healthy = (state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN);
    if (!healthy || eventsDelta < 100) {
        // No upgrade; still advance the baseline so the next window is fresh.
        lastRingAdjustMs_.store(nowMs, std::memory_order_relaxed);
        lostAtLastAdjust_.store(totalLost, std::memory_order_relaxed);
        eventsAtLastAdjust_.store(totalEvents, std::memory_order_relaxed);
        return;
    }
    if (lossRate < kRingLowLossThreshold && ringPages_ > kRingPagesNormal) {
        int streak = lowLossStreak_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (streak >= 3) {
            LOG_INFO("eBPF dynamic ring: loss rate %.3f%% for %d windows, downgrading ring %d->%d pages",
                     lossRate * 100, streak, ringPages_, kRingPagesNormal);
            stopPollThread();
            {
                std::lock_guard<std::mutex> lk(lifecycleMutex_);
                unload();
                ringPages_ = kRingPagesNormal;
                lowLossStreak_.store(0, std::memory_order_relaxed);
                load();
                if (state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN) {
                    startPollThread();
                }
            }
        }
    } else if (lossRate >= kRingLowLossThreshold) {
        lowLossStreak_.store(0, std::memory_order_relaxed);
    }
    lastRingAdjustMs_.store(nowMs, std::memory_order_relaxed);
    lostAtLastAdjust_.store(totalLost, std::memory_order_relaxed);
    eventsAtLastAdjust_.store(totalEvents, std::memory_order_relaxed);
}

// ============================================================================
// Exponential backoff for eBPF load failure retry.
//
// After all 3 load phases fail, the daemon enters POLLING mode. Previously
// this was permanent — the system never tried to reload eBPF even if the
// kernel's tracefs became available later (e.g. after a vendor daemon
// settles). Now we retry with exponential backoff:
//   1s -> 2s -> 4s -> 8s -> 15s -> 30s -> 60s -> 120s (capped at 5 min)
//
// Permanent failures (EPERM/EACCES = no CAP_BPF / no kernel support) are
// classified after the first full failure cycle: if all 3 phases fail with
// permission errors, we set loadPermanentFailed_ and stop retrying.
// ============================================================================
void EbpfScheduler::retryLoadIfDue(void) {
    // E-001 root-cause fix. Production load-failure paths (the all-3-phases
    // fallback AND the fail-closed integrity-gate refusal) land in
    //   mode_=POLLING + state_=DEGRADED_RING with NO programs attached (progFds_
    //   empty). The old second gate checked EbpfState::DEGRADED(6) — a legacy
    // value production never writes (only the g4Test offset / ring-loss seams
    // set it) — so the auto-retry NEVER fired and a device that failed to load
    // eBPF stayed in POLLING until a daemon restart or a config hot-reload.
    //
    // We must retry "POLLING because nothing loaded" but NOT tear down a load
    // that already succeeded. ring-loss degrade() and the offset-degraded
    // post-load decision both keep programs ATTACHED (progFds_ non-empty), so
    // progFds_ emptiness is the precise discriminator:
    //   * load failure / integrity refusal -> progFds_ empty  -> retry.
    //   * ring-loss (HYBRID, or POLLING with poll-fallback disabled) -> progs
    //     still attached -> owned by recoverIfHealthy()/dynamicRingAdjust; reload
    //     here would tear down a working load.
    //   * offset-degraded POLLING -> progs still attached -> a future config
    //     reload that re-runs verifyTracepointOffsets() owns any recovery.
    if (mode_ != EbpfMode::POLLING) return;
    if (state_ != EbpfState::DEGRADED_RING) return;
    if (!progFds_.empty()) return;   // programs attached -> not a load failure
    // ROUND-014 P3 C36: after permanent latch, a single low-frequency 600s re-probe
    // clock (separate from the backoff counter) so a SELinux relabel / kernel
    // upgrade can self-heal without a daemon restart.
    if (loadPermanentFailed_) {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t lastProbe = lastLoadAttemptMs_.load(std::memory_order_relaxed);
        if (nowMs - lastProbe < 600000ULL) return;   // 600s throttle
        loadPermanentFailed_ = false;   // one re-probe attempt; re-latches on failure
        loadFailureCount_.store(0);
    }
    if (loadFailureCount_.load() >= kBackoffMaxRetries) return;

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastAttempt = lastLoadAttemptMs_.load(std::memory_order_relaxed);
    uint64_t nextDelay = nextRetryDelayMs_.load(std::memory_order_relaxed);

    // First attempt timing: if we've never recorded an attempt time, seed it
    if (lastAttempt == 0) {
        lastLoadAttemptMs_.store(nowMs, std::memory_order_relaxed);
        return;
    }
    // Wait for the current backoff delay to elapse
    if ((nowMs - lastAttempt) < nextDelay) return;

    // Time to retry
    LOG_INFO("eBPF retryLoad: attempt %d, retrying load after %lldms backoff",
             loadFailureCount_.load(), (unsigned long long)nextDelay);
    // ROUND-004 adversarial: serialize against onConfigReload() on the config
    // thread. Never run unload()/load() concurrently with a hot-reload.
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        // E-001: the all-phases-failed fallback leaves active_=true (polling
        // fallback is "active") AND unloaded_=true (set by the partial-attach
        // unload() calls inside load()). As-is, unload() below is an idempotent
        // no-op (unloaded_ guard) and load() early-returns at its
        // "if (active_) return true;" guard — so the retry would be a silent
        // no-op even after the gate fix. Clear unloaded_ first so unload() does a
        // real teardown (stop poll thread, close any fds, unmap rings) and load()
        // actually re-runs the 3-phase loader.
        unloaded_.store(false, std::memory_order_relaxed);
        unload();  // clean up any partial state
        bool ok = load();
        lastLoadAttemptMs_.store(nowMs, std::memory_order_relaxed);

        if (ok && state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN) {
            // Success! Reset backoff
            LOG_FORCE("eBPF retryLoad: SUCCESS, now active via %s", getLoaderString());
            loadFailureCount_.store(0, std::memory_order_relaxed);
            nextRetryDelayMs_.store(0, std::memory_order_relaxed);
            startPollThread();
            return;
        }
    }

    // Failed again — compute next backoff
    int fails = loadFailureCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    // Exponential: 1s, 2s, 4s, 8s, then cap steps at 15s increments
    uint64_t delay;
    if (fails <= 3) {
        delay = kBackoffBaseMs << fails;  // 2s, 4s, 8s
    } else {
        // Step up: 15s, 30s, 60s, 120s, then cap at 300s
        static const uint64_t steps[] = {15000, 30000, 60000, 120000, 300000};
        int idx = fails - 4;
        if (idx >= (int)(sizeof(steps)/sizeof(steps[0]))) idx = (int)(sizeof(steps)/sizeof(steps[0])) - 1;
        delay = steps[idx];
    }
    if (delay > kBackoffMaxMs) delay = kBackoffMaxMs;
    nextRetryDelayMs_.store(delay, std::memory_order_relaxed);
    LOG_WARN("eBPF retryLoad: attempt %d failed, next retry in %llums (max %d retries)",
             fails, (unsigned long long)delay, kBackoffMaxRetries);
}

// ============================================================================
// Partial degradation detection.
//
// eBPF can be ACTIVE (programs attached, ring buffers flowing) while one or
// more tracepoints have DEGRADED offset status (known-wrong ctx field offsets).
// In that case the affected events are filtered, but the system still runs on
// the remaining tracepoints. The WebUI should show "ACTIVE PARTIAL" to inform
// the user that some event types may be incomplete.
// ============================================================================
bool EbpfScheduler::isPartiallyDegraded(void) const {
    if (state_ != EbpfState::EVENT_DRIVEN) return false;
    // Any tracepoint actively detected as DEGRADED = partial
    return (offsetStatus_switch_.load(std::memory_order_relaxed) == TpOffsetStatus::DEGRADED ||
            offsetStatus_fork_.load(std::memory_order_relaxed)   == TpOffsetStatus::DEGRADED ||
            offsetStatus_exec_.load(std::memory_order_relaxed)   == TpOffsetStatus::DEGRADED ||
            offsetStatus_exit_.load(std::memory_order_relaxed)  == TpOffsetStatus::DEGRADED);
}

const char* EbpfScheduler::getDetailedStateString(void) const {
    // G4-001: an offset-unverified daemon must not be reported as a plain ACTIVE.
    // It is attached but its events are filtered and it cannot auto-recover.
    if (offsetDegraded_.load(std::memory_order_relaxed)) {
        return "DEGRADED_OFFSET";
    }
    // ROUND-003: soft-trust (known kernel family, tracefs unreadable) is NOT a
    // degradation — surface it distinctly so operators can see built-in offsets
    // are in use without it being treated as a fault.
    if (offsetUnverifiedButTrusted_.load(std::memory_order_relaxed)) {
        return "TRUSTED_BUILTIN";
    }
    if (state_ == EbpfState::EVENT_DRIVEN && isPartiallyDegraded()) {
        return "ACTIVE_PARTIAL";
    }
    return getStateString();
}

// ============================================================================
// v94 fix / 问题43/44/45: runtime tracepoint ctx-offset verification.
//
// The BPF programs in sched_events.bpf.c read the tracepoint context at HARDCODED
// byte offsets, derived from the Linux 4.14 generic TP_STRUCT__entry layout:
//
//   sched_switch      : prev_comm@8  prev_pid@24  next_comm@40  next_pid@56  next_prio@60
//                       (the v94-r2 correction accounts for prev_state being `long`
//                        = 8 bytes on arm64, which shifts next_* by +8).
//   sched_process_fork : child_pid@44  child_tgid@48  (P0-1: was @32/@36;
//                       old layout fabricated parent_tgid[4], real entry has
//                       child_comm[16] after parent_pid, shifting child_* by +12)
//   sched_process_exec : does NOT read ctx (uses bpf_get_current_comm) -> no check.
//
// These are correct on the reference target (Dimensity 1100 / MT6893, kernel
// 4.14.186) but are NOT a universal assumption: other kernel versions / vendor
// BSPs can reorder or widen TP_STRUCT__entry fields. At load() we read the live
//   /sys/kernel/[debug/]tracing/events/sched/<event>/format   file, parse each
// field's `offset:`, and compare. On mismatch we only LOG_WARN and CONTINUE (the
// 4.14 layout should be stable; a mismatch means "re-verify on this kernel", not
// "abort"). If debugfs/tracefs is not mounted (common on locked production ROMs)
// we degrade gracefully to "unknown". The verdict is surfaced via
// getOffsetVerifyResult() and the daemon writes it to
//   <state_dir>/ebpf_offset_verified .
// ============================================================================
const char* EbpfScheduler::tpOffsetStatusToString(TpOffsetStatus s) {
    switch (s) {
        case TpOffsetStatus::VERIFIED: return "verified";
        case TpOffsetStatus::DEGRADED: return "degraded";
        case TpOffsetStatus::NO_CTX:   return "no_ctx";
        case TpOffsetStatus::UNKNOWN:
        default:                        return "unknown";
    }
}

void EbpfScheduler::verifyTracepointOffsets() {
    // Same mount bases as readTracepointId().
    static const char* bases[] = {
        "/sys/kernel/debug/tracing/events/sched/",
        "/sys/kernel/tracing/events/sched/",
        nullptr
    };

    // G4-002: capability/empirical probe, not a hard kernel-version gate.
    // Previously the hardcoded offsets were ONLY trusted when the running kernel
    // was exactly 4.14.x (fpOk). That meant every modern GKI device (5.10/5.15/6.1)
    // was rejected out of sight of the live tracefs format file. The correct
    // authority is the live /sys/.../events/<ev>/format file itself:
    //   - format readable AND the fields the BPF code actually reads match the
    //     live offsets exactly  -> VERIFIED (any kernel major/minor).
    //   - format readable but a read field mismatches the live offset -> DEGRADED
    //     (events from that tracepoint are filtered; we never parse at a guessed
    //     offset).
    //   - format NOT readable (locked ROM) -> UNKNOWN -> fail-safe degrade.
    // The kernel fingerprint is retained only as a logged hint ("known 4.14
    // layout family" vs "other kernel") and never overrides the empirical verdict.
    auto kernelFingerprintOk = []() -> bool {
        struct utsname u;
        if (uname(&u) != 0) return false;
        int major=0, minor=0, patch=0;
        if (sscanf(u.release, "%d.%d.%d", &major, &minor, &patch) < 2) return false;
        return (major == 4 && minor == 14);
    };
    const bool fpOk = kernelFingerprintOk();
    knownKernelFamily_.store(fpOk, std::memory_order_relaxed);
    if (!fpOk) {
        LOG_INFO("offset-verify: kernel is NOT the 4.14.x layout family; "
                 "will trust live tracefs format offsets empirically (fail-safe on mismatch/unreadable)");
    }

    // Parse one event's `format` file -> map{fieldName -> byte offset}.
    // Returns an empty map only when NO base path was readable at all.
    auto readOffsets = [](const char* eventDir) -> std::map<std::string, int> {
        std::map<std::string, int> out;
        for (int b = 0; bases[b]; ++b) {
            std::string path = std::string(bases[b]) + eventDir + "/format";
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string line;
            while (std::getline(f, line)) {
                // e.g. "        field:char next_comm[16];\toffset:40;\tsize:16; signed:1;"
                size_t fp = line.find("field:");
                if (fp == std::string::npos) continue;
                size_t declEnd = line.find(';', fp);
                if (declEnd == std::string::npos) continue;
                std::string decl = line.substr(fp + 6, declEnd - (fp + 6));
                size_t sp = decl.find_last_of(" \t");
                std::string name = (sp == std::string::npos) ? decl : decl.substr(sp + 1);
                size_t br = name.find('[');
                if (br != std::string::npos) name = name.substr(0, br);
                size_t op = line.find("offset:", declEnd);
                if (op == std::string::npos) continue;
                size_t oe = line.find(';', op + 7);
                if (oe == std::string::npos) continue;
                try {
                    out[name] = std::stoi(line.substr(op + 7, oe - (op + 7)));
                } catch (...) {
                    // Cold one-time boot offset-verify parse; skip a malformed token
                    // without aborting the whole map. LOG_DEBUG (compiled out in release).
                    LOG_DEBUG("offset-verify: non-integer offset for '%s', skipping", name.c_str());
                }
            }
            if (!out.empty()) return out;
        }
        return out;
    };

    struct Expect { const char* field; int off; };
    bool anyReadable = false;
    bool allMatch = true;
    std::string detailAll;

    // Helper: compare a live offset map against an expected field-offset table.
    // Returns true iff every expected field exists AND is at the expected offset.
    // On mismatch, logs per-field detail into `d` for the human-readable verdict.
    auto checkExpect = [](const std::map<std::string, int>& offs,
                          const Expect* table, size_t n,
                          std::string& d) -> bool {
        bool okAll = true;
        for (size_t i = 0; i < n; ++i) {
            const Expect& e = table[i];
            auto it = offs.find(e.field);
            int actual = (it != offs.end()) ? it->second : -1;
            bool ok = (actual == e.off);
            if (!ok) okAll = false;
            d += std::string(e.field) + "@" + std::to_string(actual) +
                 (ok ? "==exp" : ("!=exp" + std::to_string(e.off))) + " ";
        }
        return okAll;
    };

    // --- sched_switch (the load-bearing offsets) ---
    {
        std::map<std::string, int> offs = readOffsets("sched_switch");
        // G4-015: only the fields the BPF handler actually reads must match.
        // next_prio@60 is NOT read by handle_switch(); verifying it could
        // over-conservatively mark the whole tracepoint DEGRADED on a BSP that
        // reordered the unused prio field.
        static const Expect kSwitch[] = {
            {"prev_comm", 8}, {"prev_pid", 24},
            {"next_comm", 40}, {"next_pid", 56},
        };
        if (offs.empty()) {
            offsetStatus_switch_.store(TpOffsetStatus::UNKNOWN, std::memory_order_relaxed);
            detailAll += "sched_switch=unavailable;";
        } else {
            anyReadable = true;
            std::string d;
            bool ok = checkExpect(offs, kSwitch,
                                  sizeof(kSwitch)/sizeof(kSwitch[0]), d);
            if (!ok) allMatch = false;
            offsetStatus_switch_.store(ok ? TpOffsetStatus::VERIFIED
                                          : TpOffsetStatus::DEGRADED,
                                      std::memory_order_relaxed);
            LOG_WARN("offset-verify sched_switch: %s -> %s",
                     d.c_str(), tpOffsetStatusToString(offsetStatus_switch_.load(std::memory_order_relaxed)));
            detailAll += "sched_switch[" + d + "];";
        }
    }
    // --- sched_process_fork (child_pid) ---
    {
        std::map<std::string, int> offs = readOffsets("sched_process_fork");
        // G4-014: the BPF handler (R9-022) only reads child_pid@44. child_tgid@48
        // is no longer probed; verifying it could over-conservatively filter fork
        // events on a BSP that dropped the field.
        static const Expect kFork[] = { {"child_pid", 44} };
        if (offs.empty()) {
            offsetStatus_fork_.store(TpOffsetStatus::UNKNOWN, std::memory_order_relaxed);
            detailAll += "sched_process_fork=unavailable;";
        } else {
            anyReadable = true;
            std::string d;
            bool ok = checkExpect(offs, kFork,
                                  sizeof(kFork)/sizeof(kFork[0]), d);
            if (!ok) allMatch = false;
            offsetStatus_fork_.store(ok ? TpOffsetStatus::VERIFIED
                                        : TpOffsetStatus::DEGRADED,
                                    std::memory_order_relaxed);
            LOG_WARN("offset-verify sched_process_fork: %s -> %s",
                     d.c_str(), tpOffsetStatusToString(offsetStatus_fork_.load(std::memory_order_relaxed)));
            detailAll += "sched_process_fork[" + d + "];";
        }
    }
    // --- sched_process_exec / sched_process_exit: the BPF handlers do NOT
    // dereference tracepoint ctx (they use bpf_get_current_comm /
    // bpf_get_current_pid_tgid), so there is no ctx-offset layout to verify.
    // They are always marked NO_CTX (trusted). If a future BPF revision starts
    // reading ctx for these two, add a checkExpect() block here and switch the
    // default to UNKNOWN.
    offsetStatus_exec_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_exit_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    detailAll += "sched_process_exec=ctx-offsets-not-used;";
    detailAll += "sched_process_exit=ctx-offsets-not-used;";

    // --- P1-4: sched_wakeup offset verification. The BPF wakeup handlers read
    // pid@ctx+24 and target_cpu@ctx+36. If these offsets drift on a different
    // kernel, wakeup_cache gets poisoned keys and runq latency is garbage.
    {
        std::map<std::string, int> offs = readOffsets("sched_wakeup");
        // LED-019 (R2-004): previously only pid@24 was verified. The BPF code also
        // reads target_cpu@36 (sched_events.bpf.c:658,683); if a vendor BSP
        // reordered fields after pid, pid@24 could still match while target_cpu
        // silently pointed at the wrong field, corrupting RunqEvent.targetCpu
        // without tripping the offset gate. Verify BOTH fields.
        static const Expect kWakeup[] = { {"pid", 24}, {"target_cpu", 36} };
        if (offs.empty()) {
            offsetStatus_wakeup_.store(TpOffsetStatus::UNKNOWN, std::memory_order_relaxed);
            detailAll += "sched_wakeup=unavailable;";
        } else {
            anyReadable = true;
            std::string d;
            bool ok = checkExpect(offs, kWakeup,
                                  sizeof(kWakeup)/sizeof(kWakeup[0]), d);
            if (!ok) allMatch = false;
            offsetStatus_wakeup_.store(ok ? TpOffsetStatus::VERIFIED
                                          : TpOffsetStatus::DEGRADED,
                                      std::memory_order_relaxed);
            LOG_WARN("offset-verify sched_wakeup: %s -> %s",
                     d.c_str(), tpOffsetStatusToString(offsetStatus_wakeup_.load(std::memory_order_relaxed)));
            detailAll += "sched_wakeup[" + d + "];";
        }
    }

    // P2-6: build the result string locally, then publish it under
    // offsetVerifyMutex_ so the WebUI/state-writer thread's getOffsetVerifyResult()
    // never observes a half-constructed std::string.
    std::string verifyResult;
    if (!anyReadable) {
        verifyResult = "unknown (debugfs/tracefs unavailable)";
        LOG_WARN("offset-verify: no tracepoint format readable; "
                 "ebpf_offset_verified=unknown (debugfs unavailable)");
    } else if (allMatch) {
        verifyResult = "true (" + detailAll + ")";
        LOG_FORCE("offset-verify: hardcoded 4.14 tracepoint offsets MATCH live format");
    } else {
        // v94-r3 fix: a mismatch is no longer "continuing anyway". The
        // per-tracepoint flags above tell deliver*Event() which events to
        // filter. We still write the legacy combined string for back-compat,
        // but the load is expected to call degrade() if sched_switch itself
        // is DEGRADED (see load()).
        verifyResult = "false (" + detailAll + ")";
        LOG_WARN("offset-verify: MISMATCH vs live format (per-tracepoint degradation active). %s",
                 detailAll.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(offsetVerifyMutex_);
        offsetVerifyResult_ = std::move(verifyResult);
    }

    // v94-r3 / P2-12: sched_switch is load-bearing. We can only trust its
    if (offsetStatus_switch_.load(std::memory_order_relaxed) != TpOffsetStatus::VERIFIED) {
        LOG_ERROR("offset-verify: sched_switch offsets NOT VERIFIED "
                  "(status=%s) — eBPF switch data unreliable; will degrade to "
                  "polling after load() completes",
                  tpOffsetStatusToString(offsetStatus_switch_.load(std::memory_order_relaxed)));
    }
    if (offsetStatus_fork_.load(std::memory_order_relaxed) == TpOffsetStatus::DEGRADED ||
        offsetStatus_fork_.load(std::memory_order_relaxed) == TpOffsetStatus::UNKNOWN) {
        LOG_WARN("offset-verify: sched_process_fork offsets not verified "
                 "(status=%s) — fork events will be filtered in deliverForkEvent()",
                 tpOffsetStatusToString(offsetStatus_fork_.load(std::memory_order_relaxed)));
    }
}

// ============================================================================
// Main load entry — auto-detect best method
// ============================================================================
bool EbpfScheduler::load() {
    if (active_) return true;
    state_ = EbpfState::LOADING;
    // P0: unloaded_ is set true by unload() (idempotent guard). It was never
    // reset on the second load(), so after an unload()/load() cycle the new
    // load()'s fds/mmaps/programs would be treated as already-unloaded and
    // leaked at process exit. Clear it here so a re-load tracks fresh resources.
    unloaded_.store(false);
    LOG_FORCE("[eBPF] initializing...");
    LOG_FORCE("EbpfScheduler::load() starting auto-detection");
    LOG_FORCE("eBPF load strategy: KPROBE(embedded) -> TRACEPOINT(fallback) -> syscall_precompiled -> bpftool_precompiled -> POLLING");

    detectEnvironment();

    // v94 fix / 问题43/44/45: verify the hardcoded tracepoint ctx offsets against
    // the live kernel format files BEFORE we depend on them. Read-only; never
    // blocks the load on failure (degrades to "unknown" if debugfs absent).
    verifyTracepointOffsets();

    // v94-r3 fix / G4-001: if sched_switch (the load-bearing tracepoint) is not
    // VERIFIED, the eBPF switch data would be garbage. We still run the attach
    // (constraint: do not change the load path). The offset verdict now runs
    // INSIDE each load phase immediately after a successful attach and BEFORE
    // startPollThread() — previously it ran here, after the poll thread was
    // already draining rings, which left a window where early UNKNOWN events
    // were conservatively dropped before the flags existed. post-load outcomes:
    //   * VERIFIED            -> healthy, flags cleared.
    //   * DEGRADED (readable but != built-in) -> evidence of a different layout:
    //                            the phase latches POLLING/DEGRADED_RING and does
    //                            NOT start the poll thread (fail-safe, no
    //                            auto-recover — recoverIfHealthy() refuses to
    //                            flip back while offsetDegraded_ is set).
    //   * UNKNOWN + known family -> soft-trust flag set, EVENT_DRIVEN kept.
    // deliverSwitchEvent() also filters events as a second layer of defense.

    // v94 fix / 问题46: product-level embedded-bytecode SIZE assertion.
    // P1-5: replaced hardcoded kExpectedSizes with self-validation — iterate
    // all kEmbeddedPrograms and log their sizes. A size drift is surfaced as
    // an INFO log (not WARN) because the header is regenerated from the same
    // source tree; the real protection is the gen_embedded_header.py JMP32
    // check (P2-5) which fails the build on incompatible opcodes.
    {
        LOG_INFO("embedded-size: self-validating %d programs:", kEmbeddedProgramCount);
        for (int pi = 0; pi < kEmbeddedProgramCount; pi++) {
            const auto& info = kEmbeddedPrograms[pi];
            LOG_INFO("embedded-size: [%d] %s = %d bytes (%d refs)",
                     pi, info.sectionName, info.bytecodeSize, info.refCount);
        }
    }

    // G3 integrity gate: refuse ALL privileged eBPF program load/attach when the
    // integrity verdict is not OK. privilegedOpsAllowed() is fail-CLOSED: Unknown
    // (pre-verify) and Failed both deny. This is the outermost choke point that
    // covers embedded_syscall, syscall_precompiled and bpftool_precompiled in one
    // place. On denial we skip every BPF_PROG_LOAD/attach and land in the same
    // POLLING/DEGRADED fallback as a normal all-paths-failed load, so the state
    // machine (round-1) reports truthfully and retryLoadIfDue re-checks the gate.
    // The non-privileged periodic /proc polling backstop is untouched.
    if (!aatpe::integrity::privilegedOpsAllowed()) {
        LOG_ERROR("eBPF: integrity gate not OK; refusing privileged attach; "
                  "stay in polling fallback");
        loaderType_ = EbpfLoaderType::FALLBACK;
        mode_ = EbpfMode::POLLING;
        active_ = true;  // active in fallback (polling) sense, same as all-fail
        state_ = EbpfState::DEGRADED_RING;
        aatpe::subsys::Registry::instance().markError("ebpf",
            "integrity gate refused privileged attach; polling fallback");
        aatpe::subsys::Registry::instance().setState("ebpf",
            aatpe::subsys::SubsystemState::DEGRADED);
        aatpe::subsys::safemode::trigger(aatpe::subsys::safemode::EBPF_STREAK,
            "integrity gate refused eBPF attach");
        // E-001: seed the exponential-backoff clock symmetrically with the
        // all-phases-failed path below, so retryLoadIfDue() (which now correctly
        // re-checks this gate on every backoff step, per the comment above) does
        // not fire an immediate retry on the very next main-loop iteration. The
        // gate itself is still re-evaluated inside load() on every attempt, so
        // this only paces the re-probe; fail-closed behavior is unchanged.
        {
            uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            lastLoadAttemptMs_.store(nowMs, std::memory_order_relaxed);
            if (loadFailureCount_.load(std::memory_order_relaxed) == 0) {
                nextRetryDelayMs_.store(kBackoffBaseMs, std::memory_order_relaxed);
            }
        }
        return true;
    }

    // Phase 1: embedded syscall (KPROBE优先, 内部自动降级到TRACEPOINT)
    LOG_FORCE("--- Phase 1: tryLoadEmbeddedSyscall (KPROBE优先) ---");
    if (tryLoadEmbeddedSyscall()) {
        LOG_FORCE("LOAD OK via embedded_syscall (prog_type=%d, attach=%s)",
                 activeProgType_, activeAttachMethod_.c_str());
        // Offset verdict already ran inside the phase (pre-poll-thread).
        aatpe::subsys::safemode::resetStreak(aatpe::subsys::safemode::EBPF_STREAK);  // G1-001
        return true;
    }
    LOG_WARN("embedded_syscall failed (KPROBE+TRACEPOINT both exhausted), trying syscall_precompiled");
    // v94 fix / 问题51: PARTIAL-ATTACH GUARD. If phase 1 attached some (but not
    // all) programs before failing, tear them down before trying phase 2 so we
    // never run half-attached (some tracepoints live, others not). unload() is
    // safe here: the poll thread has not started yet and active_ is still false.
    unload();

    // Phase 2: syscall precompiled (.o ELF parse + bpf() syscall)
    LOG_FORCE("--- Phase 2: tryLoadSyscallPrecompiled ---");
    if (tryLoadSyscallPrecompiled()) {
        LOG_FORCE("LOAD OK via syscall_precompiled");
        aatpe::subsys::safemode::resetStreak(aatpe::subsys::safemode::EBPF_STREAK);  // G1-001
        return true;
    }
    LOG_WARN("syscall_precompiled failed, trying bpftool_precompiled");
    unload();  // v94 fix / 问题51: same partial-attach guard before phase 3

    // Phase 3: bpftool precompiled
    LOG_FORCE("--- Phase 3: tryLoadBpftoolPrecompiled ---");
    if (tryLoadBpftoolPrecompiled()) {
        LOG_FORCE("LOAD OK via bpftool_precompiled");
        aatpe::subsys::safemode::resetStreak(aatpe::subsys::safemode::EBPF_STREAK);  // G1-001
        return true;
    }
    LOG_WARN("bpftool_precompiled failed, degrading to POLLING (partial attach torn down)");
    unload();  // v94 fix / 问题51: ensure no leftover attach before pure polling

    // All failed — fallback to polling
    LOG_FORCE("========================================");
    LOG_FORCE("ALL eBPF load paths FAILED — falling back to POLLING mode");
    LOG_FORCE("  - embedded_syscall (KPROBE+TRACEPOINT): FAILED");
    LOG_FORCE("  - syscall_precompiled: FAILED");
    LOG_FORCE("  - bpftool_precompiled: FAILED");
    LOG_FORCE("  - Degrading to pure polling (30s interval)");
    LOG_FORCE("========================================");
    loaderType_ = EbpfLoaderType::FALLBACK;
    mode_ = EbpfMode::POLLING;
    active_ = true;  // active in fallback sense (polling will be used)
    state_ = EbpfState::DEGRADED_RING;

    // round-5: report to the unified registry + safe-mode streak trigger.
    aatpe::subsys::Registry::instance().markError("ebpf", "all load paths failed; polling fallback");
    aatpe::subsys::Registry::instance().setState("ebpf", aatpe::subsys::SubsystemState::DEGRADED);
    aatpe::subsys::safemode::trigger(aatpe::subsys::safemode::EBPF_STREAK, "ebpf load failure");

    // record load failure for exponential backoff retry.
    // If this is the very first failure (loadFailureCount_ == 0), seed the
    // backoff clock. retryLoadIfDue() will handle subsequent timing.
    {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        lastLoadAttemptMs_.store(nowMs, std::memory_order_relaxed);
        if (loadFailureCount_.load(std::memory_order_relaxed) == 0) {
            // First failure: start with 1s backoff
            nextRetryDelayMs_.store(kBackoffBaseMs, std::memory_order_relaxed);
        }
        // Note: permanent failure classification (EPERM/EACCES) would require
        // tracking errno from the bpf() syscalls. For safety we retry up to
        // kBackoffMaxRetries times regardless; if the device truly lacks BPF
        // support, retries are cheap (they fail fast on the first syscall).
    }
    return true;
}

// ============================================================================
// State string helpers
// ============================================================================
const char* EbpfScheduler::getStateString() const {
    switch (state_) {
        case EbpfState::UNINITIALIZED: return "UNINITIALIZED";
        case EbpfState::LOADING: return "LOADING";
        case EbpfState::LOAD_TIMEOUT: return "LOAD_TIMEOUT";
        case EbpfState::LOAD_FAILED: return "LOAD_FAILED";
        case EbpfState::ATTACHING: return "ATTACHING";
        case EbpfState::ACTIVE: return "event_driven";
        case EbpfState::DEGRADED: return "DEGRADED";
        case EbpfState::UNLOADED: return "UNLOADED";
        // E-002: ROUND-014 P3 seven-state diagnostic ladder. Previously these
        // fell through to "UNKNOWN", which mis-reported the real failure cause
        // (e.g. a device stuck in DEGRADED_RING/POLLING showed as UNKNOWN).
        case EbpfState::EVENT_DRIVEN: return "EVENT_DRIVEN";
        case EbpfState::DEGRADED_RING: return "DEGRADED_RING";
        case EbpfState::POLLING: return "POLLING";
        case EbpfState::DENIED_SELINUX: return "DENIED_SELINUX";
        case EbpfState::NO_CAP: return "NO_CAP";
        case EbpfState::KERNEL_UNSUPPORTED: return "KERNEL_UNSUPPORTED";
        case EbpfState::UNVERIFIED: return "UNVERIFIED";
    }
    return "UNKNOWN";
}

// ROUND-014 P3: seven-state string via single mapping function.
const char* EbpfScheduler::getEbpfStateSevenString() const {
    return aatpe::ebpfStateToString((int)state_.load());
}

const char* EbpfScheduler::getLoaderString() const {
    switch (loaderType_) {
        case EbpfLoaderType::NONE: return "none";
        case EbpfLoaderType::SYSCALL_PRECOMPILED: return "syscall_precompiled";
        case EbpfLoaderType::BPFTOOL_PRECOMPILED: return "bpftool_precompiled";
        case EbpfLoaderType::BPFTOOL_RUNTIME: return "bpftool_runtime";
        case EbpfLoaderType::FALLBACK: return "fallback";
        case EbpfLoaderType::EMBEDDED_SYSCALL: return "embedded_syscall";
        default: return "unknown";
    }
}

const char* EbpfScheduler::getModeString() const {
    switch (mode_) {
        case EbpfMode::POLLING: return "polling";
        case EbpfMode::EVENT_DRIVEN: return "event_driven";
        case EbpfMode::HYBRID: return "hybrid";
        default: return "unknown";
    }
}

// ============================================================================
// Legacy CPU stats fallback
// ============================================================================
bool EbpfScheduler::readFallbackSchedstat(std::vector<CpuSchedStats>& outStats) {
    outStats.clear();
    std::ifstream f("/proc/schedstat");
    if (!f.good()) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) continue;
        std::istringstream iss(line);
        std::string cpuStr;
        iss >> cpuStr;
        CpuSchedStats stats;
        uint64_t ts = 0, runTime = 0, runCount = 0, waitTime = 0;
        std::string token;
        int field = 0;
        while (iss >> token && field < 4) {
            try {
                uint64_t val = std::stoull(token);
                switch (field) {
                    case 0: ts = val; break;
                    case 1: runTime = val; break;
                    case 2: runCount = val; break;
                    case 3: waitTime = val; break;
                }
            } catch (const std::exception&) {
                // Periodic schedstat field parse; a non-numeric field is skipped.
                // LOG_DEBUG only (release builds trim it: zero hot-path cost).
                LOG_DEBUG("schedstat: non-numeric field %d, skipping", field);
            }
            field++;
        }
        stats.totalRuntimeNs = runTime;
        stats.totalWaitTimeNs = waitTime;
        stats.switchCount = (uint32_t)runCount;
        (void)ts;
        outStats.push_back(stats);
    }
    return !outStats.empty();
}

bool EbpfScheduler::readCpuStats(std::vector<CpuSchedStats>& outStats) {
    outStats.clear();
    if (loaderType_ == EbpfLoaderType::FALLBACK || !active_) {
        return readFallbackSchedstat(outStats);
    }
    std::vector<uint64_t> counts;
    if (readEmbeddedCpuCounts(counts)) {
        for (size_t i = 0; i < counts.size(); i++) {
            CpuSchedStats stats;
            stats.switchCount = (uint32_t)counts[i];
            outStats.push_back(stats);
        }
        return true;
    }
    return readFallbackSchedstat(outStats);
}

// ============================================================================
// ROUND-004: config hot-reload convergence.
// ============================================================================
bool EbpfScheduler::onConfigReload(const EbpfRuntimeConfig& cfg) {
    // Serialize against the main-loop retryLoadIfDue() so two threads cannot
    // interleave unload()/load() on the shared fd/ring vectors.
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    // Live-apply the cheap knobs first (no reload needed).
    if (cfg.pollFallback != pollFallbackEnabled_.load(std::memory_order_relaxed)) {
        setPollFallbackEnabled(cfg.pollFallback);
    }
    const bool ringPagesChanged = (cfg.ringPages != ringPages_);
    if (ringPagesChanged) {
        setRingPages(cfg.ringPages);  // takes effect on next attach
    }

    const bool currentlyActive = (active_.load() && !unloaded_.load(std::memory_order_relaxed));

    if (!cfg.enabled) {
        // 1 -> 0: safe teardown. No-op if already stopped.
        if (!currentlyActive) return false;
        LOG_WARN("eBPF config reload: enabled=0 -> safe unload (stop ring/agg, detach)");
        unload();
        active_ = false;
        state_ = EbpfState::UNLOADED;
        return true;
    }

    // enabled = true. Already live with unchanged ring size -> no-op (dedupe).
    if (currentlyActive && !ringPagesChanged) return false;

    // 0 -> 1, or ringPages changed while enabled: (re)load. unload() first to
    // avoid a half-attached state (reuses the existing inter-phase guard).
    if (!unloaded_.load(std::memory_order_relaxed)) {
        unload();
    }
    LOG_FORCE("eBPF config reload: enabled=1 -> load (ringPages=%d)", cfg.ringPages);
    // E-004: propagate load()'s real result instead of discarding it and
    // hardcoding true. load() itself returns true even when it lands in the
    // non-privileged POLLING fallback (that is a handled outcome, not a crash),
    // so callers keep the historical "reload action taken" semantics; what this
    // fixes is that the return value is no longer a lie when load() ever does
    // return false. Combined with the E-001 retry gate, a reload-triggered
    // failure now also feeds the exponential-backoff retry.
    return load();
}

void EbpfScheduler::registerConfigReloadHook() {
    // Production adapter: convert the global Config snapshot to the decoupled
    // runtime knob struct and converge. Fires on boot and every hot-reload.
    aatpe::config::registerReloadHook("ebpf", [this](const Config& c) {
        onConfigReload({c.ebpfEnabled, c.ebpfRingPages, c.ebpfPollFallback});
    });
}

// ============================================================================
// Unload
// ============================================================================
void EbpfScheduler::unload() {
    if (unloaded_.load()) return;  // idempotent: main may call unload(), then dtor calls again
    unloaded_.store(true);
    stopPollThread();  // v83: stop dedicated poll thread before tearing down
    active_ = false;
    state_ = EbpfState::UNLOADED;

    unmapAllRings();

    for (int fd : tracepointFds_) {
        if (fd >= 0) {
            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            close(fd);
        }
    }
    tracepointFds_.clear();

    // #21: close raw tracepoint link fds (closing = detach).
    for (int fd : rawTracepointLinks_) {
        if (fd >= 0) close(fd);
    }
    rawTracepointLinks_.clear();

    // Clean up kprobe events created in /sys/kernel/tracing/kprobe_events
    cleanupKprobeEvents();
    // P2-4: the debugfs kprobe rows are gone now; drop the dedup cache so the
    // next load() re-creates them fresh (kpId values are invalid after unload).
    kprobeIdCache_.clear();

    // v94 fix(P45): syscallPerfFds_ / embeddedPerfFds_ close loops removed —
    // legacy vectors never filled in new load flow.

    for (int fd : progFds_) {
        if (fd >= 0) close(fd);
    }
    progFds_.clear();

    for (int fd : mapFds_) {
        if (fd >= 0) close(fd);
    }
    mapFds_.clear();

    // v94 fix(P46): progFd_ / mapFd_ legacy single-fd close lines removed —
    // new flow uses progFds_ / mapFds_ vectors above.

    eventsMapFd_ = -1;
    countersMapFd_ = -1;
    threadCacheMapFd_ = -1;
    onlineCpuIds_.clear();  // #24: re-read on next load (hotplug may have changed mask)

    // EBPF-03 fix: drain stale SPSC queues and switchAgg_ so that after
    // unload()+load() the new epoch does not dispatch FORK/SWITCH events whose
    // pid/comm came from before unload(). The poll thread is already stopped
    // (stopPollThread() above joins it), so no producer is running; the main
    // loop has already drained in the normal lifecycle, but belt-and-suspenders.
    queueEnabled_ = false;
    criticalQueue_.clear();
    normalQueue_.clear();
    {
        std::lock_guard<std::mutex> lk(switchAggMutex_);
        switchAgg_.clear();
    }

    // EBPF-16: reset the one-shot offset-filter WARN flags (g_forkWarned /
    // g_exitWarned / g_switchWarned / g_execWarned). They are file-scope
    // statics with no notion of load epoch: without this reset, a kernel whose
    // tracepoint offsets never verify would emit its "filtering X events" WARN
    // exactly ONCE for the whole daemon lifetime — after the first unload()
    // (hotplug rebuild / config reload / ring resize), every later reload would
    // stay silent in the logs even though events are still being dropped.
    // The poll thread is already joined above, so no deliver*Event() can race
    // these stores.
    g_forkWarned.store(false, std::memory_order_relaxed);
    g_exitWarned.store(false, std::memory_order_relaxed);
    g_switchWarned.store(false, std::memory_order_relaxed);
    g_execWarned.store(false, std::memory_order_relaxed);

    // NOTE: no LOG_INFO here — this function is called from the static destructor
    // (~EbpfScheduler), at which point g_logMutex may already have been destroyed
    // (static destruction order: aatped_logging.cpp destructs before aatped_main.cpp).
    // Calling logMsg() on a destroyed mutex triggers FORTIFY abort.
}

// ============================================================================
// ThreadCpuRuntimeAggregator (legacy)
// ============================================================================
ThreadCpuRuntimeAggregator::ThreadCpuRuntimeAggregator() {}
ThreadCpuRuntimeAggregator::~ThreadCpuRuntimeAggregator() {}

uint64_t ThreadCpuRuntimeAggregator::update(int tid, int cpu, uint64_t runTimeNs, uint64_t timestampNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = (uint64_t)tid;
    auto it = snapshots_.find(key);
    uint64_t delta = 0;
    if (it != snapshots_.end()) {
        if (timestampNs > it->second.timestamp && runTimeNs >= it->second.runTimeNs)
            delta = runTimeNs - it->second.runTimeNs;
        it->second.runTimeNs = runTimeNs;
        it->second.timestamp = timestampNs;
        it->second.cpu = cpu;
    } else {
        ThreadCpuRuntime r;
        r.tid = tid; r.cpu = cpu; r.runTimeNs = runTimeNs; r.timestamp = timestampNs;
        snapshots_[key] = r;
    }
    return delta;
}

void ThreadCpuRuntimeAggregator::getAllSnapshots(std::vector<ThreadCpuRuntime>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    for (const auto& pair : snapshots_) out.push_back(pair.second);
}

void ThreadCpuRuntimeAggregator::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.clear();
}

// ============================================================================
// Constructors
// ============================================================================
EbpfScheduler::EbpfScheduler() {}
EbpfScheduler::~EbpfScheduler() { unload(); }

} // namespace aatpe_ebpf

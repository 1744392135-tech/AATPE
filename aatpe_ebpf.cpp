#include "aatpe_ebpf.h"
#include "aatped.h"
#include "aatpe_subsystem.h"
#include "integrity.h"
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
const char* ebpfStateToString(int s) {
    using S = aatpe_ebpf::EbpfState;
    switch ((S)s) {
        case S::EVENT_DRIVEN:  return "event_driven";
        case S::ACTIVE:        return "event_driven";
        case S::DEGRADED_RING: return "degraded_ring";
        case S::POLLING:       return "polling";
        case S::DEGRADED:      return "polling";
        case S::DENIED_SELINUX:return "denied_selinux";
        case S::NO_CAP:        return "no_cap";
        case S::KERNEL_UNSUPPORTED: return "kernel_unsupported";
        case S::UNVERIFIED:    return "unverified";
        case S::LOAD_TIMEOUT:  return "kernel_unsupported";
        case S::LOADING: case S::UNINITIALIZED: return "unverified";
        default:               return "unverified";
    }
}
}

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
}
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
#ifndef BPF_MAP_TYPE_LRU_HASH
#define BPF_MAP_TYPE_LRU_HASH 9
#endif

#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif

#ifndef PERF_COUNT_SW_BPF_OUTPUT
#define PERF_COUNT_SW_BPF_OUTPUT 10
#endif

static const int kLoadTimeoutMs = 2000;
static const size_t kBpfLogSize = 65536;
static thread_local char bpfLogBuf[kBpfLogSize + 1];

static uint32_t computeKernelVersionCode() {
    struct utsname buf;
    if (uname(&buf) != 0) return 0;
    int major = 0, minor = 0, patch = 0;
    if (sscanf(buf.release, "%d.%d.%d", &major, &minor, &patch) >= 2)
        return (uint32_t)((major << 16) | (minor << 8) | (patch & 0xFF));
    return 0;
}

std::vector<int> parseCpuOnlineMask(const std::string& text) {
    std::vector<int> out;
    if (text.empty()) return out;
    char* work = strdup(text.c_str());
    if (!work) return out;
    char* saveptr = nullptr;
    for (char* tok = strtok_r(work, ",", &saveptr); tok;
         tok = strtok_r(nullptr, ",", &saveptr)) {
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
    runCapabilityDetection();
}

#ifdef AATPE_HOST_TEST
static std::atomic<uint32_t>& hostBpfInjectFlags() {
    static std::atomic<uint32_t> f{0};
    return f;
}
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

static int wakeupCacheMapDecision(int lruFd, int lruErrno) {
    if (lruFd >= 0) return 0;
    if (lruErrno == EINVAL) return 1;
    return -1;
}
#ifdef AATPE_HOST_TEST
extern "C" int aatpe_wakeup_map_decision_for_test(int lruFd, int lruErrno) {
    return wakeupCacheMapDecision(lruFd, lruErrno);
}
extern "C" int aatpe_wakeup_preferred_type_for_test() { return BPF_MAP_TYPE_LRU_HASH; }
extern "C" int aatpe_wakeup_embedded_type_for_test()   { return kEmbeddedMaps[2].type; }
#endif

int EbpfScheduler::loadBpfProgFromBytes(const uint8_t* bytes, size_t len,
                                         const char* license,
                                         const std::vector<int>& mapFds,
                                         int progType) {
    if (!bytes || len == 0 || len % 8 != 0) return -1;

    std::vector<uint8_t> patched;
    patched.assign(bytes, bytes + len);

    size_t mapRefIdx = 0;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint8_t code = patched[i];
        uint8_t src_reg = (patched[i + 1] >> 4) & 0x0F;
        if (code == 0x18 && src_reg == 1) {
            int fd = (mapRefIdx < mapFds.size()) ? mapFds[mapRefIdx] : -1;
            if (fd < 0) {
                LOG_WARN("loadBpfProgFromBytes(sequential): unresolved map reference "
                         "#%zu (mapFds=%zu) — refusing to patch", mapRefIdx, mapFds.size());
                return -1;
            }
            {
                int32_t imm = fd;
                std::memcpy(patched.data() + i + 4, &imm, sizeof(imm));
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
    attr.log_size = kBpfLogSize;
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

int EbpfScheduler::loadBpfProgFromBytesWithRefs(const uint8_t* bytes, size_t len,
                                                  const char* license,
                                                  const std::vector<int>& mapFds,
                                                  const std::vector<int>& mapIndices,
                                                  int progType) {
    if (!bytes || len == 0 || len % 8 != 0) return -1;

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
            if (fd < 0) {
                LOG_WARN("loadBpfProgFromBytesWithRefs: unresolved map reference #%zu (idx list size=%zu, mapFds=%zu) — refusing to patch to events_map",
                         mapRefIdx, mapIndices.size(), mapFds.size());
                return -1;
            }
            {
                int32_t imm = fd;
                std::memcpy(patched.data() + i + 4, &imm, sizeof(imm));
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
    attr.log_size = kBpfLogSize;
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

int EbpfScheduler::loadBpfProgFromBytesWithExplicitRefs(
    const uint8_t* bytes, size_t len, const char* license,
    const int* refOffsets, const int* refMapIndices, int refCount,
    const std::vector<int>& mapFds, int progType) {
    if (!bytes || len == 0 || len % 8 != 0) return -1;

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
        uint8_t op = patched[off];
        if (op != 0x18) {
            LOG_WARN("explicit-ref: ref[%d] offset=%d expected op=0x18 but got 0x%02x, skipping",
                     ri, off, op);
            continue;
        }
        uint8_t regs = patched[off + 1];
        uint8_t dst_reg = regs & 0x0F;
        patched[off + 1] = static_cast<uint8_t>(dst_reg | (1u << 4));

        {
            int32_t imm = fd;
            std::memcpy(patched.data() + off + 4, &imm, sizeof(imm));
        }

        if ((size_t)(off + 16) <= len) {
            int32_t immHi = 0;
            std::memcpy(patched.data() + off + 12, &immHi, sizeof(immHi));
        }
        patchedCount++;
    }
    LOG_INFO("explicit-ref: patched %d/%d map references (prog_type=%d)",
             patchedCount, refCount, progType);

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = (uint32_t)progType;
    attr.insn_cnt = (uint32_t)(len / sizeof(struct bpf_insn));
    attr.insns = (uint64_t)(unsigned long)patched.data();
    attr.license = (uint64_t)(unsigned long)license;
    attr.kern_version = computeKernelVersionCode();
    memset(bpfLogBuf, 0, sizeof(bpfLogBuf));
    attr.log_buf = (uint64_t)(unsigned long)bpfLogBuf;
    attr.log_size = kBpfLogSize;
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

int EbpfScheduler::readTracepointId(const char* tpPath) {
    std::string path(tpPath);
    if (path.rfind("tracepoint/", 0) == 0)
        path = path.substr(11);
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

struct AttachStrategy {
    uint32_t size;
    uint64_t sample_period;
    uint32_t wakeup_events;
    uint64_t sample_type;
    bool enable_first;
};

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
    int lastFailStage = 0;
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

bool EbpfScheduler::openRingBufferOnlyEvent(int tpId, int cpu, int& outPerfFd) {
    (void)tpId;
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_SOFTWARE;
    pe.size = 64;
    pe.config = PERF_COUNT_SW_BPF_OUTPUT;
    pe.sample_period = 1;
    pe.sample_type = PERF_SAMPLE_RAW;
    pe.wakeup_events = 1;
    pe.disabled = 1;

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

int EbpfScheduler::setupPerCpuRingBuffers(int tpId, int attachFd, int attachCpu,
                                           int ncpus, int progIndex) {
    if (progIndex != 0) return 0;
    int ringsMapped = 0;

    LOG_INFO("setupPerCpuRingBuffers: v82 PERF_TYPE_SOFTWARE/BPF_OUTPUT mode — "
             "eventsMapFd_=%d attachFd=%d attachCpu=%d ncpus=%d",
             eventsMapFd_, attachFd, attachCpu, ncpus);

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

        if (!mmapPerfRingForCpu(cpu, ringFd, ringPages_)) {
            LOG_WARN("setupPerCpuRingBuffers: mmap failed for cpu=%d fd=%d", cpu, ringFd);
            tracepointFds_.pop_back();
            close(ringFd);
            continue;
        }
        ringsMapped++;

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

    ringsMappedCount_.store(ringsMapped, std::memory_order_relaxed);
    ringsTotalCount_.store(totalRings, std::memory_order_relaxed);
    if (ringsMapped < totalRings) {
        LOG_WARN("setupPerCpuRingBuffers: PARTIAL ring coverage %d/%d — events on %d CPU(s) will be silently dropped by the kernel",
                 ringsMapped, totalRings, totalRings - ringsMapped);
    }

    return ringsMapped;
}

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
    kprobeEventNames_.push_back(eventName);
    kprobeIdCache_[key] = {eventName, kpId};
    outEventName = eventName;
    LOG_FORCE("kprobe ensure: created event=%s kpId=%d (shared across all CPUs)",
              eventName.c_str(), kpId);
    return kpId;
}

bool EbpfScheduler::attachKprobe(int progFd, const char* funcName, bool isKretprobe,
                                  int cpu, int& outPerfFd, std::string& outEventName) {
    LOG_FORCE("kprobe attach: func=%s is_ret=%d cpu=%d progFd=%d",
             funcName, isKretprobe, cpu, progFd);

    int kpId = ensureKprobeEvent(funcName, isKretprobe, outEventName);
    if (kpId <= 0) {
        LOG_WARN("kprobe attach: ensure event failed for func=%s cpu=%d", funcName, cpu);
        return false;
    }

    if (!attachTracepoint(progFd, kpId, cpu, outPerfFd)) {
        LOG_WARN("kprobe attach: perf_event_open/SET_BPF failed for %s cpu=%d (kpId=%d)",
                 outEventName.c_str(), cpu, kpId);
        return false;
    }

    LOG_FORCE("kprobe attach: SUCCESS func=%s event=%s cpu=%d perfFd=%d kpId=%d",
             funcName, outEventName.c_str(), cpu, outPerfFd, kpId);
    return true;
}

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
            if (attempt > 0) {
                LOG_INFO("updateEventsMap: events_map[%d]=%d OK (after %d retries)",
                         cpu, perfFd, attempt);
            }
            return true;
        }

        int savedErrno = errno;
        LOG_WARN("updateEventsMap: update FAILED cpu=%d fd=%d attempt=%d errno=%d (%s) — retrying",
                 cpu, perfFd, attempt, savedErrno, aatpeErrStr(savedErrno));
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
    for (auto& ring : rings_) {
        if (ring.base != nullptr && ring.mmapSize > 0) {
            munmap(ring.base, ring.mmapSize);
        }
        if (ring.perfFd >= 0) {
            ioctl(ring.perfFd, PERF_EVENT_IOC_DISABLE, 0);
            ring.perfFd = -1;
        }
        ring.base = nullptr;
        ring.mmapSize = 0;
    }
    rings_.clear();
    unmapPerfRing();
}

int EbpfScheduler::processRingData(PerfRing& ring, int maxEvents) {
    if (!ring.base || ring.mmapSize == 0) return 0;

    static const long s_pageSize = []() {
        long ps = sysconf(_SC_PAGESIZE);
        return (ps > 0) ? ps : 4096L;
    }();
    const long pageSize = s_pageSize;

    auto* header = reinterpret_cast<volatile struct perf_event_mmap_page*>(ring.base);
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

    if (dataHead - dataTail > dataSize) {
        uint64_t lostBytes = (dataHead - dataTail) - dataSize;
        uint64_t consumed = eventCountTotal_.load(std::memory_order_relaxed);
        uint64_t avgSize = (consumed > 0)
            ? (ringConsumedBytes_.load(std::memory_order_relaxed) / consumed)
            : 64ULL;
        if (avgSize < 16) avgSize = 16;
        uint64_t lostEvents = lostBytes / avgSize;
        windowLost_.fetch_add(lostEvents, std::memory_order_relaxed);
        lastRingLossMs_.store((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
        dataTail = dataHead - dataSize;
    }

    int processed = 0;
    uint8_t* dataBase = static_cast<uint8_t*>(ring.base) + pageSize;

    struct LocalSwitchAcc {
        uint32_t count = 0;
        uint64_t lastTs = 0;
        SwitchEvent firstEv;
    };
    std::unordered_map<uint32_t, LocalSwitchAcc> localSwitchBatch;
    localSwitchBatch.reserve(64);
    std::vector<SwitchEvent> pendingSwitchDeliver;

    while (dataTail < dataHead && processed < maxEvents) {
        uint64_t offset = dataTail % dataSize;
        if (offset + sizeof(struct perf_event_header) > dataSize) {
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
            uint64_t sampleOffset = offset + sizeof(struct perf_event_header);
            if (sampleOffset + 4 <= dataSize) {
                uint32_t rawSize = *reinterpret_cast<volatile uint32_t*>(dataBase + sampleOffset);
                uint64_t rawOffset = sampleOffset + 4;
                if (rawSize >= sizeof(EventHeader) && rawOffset + rawSize <= dataSize) {
                    uint32_t evtType = 0;
                    memcpy(&evtType, dataBase + rawOffset, sizeof(evtType));
                    bool validBpfEvent = (evtType >= 1 && evtType <= 5);
                    if (!validBpfEvent) {
                        dataTail += size;
                        continue;
                    }
                    switch (evtType) {
                        case (uint32_t)EventType::FORK: {
                            if (rawSize >= sizeof(ForkEvent)) {
                                ForkEvent ev;
                                memcpy(&ev, dataBase + rawOffset, sizeof(ForkEvent));
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
        ringConsumedBytes_.fetch_add(size, std::memory_order_relaxed);
        (void)misc;
    }

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
                    deliverNow = true;
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

    __atomic_store_n(&header->data_tail, dataTail, __ATOMIC_RELEASE);
    if (processed > 0) {
        windowEvents_.fetch_add((uint64_t)processed, std::memory_order_relaxed);
    }
    return processed;
}

#ifdef AATPE_HOST_TEST
int EbpfScheduler::hostTestParseRingBlob(const void* data, size_t dataBytes) {
    static const long kPage = 4096;
    if (dataBytes > (size_t(1) << 20)) return -1;
    offsetStatus_fork_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_switch_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_exec_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_exit_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_wakeup_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);

    std::vector<uint8_t> buf(kPage + dataBytes, 0);
    auto* hdr = reinterpret_cast<struct perf_event_mmap_page*>(buf.data());
    hdr->data_tail = 0;
    hdr->data_head = dataBytes;
    if (dataBytes && data) memcpy(buf.data() + kPage, data, dataBytes);

    PerfRing ring;
    ring.base = buf.data();
    ring.mmapSize = kPage + dataBytes;
    ring.cpu = 0;
    return processRingData(ring, 1 << 20);
}
#endif

namespace {
inline void wakeCriticalIfSet(const std::function<void()>& hook) {
    if (!hook) return;
    static std::atomic<long long> s_lastWakeMs{0};
    long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long last = s_lastWakeMs.load(std::memory_order_relaxed);
    if (nowMs - last < 50) return;
    if (s_lastWakeMs.compare_exchange_strong(last, nowMs, std::memory_order_relaxed)) {
        hook();
    }
}
}

namespace {
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
}

#ifdef AATPE_HOST_TEST
static int embeddedCpuBufferSlots(long possibleCpus);
bool EbpfScheduler::hostTestOffsetWarnFired(char kind) const {
    switch (kind) {
        case 'f': return g_forkWarned.load(std::memory_order_relaxed);
        case 'x': return g_exitWarned.load(std::memory_order_relaxed);
        case 's': return g_switchWarned.load(std::memory_order_relaxed);
        case 'e': return g_execWarned.load(std::memory_order_relaxed);
        default:  return false;
    }
}

bool EbpfScheduler::hostTestParseElfMapIndices(const std::string& path,
                                               std::vector<std::vector<int>>& outMapIndices) {
    std::vector<std::vector<uint8_t>> progs;
    std::vector<std::string> tps;
    std::string lic;
    outMapIndices.clear();
    return parseElfPrograms(path, progs, tps, outMapIndices, lic);
}

int EbpfScheduler::hostTestEmbeddedCpuBufferSlots(long possibleCpus) {
    return embeddedCpuBufferSlots(possibleCpus);
}

void EbpfScheduler::hostTestSetOffsetDegraded(char kind) {
    switch (kind) {
        case 'f': offsetStatus_fork_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
        case 'x': offsetStatus_exit_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
        case 's': offsetStatus_switch_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
        case 'e': offsetStatus_exec_.store(TpOffsetStatus::DEGRADED, std::memory_order_relaxed); break;
    }
}

void EbpfScheduler::hostTestSetOffsetStatus(char kind, TpOffsetStatus s) {
    switch (kind) {
        case 'f': offsetStatus_fork_.store(s, std::memory_order_relaxed); break;
        case 's': offsetStatus_switch_.store(s, std::memory_order_relaxed); break;
        case 'w': offsetStatus_wakeup_.store(s, std::memory_order_relaxed); break;
    }
}
#endif

bool EbpfScheduler::rateLimitCriticalEvent(void) {
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t winStart = criticalWindowStartMs_.load(std::memory_order_relaxed);
    if (winStart == 0 || (nowMs - winStart) >= 1000) {
        criticalWindowStartMs_.store(nowMs, std::memory_order_relaxed);
        criticalEventsInWindow_.store(1, std::memory_order_relaxed);
        return false;
    }
    uint64_t count = criticalEventsInWindow_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > (uint64_t)kMaxCriticalPerSec) {
        criticalDroppedByRate_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void EbpfScheduler::deliverForkEvent(const ForkEvent& ev) {
    if (!isForkOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_forkWarned)) {
            LOG_WARN("offset-gate: sched_process_fork offsets mismatch — "
                     "filtering fork events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    if (rateLimitCriticalEvent()) return;
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.fork = ev;
        criticalQueue_.push(a);
        wakeCriticalIfSet(criticalEventWakeHook_);
    } else if (eventConsumer_) {
        eventConsumer_->onFork(ev);
        wakeCriticalIfSet(criticalEventWakeHook_);
    }
}

void EbpfScheduler::deliverExitEvent(const ExitEvent& ev) {
    if (!isExitOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_exitWarned)) {
            LOG_WARN("offset-gate: sched_process_exit offsets mismatch — "
                     "filtering exit events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    if (rateLimitCriticalEvent()) return;
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.exit = ev;
        criticalQueue_.push(a);
        wakeCriticalIfSet(criticalEventWakeHook_);
    } else if (eventConsumer_) {
        eventConsumer_->onExit(ev);
        wakeCriticalIfSet(criticalEventWakeHook_);
    }
}

void EbpfScheduler::deliverSwitchEvent(const SwitchEvent& ev) {
    if (!isSwitchOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_switchWarned)) {
            LOG_WARN("offset-gate: sched_switch offsets mismatch — "
                     "filtering switch events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.sw = ev;
        normalQueue_.push(a);
    } else if (eventConsumer_) {
        eventConsumer_->onSwitch(ev);
    }
}

void EbpfScheduler::deliverExecEvent(const ExecEvent& ev) {
    if (!isExecOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        if (firstDrop(g_execWarned)) {
            LOG_WARN("offset-gate: sched_process_exec offsets mismatch — "
                     "filtering exec events (count now=%llu)",
                     (unsigned long long)offsetFilteredCount_.load());
        }
        return;
    }
    if (rateLimitCriticalEvent()) return;
    if (queueEnabled_) {
        AnyEvent a;
        a.hdr = ev.hdr;
        a.data.exec = ev;
        criticalQueue_.push(a);
        wakeCriticalIfSet(criticalEventWakeHook_);
    } else if (eventConsumer_) {
        eventConsumer_->onExec(ev);
        wakeCriticalIfSet(criticalEventWakeHook_);
    }
}

void EbpfScheduler::deliverRunqEvent(const RunqEvent& ev) {
    if (!isWakeupOffsetTrusted() || !isSwitchOffsetTrusted()) {
        offsetFilteredCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t prevMax = runqMaxDelayNs_.load(std::memory_order_relaxed);
    while (ev.delayNs > prevMax &&
           !runqMaxDelayNs_.compare_exchange_weak(
               prevMax, ev.delayNs, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    if (eventConsumer_) {
        eventConsumer_->onRunqLatency(ev);
    }
}

size_t EbpfScheduler::dispatchQueuedEvents() {
    if (!eventConsumer_) return 0;
    static thread_local std::vector<AnyEvent> s_batch;
    s_batch.clear();

    size_t total = 0;

    size_t nCritical = criticalQueue_.popBatch(s_batch, kCriticalDispatchBudgetPerCycle);
    for (size_t i = 0; i < nCritical; ++i) {
        const AnyEvent& a = s_batch[i];
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
            default: break;
        }
    }
    total += nCritical;

    s_batch.clear();
    size_t nNormal = normalQueue_.popBatch(s_batch, kDispatchBudgetPerCycle);
    for (size_t i = 0; i < nNormal; ++i) {
        const AnyEvent& a = s_batch[i];
        if ((uint32_t)a.hdr.type == (uint32_t)EventType::SWITCH) {
            SwitchEvent ev; std::memcpy(&ev, &a.data, sizeof(ev)); eventConsumer_->onSwitch(ev);
        }
    }
    total += nNormal;

    lastCycleEventsProcessed_.store((uint64_t)total, std::memory_order_relaxed);
    return total;
}

uint64_t EbpfScheduler::getQueueDropped() const {
    return criticalQueue_.droppedCount() + normalQueue_.droppedCount();
}

size_t EbpfScheduler::getQueueSize() const {
    return criticalQueue_.sizeApprox() + normalQueue_.sizeApprox();
}

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
    s.criticalDropped   = criticalQueue_.droppedCount();
    s.normalDropped     = normalQueue_.droppedCount();
    s.criticalBacklog   = criticalQueue_.sizeApprox();
    s.eventsProcessedLastCycle = lastCycleEventsProcessed_.load(std::memory_order_relaxed);
    s.dropped += criticalDroppedByRate_.load(std::memory_order_relaxed);
    return s;
}

int EbpfScheduler::pollEvents(int maxEvents) {
    if (!active_) return 0;

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

    if (mode_ == EbpfMode::POLLING) return 0;
    if (rings_.empty()) return 0;

    static const long s_pageSizeCached = []() {
        long ps = sysconf(_SC_PAGESIZE);
        return (ps > 0) ? ps : 4096L;
    }();

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

    int pollRet = poll(s_pfds.data(), s_pfds.size(), 0);
    (void)pollRet;

    static std::atomic<uint64_t> s_pollDiagCounter{0};
    bool logThisCall = ((s_pollDiagCounter.fetch_add(1) % 256) == 0);

    int total = 0;

    size_t pfdIdx = 0;
    for (auto& ring : rings_) {
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
                    uint64_t dataSize = (ring.mmapSize > (size_t)s_pageSizeCached)
                        ? (uint64_t)(ring.mmapSize - (size_t)s_pageSizeCached) : 0;
                    (void)dataSize;
                    auto* hdr = reinterpret_cast<volatile struct perf_event_mmap_page*>(ring.base);
                    uint64_t fbHead = __atomic_load_n(&hdr->data_head, __ATOMIC_ACQUIRE);
                    uint64_t fbTail = __atomic_load_n(&hdr->data_tail, __ATOMIC_RELAXED);
                    if (fbHead != fbTail) {
                        int perRingMax = maxEvents - total;
                        if (perRingMax > 0) {
                            if (logThisCall) {
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
        LOG_DEBUG("[eBPF-DIAG] pollEvents: pollRet=%d rings=%zu processed=%d totalEvents=%llu "
                 "fork=%llu exit=%llu switch=%llu exec=%llu",
                 pollRet, rings_.size(), total,
                 (unsigned long long)eventCountTotal_.load(),
                 (unsigned long long)eventCountFork_.load(),
                 (unsigned long long)eventCountExit_.load(),
                 (unsigned long long)eventCountSwitch_.load(),
                 (unsigned long long)eventCountExec_.load());

        if (countersMapFd_ >= 0) {
            std::vector<uint64_t> bpfCounts;
            if (readEmbeddedCpuCounts(bpfCounts)) {
                uint64_t bpfTotal = 0;
                for (uint64_t v : bpfCounts) bpfTotal += v;
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

    uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastCheck = lastRateCheckNs_.load();
    if (lastCheck > 0) {
        uint64_t elapsedNs = now - lastCheck;
        if (elapsedNs >= 1000000000ULL) {
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

void EbpfScheduler::evictStaleSwitchAggLocked(uint64_t nowMs) {
    if (switchAgg_.empty()) return;
    uint64_t nowNs = nowMs * 1000000ULL;
    uint64_t cutoffNs = (nowNs > kSwitchAggEntryTtlMs * 1000000ULL)
        ? nowNs - kSwitchAggEntryTtlMs * 1000000ULL : 0;

    for (auto it = switchAgg_.begin(); it != switchAgg_.end(); ) {
        if (it->second.lastTs < cutoffNs) {
            it = switchAgg_.erase(it);
        } else {
            ++it;
        }
    }
    if (switchAgg_.size() > kMaxSwitchAggEntries) {
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
    std::lock_guard<std::mutex> aggLock(switchAggMutex_);
    evictStaleSwitchAggLocked(nowMs);
}

void EbpfScheduler::pollThreadFunc() {
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
        LOG_WARN("[eBPF-v89] pollThreadFunc: setpriority(-10) failed: %s",
                 aatpeErrStr(errno));
    }

    LOG_INFO("[eBPF-v89] pollThreadFunc: started (poll-based adaptive, nice=-10)");

    static const int kTimeoutLevels[] = {20, 50, 100, 200, 500};
    static const int kNumLevels = (int)(sizeof(kTimeoutLevels) / sizeof(kTimeoutLevels[0]));
    int timeoutIdx = 0;

    std::vector<struct pollfd> pfds;
    pfds.reserve(16);

    while (!pollThreadStop_.load(std::memory_order_acquire)) {
        if (!active_.load(std::memory_order_acquire) ||
            mode_.load(std::memory_order_acquire) == EbpfMode::POLLING ||
            rings_.empty()) {
            struct pollfd wfd;
            wfd.fd = pollWakeupReadFd_;
            wfd.events = POLLIN;
            wfd.revents = 0;
            if (pollWakeupReadFd_ >= 0) {
                poll(&wfd, 1, 200);
                char drainBuf[32];
                while (read(pollWakeupReadFd_, drainBuf, sizeof(drainBuf)) > 0) {}
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            timeoutIdx = 0;
            continue;
        }

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
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        int timeoutMs = kTimeoutLevels[timeoutIdx];
        int pollRet = poll(pfds.data(), pfds.size(), timeoutMs);

        if (pollRet < 0 && errno == EINTR) {
            continue;
        }

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

        int totalProcessed = 0;
        while (totalProcessed < 4096) {
            int processed = pollEvents(4096 - totalProcessed);
            if (processed <= 0) break;
            totalProcessed += processed;
        }

        if (totalProcessed >= 4096) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (totalProcessed > 0) {
            timeoutIdx = 0;
        } else if (pollRet > 0) {
            timeoutIdx = 0;
        } else {
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
    pollWakeupReadFd_ = -1;
    pollWakeupWriteFd_ = -1;
    int pipeFds[2];
    if (pipe(pipeFds) == 0) {
        pollWakeupReadFd_ = pipeFds[0];
        pollWakeupWriteFd_ = pipeFds[1];
        int fl = fcntl(pollWakeupReadFd_, F_GETFL, 0);
        if (fl >= 0) fcntl(pollWakeupReadFd_, F_SETFL, fl | O_NONBLOCK);
    } else {
        LOG_WARN("[eBPF-v86] startPollThread: pipe() failed (%s), wakeup may be delayed",
                 aatpeErrStr(errno));
    }

    pollThreadStop_.store(false, std::memory_order_release);
    setEventQueueEnabled(true);
#ifdef AATPE_HOST_TEST
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
        setEventQueueEnabled(false);
        pollThreadRunning_.store(false);
        if (pollWakeupReadFd_ >= 0) { close(pollWakeupReadFd_); pollWakeupReadFd_ = -1; }
        if (pollWakeupWriteFd_ >= 0) { close(pollWakeupWriteFd_); pollWakeupWriteFd_ = -1; }
        return false;
    }
}

void EbpfScheduler::stopPollThread() {
    if (!pollThreadRunning_.load()) return;
    pollThreadStop_.store(true, std::memory_order_release);
    if (pollWakeupWriteFd_ >= 0) {
        char one = 1;
        ssize_t w = write(pollWakeupWriteFd_, &one, 1);
        (void)w;
    }
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
    pollThreadRunning_.store(false);
    setEventQueueEnabled(false);
    if (pollWakeupReadFd_ >= 0) { close(pollWakeupReadFd_); pollWakeupReadFd_ = -1; }
    if (pollWakeupWriteFd_ >= 0) { close(pollWakeupWriteFd_); pollWakeupWriteFd_ = -1; }
    LOG_INFO("[eBPF-v86] stopPollThread: poll thread stopped");
}

bool EbpfScheduler::tryLoadEmbeddedSyscall() {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kLoadTimeoutMs);
    LOG_FORCE("=== tryLoadEmbeddedSyscall start ===");

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
    }

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
        state_ = aatpe::classifyEbpfFailure(errno, false, false);
        return false;
    }
    LOG_FORCE("embedded: minimal probe OK (BPF syscall available)");
    close(probeFd);

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

    if (loadTimeoutExceeded(deadline)) { state_ = aatpe::classifyEbpfFailure(EACCES, false, false); return false; }
    mapFds_.clear();
    for (int i = 0; i < kEmbeddedMapCount; i++) {
        const auto& m = kEmbeddedMaps[i];
        int fd = createMap(m.type, m.keySize, m.valueSize, m.maxEntries);
        if (fd < 0 && i == 2 && m.type == BPF_MAP_TYPE_LRU_HASH &&
            wakeupCacheMapDecision(fd, errno) == 1) {
            LOG_WARN("embedded: wakeup_cache LRU_HASH rejected errno=%d (%s); "
                     "falling back to plain HASH", errno, aatpeErrStr(errno));
            fd = createMap(BPF_MAP_TYPE_HASH, m.keySize, m.valueSize, m.maxEntries);
        }
        if (fd < 0) {
            LOG_WARN("embedded: map[%d] create failed type=%u errno=%d (%s)", i, m.type, errno, aatpeErrStr(errno));
            if (m.type == BPF_MAP_TYPE_PERCPU_ARRAY) {
                LOG_FORCE("embedded: PERCPU_ARRAY map[%d] unavailable — cannot safely fallback to ARRAY (would cause counter races). Degrading to polling.", i);
            }
            LOG_FORCE("embedded: map[%d] create FAILED type=%u errno=%d (%s) — all maps exhausted",
                      i, m.type, errno, aatpeErrStr(errno));
            for (int existing : mapFds_) close(existing);
            mapFds_.clear();
            state_ = aatpe::classifyEbpfFailure(errno, false, false);
            return false;
        }
        mapFds_.push_back(fd);
    }
    eventsMapFd_ = (mapFds_.size() > 0) ? mapFds_[0] : -1;
    countersMapFd_ = (mapFds_.size() > 1) ? mapFds_[1] : -1;
    threadCacheMapFd_ = -1;
    LOG_INFO("embedded: created %d maps (events=%d counters=%d wakeup_cache=%d)",
                (int)mapFds_.size(), eventsMapFd_, countersMapFd_,
                (mapFds_.size() > 2) ? mapFds_[2] : -1);

    if (loadTimeoutExceeded(deadline)) { state_ = aatpe::classifyEbpfFailure(EACCES, false, false); return false; }

    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpus <= 0) ncpus = 8;
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

        int desiredType = info.progType;
        int altType = (desiredType == BPF_PROG_TYPE_KPROBE) ?
                       BPF_PROG_TYPE_TRACEPOINT : BPF_PROG_TYPE_KPROBE;

        if (desiredType == BPF_PROG_TYPE_KPROBE && !kprobeTypeSupported) {
            LOG_INFO("embedded: prog[%d] KPROBE type known-unsupported, trying TRACEPOINT first", pi);
            desiredType = BPF_PROG_TYPE_TRACEPOINT;
            altType = BPF_PROG_TYPE_KPROBE;
        }

        LOG_INFO("embedded: prog[%d] section=%s func=%s is_ret=%d desired_type=%d alt_type=%d",
                 pi, info.sectionName, info.kprobeFunc, info.isKretprobe,
                 desiredType, altType);

        std::vector<int> refOffsets;
        std::vector<int> refMapIndices;
        refOffsets.reserve(info.refCount);
        refMapIndices.reserve(info.refCount);
        for (int ri = 0; ri < info.refCount; ri++) {
            refOffsets.push_back(info.refs[ri].insnOffset);
            refMapIndices.push_back(info.refs[ri].mapIndex);
        }

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

        int attached = 0;
        bool useKprobe = (loadedType == BPF_PROG_TYPE_KPROBE);
        bool useRawTp = (loadedType == BPF_PROG_TYPE_RAW_TRACEPOINT);

        if (useRawTp) {
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

            if (pi == 0) {
                setupPerCpuRingBuffers(0, -1, -1, (int)ncpus, pi);
            }
            attached = 1;
        } else if (useKprobe) {
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
            int tpId = readTracepointId(info.sectionName);
            if (tpId <= 0) {
                LOG_WARN("embedded: tracepoint id not found for %s errno=%d (%s)",
                         info.sectionName, errno, aatpeErrStr(errno));
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
                LOG_WARN("embedded: prog[%d] tracepoint attach FAILED on all CPUs (tpId=%d)",
                         pi, tpId);
                continue;
            }
            tracepointFds_.push_back(attachFd);
            if (pi == 0) {
                setupPerCpuRingBuffers(tpId, attachFd, attachCpu, (int)ncpus, pi);
            }
            attached = 1;
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
    if (firstProgType == BPF_PROG_TYPE_KPROBE) activeAttachMethod_ = "kprobe";
    else if (firstProgType == BPF_PROG_TYPE_RAW_TRACEPOINT) activeAttachMethod_ = "raw_tracepoint";
    else activeAttachMethod_ = "tracepoint";
    loaderType_ = EbpfLoaderType::EMBEDDED_SYSCALL;
    mode_ = EbpfMode::EVENT_DRIVEN;
    active_ = true;
    state_ = EbpfState::EVENT_DRIVEN;
    postLoadOffsetDegradeDecision();
    if (offsetDegraded_.load(std::memory_order_relaxed)) {
        unloaded_.store(false, std::memory_order_relaxed);
        LOG_WARN("eBPF: offset-degraded after attach (embedded); poll thread NOT "
                 "started (early-UNKNOWN event-drop window eliminated)");
        return true;
    }
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

    for (const auto& ring : rings_) {
        if (ring.base) {
            auto* hdr = reinterpret_cast<volatile struct perf_event_mmap_page*>(ring.base);
            LOG_INFO("[eBPF-DIAG] ring cpu=%d fd=%d base=%p head=%llu tail=%llu",
                     ring.cpu, ring.perfFd, ring.base,
                     (unsigned long long)hdr->data_head,
                     (unsigned long long)hdr->data_tail);
        }
    }

    if (countersMapFd_ >= 0) {
        std::vector<uint64_t> countsBefore;
        if (readEmbeddedCpuCounts(countsBefore)) {
            uint64_t beforeTotal = 0;
            for (uint64_t v : countsBefore) beforeTotal += v;

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
    unloaded_.store(false, std::memory_order_relaxed);
    return true;
}

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

    auto rd16 = [&](size_t off) -> uint16_t { uint16_t v; memcpy(&v, data.data()+off, 2); return v; };
    auto rd32 = [&](size_t off) -> uint32_t { uint32_t v; memcpy(&v, data.data()+off, 4); return v; };
    auto rd64 = [&](size_t off) -> uint64_t { uint64_t v; memcpy(&v, data.data()+off, 8); return v; };

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
    if (e_shstrndx == 0 || e_shstrndx >= e_shnum)
        return false;

    uint64_t shstrHdrOff = e_shoff + (uint64_t)e_shstrndx * e_shentsize;
    if (shstrHdrOff + 64 > size) return false;
    uint64_t shstrOff = rd64(shstrHdrOff + 24);
    uint64_t shstrSize = rd64(shstrHdrOff + 32);
    if (shstrOff + shstrSize > size) return false;
    const uint8_t* shstr = data.data() + shstrOff;

    int symtabIdx = -1, strtabIdx = -1;
    struct SectionInfo {
        std::string name;
        uint32_t type;
        uint64_t offset;
        uint64_t size;
        uint32_t link;
        uint32_t info;
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
        if (sh_name >= shstrSize) {
            si.name.clear();
            sections.push_back(si);
            continue;
        }
        size_t shRem = shstrSize - sh_name;
        size_t shLen = cstrLenWithin(shstr + sh_name, shRem);
        if (shLen == kNoNul) {
            LOG_WARN("parseElfPrograms: shstrtab name at offset %u not NUL-terminated within %zu bytes — reject corrupt .o",
                     (unsigned)sh_name, (size_t)shstrSize);
            return false;
        }
        si.name.assign(reinterpret_cast<const char*>(shstr + sh_name), shLen);
        sections.push_back(si);
        if (si.type == 2 ) symtabIdx = i;
        if (si.name == ".strtab") strtabIdx = i;
    }

    auto mapNameToIndex = [](const std::string& symName) -> int {
        if (symName == "events_map") return 0;
        if (symName == "event_counters") return 1;
        if (symName == "wakeup_cache") return 2;
        return -1;
    };

    std::vector<int> symMapIndex;
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
                uint32_t st_name = rd32(symOff);
                if (st_name < strtab.size) {
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

    for (int i = 0; i < (int)sections.size(); i++) {
        const SectionInfo& si = sections[i];
        bool isBpfProgSection = (si.name.rfind("tracepoint/", 0) == 0 ||
                                  si.name.rfind("kprobe/", 0) == 0 ||
                                  si.name.rfind("kretprobe/", 0) == 0);
        if (si.type == 1  && isBpfProgSection
            && si.size > 0 && si.offset + si.size <= size) {
            std::vector<uint8_t> prog(data.data() + si.offset,
                                       data.data() + si.offset + si.size);
            size_t pseudoRefCount = 0;
            for (size_t off = 0; off + 8 <= prog.size(); off += 8) {
                if (prog[off] == 0x18 && ((prog[off + 1] >> 4) & 0x0F) == 1) {
                    pseudoRefCount++;
                }
            }
            outPrograms.push_back(std::move(prog));
            outTpPaths.push_back(si.name);

            std::vector<int> mapIndices;
            std::string relSecName = ".rel" + si.name;
            for (int j = 0; j < (int)sections.size(); j++) {
                if (sections[j].name == relSecName &&
                    sections[j].type == 9  &&
                    sections[j].offset + sections[j].size <= size) {
                    const SectionInfo& rel = sections[j];
                    size_t relEntSize = rel.entsize ? rel.entsize : 16;
                    size_t numRels = rel.size / relEntSize;
                    for (size_t r = 0; r < numRels; r++) {
                        uint64_t relOff = rel.offset + r * relEntSize;
                        if (relOff + 16 > size) break;
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
            if (mapIndices.empty()) {
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

        mapFds_.clear();
        int fd;
        fd = createMap(BPF_MAP_TYPE_PERF_EVENT_ARRAY, 4, 4, 256);
        if (fd < 0) continue;
        mapFds_.push_back(fd);
        eventsMapFd_ = fd;

        fd = createMap(BPF_MAP_TYPE_PERCPU_ARRAY, 4, 8, 12);
        if (fd < 0) {

            LOG_FORCE("precompiled: PERCPU_ARRAY counters map create FAILED, cannot safely fallback");
            close(eventsMapFd_); mapFds_.clear(); continue;
        }
        mapFds_.push_back(fd);
        countersMapFd_ = fd;

        fd = createMap(BPF_MAP_TYPE_LRU_HASH, 4, 24, 8192);
        if (fd < 0 && wakeupCacheMapDecision(fd, errno) == 1) {
            LOG_WARN("syscall_precompiled: wakeup_cache LRU_HASH rejected errno=%d "
                     "(%s); falling back to plain HASH", errno, aatpeErrStr(errno));
            fd = createMap(BPF_MAP_TYPE_HASH, 4, 24, 8192);
        }
        if (fd < 0) { for (int f : mapFds_) close(f); mapFds_.clear(); continue; }
        mapFds_.push_back(fd);
        threadCacheMapFd_ = fd;

        fd = createMap(BPF_MAP_TYPE_PERCPU_ARRAY, 4, 8, 1);
        if (fd < 0) { for (int f : mapFds_) close(f); mapFds_.clear(); continue; }
        mapFds_.push_back(fd);

        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpus <= 0) ncpus = 8;
        onlineCpuIds_ = readOnlineCpuIds();
        int progsLoaded = 0;
        for (size_t i = 0; i < programs.size() && i < tpPaths.size(); i++) {
            if (loadTimeoutExceeded(deadline)) break;
            const std::vector<int>& mapIdx = (i < progMapIndices.size()) ?
                progMapIndices[i] : std::vector<int>();

            const std::string& secName = tpPaths[i];
            bool isKprobe = (secName.rfind("kprobe/", 0) == 0 ||
                             secName.rfind("kretprobe/", 0) == 0);
            bool isKretprobe = (secName.rfind("kretprobe/", 0) == 0);
            int progType = isKprobe ? BPF_PROG_TYPE_KPROBE : BPF_PROG_TYPE_TRACEPOINT;

            std::string funcName;
            if (isKretprobe) funcName = secName.substr(strlen("kretprobe/"));
            else if (isKprobe) funcName = secName.substr(strlen("kprobe/"));

            LOG_INFO("syscall_precompiled: prog[%zu] section=%s type=%d func=%s is_ret=%d",
                     i, secName.c_str(), progType, funcName.c_str(), isKretprobe);

            int progFd = loadBpfProgFromBytesWithRefs(programs[i].data(), programs[i].size(),
                                                       license.c_str(), mapFds_, mapIdx,
                                                       progType);
            if (progFd < 0) {
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
                attached = 1;
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
            postLoadOffsetDegradeDecision();
            if (offsetDegraded_.load(std::memory_order_relaxed)) {
                unloaded_.store(false, std::memory_order_relaxed);
                LOG_WARN("eBPF: offset-degraded after attach (syscall_precompiled); "
                         "poll thread NOT started");
                return true;
            }
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
            unloaded_.store(false, std::memory_order_relaxed);
            return true;
        }
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

static int runBpftoolArgv(const std::vector<std::string>& argv) {
    int status = -1;
    (void)aatpeExecArgv(argv, "bpftool", &status, true);
    if (status < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int runBpftoolLoadall(const std::string& bin, const std::string& obj,
                             const std::string& pinBase, const std::string& pinmapsDir,
                             const char* type) {
    std::vector<std::string> argv = { bin, "prog", "loadall", obj, pinBase };
    if (type) { argv.push_back("type"); argv.push_back(type); }
    if (!pinmapsDir.empty()) { argv.push_back("pinmaps"); argv.push_back(pinmapsDir); }
    return runBpftoolArgv(argv);
}

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
    std::string bpftoolBin = findBpftoolPath();
    if (bpftoolBin.empty()) {
        LOG_WARN("bpftool_precompiled: bpftool not available at known paths");
        return false;
    }

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

        std::string pinBase = "/sys/fs/bpf/aatpe_";
        std::string mapPinDir = "/sys/fs/bpf/aatpe_maps";
        for (size_t pi = 0; pi < (sizeof(kBpftoolAttachTable)/sizeof(kBpftoolAttachTable[0])); pi++)
            ::unlink((pinBase + kBpftoolAttachTable[pi].progName).c_str());
        for (int pi = 0; pi < (int)(sizeof(kBpftoolAttachTable)/sizeof(kBpftoolAttachTable[0])); pi++)
            ::unlink((pinBase + "prog_" + std::to_string(pi)).c_str());
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

        const std::string& obj = objPaths[i];

        int ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, mapPinDir, nullptr);
        LOG_INFO("bpftool: auto-detect loadall ret=%d", ret);

        if (ret != 0) {
            ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, mapPinDir, "kprobe");
            LOG_INFO("bpftool: type=kprobe loadall ret=%d", ret);
        }

        if (ret != 0) {
            ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, mapPinDir, "tracepoint");
            LOG_INFO("bpftool: type=tracepoint loadall ret=%d", ret);
        }

        if (ret != 0) {
            ret = runBpftoolLoadall(bpftoolBin, obj, pinBase, "", nullptr);
            LOG_INFO("bpftool: no-pinmaps loadall ret=%d", ret);
        }
        if (ret != 0) continue;

        const int kProgCount =
            (int)(sizeof(kBpftoolAttachTable)/sizeof(kBpftoolAttachTable[0]));

        std::string eventsMapPin = mapPinDir + "/events_map";
        eventsMapFd_ = open(eventsMapPin.c_str(), O_RDONLY | O_CLOEXEC);
        if (eventsMapFd_ >= 0) {
            LOG_INFO("bpftool: opened pinned events_map fd=%d", eventsMapFd_);
            mapFds_.push_back(eventsMapFd_);
            int cfd = open((mapPinDir + "/event_counters").c_str(), O_RDONLY | O_CLOEXEC);
            if (cfd >= 0) { countersMapFd_ = cfd; mapFds_.push_back(cfd); }

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
        onlineCpuIds_ = readOnlineCpuIds();
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
            postLoadOffsetDegradeDecision();
            if (offsetDegraded_.load(std::memory_order_relaxed)) {
                unloaded_.store(false, std::memory_order_relaxed);
                LOG_WARN("eBPF: offset-degraded after attach (bpftool_precompiled); "
                         "poll thread NOT started");
                return true;
            }
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
            unloaded_.store(false, std::memory_order_relaxed);
            return true;
        }
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
    }
    return false;
}

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

static int embeddedCpuBufferSlots(long possibleCpus) {
    if (possibleCpus < 1) possibleCpus = 1;
    return (int)(((possibleCpus + 7) / 8) * 8) + 8;
}

bool EbpfScheduler::readEmbeddedCpuCounts(std::vector<uint64_t>& outCounts) {
    outCounts.clear();
    if (!active_ || countersMapFd_ < 0) return false;
    int key = 0;
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = (uint32_t)countersMapFd_;
    attr.key = (uint64_t)(unsigned long)&key;
    long possibleCpus = 0;
    {
        FILE* pf = fopen("/sys/devices/system/cpu/possible", "re");
        if (pf) {
            char line[128] = {0};
            if (fgets(line, sizeof(line), pf)) {
                int maxIdx = -1;
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
    int roundedCpus = embeddedCpuBufferSlots(possibleCpus);
    size_t bufSize = (size_t)roundedCpus * sizeof(uint64_t);
    std::vector<uint8_t> buffer(bufSize, 0);
    attr.value = (uint64_t)(unsigned long)buffer.data();
    if (bpfSyscall(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr)) != 0) return false;
    uint64_t* counts = reinterpret_cast<uint64_t*>(buffer.data());
    if (!onlineCpuIds_.empty()) {
        for (int cpuId : onlineCpuIds_) {
            if (cpuId < 0 || cpuId >= roundedCpus) continue;
            outCounts.push_back(counts[cpuId]);
        }
    } else {
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

void EbpfScheduler::degrade(const char* reason) {
    fallbackCount_.fetch_add(1, std::memory_order_relaxed);
    degradeEventCount_.fetch_add(1, std::memory_order_relaxed);

    if (state_ == EbpfState::DEGRADED_RING) return;
    if (mode_ == EbpfMode::POLLING) return;

    LOG_WARN("eBPF DEGRADED: %s (mode -> HYBRID/POLLING)", reason);
#ifdef AATPE_HOST_TEST
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

void EbpfScheduler::logDegradeSummaryIfDue(void) {
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last = degradeSummaryLastMs_.load(std::memory_order_relaxed);
    if (nowMs - last < 30000) return;

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
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t rec = lastRecoverMs_.load(std::memory_order_relaxed);
    if (rec != 0 && (nowMs - rec) < 60000) return false;
    uint64_t total = eventCountTotal_.load();
    uint64_t lost = ringLost_.load();
    if (total > 1000 && lost > total / 10) return true;
    return false;
}

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
        return;
    }
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
    if (state_ != EbpfState::DEGRADED_RING) return;
    if (mode_ != EbpfMode::HYBRID) return;
    if (offsetDegraded_.load(std::memory_order_relaxed)) return;

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastLoss = lastRingLossMs_.load(std::memory_order_relaxed);
    if (lastLoss == 0) {
        lastRingLossMs_.store(nowMs, std::memory_order_relaxed);
        return;
    }
    if ((nowMs - lastLoss) < 30000) return;

    startPollThread();
    state_ = EbpfState::EVENT_DRIVEN;
    mode_ = EbpfMode::EVENT_DRIVEN;
    lastRecoverMs_.store(nowMs, std::memory_order_relaxed);
    LOG_WARN("eBPF recovered: ring buffer loss-free for 30s, mode -> EVENT_DRIVEN");
    aatpe::subsys::Registry::instance().setState("ebpf", aatpe::subsys::SubsystemState::ACTIVE);
    aatpe::subsys::Registry::instance().markRecovery("ebpf");
}

bool EbpfScheduler::onlineMaskSetChanged(const std::vector<int>& oldIds,
                                          const std::string& newText) {
    std::vector<int> newIds = parseCpuOnlineMask(newText);
    if (newIds.empty()) return false;
    return newIds != oldIds;
}

std::vector<int> EbpfScheduler::hotplugRebuildIfDue(void) {
    if (!active_) return {};
    if (mode_ != EbpfMode::EVENT_DRIVEN) return {};
    if (onlineCpuIds_.empty()) return {};

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t last = lastHotplugRebuildMs_.load(std::memory_order_relaxed);
    if (last != 0 && (nowMs - last) < kHotplugRebuildMinIntervalMs) return {};

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
        unload();
        bool ok = load();
        if (ok && state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN) {
            startPollThread();
            LOG_FORCE("eBPF hotplug: rebuild OK, now tracking %zu online cpus",
                      onlineCpuIds_.size());
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
        hotplugRebuildIfDue();
        return;
    }
    if (!hotplugRebuildIfDue().empty()) return;
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastAdj = lastRingAdjustMs_.load(std::memory_order_relaxed);
    if (lastAdj == 0) {
        lastRingAdjustMs_.store(nowMs, std::memory_order_relaxed);
        lostAtLastAdjust_.store(ringLost_.load(), std::memory_order_relaxed);
        eventsAtLastAdjust_.store(eventCountTotal_.load(), std::memory_order_relaxed);
        return;
    }
    if ((nowMs - lastAdj) < kRingAdjustIntervalMs) return;

    uint64_t totalLost = ringLost_.load(std::memory_order_relaxed);
    uint64_t totalEvents = eventCountTotal_.load(std::memory_order_relaxed);
    uint64_t lostDelta = totalLost - lostAtLastAdjust_.load(std::memory_order_relaxed);
    uint64_t eventsDelta = totalEvents - eventsAtLastAdjust_.load(std::memory_order_relaxed);
    double lossRate = (eventsDelta > 0) ? ((double)lostDelta / (double)eventsDelta) : 0.0;

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
        {
            std::lock_guard<std::mutex> lk(lifecycleMutex_);
            unload();
            ringPages_ = kRingPagesHigh;
            lowLossStreak_.store(0, std::memory_order_relaxed);
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

    bool healthy = (state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN);
    if (!healthy || eventsDelta < 100) {
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

void EbpfScheduler::retryLoadIfDue(void) {
    if (mode_ != EbpfMode::POLLING) return;
    if (state_ != EbpfState::DEGRADED_RING) return;
    if (!progFds_.empty()) return;
    if (loadPermanentFailed_) {
        uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t lastProbe = lastLoadAttemptMs_.load(std::memory_order_relaxed);
        if (nowMs - lastProbe < 600000ULL) return;
        loadPermanentFailed_ = false;
        loadFailureCount_.store(0);
    }
    if (loadFailureCount_.load() >= kBackoffMaxRetries) return;

    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t lastAttempt = lastLoadAttemptMs_.load(std::memory_order_relaxed);
    uint64_t nextDelay = nextRetryDelayMs_.load(std::memory_order_relaxed);

    if (lastAttempt == 0) {
        lastLoadAttemptMs_.store(nowMs, std::memory_order_relaxed);
        return;
    }
    if ((nowMs - lastAttempt) < nextDelay) return;

    LOG_INFO("eBPF retryLoad: attempt %d, retrying load after %lldms backoff",
             loadFailureCount_.load(), (unsigned long long)nextDelay);
    {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        unloaded_.store(false, std::memory_order_relaxed);
        unload();
        bool ok = load();
        lastLoadAttemptMs_.store(nowMs, std::memory_order_relaxed);

        if (ok && state_ == EbpfState::EVENT_DRIVEN && mode_ == EbpfMode::EVENT_DRIVEN) {
            LOG_FORCE("eBPF retryLoad: SUCCESS, now active via %s", getLoaderString());
            loadFailureCount_.store(0, std::memory_order_relaxed);
            nextRetryDelayMs_.store(0, std::memory_order_relaxed);
            startPollThread();
            return;
        }
    }

    int fails = loadFailureCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t delay;
    if (fails <= 3) {
        delay = kBackoffBaseMs << fails;
    } else {
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

bool EbpfScheduler::isPartiallyDegraded(void) const {
    if (state_ != EbpfState::EVENT_DRIVEN) return false;
    return (offsetStatus_switch_.load(std::memory_order_relaxed) == TpOffsetStatus::DEGRADED ||
            offsetStatus_fork_.load(std::memory_order_relaxed)   == TpOffsetStatus::DEGRADED ||
            offsetStatus_exec_.load(std::memory_order_relaxed)   == TpOffsetStatus::DEGRADED ||
            offsetStatus_exit_.load(std::memory_order_relaxed)  == TpOffsetStatus::DEGRADED);
}

const char* EbpfScheduler::getDetailedStateString(void) const {
    if (offsetDegraded_.load(std::memory_order_relaxed)) {
        return "DEGRADED_OFFSET";
    }
    if (offsetUnverifiedButTrusted_.load(std::memory_order_relaxed)) {
        return "TRUSTED_BUILTIN";
    }
    if (state_ == EbpfState::EVENT_DRIVEN && isPartiallyDegraded()) {
        return "ACTIVE_PARTIAL";
    }
    return getStateString();
}

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
    static const char* bases[] = {
        "/sys/kernel/debug/tracing/events/sched/",
        "/sys/kernel/tracing/events/sched/",
        nullptr
    };

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

    auto readOffsets = [](const char* eventDir) -> std::map<std::string, int> {
        std::map<std::string, int> out;
        for (int b = 0; bases[b]; ++b) {
            std::string path = std::string(bases[b]) + eventDir + "/format";
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string line;
            while (std::getline(f, line)) {
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

    {
        std::map<std::string, int> offs = readOffsets("sched_switch");
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
    {
        std::map<std::string, int> offs = readOffsets("sched_process_fork");
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
    offsetStatus_exec_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    offsetStatus_exit_.store(TpOffsetStatus::NO_CTX, std::memory_order_relaxed);
    detailAll += "sched_process_exec=ctx-offsets-not-used;";
    detailAll += "sched_process_exit=ctx-offsets-not-used;";

    {
        std::map<std::string, int> offs = readOffsets("sched_wakeup");
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

    std::string verifyResult;
    if (!anyReadable) {
        verifyResult = "unknown (debugfs/tracefs unavailable)";
        LOG_WARN("offset-verify: no tracepoint format readable; "
                 "ebpf_offset_verified=unknown (debugfs unavailable)");
    } else if (allMatch) {
        verifyResult = "true (" + detailAll + ")";
        LOG_FORCE("offset-verify: hardcoded 4.14 tracepoint offsets MATCH live format");
    } else {
        verifyResult = "false (" + detailAll + ")";
        LOG_WARN("offset-verify: MISMATCH vs live format (per-tracepoint degradation active). %s",
                 detailAll.c_str());
    }
    {
        std::lock_guard<std::mutex> lk(offsetVerifyMutex_);
        offsetVerifyResult_ = std::move(verifyResult);
    }

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

bool EbpfScheduler::load() {
    if (active_) return true;
    state_ = EbpfState::LOADING;
    unloaded_.store(false);
    LOG_FORCE("[eBPF] initializing...");
    LOG_FORCE("EbpfScheduler::load() starting auto-detection");
    LOG_FORCE("eBPF load strategy: KPROBE(embedded) -> TRACEPOINT(fallback) -> syscall_precompiled -> bpftool_precompiled -> POLLING");

    detectEnvironment();

    verifyTracepointOffsets();

    {
        LOG_INFO("embedded-size: self-validating %d programs:", kEmbeddedProgramCount);
        for (int pi = 0; pi < kEmbeddedProgramCount; pi++) {
            const auto& info = kEmbeddedPrograms[pi];
            LOG_INFO("embedded-size: [%d] %s = %d bytes (%d refs)",
                     pi, info.sectionName, info.bytecodeSize, info.refCount);
        }
    }

    if (!aatpe::integrity::privilegedOpsAllowed()) {
        LOG_ERROR("eBPF: integrity gate not OK; refusing privileged attach; "
                  "stay in polling fallback");
        loaderType_ = EbpfLoaderType::FALLBACK;
        mode_ = EbpfMode::POLLING;
        active_ = true;
        state_ = EbpfState::DEGRADED_RING;
        aatpe::subsys::Registry::instance().markError("ebpf",
            "integrity gate refused privileged attach; polling fallback");
        aatpe::subsys::Registry::instance().setState("ebpf",
            aatpe::subsys::SubsystemState::DEGRADED);
        aatpe::subsys::safemode::trigger(aatpe::subsys::safemode::EBPF_STREAK,
            "integrity gate refused eBPF attach");
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

    LOG_FORCE("--- Phase 1: tryLoadEmbeddedSyscall (KPROBE优先) ---");
    if (tryLoadEmbeddedSyscall()) {
        LOG_FORCE("LOAD OK via embedded_syscall (prog_type=%d, attach=%s)",
                 activeProgType_, activeAttachMethod_.c_str());
        aatpe::subsys::safemode::resetStreak(aatpe::subsys::safemode::EBPF_STREAK);
        return true;
    }
    LOG_WARN("embedded_syscall failed (KPROBE+TRACEPOINT both exhausted), trying syscall_precompiled");
    unload();

    LOG_FORCE("--- Phase 2: tryLoadSyscallPrecompiled ---");
    if (tryLoadSyscallPrecompiled()) {
        LOG_FORCE("LOAD OK via syscall_precompiled");
        aatpe::subsys::safemode::resetStreak(aatpe::subsys::safemode::EBPF_STREAK);
        return true;
    }
    LOG_WARN("syscall_precompiled failed, trying bpftool_precompiled");
    unload();

    LOG_FORCE("--- Phase 3: tryLoadBpftoolPrecompiled ---");
    if (tryLoadBpftoolPrecompiled()) {
        LOG_FORCE("LOAD OK via bpftool_precompiled");
        aatpe::subsys::safemode::resetStreak(aatpe::subsys::safemode::EBPF_STREAK);
        return true;
    }
    LOG_WARN("bpftool_precompiled failed, degrading to POLLING (partial attach torn down)");
    unload();

    LOG_FORCE("========================================");
    LOG_FORCE("ALL eBPF load paths FAILED — falling back to POLLING mode");
    LOG_FORCE("  - embedded_syscall (KPROBE+TRACEPOINT): FAILED");
    LOG_FORCE("  - syscall_precompiled: FAILED");
    LOG_FORCE("  - bpftool_precompiled: FAILED");
    LOG_FORCE("  - Degrading to pure polling (30s interval)");
    LOG_FORCE("========================================");
    loaderType_ = EbpfLoaderType::FALLBACK;
    mode_ = EbpfMode::POLLING;
    active_ = true;
    state_ = EbpfState::DEGRADED_RING;

    aatpe::subsys::Registry::instance().markError("ebpf", "all load paths failed; polling fallback");
    aatpe::subsys::Registry::instance().setState("ebpf", aatpe::subsys::SubsystemState::DEGRADED);
    aatpe::subsys::safemode::trigger(aatpe::subsys::safemode::EBPF_STREAK, "ebpf load failure");

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

bool EbpfScheduler::onConfigReload(const EbpfRuntimeConfig& cfg) {
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    if (cfg.pollFallback != pollFallbackEnabled_.load(std::memory_order_relaxed)) {
        setPollFallbackEnabled(cfg.pollFallback);
    }
    const bool ringPagesChanged = (cfg.ringPages != ringPages_);
    if (ringPagesChanged) {
        setRingPages(cfg.ringPages);
    }

    const bool currentlyActive = (active_.load() && !unloaded_.load(std::memory_order_relaxed));

    if (!cfg.enabled) {
        if (!currentlyActive) return false;
        LOG_WARN("eBPF config reload: enabled=0 -> safe unload (stop ring/agg, detach)");
        unload();
        active_ = false;
        state_ = EbpfState::UNLOADED;
        return true;
    }

    if (currentlyActive && !ringPagesChanged) return false;

    if (!unloaded_.load(std::memory_order_relaxed)) {
        unload();
    }
    LOG_FORCE("eBPF config reload: enabled=1 -> load (ringPages=%d)", cfg.ringPages);
    return load();
}

void EbpfScheduler::registerConfigReloadHook() {
    aatpe::config::registerReloadHook("ebpf", [this](const Config& c) {
        onConfigReload({c.ebpfEnabled, c.ebpfRingPages, c.ebpfPollFallback});
    });
}

void EbpfScheduler::unload() {
    if (unloaded_.load()) return;
    unloaded_.store(true);
    stopPollThread();
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

    for (int fd : rawTracepointLinks_) {
        if (fd >= 0) close(fd);
    }
    rawTracepointLinks_.clear();

    cleanupKprobeEvents();
    kprobeIdCache_.clear();

    for (int fd : progFds_) {
        if (fd >= 0) close(fd);
    }
    progFds_.clear();

    for (int fd : mapFds_) {
        if (fd >= 0) close(fd);
    }
    mapFds_.clear();

    eventsMapFd_ = -1;
    countersMapFd_ = -1;
    threadCacheMapFd_ = -1;
    onlineCpuIds_.clear();

    queueEnabled_ = false;
    criticalQueue_.clear();
    normalQueue_.clear();
    {
        std::lock_guard<std::mutex> lk(switchAggMutex_);
        switchAgg_.clear();
    }

    g_forkWarned.store(false, std::memory_order_relaxed);
    g_exitWarned.store(false, std::memory_order_relaxed);
    g_switchWarned.store(false, std::memory_order_relaxed);
    g_execWarned.store(false, std::memory_order_relaxed);

}

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

EbpfScheduler::EbpfScheduler() {}
EbpfScheduler::~EbpfScheduler() { unload(); }

}

// D3 host test: CPU-hotplug online-mask -> eBPF ring rebuild DECISION core.
//
// The actual unload()+load() rebuild touches real BPF/perf-event fds and cannot
// run on x86_64 host. What IS host-testable is the pure decision:
//   * parseCpuOnlineMask("0-7")        -> {0,1,2,3,4,5,6,7}
//   * parseCpuOnlineMask("0,1,4,5")   -> {0,1,4,5}
//   * onlineMaskSetChanged(old, new)   -> true iff the parsed sets differ.
// This covers the required "0-7 -> 0,1,4,5 shrink" transition (a core offline at
// load time whose ring would otherwise be stale) plus the no-change and garbage
// cases. The guarded rebuild itself is exercised on device (see report).
#include "aatpe_ebpf.h"

#include <cstdio>
#include <vector>
#include <algorithm>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  [PASS] %s\n", msg); } \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

static bool eqSet(const std::vector<int>& a, std::vector<int> b) {
    std::sort(b.begin(), b.end());
    std::vector<int> aa = a; std::sort(aa.begin(), aa.end());
    return aa == b;
}

int main() {
    printf("==> D3 eBPF hotplug: online-mask decision core ==\n");
    using namespace aatpe_ebpf;

    // 1. parseCpuOnlineMask basics.
    auto m8   = parseCpuOnlineMask("0-7");
    CHECK(eqSet(m8, {0,1,2,3,4,5,6,7}), "D3: parse '0-7' -> 8 cpus");
    auto m45  = parseCpuOnlineMask("0,1,4,5");
    CHECK(eqSet(m45, {0,1,4,5}), "D3: parse '0,1,4,5' -> {0,1,4,5}");
    auto m13  = parseCpuOnlineMask("3");
    CHECK(eqSet(m13, {3}), "D3: parse '3' -> {3}");

    // 2. the required transition: loaded mask 0-7, runtime online becomes 0,1,4,5.
    bool changed = EbpfScheduler::onlineMaskSetChanged(m8, "0,1,4,5");
    CHECK(changed, "D3: mask 0-7 -> 0,1,4,5 triggers rebuild (8->4 cpus)");

    // 3. no-change: same mask -> no rebuild.
    bool same = EbpfScheduler::onlineMaskSetChanged(m8, "0-7");
    CHECK(!same, "D3: mask 0-7 -> 0-7 no rebuild");

    // 4. newly online core: 0-7 -> 0-7,6 is malformed; instead test 0-3 -> 0-7 (grow).
    bool grew = EbpfScheduler::onlineMaskSetChanged(parseCpuOnlineMask("0-3"), "0-7");
    CHECK(grew, "D3: mask 0-3 -> 0-7 triggers rebuild (4->8 cpus, core online)");

    // 5. garbage / unreadable mask -> no action (never rebuild on bad input).
    bool garbage = EbpfScheduler::onlineMaskSetChanged(m8, "nonsense-zzz");
    CHECK(!garbage, "D3: garbage mask string -> no rebuild (safe)");
    bool empty = EbpfScheduler::onlineMaskSetChanged(m8, "");
    CHECK(!empty, "D3: empty mask string -> no rebuild (safe)");

    // 6. reordered but equal set is NOT a change (set equality, not string eq).
    bool reordered = EbpfScheduler::onlineMaskSetChanged(m45, "5,4,1,0");
    CHECK(!reordered, "D3: '0,1,4,5' vs '5,4,1,0' same set -> no rebuild");

    printf("==> D3 eBPF hotplug: done\n");
    if (failures == 0) { printf("D3 hotplug: ALL PASS\n"); return 0; }
    printf("D3 hotplug: %d FAILURES\n", failures);
    return 1;
}

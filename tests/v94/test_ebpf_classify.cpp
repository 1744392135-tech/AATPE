// ROUND-014 P3: host tests for eBPF seven-state classification.
#include <cassert>
#include <cstdio>
#include <cerrno>
#include "aatpe_ebpf.h"

// classifyEbpfFailure is in namespace aatpe (defined in aatpe_ebpf.cpp)
namespace aatpe {
aatpe_ebpf::EbpfState classifyEbpfFailure(int err, bool progLoaded, bool ringReadable);
}

int main() {
    using S = aatpe_ebpf::EbpfState;
    // EACCES/EPERM -> denied_selinux
    assert(aatpe::classifyEbpfFailure(EACCES, false, false) == S::DENIED_SELINUX);
    assert(aatpe::classifyEbpfFailure(EPERM, false, false) == S::DENIED_SELINUX);
    // ENOSYS/EOPNOTSUPP -> kernel_unsupported
    assert(aatpe::classifyEbpfFailure(ENOSYS, false, false) == S::KERNEL_UNSUPPORTED);
    assert(aatpe::classifyEbpfFailure(EOPNOTSUPP, false, false) == S::KERNEL_UNSUPPORTED);
    // prog loaded but ring not readable -> degraded_ring
    assert(aatpe::classifyEbpfFailure(0, true, false) == S::DEGRADED_RING);
    // all good -> event_driven
    assert(aatpe::classifyEbpfFailure(0, true, true) == S::EVENT_DRIVEN);
    // unknown errno -> polling
    assert(aatpe::classifyEbpfFailure(EBADF, false, false) == S::POLLING);
    printf("test_ebpf_classify: all 7 assertions passed\n");
    return 0;
}

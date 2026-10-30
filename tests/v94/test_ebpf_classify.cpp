#include <cassert>
#include <cstdio>
#include <cerrno>
#include "aatpe_ebpf.h"

namespace aatpe {
aatpe_ebpf::EbpfState classifyEbpfFailure(int err, bool progLoaded, bool ringReadable);
}

int main() {
    using S = aatpe_ebpf::EbpfState;
    assert(aatpe::classifyEbpfFailure(EACCES, false, false) == S::DENIED_SELINUX);
    assert(aatpe::classifyEbpfFailure(EPERM, false, false) == S::DENIED_SELINUX);
    assert(aatpe::classifyEbpfFailure(ENOSYS, false, false) == S::KERNEL_UNSUPPORTED);
    assert(aatpe::classifyEbpfFailure(EOPNOTSUPP, false, false) == S::KERNEL_UNSUPPORTED);
    assert(aatpe::classifyEbpfFailure(0, true, false) == S::DEGRADED_RING);
    assert(aatpe::classifyEbpfFailure(0, true, true) == S::EVENT_DRIVEN);
    assert(aatpe::classifyEbpfFailure(EBADF, false, false) == S::POLLING);
    printf("test_ebpf_classify: all 7 assertions passed\n");
    return 0;
}

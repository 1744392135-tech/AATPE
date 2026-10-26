# AATPE eBPF: scheduler tracepoint event monitor

This directory holds the BPF program that feeds scheduler events
(fork / exit / sched_switch / exec / wakeup) from kernel space into the
`aatped` daemon.

## Files

| File                          | Purpose                                                  |
|-------------------------------|----------------------------------------------------------|
| `sched_events.bpf.c`          | BPF C source (tracepoint programs, fully self-contained) |
| `sched_events.bpf.o`          | Prebuilt BPF object (committed fallback)                 |
| `sched_events_embedded.h`     | Generated C array `aatpe_ebpf.cpp` `#include`s          |
| `gen_embedded_header.py`      | `.o` -> embedded header generator (self-checks 4.14 ABI) |
| `sched_switch.bpf.c`          | Historical / reference variant (not built by default)    |

## Build chain (P2-29/P2-30/P2-35)

    ebpf/sched_events.bpf.c
        -- clang -target bpf -D__TARGET_ARCH_aarch64 -O2 -g -c -->
    ebpf/sched_events.bpf.o
        -- python3 gen_embedded_header.py -->
    ebpf/sched_events_embedded.h
        -- #include "ebpf/sched_events_embedded.h" (aatpe_ebpf.cpp) -->
    aatped

The Makefile target `ebpf-prepare` (order-only prereq of `aatped`) runs this
chain automatically when the NDK clang supports `-target bpf`; `make
ebpf-regen` is the explicit alias. On a host without BPF clang the build falls
back to the committed `sched_events.bpf.o` + `sched_events_embedded.h`.

Compile command (identical flags to the Makefile):

    $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang \
        -target bpf -D__TARGET_ARCH_aarch64 -O2 -g -Wall \
        -Wno-unused-variable -Wno-unused-value -Wno-pointer-sign \
        -Wno-unused-function \
        -c sched_events.bpf.c -o sched_events.bpf.o
    python3 gen_embedded_header.py sched_events.bpf.o sched_events_embedded.h

## Kernel ABI constraint (4.14, e.g. Dimensity 1100 / kernel 4.14.186)

The bytecode MUST stay compatible with the 4.14 verifier:

- NO `BPF_JMP32` (class=6) — requires kernel >= 4.20.
- NO `BPF_ATOMIC` — requires kernel >= 5.1 (counters use PERCPU_ARRAY).
- NO `ringbuf` (`BPF_MAP_TYPE_RINGBUF`) — requires kernel >= 5.8; we use
  `BPF_MAP_TYPE_PERF_EVENT_ARRAY` + `bpf_perf_event_output` with
  `BPF_F_CURRENT_CPU`.
- NO CO-RE / BPF skeleton relocations — map FDs are patched at explicit
  relocation offsets by the user-space loader.

`gen_embedded_header.py` scans every program and prints
"no BPF_ATOMIC / BPF_JMP32 instructions (OK)"; the embedded header is only
written when all programs pass.

# AATPE Build Guide

## 0. 可复现构建速查（#34，v4.2.x）
精确复现发布四二进制：
- NDK **r30.0.16248370**，路径 `~/.local/android-ndk-r30`（工具链 `toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++`）。
- 构建：`export NDK=$HOME/.local/android-ndk-r30 SOURCE_DATE_EPOCH=1767225600 && make NDK=$NDK SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH -j1 all`
- strip（发布二进制）：`llvm-strip --strip-all --remove-section=.comment --remove-section=.note.android.ident`（`make install` 已自动执行）。
- eBPF 由 `ebpf/sched_events.bpf.c` + `gen_embedded_header.py` 确定性再生，不依赖手工产物。
- 校验：`sha256sum _install/system/bin/{aatped,aatpe-cli,aatpe-watchdog,aatpe-probe}`；三份 html `md5sum webui/index.html webroot/index.html system/etc/aatpe/webui/index.html`（`bash check-html-sync.sh` 去重=1）。
- 测试入口：v94 `bash tests/v94/build.sh`；集成 `bash tests/integration/build.sh "$PWD" "$PWD/tests/integration" && bash tests/integration/run_integration.sh "$PWD" "$PWD/tests/integration/aatpe_integration_test"`；QA `bash tests/qa/run_all_qa.sh`。

This document records the exact, reproducible build environment for AATPE
`v6.0.0` (versionCode 4411, RELEASE_ID=rel11, BUILD_DATE=2026-10-23).

## 1. Toolchain

| Item            | Value                                              |
|-----------------|----------------------------------------------------|
| NDK             | r30                                                |
| Clang           | 21.0.0 (NDK prebuilt, +pgo +bolt +lto)             |
| Target ABI      | aarch64 (ARM64)                                    |
| Android API     | android-29 (minSdk = 29)                           |
| Linker          | lld (default `clang++` driver)                     |
| C++ standard    | C++17                                              |
| Strip tool      | `$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip` |

The NDK location is configurable via the `NDK` variable or `ANDROID_NDK_HOME`
env var (see `make version`). Default: `/opt/android-ndk-r30`.

> **B-002 toolchain version note.** The table above is the *minimum required*
> toolchain (NDK r30 / Clang 21). Release builds are produced with whatever NDK
> point release is installed on the build host, which may be newer (e.g. NDK
> r30.x shipping Clang 22 object format). Such a build legitimately contains
> Clang-22-compiled objects even though this guide documents Clang 21 — this is
> an expected point-release drift, not a misbuild. The ABI, flags, and output are
> unchanged; only the embedded compiler version string differs. Always build with
> **NDK r30 or newer** (`aarch64-linux-android29-clang++`); older NDKs are not
> supported.

## 2. Compiler / linker flags

```
CFLAGS  = -O3 -flto=thin -march=armv8.2-a \
          -std=c++17 -Wall -Wextra -Werror -fPIE \
          -fstack-protector-strong -D_FORTIFY_SOURCE=2 -MMD -MP \
          $(REPRO_FLAGS) -ffile-prefix-map=$(CURDIR)=.
LDFLAGS = -static-libstdc++ -flto=thin -fPIE -pie \
          -Wl,-z,relro,-z,now -llog -pthread
```

`REPRO_FLAGS` (see Makefile §"Reproducible build") injects `-D__DATE__` /
`-D__TIME__` derived from `SOURCE_DATE_EPOCH` when it is non-zero.

eBPF programs are compiled from `ebpf/sched_events.bpf.c` with the NDK clang:
`clang --target=bpf -D__TARGET_ARCH_aarch64 -O2 -g -c sched_events.bpf.c -o
sched_events.bpf.o`, then `ebpf/gen_embedded_header.py` converts the `.o` into
`ebpf/sched_events_embedded.h` (a C array included directly by
`aatpe_ebpf.cpp`). See section 6 for the full reproducibility contract.

## 6. eBPF build reproducibility (P2-29/P2-30/P2-35)

**Decision: the committed `ebpf/sched_events.bpf.o` and
`ebpf/sched_events_embedded.h` are kept as a zero-dependency fallback, BUT every
clean build on a BPF-capable host regenerates them from `sched_events.bpf.c`.**

- **Toolchain:** the NDK r30 clang (`$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/clang`,
  Clang 21.0.0) supports `-target bpf`. The Makefile probes it with
  `clang -target bpf -x c -c /dev/null -o /dev/null` (the `-c` is required —
  probing without `-c` links an empty TU and always fails, which is the
  original P2-29 root cause).
- **Build chain (source → object → embedded header → aatped):**
  `ebpf/sched_events.bpf.c` → `ebpf/sched_events.bpf.o` →
  `ebpf/sched_events_embedded.h` → `#include` in `aatpe_ebpf.cpp` → `aatped`.
  The order-only target `ebpf-prepare` runs this automatically as a prereq of
  `aatped`; `make ebpf-regen` is the explicit alias.
- **Kernel ABI (4.14 constraint):** the bytecode MUST avoid `BPF_JMP32`
  (class=6, needs kernel ≥4.20), `BPF_ATOMIC` (needs ≥5.1), `ringbuf`
  (needs ≥5.8) and CO-RE relocations (needs BPF skeleton/BTF-on-host). The
  program uses only `PERCPU_ARRAY` counters + `bpf_perf_event_output` with
  `BPF_F_CURRENT_CPU`. `gen_embedded_header.py` self-checks every program for
  `BPF_JMP32`/`BPF_ATOMIC` and aborts the generation on violation.
- **Reproducibility verified on NDK r30:** compiling the `.c` produces a `.o`
  whose tracepoint `PROGBITS` program bytes are identical to the committed
  prebuilt, and the regenerated `sched_events_embedded.h` is byte-identical to
  the committed header (only DWARF/BTF debug bytes inside the `.o` differ, and
  those are NOT embedded into the loader header).
- **Host without BPF clang:** `ebpf-prepare` falls back to the committed
  `sched_events.bpf.o` + `sched_events_embedded.h` and prints a NOTE; the build
  still succeeds (zero-dependency loader). It errors out only when both the
  toolchain AND the committed artifacts are missing.
- **Source ZIP:** `build_release.sh` packages `sched_events.bpf.c`,
  `sched_events.bpf.o`, `sched_events_embedded.h` and `gen_embedded_header.py`
  under `system/etc/aatpe/ebpf/` so the chain is complete on-device and the
  source archive can rebuild the BPF object from source.

## 3. Build

```sh
export NDK=/path/to/android-ndk-r30     # or: export ANDROID_NDK_HOME=...
make version                            # print resolved build environment
make                                    # build all 4 binaries (unstripped)
make strip                              # strip release binaries in-place
                                        # (unstripped kept as <bin>.debug)
make install DESTDIR=/tmp/aatpe-out     # install + strip into a module tree
make checksum                           # SHA256 of all release binaries
```

Binaries produced: `aatped`, `aatpe-cli`, `aatpe-watchdog`, `aatpe-probe`.

## 4. Reproducible build support (P32)

The Makefile applies basic reproducibility measures:

- **Path normalization:** `-ffile-prefix-map=$(CURDIR)=.` rewrites absolute
  source paths embedded in DWARF/debug info and `__FILE__` to a stable `.`,
  so two checkouts at different absolute paths build identically.
- **Build order:** targets are listed explicitly; `make` resolves the order.
- **`SOURCE_DATE_EPOCH`:** honored as a Make variable (defaults to `0`).
  Pass a fixed value to document a deterministic build timestamp:
  `make SOURCE_DATE_EPOCH=1700000000`.

### Build timestamp handling

`aatpe-cli version` prints `__DATE__` / `__TIME__` for diagnostics. When
`SOURCE_DATE_EPOCH` is set to a non-zero value, the Makefile derives a UTC
`"Mmm dd yyyy" HH:MM:SS"` from it and injects it as `-D__DATE__=...
-D__TIME__=...`, so these become deterministic for that build. When
`SOURCE_DATE_EPOCH=0` (the default, for iterative local builds), the compiler
uses the real wall-clock build time. The on-device version string
(`AATPE_VERSION`) is always a constant regardless.

### Verifying reproducibility

```sh
make clean && make checksum > checksum.1
make clean && make checksum > checksum.2
diff checksum.1 checksum.2     # should be empty when SOURCE_DATE_EPOCH is set
```
```

## 5. eBPF tracepoint offset verification (问题43/44/45)

The BPF programs read the tracepoint context at **hardcoded byte offsets** based on
the Linux 4.14 generic `TP_STRUCT__entry` layout (see `ebpf/sched_events.bpf.c`
header and `docs/SOURCE_OF_TRUTH.md`). These are correct on the reference target
(Dimensity 1100 / kernel 4.14.186) but are NOT a universal assumption.

- **Runtime check (implemented):** `aatpe_ebpf.cpp::verifyTracepointOffsets()`
  reads `/sys/kernel/[debug/]tracing/events/sched/{sched_switch,
  sched_process_fork}/format` at load and compares the live `offset:` against the
  hardcoded values. It only WARNs on mismatch (the 4.14 layout should be stable) and
  degrades to `unknown` if debugfs/tracefs is unmounted. The daemon publishes the
  verdict to `<state_dir>/ebpf_offset_verified`.
- **Build-time doc check:** expected offsets are recorded as comments in
  `ebpf/sched_events_embedded.h` and emitted by `ebpf/gen_embedded_header.py`:
  `prev_comm@8 prev_pid@24 next_comm@40 next_pid@56 next_prio@60` (sched_switch),
  `child_pid@32 child_tgid@36` (fork).
- **On-device verification (when porting to a new kernel):**
  ```sh
  cat /sys/kernel/debug/tracing/events/sched/sched_switch/format
  # field:char prev_comm[16]; offset:8  / field:pid_t next_pid; offset:56  ...
  cat /dev/aatpe/state/ebpf_offset_verified    # expect: true (...)
  ```
  If the `offset:` values differ from the table above, the hardcoded offsets in
  `sched_events.bpf.c` MUST be recomputed for that kernel before shipping.

See also `docs/SELINUX.md` (eBPF load under SELinux) and `docs/ARCHITECTURE.md`.


## Reproducible build & BUILD_MANIFEST (ROUND-001)

- **Version single source of truth:** `VERSION` → `aatpe-version.h`
  (`AATPE_VERSION` / `AATPE_VERSION_CODE`). `module.prop`, `customize.sh`/`service.sh`
  banners and the WebUI footer mirror these; `check-source-consistency.sh` enforces
  the match on every build.
- **No build timestamp baked into binaries:** `aatpe-cli version` no longer prints
  `__DATE__`/`__TIME__` (that was the main per-build drift). `SOURCE_DATE_EPOCH`
  remains supported: set it to a nonzero epoch to pin any remaining date macros.
- **thinLTO is NOT bit-for-bit reproducible across machines/builds.** Therefore the
  release flow does *not* expect a checked-in `BUILD_MANIFEST.txt` to match an
  arbitrary rebuild. Instead:
  ```sh
  make clean && make all          # source gate passes on a clean tree
  make install                    # strips binaries, mirrors to ./system/, then:
                                  #   1) `make release-manifest` recomputes
                                  #      BUILD_MANIFEST.txt from THIS build's stripped
                                  #      system/bin/* bytes (automatic, not hand-edited)
                                  #   2) check-release-consistency.sh re-verifies
                                  #      system/bin sha256 == manifest
  ```
  So `make install` always ends with a self-consistent manifest and a green release
  gate. Treat `BUILD_MANIFEST.txt` as an output of the build, not an input.

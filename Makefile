# Reproducible Android arm64 build with NDK r30.
# NDK location overridable via `make NDK=/path` or ANDROID_NDK_HOME.
NDK ?= $(if $(ANDROID_NDK_HOME),$(ANDROID_NDK_HOME),/opt/android-ndk-r30)

TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
AATPE_CC  := $(TOOLCHAIN)/bin/aarch64-linux-android29-clang++
# P28: NDK llvm-strip (separate binutils not needed with the LLVM toolchain).
AATPE_STRIP := $(TOOLCHAIN)/bin/llvm-strip

# P32: Reproducible-build support.
#   -fdebug-prefix-map rewrites absolute build paths to a stable placeholder so
#   two builds from different checkout locations produce identical binaries.
#   SOURCE_DATE_EPOCH, when set to a NONZERO epoch second, fixes __DATE__/__TIME__:
#   we derive a fixed "Mmm dd yyyy" / "HH:MM:SS" (UTC) from it and inject them as
#   -D macros so the compiler's built-in __DATE__/__TIME__ are overridden with a
#   deterministic value instead of the wall-clock at build time. The default of 0
#   means "not pinned" and leaves __DATE__/__TIME__ alone (a normal, non-repro
#   build). This replaces the old claim that SOURCE_DATE_EPOCH was honoured while
#   REPRO_FLAGS actually only mapped paths.
REPRO_FLAGS := -ffile-prefix-map=$(CURDIR)=.
ifeq ($(SOURCE_DATE_EPOCH),)
  SOURCE_DATE_EPOCH := 0
endif
ifneq ($(SOURCE_DATE_EPOCH),0)
  # __DATE__ renders as "Mmm dd yyyy" with a SPACE-padded two-digit day, exactly
  # like the compiler built-in (e.g. "Aug  3 2026"). %b = locale-month, %e = space-
  # padded day, %Y = 4-digit year; %T = HH:MM:SS. GNU date (host) provides these.
  BUILD_DATE := $(shell date -u -d @$(SOURCE_DATE_EPOCH) +'%b %e %Y')
  BUILD_TIME := $(shell date -u -d @$(SOURCE_DATE_EPOCH) +'%T')
  # Pinning __DATE__/__TIME__ on the command line redefines clang builtins; under
  # -Werror that is -Wbuiltin-macro-redefined. This is the documented, canonical
  # reproducible-build way to pin them — suppress only that one warning (does NOT
  # relax any other -Wall/-Wextra check).
  REPRO_FLAGS += -Wno-builtin-macro-redefined -D__DATE__="\"$(BUILD_DATE)\"" -D__TIME__="\"$(BUILD_TIME)\""
endif

# Problem #68: upgrade from -std=c++11 to -std=c++17.
# Rationale:
#   - NDK r30 ships Clang 21, which fully supports C++17 (and C++20).
#   - Several production sources (aatpe-watchdog.cpp, aatpe-probe.cpp) and the
#     whole tests/ tree already require -std=c++17 per their own comments.
#   - grep audit of production sources found NO use of C++17-removed/removed-in-
#     C++17 constructs (std::auto_ptr, std::binary_function, std::unary_function,
#     bind1st/bind2nd, register keyword) and no use of C++17-only features, so
#     the upgrade is a pure, safe superset.
# P0-opt5: -O3 + ThinLTO for hot-path inlining and cross-TU optimization.
# v6.0.0: generic armv8.2-a scheduling. The old -mtune=cortex-a78 pinned the
# codegen to one SoC's big core; dropping it lets clang schedule generically
# for -march=armv8.2-a (all arm64 Android 10+ targets). This is a portability
# enhancement, not a regression.
# P1-thermzone: -I shared exposes shared/cpu_zone_selector.h to BOTH thermal.cpp
# (linked into aatped) and aatpe-watchdog.cpp, so the two binaries derive the trusted
# CPU temperature from the SAME pure header (W13/W14 red-team approved).
CFLAGS := -O3 -flto=thin -march=armv8.2-a -std=c++17 -Wall -Wextra -Werror -Wno-error=deprecated -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2 -MMD -MP -I shared -I mech/include $(REPRO_FLAGS)

# Problem #66: link against pthread explicitly.
#   -pthread is REQUIRED: production sources use std::thread / std::mutex /
#   std::atomic / std::lock_guard / std::condition_variable / std::this_thread
#   heavily (590 occurrences across 28 production files). Although NDK clang++
#   usually links pthread implicitly for libc++, the explicit flag is the
#   portable, canonical way to request the threading runtime.
#
#   -ldl is intentionally NOT added: grep over the whole tree found ZERO uses
#   of dlopen / dlsym / dlclose / dladdr. eBPF loading goes through the raw
#   bpf(BPF_PROG_LOAD) syscall (see aatpe_ebpf.cpp:bpfSyscall), not libdl.
#   Adding -ldl here would be dead weight; it can be re-added later if any
#   dlopen-based plugin loading is introduced.
# ROUND-010: -z max-page-size=16384 guarantees 16 KB page-size device support
# (Android 15+ 16 KB kernels; Play requirement). NDK r28+ defaults to this but
# pin it explicitly so older NDK builds stay compatible.
# --as-needed: only binaries that actually reference zlib symbols (aatped,
# aatpe-cli via aatped_apklabel) get a libz DT_NEEDED; aatpe-watchdog /
# aatpe-probe (which never compile aatped_apklabel.cpp) do not.
LDFLAGS := -flto=thin -static-libstdc++ -fPIE -pie -Wl,-z,max-page-size=16384 -Wl,-z,relro,-z,now -llog -pthread -Wl,--as-needed -lz

TARGETS := aatped aatpe-cli aatpe-watchdog aatpe-probe

# ---------------------------------------------------------------------------
# v94 fix / 问题47: eBPF build dependency (single source of truth).
#   aatpe_ebpf.cpp does NOT compile a .c directly; it #includes the PREBUILT
#   bytecode blob  ebpf/sched_events_embedded.h . That header is GENERATED by
#   ebpf/gen_embedded_header.py from  ebpf/sched_events.bpf.o, which is itself
#   compiled from  ebpf/sched_events.bpf.c .
#
#   Dependency chain (DO NOT reorder / do not skip a step):
#     ebpf/sched_events.bpf.c  --clang --target bpf-->  ebpf/sched_events.bpf.o
#       --python3 gen_embedded_header.py-->  ebpf/sched_events_embedded.h
#       --cp-->  system/etc/aatpe/ebpf/  (device mirror, build-time sync only)
#       --#include aatpe_ebpf.cpp-->  links into aatped
#
# => CHOSEN MODE (round-2, P0-3): the pre-generated
#      ebpf/sched_events_embedded.h is COMMITTED and is the deterministic source
#      of truth aatpe_ebpf.cpp #includes. You do NOT need a BPF-capable clang to
#      build aatped. The committed ebpf/sched_events.bpf.o is also checked in so
#      the chain is complete out of the box.
#
#   => IF you edit ebpf/sched_events.bpf.c, regenerate explicitly with
#      `make ebpf-regen` on a host that has the NDK clang (it is the only host
#      with -target bpf). The build does NOT force this: if sched_events.bpf.c is
#      newer than the committed .h, `make` prints a STALENESS warning but still
#      links the committed header (so clean builds never fail on hosts without
#      BPF clang). See docs/SOURCE_OF_TRUTH.md.
# ---------------------------------------------------------------------------
AATPED_SRCS := aatped_main.cpp aatped_utils.cpp aatped_logging.cpp aatped_config.cpp aatped_topology.cpp aatped_state.cpp aatped_control.cpp aatped_hmac.cpp aatped_gamescene.cpp aatped_battery.cpp aatped_gpu.cpp aatped_audio.cpp aatped_applaunch.cpp aatped_uevent.cpp aatped_abtest.cpp aatped_ebpf_agg.cpp scoring.cpp event_monitor.cpp uevent_monitor.cpp input_monitor.cpp touch_uclamp.c touch_uclamp_act.cpp touch_uclamp_fgscan.cpp cold_start_rules.cpp gpu_boost.cpp thermal.cpp frame.cpp thread_scan.cpp placement.cpp webui_json.cpp webui_http.cpp aatpe_ml.cpp aatpe_ml_hybrid_main.cpp aatpe_ml_hybrid_train.cpp aatpe_ml_hybrid_predict.cpp aatpe_ml_hybrid_persist.cpp aatpe_ml_hy_alpha.cpp aatpe_federated_main.cpp aatpe_federated_aggregate.cpp aatpe_ebpf.cpp aatpe_observability.cpp frame_binder.cpp adrc.cpp integrity.cpp aatped_priority.cpp aatpe_state_ttl.cpp aatpe_helper_audit.cpp aatpe_managers.cpp aatpe_config_schema.cpp aatpe_command_dispatcher.cpp aatpe_session_token.cpp aatpe_recovery.cpp aatpe_command_queue.cpp aatpe_subsystem.cpp aatpe_placement_mech.cpp placement_heal_decision.cpp pre_boost_predict.cpp thermal_headroom.cpp scene_infer.cpp c2_envelope.cpp scene_infer_wiring.cpp c4_freezer.cpp aatpe_diag.cpp aatpe_caps.cpp aatped_threadutil.cpp aatped_apklabel.cpp psi_decision.c aatped_psi.cpp thermal_ff/thermal_ff.c mech/src/game_scene.c mech/src/device_profile.c mech/src/load_forecast.c mech/src/context.c aatped_arbitrator.cpp
AATPED_HDRS := aatped.h aatped_types.h aatped_utils.h aatped_logging.h aatped_config.h aatped_topology.h aatped_state.h aatped_control.h aatped_hmac.h aatped_gamescene.h aatped_battery.h aatped_gpu.h aatped_audio.h aatped_applaunch.h aatped_uevent.h aatped_abtest.h aatped_ebpf_agg.h aatpe_ml.h aatpe-constants.h aatpe-version.h webui_http.h aatpe_ebpf.h aatpe_ml_hybrid.h aatpe_federated.h aatpe_observability.h scoring_interface.h aatpe_crypto.h integrity.h aatped_priority.h aatpe_state_ttl.h aatpe_helper_audit.h aatpe_managers.h aatpe_config_schema.h aatpe_command_dispatcher.h aatpe_session_token.h aatpe_recovery.h aatpe_command_queue.h aatpe_subsystem.h aatpe_placement_mech.h aatped_threadutil.h aatped_apklabel.h touch_uclamp.h touch_uclamp_act.h psi_decision.h aatped_psi.h thermal_ff/thermal_ff.h thermal_ff/thermal_ff_adapter.h

# Destination root for `make install`. Override with:
#   make install DESTDIR=/tmp/aatpe-out
# Layout mirrors the module's on-device tree:
#   $(DESTDIR)/system/bin/                 <- daemons / CLI / probe
#   $(DESTDIR)/system/etc/aatpe/           <- config + profiles + rules + webui
DESTDIR ?= $(CURDIR)/_install

all: $(TARGETS)
	@ # P0-2 (round-2): the consistency gate is SPLIT in two.
	@#   check-source-consistency.sh runs HERE, right after the targets link. It
	@#   only inspects source files (VERSION/module.prop/aatpe-version.h/WebUI/
	@#   shell banners/conf mirrors) and MUST pass on a clean tree — it never
	@#   demands system/bin/* binaries, which do not exist before `make install`.
	@#   The binary/SHA256 gate lives in check-release-consistency.sh and runs
	@#   from `make install` (after system/bin/ is populated).
	@bash ./check-source-consistency.sh

# P0-2: post-install release gate. Requires `make install` to have populated
# system/bin/*. Verifies ELF/PIE/arch/version on both ./ and system/bin/* and,
# when BUILD_MANIFEST.txt exists, that the packaged bytes match it 1:1.
.PHONY: check-release
check-release: $(TARGETS)
	@bash ./check-release-consistency.sh

# ---------------------------------------------------------------------------
# fix-v96 #7 / P2-29+30: reproducible eBPF build chain.
#   ebpf/sched_events.bpf.c --(clang -target bpf)--> ebpf/sched_events.bpf.o
#     --(python3 gen_embedded_header.py)--> ebpf/sched_events_embedded.h
#     --(#include aatpe_ebpf.cpp)--> links into aatped
#
# The embedded header is COMMITTED (zero-dependency loader), so `make all`
# builds aatped even on a host with NO BPF clang. The NDK's own clang supports
# -target bpf, so `make ebpf-prepare` rebuilds the .o AND the embedded header
# from source when you have the NDK installed. The committed
# ebpf/sched_events.bpf.o is the checked-in prebuilt bytecode so the chain is
# complete out of the box on hosts without BPF clang.
#
# P2-29+30 root-cause fix: the original toolchain probe ran
#     clang -target bpf -x c /dev/null -o /dev/null   (no -c)
# which attempts a LINK of an empty program -> always fails (undefined main),
# so even an NDK host with working -target bpf silently fell back to the
# committed .o and NEVER rebuilt from source. The probe now uses `-c` (object
# compile, no link) so BPF-capable toolchains are correctly detected.
#
# Verified on NDK r30 / clang 21: sched_events.bpf.c compiles to a .o whose
# tracepoint PROGBITS bytes are identical to the committed prebuilt, and the
# regenerated sched_events_embedded.h is byte-identical (gen_embedded_header.py
# also self-checks: no BPF_JMP32 / no BPF_ATOMIC -> kernel 4.14 safe).
# ---------------------------------------------------------------------------
CLANG_BPF ?= $(TOOLCHAIN)/bin/clang

# P2-29+30: BPF-capable clang probe. MUST use `-c` (compile to object, no
# link) — probing without -c links an empty TU and always fails.
CLANG_BPF_OK := $(shell "$(CLANG_BPF)" -target bpf -x c -c /dev/null -o /dev/null >/dev/null 2>&1 && echo yes || echo no)

# P2-29+30: reproducible eBPF prepare step. Order-only prereq of aatped.
#   * BPF clang available  -> rebuild .o from .c, regenerate embedded header.
#                            (clean build reproduces source->object->header)
#   * BPF clang absent     -> require committed .o + embedded.h (zero-dep build)
# This never fails a host that already has the committed artifacts.
.PHONY: ebpf-prepare
ebpf-prepare:
	@if [ "$(CLANG_BPF_OK)" = "yes" ]; then \
	  echo "  CLANG-BPF rebuild from source: ebpf/sched_events.bpf.c"; \
	  "$(CLANG_BPF)" -target bpf -D__TARGET_ARCH_aarch64 -O2 -g0 -Wall \
	    -Wno-unused-variable -Wno-unused-value -Wno-pointer-sign \
	    -Wno-unused-function \
	    -c ebpf/sched_events.bpf.c -o ebpf/sched_events.bpf.o || exit 1; \
	  python3 -B ebpf/gen_embedded_header.py ebpf/sched_events.bpf.o ebpf/sched_events_embedded.h; \
	  echo "  eBPF object + embedded header regenerated from C source (4.14-safe)"; \
	else \
	  if [ -f ebpf/sched_events.bpf.o ] && [ -f ebpf/sched_events_embedded.h ]; then \
	    echo "  NOTE: BPF-capable clang not detected; using committed ebpf/sched_events.bpf.o + embedded.h"; \
	    echo "        (install NDK r30 to rebuild from source; edits to the .bpf.c will NOT be compiled here)"; \
	  else \
	    echo "  ERROR: BPF clang not found at $(CLANG_BPF) and no committed ebpf artifacts."; \
	    echo "         Set NDK or ANDROID_NDK_HOME to an NDK r30 path (its clang supports -target bpf)."; \
	    exit 1; \
	  fi; \
	fi

# Kept for explicit source regeneration (P2-29+30): same as ebpf-prepare but
# named to match the historical `make ebpf-regen` workflow.
.PHONY: ebpf-regen
ebpf-regen: ebpf-prepare
	@echo "eBPF chain ready: ebpf/sched_events.bpf.c -> ebpf/sched_events.bpf.o -> ebpf/sched_events_embedded.h"

.PHONY: check-toolchain
check-toolchain:
	@test -x "$(AATPE_CC)" || { echo "ERROR: compiler not executable: $(AATPE_CC)"; echo "       Set NDK or ANDROID_NDK_HOME to your Android NDK r30 path."; exit 2; }

# B-001: aatped ships with -fstack-protector-strong in CFLAGS, but under
# ThinLTO the optimizer can inline/eliminate every frame that qualified for a
# strong-protector canary, leaving the binary with NO __stack_chk_fail import.
# The aatped daemon is the long-running, attack-exposed surface, so it gets the
# stronger -fstack-protector-all (a canary on EVERY function) in addition to the
# common flags. -z relro/-z now are already in LDFLAGS. Verified post-build via:
#   readelf --dyn-syms system/bin/aatped | grep stack_chk_fail
AATPED_CFLAGS := $(CFLAGS) -fstack-protector-all -DAATPED_ARBITRATOR_DAEMON

# P2-29+30: aatped links the embedded bytecode. Order-only prereq ebpf-prepare
# rebuilds .o+embedded.h from .c when BPF clang is available (clean-build
# reproducibility), and falls back to the committed artifacts otherwise.
aatped: $(AATPED_SRCS) $(AATPED_HDRS) | check-toolchain ebpf-prepare
	$(AATPE_CC) $(AATPED_CFLAGS) $(AATPED_SRCS) -o $@ $(LDFLAGS)

aatpe-cli: aatpe-cli.cpp aatpe_ml.cpp aatped_apklabel.cpp aatpe_ml.h aatpe-version.h aatped_apklabel.h | check-toolchain
	$(AATPE_CC) $(CFLAGS) aatpe-cli.cpp aatpe_ml.cpp aatped_apklabel.cpp -o $@ $(LDFLAGS)

aatpe-watchdog: aatpe-watchdog.cpp aatpe-version.h | check-toolchain
	$(AATPE_CC) $(CFLAGS) aatpe-watchdog.cpp -o $@ $(LDFLAGS)

aatpe-probe: aatpe-probe.cpp aatpe-version.h | check-toolchain
	$(AATPE_CC) $(CFLAGS) aatpe-probe.cpp -o $@ $(LDFLAGS)

# ---------------------------------------------------------------------------
# install target (bonus build-system hardening):
#   Copies built binaries and module resources into $(DESTDIR), matching the
#   on-device layout used by customize.sh / service.sh.
# ---------------------------------------------------------------------------
install: $(TARGETS)
	@echo "Installing AATPE into $(DESTDIR) ..."
	mkdir -p "$(DESTDIR)/system/bin"
	mkdir -p "$(DESTDIR)/system/etc/aatpe/ebpf"
	mkdir -p "$(DESTDIR)/system/etc/aatpe/profiles"
	mkdir -p "$(DESTDIR)/system/etc/aatpe/rules"
	mkdir -p "$(DESTDIR)/system/etc/aatpe/webui"
	# P28: install stripped release binaries; unstripped build copies stay in CURDIR for debugging.
	install -m 0755 aatped        "$(DESTDIR)/system/bin/"
	install -m 0755 aatpe-cli     "$(DESTDIR)/system/bin/"
	install -m 0755 aatpe-watchdog "$(DESTDIR)/system/bin/"
	install -m 0755 aatpe-probe   "$(DESTDIR)/system/bin/"
	# P28 + ROUND-014: strip hardening (G3): strip-all plus drop .comment
	# (toolchain/host path leak) and .note.android.ident. Post-strip verified
	# DYN(PIE)/AArch64; repro reproduces bytes.
	"$(AATPE_STRIP)" --strip-all --remove-section=.comment \
	    --remove-section=.note.android.ident \
	    "$(DESTDIR)/system/bin/aatped" \
	    "$(DESTDIR)/system/bin/aatpe-cli" "$(DESTDIR)/system/bin/aatpe-watchdog" \
	    "$(DESTDIR)/system/bin/aatpe-probe"
	install -m 0644 aatpe.conf    "$(DESTDIR)/system/etc/aatpe/"
	cp -f profiles/*.conf         "$(DESTDIR)/system/etc/aatpe/profiles/"
	cp -f rules/*.conf            "$(DESTDIR)/system/etc/aatpe/rules/"
	cp -f webui/index.html        "$(DESTDIR)/system/etc/aatpe/webui/"
	# G10-005: the on-device full-function test script now lives in the source tree
	# (device/) and is installed into the package, instead of being dropped in
	# ad-hoc at packaging time.
	install -m 0755 device/aatpe_full_function_test.sh "$(DESTDIR)/system/etc/aatpe/"
	# v4.4.5: on-device evidence collector + readiness doctor (C4/GPU validation).
	install -m 0755 device/collect.sh "$(DESTDIR)/system/etc/aatpe/"
	install -m 0755 device/doctor.sh  "$(DESTDIR)/system/etc/aatpe/"
	# eBPF source / compiled object / embedded header / generator script are
	# shipped to the device so customize.sh can set_perm on them and the
	# on-device eBPF diagnostic can read the source for offset verification.
	cp -f ebpf/sched_events.bpf.c ebpf/sched_events.bpf.o \
	      ebpf/sched_events_embedded.h ebpf/gen_embedded_header.py \
	      "$(DESTDIR)/system/etc/aatpe/ebpf/"
	# I-009 (v4.3.4): strip Python bytecode cache from the installed tree. The
	# ebpf/ generator script (gen_embedded_header.py) leaves __pycache__/*.pyc
	# next to it on the build host; without this cleanup those caches would ship
	# in the module zip and land on-device (where they are never used, only
	# consume space, and can leak build-host paths).
	rm -rf "$(DESTDIR)/system/etc/aatpe/ebpf/__pycache__"
	find "$(DESTDIR)" -type d -name '__pycache__' -prune -exec rm -rf {} +
	find "$(DESTDIR)" -type f -name '*.pyc' -delete
	@echo "Install complete. Tree under $(DESTDIR)/system/ (binaries stripped)"
	@# P0-2: run the post-install release gate against the installed tree. When
	@# DESTDIR is the default _install/, also mirror into ./system/bin so the
	# release-consistency script (which inspects ./system/bin) sees the stripped
	# bytes used by the module package.
	@# LAYOUT-GATE FIX: -MMD -MP emits one root-level *.d dependency sidecar per
	@# linked target (aatped.d / aatpe-cli.d / aatpe-watchdog.d / aatpe-probe.d).
	@# These are build byproducts, NOT release content, but check-manifest-layout.sh's
	@# stray-artifact gate treats ANY *.d as stray -> a clean rebuild false-FAILs
	@# "[FAIL] stray build artifacts" even though all 4 stripped binaries shipped.
	@# We rm the root-level build .d sidecars inside the gate block (right before
	@# check-manifest-layout.sh); `make` regenerates them on the next build.
	@if [ "$(DESTDIR)" = "$(CURDIR)/_install" ]; then \
	  mkdir -p system/bin system/etc/aatpe/webui system/etc/aatpe/profiles system/etc/aatpe/rules system/etc/aatpe/ebpf; \
	  cp -f _install/system/bin/* system/bin/; \
	  cp -f _install/system/etc/aatpe/aatpe.conf system/etc/aatpe/; \
	  cp -f _install/system/etc/aatpe/webui/index.html system/etc/aatpe/webui/; \
	  cp -f _install/system/etc/aatpe/aatpe_full_function_test.sh system/etc/aatpe/; \
	  cp -f _install/system/etc/aatpe/collect.sh system/etc/aatpe/; \
	  cp -f _install/system/etc/aatpe/doctor.sh system/etc/aatpe/; \
	  cp -f profiles/*.conf system/etc/aatpe/profiles/; \
	  cp -f rules/*.conf system/etc/aatpe/rules/; \
	  cp -f ebpf/sched_events.bpf.c ebpf/sched_events.bpf.o ebpf/sched_events_embedded.h ebpf/gen_embedded_header.py system/etc/aatpe/ebpf/; \
	  $(MAKE) --no-print-directory release-manifest; \
	  rm -f ./aatped.d ./aatpe-cli.d ./aatpe-watchdog.d ./aatpe-probe.d; \
	  bash ./check-manifest-layout.sh; \
	  bash ./check-release-consistency.sh; \
	  bash ./check-html-sync.sh; \
	fi

# ---------------------------------------------------------------------------
# release-manifest target (ROUND-001 G10): regenerate BUILD_MANIFEST.txt from the
# freshly stripped system/bin/* bytes, then let check-release-consistency.sh
# re-verify bytes==manifest. thinLTO build output is not bit-for-bit reproducible
# across machines, so the manifest MUST be (re)computed from THIS build's stripped
# binaries as the final release step — never hand-edited, never committed from a
# different build. This automates what previously had to be done by hand.
# ---------------------------------------------------------------------------
release-manifest:
	@echo "==> Regenerating BUILD_MANIFEST.txt from system/bin/* (this build)"
	@test -d system/bin || { echo "system/bin missing; run 'make install' first" >&2; exit 1; }
	@head -n 3 BUILD_MANIFEST.txt > BUILD_MANIFEST.txt.new 2>/dev/null || true
	@echo "" >> BUILD_MANIFEST.txt.new
	@( cd system/bin && sha256sum aatped aatpe-cli aatpe-watchdog aatpe-probe ) >> BUILD_MANIFEST.txt.new
	@mv BUILD_MANIFEST.txt.new BUILD_MANIFEST.txt
	@echo "==> BUILD_MANIFEST.txt updated:"; grep -E '  (aatped|aatpe-cli|aatpe-watchdog|aatpe-probe)$$' BUILD_MANIFEST.txt

# ---------------------------------------------------------------------------
# strip target (P28): strip release binaries in-place (module-package build).
#   Unstripped versions are kept alongside with a .debug suffix for debugging.
# ---------------------------------------------------------------------------
strip: $(TARGETS)
	@for b in $(TARGETS); do \
	  cp -f "$$b" "$$b.debug"; \
	  "$(AATPE_STRIP)" --strip-all --remove-section=.comment --remove-section=.note.android.ident "$$b"; \
	  echo "stripped $$b (unstripped kept as $$b.debug)"; \
	done

# ---------------------------------------------------------------------------
# version target (P31): print the build environment used by this Makefile.
# ---------------------------------------------------------------------------
version:
	@echo "AATPE build environment"
	@echo "  AATPE_VERSION       = $$(grep -m1 '^#define AATPE_VERSION ' aatpe-version.h | awk '{print $$3}')"
	@echo "  AATPE_VERSION_CODE  = $$(grep -m1 '^#define AATPE_VERSION_CODE' aatpe-version.h | awk '{print $$3}')"
	@echo "  NDK                  = $(NDK)"
	@echo "  TOOLCHAIN            = $(TOOLCHAIN)"
	@echo "  CXX                  = $(AATPE_CC)"
	@echo "  STRIP                = $(AATPE_STRIP)"
	@echo "  API level            = android29 (aarch64)"
	@echo "  linker               = lld (default clang++)"
	@echo "  C++ standard         = c++17"
	@echo "  CFLAGS               = $(CFLAGS)"
	@echo "  LDFLAGS              = $(LDFLAGS)"
	@echo "  SOURCE_DATE_EPOCH    = $(SOURCE_DATE_EPOCH)"
	@echo "  CC version           = $$(test -x "$(AATPE_CC)" && "$(AATPE_CC)" --version | head -1 || echo '(compiler not found; set NDK)')"

# ---------------------------------------------------------------------------
# checksum target (P32): SHA256 of all release binaries for reproducibility.
# ---------------------------------------------------------------------------
checksum: $(TARGETS)
	@echo "==> SHA256 of release binaries"
	@sha256sum $(TARGETS)

# ---------------------------------------------------------------------------
# test targets (P2 issues #29/#31/#35):
#
#   make test          -> AUTHORITATIVE v94 test surface:
#                           1) tests/v94/build.sh    (9 hermetic C++ unit tests)
#                           2) tests/integration/     (end-to-end integration)
#                         Both run on the HOST with g++ (no NDK needed).
#   make test-unit     -> v94 hermetic C++ unit tests only (tests/v94/build.sh)
#   make test-integration -> end-to-end integration test only
#   make test-legacy   -> HISTORICAL v74 Python suite (tests/aatpe_v74_test_suite.py)
#                         Kept for archaeology; its failures are gold-value drift,
#                         NOT production regressions. See tests/README.md.
#
# These are host-g++ based and deliberately NOT part of the NDK arm64 cross-build.
# ---------------------------------------------------------------------------
test: test-unit test-integration
	@echo "==> make test: authoritative v94 surface (unit + integration) OK"

test-unit:
	@echo "==> [v94 unit] building + running tests/v94/build.sh"
	@bash tests/v94/build.sh

test-integration:
	@echo "==> [v94 integration] building tests/integration"
	@bash tests/integration/build.sh "$(CURDIR)" "$(CURDIR)/tests/integration"
	@echo "==> [v94 integration] running (unprivileged user+mount namespace)"
	@bash tests/integration/run_integration.sh "$(CURDIR)" "$(CURDIR)/tests/integration/aatpe_integration_test"

# Historical / legacy v74 suite. A failure here is almost always stale gold,
# not a regression — classify per tests/README.md before chasing production code.
test-legacy:
	@echo "==> [legacy] running v74 Python suite (historical; failures may be stale gold)"
	@python3 tests/aatpe_v74_test_suite.py

clean:
	rm -f $(TARGETS) *.o *.d *.debug
	rm -rf "$(DESTDIR)"
	# S-003: `make install` mirrors stripped binaries + module resources into the
	# ./system/ image (see the install target's default-DESTDIR branch). The old
	# clean only removed $(DESTDIR)=_install, leaving ./system/ behind so a later
	# `make install`/release gate could pick up stale stripped bytes. Remove it.
	rm -rf system/

.PHONY: all check-toolchain install strip version checksum check-release test test-unit test-integration test-legacy clean

-include $(wildcard *.d)

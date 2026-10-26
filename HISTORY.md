# AATPE Version History (HISTORY.md)

v94 fix / 问题39. This file is the human-readable changelog. Historical
inline comments in the code are **kept on purpose** (archaeological value) and
are NOT deleted; this file only summarizes the lineage.

Convention: every v94 fix in code is tagged with `// v94 fix / 问题NN:` so it
can be grepped (`grep -rn "v94 fix"`) for later maintenance.

## Version lineage (major engineering milestones)

| Tag   | Theme |
|-------|-------|
| v34.. | Early scoring/role classification baseline. |
| v66   | HMAC security, ML learning funnel, hybrid learning, battery aging, cold-start, v66 P0/P2/P3 audit (mutex, unit detection, stale-zone cleanup). |
| v68   | HMAC replay protection, hybrid learning constants, eBPF ring buffer defaults. |
| v69   | Comprehensive host-side suite (800+ units). |
| v70..v73 | Game-scene recognition, config hot-reload, watchdog self-heal. |
| v74   | Embedded eBPF bytecode verification suite, perf ring buffer, dual-mode. |
| v83   | Ring page-count tuning (128KB). |
| v86/v87/v88 | Load-balancing / contention recalibration; temp jump threshold 25→45°C (50°C in-game); `thermalEmergency<=0` default 105. |
| v89..v92 | Mid-pool (A78) score uplift, cross-pool penalty 10→7, idle-boost 80→40. |
| v93   | Thread-limit fix. |
| v94   | Comprehensive consistency + test-engineering + observability + lifecycle pass. |
| v3.2.0 | Engineering maintainability: per-app config wired, idempotent rules reload, TID-reuse aux-state invalidation, WebUI cache self-heal, runtime 0700/0600, CSPRNG token fail-closed, SOURCE_DATE_EPOCH pinning. |
| v3.3.0 | Compute-first ("算力优先") placement: big-pool favors highest-capacity cluster; mid-pool score uplift. |
| v3.4.0 | Thermal three-tier thresholds (WARNING/CRITICAL/EMERGENCY), profile-arbiter, recovery, command queue, ADRC controller. |
| v3.4.2 | On-device migration fixes: emergency bypass, ring 64→128 vote from degraded path, confidence 50→30, reject reason counters. |
| v3.4.4 | Crash hardening: static-destruction-order fiasco, control-file O_TRUNC on startup. |
| **v3.4.6** | Trusted-install integrity manifest, eBPF offset verification, PID-identity guard, WebUI HMAC/path-traversal/oversize hardening, system/ mirror committed. |

## v94 changes (this pass)

### P2 — test engineering
- **问题33** `tests/aatpe_v74_test_suite.py`: robust source-root resolution
  (`tests/aatpe_src` symlink, fallback to script-parent).
- **问题34/36/37** `tests/README.md`: authoritative vs legacy tests, behavioral-vs-structural
  strategy, failure-classification guide; legacy test files marked `[LEGACY-TEST]`;
  report now carries a `FAILURE CLASSIFICATION` block.
- **问题35** `tests/v94/test_webui_json.cpp`: explicit `<cstdint>`.
- **问题38** `tests/v94/test_thermal.cpp`: rewritten for the real thermal policy
  (20s boot grace, 3-read debounce, 45°C jump filter) instead of instant-emergency.

### P3 — code quality / observability / lifecycle
- **问题39** this file; unified `// v94 fix / 问题NN:` markers.
- **问题40** `aatpe-constants.h`: consolidated `fallback::` constants.
- **问题41** daemon is the single state producer; watchdog/WebUI consume state files.
- **问题42** unused/legacy functions marked `// TODO: consider removing`.
- **问题44/45** high-frequency eBPF events logged at DEBUG; level policy documented.
- **问题46** eBPF event stats (total/processed/dropped/overflow/fallback) exposed;
  fixed the `AnyEvent` union default-ctor compile break in `aatpe_ebpf.h`.
- **问题47** placement failures classified by reason + counters.
- **问题48** `uninstall.sh`/`service.sh` made idempotent.
- **问题49** daemon exit cleanup (pid/socket/temp/eBPF detach) audited.
- **问题50/51** watchdog↔daemon ordering and eBPF attach-failure fallback documented
  (require on-device verification).

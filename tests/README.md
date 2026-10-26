# AATPE Test Suite — Guide (v94)

This directory holds the whole AATPE test surface. v94 introduced this
document (issues **#34 / #36 / #37**) so that a red test run can be triaged
quickly: **which tests are authoritative, which are historical, and how to tell
a real production bug from an outdated gold-value assertion.**

---

## 1. Which tests are authoritative for v94

### 1.1 v94 behavioral tests — **authoritative**
- `tests/v94/*.cpp` — small, hermetic, behavior-driven unit tests. Each file
  exercises one production translation unit's *behavior* (not its source text):
  - `test_cli.cpp`, `test_probe.cpp`, `test_interfaces.cpp`,
    `test_observability.cpp`, `test_event_monitor.cpp`, `test_uevent.cpp`,
    `test_watchdog.cpp`, `test_webui_json.cpp`, `test_thermal.cpp`
- `tests/integration/` — host-buildable integration binary
  (`build.sh` → `aatpe_integration_test`).

> ~~`tests/ab/aatpe_test_v94_comprehensive_ab.cpp`~~ — 已在 ROUND-001 (2026-09-30)
> 移入 `tests/legacy/ab/`。它在当前源码树上主机编译失败（全局变量已从 `int` 改为
> `std::atomic`，结构漂移），**未接入 `make test`**，不再是权威 A/B 门禁。

**If a v94 test fails, that is a strong signal of a real regression** and
should be investigated as a production bug (unless the test itself is new and
wrong — but it tests behavior, so the bar is high).

### 1.2 Historical / legacy tests — **archived under `tests/legacy/`**
These predate v94 and encode the source *as it looked at the time*. As of
ROUND-001 they have been moved out of the build/search paths into `tests/legacy/`;
see `tests/legacy/README.md` for the full inventory. They are **not built, not run
by `make test`, and kept only for archaeology**:
- `tests/legacy/root/aatpe_test*.cpp` (11 files, v66–v71)
- `tests/legacy/tests/test_{v66,v68,v69,p0,p1,scoring,http_server,ml_funnel}.cpp` (8 files)
- `tests/legacy/ab/aatpe_test_v94_comprehensive_ab.cpp` (v94 A/B harness; currently stale)
- Python: `tests/aatpe_v74_test_suite.py` (v74) — **still at `tests/`** because
  `make test-legacy` references that exact path; failures = stale gold.

Every one of these now carries a `[LEGACY-TEST vNN]` header marker.
**A failure here does NOT automatically mean production is broken** — it often
means the test's baked-in gold value (eBPF offsets, map counts, md5, version
string) no longer matches the evolved source. These are kept on purpose for
archaeology; they are **not deleted**.

---

## 2. Build & run commands (P2 issues #29 / #31 / #32 / #35)

The Makefile now points `make test` at the **authoritative v94 surface**, not
the legacy v74 suite:

| Command | What it runs | Notes |
|---|---|---|
| `make test` | v94 authoritative = `test-unit` + `test-integration` | the single entry point |
| `make test-unit` | `tests/v94/build.sh` (50+ hermetic C++ unit tests) | host g++, `-Werror` |
| `make test-integration` | build + run `tests/integration/` (namespace) | 10 scenarios / 99 points |
| `make test-legacy` | `python3 tests/aatpe_v74_test_suite.py` | **historical**; failures = stale gold |

### 2.1 v94 unit tests — `tests/v94/build.sh`
- One script builds **and runs** all hermetic tests (~50 targets; the set is
  enumerated in `build.sh`'s `ALL` case — do not hard-code a count). Test files compile with
  `-std=c++17 -Wall -Wextra -Werror`; production `.cpp` files compile with `-w`
  (they carry their own warnings policy).
- Build artifacts land in `tests/v94/build/`. Run a single test:
  `bash tests/v94/build.sh thermal`.
- `thermal` self-runs 4 scenarios under `unshare` with a fake `/sys/class`.

### 2.2 `tests/aatpe_src -> ..` symlink (P2 issue #35)
- This self-referencing link is **legacy-compat only**. The Python suite now
  resolves the source root in priority order: `--source-root <DIR>` →
  `AATPE_SRC_DIR` env → the symlink → script's parent dir. The symlink may be
  deleted and tests still run; it is kept only for the release-package layout.

### 2.3 Where run-time reports go (P2 issue #34)
- The Python suite writes `tests/output/test_report_v74.txt` (not into the
  source root). Override with `--report <path>` or `AATPE_TEST_OUT=<dir>`.

---

## 3. Test strategy (issue #36)

- **Behavioral tests beat structural tests.** Prefer asserting *what the code
  does* (outputs, state transitions, return values, JSON shape) over asserting
  *what the source looks like* (grep for a byte offset, count of maps, file
  md5, presence of a literal).
- `tests/v94/` is the reference for this: each test calls the production
  function and checks observable behavior under controlled (stubbed sysfs /
  env-var) inputs.
- **Do not add new structural assertions.** If you need to verify a loader or
  parser, drive it with sample input and check the parsed result, not the byte
  layout.
- Legacy structural tests may stay, but new tests must not copy their style.

---

## 4. Test-failure classification guide (issue #37)

When a test fails, classify it **before** chasing production code:

### 3.1 Likely "test itself is stale" (gold-value drift)
These failure patterns almost always mean the assertion is out of date, not
that production regressed:

| Pattern | Why it drifts | What to do |
|---|---|---|
| eBPF instruction/offset mismatch (`expected [0,1,1], got [0,1,1,1]`) | The embedded bytecode was rebuilt/regenerated in a later version; instruction layout legitimately changed | Update the gold value or mark the assertion version-pinned |
| map count / map def list mismatch | Maps were added/removed during feature work | Re-derive expected map list from current `sched_events.bpf.c` |
| file md5 / sha mismatch | Any source edit changes the hash | Never pin md5 of mutable source files |
| hard-coded version string (`v74`/`v87`/...) | Version bumped in `aatpe-version.h` / `module.prop` | Read the version dynamically |
| "binary exists aatped/aatpe-cli/..." in a host run | Tests expect an Android-build output that does not exist on host | Expected on host; only meaningful after a device build |
| `legacy_sched_switch_removed` / old map-name grep | Production legitimately removed a deprecated path | Delete the assertion or re-point it at the new path |

### 3.2 Likely "real production bug"
- A `tests/v94/*` behavioral test fails (these test outputs, not layout).
- A failure changes observable **runtime behavior**: wrong threshold used,
  wrong state machine transition, crash/UB, wrong JSON field, wrong unit
  conversion, mutex/race symptom.
- The same scenario that *used to pass a behavioral assertion* now fails after a
  production change.

### 3.3 Triage steps when in doubt
1. Note the failing test's marker: `[LEGACY-TEST]` ⇒ suspect gold drift first;
   `tests/v94/` ⇒ suspect production first.
2. Read the assertion. Does it compare a **behavior** (return value / state /
   JSON) or a **source artifact** (offset / count / md5 / version literal)?
   Artifact comparison → almost certainly stale.
3. For eBPF gold failures, regenerate the expected list from
   `ebpf/sched_events.bpf.c` + `ebpf/sched_events_embedded.h` of the *current*
   version instead of blaming the loader.
4. Only after ruling out (1)–(3) treat it as a production regression.

### 3.4 Failure classification field in reports
Test reports **must** carry a per-failure `classification` field so CI/QA can
split the triage load. Use one of:

- `PRODUCTION_BUG` — likely real regression (behavioral).
- `TEST_STALE_GOLD` — eBPF offset / map count / md5 / version literal drift.
- `TEST_ENVIRONMENT` — host-vs-device (missing binary, no sysfs, no ring buf).
- `AMBIIGUOUS` — needs human review.

The legacy Python suite (`aatpe_v74_test_suite.py`) records failures with a
leading `[category]` tag; when filing a bug for a failure from it, annotate the
issue with one of the four labels above rather than re-running blindly.

---

## 5. Layout

```
tests/
├── README.md                  # this file
├── aatpe_v74_test_suite.py    # LEGACY (v74) structural gold suite (still wired to make test-legacy)
├── aatpe_src -> ..            # LEGACY self-referencing symlink (compat only;
│                              #   Python suite resolves source root without it)
├── v94/                       # v94 behavioral tests (authoritative)
│   ├── build.sh               # unified build+run for all hermetic targets (P2 #32)
│   ├── build/                 # build artifacts (created by build.sh; gitignored)
│   └── test_*.cpp             # ~52 hermetic unit tests
├── integration/               # host integration build + runner (10 scenarios)
├── legacy/                    # ARCHIVED historical tests (ROUND-001; not built, not run)
│   ├── README.md              # inventory + why they are here
│   ├── root/                  # former source-root aatpe_test*.cpp (v66–v71)
│   ├── tests/                 # former tests/test_*.cpp (v66–v74)
│   └── ab/                    # former v94 A/B harness (currently stale)
├── stubs/                     # host stubs (android/log.h, properties, log,
│                              #   v94_daemon_deps.cpp daemon-global bag)
└── output/                    # run-time reports (P2 #34; gitignored)
```

#!/usr/bin/env python3
"""
AATPE v74 classify_failure() deliberate-failure gate (final-review fix).

Purpose
-------
The v74 suite's classify_failure() previously swallowed real regressions:
  * "expected 4 maps, got 3 maps"   (map-count regression)   -> TEST_STALE_GOLD
  * "expected 24, got 28"           (offset drift)          -> TEST_STALE_GOLD
  * assert_true with empty detail                              -> AMBIGUOUS
and sys.exit() stayed 0. This gate loads the REAL suite module, runs it, and
asserts the tightened routing on the three adversarial strings plus the
preserved (still-correct) env/stale cases. It exits 1 if any routing assertion
is wrong, so a regression of this fix itself fails CI.

Usage:
    python3 tests/aatpe_v74_classify_gate_selftest.py
"""
import contextlib
import io
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SUITE = os.path.join(_HERE, 'aatpe_v74_test_suite.py')

_failures = []


def check(cond, label):
    if cond:
        print(f'  [GATE-PASS] {label}')
    else:
        print(f'  [GATE-FAIL] {label}')
        _failures.append(label)


def main():
    print('=== v74 classify_failure deliberate-failure gate ===')
    # Load + run the real suite (its module-level 646 tests execute here;
    # sys.exit() at the end becomes SystemExit, which we catch to read state).
    print('--- loading real suite module (full run) ---')
    ns = {'__name__': 'v74_suite_under_gate', '__file__': _SUITE}
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            with open(_SUITE) as f:
                src = f.read()
            exec(compile(src, _SUITE, 'exec'), ns)
            suite_rc = 0
        except SystemExit as e:
            suite_rc = e.code if isinstance(e.code, int) else (1 if e.code else 0)
    classify = ns['classify_failure']
    R = ns['R']

    print(f'--- real suite run finished: sys.exit rc was {suite_rc} ---')
    print(f'--- real run classification: {dict(R.by_class)} ---')

    # 1) Adversarial strings MUST route to PRODUCTION_BUG (functional-regression
    #    context: a non-legacy category, as a real e2e/functional test would).
    print('--- adversarial routing (must be PRODUCTION_BUG) ---')
    adv = [
        ('9.functional_e2e', 'e2e_map_count', 'expected 4 maps, got 3 maps'),
        ('9.functional_e2e', 'e2e_offset_drift', 'expected 24, got 28'),
        ('9.functional_e2e', 'e2e_bare_truth_check', ''),
    ]
    for cat, name, detail in adv:
        got = classify(cat, name, detail)
        check(got == 'PRODUCTION_BUG',
              f'{cat}/{name!r}/{detail!r} -> {got} (want PRODUCTION_BUG)')

    # 2) Preserved: real legacy-gold cases stay TEST_STALE_GOLD.
    print('--- preserved legacy-gold routing (must stay TEST_STALE_GOLD) ---')
    gold = [
        ('1.eBPF_bytecode', 'program_count', 'expected 4, got 6'),
        ('1.eBPF_bytecode', 'map_count', 'expected 3, got 4'),
        ('1.eBPF_bytecode', 'sched_switch_next_pid_offset_value',
         'expected 56, got None'),
        ('3.perf_ring_buffer', 'mmap_updates_eventsMap', ''),
    ]
    for cat, name, detail in gold:
        got = classify(cat, name, detail)
        check(got == 'TEST_STALE_GOLD',
              f'{cat}/{name!r} -> {got} (want TEST_STALE_GOLD)')

    # 3) Preserved: environment cases stay TEST_ENVIRONMENT.
    print('--- preserved environment routing (must stay TEST_ENVIRONMENT) ---')
    got = classify('2.eBPF_loader', 'ebpf_o_exists', 'expected /data/aatpe/ebpf.o missing')
    check(got == 'TEST_ENVIRONMENT', f'env blob -> {got} (want TEST_ENVIRONMENT)')

    # 4) Exit-gate proof: record the three adversarial failures into the REAL
    #    TestResult and show the suite's own exit decision goes non-zero.
    print('--- exit-gate proof on the real TestResult ---')
    before_pb = R.by_class.get('PRODUCTION_BUG', 0)
    for cat, name, detail in adv:
        R.record(cat, name, False, detail)
    after_pb = R.by_class.get('PRODUCTION_BUG', 0)
    ambig = R.by_class.get('AMBIGUOUS', 0)
    check(after_pb == before_pb + 3,
          f'adversarial records bumped PRODUCTION_BUG {before_pb} -> {after_pb}')
    would_exit = 0 if (after_pb == 0 and ambig == 0) else 1
    check(would_exit == 1, 'exit gate decided rc=1 with adversarial failures present')

    print()
    if _failures:
        print(f'GATE RESULT: FAIL ({len(_failures)} routing assertions wrong)')
        return 1
    print('GATE RESULT: PASS — adversarial strings route to PRODUCTION_BUG and the '
          'exit gate is non-zero; env/legacy-gold routing preserved.')
    return 0


if __name__ == '__main__':
    sys.exit(main())

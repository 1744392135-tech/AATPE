#!/bin/bash
set -u
QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export QA_MODE=fuzz; source "$QA_DIR/common.sh"

ART="$QA_DIR/artifacts"; mkdir -p "$ART"
MUTDIR="$QA_DIR/build/mutated_corpus"; rm -rf "$MUTDIR"; mkdir -p "$MUTDIR"

fuzz_bin() {
  local t="$1"
  for d in "$QA_DIR/build/fuzz" "$QA_DIR/build/fuzz_noengine"; do
    [ -x "$d/fuzz_$t" ] && { echo "$d/fuzz_$t"; return 0; }
  done
  return 1
}

HAVE_LIBFUZZER=0
if echo 'int main(){}' | $CXX -fsanitize=fuzzer -x c++ - -o /tmp/_lf_t 2>/dev/null; then
  HAVE_LIBFUZZER=1; rm -f /tmp/_lf_t
fi

FUZZ_TIME="${FUZZ_TIME:-30}"
MUT_COUNT="${MUT_COUNT:-2000}"

gen_mutated() {
  local t="$1"
  python3 - "$QA_DIR/corpus/$t" "$MUTDIR/$t" "$MUT_COUNT" <<'PY'
import os, sys, random
seed_dir, out_dir, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
random.seed(20260929)
os.makedirs(out_dir, exist_ok=True)
seeds = []
for fn in sorted(os.listdir(seed_dir)):
    p = os.path.join(seed_dir, fn)
    with open(p, 'rb') as f: seeds.append(f.read())
if not seeds: seeds = [b'']
nonempty = [s for s in seeds if s] or [b'x']
idx = 0
def emit(b):
    global idx
    with open(os.path.join(out_dir, 'm%05d' % idx), 'wb') as f: f.write(b)
    idx += 1
for s in seeds: emit(s)
for _ in range(n):
    s = bytearray(random.choice(seeds))
    op = random.randrange(6)
    if op == 0 and s:
        i = random.randrange(len(s)); s[i] ^= 1 << random.randrange(8)
    elif op == 1 and s:
        del s[random.randrange(len(s)):]
    elif op == 2:
        i = random.randrange(len(s)+1); s[i:i] = bytes([random.randrange(256)])
    elif op == 3:
        o = random.choice(nonempty); cut = random.randrange(len(o))
        s = s + o[:cut]
    elif op == 4:
        b = random.randrange(256); s = bytearray([b]) * random.randint(1, 64)
    else:
        for i in range(len(s)):
            if random.random() < 0.1: s[i] = 0
    emit(bytes(s))
PY
}

run_one() {
  local t="$1"
  local bin; bin="$(fuzz_bin "$t")"
  if [ -z "$bin" ]; then
    echo "==> [fuzz:$t] binary missing, building..."; bash "$QA_DIR/build_fuzz.sh" "$t" || { qa_record_fail "fuzz_$t" "build failed"; return; }
    bin="$(fuzz_bin "$t")"
  fi
  echo "==> [fuzz:$t]"
  local log="$ART/fuzz_$t.log"
  if [ "$HAVE_LIBFUZZER" = 1 ]; then
    ASAN_OPTIONS=detect_leaks=1 "$bin" -max_total_time="$FUZZ_TIME" \
        -print_final_stats=1 "$QA_DIR/corpus/$t" >"$log" 2>&1
  else
    gen_mutated "$t"
    ASAN_OPTIONS=detect_leaks=1 "$bin" "$MUTDIR/$t" >"$log" 2>&1
  fi
  local rc=$?
  if [ "$rc" -eq 0 ]; then
    local n; n=$(grep -c 'bytes)' "$log" 2>/dev/null || echo 0)
    qa_record_pass "fuzz_$t" "no crash ($n inputs, $(grep -m1 'complete' "$log" || echo 'ok'))"
  else
    local crash; crash=$(grep -oE "$MUTDIR/$t/[A-Za-z0-9_.-]+" "$log" | tail -1)
    if [ -n "$crash" ] && [ -f "$crash" ]; then
      cp "$crash" "$ART/fuzz_${t}-crash-input.bin"
      echo "  [CRASH] reproducer saved: $ART/fuzz_${t}-crash-input.bin"
    fi
    echo "  ---- fuzz log tail ----"; tail -30 "$log"
    qa_record_fail "fuzz_$t" "exit=$rc see fuzz_$t.log"
  fi
}

if [ "$#" -eq 0 ]; then set -- json config event command cgroup uclamp threadname; fi
for t in "$@"; do run_one "$t"; done
qa_summary
[ "$QA_FAIL" -eq 0 ]

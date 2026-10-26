#!/bin/bash
# batch fz build: compile common prod TUs once, link each fz + stubs.
set -u
FZ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$FZ_DIR/../.." && pwd)"
STUB="$SRC/tests/stubs"
OUT="$FZ_DIR/build"; mkdir -p "$OUT/p"
CXX=g++
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
INC="-I$SRC -I$SRC/shared -I$STUB -D__clang_version__=\"host\""

# directory stubs
cat > "$OUT/dirs.cpp" <<'EOF'
const char* STATE_DIR="/tmp/aatpe_state";
const char* CONF_DIR="/tmp/aatpe_conf";
const char* WEBUI_DIR="/tmp/aatpe_webui";
int stateCacheIsSeeded(){return 1;}
EOF
$CXX -std=c++17 -w -fPIC -pthread $SAN $INC -c "$OUT/dirs.cpp" -o "$OUT/p/dirs.o" || exit 1

# prod objects (function-sections for the heavy webui_json so only jsonEscape survives)
compile() { # $1=tuple  $2=extra
  $CXX -std=c++17 -O1 -w -fPIC -pthread $SAN $INC ${2:-} -c "$SRC/$1.cpp" -o "$OUT/p/$1.o" 2>"$OUT/p/$1.err" \
    && echo "cc $1" || { echo "CCFAIL $1"; head -3 "$OUT/p/$1.err"; }
}
compile aatpe_config_schema ""
compile aatped_control ""
compile aatped_utils ""
compile aatped_hmac ""
compile aatped_logging ""
# webui_json gc
$CXX -std=c++17 -O1 -w -fPIC -pthread $SAN -ffunction-sections -fdata-sections $INC -c "$SRC/webui_json.cpp" -o "$OUT/p/webui_json.o" 2>/dev/null && echo "cc webui_json"

PASS=0; FAIL=0; BF=0
for f in "$FZ_DIR"/fz_*.cpp; do
  name=$(basename "$f" .cpp); bin="$OUT/$name"
  GC="-Wl,--gc-sections"
  if $CXX -std=c++17 -Wall -w -fPIC -pthread $SAN $GC $INC "$f" \
       "$OUT/p/aatpe_config_schema.o" "$OUT/p/aatped_control.o" "$OUT/p/aatped_utils.o" \
       "$OUT/p/aatped_hmac.o" "$OUT/p/aatped_logging.o" "$OUT/p/webui_json.o" "$OUT/p/dirs.o" \
       -o "$bin" 2>"$OUT/$name.e"; then
    timeout 20 "$bin" "$OUT/crash" "$name" > "$OUT/$name.run" 2>&1; rc=$?
    if [ $rc -eq 0 ] || [ $rc -eq 124 ]; then echo "PASS $name rc=$rc"; PASS=$((PASS+1))
    else echo "FAIL $name rc=$rc"; FAIL=$((FAIL+1)); cp "$OUT/$name.run" "$LOGS/$name-crash.log" 2>/dev/null; fi
  else
    echo "BUILDFAIL $name: $(grep -m1 undefined "$OUT/$name.e" | sed 's/.*undefined//')"; BF=$((BF+1))
  fi
done
echo "=== fz: PASS=$PASS FAIL=$FAIL BUILDFAIL=$BF ==="

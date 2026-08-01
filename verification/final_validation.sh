#!/bin/bash
# MINICAT v2.0.1 FINAL VALIDATION — single WSL invocation
# Usage: bash final_validation.sh <win-src> <win-scripts-dir>
SRC="$1"; SCRIPTS="$2"
MT=/tmp/mt
mkdir -p "$MT"
cp "$SRC" "$MT/minicat.c"
cp "$SCRIPTS"/*.sh "$MT/" 2>/dev/null
cp -r "$SCRIPTS/seeds_http" "$MT/" 2>/dev/null
sed -i 's/\r$//' "$MT/minicat.c" "$MT"/*.sh
echo "== [1] BUILD =="
gcc -Wall -Wextra -Wpedantic -O2 -o "$MT/minicat" "$MT/minicat.c" && echo "gcc OK"
clang -Wall -Wextra -Wpedantic -O2 -fsanitize=address,undefined -fno-omit-frame-pointer -o "$MT/minicat_asan" "$MT/minicat.c" && echo "clang-asan OK"
afl-clang-fast -O2 -o "$MT/minicat_afl" "$MT/minicat.c" 2>/dev/null && echo "afl OK"
echo "== [2] VERSION SMOKE =="
"$MT/minicat" -l -p 18820 -H -k >"$MT/smoke.log" 2>&1 &
SP=$!
sleep 0.7
python3 - "$MT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", 18820), timeout=5)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
d = b""
try:
    while True:
        x = s.recv(4096)
        if not x: break
        d += x
except Exception: pass
s.close()
ver = b"v2.0.1" in d
print("smoke:", "OK v2.0.1" if (b"200 OK" in d and ver) else "FAIL", repr(d.split(b"\r\n")[5:7]))
PY
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo "== [3] BATTERY =="
timeout 280 bash "$MT/verify_v3.sh" "$MT/minicat" > "$MT/battery_result.txt" 2>&1
grep -E "RESULT|FAILED" "$MT/battery_result.txt"
echo "== [4] LOAD =="
timeout 200 bash "$MT/load_test.sh" "$MT/minicat" > "$MT/load_result.txt" 2>&1
grep -E "RESULT|FAIL" "$MT/load_result.txt"
echo "== [5] VALGRIND =="
timeout 400 bash "$MT/vg_run.sh" "$MT/minicat" > "$MT/vg_result.txt" 2>&1
grep -E "VALGRIND RESULT" "$MT/vg_result.txt"
echo "== [6] AFL SMOKE (75s HTTP parser) =="
rm -rf "$MT/afl_out"
timeout 80 env AFL_SKIP_BIN_CHECK=1 afl-fuzz -i "$MT/seeds_http" -o "$MT/afl_out" -m none -t 2000+ -- env MINICAT_FUZZ_FILE=@@ "$MT/minicat_afl" -l -H -p 12345 > "$MT/afl_fuzz.log" 2>&1
grep -E "execs done|unique crashes|unique hangs|LAST UPDATE|map coverage" "$MT/afl_fuzz.log" | tail -5
echo "== [7] ASAN REPLAY =="
Q="$MT/afl_out/default/queue"
if [ -d "$Q" ]; then
  bad=0; n=0
  for f in "$Q"/*; do
    [ -f "$f" ] || continue
    n=$((n+1))
    ASAN_OPTIONS=detect_leaks=0 timeout 10 env MINICAT_FUZZ_FILE="$f" "$MT/minicat_asan" -l -H -p 12345 >/dev/null 2>"$MT/replay_err.txt"
    rc=$?
    if [ $rc -ne 0 ]; then bad=$((bad+1)); echo "BAD: $f rc=$rc"; cat "$MT/replay_err.txt"; fi
  done
  echo "ASAN replay: $n inputs, bad=$bad"
else
  echo "ASAN replay: no queue (afl smoke produced none)"
fi
echo "FINAL_VALIDATION_DONE"

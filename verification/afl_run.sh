#!/bin/bash
# MINICAT v2.0.1 — AFL via MINICAT_FUZZ_FILE hook (forkserver speed)
cd /tmp/mt
PASS=0; FAIL=0

afl-clang-fast -Wall -Wextra -O2 -o /tmp/mt/minicat_afl /tmp/mt/minicat.c || { echo "[FAIL] afl build"; exit 1; }

mkdir -p afl_seeds_http afl_seeds_ws
printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' > afl_seeds_http/req1
printf 'GET /a?b=1 HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n' > afl_seeds_http/req2
printf 'HEAD / HTTP/1.1\r\n\r\n' > afl_seeds_http/req3
printf 'POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello' > afl_seeds_http/req4
printf 'GET /%s HTTP/1.1\r\n\r\n' "$(printf 'A%.0s' {1..300})" > afl_seeds_http/req5

KEY=$(printf '0123456789abcdef' | base64)
printf 'GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' "$KEY" > afl_seeds_ws/hs1
printf '\x81\x02HI' > afl_seeds_ws/frame1
printf '\x81\x81\x00\x00\x00\x00HI' > afl_seeds_ws/frame2
printf '\x89\x00' > afl_seeds_ws/ping
printf '\x01\x84\x00\x00\x00\x00FRAG' > afl_seeds_ws/frag

run_campaign() {
  local NAME="$1" SEEDS="$2" OUT="$3"
  rm -rf "$OUT"
  AFL_SKIP_CPUFREQ=1 AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_NO_UI=1 \
    AFL_SKIP_BIN_CHECK=1 \
    timeout 100 afl-fuzz -i "$SEEDS" -o "$OUT" -V 90 -m none \
    -- env MINICAT_FUZZ_FILE=@@ ./minicat_afl -l -p 18601 -H -k >"$OUT.log" 2>&1
  local S C H
  S=$(grep -E "execs_done|paths_total|unique_crashes|unique_hangs|execs_per_sec|edges_found|total_edges" "$OUT/default/fuzzer_stats" 2>/dev/null | tr '\n' ' ')
  C=$(ls "$OUT/default/crashes/" 2>/dev/null | grep -v README | wc -l)
  H=$(ls "$OUT/default/hangs/" 2>/dev/null | grep -v README | wc -l)
  echo "  $NAME: $S"
  echo "  $NAME crashes: $C, hangs: $H"
  if [ "$C" -eq 0 ]; then echo "  [PASS] $NAME: no crashes"; PASS=$((PASS+1)); else echo "  [FAIL] $NAME: $C crashes!"; FAIL=$((FAIL+1)); fi
  if [ "$H" -eq 0 ]; then echo "  [PASS] $NAME: no hangs"; PASS=$((PASS+1)); else echo "  [FAIL] $NAME: $H hangs"; FAIL=$((FAIL+1)); fi
}

echo "== A1. AFL HTTP (90s, forkserver) =="
run_campaign "A1 HTTP" afl_seeds_http afl_out_http

echo "== A2. AFL WS (90s, forkserver) =="
run_campaign "A2 WS" afl_seeds_ws afl_out_ws

echo "== A3. ASAN replay of full queue =="
BAD=0; N=0
for d in afl_out_http afl_out_ws; do
  for f in "$d"/default/queue/*; do
    [ -f "$f" ] || continue
    N=$((N+1))
    ASAN_OPTIONS=detect_leaks=0 env MINICAT_FUZZ_FILE="$f" ./minicat_asan -l -p 18602 -H -k >/tmp/mt/replay_srv.log 2>&1
    RC=$?
    if [ $RC -ne 0 ]; then echo "  [FAIL] replay $f: rc=$RC"; BAD=$((BAD+1)); fi
    if grep -qE "ERROR: AddressSanitizer|runtime error:" /tmp/mt/replay_srv.log 2>/dev/null; then
      echo "  [FAIL] replay $f: ASAN error"
      grep -E "ERROR: AddressSanitizer|runtime error:" /tmp/mt/replay_srv.log | head -3
      BAD=$((BAD+1))
    fi
  done
done
echo "  replayed $N queue inputs"
if [ "$BAD" -eq 0 ]; then echo "  [PASS] A3 ASAN replay: all clean"; PASS=$((PASS+1)); else echo "  [FAIL] A3 ASAN replay: $BAD bad"; FAIL=$((FAIL+1)); fi

echo "========================================="
echo "AFL RESULT: $PASS passed, $FAIL failed"
exit $FAIL

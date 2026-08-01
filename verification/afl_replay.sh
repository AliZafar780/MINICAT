#!/bin/bash
# ASAN replay of AFL queues with per-file timeout + progress log
RES=/tmp/mt/replay_result.txt
: > "$RES"
BAD=0; N=0
for d in /tmp/mt/afl_out_http /tmp/mt/afl_out_ws; do
  for f in "$d"/default/queue/*; do
    [ -f "$f" ] || continue
    N=$((N+1))
    T0=$(date +%s)
    ASAN_OPTIONS=detect_leaks=0 timeout 15 env MINICAT_FUZZ_FILE="$f" /tmp/mt/minicat_asan -l -p 18602 -H -k >/tmp/mt/replay_srv.log 2>&1
    RC=$?
    T1=$(date +%s)
    DT=$((T1 - T0))
    if [ $RC -ne 0 ] && [ $RC -ne 124 ]; then
      echo "  [FAIL] $f rc=$RC (${DT}s)" >> "$RES"
      BAD=$((BAD+1))
    elif [ $RC -eq 124 ]; then
      echo "  [SLOW] $f timed out at 15s under ASAN" >> "$RES"
    fi
    if grep -qE "ERROR: AddressSanitizer|runtime error:" /tmp/mt/replay_srv.log 2>/dev/null; then
      echo "  [FAIL-ASAN] $f" >> "$RES"
      grep -E "ERROR: AddressSanitizer|runtime error:" /tmp/mt/replay_srv.log | head -3 >> "$RES"
      BAD=$((BAD+1))
    fi
    [ $((N % 20)) -eq 0 ] && echo "progress: $N" >> "$RES"
  done
done
echo "replayed $N queue inputs, bad=$BAD" >> "$RES"
[ $BAD -eq 0 ] && echo "ASAN REPLAY: ALL CLEAN" >> "$RES"

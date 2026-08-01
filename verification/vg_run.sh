#!/bin/bash
# MINICAT v2.0.1 — valgrind pass (independent memory checker)
# Exercises every major path under valgrind:
#   --leak-check=full --errors-for-leak-kinds=definite --error-exitcode=42
# Usage: bash vg_run.sh [path-to-minicat]
BIN="${1:-./minicat}"
LOGDIR="${MINICAT_LOGDIR:-/tmp/mt}"
mkdir -p "$LOGDIR"
VG="valgrind --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite --error-exitcode=42 --trace-children=yes"
PASS=0; FAIL=0; FAILED=()

check() {
  local NAME="$1" LOG="$2" RC="$3"
  if [ $RC -eq 0 ] && grep -q "ERROR SUMMARY: 0 errors" "$LOG"; then
    echo "  [PASS] $NAME: valgrind clean (0 errors)"
    PASS=$((PASS+1))
  else
    echo "  [FAIL] $NAME: rc=$RC"
    grep -E "ERROR SUMMARY|definitely lost|Invalid " "$LOG" | head -5
    FAIL=$((FAIL+1)); FAILED+=("$NAME")
  fi
}

echo "== V1. HTTP server (GET/HEAD/404/414/keep-alive) =="
P=18301
$VG "$BIN" -l -p $P -H -k >"$LOGDIR/vg_http.log" 2>&1 &
SP=$!
sleep 1
for i in 1 2 3; do
  printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; cat <&3" >/dev/null 2>&1
done
printf 'GET /nope HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; cat <&3" >/dev/null 2>&1
LONG=$(printf 'A%.0s' $(seq 1 4000))
printf "GET /%s HTTP/1.1\r\nHost: x\r\n\r\n" "$LONG" | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; cat <&3" >/dev/null 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V1 HTTP" "$LOGDIR/vg_http.log" $?

echo "== V2. WebSocket (handshake+echo+ping+frag+close) =="
P=18302
$VG "$BIN" -l -p $P -W >"$LOGDIR/vg_ws.log" 2>&1 &
SP=$!
sleep 1
LOGDIR="$LOGDIR" python3 - <<'PY' 2>/dev/null
import os, socket, base64
s = socket.create_connection(("127.0.0.1", 18302), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
m = b"\x01\x02\x03\x04"
def mk(b0, b1, mask, payload):
    return bytes([b0, b1]) + mask + bytes(x ^ mask[i%4] for i, x in enumerate(payload))
s.sendall(mk(0x81, 0x81, m, b"HI"))
s.sendall(mk(0x89, 0x81, m, b"P"))
s.sendall(mk(0x01, 0x84, m, b"FRAG"))
s.sendall(mk(0x80, 0x84, m, b"MENT"))
s.sendall(mk(0x88, 0x80, m, b""))
s.settimeout(1)
try:
    while s.recv(4096): pass
except Exception: pass
s.close()
PY
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V2 WS" "$LOGDIR/vg_ws.log" $?

echo "== V3. Exec mode =="
P=18303
$VG "$BIN" -l -p $P -e 'echo EXECVG' >"$LOGDIR/vg_exec.log" 2>&1 &
SP=$!
sleep 1
printf 'GO' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; cat <&3" >/dev/null 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V3 exec" "$LOGDIR/vg_exec.log" $?

echo "== V4. Relay echo + client (half-close) =="
P=18304
$VG "$BIN" -l -p $P -x >"$LOGDIR/vg_relay.log" 2>&1 &
SP=$!
sleep 1
printf 'VGDATA' | timeout 5 "$BIN" 127.0.0.1 $P >/dev/null 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V4 relay+client" "$LOGDIR/vg_relay.log" $?

echo "== V5. UDP server + client =="
P=18305
$VG "$BIN" -l -p $P -u -e 'echo UDPVG' >"$LOGDIR/vg_udp.log" 2>&1 &
SP=$!
sleep 1
printf 'x' | timeout 5 "$BIN" -u 127.0.0.1 $P >/dev/null 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V5 UDP" "$LOGDIR/vg_udp.log" $?

echo "== V6. Chat broadcast (2 clients) =="
P=18306
$VG "$BIN" -l -p $P -K >"$LOGDIR/vg_chat.log" 2>&1 &
SP=$!
sleep 1
( printf 'HELLO' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; sleep 1" >/dev/null 2>&1 ) &
sleep 0.3
printf 'WORLD' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; sleep 1" >/dev/null 2>&1
sleep 2
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V6 chat" "$LOGDIR/vg_chat.log" $?

echo "== V7. Fork mode (12 clients) =="
P=18307
$VG "$BIN" -l -p $P -F >"$LOGDIR/vg_fork.log" 2>&1 &
SP=$!
sleep 1
for i in $(seq 1 12); do
  printf "F$i" | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; cat >&3; exec 3<&-" >/dev/null 2>&1 &
done
sleep 4
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V7 fork" "$LOGDIR/vg_fork.log" $?

echo "== V8. Proxy mode =="
P=18308
$VG "$BIN" -l -p $P -P >"$LOGDIR/vg_proxy.log" 2>&1 &
SP=$!
sleep 1
timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/$P; printf 'GET http://example.com/ HTTP/1.1\r\nHost: example.com\r\n\r\n' >&3; cat <&3" >/dev/null 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null
check "V8 proxy" "$LOGDIR/vg_proxy.log" $?

echo "== V9. Client conn-refused + EOF =="
timeout 5 $VG "$BIN" 127.0.0.1 18399 </dev/null >"$LOGDIR/vg_client.log" 2>&1
RC=$?
# Client exits non-zero (255) on ECONNREFUSED by design; valgrind must
# still report 0 errors (and not its --error-exitcode 42).
if [ $RC -ne 42 ] && grep -q "ERROR SUMMARY: 0 errors" "$LOGDIR/vg_client.log"; then
  echo "  [PASS] V9 client refused+EOF: valgrind clean"
  PASS=$((PASS+1))
else
  echo "  [FAIL] V9 client: rc=$RC"
  grep -E "ERROR SUMMARY|definitely lost|Invalid " "$LOGDIR/vg_client.log" | head -5
  FAIL=$((FAIL+1)); FAILED+=(V9)
fi

echo "========================================="
echo "VALGRIND RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || echo "FAILED: ${FAILED[*]}"
exit $FAIL

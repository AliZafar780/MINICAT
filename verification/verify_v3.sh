#!/bin/bash
# MINICAT v2.0.1 — Extended edge-case battery (E1-E24)
# Usage: bash verify_v3.sh [path-to-minicat]
BIN="${1:-./minicat}"
LOGDIR="${MINICAT_LOGDIR:-/tmp/mt}"
mkdir -p "$LOGDIR"
PASS=0; FAIL=0; FAILED=()

ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1 - $2"; FAIL=$((FAIL+1)); FAILED+=("$1"); }

start_srv() { # start_srv <name> <port> [args...]
  local NAME="$1" PORT="$2"; shift 2
  # ASAN_OPTIONS=verify_asan_link_order=0 lets ASAN builds coexist with
  # stdbuf's LD_PRELOAD (otherwise "ASan runtime does not come first").
  ASAN_OPTIONS=verify_asan_link_order=0 stdbuf -oL -eL "$BIN" -l -p "$PORT" "$@" >"$LOGDIR/srv_${NAME}.log" 2>&1 &
  SRV_PID=$!
  sleep 1
}
stop_srv() { kill $SRV_PID 2>/dev/null; wait $SRV_PID 2>/dev/null; }

http_get() { # http_get <port> <request-line> [extra headers...]
  local PORT="$1" LINE="$2"; shift 2
  python3 - "$PORT" "$LINE" "$@" <<'PY' 2>/dev/null
import socket, sys
p, line, hdrs = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
s = socket.create_connection(("127.0.0.1", p), timeout=5)
req = line + "\r\n" + "".join(h + "\r\n" for h in hdrs) + "\r\n"
s.sendall(req.encode())
data = b""
try:
    while True:
        x = s.recv(4096)
        if not x: break
        data += x
        if len(data) > 32768: break
except Exception: pass
s.close()
sys.stdout.buffer.write(data)
PY
}

echo "== E1. HTTP keep-alive: two requests on one conn (real headers) =="
if start_srv e1 18201 -H -k; then
  OUT=$(http_get 18201 "GET / HTTP/1.1" "Host: x" "User-Agent: t" "Connection: keep-alive")
  [ "$(echo "$OUT" | grep -c '^HTTP/1.1 200')" -ge 1 ] && ok "E1: 1st 200" || bad "E1 first" "$OUT"
  OUT2=$(http_get 18201 "GET / HTTP/1.1" "Host: x")
  [ "$(echo "$OUT2" | grep -c '^HTTP/1.1 200')" -ge 1 ] && ok "E1: 2nd 200 on new conn" || bad "E1 second" "$OUT2"
  stop_srv
fi

echo "== E2. exec output >64KB: capped, no hang, server alive =="
if start_srv e2 18202 -e 'seq 1 100000'; then
  OUT=$(printf 'GO' | timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/18202; cat >&3; cat <&3" 2>/dev/null | wc -c)
  [ "$OUT" -le 70000 ] && [ "$OUT" -ge 65000 ] && ok "E2: exec >64KB drained (65535 cap)" || bad "E2 cap" "got $OUT"
  printf 'AGAIN' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/18202; cat >&3; cat <&3" >/dev/null 2>&1
  [ $? -ne 124 ] && ok "E2: server alive after big exec" || bad "E2 alive" "server hung"
  stop_srv
fi

echo "== E3. client 1MB push through slow reader (partial writes) =="
if start_srv e3 18203; then
  python3 -c "import sys; sys.stdout.buffer.write(b'A'*1048576)" | timeout 30 "$BIN" 127.0.0.1 18203 >/dev/null 2>&1 &
  CP=$!
  python3 - "$LOGDIR/srv_e3.log" <<'PY' 2>/dev/null
import os, sys, time
path = sys.argv[1]
got = 0; t0 = time.time()
while time.time() - t0 < 25:
    try:
        got = os.path.getsize(path)
    except FileNotFoundError:
        got = 0
    if got >= 1048576: break
    time.sleep(0.2)
print(got)
PY
  wait $CP 2>/dev/null
  [ "$(cat "$LOGDIR/srv_e3.log" 2>/dev/null | wc -c)" -eq 1048576 ] && ok "E3: client 1MB push, all 1048576 bytes received" || bad "E3 relay" "bytes=$(cat "$LOGDIR/srv_e3.log" 2>/dev/null | wc -c)"
  stop_srv
fi

echo "== E4. WS: unmasked client data frame -> closed =="
if start_srv e4 18204 -W; then
  python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18204), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
s.sendall(b"\x81\x02HI")          # unmasked text frame (RFC6455 violation)
s.settimeout(3)
closed = False
try:
    while True:
        x = s.recv(4096)
        if not x: closed = True; break
except Exception:
    closed = True
print("CLOSED" if closed else "OPEN")
PY
  R=$(python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18204), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
s.sendall(b"\x81\x02HI")
s.settimeout(3)
closed = False
try:
    while True:
        x = s.recv(4096)
        if not x: closed = True; break
except Exception:
    closed = True
print("CLOSED" if closed else "OPEN")
PY
)
  [ "$R" = "CLOSED" ] && ok "E4: unmasked frame -> closed" || bad "E4 unmasked" "got $R"
  stop_srv
fi

echo "== E5. WS: ping -> pong =="
if start_srv e5 18205 -W; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18205), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
s.sendall(bytes.fromhex("890150"))   # ping "P" (unmasked control, lenient)
s.settimeout(2)
out = b""
try:
    while len(out) < 3:
        x = s.recv(4096)
        if not x: break
        out += x
except Exception: pass
print(out.hex())
PY
)
  [ "$R" = "8a0150" ] && ok "E5: ping->pong (8a0150)" || bad "E5 pong" "got $R"
  stop_srv
fi

echo "== E6. WS: fragmented message FRAG+MENT reassembled =="
if start_srv e6 18206 -W; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18206), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
m1 = b"\x01\x00\x00\x00"; m2 = b"\x00\x00\x00\x00"
def mk(b0, b1, mask, payload):
    return bytes([b0, b1]) + mask + bytes(x ^ mask[i%4] for i, x in enumerate(payload))
s.sendall(mk(0x01, 0x80|4, m1, b"FRAG"))           # text, FIN=0, 4 bytes
s.sendall(mk(0x80, 0x80|4, m2, b"MENT"))           # continuation, FIN=1
s.settimeout(2)
out = b""
try:
    while len(out) < 10:
        x = s.recv(4096)
        if not x: break
        out += x
except Exception: pass
print(out.hex())
PY
)
  [ "$R" = "8108465241474d454e54" ] && ok "E6: fragmented message reassembled (FRAGMENT)" || bad "E6 fragmentation" "got $R"
  stop_srv
fi

echo "== E7. WS: orphan continuation -> closed =="
if start_srv e7 18207 -W; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18207), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
m = b"\x01\x00\x00\x00"
def mk(b0, b1, mask, payload):
    return bytes([b0, b1]) + mask + bytes(x ^ mask[i%4] for i, x in enumerate(payload))
s.sendall(mk(0x80, 0x80|1, m, b"X"))    # continuation with no open fragment
s.settimeout(3)
closed = False
try:
    while True:
        x = s.recv(4096)
        if not x: closed = True; break
except Exception:
    closed = True
print("CLOSED" if closed else "OPEN")
PY
)
  [ "$R" = "CLOSED" ] && ok "E7: orphan continuation -> closed" || bad "E7 orphan cont" "got $R"
  stop_srv
fi

echo "== E8. WS: no Upgrade header -> 400 =="
if start_srv e8 18208 -W; then
  R=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\nSec-WebSocket-Version: 13\r\n\r\n' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/18208; cat >&3; cat <&3" 2>/dev/null | head -1)
  echo "$R" | grep -q '400' && ok "E8: no Upgrade -> 400" || bad "E8 no upgrade" "got $R"
  stop_srv
fi

echo "== E9. WS: wrong version 12 -> 400 =="
if start_srv e9 18209 -W; then
  R=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\nSec-WebSocket-Version: 12\r\n\r\n' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/18209; cat >&3; cat <&3" 2>/dev/null | head -1)
  echo "$R" | grep -q '400' && ok "E9: wrong version -> 400" || bad "E9 wrong version" "got $R"
  stop_srv
fi

echo "== E10. HTTP idle timeout closes slowloris conns =="
if MINICAT_HTTP_IDLE=3 start_srv e10 18210 -H -k; then
  LOGDIR="$LOGDIR" python3 - <<'PY' 2>/dev/null &
import os, socket, sys, time
s = socket.create_connection(("127.0.0.1", 18210), timeout=10)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")   # request, keep alive
d = b""
try:
    while b"\r\n\r\n" not in d:
        x = s.recv(4096)
        if not x: break
        d += x
except Exception: pass
time.sleep(6)                                     # idle past 3s timeout
try:
    x = s.recv(1)
    closed = (x == b"")
except Exception:
    closed = True
s.close()
open(os.environ["LOGDIR"]+"/e10.txt","w").write("1" if closed else "0")
PY
  SLOW_PID=$!
  wait $SLOW_PID 2>/dev/null
  grep -q '^1' $LOGDIR/e10.txt && ok "E10: idle conn closed by timeout" || bad "E10 idle timeout" "conn survived 6s"
  stop_srv
fi

echo "== E11. CLI: bad -p values rejected =="
"$BIN" -l -p 0    >/dev/null 2>&1; [ $? -ne 0 ] && ok "E11: -p 0 rejected" || bad "E11 p0" "accepted"
"$BIN" -l -p 65536 >/dev/null 2>&1; [ $? -ne 0 ] && ok "E11: -p 65536 rejected" || bad "E11 p65536" "accepted"
"$BIN" -l -p abc  >/dev/null 2>&1; [ $? -ne 0 ] && ok "E11: -p abc rejected" || bad "E11 pabc" "accepted"

echo "== E12. CLI: duplicate port rejected =="
"$BIN" -l -p 18212 18212 >/dev/null 2>&1; [ $? -ne 0 ] && ok "E12: duplicate port rejected" || bad "E12 dup" "accepted"

echo "== E13. CLI: client arg strictness =="
"$BIN" 127.0.0.1 >/dev/null 2>&1; [ $? -ne 0 ] && ok "E13: missing port rejected" || bad "E13 noport" "accepted"
"$BIN" 127.0.0.1 18213 extra >/dev/null 2>&1; [ $? -ne 0 ] && ok "E13: extra arg rejected" || bad "E13 extra" "accepted"

echo "== E14. IPv6 ::1 relay =="
if start_srv e14 18214 -x; then
  printf 'HEXV4' | timeout 5 "$BIN" ::1 18214 >/dev/null 2>&1
  sleep 0.5
  R=$(grep -o 'HEX(5)' "$LOGDIR/srv_e14.log" | head -1)
  [ "$R" = "HEX(5)" ] && ok "E14: IPv6 ::1 relay works" || bad "E14 ipv6" "got $R"
  stop_srv
fi

echo "== E15. UDP exec response (client waits for reply) =="
if start_srv e15 18215 -u -e 'echo UDPEXEC'; then
  R=$(printf 'x' | timeout 5 "$BIN" -u 127.0.0.1 18215 2>/dev/null)
  [ "$R" = "UDPEXEC" ] && ok "E15: UDP exec response" || bad "E15 udp exec" "got '$R'"
  stop_srv
fi

echo "== E16. Pipelined 2x200 on one conn =="
if start_srv e16 18216 -H -k; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket
s = socket.create_connection(("127.0.0.1", 18216), timeout=5)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n")
d = b""
try:
    while True:
        x = s.recv(4096)
        if not x: break
        d += x
        if len(d) > 16384: break
except Exception: pass
print(d.count(b"HTTP/1.1 200"))
PY
)
  [ "$R" = "2" ] && ok "E16: pipelined 2x200" || bad "E16 pipeline" "got $R"
  stop_srv
fi

echo "== E17. GET 200 then POST 405 on one conn =="
if start_srv e17 18217 -H -k; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket
s = socket.create_connection(("127.0.0.1", 18217), timeout=5)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
d = b""
while b"HTTP/1.1 200" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
s.sendall(b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
d2 = b""
try:
    while True:
        x = s.recv(4096)
        if not x: break
        d2 += x
except Exception: pass
print(b"200" in d and b"405" in d2)
PY
)
  [ "$R" = "True" ] && ok "E17: GET 200 then POST 405 on one conn" || bad "E17 post405" "got $R"
  stop_srv
fi

echo "== E18. 429 then 200 on one keep-alive conn =="
if start_srv e18 18218 -H -k -T 1; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket, time
s = socket.create_connection(("127.0.0.1", 18218), timeout=5)
def req():
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    d = b""
    while b"HTTP/1.1" not in d:
        x = s.recv(4096)
        if not x: break
        d += x
    return d
first = req()                       # consumes token
second = req()                      # 429 (bucket empty)
time.sleep(1.2)                     # refill
third = req()                       # 200 again
print(b"429" in second and b"200" in third)
PY
)
  [ "$R" = "True" ] && ok "E18: 200 then 429 on one keep-alive conn" || bad "E18 429" "got $R"
  stop_srv
fi

echo "== E19. 2MB client relay MD5 match =="
if start_srv e19 18219; then
  python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256))*8192)" >"$LOGDIR/e19_in.bin"
  timeout 30 "$BIN" 127.0.0.1 18219 <"$LOGDIR/e19_in.bin" >/dev/null 2>&1
  sleep 2
  IN_MD5=$(md5sum <"$LOGDIR/e19_in.bin" | cut -d' ' -f1)
  OUT_MD5=$(md5sum <"$LOGDIR/srv_e19.log" | cut -d' ' -f1)
  stop_srv
  [ "$IN_MD5" = "$OUT_MD5" ] && ok "E19: 2MB client relay md5 match" || bad "E19 md5" "$IN_MD5 != $OUT_MD5"
fi

echo "== E20. WS: oversized control frame (126) -> closed =="
if start_srv e20 18220 -W; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18220), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
s.sendall(bytes.fromhex("89 7e 00 7e") + b"A"*126)   # ping len 126
s.settimeout(3)
closed = False
try:
    while True:
        x = s.recv(4096)
        if not x: closed = True; break
except Exception:
    closed = True
print("CLOSED" if closed else "OPEN")
PY
)
  [ "$R" = "CLOSED" ] && ok "E20: oversized control frame -> closed" || bad "E20 big ping" "got $R"
  stop_srv
fi

echo "== E21. WS: two echoes on one conn =="
if start_srv e21 18221 -W; then
  R=$(python3 - <<'PY' 2>/dev/null
import socket, base64
s = socket.create_connection(("127.0.0.1", 18221), timeout=3)
key = base64.b64encode(b"0123456789abcdef").decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
d = b""
while b"\r\n\r\n" not in d:
    x = s.recv(4096)
    if not x: break
    d += x
m = b"\x01\x00\x00\x00"
def mk(b0, b1, mask, payload):
    return bytes([b0, b1]) + mask + bytes(x ^ mask[i%4] for i, x in enumerate(payload))
s.sendall(mk(0x81, 0x81, m, b"A"))
s.sendall(mk(0x81, 0x81, m, b"B"))
s.settimeout(2)
out = b""
try:
    while len(out) < 8:
        x = s.recv(4096)
        if not x: break
        out += x
except Exception: pass
print(out.hex())
PY
)
  [ "$R" = "810141810142" ] && ok "E21: two echoes on one conn" || bad "E21 echoes" "got $R"
  stop_srv
fi

echo "== E22. HTTP: 4000-char URI -> 414 =="
if start_srv e22 18222 -H -k; then
  LONG=$(python3 -c "print('A'*4000, end='')")
  R=$(printf "GET /%s HTTP/1.1\r\nHost: x\r\n\r\n" "$LONG" | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/18222; cat >&3; cat <&3" 2>/dev/null | head -1)
  echo "$R" | grep -q '414' && ok "E22: 4000-char URI -> 414" || bad "E22 414" "got $R"
  stop_srv
fi

echo "== E23. exec: all shell metachars rejected =="
META=('$(id)' '`id`' '${x}' 'a|b' 'a&b' 'a;b' 'a>b' 'a<b' 'a*b' 'a?b' 'a[b]' 'a{b}' 'a(b)' 'a"b' "a'b" 'a$b' 'a~b' 'a!b' 'a#b')
for m in "${META[@]}"; do
  "$BIN" -l -e "echo $m" >/dev/null 2>&1
  if [ $? -ne 0 ]; then
    ok "E23: rejected: echo $m"
  else
    bad "E23 accepted" "echo $m"
  fi
done

echo "== E24. SIGTERM exits cleanly =="
start_srv e24 18224 -H
kill -TERM $SRV_PID
wait $SRV_PID 2>/dev/null
[ $? -eq 0 ] && ok "E24: SIGTERM exits cleanly" || bad "E24 sigterm" "rc=$?"

echo "========================================="
echo "RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] || echo "FAILED: ${FAILED[*]}"
exit $FAIL

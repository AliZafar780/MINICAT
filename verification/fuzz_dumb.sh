#!/bin/bash
# MINICAT v2.0.1 — dumb fuzzing v2: fire-and-forget, ASAN build
cd /tmp/mt
LOGDIR=/tmp/mt
PASS=0; FAIL=0

check_alive() {
  if kill -0 "$2" 2>/dev/null; then
    echo "  [PASS] $1: server survived"
    PASS=$((PASS+1))
  else
    echo "  [FAIL] $1: server DIED"
    FAIL=$((FAIL+1))
    wait "$2" 2>/dev/null
    echo "  exit: $?"
  fi
}
check_asan() {
  if grep -qE "ERROR: AddressSanitizer|runtime error:" "$2" 2>/dev/null; then
    echo "  [FAIL] $1: ASAN/UBSAN error found"
    grep -E "ERROR: AddressSanitizer|runtime error:" "$2" | head -3
    FAIL=$((FAIL+1))
  else
    echo "  [PASS] $1: no ASAN/UBSAN errors"
    PASS=$((PASS+1))
  fi
}

echo "== F1. HTTP parser fuzz (20,000 payloads, fire-and-forget) =="
ASAN_OPTIONS=detect_leaks=0 ./minicat_asan -l -p 18501 -H -k >"$LOGDIR/fz_http.log" 2>&1 &
SP=$!
sleep 1
python3 - "$LOGDIR" <<'PY'
import os, socket, random, sys
random.seed(0xC0FFEE)
seeds = [
    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /a?b=c HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    b"HEAD / HTTP/1.1\r\n\r\n",
    b"POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
    b"GET /" + b"A"*400 + b" HTTP/1.1\r\n\r\n",
    b"\r\n\r\nGET / HTTP/1.1\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: " + b"B"*500 + b"\r\n\r\n",
]
def mutate(b):
    b = bytearray(b)
    for _ in range(random.randint(1, 8)):
        op = random.randint(0, 4)
        if op == 0 and b: b[random.randrange(len(b))] = random.randrange(256)
        elif op == 1: b.insert(random.randrange(len(b)+1), random.randrange(256))
        elif op == 2 and b: del b[random.randrange(len(b))]
        elif op == 3: b = b + bytes(random.randrange(256) for _ in range(random.randint(1, 64)))
        else:
            b = bytearray(bytes(random.randrange(256) for _ in range(random.randint(1, 128))))
    return bytes(b)
n = 20000
for i in range(n):
    p = seeds[random.randrange(len(seeds))]
    if random.random() < 0.7: p = mutate(p)
    try:
        s = socket.create_connection(("127.0.0.1", 18501), timeout=1)
        s.sendall(p)
        s.close()
    except Exception:
        pass
    if i % 5000 == 0: print(f"  fuzz {i}/{n}")
print("F1 done")
PY
check_alive "F1 HTTP survive" $SP
sleep 0.5
check_asan "F1 HTTP asan" "$LOGDIR/fz_http.log"
kill $SP 2>/dev/null; wait $SP 2>/dev/null

echo "== F2. WebSocket parser fuzz (10,000 payloads) =="
ASAN_OPTIONS=detect_leaks=0 ./minicat_asan -l -p 18502 -W >"$LOGDIR/fz_ws.log" 2>&1 &
SP=$!
sleep 1
python3 - "$LOGDIR" <<'PY'
import os, socket, random, sys, base64
random.seed(0xDEADBEEF)
key = base64.b64encode(b"0123456789abcdef").decode()
hs = (f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode()
seeds = [
    b"\x81\x02HI", b"\x81\x81\x00\x00\x00\x00HI", b"\x89\x00",
    b"\x01\x84\x00\x00\x00\x00FRAG", b"\x80\x81\x00\x00\x00\x00X",
    b"\x88\x00", b"\x8a\x00", b"\x81\x7e\x00\x7e" + b"A"*126,
    b"\x81\x7f" + b"\x00"*8 + b"PAD",
    b"\x00\x81\x00\x00\x00\x00X", b"\x82\x81\x00\x00\x00\x00B",
    b"\xff\x00", b"\x81\xfe\x00\x01", b"\x81",
]
def mutate(b):
    b = bytearray(b)
    for _ in range(random.randint(1, 6)):
        op = random.randint(0, 3)
        if op == 0 and b: b[random.randrange(len(b))] = random.randrange(256)
        elif op == 1: b.insert(random.randrange(len(b)+1), random.randrange(256))
        elif op == 2 and b: del b[random.randrange(len(b))]
        else: b = b + bytes(random.randrange(256) for _ in range(random.randint(1, 96)))
    return bytes(b)
for i in range(10000):
    p = seeds[random.randrange(len(seeds))]
    if random.random() < 0.7: p = mutate(p)
    try:
        s = socket.create_connection(("127.0.0.1", 18502), timeout=1)
        s.sendall(hs)
        s.sendall(p)
        s.close()
    except Exception:
        pass
    if i % 2500 == 0: print(f"  fuzz {i}/10000")
print("F2 done")
PY
check_alive "F2 WS survive" $SP
sleep 0.5
check_asan "F2 WS asan" "$LOGDIR/fz_ws.log"
kill $SP 2>/dev/null; wait $SP 2>/dev/null

echo "== F3. Exec mode fuzz (5,000 payloads) =="
ASAN_OPTIONS=detect_leaks=0 ./minicat_asan -l -p 18503 -e 'echo FUZZ' >"$LOGDIR/fz_exec.log" 2>&1 &
SP=$!
sleep 1
python3 - "$LOGDIR" <<'PY'
import socket, random, sys
random.seed(0xFEED)
for i in range(5000):
    n = random.randint(0, 512)
    p = bytes(random.randrange(256) for _ in range(n))
    try:
        s = socket.create_connection(("127.0.0.1", 18503), timeout=1)
        s.sendall(p)
        s.close()
    except Exception:
        pass
    if i % 1000 == 0: print(f"  fuzz {i}/5000")
print("F3 done")
PY
check_alive "F3 exec survive" $SP
sleep 0.5
check_asan "F3 exec asan" "$LOGDIR/fz_exec.log"
kill $SP 2>/dev/null; wait $SP 2>/dev/null

echo "== F4. UDP exec fuzz (5,000 datagrams) =="
ASAN_OPTIONS=detect_leaks=0 ./minicat_asan -l -p 18504 -u -e 'echo UFUZZ' >"$LOGDIR/fz_udp.log" 2>&1 &
SP=$!
sleep 1
python3 - "$LOGDIR" <<'PY'
import socket, random, sys
random.seed(0xABCD)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(5000):
    n = random.randint(0, 1400)
    p = bytes(random.randrange(256) for _ in range(n))
    try:
        s.sendto(p, ("127.0.0.1", 18504))
    except Exception:
        pass
    if i % 1000 == 0: print(f"  fuzz {i}/5000")
print("F4 done")
PY
check_alive "F4 UDP survive" $SP
sleep 0.5
check_asan "F4 UDP asan" "$LOGDIR/fz_udp.log"
kill $SP 2>/dev/null; wait $SP 2>/dev/null

echo "========================================="
echo "FUZZ RESULT: $PASS passed, $FAIL failed"
exit $FAIL

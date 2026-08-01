#!/bin/bash
# MINICAT v2.0.1 — load & stress: concurrency, fork storm, chat, throughput, UDP, memory
cd /tmp/mt
ulimit -n 65535
PASS=0; FAIL=0
BIN=/tmp/mt/minicat

rss_kb() { grep VmRSS /proc/$1/status 2>/dev/null | awk '{print $2}'; }

echo "== L1. 1,000 concurrent keep-alive HTTP connections (5 reqs each) =="
$BIN -l -p 18701 -H -k >/dev/null 2>&1 &
SP=$!
sleep 0.5
R0=$(rss_kb $SP)
python3 - <<'PY'
import socket, threading, sys
ok = [0]
err = [0]
def worker():
    try:
        s = socket.create_connection(("127.0.0.1", 18701), timeout=10)
        for _ in range(5):
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk: break
                data += chunk
            if b"200" in data.split(b"\r\n")[0]:
                ok[0] += 1
            else:
                err[0] += 1
        s.close()
    except Exception:
        err[0] += 1
ts = [threading.Thread(target=worker) for _ in range(1000)]
for t in ts: t.start()
for t in ts: t.join()
print(f"OK={ok[0]} ERR={err[0]}")
PY
sleep 1
R1=$(rss_kb $SP)
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo "  RSS before=${R0}KB after=${R1}KB"

echo "== L2. 10,000 sequential HTTP connections (bursts of 500) =="
$BIN -l -p 18702 -H -k >/dev/null 2>&1 &
SP=$!
sleep 0.5
OK=0; BAD=0
for b in $(seq 1 20); do
  python3 - <<'PY' &
import socket
for _ in range(500):
    try:
        s = socket.create_connection(("127.0.0.1", 18702), timeout=5)
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        d = b""
        while b"\r\n\r\n" not in d:
            c = s.recv(4096)
            if not c: break
            d += c
        s.close()
    except Exception:
        pass
PY
  wait $!
done
echo "  sent 10000 connections"
kill $SP 2>/dev/null; wait $SP 2>/dev/null

echo "== L3. Fork storm: -F mode, 300 rapid connections =="
$BIN -l -p 18703 -F >/tmp/mt/l3_srv.log 2>&1 &
SP=$!
sleep 0.5
python3 - <<'PY'
import socket
for i in range(300):
    try:
        s = socket.create_connection(("127.0.0.1", 18703), timeout=3)
        s.sendall(b"FORKMARK-%d" % i)
        s.close()
    except Exception:
        pass
print("sent 300")
PY
sleep 2
M=$(grep -o "FORKMARK-" /tmp/mt/l3_srv.log | wc -l)
Z=$(ps -eo stat | grep -c Z)
A=$(kill -0 $SP 2>/dev/null && echo yes || echo no)
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo "  markers relayed: $M/300, zombies: $Z, alive: $A"
if [ "$M" -eq 300 ] && [ "$Z" -eq 0 ]; then echo "  [PASS] L3 fork storm"; PASS=$((PASS+1)); else echo "  [FAIL] L3 fork storm"; FAIL=$((FAIL+1)); fi

echo "== L4. Chat: 50 clients, broadcast storm (join barrier) =="
$BIN -l -p 18704 -K >/dev/null 2>&1 &
SP=$!
sleep 0.5
python3 - <<'PY'
import socket, threading, time
MSGS = 3
N = 50
counts = [0]*N
barrier = threading.Barrier(N)
def client(i):
    try:
        s = socket.create_connection(("127.0.0.1", 18704), timeout=5)
        s.settimeout(5)
        barrier.wait()          # all joined before any message
        for m in range(MSGS):
            s.sendall(b"msg %d from %d" % (m, i))
        data = b""
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                d = s.recv(65536)
                if not d: break
                data += d
            except socket.timeout:
                break
        counts[i] = data.count(b"msg ")
        s.close()
    except Exception:
        counts[i] = -1
ts = [threading.Thread(target=client, args=(i,)) for i in range(N)]
for t in ts: t.start()
for t in ts: t.join()
print(f"total_msg_occurrences={sum(counts)}")
print(f"min_client_count={min(counts)}")
PY
sleep 0.5
kill $SP 2>/dev/null; wait $SP 2>/dev/null
L4T=$(grep -o "total_msg_occurrences=[0-9]*" /tmp/mt/load_result.txt | tail -1 | cut -d= -f2)
L4M=$(grep -o "min_client_count=[0-9-]*" /tmp/mt/load_result.txt | tail -1 | cut -d= -f2)
echo "  message occurrences: $L4T (expect 7350), worst client: $L4M (expect 147)"
if [ "$L4T" = "7350" ] && [ "$L4M" = "147" ]; then echo "  [PASS] L4 chat storm"; PASS=$((PASS+1)); else echo "  [FAIL] L4 chat storm (got $L4T / $L4M)"; FAIL=$((FAIL+1)); fi

echo "== L5. Relay throughput: 10 MB through server (client->server stdout) =="
$BIN -l -p 18705 >/tmp/mt/l5_srv.log 2>&1 &
SP=$!
sleep 0.5
python3 - <<'PY'
import socket, time
payload = b"R" * (1024*1024)
s = socket.create_connection(("127.0.0.1", 18705), timeout=30)
t0 = time.time()
for _ in range(10):
    s.sendall(payload)
s.close()
dt = time.time() - t0
print(f"10MB sent in {dt:.2f}s")
PY
sleep 1
L5B=$(wc -c < /tmp/mt/l5_srv.log)
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo "  server received: $L5B bytes (expect 10485760)"
if [ "$L5B" -eq 10485760 ]; then echo "  [PASS] L5 throughput"; PASS=$((PASS+1)); else echo "  [FAIL] L5 throughput"; FAIL=$((FAIL+1)); fi

echo "== L6. UDP: 1,000 datagrams through server =="
$BIN -l -p 18706 -u >/tmp/mt/l6_srv.log 2>&1 &
SP=$!
sleep 0.5
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for i in range(1000):
    p = b"udp-%d" % i
    s.sendto(p, ("127.0.0.1", 18706))
print("sent 1000 datagrams")
PY
sleep 1
L6N=$(grep -o "udp-" /tmp/mt/l6_srv.log | wc -l)
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo "  server received: $L6N/1000 datagrams"
if [ "$L6N" -eq 1000 ]; then echo "  [PASS] L6 UDP"; PASS=$((PASS+1)); else echo "  [FAIL] L6 UDP"; FAIL=$((FAIL+1)); fi

echo "========================================="
echo "LOAD RESULT: $PASS passed, $FAIL failed"
exit $FAIL

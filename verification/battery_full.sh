#!/bin/bash
# MINICAT full regression — single WSL invocation (tmpfs-safe)
# Usage: bash battery_full.sh <win-path-to-src> <win-path-to-scripts-dir>
SRC="$1"; SCRIPTS="$2"
MT=/tmp/mt
mkdir -p "$MT"
cp "$SRC" "$MT/minicat.c"
cp "$SCRIPTS"/*.sh "$MT/" 2>/dev/null
sed -i 's/\r$//' "$MT/minicat.c" "$MT"/*.sh
echo "== BUILD =="
gcc -Wall -Wextra -Wpedantic -O2 -o "$MT/minicat" "$MT/minicat.c" && echo "gcc OK"
clang -Wall -Wextra -Wpedantic -O2 -fsanitize=address,undefined -fno-omit-frame-pointer -o "$MT/minicat_asan" "$MT/minicat.c" && echo "clang-asan OK"
echo "== SMOKE =="
"$MT/minicat" -l -p 18810 -H -k >"$MT/smoke.log" 2>&1 &
SP=$!
sleep 0.7
python3 - "$MT" <<'PY'
import socket, sys
p = 18810
s = socket.create_connection(("127.0.0.1", p), timeout=5)
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
d = b""
try:
    while True:
        x = s.recv(4096)
        if not x: break
        d += x
except Exception: pass
s.close()
print("smoke:", "OK" if b"200 OK" in d else "FAIL", repr(d[:60]))
PY
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo "== BATTERY =="
timeout 280 bash "$MT/verify_v3.sh" "$MT/minicat" > "$MT/battery_result.txt" 2>&1
grep -E "RESULT|FAILED" "$MT/battery_result.txt"
echo "== LOAD =="
timeout 200 bash "$MT/load_test.sh" "$MT/minicat" > "$MT/load_result.txt" 2>&1
grep -E "RESULT|FAIL" "$MT/load_result.txt"
echo "ALL_DONE"

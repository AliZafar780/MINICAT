#!/bin/bash
# MINICAT v2.0.1 — compile matrix: strict flags, clang, FORTIFY, static
cd /tmp/mt
PASS=0; FAIL=0

t() { # t <name> <build-cmd...>
  local NAME="$1"; shift
  if "$@" >/dev/null 2>/tmp/mt/matrix_err.log; then
    W=$(grep -c -i "warning" /tmp/mt/matrix_err.log 2>/dev/null)
    echo "  [PASS] $NAME (warnings: $W)"
    PASS=$((PASS+1))
  else
    echo "  [FAIL] $NAME"
    head -10 /tmp/mt/matrix_err.log
    FAIL=$((FAIL+1))
  fi
}

echo "== M1. clang -Wall -Wextra -Wpedantic -O2 =="
t "M1 clang strict" clang -Wall -Wextra -Wpedantic -O2 -o /tmp/mt/minicat_clang /tmp/mt/minicat.c

echo "== M2. gcc -Wconversion -Wshadow -Wformat=2 (superset) =="
t "M2 gcc wconversion" gcc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wno-sign-conversion -O2 -o /tmp/mt/minicat_wc /tmp/mt/minicat.c

echo "== M3. gcc -D_FORTIFY_SOURCE=2 -O2 =="
t "M3 gcc fortify" gcc -Wall -Wextra -Wpedantic -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -o /tmp/mt/minicat_fort /tmp/mt/minicat.c

echo "== M4. gcc -static =="
t "M4 gcc static" gcc -Wall -Wextra -Wpedantic -O2 -static -o /tmp/mt/minicat_static /tmp/mt/minicat.c

echo "== M5. clang -fsanitize=address,undefined (quick smoke) =="
t "M5 clang asan+ubsan" clang -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -o /tmp/mt/minicat_asan /tmp/mt/minicat.c

echo "== M6. gcc -O0 (debug) -g =="
t "M6 gcc -O0 -g" gcc -Wall -Wextra -Wpedantic -O0 -g -o /tmp/mt/minicat_O0 /tmp/mt/minicat.c

echo "== Smoke: each build must serve HTTP 200 =="
for B in minicat_clang minicat_wc minicat_fort minicat_static minicat_O0; do
  ./$B -l -p 18401 -H -k >/dev/null 2>&1 &
  SP=$!; sleep 0.7
  R=$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/18401; cat >&3; cat <&3" 2>/dev/null | head -1)
  kill $SP 2>/dev/null; wait $SP 2>/dev/null
  if echo "$R" | grep -q '200'; then echo "  [PASS] smoke $B: 200 OK"; PASS=$((PASS+1)); else echo "  [FAIL] smoke $B: $R"; FAIL=$((FAIL+1)); fi
done
# ASAN build smoke (env needed)
ASAN_OPTIONS=detect_leaks=0 ./minicat_asan -l -p 18402 -H -k >/dev/null 2>&1 &
SP=$!; sleep 0.7
R=$(printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/18402; cat >&3; cat <&3" 2>/dev/null | head -1)
kill $SP 2>/dev/null; wait $SP 2>/dev/null
if echo "$R" | grep -q '200'; then echo "  [PASS] smoke minicat_asan: 200 OK"; PASS=$((PASS+1)); else echo "  [FAIL] smoke minicat_asan: $R"; FAIL=$((FAIL+1)); fi

echo "========================================="
echo "MATRIX RESULT: $PASS passed, $FAIL failed"
exit $FAIL

#!/bin/bash
# SSL end-to-end + ASAN memory-safety verification for MINICAT v2
cd /tmp/mt

echo "=== BUILD ALL ==="
gcc -Wall -Wextra -Wpedantic -O2 -o minicat minicat.c 2>&1 && echo BUILD_OK
gcc -DWITH_SSL -Wall -Wextra -Wpedantic -O2 -o minicat_ssl minicat.c -lssl -lcrypto 2>&1 && echo SSL_BUILD_OK
gcc -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o minicat_asan minicat.c 2>&1 && echo ASAN_BUILD_OK

echo
echo "=== SSL TEST 1: TLS HTTP server (openssl s_client) ==="
stdbuf -oL -eL ./minicat_ssl -l -p 18130 -S -c cert.pem -j key.pem -H -v > ssl_http.log 2>&1 &
SP=$!
sleep 0.5
RESP=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | timeout 5 openssl s_client -connect 127.0.0.1:18130 -quiet -verify_quiet 2>/dev/null)
echo "$RESP" | grep -q '200 OK' && echo "  [PASS] TLS HTTP GET / -> 200" || { echo "  [FAIL] TLS HTTP"; echo "$RESP" | head -5; }
kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null

echo "=== SSL TEST 2: TLS data relay (minicat_ssl client <-> server) ==="
stdbuf -oL -eL ./minicat_ssl -l -p 18131 -S -c cert.pem -j key.pem -x > ssl_relay.log 2>&1 &
SP=$!
sleep 0.5
printf 'SECRET-TLS' | timeout 5 ./minicat_ssl -S 127.0.0.1 18131 >/dev/null 2>&1
sleep 0.5
kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null
grep -q 'HEX(10)' ssl_relay.log && echo "  [PASS] TLS relay: 10 bytes decrypted+relayed" || { echo "  [FAIL] TLS relay"; cat ssl_relay.log; }

echo "=== SSL TEST 3: -S without cert/key -> error ==="
ERR=$(timeout 2 ./minicat_ssl -l -p 18132 -S 2>&1 | head -2)
echo "$ERR" | grep -qi 'cert' && echo "  [PASS] -S missing cert/key -> clean error" || echo "  [FAIL] -S missing cert: $ERR"

echo "=== SSL TEST 4: SSL client with XOR+SSL combined (relay integrity) ==="
stdbuf -oL -eL ./minicat_ssl -l -p 18133 -S -c cert.pem -j key.pem -E -A key123 > ssl_xor.log 2>&1 &
SP=$!
sleep 0.5
printf 'DOUBLELAYER' | timeout 5 ./minicat_ssl -S -E -A key123 127.0.0.1 18133 >/dev/null 2>&1
sleep 0.5
kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null
grep -q 'DOUBLELAYER' ssl_xor.log && echo "  [PASS] TLS+XOR: data relayed intact" || { echo "  [FAIL] TLS+XOR"; cat ssl_xor.log; }

echo
echo "=== ASAN BATTERY (memory safety on all paths) ==="
# NOTE: no LD_PRELOAD needed — the -fsanitize=address binary links its own
# runtime. (LD_PRELOAD here breaks stdbuf: libstdbuf.so lands first in the
# preload list and ASan aborts with "runtime does not come first".)
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 NO_STDBUF=1 bash verify_v2.sh ./minicat_asan 2>&1 | tail -10
echo "--- ASAN error scan (any AddressSanitizer/UndefinedBehavior lines?) ---"
grep -l 'AddressSanitizer\|runtime error\|ERROR:' /tmp/mt/srv_*.log 2>/dev/null && echo "ASAN ERRORS FOUND" || echo "  [PASS] No ASAN/UBSan errors in any server log"

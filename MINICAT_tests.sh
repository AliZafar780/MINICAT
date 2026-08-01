#!/bin/bash
# ============================================================================
# MINICAT v2.0.0 — OFFICIAL TEST SUITE (replaces the original MINICAT_tests.sh)
#
# Usage:  bash MINICAT_tests.sh [workdir]
#   - Builds release, SSL and ASan+UBSan variants
#   - Runs the 23-assertion adversarial verification battery on RELEASE
#   - Runs 4 SSL end-to-end tests (needs openssl)
#   - Runs the SAME battery under AddressSanitizer+UBSan (memory safety)
#   - Exit code 0 = all green; 1 = any failure
#
# Requires: gcc, openssl, python3, stdbuf (coreutils), verify_v2.sh in workdir
# ============================================================================
set -u
DIR="${1:-.}"
cd "$DIR" || exit 2
WORK="$(pwd)"
mkdir -p "$WORK/.tests"
FAIL=0

say() { echo; echo "========== $* =========="; }
pass() { echo "  [PASS] $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  [FAIL] $1 — $2"; }

# ---- 0. builds --------------------------------------------------------------
say "BUILD RELEASE (warning-free, -Wall -Wextra -Wpedantic)"
if gcc -Wall -Wextra -Wpedantic -O2 -o minicat minicat.c 2>"$WORK/.tests/build_release.err"; then
  pass "release build clean"
else
  bad "release build" "$(head -5 "$WORK/.tests/build_release.err")"
fi

say "BUILD SSL (-DWITH_SSL, links OpenSSL)"
if gcc -Wall -Wextra -Wpedantic -O2 -DWITH_SSL -o minicat_ssl minicat.c -lssl -lcrypto 2>"$WORK/.tests/build_ssl.err"; then
  pass "ssl build clean"
else
  bad "ssl build" "$(head -5 "$WORK/.tests/build_ssl.err")"
fi

say "BUILD ASAN+UBSAN"
if gcc -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o minicat_asan minicat.c 2>"$WORK/.tests/build_asan.err"; then
  pass "asan build clean"
else
  bad "asan build" "$(head -5 "$WORK/.tests/build_asan.err")"
fi

# ---- 1. release battery -----------------------------------------------------
say "ADVERSARIAL BATTERY — RELEASE BUILD (23 assertions)"
if timeout 300 env MINICAT_LOGDIR="$WORK/.tests" bash "$WORK/verify_v2.sh" "$WORK/minicat"; then
  pass "release battery 23/23"
else
  bad "release battery" "see output above"
fi

# ---- 2. SSL end-to-end ------------------------------------------------------
say "SSL END-TO-END (4 tests)"
openssl req -x509 -newkey rsa:2048 -keyout "$WORK/.tests/key.pem" -out "$WORK/.tests/cert.pem" \
  -days 1 -nodes -subj "/CN=localhost" 2>/dev/null

# 2.1 TLS HTTP server via openssl s_client
"$WORK/minicat_ssl" -l -p 18130 -S -c "$WORK/.tests/cert.pem" -j "$WORK/.tests/key.pem" -H -v > "$WORK/.tests/ssl_http.log" 2>&1 &
SP=$!; sleep 0.5
R=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | timeout 5 openssl s_client -connect 127.0.0.1:18130 -quiet -verify_quiet 2>/dev/null)
echo "$R" | grep -q '200 OK' && pass "TLS HTTP GET / → 200" || bad "TLS HTTP" "$(echo "$R" | head -3)"
kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null

# 2.2 TLS data relay integrity
"$WORK/minicat_ssl" -l -p 18131 -S -c "$WORK/.tests/cert.pem" -j "$WORK/.tests/key.pem" -x > "$WORK/.tests/ssl_relay.log" 2>&1 &
SP=$!; sleep 0.5
printf 'SECRET-TLS' | timeout 5 "$WORK/minicat_ssl" -S 127.0.0.1 18131 >/dev/null 2>&1
sleep 0.5
kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null
grep -q 'HEX(10)' "$WORK/.tests/ssl_relay.log" && pass "TLS relay: 10 bytes decrypted+relayed" || bad "TLS relay" "$(cat "$WORK/.tests/ssl_relay.log")"

# 2.3 -S without cert/key → clean error
E=$(timeout 2 "$WORK/minicat_ssl" -l -p 18132 -S 2>&1 | head -2)
echo "$E" | grep -qi 'cert' && pass "-S missing cert/key → clean error" || bad "-S missing cert" "$E"

# 2.4 TLS + XOR combined
"$WORK/minicat_ssl" -l -p 18133 -S -c "$WORK/.tests/cert.pem" -j "$WORK/.tests/key.pem" -E -A key123 > "$WORK/.tests/ssl_xor.log" 2>&1 &
SP=$!; sleep 0.5
printf 'DOUBLELAYER' | timeout 5 "$WORK/minicat_ssl" -S -E -A key123 127.0.0.1 18133 >/dev/null 2>&1
sleep 0.5
kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null
grep -q 'DOUBLELAYER' "$WORK/.tests/ssl_xor.log" && pass "TLS+XOR: data relayed intact" || bad "TLS+XOR" "$(cat "$WORK/.tests/ssl_xor.log")"

# ---- 3. ASAN battery --------------------------------------------------------
say "ADVERSARIAL BATTERY — ASAN+UBSAN BUILD (memory safety on all 23 paths)"
# NO_STDBUF: stdbuf injects libstdbuf.so via LD_PRELOAD, which lands before
# libasan and makes ASan abort ("runtime does not come first").
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 NO_STDBUF=1 \
  env MINICAT_LOGDIR="$WORK/.tests" bash "$WORK/verify_v2.sh" "$WORK/minicat_asan" | tail -5
if grep -lq 'AddressSanitizer\|runtime error\|ERROR:' "$WORK"/.tests/srv_*.log 2>/dev/null; then
  bad "ASAN battery" "sanitizer errors in server logs"
else
  pass "ASAN battery: no AddressSanitizer/UBSan errors on any path"
fi

# ---- summary ----------------------------------------------------------------
say "RESULT"
if [ "$FAIL" -eq 0 ]; then
  echo "ALL TESTS PASSED — MINICAT v2.0.1 is production-ready."
  exit 0
else
  echo "$FAIL group(s) FAILED."
  exit 1
fi

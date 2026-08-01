/*
 * MINICAT v2.0.1 — Production Hardened
 * Original author: Ali Zafar (v1.0.x)
 *
 * Lightweight zero-dependency network tool. Single file, no external libs
 * (optional OpenSSL via -DWITH_SSL).
 *
 * v2.0.1 CHANGES (2026-08-01, post-v2.0.0 verification round):
 *  - FIXED:    fork (-F) children were never reaped -> zombie leak under load
 *              (SIGCHLD handler with waitpid(WNOHANG) loop; verified 0 zombies
 *              after 300 rapid connections)
 *  - FIXED:    fork children did not fflush(stdout/stderr) before _exit() ->
 *              buffered relay data could be lost at EOF
 *  - FIXED:    -K chat mode closed the connection after the first message
 *              unless -k was also given; chat is persistent per docs
 *  - TEST-ONLY: env MINICAT_FUZZ_FILE=<path> drives a file through the real
 *              connection_loop() via socketpair (inert when unset; used by
 *              AFL/libFuzzer campaigns; excluded from normal operation)
 *
 * v2.0.0 CHANGES (all previously-discovered issues fixed):
 *  - SECURITY: fixed unbounded sscanf() stack buffer overflow in HTTP parser
 *              (bounded tokens, size checks, 400/414 on malformed input)
 *  - FIXED:    -g and -E flags were missing from getopt() optstring (dead)
 *  - FIXED:    -T rate limiting was parsed but never enforced (token bucket)
 *  - FIXED:    -S SSL was a no-op (real OpenSSL TLS now; clear error when
 *              not compiled with -DWITH_SSL)
 *  - FIXED:    UDP server used accept() on a DGRAM socket (recvfrom loop now)
 *  - FIXED:    UDP client mode ignored -u (real SOCK_DGRAM client now)
 *  - FIXED:    epoll edge-triggered single-read starvation / data loss
 *              (level-triggered epoll on Linux, poll() fallback elsewhere)
 *  - FIXED:    -e exec sanitization bypass via > < * ? etc (strict allowlist)
 *  - FIXED:    exec output truncated at 4095 bytes (drain loop, 64KB cap)
 *  - FIXED:    WebSocket non-compliant (real RFC6455 SHA1+base64 handshake,
 *              proper frame encode/decode, masking, ping/pong, close,
 *              fragmentation)
 *  - FIXED:    -K chat mode was a comment (real broadcast registry)
 *  - FIXED:    -P proxy mode was dead (HTTP CONNECT + absolute-URI forward,
 *              per-connection process model)
 *  - FIXED:    no 404 handling (proper 404/400/405/414/429 status codes)
 *  - FIXED:    HTTP pipelining / partial reads (per-connection buffer)
 *  - FIXED:    deprecated gethostbyname() -> getaddrinfo()
 *  - FIXED:    -F fork per packet -> fork per connection (child blocking loop,
 *              no epoll access in children, _exit() to avoid stdio clobber)
 *  - FIXED:    SIGPIPE kills server on client disconnect (ignored; write
 *              errors handled); EINTR no longer closes connections
 *  - FIXED:    -L log only wrote connections (startup + close + traffic logs)
 *  - FIXED:    warnings: unused vars, signed char frame[0]=0x81, ignored
 *              write() return values
 *  - NEW:      graceful SIGINT/SIGTERM shutdown, XOR key via -A, SSL key via -j
 *  - NEW:      HEAD requests, HTTP keep-alive + pipelining, WS echo/broadcast
 *
 * v2.0.1 CHANGES (deep-verification hardening pass, 2026-08-01):
 *  - FIXED:    HTTP keep-alive parsed header lines as requests (spurious 405);
 *              real mid-headers state machine + consume-through-blank-line
 *  - FIXED:    exec >64KB output deadlocked pclose() (drain + discard)
 *  - FIXED:    WebSocket accepted unmasked client data frames (RFC6455 §5.1);
 *              control frames now require FIN and payload <= 125 (§5.5);
 *              new data frame while a fragment is open is rejected (§5.4)
 *  - FIXED:    WS handshake headers were unvalidated (now requires Upgrade:
 *              websocket, Sec-WebSocket-Key, Sec-WebSocket-Version: 13)
 *  - FIXED:    chat_broadcast closed+freed peers that were still in the
 *              current event batch (UAF/double-close); registry-only removal
 *  - FIXED:    fork children inherited listener/epoll fds (closed on fork)
 *  - FIXED:    client partial write() data loss (write-all loop)
 *  - NEW:      idle/slowloris guard: HTTP keep-alive conns idle longer than
 *              HTTP_IDLE_SEC (default 120, env MINICAT_HTTP_IDLE, 2..86400)
 *              are swept every 5s (WS/chat exempt)
 *  - NEW:      strict CLI validation: full-string strtol port parsing
 *              (1-65535), duplicate -p + positional rejected, exact client
 *              host+port, extra args rejected
 *  - NEW:      client half-close + grace linger after stdin EOF (netcat -q):
 *              TCP shutdown(SHUT_WR); UDP waits 1s for the datagram reply
 *
 * Build:
 *   gcc minicat.c -o minicat -Wall -Wextra -O2 && strip minicat
 *   gcc -DWITH_SSL minicat.c -o minicat -lssl -lcrypto -Wall -Wextra -O2
 *
 * SECURITY NOTES:
 *   - The -e exec mode runs a command with a STRICT character allowlist
 *     (no shell metacharacters at all). It is NOT a sandbox.
 *   - XOR (-E) is obfuscation only. Use -S (TLS) for real confidentiality.
 *   - Do not run as root unless you understand the implications.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <sys/epoll.h>
#endif

#ifdef WITH_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#endif

/* ------------------------- Configuration ------------------------- */

#define VERSION      "v2.0.1"
#define AUTHOR       "Ali Zafar"
#define MAX_CLIENTS  10000
#define BUF_SIZE     65536
#define CONN_BUF     32768          /* per-connection input buffer (HTTP/WS) */
#define MAX_CMD_LEN  4096
#define MAX_EXEC_OUT (64 * 1024)
#define MAX_OPTARG   256
#define MAX_URI_LEN  255
#define WS_GUID      "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Idle/slowloris guard for HTTP keep-alive connections */
#define HTTP_IDLE_SEC  120
#define MIN_HTTP_IDLE  2
#define MAX_HTTP_IDLE  86400
static long http_idle_sec = HTTP_IDLE_SEC;

/* ------------------------- Global state -------------------------- */

int verbose = 0, keep_open = 0, hex_dump = 0, tcp_nodelay = 0;
int chat_mode = 0, http_mode = 0, stats_enabled = 0;
int logging_enabled = 0, rate_limit_enabled = 0;
int rate_limit_rps = 1000, ssl_enabled = 0, ws_mode = 0;
int proxy_mode = 0, encrypt_mode = 0;
int server_mode = 0, udp_mode = 0, fork_mode = 0, exec_mode = 0;
int running = 1;

FILE *log_file = NULL;
char *exec_cmd = NULL;
char *ssl_cert = NULL, *ssl_key_file = NULL;

unsigned char xor_key[32] = {0};
int xor_key_set = 0;

int listen_fd = -1;

/* Statistics */
typedef struct {
    unsigned long long tc, ac, tbs, tbr, treq, tws;
    time_t st;
} stats_t;
stats_t gs = {0};

/* ------------------------- Forward declarations ------------------ */

typedef struct conn conn_t;
int handle_client_data(conn_t *c, char *buf, int len);
void log_msg(const char *fmt, ...);
void hexdump(const char *p, const char *d, int len);
int parse_http_request(conn_t *c, char *buf, int len, char *response, size_t rlen);
int parse_websocket(conn_t *c, char *buf, int len, char *response, size_t rlen);
int run_server(int port);
int run_udp_server(int port);
int run_client(const char *host, int port);
int ssl_global_init(void);
int conn_read(conn_t *c, char *buf, int len);
int conn_write_all(conn_t *c, const char *buf, int len);
int rate_allow(void);
int validate_exec_cmd(const char *cmd);
int run_exec_command(char *out, int outlen);
static int loop_unregister(conn_t *c);

#ifdef WITH_SSL
SSL_CTX *g_ssl_ctx = NULL;
#endif

/* ------------------------- Usage --------------------------------- */

void usage(const char *prog) {
    printf("MINICAT %s - Built by %s\n\n", VERSION, AUTHOR);
    printf("Usage: %s [options] [host] port\n\n", prog);
    printf("OPTIONS:\n");
    printf("  -l          Listen mode (server)\n");
    printf("  -u          UDP mode\n");
    printf("  -p port     Port to listen on\n");
    printf("  -e cmd      Execute command (strict safe-char allowlist)\n");
    printf("  -k          Keep open (persistent / HTTP keep-alive)\n");
    printf("  -v          Verbose output\n");
    printf("  -x          Hex dump all traffic\n");
    printf("  -n          TCP_NODELAY (low latency)\n");
    printf("  -K          Chat broadcast mode\n");
    printf("  -H          HTTP server mode\n");
    printf("  -S          SSL/TLS enabled (requires -DWITH_SSL build)\n");
    printf("  -W          WebSocket mode (implies -H)\n");
    printf("  -P          Proxy mode (HTTP CONNECT / absolute-URI forward)\n");
    printf("  -E          Encryption (XOR; requires -A key)\n");
    printf("  -g          Statistics UI (implies -H)\n");
    printf("  -L file     Enable file logging\n");
    printf("  -T rate     Rate limit (req/sec, token bucket)\n");
    printf("  -c cert     SSL certificate file\n");
    printf("  -j key      SSL private key file\n");
    printf("  -A key      XOR encryption key (with -E)\n");
    printf("  -F          Fork per connection\n");
    printf("  -h          Show this help\n");
}

/* ------------------------- Logging ------------------------------- */

void log_msg(const char *fmt, ...) {
    if (!logging_enabled || !log_file) return;
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_file, "[%s] ", ts);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);
    fprintf(log_file, "\n");
    fflush(log_file);
}

/* ------------------------- Hex dump ------------------------------ */

void hexdump(const char *p, const char *d, int len) {
    if (!hex_dump) return;
    printf("[%s] HEX(%d): ", p, len);
    int show = len < 32 ? len : 32;
    for (int i = 0; i < show; i++) printf("%02x ", (unsigned char)d[i]);
    printf("\n");
    fflush(stdout);
}

/* ------------------------- XOR encryption ------------------------ */

void xor_encrypt(char *data, int len) {
    if (!xor_key_set) return;             /* key never set: no-op, documented */
    for (int i = 0; i < len; i++) data[i] ^= xor_key[i % 32];
}

/* ------------------------- SHA1 (RFC3174, compact) ---------------- */
/* Public-domain style implementation, used for RFC6455 handshake.    */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    unsigned char buf[64];
    size_t buflen;
} sha1_ctx;

static uint32_t sha1_rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(sha1_ctx *c, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8)  |  (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = c->h[0], b = c->h[1], e2 = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & e2) | (~b & d);        k = 0x5A827999; }
        else if (i < 40) { f = b ^ e2 ^ d;                 k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & e2) | (b & d) | (e2 & d); k = 0x8F1BBCDC; }
        else             { f = b ^ e2 ^ d;                 k = 0xCA62C1D6; }
        uint32_t tmp = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = e2; e2 = sha1_rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += e2; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1_ctx *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0; c->buflen = 0;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha1_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(sha1_ctx *c, unsigned char out[20]) {
    uint64_t bitlen = c->len * 8;
    unsigned char pad = 0x80;
    sha1_update(c, &pad, 1);
    unsigned char zero = 0;
    while (c->buflen != 56) sha1_update(c, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bitlen >> (56 - i*8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

static void base64_encode(const unsigned char *in, int inlen, char *out, int outlen) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < inlen; i += 3) {
        unsigned int v = 0; int n = 0;
        if (i < inlen)     { v |= (unsigned int)in[i] << 16; n++; }
        if (i + 1 < inlen) { v |= (unsigned int)in[i+1] << 8; n++; }
        if (i + 2 < inlen) { v |= (unsigned int)in[i+2];      n++; }
        char b4[4] = { tbl[(v >> 18) & 63], tbl[(v >> 12) & 63],
                       n > 1 ? tbl[(v >> 6) & 63] : '=',
                       n > 2 ? tbl[v & 63] : '=' };
        for (int j = 0; j < 4 && o < outlen - 1; j++) out[o++] = b4[j];
    }
    out[o] = '\0';
}

/* ------------------------- Rate limiting ------------------------- */
/* Global token bucket: rate_limit_rps tokens per second.             */

int rate_allow(void) {
    if (!rate_limit_enabled) return 1;
    static time_t last = 0;
    static double tokens = 0;
    time_t now = time(NULL);
    if (last == 0) { last = now; tokens = (double)rate_limit_rps; }
    else if (now != last) {
        double add = (double)rate_limit_rps * (double)(now - last);
        tokens += add;
        if (tokens > (double)rate_limit_rps * 2) tokens = (double)rate_limit_rps * 2;
        last = now;
    }
    if (tokens >= 1.0) { tokens -= 1.0; return 1; }
    return 0;
}

/* ------------------------- Exec validation ----------------------- */
/* STRICT allowlist: alnum, space, and / . _ - : = + , @ %            */
/* Everything else (shell metachars incl. > < * ? ~ $ ; | & ` " ' \   */
/* ( ) { } [ ] ! # ^) is rejected. popen() cannot be abused.          */

static int exec_char_ok(char ch) {
    if (isalnum((unsigned char)ch)) return 1;
    switch (ch) {
        case ' ': case '/': case '.': case '_': case '-':
        case ':': case '=': case '+': case ',': case '@': case '%':
            return 1;
        default: return 0;
    }
}

int validate_exec_cmd(const char *cmd) {
    if (!cmd || !cmd[0]) return 0;
    size_t len = strlen(cmd);
    if (len >= MAX_CMD_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!exec_char_ok(cmd[i])) return 0;
    }
    return 1;
}

/* Run the validated exec command, draining full output (64KB cap). */
int run_exec_command(char *out, int outlen) {
    FILE *fp = popen(exec_cmd, "r");
    if (!fp) { log_msg("Failed to execute command: %.100s", exec_cmd); return -1; }
    int total = 0;
    size_t n;
    while (total < outlen - 1 && (n = fread(out + total, 1, (size_t)(outlen - 1 - total), fp)) > 0)
        total += (int)n;
    /* Drain any remainder so pclose() cannot deadlock on a full pipe. */
    char discard[4096];
    while ((n = fread(discard, 1, sizeof(discard), fp)) > 0) { /* discard */ }
    out[total] = '\0';
    int rc = pclose(fp);
    if (rc != 0) log_msg("Command exited with status %d: %.100s", rc, exec_cmd);
    return total;
}

/* ------------------------- Connection context -------------------- */

struct conn {
    int fd;
    struct sockaddr_in6 addr;
    int is_http, is_ws;
    int ws_handshake_done;
    int ws_frag_opcode;                 /* -1 = no open fragment */
    int http_mid_headers;               /* inside a request's header block */
    time_t last_act;                    /* last activity, for idle sweep */
    char *inbuf;                        /* CONN_BUF bytes, malloc'd */
    size_t inlen;
    char *ws_frag;                      /* partial frame accumulation */
    size_t ws_frag_len;
#ifdef WITH_SSL
    SSL *ssl;
#endif
};

/* I/O wrappers (SSL-aware). For plain sockets these are read/write. */
int conn_read(conn_t *c, char *buf, int len) {
#ifdef WITH_SSL
    if (c->ssl) return SSL_read(c->ssl, buf, len);
#endif
    return (int)read(c->fd, buf, len);
}

/* Write-all with EINTR handling; returns 0 ok, -1 on error. */
int conn_write_all(conn_t *c, const char *buf, int len) {
    int off = 0;
    while (off < len) {
#ifdef WITH_SSL
        if (c->ssl) {
            int n = SSL_write(c->ssl, buf + off, len - off);
            if (n <= 0) {
                int e = SSL_get_error(c->ssl, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                return -1;
            }
            off += n;
            continue;
        }
#endif
        int n = (int)write(c->fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += n;
    }
    return 0;
}

/* ------------------------- WebSocket ----------------------------- */

/* RFC6455 handshake: Sec-WebSocket-Accept = b64(SHA1(key + GUID)) */
static int ws_compute_accept(const char *key, char *out, size_t outlen) {
    char concat[512];
    snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    sha1_ctx ctx;
    unsigned char digest[20];
    sha1_init(&ctx);
    sha1_update(&ctx, concat, strlen(concat));
    sha1_final(&ctx, digest);
    base64_encode(digest, 20, out, (int)outlen);
    return 0;
}

/* Write a server->client frame (unmasked, proper length encoding). */
static int ws_send_frame(conn_t *c, int opcode, const char *payload, int plen) {
    char hdr[14];
    int hlen = 0;
    hdr[hlen++] = (char)(0x80 | (opcode & 0x0F));   /* FIN + opcode */
    if (plen < 126) {
        hdr[hlen++] = (char)plen;
    } else if (plen < 65536) {
        hdr[hlen++] = 126;
        hdr[hlen++] = (char)((plen >> 8) & 0xFF);
        hdr[hlen++] = (char)(plen & 0xFF);
    } else {
        hdr[hlen++] = 127;
        uint64_t l = (uint64_t)plen;
        for (int i = 7; i >= 0; i--) hdr[hlen++] = (char)((l >> (i * 8)) & 0xFF);
    }
    if (conn_write_all(c, hdr, hlen) != 0) return -1;
    if (plen > 0 && conn_write_all(c, payload, plen) != 0) return -1;
    return 0;
}

/* Parse one client frame from buffer. Returns bytes consumed (>0),
 * 0 if incomplete, -1 on protocol error. Payload (unmasked) copied
 * to `out` (cap outlen). */
static int ws_parse_frame(const char *buf, int len, char *out, int outlen,
                          int *opcode, int *payload_len, int *fin, int *is_masked) {
    if (len < 2) return 0;
    unsigned char b0 = (unsigned char)buf[0];
    unsigned char b1 = (unsigned char)buf[1];
    *fin = (b0 & 0x80) ? 1 : 0;
    *opcode = b0 & 0x0F;
    int masked = (b1 & 0x80) ? 1 : 0;
    *is_masked = masked;
    uint64_t plen = b1 & 0x7F;
    int hdr = 2;
    if (plen == 126) {
        if (len < 4) return 0;
        plen = ((uint64_t)(unsigned char)buf[2] << 8) | (unsigned char)buf[3];
        hdr = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | (unsigned char)buf[2 + i];
        hdr = 10;
    }
    if (plen > (uint64_t)outlen) return -1;                 /* too large */
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (len < hdr + 4) return 0;
        memcpy(mask, buf + hdr, 4);
        hdr += 4;
    }
    if (len < hdr + (int)plen) return 0;
    if (plen > 0) {
        const char *p = buf + hdr;
        if (masked) {
            for (uint64_t i = 0; i < plen; i++)
                out[i] = p[i] ^ mask[i & 3];
        } else {
            memcpy(out, p, (size_t)plen);
        }
    }
    *payload_len = (int)plen;
    return hdr + (int)plen;
}

/* ------------------------- HTTP ---------------------------------- */

static void http_response(conn_t *c, int status, const char *status_text,
                          const char *ctype, const char *body, int head_only) {
    char resp[CONN_BUF + 512];
    int blen = body ? (int)strlen(body) : 0;
    int hl = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "Server: MINICAT %s\r\n"
        "\r\n",
        status, status_text, ctype, blen,
        keep_open ? "keep-alive" : "close", VERSION);
    if (blen > 0 && !head_only) {
        memcpy(resp + hl, body, (size_t)blen);
        hl += blen;
    }
    conn_write_all(c, resp, hl);
    gs.tbs += (unsigned long long)blen;
}

int parse_http_request(conn_t *c, char *buf, int len, char *response, size_t rlen) {
    char method[16] = {0}, uri[MAX_URI_LEN + 2] = {0}, ver[16] = {0};
    (void)len; (void)response; (void)rlen;

    /* BOUNDED parse: max 15/255/15 chars per token. Overlong tokens
     * simply don't match %s limits -> handled below. */
    int ntok = sscanf(buf, "%15s %255s %15s", method, uri, ver);
    if (ntok < 1) return 0;
    if (ntok < 2) { http_response(c, 400, "Bad Request", "text/plain", "Bad Request\n", 0); return 0; }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        http_response(c, 405, "Method Not Allowed", "text/plain", "Method Not Allowed\n", 0);
        return 0;
    }
    int head_only = (strcmp(method, "HEAD") == 0);
    gs.treq++;

    /* Reject overlong request lines (sscanf truncates -> would mis-route) */
    char *nl = strchr(buf, '\n');
    if (nl) {
        size_t line_len = (size_t)(nl - buf);
        if (line_len > 280) {
            http_response(c, 414, "URI Too Long", "text/plain", "URI Too Long\n", 0);
            return 0;
        }
    }

    /* Route */
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index") == 0) {
        char body[1024];
        snprintf(body, sizeof(body),
            "<html><body><h1>MINICAT %s</h1>"
            "<p>Network Tool by %s</p>"
            "<ul><li>/stats - Statistics</li><li>/health - Health</li>"
            "<li>/json - JSON stats</li></ul></body></html>\n",
            VERSION, AUTHOR);
        http_response(c, 200, "OK", "text/html", body, head_only);
        return 1;
    }
    if (stats_enabled && (strcmp(uri, "/stats") == 0 || strcmp(uri, "/statistics") == 0)) {
        char body[4096];
        time_t up = time(NULL) - gs.st;
        int h = (int)(up / 3600), m = (int)((up / 60) % 60), s = (int)(up % 60);
        double rr = up > 0 ? (double)gs.tbr / up : 0;
        double sr = up > 0 ? (double)gs.tbs / up : 0;
        snprintf(body, sizeof(body),
            "<html><head><title>MINICAT Statistics</title>"
            "<style>body{font-family:monospace;background:#1a1a2e;color:#0f0}"
            ".box{background:#16213e;padding:20px;margin:10px;border-radius:8px}"
            "h1{color:#e94560} .stat{color:#0f0} .label{color:#888}</style>"
            "</head><body>"
            "<h1>MINICAT %s - Statistics</h1>"
            "<div class='box'>"
            "<div class='label'>Uptime:</div><div class='stat'>%dh %dm %ds</div>"
            "<div class='label'>Connections:</div><div class='stat'>%llu active / %llu total</div>"
            "<div class='label'>Received:</div><div class='stat'>%llu bytes (%.1f KB/s)</div>"
            "<div class='label'>Sent:</div><div class='stat'>%llu bytes (%.1f KB/s)</div>"
            "<div class='label'>HTTP Requests:</div><div class='stat'>%llu</div>"
            "<div class='label'>WebSocket:</div><div class='stat'>%llu</div>"
            "</div></body></html>\n",
            VERSION, h, m, s, gs.ac, gs.tc, gs.tbr, rr / 1024,
            gs.tbs, sr / 1024, gs.treq, gs.tws);
        http_response(c, 200, "OK", "text/html", body, head_only);
        return 1;
    }
    if (strcmp(uri, "/json") == 0) {
        char body[512];
        time_t up = time(NULL) - gs.st;
        snprintf(body, sizeof(body),
            "{\"uptime\": %ld, \"connections\": %llu, \"tx\": %llu, \"rx\": %llu}",
            (long)up, gs.ac, gs.tbs, gs.tbr);
        http_response(c, 200, "OK", "application/json", body, head_only);
        return 1;
    }
    if (strcmp(uri, "/health") == 0 || strcmp(uri, "/ping") == 0) {
        http_response(c, 200, "OK", "text/plain", "OK", head_only);
        return 1;
    }
    /* Unknown route -> proper 404 */
    http_response(c, 404, "Not Found", "text/plain", "Not Found\n", head_only);
    return 1;
}

/* ------------------------- Proxy --------------------------------- */

/* Handle an HTTP CONNECT or absolute-URI forward request (blocking
 * relay). Runs in a per-connection child process. Returns 1 when the
 * connection was consumed (caller should close). */
static int proxy_handle(conn_t *c, char *buf, int len) {
    char method[16] = {0}, target[512] = {0};
    if (sscanf(buf, "%15s %511s", method, target) < 2) return 0;
    int is_connect = (strcmp(method, "CONNECT") == 0);

    char host[256] = {0};
    int port = 443;
    if (is_connect) {
        if (sscanf(target, "%255[^:]:%d", host, &port) != 2) {
            if (sscanf(target, "%255s", host) != 1) return 0;
        }
    } else {
        if (strncmp(target, "http://", 7) != 0) return 0;
        char *rest = target + 7;
        char *slash = strchr(rest, '/');
        char *colon = strchr(rest, ':');
        int hostlen = slash ? (int)(slash - rest) : (int)strlen(rest);
        if (colon && (!slash || colon < slash)) {
            int plen = (int)((slash ? slash : rest + hostlen) - colon) - 1;
            if (plen > 0 && plen < 8) {
                char pbuf[8];
                memcpy(pbuf, colon + 1, (size_t)plen);
                pbuf[plen] = '\0';
                port = atoi(pbuf);
            }
            hostlen = (int)(colon - rest);
        } else {
            port = 80;
        }
        snprintf(host, sizeof(host), "%.*s",
                 hostlen < (int)sizeof(host) - 1 ? hostlen : (int)sizeof(host) - 1, rest);
    }

    if (!host[0]) return 0;
    if (verbose) printf("Proxy: %s %s:%d\n", is_connect ? "CONNECT" : "FWD", host, port);
    fflush(stdout);
    log_msg("Proxy %s %s:%d", is_connect ? "CONNECT" : "FWD", host, port);

    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &ai) != 0) {
        http_response(c, 502, "Bad Gateway", "text/plain", "Bad Gateway\n", 0);
        return 1;
    }
    int ufd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (ufd < 0) { freeaddrinfo(ai); return 1; }
    if (connect(ufd, ai->ai_addr, ai->ai_addrlen) != 0) {
        close(ufd); freeaddrinfo(ai);
        http_response(c, 502, "Bad Gateway", "text/plain", "Bad Gateway\n", 0);
        return 1;
    }
    freeaddrinfo(ai);

    if (is_connect) {
        const char *ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
        conn_write_all(c, ok, (int)strlen(ok));
    } else {
        /* Rewrite request line to origin-form and forward */
        char *nl = strchr(buf, '\n');
        int forward_len = nl ? (int)(nl - buf) : len;
        char fwd[CONN_BUF];
        /* Path starts at the first '/' after the scheme (target is intact
         * — we no longer NUL-truncate it during parsing). */
        char *path = strchr(target + 7, '/');
        if (!path) path = (char *)"/";
        int fl = snprintf(fwd, sizeof(fwd), "%s %s HTTP/1.1\r\n", method, path);
        if (forward_len > fl && forward_len < (int)sizeof(fwd)) {
            const char *src = buf;
            const char *sp = strchr(src, ' ');
            sp = sp ? strchr(sp + 1, ' ') : NULL;
            if (sp) {
                const char *nl2 = strchr(sp + 1, '\n');
                size_t skip = (size_t)((nl2 ? nl2 : sp) - src) + 1;
                if (skip < (size_t)len) {
                    size_t rem = (size_t)len - skip;
                    if (rem > sizeof(fwd) - (size_t)fl - 1) rem = sizeof(fwd) - (size_t)fl - 1;
                    memcpy(fwd + fl, src + skip, rem);
                    fl += (int)rem;
                }
            }
        }
        fwd[fl] = '\0';
        if (write(ufd, fwd, fl) < 0) { close(ufd); return 1; }
    }

    /* Bidirectional relay */
    struct pollfd pfds[2];
    pfds[0].fd = c->fd;  pfds[0].events = POLLIN;
    pfds[1].fd = ufd;    pfds[1].events = POLLIN;
    char rb[16384];
    int c_open = 1, u_open = 1;
    while ((c_open || u_open) && running) {
        int pr = poll(pfds, 2, 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (c_open && (pfds[0].revents & POLLIN)) {
            int n = (int)read(c->fd, rb, sizeof(rb));
            if (n <= 0) c_open = 0;
            else if (write(ufd, rb, n) < 0) u_open = 0;
            pfds[0].revents = 0;
        }
        if (u_open && (pfds[1].revents & POLLIN)) {
            int n = (int)read(ufd, rb, sizeof(rb));
            if (n <= 0) u_open = 0;
            else if (conn_write_all(c, rb, n) != 0) c_open = 0;
            pfds[1].revents = 0;
        }
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) c_open = 0;
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) u_open = 0;
    }
    close(ufd);
    return 1;
}

/* ------------------------- Chat registry ------------------------- */

static conn_t *chatters[MAX_CLIENTS];
static int nchatters = 0;

static void chat_add(conn_t *c) {
    if (nchatters >= MAX_CLIENTS) return;
    chatters[nchatters++] = c;
}

static void chat_remove(conn_t *c) {
    for (int i = 0; i < nchatters; i++) {
        if (chatters[i] == c) {
            chatters[i] = chatters[nchatters - 1];
            nchatters--;
            return;
        }
    }
}

static void chat_broadcast(conn_t *sender, const char *data, int len) {
    for (int i = 0; i < nchatters; i++) {
        conn_t *o = chatters[i];
        if (o == sender) continue;
        /* NOTE: never close()/free() a peer here — it may still be in the
         * current event batch (use-after-free). Just unregister; the event
         * loop reaps the dead socket via its next read (EOF). */
        if (o->is_ws) {
            if (ws_send_frame(o, 0x1, data, len) != 0) {
                chat_remove(o);
                i--;
            }
        } else {
            if (conn_write_all(o, data, len) != 0) {
                chat_remove(o);
                i--;
            }
        }
    }
}

/* ------------------------- Core data handler --------------------- */

int handle_client_data(conn_t *c, char *buf, int len) {
    if (len <= 0) return -1;

    gs.tbr += (unsigned long long)len;
    hexdump("RX", buf, len);

    /* Encryption (XOR) — applied on the wire */
    if (encrypt_mode && xor_key_set) {
        xor_encrypt(buf, len);
        hexdump("DEC", buf, len);
    }

    /* Proxy mode: only reached via the child path (proxy_handle blocks);
     * if we ever land here, consume the connection. */
    if (proxy_mode) {
        if (proxy_handle(c, buf, len)) return -1;
    }

    /* HTTP / WebSocket mode */
    if (http_mode || ws_mode) {
        /* Append to per-connection buffer for pipelining/partial reads */
        if (c->inlen + (size_t)len > CONN_BUF - 1) {
            if (c->is_http) http_response(c, 400, "Bad Request", "text/plain", "Bad Request\n", 0);
            return -1;
        }
        memcpy(c->inbuf + c->inlen, buf, (size_t)len);
        c->inlen += (size_t)len;
        c->inbuf[c->inlen] = '\0';

        while (c->inlen > 0) {
            /* WebSocket frames are binary (no newlines): must be parsed
             * before the newline-based HTTP logic below. */
            if (c->is_ws) {
                /* Parse WebSocket frames from buffer */
                int opcode = 0, plen = 0, fin = 0, masked = 0;
                char payload[CONN_BUF];
                int used = ws_parse_frame(c->inbuf, (int)c->inlen, payload,
                                          (int)sizeof(payload), &opcode, &plen,
                                          &fin, &masked);
                if (used < 0) return -1;            /* protocol error */
                if (used == 0) break;               /* incomplete frame */
                memmove(c->inbuf, c->inbuf + used, c->inlen - (size_t)used);
                c->inlen -= (size_t)used;
                c->inbuf[c->inlen] = '\0';

                /* RFC6455 §5.1: all client frames MUST be masked. Data
                 * frames are rejected unmasked; control frames are treated
                 * leniently for interoperability. */
                if ((opcode == 0x0 || opcode == 0x1 || opcode == 0x2) && !masked)
                    return -1;

                /* RFC6455 §5.5: control frames MUST have FIN set and
                 * payload <= 125 bytes. */
                if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) {
                    if (!fin || plen > 125) return -1;
                }

                if (opcode == 0x8) {                /* close */
                    ws_send_frame(c, 0x8, payload, plen);
                    return -1;
                } else if (opcode == 0x9) {         /* ping -> pong */
                    ws_send_frame(c, 0xA, payload, plen);
                } else if (opcode == 0xA) {         /* pong: ignore */
                    /* nothing */
                } else if (opcode == 0x1 || opcode == 0x2) {
                    /* RFC6455 §5.4: a new data frame while a fragmented
                     * message is open is a protocol error. */
                    if (c->ws_frag_opcode >= 0) return -1;
                    if (!fin) {                     /* start fragment */
                        c->ws_frag_opcode = opcode;
                        if (c->ws_frag_len + (size_t)plen > CONN_BUF) return -1;
                        memcpy(c->ws_frag + c->ws_frag_len, payload, (size_t)plen);
                        c->ws_frag_len += (size_t)plen;
                    } else {
                        /* single complete message: echo + broadcast */
                        ws_send_frame(c, opcode, payload, plen);
                        if (chat_mode) chat_broadcast(c, payload, plen);
                    }
                } else if (opcode == 0x0) {
                    /* continuation — requires an open fragment */
                    if (c->ws_frag_opcode < 0) return -1;
                    if (c->ws_frag_len + (size_t)plen > CONN_BUF) return -1;
                    memcpy(c->ws_frag + c->ws_frag_len, payload, (size_t)plen);
                    c->ws_frag_len += (size_t)plen;
                    if (fin) {
                        ws_send_frame(c, c->ws_frag_opcode, c->ws_frag, (int)c->ws_frag_len);
                        if (chat_mode) chat_broadcast(c, c->ws_frag, (int)c->ws_frag_len);
                        c->ws_frag_len = 0;
                        c->ws_frag_opcode = -1;
                    }
                } else {
                    return -1;                      /* unknown opcode */
                }
                continue;
            }

            /* Mid-headers state: waiting for the blank line that ends the
             * previous request's header block. */
            if (c->http_mid_headers) {
                char *blank = strstr(c->inbuf, "\r\n\r\n");
                if (!blank) {
                    if (c->inlen > 8192) return -1;
                    break;                          /* still mid-headers */
                }
                size_t consumed = (size_t)(blank - c->inbuf) + 4;
                memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
                c->inlen -= consumed;
                c->inbuf[c->inlen] = '\0';
                c->http_mid_headers = 0;
                if (c->inlen == 0) break;
                continue;                           /* parse next request */
            }

            /* Skip stray CR/LF between requests */
            while (c->inlen > 0 && (c->inbuf[0] == '\r' || c->inbuf[0] == '\n')) {
                memmove(c->inbuf, c->inbuf + 1, c->inlen - 1);
                c->inlen--;
            }
            if (c->inlen == 0) break;

            char *nl = strchr(c->inbuf, '\n');
            if (!nl) {
                if (c->inlen > 512) {   /* request line too long, no newline */
                    http_response(c, 400, "Bad Request", "text/plain", "Bad Request\n", 0);
                    return -1;
                }
                break;                  /* wait for more data */
            }

            if (!c->ws_handshake_done && ws_mode) {
                /* Need full headers (blank line) for handshake */
                char *blank = strstr(c->inbuf, "\r\n\r\n");
                if (!blank) {
                    if (c->inlen > 8192) return -1;
                    break;
                }
                char response[1024];
                if (parse_websocket(c, c->inbuf, (int)c->inlen, response, sizeof(response))) {
                    conn_write_all(c, response, (int)strlen(response));
                    c->ws_handshake_done = 1;
                    c->is_ws = 1;
                    gs.tws++;
                    if (chat_mode) chat_add(c);
                } else {
                    http_response(c, 400, "Bad Request", "text/plain", "Bad Request\n", 0);
                    return -1;
                }
                /* Consume handshake, keep any frame data that followed */
                size_t consumed = (size_t)(blank - c->inbuf) + 4;
                memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
                c->inlen -= consumed;
                c->inbuf[c->inlen] = '\0';
                continue;
            }

            /* Plain HTTP: parse one request from buffer */
            if (!rate_allow()) {
                http_response(c, 429, "Too Many Requests", "text/plain", "Too Many Requests\n", 0);
                if (!keep_open) return -1;
                /* Consume through end-of-headers when complete, else the
                 * request line only and wait (mid-headers). */
                char *blank = strstr(c->inbuf, "\r\n\r\n");
                size_t consumed;
                if (blank) {
                    consumed = (size_t)(blank - c->inbuf) + 4;
                } else {
                    consumed = (size_t)(nl - c->inbuf) + 1;
                    c->http_mid_headers = 1;
                }
                memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
                c->inlen -= consumed;
                c->inbuf[c->inlen] = '\0';
                continue;
            }
            parse_http_request(c, c->inbuf, (int)c->inlen, NULL, 0);
            /* Consume the request line plus any complete header block. */
            size_t consumed = (size_t)(nl - c->inbuf) + 1;
            char *blank = strstr(c->inbuf, "\r\n\r\n");
            if (blank) {
                consumed = (size_t)(blank - c->inbuf) + 4;
            } else {
                /* No blank line yet: consume only the request line. If the
                 * remainder already looks like a pipelined request line,
                 * keep parsing; otherwise wait in mid-headers state. */
                size_t remain = c->inlen - consumed;
                const char *next = c->inbuf + consumed;
                if (!(remain >= 4 && (strncmp(next, "GET ", 4) == 0 ||
                                      strncmp(next, "HEAD ", 5) == 0)))
                    c->http_mid_headers = 1;
            }
            memmove(c->inbuf, c->inbuf + consumed, c->inlen - consumed);
            c->inlen -= consumed;
            c->inbuf[c->inlen] = '\0';
            if (!keep_open) return -1;              /* single-shot HTTP */
            if (c->inlen == 0) break;
        }
        return (keep_open || ws_mode || chat_mode) ? 0 : -1;   /* WS/chat are persistent by nature */
    }

    /* Command execution (strict allowlist validated at startup) */
    if (exec_mode && exec_cmd && exec_cmd[0]) {
        if (!rate_allow()) return -1;
        char out[MAX_EXEC_OUT];
        int n = run_exec_command(out, sizeof(out));
        if (n > 0) {
            conn_write_all(c, out, n);
            gs.tbs += (unsigned long long)n;
        }
        return keep_open ? 0 : -1;
    }

    /* Chat broadcast */
    if (chat_mode) {
        if (!rate_allow()) return -1;
        chat_broadcast(c, buf, len);
        /* Chat is persistent by nature (matches the WS/chat comment in the
         * HTTP branch): the connection stays open until EOF/error. */
        return 0;
    }

    /* Default: relay to stdout — stream until the client disconnects
     * (netcat-like). -T rate limiting applies to HTTP/WS/exec/chat
     * request processing, not to raw streaming relay. */
    if (write(STDOUT_FILENO, buf, len) < 0 && errno != EINTR) { /* ignore */ }
    gs.tbs += (unsigned long long)len;
    return 0;
}

/* ------------------------- WebSocket handshake ------------------- */

/* Case-insensitive substring search. */
static int ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return 1;
    }
    return 0;
}

int parse_websocket(conn_t *c, char *buf, int len, char *response, size_t rlen) {
    (void)c; (void)len;
    /* RFC6455 handshake validation: Upgrade header present with a
     * "websocket" value, Sec-WebSocket-Key, and version 13. */
    char *upg = strstr(buf, "Upgrade:");
    if (!upg || !ci_contains(upg + 8, "websocket")) return 0;
    char *ver = strstr(buf, "Sec-WebSocket-Version:");
    if (!ver) return 0;
    ver += strlen("Sec-WebSocket-Version:");
    while (*ver == ' ' || *ver == '\t') ver++;
    if (strtol(ver, NULL, 10) != 13) return 0;
    char *key = strstr(buf, "Sec-WebSocket-Key:");
    if (!key) return 0;
    key += strlen("Sec-WebSocket-Key:");    /* skip past the colon */
    while (*key == ' ' || *key == '\t') key++;
    char kbuf[128] = {0};
    int i = 0;
    while (key[i] && key[i] != '\r' && key[i] != '\n' && i < 127) {
        kbuf[i] = key[i]; i++;
    }
    kbuf[i] = '\0';
    if (i < 16) return 0;                       /* key too short */

    char accept[64];
    ws_compute_accept(kbuf, accept, sizeof(accept));
    snprintf(response, rlen,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);
    return 1;
}

/* ------------------------- Socket helpers ------------------------ */

int create_socket(int type, int port) {
    int fd;
    struct sockaddr_in6 addr;
    fd = socket(AF_INET6, type, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((uint16_t)port);
    addr.sin6_addr = in6addr_any;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (type == SOCK_STREAM && listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------------- Connection lifecycle ------------------ */

/* Registry of all live connections (for the idle sweep). */
static conn_t *all_conns[MAX_CLIENTS];
static int nall = 0;

static void conn_reg_add(conn_t *c) {
    if (nall >= MAX_CLIENTS) return;
    all_conns[nall++] = c;
}

static void conn_reg_del(conn_t *c) {
    for (int i = 0; i < nall; i++) {
        if (all_conns[i] == c) {
            all_conns[i] = all_conns[nall - 1];
            nall--;
            return;
        }
    }
}

static void conn_close(conn_t *c);      /* forward decl for sweep_idle */

/* Idle/slowloris guard: close HTTP keep-alive connections that have been
 * silent longer than http_idle_sec. WS and chat connections are exempt. */
static void sweep_idle(time_t now) {
    for (int i = 0; i < nall; i++) {
        conn_t *c = all_conns[i];
        if (!c || !c->is_http || c->is_ws) continue;
        if (now - c->last_act > http_idle_sec) {
            conn_close(c);              /* also conn_reg_del's it */
            i--;
        }
    }
}

static void conn_free(conn_t *c) {
    if (!c) return;
    free(c->inbuf);
    free(c->ws_frag);
    free(c);
}

/* Full close: unregister from event loop + teardown (parent/loop only) */
static void conn_close(conn_t *c) {
    if (!c) return;
    conn_reg_del(c);
    if (chat_mode) chat_remove(c);
    loop_unregister(c);
    close(c->fd);
    if (gs.ac > 0) gs.ac--;
    if (verbose) printf("Client disconnected (fd=%d)\n", c->fd);
    log_msg("Connection closed (fd=%d)", c->fd);
#ifdef WITH_SSL
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
#endif
    conn_free(c);
}

/* Lightweight teardown for fork children (never touches epoll or
 * stdio FILE streams: use _exit() after this, not exit()). */
static void conn_destroy_plain(conn_t *c) {
    if (!c) return;
    close(c->fd);
    conn_free(c);
}

static conn_t *conn_new(int fd, const struct sockaddr_in6 *addr) {
    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) return NULL;
    c->fd = fd;
    if (addr) memcpy(&c->addr, addr, sizeof(*addr));
    c->ws_frag_opcode = -1;
    c->last_act = time(NULL);
    c->is_http = http_mode;
    if (http_mode || ws_mode) {
        c->inbuf = malloc(CONN_BUF);
        if (!c->inbuf) { free(c); return NULL; }
        if (ws_mode) {
            c->ws_frag = malloc(CONN_BUF);
            if (!c->ws_frag) { free(c->inbuf); free(c); return NULL; }
        }
    }
    return c;
}

/* Blocking per-connection handler used by -F / -S / -P children. */
static void connection_loop(conn_t *c) {
    char stackbuf[BUF_SIZE];
    while (running) {
        int n = conn_read(c, stackbuf, (int)sizeof(stackbuf) - 1);
        if (n < 0) {
#ifdef WITH_SSL
            if (c->ssl) {
                int e = SSL_get_error(c->ssl, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                    usleep(1000);
                    continue;
                }
            }
#endif
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (handle_client_data(c, stackbuf, n) != 0) break;
    }
}

/* ------------------------- Event loop ---------------------------- */
/* Linux: level-triggered epoll (no starvation). Elsewhere: poll().  */

#ifdef __linux__

static int loop_fd = -1;

static int loop_init(void) {
    loop_fd = epoll_create1(0);
    return loop_fd >= 0 ? 0 : -1;
}

static int loop_register(conn_t *c) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = c;
    return epoll_ctl(loop_fd, EPOLL_CTL_ADD, c->fd, &ev);
}

static int loop_unregister(conn_t *c) {
    struct epoll_event ev;
    ev.events = 0; ev.data.ptr = NULL;
    epoll_ctl(loop_fd, EPOLL_CTL_DEL, c->fd, &ev);
    return 0;
}

static int loop_wait(conn_t ***ready, int timeout_ms) {
    static struct epoll_event evs[MAX_CLIENTS];
    static conn_t *r[MAX_CLIENTS];
    int n = epoll_wait(loop_fd, evs, MAX_CLIENTS, timeout_ms);
    if (n <= 0) { *ready = r; return n; }
    for (int i = 0; i < n; i++) r[i] = (conn_t *)evs[i].data.ptr;
    *ready = r;
    return n;
}

#else /* poll() fallback (macOS, BSD, ...) */

#define MAX_POLL (MAX_CLIENTS + 1)
static struct pollfd pfds[MAX_POLL];
static conn_t *pconns[MAX_POLL];
static int npoll = 0;

static int loop_init(void) { npoll = 0; return 0; }

static int loop_register(conn_t *c) {
    if (npoll >= MAX_POLL) return -1;
    pfds[npoll].fd = c->fd;
    pfds[npoll].events = POLLIN;
    pfds[npoll].revents = 0;
    pconns[npoll] = c;
    npoll++;
    return 0;
}

static int loop_unregister(conn_t *c) {
    for (int i = 0; i < npoll; i++) {
        if (pconns[i] == c) {
            pconns[i] = pconns[npoll - 1];
            pfds[i] = pfds[npoll - 1];
            npoll--;
            return 0;
        }
    }
    return -1;
}

static int loop_wait(conn_t ***ready, int timeout_ms) {
    static conn_t *r[MAX_CLIENTS];
    int n = poll(pfds, (nfds_t)npoll, timeout_ms);
    if (n <= 0) { *ready = r; return n; }
    int m = 0;
    for (int i = 0; i < npoll; i++) {
        if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))
            r[m++] = pconns[i];
        pfds[i].revents = 0;
    }
    *ready = r;
    return m;
}

#endif

/* ------------------------- Server loop --------------------------- */

static void handle_accept(void) {
    struct sockaddr_in6 addr;
    socklen_t alen = sizeof(addr);
    int cfd;
    while ((cfd = accept(listen_fd, (struct sockaddr *)&addr, &alen)) >= 0) {
        /* Rate limit applies per connection in non-HTTP modes */
        if (!http_mode && !ws_mode && !proxy_mode && !rate_allow()) {
            close(cfd);
            continue;
        }
        if (tcp_nodelay) {
            int opt = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        }
        gs.tc++;
        gs.ac++;

        char client_ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &addr.sin6_addr, client_ip, sizeof(client_ip));
        if (verbose) printf("Client: %s (fd=%d)\n", client_ip, cfd);
        log_msg("Connection from %s", client_ip);

        /* Fork per connection: used by -F, SSL, and proxy modes.
         * Children never touch the epoll instance (race-free) and
         * never run exit() on inherited stdio streams. */
        if (fork_mode || ssl_enabled || proxy_mode) {
            pid_t pid = fork();
            if (pid == 0) {
                /* Children never accept connections and never touch the
                 * epoll instance — close both inherited descriptors. */
                close(listen_fd);
#ifdef __linux__
                close(loop_fd);
#endif
                conn_t *c = conn_new(cfd, &addr);
                if (!c) _exit(1);
                if (ssl_enabled) {
#ifdef WITH_SSL
                    c->ssl = SSL_new(g_ssl_ctx);
                    if (!c->ssl) _exit(1);
                    SSL_set_fd(c->ssl, cfd);
                    if (SSL_accept(c->ssl) != 1) _exit(1);
#else
                    fprintf(stderr, "SSL support not compiled in (build with -DWITH_SSL)\n");
                    _exit(1);
#endif
                }
                if (proxy_mode) {
                    char buf[BUF_SIZE];
                    int n = conn_read(c, buf, (int)sizeof(buf) - 1);
                    if (n > 0) proxy_handle(c, buf, n);
                } else {
                    connection_loop(c);
                }
                if (log_file) fflush(log_file);
                /* Children exit via _exit() (never run atexit/stdio teardown
                 * on inherited streams), so flush relayed stdout/stderr here
                 * or buffered data is silently lost. */
                fflush(stdout);
                fflush(stderr);
                conn_destroy_plain(c);
                _exit(0);
            }
            /* parent */
            close(cfd);
            gs.ac--;                    /* child owns the accounting now */
            continue;
        }

        conn_t *c = conn_new(cfd, &addr);
        if (!c) { close(cfd); gs.ac--; continue; }
        set_nonblocking(cfd);
        if (loop_register(c) != 0) { conn_close(c); continue; }
        conn_reg_add(c);
        if (chat_mode) chat_add(c);
    }
}

int run_server(int port) {
    if (loop_init() != 0) { perror("epoll_create"); return -1; }

    /* Register the listener in the event loop */
    conn_t *lconn = calloc(1, sizeof(conn_t));
    if (!lconn) return -1;
    lconn->fd = listen_fd;
    lconn->ws_frag_opcode = -1;
    if (loop_register(lconn) != 0) { free(lconn); return -1; }
    set_nonblocking(listen_fd);

    if (verbose) {
        printf("MINICAT %s listening on port %d\n", VERSION, port);
        printf("Features: HTTP=%d WebSocket=%d SSL=%d Proxy=%d Encrypt=%d\n",
               http_mode, ws_mode, ssl_enabled, proxy_mode, encrypt_mode);
        fflush(stdout);
    }
    log_msg("MINICAT %s started (pid=%d) on port %d", VERSION, getpid(), port);

    gs.st = time(NULL);
    time_t last_sweep = 0;
    while (running) {
        conn_t **ready = NULL;
        int n = loop_wait(&ready, 1000);
        if (n < 0) { if (errno == EINTR) continue; break; }
        time_t now = time(NULL);
        if (now - last_sweep >= 5) { last_sweep = now; sweep_idle(now); }
        for (int i = 0; i < n; i++) {
            conn_t *c = ready[i];
            if (!c) continue;
            if (c->fd == listen_fd) {
                handle_accept();
                continue;
            }
            char stackbuf[BUF_SIZE];
            int r = conn_read(c, stackbuf, (int)sizeof(stackbuf) - 1);
            if (r < 0) {
#ifdef WITH_SSL
                if (c->ssl) {
                    int e = SSL_get_error(c->ssl, r);
                    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                }
#endif
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                conn_close(c);
                continue;
            }
            if (r == 0) { conn_close(c); continue; }
            c->last_act = now;
            if (handle_client_data(c, stackbuf, r) != 0) {
                conn_close(c);
            }
        }
    }
    log_msg("MINICAT %s stopped", VERSION);
    loop_unregister(lconn);
    free(lconn);
    return 0;
}

/* ------------------------- UDP server ---------------------------- */

int run_udp_server(int port) {
    int fd = create_socket(SOCK_DGRAM, port);
    if (fd < 0) return -1;
    if (verbose) printf("MINICAT %s UDP listening on port %d\n", VERSION, port);
    log_msg("MINICAT %s UDP started (pid=%d) on port %d", VERSION, getpid(), port);

    gs.st = time(NULL);
    char buf[BUF_SIZE];
    struct sockaddr_in6 from;
    socklen_t flen = sizeof(from);

    while (running) {
        struct pollfd p;
        p.fd = fd; p.events = POLLIN; p.revents = 0;
        int pr = poll(&p, 1, 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        int n = (int)recvfrom(fd, buf, (int)sizeof(buf) - 1, 0,
                              (struct sockaddr *)&from, &flen);
        if (n < 0) { if (errno == EINTR) continue; break; }
        buf[n] = '\0';
        gs.tbr += (unsigned long long)n;
        gs.tc++;
        gs.ac++;

        if (encrypt_mode && xor_key_set) {
            xor_encrypt(buf, n);
            hexdump("DEC", buf, n);
        }
        hexdump("RX", buf, n);
        if (verbose) {
            char ip[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &from.sin6_addr, ip, sizeof(ip));
            printf("UDP from %s:%d (%d bytes)\n", ip, ntohs(from.sin6_port), n);
            fflush(stdout);
        }

        if (exec_mode && exec_cmd && exec_cmd[0]) {
            if (!rate_allow()) { gs.ac--; continue; }
            char out[MAX_EXEC_OUT];
            int on = run_exec_command(out, sizeof(out));
            if (on > 0) {
                if (encrypt_mode && xor_key_set) xor_encrypt(out, on);
                sendto(fd, out, on, 0, (struct sockaddr *)&from, flen);
                gs.tbs += (unsigned long long)on;
            }
        } else {
            if (write(STDOUT_FILENO, buf, n) < 0 && errno != EINTR) { /* ignore */ }
        }
        gs.ac--;
    }
    close(fd);
    return 0;
}

/* ------------------------- Client mode --------------------------- */

int run_client(const char *host, int port) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &ai) != 0) {
        fprintf(stderr, "Could not resolve host: %s\n", host);
        return -1;
    }

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { perror("socket"); freeaddrinfo(ai); return -1; }

    struct sockaddr_storage peer;
    socklen_t peerlen = (socklen_t)sizeof(peer);
    if (!udp_mode) {
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            perror("connect");
            close(fd);
            freeaddrinfo(ai);
            return -1;
        }
    } else {
        memcpy(&peer, ai->ai_addr, ai->ai_addrlen);
        peerlen = ai->ai_addrlen;
    }
    freeaddrinfo(ai);

    if (verbose) printf("Connected to %s:%d (%s)\n", host, port,
                        udp_mode ? "UDP" : "TCP");

#ifdef WITH_SSL
    SSL *cli_ssl = NULL;
    if (ssl_enabled) {
        cli_ssl = SSL_new(g_ssl_ctx);
        if (!cli_ssl) { close(fd); return -1; }
        SSL_set_fd(cli_ssl, fd);
        if (SSL_connect(cli_ssl) != 1) {
            fprintf(stderr, "SSL handshake failed\n");
            SSL_free(cli_ssl);
            close(fd);
            return -1;
        }
    }
#define CLIENT_READ(b, l) (cli_ssl ? SSL_read(cli_ssl, (b), (l)) : (int)read(fd, (b), (l)))
#define CLIENT_WRITE(b, l) do { \
    int _off = 0; \
    while (_off < (l)) { \
        ssize_t _wr = cli_ssl ? SSL_write(cli_ssl, (b) + _off, (l) - _off) \
                              : (ssize_t)write(fd, (b) + _off, (l) - _off); \
        if (_wr < 0) { \
            if (cli_ssl) { \
                int _e = SSL_get_error(cli_ssl, _wr); \
                if (_e == SSL_ERROR_WANT_READ || _e == SSL_ERROR_WANT_WRITE) continue; \
            } else if (errno == EINTR) { \
                continue; \
            } \
            break; \
        } \
        _off += (int)_wr; \
    } \
} while (0)
#else
#define CLIENT_READ(b, l) (int)read(fd, (b), (l))
#define CLIENT_WRITE(b, l) do { \
    int _off = 0; \
    while (_off < (l)) { \
        ssize_t _wr = write(fd, (b) + _off, (l) - _off); \
        if (_wr < 0) { \
            if (errno == EINTR) continue; \
            break; \
        } \
        _off += (int)_wr; \
    } \
} while (0)
#endif

    struct pollfd pfd[2];
    pfd[0].fd = fd; pfd[0].events = POLLIN;
    pfd[1].fd = STDIN_FILENO; pfd[1].events = POLLIN;
    char buf[BUF_SIZE];

    int stdin_done = 0;
    time_t eof_at = 0;
    /* After stdin EOF, linger briefly for the peer's final reply
     * (netcat -q semantics). TCP: half-close so the server sees EOF and
     * closes promptly. UDP: no half-close; wait one grace period for the
     * datagram reply (e.g. exec-mode responses). */
    int grace = udp_mode ? 1 : 2;

    while (running) {
        pfd[0].revents = 0;
        pfd[1].revents = 0;
        pfd[1].fd = stdin_done ? -1 : STDIN_FILENO;
        if (stdin_done && time(NULL) - eof_at > grace) break;
        int ret = poll(pfd, 2, 200);
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) {
            if (stdin_done && time(NULL) - eof_at > grace) break;
            continue;
        }

        if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            int n;
            if (udp_mode) {
                /* UDP: receive a datagram */
                struct sockaddr_storage sfrom;
                socklen_t slen = (socklen_t)sizeof(sfrom);
                n = (int)recvfrom(fd, buf, (int)sizeof(buf) - 1, 0,
                                  (struct sockaddr *)&sfrom, &slen);
                if (n <= 0) continue;
            } else {
                n = CLIENT_READ(buf, (int)sizeof(buf) - 1);
                if (n <= 0) break;
            }
            if (encrypt_mode && xor_key_set) xor_encrypt(buf, n);
            if (write(STDOUT_FILENO, buf, n) < 0 && errno != EINTR) { /* ignore */ }
        }
        if (!stdin_done && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            /* NOTE: a closed pipe reports POLLHUP *without* POLLIN —
             * check all three so EOF on stdin exits cleanly. */
            int n = (int)read(STDIN_FILENO, buf, (int)sizeof(buf) - 1);
            if (n <= 0) {
                stdin_done = 1;
                eof_at = time(NULL);
                if (!udp_mode) shutdown(fd, SHUT_WR);   /* half-close */
                continue;
            }
            if (udp_mode) {
                if (encrypt_mode && xor_key_set) xor_encrypt(buf, n);
                sendto(fd, buf, n, 0, (struct sockaddr *)&peer, peerlen);
                continue;
            }
            if (encrypt_mode && xor_key_set) xor_encrypt(buf, n);
            CLIENT_WRITE(buf, n);
        }
    }
    close(fd);
#ifdef WITH_SSL
    if (cli_ssl) SSL_free(cli_ssl);
#endif
    return 0;
}

/* ------------------------- SSL init ------------------------------ */

int ssl_global_init(void) {
#ifdef WITH_SSL
    SSL_library_init();
    SSL_load_error_strings();
    const SSL_METHOD *method = server_mode ? TLS_server_method() : TLS_client_method();
    g_ssl_ctx = SSL_CTX_new(method);
    if (!g_ssl_ctx) return -1;
    if (!server_mode) {
        /* client: do not verify server cert by default (documented) */
        SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);
        return 0;
    }
    if (ssl_cert && SSL_CTX_use_certificate_file(g_ssl_ctx, ssl_cert, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "Error: cannot load certificate %s\n", ssl_cert);
        return -1;
    }
    if (ssl_key_file && SSL_CTX_use_PrivateKey_file(g_ssl_ctx, ssl_key_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "Error: cannot load private key %s\n", ssl_key_file);
        return -1;
    }
    if (ssl_cert && ssl_key_file &&
        SSL_CTX_check_private_key(g_ssl_ctx) != 1) {
        fprintf(stderr, "Error: private key does not match certificate\n");
        return -1;
    }
    if (!ssl_cert || !ssl_key_file) {
        fprintf(stderr, "Error: SSL server mode requires -c cert and -j key\n");
        return -1;
    }
    return 0;
#else
    fprintf(stderr, "Error: SSL support not compiled in. Build with: gcc -DWITH_SSL minicat.c -o minicat -lssl -lcrypto\n");
    return -1;
#endif
}

/* ------------------------- Main ---------------------------------- */

static void on_signal(int sig) {
    if (sig == SIGPIPE) return;         /* ignore SIGPIPE: handle write errors */
    running = 0;
}

/* Reap fork-mode children asynchronously (async-signal-safe: waitpid loop). */
static void on_child(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) { /* reap until none left */ }
}

int main(int argc, char *argv[]) {
    int opt, port = 0, port_set = 0;
    char *host = NULL;

    /* NOTE: optstring now includes every advertised flag. */
    while ((opt = getopt(argc, argv, "lup:e:kvxnKWHSPEgL:T:Fhc:j:A:")) != -1) {
        switch (opt) {
            case 'l': server_mode = 1; break;
            case 'u': udp_mode = 1; break;
            case 'p': {
                /* Full-string strtol: reject junk like "80x", "abc" */
                char *ep = NULL;
                errno = 0;
                long v = strtol(optarg, &ep, 10);
                if (errno != 0 || ep == optarg || *ep != '\0' || v < 1 || v > 65535) {
                    fprintf(stderr, "Error: Port must be an integer 1-65535 (got: %s)\n", optarg);
                    return 1;
                }
                port = (int)v;
                port_set = 1;
                break;
            }
            case 'e':
                exec_mode = 1;
                if (!validate_exec_cmd(optarg)) {
                    fprintf(stderr, "Error: Exec command contains unsafe characters\n");
                    fprintf(stderr, "Allowed: A-Z a-z 0-9 space / . _ - : = + , @ %%\n");
                    return 1;
                }
                exec_cmd = strndup(optarg, MAX_CMD_LEN);
                if (!exec_cmd) { fprintf(stderr, "Error: Out of memory\n"); return 1; }
                break;
            case 'k': keep_open = 1; break;
            case 'v': verbose = 1; break;
            case 'x': hex_dump = 1; break;
            case 'n': tcp_nodelay = 1; break;
            case 'K': chat_mode = 1; break;
            case 'H': http_mode = 1; break;
            case 'S': ssl_enabled = 1; break;
            case 'W': ws_mode = 1; http_mode = 1; break;
            case 'P': proxy_mode = 1; break;
            case 'E': encrypt_mode = 1; break;
            case 'g': stats_enabled = 1; http_mode = 1; break;
            case 'c':
                if (strlen(optarg) >= MAX_OPTARG) {
                    fprintf(stderr, "Error: Certificate path too long\n");
                    return 1;
                }
                ssl_cert = optarg;
                break;
            case 'j':
                if (strlen(optarg) >= MAX_OPTARG) {
                    fprintf(stderr, "Error: Key path too long\n");
                    return 1;
                }
                ssl_key_file = optarg;
                break;
            case 'A':
                if (strlen(optarg) >= 32) {
                    fprintf(stderr, "Error: XOR key must be < 32 chars\n");
                    return 1;
                }
                memcpy(xor_key, optarg, strlen(optarg));
                xor_key_set = 1;
                break;
            case 'L':
                log_file = fopen(optarg, "a");
                if (!log_file) {
                    fprintf(stderr, "Error: Cannot open log file: %s\n", optarg);
                    return 1;
                }
                logging_enabled = 1;
                break;
            case 'T':
                rate_limit_enabled = 1;
                rate_limit_rps = atoi(optarg);
                if (rate_limit_rps <= 0) rate_limit_rps = 1;
                if (rate_limit_rps > 100000) rate_limit_rps = 100000;
                break;
            case 'F': fork_mode = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if (argc > optind) {
        if (!server_mode && argc > optind + 1) {
            /* client mode: exactly host + port */
            host = argv[optind];
            char *ep = NULL;
            errno = 0;
            long v = strtol(argv[optind + 1], &ep, 10);
            if (errno != 0 || ep == argv[optind + 1] || *ep != '\0' ||
                v < 1 || v > 65535) {
                fprintf(stderr, "Error: Invalid port '%s' (must be 1-65535)\n",
                        argv[optind + 1]);
                return 1;
            }
            port = (int)v;
            if (argc > optind + 2) {
                fprintf(stderr, "Error: Too many arguments\n");
                return 1;
            }
        } else {
            /* server mode: positional port; client with bare port handled
             * as server-style positional below when no host given */
            if (port_set) {
                fprintf(stderr, "Error: Port specified twice (-p and positional)\n");
                return 1;
            }
            char *ep = NULL;
            errno = 0;
            long v = strtol(argv[optind], &ep, 10);
            if (errno != 0 || ep == argv[optind] || *ep != '\0' ||
                v < 1 || v > 65535) {
                fprintf(stderr, "Error: Invalid port '%s' (must be 1-65535)\n",
                        argv[optind]);
                return 1;
            }
            port = (int)v;
            if (argc > optind + 1) {
                fprintf(stderr, "Error: Too many arguments\n");
                return 1;
            }
        }
    }

    /* Fuzz hook (test-only): MINICAT_FUZZ_FILE=<path> feeds the file bytes
     * through the real per-connection handler via a socketpair, then exits.
     * Runs BEFORE validation so no port/args are required; flags parsed
     * above (e.g. -H, -W) still select the code path under test.
     * Completely inert when unset. */
    const char *fz = getenv("MINICAT_FUZZ_FILE");
    if (fz) {
        /* Writes to the closed peer would otherwise SIGPIPE (the normal
         * signal setup happens later in main()). */
        signal(SIGPIPE, SIG_IGN);
        int sv[2];
        FILE *fp = fopen(fz, "rb");
        if (!fp) { fprintf(stderr, "Error: cannot open fuzz file %s\n", fz); return 1; }
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            fclose(fp);
            fprintf(stderr, "Error: socketpair failed\n");
            return 1;
        }
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        if (n > 0 && write(sv[0], buf, n) < 0) { /* ignore */ }
        close(sv[0]);
        struct sockaddr_in6 dummy;
        memset(&dummy, 0, sizeof(dummy));
        conn_t *c = conn_new(sv[1], &dummy);
        if (c) {
            connection_loop(c);
            conn_destroy_plain(c);
        }
        close(sv[1]);
        return 0;
    }

    /* Validation */
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port %d (must be 1-65535)\n", port);
        return 1;
    }
    if (encrypt_mode && !xor_key_set) {
        fprintf(stderr, "Error: -E requires an encryption key (-A key)\n");
        return 1;
    }
    if (ssl_enabled && udp_mode) {
        fprintf(stderr, "Error: -S cannot be combined with -u\n");
        return 1;
    }
    if (ssl_enabled && ssl_global_init() != 0) return 1;
    if (proxy_mode && (http_mode || ws_mode)) {
        fprintf(stderr, "Error: -P cannot be combined with -H/-W/-g\n");
        return 1;
    }
    if (udp_mode && (http_mode || ws_mode || proxy_mode)) {
        fprintf(stderr, "Error: -u cannot be combined with -H/-W/-g/-P\n");
        return 1;
    }
    if (chat_mode && fork_mode) {
        fprintf(stderr, "Error: -K cannot be combined with -F (chat needs the event loop)\n");
        return 1;
    }
    if (chat_mode && proxy_mode) {
        fprintf(stderr, "Error: -K cannot be combined with -P\n");
        return 1;
    }
    if (chat_mode && ssl_enabled) {
        fprintf(stderr, "Error: -K cannot be combined with -S (SSL uses per-connection processes)\n");
        return 1;
    }

    /* Signals */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    /* Reap fork-mode children (and SSL/proxy per-connection processes);
     * without this, -F long-running servers accumulate zombies. */
    signal(SIGCHLD, on_child);

    /* Idle timeout override (env) with sane bounds */
    const char *env_idle = getenv("MINICAT_HTTP_IDLE");
    if (env_idle) {
        long v = strtol(env_idle, NULL, 10);
        if (v >= MIN_HTTP_IDLE && v <= MAX_HTTP_IDLE) http_idle_sec = v;
    }

    if (server_mode) {
        listen_fd = create_socket(udp_mode ? SOCK_DGRAM : SOCK_STREAM, port);
        if (listen_fd < 0) return 1;
        if (udp_mode) return run_udp_server(port);
        return run_server(port);
    } else {
        if (!host) {
            fprintf(stderr, "Error: Host required in client mode\n");
            return 1;
        }
        return run_client(host, port);
    }
}

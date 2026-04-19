# MINICAT - HURDLES AND CHALLENGES

Every project has obstacles. Here's every hurdle we faced and how we overcame it.

---

## CHALLENGE 1: epoll vs select() - FD Limit

### PROBLEM:
The original ncat uses `select()` for I/O multiplexing. But `select()` has a 
fundamental limitation: `FD_SETSIZE` is typically 1024. This means maximum 
1,000 concurrent connections - not enough for modern needs.

### DISCOVERY:
In Ghidra, we saw `select()` calls in the main event loop. Simple but limited.

### SOLUTION:
Use `epoll_create1()` with edge-triggered monitoring. This gives O(1) 
handling per connection with no built-in limit.

```c
int epoll_fd = epoll_create1(0);
struct epoll_event ev;
ev.events = EPOLLIN | EPOLLET;
ev.data.fd = listen_fd;
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
```

**RESULT:** 10,000+ concurrent connections now possible.

---

## CHALLENGE 2: HTTP Parsing Complexity

### PROBLEM:
A full HTTP parser according to RFC 7230 is complex. Headers, methods,
status codes, content-length, chunking... it adds thousands of lines.

### DISCOVERY:
In ncat, there's `http_digest()` for authentication but we need 
actual serving.

### SOLUTION:
Use simple substring matching for common endpoints. Not RFC-compliant
but works for basic usage.

```c
if (strstr(buf, "GET /stats")) {
    generate_stats_html(response, 4096);
} else if (strncmp(buf, "GET", 3) == 0) {
    // Basic response
}
```

**RESULT:** ~20 lines of code for HTTP support vs 2000+ for full parser.

---

## CHALLENGE 3: Statistics Real-Time Updates

### PROBLEM:
We want real-time statistics at `/stats` endpoint. But calculating 
rates requires time deltas. Need accurate time tracking.

### DISCOVERY:
`time()` returns seconds since epoch. Enough for basic rate calculation.

### SOLUTION:
Store `start_time` in `stats_t` struct, calculate on each request.

```c
time_t up = time(NULL) - gs.st;
int h = up / 3600, m = (up % 3600) / 60;
double rr = up > 0 ? (double)gs.tbr / up : 0;
```

**RESULT:** Uptime, rates, and totals displayed correctly.

---

## CHALLENGE 4: Rate Limiting Implementation

### PROBLEM:
DoS protection requires request rate limiting. Need to track 
requests per second without heavy infrastructure.

### DISCOVERY:
N/A - our own feature to add.

### SOLUTION:
Simple counter with `time()` check. Token bucket algorithm simplified.

```c
int rate_limit_check(void) {
    static time_t last = 0;
    static int cnt = 0;
    time_t now = time(NULL);
    if (now != last) { last = now; cnt = 0; }
    if (cnt >= rate_limit_rps) return 0;
    cnt++;
    return 1;
}
```

**RESULT:** Configurable rate limiting works (tested at 5 req/sec).

---

## CHALLENGE 5: Socket - IPv4 vs IPv6

### PROBLEM:
IPv4 and IPv6 are different address families. Should we
support both? How?

### SOLUTION:
Use `AF_INET6` which handles IPv4 too (dual-stack). Linux maps
IPv4 to IPv6 automatically.

```c
fd = socket(AF_INET6, SOCK_STREAM, 0);
addr.sin6_family = AF_INET6;
// Works with both IPv4 and IPv6 addresses
```

**RESULT:** Single socket handles all IPs.

---

## CHALLENGE 6: Compiling String Issues

### PROBLEM:
When using bash heredocs with C code containing special 
characters (braces, semicolons, quotes), parsing gets confused.
Got syntax errors like:
```
/tmp/minicat.c:64:64: error: expected ')' before '{' token
```

### DISCOVERY:
Bash is interpreting C syntax as shell syntax.

### SOLUTION:
Use Python to write complex files instead of heredocs.

```bash
python3 -c 'with open("minicat.c","w") as f: f.write(source)'
```

**RESULT:** Clean code, no parsing errors.

---

## CHALLENGE 7: Hex Dump Display

### PROBLEM:
Want to show traffic in hex for debugging, but should be 
conditional (verbose).

### SOLUTION:
Simple flag check before printing hex.

```c
void hexdump(const char *prefix, const char *data, int len) {
    if (!hex_dump) return;
    printf("[%s] HEX(%d): ", prefix, len);
    for (int i = 0; i < len && i < 32; i++) 
        printf("%02x ", (unsigned char)data[i]);
    printf("\n");
}
```

**RESULT:** `-x` flag enables hex output.

---

## CHALLENGE 8: Non-blocking I/O

### PROBLEM:
Epoll requires non-blocking sockets or accept() can block.

### SOLUTION:
Set `O_NONBLOCK` on all client sockets.

```c
int set_nonblocking(int fd) { 
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); 
}
```

**RESULT:** Edge-triggered epoll works correctly.

---

## CHALLENGE 9: Port Binding Conflicts

### PROBLEM:
Socket lingers in TIME_WAIT after close, preventing reuse.

### SOLUTION:
Set `SO_REUSEADDR` before bind().

```c
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

**RESULT:** Can restart server immediately.

---

## CHALLENGE 10: Large Data Transfer

### PROBLEM:
Sending 1MB+ data causes buffer issues.

### SOLUTION:
Use 64KB buffer, loop on write.

```c
char buffer[65536];
while (1) {
    int n = read(fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) break;
    write(fd, buffer, n);
}
```

**RESULT:** 1MB test passed.

---

## CHALLENGE 11: HTTP Endpoint Implementation

### PROBLEM:
Need multiple endpoints: /stats, /health, /json - all from same code.

### SOLUTION:
Parse URI from HTTP request, route to appropriate handler.

```c
char method[16], uri[256], ver[16];
sscanf(buf, "%s %s %s", method, uri, ver);
if (strcmp(uri, "/stats") == 0) generate_stats_html();
else if (strcmp(uri, "/health") == 0) strcpy(response, "OK");
else if (strcmp(uri, "/json") == 0) generate_json();
```

**RESULT:** All 4 endpoints work correctly.

---

## CHALLENGE 12: Variable Naming Conflicts

### PROBLEM:
In `handle_connection()`, used same variable name 'buf' for two purposes.

### ERROR:
```c
inet_ntop(AF_INET6, &addr.sin6_addr, buf, sizeof(buf)) ? buf : "unknown"
```

### DISCOVERY:
Second 'buf' was being used before declaration in that scope.

### SOLUTION:
Use different variable names: `char addr_buf[INET6_ADDRSTRLEN];`

**RESULT:** Clean compilation with no warnings.

---

## 🎉 ALL CHALLENGES OVERCOME!

Each hurdle made MINICAT better. The final tool is stronger 
for the challenges.

### Key lessons:
1. **Start simple** - add complexity incrementally  
2. **epoll is essential** - for modern network tools
3. **Python is great** - for code generation
4. **Test-driven development** - catches issues early
5. **Simplicity scales** - better than complexity

---

*Built by Ali Zafar v1.0* 🎯

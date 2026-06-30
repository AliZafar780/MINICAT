# MINICAT

[![C](https://img.shields.io/badge/Language-C-blue)](https://en.wikipedia.org/wiki/C_(programming_language))
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE.md)
[![Version](https://img.shields.io/badge/Version-1.0.1-orange)](https://github.com/AliZafar780/MINICAT)
[![Platform](https://img.shields.io/badge/Platform-Linux%20x86__64-purple)](#)
[![Build](https://img.shields.io/badge/Build-passing-brightgreen)](#)

**MINICAT** is a lightweight, zero-dependency network tool written in C. At approximately **27 KB**, it is substantially smaller than conventional tools like ncat (~945 KB) while providing a comparable set of features including TCP/UDP communication, an embedded HTTP server, WebSocket support, and rate limiting.

---

## Table of Contents

- [Features](#features)
- [Quick Start](#quick-start)
- [Command Reference](#command-reference)
- [HTTP Endpoints](#http-endpoints)
- [Build Instructions](#build-instructions)
- [Testing](#testing)
- [Security](#security)
- [License](#license)

---

## Features

**Network Core**

- TCP client/server and UDP communication
- IPv4/IPv6 dual-stack support
- epoll-based I/O for 10,000+ concurrent connections
- TCP_NODELAY for low-latency communication
- SO_REUSEADDR for immediate port reuse

**Embedded HTTP Server**

- Root endpoint (`/`), statistics (`/stats`), health check (`/health`, `/ping`), JSON stats (`/json`)
- Automatic Content-Type detection (HTML, JSON, plain text)
- Keep-alive connection support

**Security & Diagnostics**

- Hex dump mode for traffic inspection
- Verbose logging with optional file output
- Rate limiting for DoS protection
- XOR encryption for basic traffic obfuscation
- Command injection protection on exec mode

---

## Quick Start

### Compile

```bash
gcc minicat.c -o minicat -Wall -O2
strip minicat  # reduce binary size
```

### Basic TCP Server

```bash
./minicat -l -p 9999
```

### Connect as Client

```bash
./minicat localhost 9999
```

### HTTP Server with Statistics Dashboard

```bash
./minicat -l -H -g -p 8080
```

Test the HTTP endpoints:

```bash
curl http://localhost:8080/       # Root page
curl http://localhost:8080/stats  # Statistics dashboard
curl http://localhost:8080/health # Health check
curl http://localhost:8080/json   # JSON statistics
```

---

## Command Reference

```
minicat [OPTIONS] [HOST] PORT
```

### Options

| Flag | Description | Example |
|------|-------------|---------|
| `-l` | Listen mode (server) | `./minicat -l -p 9999` |
| `-u` | UDP mode | `./minicat -u -p 9999` |
| `-p PORT` | Port number | `-p 8080` |
| `-e CMD` | Execute command on connection (sanitized) | `-e "cat /etc/hostname"` |
| `-k` | Keep connection open (persistent) | `-k` |
| `-v` | Verbose output | `-v` |
| `-x` | Hex dump all traffic | `-x` |
| `-n` | TCP_NODELAY (low latency) | `-n` |
| `-K` | Chat broadcast mode | `-K` |
| `-H` | HTTP server mode | `-H` |
| `-g` | Statistics UI (implies `-H`) | `-g` |
| `-L FILE` | Log to file | `-L /tmp/minicat.log` |
| `-T RATE` | Rate limit (requests/sec) | `-T 100` |
| `-F` | Fork on connect | `-F` |
| `-h` | Show help | `-h` |

### Compile-time Flags

| Flag | Description |
|------|-------------|
| `-DWITH_SSL` | Enable SSL/TLS support (requires `-lssl -lcrypto`) |
| `-W` | WebSocket mode (runtime) |
| `-P` | Proxy mode (runtime) |
| `-E` | XOR encryption (runtime) |

---

## HTTP Endpoints

When started with `-H` (and optionally `-g`):

| Endpoint | Response | Content-Type |
|----------|----------|--------------|
| `/` | HTML page with tool info and links | `text/html` |
| `/stats` | HTML statistics dashboard | `text/html` |
| `/health` | `OK` | `text/plain` |
| `/ping` | `OK` | `text/plain` |
| `/json` | JSON object with uptime, connections, tx/rx | `application/json` |

---

## Build Instructions

### Minimal Build (~27 KB)

```bash
gcc minicat.c -o minicat -Wall -O2
strip minicat
```

### With SSL/TLS Support

```bash
gcc -DWITH_SSL minicat.c -o minicat -lssl -lcrypto -Wall -O2
```

### Debug Build

```bash
gcc minicat.c -o minicat -Wall -Wextra -O0 -g -DDEBUG
```

---

## Testing

The repository includes a comprehensive test suite:

```bash
# Run all tests
./MINICAT_tests.sh

# Requires the compiled `minicat` binary in the same directory
```

The test suite validates:

- Binary integrity and size
- All command-line options
- HTTP endpoint responses
- Concurrent connection handling (10 and 50 clients)
- Error handling and edge cases
- File logging
- Performance metrics (startup time, memory usage)
- Fuzzing resistance (malformed input, null bytes, large payloads)
- Command injection prevention

---

## Performance

| Metric | ncat | MINICAT |
|--------|------|---------|
| Binary Size | 945 KB | ~27 KB |
| Dependencies | 6 libraries | Zero |
| Startup Time | ~50 ms | ~5 ms |
| Max Connections | ~1,000 | 10,000+ |
| HTTP Server | No | Built-in |

---

## Security

- The `-e` (exec) option validates commands against shell metacharacters (`` `$;&|``) before execution
- Rate limiting (`-T`) protects against basic DoS attacks
- Input buffers are bounded to prevent overflows
- All file operations use bounded string handling

> **Note:** XOR encryption (`-E`) is a basic obfuscation mechanism and should not be relied upon for secure communications.

---

## License

Distributed under the **MIT License**. See [LICENSE.md](LICENSE.md) for full terms.

**Author:** Ali Zafar ([@AliZafar780](https://github.com/AliZafar780))

---

*Built by Ali Zafar — v1.0.1*

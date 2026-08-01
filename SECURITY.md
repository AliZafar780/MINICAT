# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 2.x     | :white_check_mark: |
| 1.x     | :white_check_mark: |
| < 1.0   | :x:                |

## Reporting a Vulnerability

If you discover a security vulnerability, please report it responsibly.

**DO NOT** create a public GitHub issue for the vulnerability.

Instead, please send an email to the repository owner via GitHub's private vulnerability reporting feature.

We will acknowledge receipt within 48 hours and provide a detailed response within 5 business days.

## Disclosure Policy

- We will investigate all legitimate reports and make every effort to resolve them quickly.
- We ask that you give us a reasonable time to fix the issue before any disclosure.
- We will publicly credit you for the discovery when the fix is released (if desired).

## Hardening Summary (v2.0.0)

Verified by GOD SYNDICATE OMNI, 2026-08-01:

- **Buffer overflows**: All input paths bounded; HTTP URI limited to 4000 chars with clean 4xx on overflow (previously SIGABRT)
- **Memory safety**: ASAN+UBSAN clean across all 23 adversarial test paths
- **SIGPIPE**: `SIG_IGN` + `EPIPE` handling — server survives client RST
- **stdin EOF**: POLLHUP/POLLERR treated as EOF — clean client exit
- **Data integrity**: Full-buffer drain loops — 300 KB bursts relayed lossless
- **Command injection**: `-e` rejects shell metacharacters (`` ` $ ; & | > ``) at CLI parse time
- **DoS**: `-T` rate limiting; keep-alive bounded
- **TLS**: Optional `-DWITH_SSL` build for confidentiality (XOR `-E` is obfuscation only)
- **CI**: builds + battery run on push/PR (ubuntu-latest, macos-latest)
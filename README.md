![logo](https://github.com/wujin1989/Xylem/blob/main/docs/images/Xylem-logo.png)
[![CMake on multiple platforms](https://github.com/wujin1989/Xylem/actions/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/wujin1989/Xylem/actions/workflows/cmake-multi-platform.yml)
[![CodeQL Advanced](https://github.com/wujin1989/Xylem/actions/workflows/codeql.yml/badge.svg)](https://github.com/wujin1989/Xylem/actions/workflows/codeql.yml)
[![CodeFactor](https://www.codefactor.io/repository/github/wujin1989/xylem/badge)](https://www.codefactor.io/repository/github/wujin1989/xylem)

# Overview
Xylem is a pure C library, **supplementing** — not replacing — the C11 standard.


# Features

- Pure C11, no external dependencies (OpenSSL optional for TLS/DTLS)
- Cross-platform: Windows (MSVC), Linux/macOS (GCC, Clang)
- Single-threaded event loop with I/O polling, timers
- TCP client/server with framing, write queue, reconnect, heartbeat, timeouts
- UDP listen/dial modes with datagram boundary preservation
- TLS over TCP via OpenSSL memory BIO (ALPN, SNI, keylog)
- DTLS over UDP via OpenSSL memory BIO (cookie exchange, session multiplexing)
- Reliable UDP over KCP (ARQ, congestion control, session multiplexing)
- HTTP/1.1 client and server
- WebSocket client/server (RFC 6455)
- Intrusive and non-intrusive data structures (list, queue, stack, heap, rbtree, ring buffer)
- Base64, SHA-1, SHA-256, gzip, JSON, varint
- Thread pool, async logger, waitgroup


# Build

```bash
cmake -B out
cmake --build out
```

See [docs/build.md](docs/build.md) for detailed instructions on generators, sanitizers, and coverage.


# Documentation

See the [docs/](docs/) directory for design documents and build instructions.


# License
```
Copyright (c) 2026–2036, Jin.Wu <wujin.developer@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

![logo](https://github.com/MeristemForge/Xylem/blob/main/docs/images/xylem-banner-animated.png)
[![CMake on multiple platforms](https://github.com/MeristemForge/Xylem/actions/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/MeristemForge/Xylem/actions/workflows/cmake-multi-platform.yml)
[![CodeQL Advanced](https://github.com/MeristemForge/Xylem/actions/workflows/codeql.yml/badge.svg)](https://github.com/MeristemForge/Xylem/actions/workflows/codeql.yml)
[![CodeFactor](https://www.codefactor.io/repository/github/meristemforge/xylem/badge)](https://www.codefactor.io/repository/github/meristemforge/xylem)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C11](https://img.shields.io/badge/Standard-C11-green.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))

# Overview
Xylem is a pure C library, **supplementing** — not replacing — the C11 standard.


# Features

- **Cross-platform** — Windows, Linux, macOS, Android, and iOS
- **Easy to use** — small, consistent APIs; get started in minutes
- **High-performance event loop** — epoll / kqueue / iocp
- **Thread-safe networking** — send and close from any thread without locking
- **Protocol coverage** — TCP, UDP, UDS, TLS, DTLS, Reliable UDP, HTTP/1.1, WebSocket, Serial


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

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Architecture

Xylem is a cross-platform C11 static library providing data structures, crypto primitives, concurrency utilities, and async networking.

### Event Loop (`xylem_loop_t`)

Central primitive that all networking modules (TCP, UDP, UDS, TLS, DTLS, RUDP, HTTP, WS) build upon. Provides I/O readiness callbacks, heap-based timers, and thread-safe deferred execution via `xylem_loop_post()` (lock-free MPSC queue in `src/mpsc.c` + wakeup fd).

### Thread Safety Model

- `send` and `close` on connections are safe from any thread — they marshal to the loop thread via post queue
- Connections use reference counting (`acquire`/`release`) to prevent use-after-free across threads

### Protocol Stack

```
HTTP / WebSocket
    └── Transport interface (http-transport.h / ws-transport.h)
            ├── TCP transport
            └── TLS transport (or stub when TLS disabled)
TLS / DTLS
    └── OpenSSL (optional, gated by XYLEM_ENABLE_TLS)
RUDP
    └── KCP (bundled) + FEC (Reed-Solomon)
TCP / UDP / UDS
    └── Event loop + platform sockets
```

### Data Structures

Two flavors: **intrusive** (list, stack, queue, heap, rbtree) where user embeds a node and recovers container via `xylem_<mod>_entry()`, and **non-intrusive** (`x`-prefixed) allocating wrappers built on top of the intrusive variants.

## Design Docs

Before modifying a networking module, read its design doc at `docs/<module>-design.md` and test design at `docs/<module>-test-design.md`.

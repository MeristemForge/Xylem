---
inclusion: auto
description: "Xylem project overview and module inventory"
---

# Xylem

Xylem is a pure C utility library that supplements (not replaces) the C11 standard. It provides cross-platform, reusable data structures and algorithms as a static or shared library.

## Current Modules

### Data Structures

| Module | Description | Style |
|--------|-------------|-------|
| **list** | Doubly-linked list | Intrusive |
| **stack** | LIFO stack | Intrusive |
| **queue** | FIFO queue (built on list) | Intrusive |
| **heap** | Binary min-heap | Intrusive |
| **rbtree** | Red-black tree (key & node-node comparators) | Intrusive |
| **xlist** | Non-intrusive doubly-linked list (built on list) | Non-intrusive |
| **xstack** | Non-intrusive LIFO stack (built on stack) | Non-intrusive |
| **xqueue** | Non-intrusive FIFO queue (built on queue) | Non-intrusive |
| **xheap** | Non-intrusive binary min-heap (built on heap) | Non-intrusive |
| **xrbtree** | Non-intrusive red-black tree (built on rbtree) | Non-intrusive |
| **ringbuf** | Ring buffer | Contiguous |

### Encoding & Hashing

| Module | Description |
|--------|-------------|
| **base64** | Standard and URL-safe Base64 (RFC 4648) |
| **bswap** | Byte-swap utilities |
| **varint** | Variable-length integer encoding |
| **sha1** | SHA-1 hash |
| **sha256** | SHA-256 hash |

### Concurrency

| Module | Description | Status |
|--------|-------------|--------|
| **waitgroup** | Thread synchronization primitive | ✅ |
| **thrdpool** | Thread pool | ✅ |
| **logger** | Leveled logging with optional async output | ✅ |

### Networking & Event Loop

| Module | Description | Status |
|--------|-------------|--------|
| **addr** | Unified network address wrapper (IPv4/IPv6) | ✅ |
| **loop** | Event loop with I/O, timer, and post callbacks (built on platform poller) | ✅ |
| **tcp** | TCP client/server with framing, reconnect, heartbeat | ✅ |
| **udp** | UDP datagram send/receive | ✅ |
| **tls** | TLS client/server over TCP (OpenSSL, ALPN, SNI) | ✅ |
| **dtls** | DTLS client/server over UDP (OpenSSL, cookie verification) | ✅ |

### Utilities

| Module | Description |
|--------|-------------|
| **utils** | Time, endianness detection, PRNG helpers |

## Design Philosophy

- Zero external dependencies beyond C11 stdlib
- Intrusive data structures — embed `xylem_<module>_node_t`, recover container via `xylem_*_entry()`
- Error codes (typically `-1`) over exceptions or global state
- Cross-platform: Windows (MSVC) + Unix (GCC/Clang)

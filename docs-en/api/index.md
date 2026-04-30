# API Reference

The Xylem SDK public API is organized into the following modules. Click a module name for detailed documentation.

## Initialization

Before using any xylem API, call `xylem_startup()`; call `xylem_cleanup()` before exit.

```c
#include <xylem.h>

int main(void) {
    xylem_startup();
    // ... use xylem ...
    xylem_cleanup();
    return 0;
}
```

| Function | Description |
|----------|-------------|
| `int xylem_startup(void)` | Initialize the library. Calls WSAStartup on Windows. Returns 0 on success. |
| `void xylem_cleanup(void)` | Clean up the library. Must be called after all resources are released. |

## Module Overview

### Core

| Module | Header | Description |
|--------|--------|-------------|
| [Event Loop](loop.md) | `xylem-loop.h` | I/O multiplexing, timers, cross-thread posting |
| [Address](addr.md) | `xylem-addr.h` | Address resolution and conversion |
| [Platform](platform.md) | `xylem-platform.h` | Platform detection macros |
| [Thread Pool](thrdpool.md) | `xylem-thrdpool.h` | Fixed-size thread pool |

### Transport

| Module | Header | Description |
|--------|--------|-------------|
| [TCP](tcp.md) | `xylem-tcp.h` | Async TCP with built-in framing, reconnection, heartbeat |
| [UDP](udp.md) | `xylem-udp.h` | Async UDP, listen / dial dual mode |
| [UDS](uds.md) | `xylem-uds.h` | Unix Domain Socket |
| [TLS](tls.md) | `xylem-tls.h` | TLS encrypted transport (OpenSSL) |
| [DTLS](dtls.md) | `xylem-dtls.h` | DTLS encrypted datagrams (OpenSSL) |
| [RUDP](rudp.md) | `xylem-rudp.h` | Reliable UDP (KCP + FEC + AES-256) |

### Application

| Module | Header | Description |
|--------|--------|-------------|
| [HTTP Common](http-common.md) | `xylem-http-common.h` | HTTP request/response type definitions |
| [HTTP Client](http-client.md) | `xylem-http-client.h` | Async HTTP client |
| [HTTP Server](http-server.md) | `xylem-http-server.h` | Async HTTP server |
| [WebSocket Common](ws-common.md) | `xylem-ws-common.h` | WebSocket frame type definitions |
| [WebSocket Client](ws-client.md) | `xylem-ws-client.h` | Async WebSocket client |
| [WebSocket Server](ws-server.md) | `xylem-ws-server.h` | Async WebSocket server |

### Device

| Module | Header | Description |
|--------|--------|-------------|
| [Serial](serial.md) | `xylem-serial.h` | Cross-platform synchronous serial port |

### Utilities

| Module | Header | Description |
|--------|--------|-------------|
| [JSON](json.md) | `xylem-json.h` | JSON parsing and generation (yyjson) |
| [Gzip](gzip.md) | `xylem-gzip.h` | Gzip compression/decompression (miniz) |
| [Base64](base64.md) | `xylem-base64.h` | Base64 encoding/decoding |
| [AES-256](aes256.md) | `xylem-aes256.h` | AES-256-CTR encryption |
| [SHA / HMAC](sha.md) | `xylem-sha1.h` `xylem-sha256.h` `xylem-hmac256.h` | Hash and message authentication |
| [FEC](fec.md) | `xylem-fec.h` | Reed-Solomon forward error correction |
| [Varint](varint.md) | `xylem-varint.h` | Variable-length integer encoding |
| [Logger](logger.md) | `xylem-logger.h` | Leveled log output |

### Data Structures

| Module | Header | Description |
|--------|--------|-------------|
| [List](list.md) | `xylem-list.h` | Intrusive doubly-linked list |
| [Queue](queue.md) | `xylem-queue.h` | Intrusive FIFO queue |
| [Stack](stack.md) | `xylem-stack.h` | Intrusive stack |
| [Heap](heap.md) | `xylem-heap.h` | Intrusive binary min-heap |
| [Red-Black Tree](rbtree.md) | `xylem-rbtree.h` | Intrusive red-black tree |
| [Ring Buffer](ringbuf.md) | `xylem-ringbuf.h` | Fixed-size ring buffer |

## Conventions

- All async operations notify results via handler callbacks
- Functions marked **thread-safe** (`send`, `close`) may be called from any thread
- Other APIs must be called on the event loop thread
- Return `0` for success, `-1` for failure (unless documented otherwise)
- `NULL` pointer parameters mean "use defaults" where documented

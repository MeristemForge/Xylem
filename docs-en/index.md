# Xylem SDK

<p align="center">
  <img src="../docs/images/xylem-banner-animated.png" alt="Xylem" width="600">
</p>

**Xylem** is a pure C async networking SDK providing a complete networking stack from event loop to application-layer protocols.

[中文文档](../){ .md-button }

---

## Features

| Category | Modules |
|----------|---------|
| **Core** | Event loop (epoll / kqueue / IOCP), thread pool, timers |
| **Transport** | TCP, UDP, UDS, TLS, DTLS, RUDP (KCP + FEC + AES) |
| **Application** | HTTP/1.1 client/server, WebSocket client/server |
| **Device** | Cross-platform serial port |
| **Utilities** | JSON, Gzip, Base64, SHA-1/256, HMAC-256, AES-256, Varint |
| **Data Structures** | List, queue, stack, heap, red-black tree, ring buffer |

## Design Principles

- **Zero external dependencies** (core) — TLS/DTLS optionally requires OpenSSL
- **Pure C11** — compatible with GCC, Clang, and MSVC
- **Cross-platform** — Linux, macOS, Windows, extensible to Android / iOS
- **Callback-driven** — all I/O notified via handler callbacks, never blocks
- **Thread-safe** — `send` and `close` callable from any thread
- **Modular** — compile only what you need

## Quick Example

### TCP Echo Server

```c
#include <xylem.h>
#include <stdio.h>

static void on_accept(xylem_tcp_server_t* server, xylem_tcp_conn_t* conn) {
    printf("new connection\n");
}

static void on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    xylem_tcp_send(conn, data, len);  // echo back
}

static void on_close(xylem_tcp_conn_t* conn, int err, const char* msg) {
    printf("closed: %s\n", msg ? msg : "normal");
}

int main(void) {
    xylem_startup();

    xylem_loop_t* loop = xylem_loop_create();
    xylem_addr_t addr;
    xylem_addr_pton("0.0.0.0", 8080, &addr);

    xylem_tcp_handler_t handler = {
        .on_accept = on_accept,
        .on_read   = on_read,
        .on_close  = on_close,
    };

    xylem_tcp_server_t* server = xylem_tcp_listen(loop, &addr, &handler, NULL);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
```

## License

[MIT License](https://opensource.org/licenses/MIT) — free for commercial and open-source use.

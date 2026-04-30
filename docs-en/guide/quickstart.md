# Quick Start

This guide walks through xylem's core concepts with simple examples.

## Core Concepts

All networking in xylem revolves around three ideas:

1. **Event loop** (`xylem_loop_t`) — drives all async I/O
2. **Handler callbacks** — struct of function pointers for event notification
3. **Handles** (conn / server / udp) — represent a network resource

```
xylem_startup()
    └── xylem_loop_create()
            ├── xylem_tcp_listen() / xylem_tcp_dial()
            ├── xylem_udp_listen() / xylem_udp_dial()
            ├── xylem_tls_listen() / xylem_tls_dial()
            └── ...
            │
            xylem_loop_run()   ← blocks until all handles closed or stop
            │
    └── xylem_loop_destroy()
xylem_cleanup()
```

## Example 1: TCP Echo Client

```c
#include <xylem.h>
#include <stdio.h>
#include <string.h>

static void on_connect(xylem_tcp_conn_t* conn) {
    printf("connected!\n");
    xylem_tcp_send(conn, "hello", 5);
}

static void on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    printf("received: %.*s\n", (int)len, (char*)data);
    xylem_tcp_close(conn);
}

static void on_close(xylem_tcp_conn_t* conn, int err, const char* msg) {
    printf("closed\n");
    xylem_loop_stop(xylem_tcp_get_loop(conn));
}

int main(void) {
    xylem_startup();
    xylem_loop_t* loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", 8080, &addr);

    xylem_tcp_handler_t handler = {
        .on_connect = on_connect,
        .on_read    = on_read,
        .on_close   = on_close,
    };

    xylem_tcp_dial(loop, &addr, &handler, NULL);
    xylem_loop_run(loop);

    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
```

## Example 2: UDP Send/Receive

```c
#include <xylem.h>
#include <stdio.h>

static void on_read(xylem_udp_t* udp, void* data, size_t len,
                    xylem_addr_t* addr) {
    printf("received %zu bytes\n", len);
    xylem_udp_send(udp, addr, data, len);  // echo back
}

int main(void) {
    xylem_startup();
    xylem_loop_t* loop = xylem_loop_create();

    xylem_addr_t addr;
    xylem_addr_pton("0.0.0.0", 9000, &addr);

    xylem_udp_handler_t handler = { .on_read = on_read };
    xylem_udp_listen(loop, &addr, &handler);

    xylem_loop_run(loop);
    xylem_loop_destroy(loop);
    xylem_cleanup();
    return 0;
}
```

## Example 3: TCP Framing

Xylem has built-in frame extraction, automatically splitting byte streams into complete messages:

```c
// Fixed 4-byte frames
xylem_tcp_opts_t opts = {
    .framing = {
        .type = XYLEM_TCP_FRAME_FIXED,
        .fixed.frame_size = 4,
    },
};

// on_read callback guaranteed to receive exactly 4 bytes each time
static void on_read(xylem_tcp_conn_t* conn, void* data, size_t len) {
    // len == 4, always
}
```

Supported framing strategies:

| Strategy | Description |
|----------|-------------|
| `FRAME_NONE` | No framing, raw byte stream |
| `FRAME_FIXED` | Fixed-length frames |
| `FRAME_LENGTH` | Length-prefixed frames (big/little endian, varint) |
| `FRAME_DELIM` | Delimiter-separated frames (e.g. `\r\n`) |
| `FRAME_CUSTOM` | User-provided parse function |

## Example 4: Cross-Thread Send

```c
static void on_connect(xylem_tcp_conn_t* conn) {
    // Acquire on the event loop thread
    xylem_tcp_conn_acquire(conn);

    // Pass to worker thread
    start_worker_thread(conn);
}

// In the worker thread:
void worker(xylem_tcp_conn_t* conn) {
    xylem_tcp_send(conn, "hello from worker", 17);  // thread-safe
    xylem_tcp_conn_release(conn);  // release when done
}
```

## Next Steps

- [API Reference](../api/index.md) — complete function documentation
- [Examples](https://github.com/user/xylem/tree/main/examples) — full working examples

# WebSocket Common

`#include <xylem/ws/xylem-ws-common.h>`

WebSocket 公共类型，供客户端和服务端共享。包括操作码、连接状态、事件回调和连接选项。

---

## 类型

### xylem_ws_opcode_t {#xylem_ws_opcode_t}

WebSocket 操作码（RFC 6455 section 5.2）。

```c
typedef enum xylem_ws_opcode_e {
    XYLEM_WS_OPCODE_TEXT   = 0x1,
    XYLEM_WS_OPCODE_BINARY = 0x2,
} xylem_ws_opcode_t;
```

| 值 | 说明 |
|----|------|
| `OPCODE_TEXT` | 文本消息 |
| `OPCODE_BINARY` | 二进制消息 |

### xylem_ws_state_t {#xylem_ws_state_t}

WebSocket 连接状态。

```c
typedef enum xylem_ws_state_e {
    XYLEM_WS_STATE_CONNECTING,
    XYLEM_WS_STATE_OPEN,
    XYLEM_WS_STATE_CLOSING,
    XYLEM_WS_STATE_CLOSED,
} xylem_ws_state_t;
```

| 值 | 说明 |
|----|------|
| `STATE_CONNECTING` | 正在建立连接（TCP/TLS 连接或 HTTP Upgrade 握手进行中） |
| `STATE_OPEN` | 连接已建立，可收发消息 |
| `STATE_CLOSING` | 关闭握手进行中 |
| `STATE_CLOSED` | 连接已关闭 |

### xylem_ws_conn_t {#xylem_ws_conn_t}

不透明类型，表示一个 WebSocket 连接句柄。通过 [`xylem_ws_dial()`](ws-client.md#xylem_ws_dial) 或 [`on_accept`](#xylem_ws_handler_t) 回调获得。

### xylem_ws_server_t {#xylem_ws_server_t}

不透明类型，表示一个 WebSocket 服务器句柄。通过 [`xylem_ws_listen()`](ws-server.md#xylem_ws_listen) 获得。

### xylem_ws_handler_t {#xylem_ws_handler_t}

WebSocket 事件回调集合。

```c
typedef struct xylem_ws_handler_s {
    void (*on_open)(xylem_ws_conn_t* conn);
    void (*on_accept)(xylem_ws_conn_t* conn);
    void (*on_message)(xylem_ws_conn_t* conn,
                       xylem_ws_opcode_t opcode,
                       const void* data, size_t len);
    void (*on_ping)(xylem_ws_conn_t* conn,
                    const void* data, size_t len);
    void (*on_pong)(xylem_ws_conn_t* conn,
                    const void* data, size_t len);
    void (*on_close)(xylem_ws_conn_t* conn,
                     uint16_t code, const char* reason, size_t reason_len);
} xylem_ws_handler_t;
```

| 回调 | 触发时机 |
|------|---------|
| `on_open` | 客户端 WebSocket 握手完成 |
| `on_accept` | 服务端接受新 WebSocket 连接 |
| `on_message` | 收到完整消息。`opcode`: [`XYLEM_WS_OPCODE_TEXT`](#xylem_ws_opcode_t) 或 [`XYLEM_WS_OPCODE_BINARY`](#xylem_ws_opcode_t) |
| `on_ping` | 收到 ping 帧。`data`: 可选载荷（最多 125 字节） |
| `on_pong` | 收到 pong 帧。`data`: 可选载荷 |
| `on_close` | 收到关闭帧或连接关闭。`code`: 状态码，`reason`: UTF-8 原因字符串 |

### xylem_ws_opts_t {#xylem_ws_opts_t}

WebSocket 连接选项。

```c
typedef struct xylem_ws_opts_s {
    size_t   max_message_size;      /* 最大入站消息大小，0 = 无限制，默认 16 MiB */
    size_t   fragment_threshold;    /* 发送分片阈值，0 = 默认 16 KiB */
    uint64_t handshake_timeout_ms;  /* 握手超时，0 = 默认 10000 ms */
    uint64_t close_timeout_ms;      /* 关闭握手超时，0 = 默认 5000 ms */
} xylem_ws_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `max_message_size` | 0（无限制，默认 16 MiB） | 最大入站消息大小 |
| `fragment_threshold` | 0（默认 16 KiB） | 发送分片阈值，超过时自动分片 |
| `handshake_timeout_ms` | 0（默认 10000） | 握手超时（毫秒） |
| `close_timeout_ms` | 0（默认 5000） | 关闭握手超时（毫秒） |

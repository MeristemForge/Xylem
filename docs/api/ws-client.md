# WebSocket Client

`#include <xylem/ws/xylem-ws-client.h>`

异步 WebSocket 客户端，支持 ws:// 和 wss://（TLS）连接、自动分片和 ping/pong。

---

## 函数

### xylem_ws_dial {#xylem_ws_dial}

```c
xylem_ws_conn_t* xylem_ws_dial(xylem_loop_t* loop,
                               const char* url,
                               xylem_ws_handler_t* handler,
                               xylem_ws_opts_t* opts);
```

发起异步 WebSocket 连接。解析 URL scheme 选择传输层：`ws://` 使用 TCP，`wss://` 使用 TLS（需要 `XYLEM_ENABLE_TLS`）。执行 TCP/TLS 连接后进行 HTTP Upgrade 握手。成功后触发 `handler->on_open`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `url` | `const char*` | WebSocket URL（`ws://` 或 `wss://`） |
| `handler` | [`xylem_ws_handler_t*`](ws-common.md#xylem_ws_handler_t) | 事件回调集合 |
| `opts` | [`xylem_ws_opts_t*`](ws-common.md#xylem_ws_opts_t) | 连接选项，`NULL` 使用默认值 |

**返回值：** 连接句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_ws_send {#xylem_ws_send}

```c
int xylem_ws_send(xylem_ws_conn_t* conn,
                  xylem_ws_opcode_t opcode,
                  const void* data, size_t len);
```

发送 WebSocket 消息。从载荷构造一个或多个 WebSocket 帧。当载荷超过配置的分片阈值时，消息自动拆分为多个分片。客户端连接对每个帧应用随机掩码密钥。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_ws_conn_t*`](ws-common.md#xylem_ws_conn_t) | 连接句柄 |
| `opcode` | [`xylem_ws_opcode_t`](ws-common.md#xylem_ws_opcode_t) | 消息类型（`XYLEM_WS_OPCODE_TEXT` 或 `XYLEM_WS_OPCODE_BINARY`） |
| `data` | `const void*` | 载荷数据 |
| `len` | `size_t` | 载荷长度（字节） |

**返回值：** 0 成功，-1 错误。

---

### xylem_ws_ping {#xylem_ws_ping}

```c
int xylem_ws_ping(xylem_ws_conn_t* conn,
                  const void* data, size_t len);
```

发送 WebSocket ping 帧。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_ws_conn_t*`](ws-common.md#xylem_ws_conn_t) | 连接句柄 |
| `data` | `const void*` | 可选 ping 载荷（最多 125 字节），`NULL` 表示无载荷 |
| `len` | `size_t` | 载荷长度（字节） |

**返回值：** 0 成功，-1 错误（如载荷 > 125 字节）。

---

### xylem_ws_close {#xylem_ws_close}

```c
int xylem_ws_close(xylem_ws_conn_t* conn,
                   uint16_t code, const char* reason, size_t reason_len);
```

发起 WebSocket 关闭握手。发送带指定状态码和可选原因字符串的关闭帧，然后转换到 CLOSING 状态。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_ws_conn_t*`](ws-common.md#xylem_ws_conn_t) | 连接句柄 |
| `code` | `uint16_t` | 关闭状态码（1000-1003、1007-1011、3000-4999） |
| `reason` | `const char*` | 可选 UTF-8 原因字符串，`NULL` 表示无原因 |
| `reason_len` | `size_t` | 原因字符串长度（字节，最大 123） |

**返回值：** 0 成功，-1 错误（如无效状态码）。

---

### xylem_ws_get_peer_addr {#xylem_ws_get_peer_addr}

```c
const xylem_addr_t* xylem_ws_get_peer_addr(xylem_ws_conn_t* conn);
```

获取 WebSocket 连接的对端地址。返回底层传输层的对端地址。指针在连接生命周期内有效。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_ws_conn_t*`](ws-common.md#xylem_ws_conn_t) | 连接句柄 |

**返回值：** 对端地址，不可用时返回 `NULL`。

---

### xylem_ws_get_userdata {#xylem_ws_get_userdata}

```c
void* xylem_ws_get_userdata(xylem_ws_conn_t* conn);
```

获取连接上的用户数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_ws_conn_t*`](ws-common.md#xylem_ws_conn_t) | 连接句柄 |

**返回值：** 用户数据指针。

---

### xylem_ws_set_userdata {#xylem_ws_set_userdata}

```c
void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
```

设置连接上的用户数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_ws_conn_t*`](ws-common.md#xylem_ws_conn_t) | 连接句柄 |
| `ud` | `void*` | 用户数据指针 |

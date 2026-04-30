# RUDP

`#include <xylem/xylem-rudp.h>`

可靠 UDP 传输，构建在 [UDP](udp.md) 模块之上。基于 KCP（ARQ 协议）实现自动重传、拥塞控制和有序交付，可选 FEC 前向纠错和 AES-256-CTR 加密。服务端在单个 UDP socket 上通过（对端地址, 会话 ID）复合键多路复用多个 KCP 会话。

---

## 类型

### xylem_rudp_conn_t {#xylem_rudp_conn_t}

不透明类型，表示一个 RUDP 会话句柄。通过 [`xylem_rudp_dial()`](#xylem_rudp_dial) 或 [`on_accept`](#xylem_rudp_handler_t) 回调获得。

### xylem_rudp_server_t {#xylem_rudp_server_t}

不透明类型，表示一个 RUDP 服务器句柄。通过 [`xylem_rudp_listen()`](#xylem_rudp_listen) 获得。

### xylem_rudp_handler_t {#xylem_rudp_handler_t}

RUDP 事件回调集合。

```c
typedef struct xylem_rudp_handler_s {
    void (*on_connect)(xylem_rudp_conn_t* rudp);
    void (*on_accept)(xylem_rudp_server_t* server,
                      xylem_rudp_conn_t* rudp);
    void (*on_read)(xylem_rudp_conn_t* rudp,
                    void* data, size_t len);
    void (*on_close)(xylem_rudp_conn_t* rudp,
                     int err, const char* errmsg);
} xylem_rudp_handler_t;
```

| 回调 | 触发时机 |
|------|---------|
| `on_connect` | 握手完成（客户端） |
| `on_accept` | 新会话接受（服务端） |
| `on_read` | 收到可靠消息（KCP 保证有序交付） |
| `on_close` | 会话关闭。`err`: 0=正常, -1=内部错误, >0=平台 errno |

### xylem_rudp_opts_t {#xylem_rudp_opts_t}

RUDP 连接选项。

```c
typedef struct xylem_rudp_opts_s {
    int32_t        mtu;
    uint64_t       timeout_ms;
    uint64_t       handshake_ms;
    int            fec_data;
    int            fec_parity;
    const uint8_t* aes_key;
} xylem_rudp_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `mtu` | 1400 | MTU 大小 |
| `timeout_ms` | 0（禁用） | Dead-link 超时 |
| `handshake_ms` | 5000 | 握手超时 |
| `fec_data` | 0（禁用 FEC） | FEC 数据分片数 |
| `fec_parity` | 0（禁用 FEC） | FEC 奇偶校验分片数 |
| `aes_key` | `NULL`（禁用加密） | 32 字节 AES-256 密钥 |

---

## 服务端

### xylem_rudp_listen {#xylem_rudp_listen}

```c
xylem_rudp_server_t* xylem_rudp_listen(xylem_loop_t* loop,
                                       xylem_addr_t* addr,
                                       xylem_rudp_handler_t* handler,
                                       xylem_rudp_opts_t* opts);
```

创建 RUDP 服务器并开始监听。在单个 UDP socket 上按（对端地址, 会话 ID）多路复用，每个新会话触发 `handler->on_accept`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 绑定地址 |
| `handler` | [`xylem_rudp_handler_t*`](#xylem_rudp_handler_t) | 回调处理器 |
| `opts` | [`xylem_rudp_opts_t*`](#xylem_rudp_opts_t) | 选项，`NULL` 使用默认值 |

**返回值：** 服务器句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_rudp_close_server {#xylem_rudp_close_server}

```c
void xylem_rudp_close_server(xylem_rudp_server_t* server);
```

关闭服务器，关闭所有活跃会话和底层 UDP socket。

| 参数 | 类型 | 说明 |
|------|------|------|
| `server` | [`xylem_rudp_server_t*`](#xylem_rudp_server_t) | 服务器句柄 |

---

### xylem_rudp_server_get_userdata {#xylem_rudp_server_get_userdata}

```c
void* xylem_rudp_server_get_userdata(xylem_rudp_server_t* server);
```

获取服务器上的用户数据。

---

### xylem_rudp_server_set_userdata {#xylem_rudp_server_set_userdata}

```c
void xylem_rudp_server_set_userdata(xylem_rudp_server_t* server, void* ud);
```

设置服务器上的用户数据。

---

## 客户端 / 会话

### xylem_rudp_dial {#xylem_rudp_dial}

```c
xylem_rudp_conn_t* xylem_rudp_dial(xylem_loop_t* loop,
                                   xylem_addr_t* addr,
                                   xylem_rudp_handler_t* handler,
                                   xylem_rudp_opts_t* opts);
```

发起可靠 UDP 连接。创建已连接 UDP socket，执行轻量级 SYN/ACK 握手，握手完成后触发 `handler->on_connect`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 目标地址 |
| `handler` | [`xylem_rudp_handler_t*`](#xylem_rudp_handler_t) | 回调处理器 |
| `opts` | [`xylem_rudp_opts_t*`](#xylem_rudp_opts_t) | 选项，`NULL` 使用默认值 |

**返回值：** RUDP 会话句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_rudp_send {#xylem_rudp_send}

```c
int xylem_rudp_send(xylem_rudp_conn_t* rudp, const void* data, size_t len);
```

发送数据。数据入队 KCP 发送缓冲区并立即 flush。

| 参数 | 类型 | 说明 |
|------|------|------|
| `rudp` | [`xylem_rudp_conn_t*`](#xylem_rudp_conn_t) | RUDP 会话句柄 |
| `data` | `const void*` | 待发送数据 |
| `len` | `size_t` | 数据长度 |

**返回值：** 0 成功，-1 失败（会话已关闭或握手未完成）。

!!! tip "线程安全"
    可从任意线程调用。跨线程调用时，数据被复制并通过 `xylem_loop_post` 转发到事件循环线程。

---

### xylem_rudp_close {#xylem_rudp_close}

```c
void xylem_rudp_close(xylem_rudp_conn_t* rudp);
```

关闭 RUDP 会话。释放 KCP 会话并关闭底层 UDP socket（客户端）或从服务器会话树移除（服务端），最后触发 `on_close`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `rudp` | [`xylem_rudp_conn_t*`](#xylem_rudp_conn_t) | RUDP 会话句柄 |

!!! tip "线程安全"
    可从任意线程调用。幂等——重复调用安全。

---

### xylem_rudp_conn_acquire {#xylem_rudp_conn_acquire}

```c
void xylem_rudp_conn_acquire(xylem_rudp_conn_t* rudp);
```

递增引用计数，防止会话内存被释放。在将会话句柄传递给其他线程前调用。

!!! warning
    必须在事件循环线程上调用（通常在 `on_connect` 或 `on_accept` 中）。

---

### xylem_rudp_conn_release {#xylem_rudp_conn_release}

```c
void xylem_rudp_conn_release(xylem_rudp_conn_t* rudp);
```

递减引用计数。归零时释放会话内存。可从任意线程调用。

---

### xylem_rudp_get_peer_addr {#xylem_rudp_get_peer_addr}

```c
const xylem_addr_t* xylem_rudp_get_peer_addr(xylem_rudp_conn_t* rudp);
```

获取对端地址。

---

### xylem_rudp_get_loop {#xylem_rudp_get_loop}

```c
xylem_loop_t* xylem_rudp_get_loop(xylem_rudp_conn_t* rudp);
```

获取会话关联的事件循环。

---

### xylem_rudp_get_userdata / set_userdata {#xylem_rudp_get_userdata}

```c
void* xylem_rudp_get_userdata(xylem_rudp_conn_t* rudp);
void  xylem_rudp_set_userdata(xylem_rudp_conn_t* rudp, void* ud);
```

获取/设置会话上的用户数据指针。

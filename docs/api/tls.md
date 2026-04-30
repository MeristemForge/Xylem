# TLS

`#include <xylem/xylem-tls.h>`

异步 TLS 加密传输，构建在 [TCP](tcp.md) 模块之上。OpenSSL 通过内存 BIO 与传输层解耦，支持 SNI、ALPN、证书验证和 keylog 输出。

---

## 类型

### xylem_tls_conn_t {#xylem_tls_conn_t}

不透明类型，表示一个 TLS 连接句柄。通过 [`xylem_tls_dial()`](#xylem_tls_dial) 或 [`on_accept`](#xylem_tls_handler_t) 回调获得。

### xylem_tls_ctx_t {#xylem_tls_ctx_t}

不透明类型，表示一个 TLS 上下文。可在多个连接和服务器之间共享。通过 [`xylem_tls_ctx_create()`](#xylem_tls_ctx_create) 获得。

### xylem_tls_server_t {#xylem_tls_server_t}

不透明类型，表示一个 TLS 服务器句柄。通过 [`xylem_tls_listen()`](#xylem_tls_listen) 获得。

### xylem_tls_opts_t {#xylem_tls_opts_t}

TLS 连接选项。

```c
typedef struct xylem_tls_opts_s {
    xylem_tcp_opts_t tcp;
    const char*      hostname;
} xylem_tls_opts_t;
```

| 字段 | 说明 |
|------|------|
| `tcp` | 底层 TCP 选项，见 [`xylem_tcp_opts_t`](tcp.md#xylem_tcp_opts_t) |
| `hostname` | SNI 主机名，用于服务器证书选择和主机名验证 |

### xylem_tls_handler_t {#xylem_tls_handler_t}

TLS 事件回调集合。

```c
typedef struct xylem_tls_handler_s {
    void (*on_connect)(xylem_tls_conn_t* tls);
    void (*on_accept)(xylem_tls_server_t* server,
                      xylem_tls_conn_t* tls);
    void (*on_read)(xylem_tls_conn_t* tls,
                    void* data, size_t len);
    void (*on_write_done)(xylem_tls_conn_t* tls,
                          const void* data, size_t len,
                          int status);
    void (*on_timeout)(xylem_tls_conn_t* tls,
                       xylem_tcp_timeout_type_t type);
    void (*on_close)(xylem_tls_conn_t* tls,
                     int err, const char* errmsg);
    void (*on_heartbeat_miss)(xylem_tls_conn_t* tls);
} xylem_tls_handler_t;
```

| 回调 | 触发时机 |
|------|---------|
| `on_connect` | TLS 握手完成（客户端） |
| `on_accept` | TLS 握手完成（服务端） |
| `on_read` | 收到解密后的数据 |
| `on_write_done` | 写请求完成。`status`: 0=已发送, -1=未发送 |
| `on_timeout` | 底层 TCP 超时透传。类型见 [`xylem_tcp_timeout_type_t`](tcp.md#xylem_tcp_timeout_type_t) |
| `on_close` | 连接关闭。`err`: 0=正常, -1=内部错误, >0=平台 errno |
| `on_heartbeat_miss` | 底层 TCP 心跳丢失透传 |

---

## 上下文管理

### xylem_tls_ctx_create {#xylem_tls_ctx_create}

```c
xylem_tls_ctx_t* xylem_tls_ctx_create(void);
```

创建 TLS 上下文。使用 `TLS_method()`，默认启用对端验证，强制 TLS 1.2 最低版本。一个上下文可被多个连接和服务器共享。

**返回值：** 上下文句柄，失败返回 `NULL`。

---

### xylem_tls_ctx_destroy {#xylem_tls_ctx_destroy}

```c
void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);
```

销毁 TLS 上下文。使用此上下文的连接必须已全部关闭。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | 上下文句柄 |

---

### xylem_tls_ctx_load_cert {#xylem_tls_ctx_load_cert}

```c
int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                            const char* cert, const char* key);
```

加载 PEM 证书链和私钥。服务端必需，客户端可选（用于客户端证书认证）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | 上下文句柄 |
| `cert` | `const char*` | PEM 证书链文件路径 |
| `key` | `const char*` | PEM 私钥文件路径 |

**返回值：** 0 成功，-1 失败。

---

### xylem_tls_ctx_set_ca {#xylem_tls_ctx_set_ca}

```c
int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file);
```

设置 CA 证书用于对端验证。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | 上下文句柄 |
| `ca_file` | `const char*` | CA 证书文件路径（PEM 格式） |

**返回值：** 0 成功，-1 失败。

---

### xylem_tls_ctx_set_verify {#xylem_tls_ctx_set_verify}

```c
void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable);
```

启用或禁用对端证书验证。启用时，握手失败若对端证书无法验证。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | 上下文句柄 |
| `enable` | `bool` | `true` 启用，`false` 禁用 |

---

### xylem_tls_ctx_set_alpn {#xylem_tls_ctx_set_alpn}

```c
int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                           const char** protocols, size_t count);
```

设置 ALPN 协议列表。客户端为提议协议，服务端为接受协议。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | 上下文句柄 |
| `protocols` | `const char**` | 协议字符串数组 |
| `count` | `size_t` | 协议数量 |

**返回值：** 0 成功，-1 失败。

---

### xylem_tls_ctx_set_keylog {#xylem_tls_ctx_set_keylog}

```c
int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path);
```

启用 TLS 密钥材料日志（NSS Key Log 格式，可供 Wireshark 解密流量）。传 `NULL` 禁用。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | 上下文句柄 |
| `path` | `const char*` | 日志文件路径，`NULL` 禁用 |

**返回值：** 0 成功，-1 失败。

!!! warning
    勿在生产环境启用。导出的密钥材料可解密所有使用此上下文的 TLS 会话。

---

## 服务端

### xylem_tls_listen {#xylem_tls_listen}

```c
xylem_tls_server_t* xylem_tls_listen(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_tls_ctx_t* ctx,
                                     xylem_tls_handler_t* handler,
                                     xylem_tls_opts_t* opts);
```

创建 TLS 服务器并开始监听。每个新 TCP 连接自动执行 TLS 握手，握手成功后触发 `handler->on_accept`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 绑定地址 |
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | TLS 上下文（需已加载证书和密钥） |
| `handler` | [`xylem_tls_handler_t*`](#xylem_tls_handler_t) | 回调处理器 |
| `opts` | [`xylem_tls_opts_t*`](#xylem_tls_opts_t) | 选项，`NULL` 使用默认值 |

**返回值：** 服务器句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_tls_close_server {#xylem_tls_close_server}

```c
void xylem_tls_close_server(xylem_tls_server_t* server);
```

关闭服务器，停止接受新连接，关闭所有已接受的 TLS 连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `server` | [`xylem_tls_server_t*`](#xylem_tls_server_t) | 服务器句柄 |

---

### xylem_tls_server_get_userdata {#xylem_tls_server_get_userdata}

```c
void* xylem_tls_server_get_userdata(xylem_tls_server_t* server);
```

获取服务器上的用户数据。

---

### xylem_tls_server_set_userdata {#xylem_tls_server_set_userdata}

```c
void xylem_tls_server_set_userdata(xylem_tls_server_t* server, void* ud);
```

设置服务器上的用户数据。

---

## 客户端 / 连接

### xylem_tls_dial {#xylem_tls_dial}

```c
xylem_tls_conn_t* xylem_tls_dial(xylem_loop_t* loop,
                                 xylem_addr_t* addr,
                                 xylem_tls_ctx_t* ctx,
                                 xylem_tls_handler_t* handler,
                                 xylem_tls_opts_t* opts);
```

发起异步 TLS 连接。先建立 TCP 连接，再执行 TLS 握手，握手完成后触发 `handler->on_connect`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 目标地址 |
| `ctx` | [`xylem_tls_ctx_t*`](#xylem_tls_ctx_t) | TLS 上下文 |
| `handler` | [`xylem_tls_handler_t*`](#xylem_tls_handler_t) | 回调处理器 |
| `opts` | [`xylem_tls_opts_t*`](#xylem_tls_opts_t) | 选项，`NULL` 使用默认值 |

**返回值：** TLS 连接句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_tls_send {#xylem_tls_send}

```c
int xylem_tls_send(xylem_tls_conn_t* tls, const void* data, size_t len);
```

发送数据。明文经 `SSL_write` 加密后通过底层 TCP 连接发送。

| 参数 | 类型 | 说明 |
|------|------|------|
| `tls` | [`xylem_tls_conn_t*`](#xylem_tls_conn_t) | TLS 连接句柄 |
| `data` | `const void*` | 待发送明文数据 |
| `len` | `size_t` | 数据长度 |

**返回值：** 0 成功（已入队），-1 失败（连接已关闭或握手未完成）。

!!! tip "线程安全"
    可从任意线程调用。跨线程调用时，数据被复制并通过 `xylem_loop_post` 转发到事件循环线程加密发送。

---

### xylem_tls_close {#xylem_tls_close}

```c
void xylem_tls_close(xylem_tls_conn_t* tls);
```

关闭 TLS 连接。发送 `close_notify`，然后关闭底层 TCP 连接，最后触发 `on_close`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `tls` | [`xylem_tls_conn_t*`](#xylem_tls_conn_t) | TLS 连接句柄 |

!!! tip "线程安全"
    可从任意线程调用。幂等——重复调用安全。

---

### xylem_tls_conn_acquire {#xylem_tls_conn_acquire}

```c
void xylem_tls_conn_acquire(xylem_tls_conn_t* tls);
```

递增引用计数，防止连接内存被释放。在将连接句柄传递给其他线程前调用。

!!! warning
    必须在事件循环线程上调用（通常在 `on_connect` 或 `on_accept` 中）。

---

### xylem_tls_conn_release {#xylem_tls_conn_release}

```c
void xylem_tls_conn_release(xylem_tls_conn_t* tls);
```

递减引用计数。归零时释放连接内存。可从任意线程调用。

---

### xylem_tls_get_alpn {#xylem_tls_get_alpn}

```c
const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls);
```

获取 TLS 握手中协商的 ALPN 协议。

**返回值：** 协议字符串，未协商时返回 `NULL`。

---

### xylem_tls_get_peer_addr {#xylem_tls_get_peer_addr}

```c
const xylem_addr_t* xylem_tls_get_peer_addr(xylem_tls_conn_t* tls);
```

获取对端地址（来自底层 TCP 连接）。返回的指针在连接生命周期内有效。

---

### xylem_tls_get_loop {#xylem_tls_get_loop}

```c
xylem_loop_t* xylem_tls_get_loop(xylem_tls_conn_t* tls);
```

获取连接关联的事件循环。

---

### xylem_tls_get_userdata / set_userdata {#xylem_tls_get_userdata}

```c
void* xylem_tls_get_userdata(xylem_tls_conn_t* tls);
void  xylem_tls_set_userdata(xylem_tls_conn_t* tls, void* ud);
```

获取/设置连接上的用户数据指针。

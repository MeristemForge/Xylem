# DTLS

`#include <xylem/xylem-dtls.h>`

异步 DTLS 加密数据报传输，构建在 [UDP](udp.md) 模块之上。OpenSSL 通过内存 BIO 与传输层解耦，使用 `DTLS_method()`。服务端在单个 UDP socket 上通过对端地址多路复用多个 DTLS 会话。

---

## 类型

### xylem_dtls_conn_t {#xylem_dtls_conn_t}

不透明类型，表示一个 DTLS 会话句柄。通过 [`xylem_dtls_dial()`](#xylem_dtls_dial) 或 [`on_accept`](#xylem_dtls_handler_t) 回调获得。

### xylem_dtls_ctx_t {#xylem_dtls_ctx_t}

不透明类型，表示一个 DTLS 上下文。可在多个会话和服务器之间共享。通过 [`xylem_dtls_ctx_create()`](#xylem_dtls_ctx_create) 获得。

### xylem_dtls_server_t {#xylem_dtls_server_t}

不透明类型，表示一个 DTLS 服务器句柄。通过 [`xylem_dtls_listen()`](#xylem_dtls_listen) 获得。

### xylem_dtls_handler_t {#xylem_dtls_handler_t}

DTLS 事件回调集合。

```c
typedef struct xylem_dtls_handler_s {
    void (*on_connect)(xylem_dtls_conn_t* dtls);
    void (*on_accept)(xylem_dtls_server_t* server,
                      xylem_dtls_conn_t* dtls);
    void (*on_read)(xylem_dtls_conn_t* dtls,
                    void* data, size_t len);
    void (*on_close)(xylem_dtls_conn_t* dtls,
                     int err, const char* errmsg);
} xylem_dtls_handler_t;
```

| 回调 | 触发时机 |
|------|---------|
| `on_connect` | DTLS 握手完成（客户端） |
| `on_accept` | DTLS 握手完成（服务端） |
| `on_read` | 收到解密后的数据报 |
| `on_close` | 会话关闭。`err`: 0=正常, -1=内部错误, >0=平台 errno |

与 [TLS handler](tls.md#xylem_tls_handler_t) 相比，DTLS 没有 `on_write_done`（发送是同步的）、`on_timeout` 和 `on_heartbeat_miss`（DTLS 自行管理重传定时器）。

---

## 上下文管理

### xylem_dtls_ctx_create {#xylem_dtls_ctx_create}

```c
xylem_dtls_ctx_t* xylem_dtls_ctx_create(void);
```

创建 DTLS 上下文。使用 `DTLS_method()`，自动配置 cookie 生成和验证回调（防止地址伪造 DoS）。

**返回值：** 上下文句柄，失败返回 `NULL`。

---

### xylem_dtls_ctx_destroy {#xylem_dtls_ctx_destroy}

```c
void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx);
```

销毁 DTLS 上下文。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | 上下文句柄 |

---

### xylem_dtls_ctx_load_cert {#xylem_dtls_ctx_load_cert}

```c
int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                             const char* cert, const char* key);
```

加载 PEM 证书链和私钥。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | 上下文句柄 |
| `cert` | `const char*` | PEM 证书链文件路径 |
| `key` | `const char*` | PEM 私钥文件路径 |

**返回值：** 0 成功，-1 失败。

---

### xylem_dtls_ctx_set_ca {#xylem_dtls_ctx_set_ca}

```c
int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx, const char* ca_file);
```

设置 CA 证书用于对端验证。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | 上下文句柄 |
| `ca_file` | `const char*` | CA 证书文件路径（PEM 格式） |

**返回值：** 0 成功，-1 失败。

---

### xylem_dtls_ctx_set_verify {#xylem_dtls_ctx_set_verify}

```c
void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable);
```

启用或禁用对端证书验证。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | 上下文句柄 |
| `enable` | `bool` | `true` 启用，`false` 禁用 |

---

### xylem_dtls_ctx_set_alpn {#xylem_dtls_ctx_set_alpn}

```c
int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                            const char** protocols, size_t count);
```

设置 ALPN 协议列表。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | 上下文句柄 |
| `protocols` | `const char**` | 协议字符串数组 |
| `count` | `size_t` | 协议数量 |

**返回值：** 0 成功，-1 失败。

---

### xylem_dtls_ctx_set_keylog {#xylem_dtls_ctx_set_keylog}

```c
int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path);
```

启用 DTLS 密钥材料日志（NSS Key Log 格式）。传 `NULL` 禁用。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | 上下文句柄 |
| `path` | `const char*` | 日志文件路径，`NULL` 禁用 |

**返回值：** 0 成功，-1 失败。

!!! warning
    勿在生产环境启用。导出的密钥材料可解密所有使用此上下文的 DTLS 会话。

---

## 服务端

### xylem_dtls_listen {#xylem_dtls_listen}

```c
xylem_dtls_server_t* xylem_dtls_listen(xylem_loop_t* loop,
                                       xylem_addr_t* addr,
                                       xylem_dtls_ctx_t* ctx,
                                       xylem_dtls_handler_t* handler);
```

创建 DTLS 服务器并开始监听。在单个 UDP socket 上按对端地址多路复用，每个成功握手触发 `handler->on_accept`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 绑定地址 |
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | DTLS 上下文（需已加载证书和密钥） |
| `handler` | [`xylem_dtls_handler_t*`](#xylem_dtls_handler_t) | 回调处理器 |

**返回值：** 服务器句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。与 [TLS listen](tls.md#xylem_tls_listen) 不同，DTLS listen 没有 `opts` 参数。

---

### xylem_dtls_close_server {#xylem_dtls_close_server}

```c
void xylem_dtls_close_server(xylem_dtls_server_t* server);
```

关闭服务器，关闭所有活跃 DTLS 会话和底层 UDP socket。

| 参数 | 类型 | 说明 |
|------|------|------|
| `server` | [`xylem_dtls_server_t*`](#xylem_dtls_server_t) | 服务器句柄 |

---

### xylem_dtls_server_get_userdata {#xylem_dtls_server_get_userdata}

```c
void* xylem_dtls_server_get_userdata(xylem_dtls_server_t* server);
```

获取服务器上的用户数据。

---

### xylem_dtls_server_set_userdata {#xylem_dtls_server_set_userdata}

```c
void xylem_dtls_server_set_userdata(xylem_dtls_server_t* server, void* ud);
```

设置服务器上的用户数据。

---

## 客户端 / 会话

### xylem_dtls_dial {#xylem_dtls_dial}

```c
xylem_dtls_conn_t* xylem_dtls_dial(xylem_loop_t* loop,
                                   xylem_addr_t* addr,
                                   xylem_dtls_ctx_t* ctx,
                                   xylem_dtls_handler_t* handler);
```

发起异步 DTLS 连接。创建已连接 UDP socket，执行 DTLS 握手，握手完成后触发 `handler->on_connect`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 目标地址 |
| `ctx` | [`xylem_dtls_ctx_t*`](#xylem_dtls_ctx_t) | DTLS 上下文 |
| `handler` | [`xylem_dtls_handler_t*`](#xylem_dtls_handler_t) | 回调处理器 |

**返回值：** DTLS 会话句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。与 [TLS dial](tls.md#xylem_tls_dial) 不同，DTLS dial 没有 `opts` 参数。

---

### xylem_dtls_send {#xylem_dtls_send}

```c
int xylem_dtls_send(xylem_dtls_conn_t* dtls, const void* data, size_t len);
```

发送数据报。明文经 `SSL_write` 加密后通过底层 UDP 发送。

| 参数 | 类型 | 说明 |
|------|------|------|
| `dtls` | [`xylem_dtls_conn_t*`](#xylem_dtls_conn_t) | DTLS 会话句柄 |
| `data` | `const void*` | 待发送明文数据 |
| `len` | `size_t` | 数据长度 |

**返回值：** 0 成功，-1 失败（会话已关闭或握手未完成）。

!!! tip "线程安全"
    可从任意线程调用。跨线程调用时，数据被复制并通过 `xylem_loop_post` 转发到事件循环线程加密发送。

---

### xylem_dtls_close {#xylem_dtls_close}

```c
void xylem_dtls_close(xylem_dtls_conn_t* dtls);
```

关闭 DTLS 会话。发送 `close_notify`，然后关闭底层 UDP socket（客户端）或从服务器会话树移除（服务端），最后触发 `on_close`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `dtls` | [`xylem_dtls_conn_t*`](#xylem_dtls_conn_t) | DTLS 会话句柄 |

!!! tip "线程安全"
    可从任意线程调用。幂等——重复调用安全。

---

### xylem_dtls_conn_acquire {#xylem_dtls_conn_acquire}

```c
void xylem_dtls_conn_acquire(xylem_dtls_conn_t* dtls);
```

递增引用计数，防止会话内存被释放。在将会话句柄传递给其他线程前调用。

!!! warning
    必须在事件循环线程上调用（通常在 `on_connect` 或 `on_accept` 中）。

---

### xylem_dtls_conn_release {#xylem_dtls_conn_release}

```c
void xylem_dtls_conn_release(xylem_dtls_conn_t* dtls);
```

递减引用计数。归零时释放会话内存。可从任意线程调用。

---

### xylem_dtls_get_alpn {#xylem_dtls_get_alpn}

```c
const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls);
```

获取 DTLS 握手中协商的 ALPN 协议。

**返回值：** 协议字符串，未协商时返回 `NULL`。

---

### xylem_dtls_get_peer_addr {#xylem_dtls_get_peer_addr}

```c
const xylem_addr_t* xylem_dtls_get_peer_addr(xylem_dtls_conn_t* dtls);
```

获取对端地址。返回的指针在会话生命周期内有效。

---

### xylem_dtls_get_loop {#xylem_dtls_get_loop}

```c
xylem_loop_t* xylem_dtls_get_loop(xylem_dtls_conn_t* dtls);
```

获取会话关联的事件循环。

---

### xylem_dtls_get_userdata / set_userdata {#xylem_dtls_get_userdata}

```c
void* xylem_dtls_get_userdata(xylem_dtls_conn_t* dtls);
void  xylem_dtls_set_userdata(xylem_dtls_conn_t* dtls, void* ud);
```

获取/设置会话上的用户数据指针。

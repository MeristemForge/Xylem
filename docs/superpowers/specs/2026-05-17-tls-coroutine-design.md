# TLS 协程化改造设计文档

## 概述

将 `xylem-tls` 模块从回调驱动模型改造为协程阻塞式模型，与 TCP 模块对齐。核心变更：

1. 去掉内存 BIO，改用 Socket BIO（`SSL_set_fd`），OpenSSL 直接操作 socket fd
2. TLS 直接持有 fd + iowait，与 TCP 平级，不再依赖 TCP 模块
3. 去掉所有回调（handler）、状态机、write queue，改为同步阻塞式 API
4. 支持 framing（复用 TCP 的帧解析逻辑模式）

## 公开 API

### 类型

```c
typedef struct xylem_tls_conn_s     xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s      xylem_tls_ctx_t;
typedef struct xylem_tls_listener_s xylem_tls_listener_t;

typedef struct xylem_tls_opts_s {
    size_t      max_read_buf;       /* 明文读缓冲区大小，0 = 默认 64KB */
    bool        disable_mss_clamp;  /* 禁用 socket MSS clamping */
    uint64_t    connect_timeout_ms; /* TCP连接+TLS握手总超时，0 = 无超时 */
    const char* hostname;           /* SNI hostname，用于服务端证书选择和主机名验证 */
} xylem_tls_opts_t;
```

去掉 `xylem_tls_handler_t`、`xylem_tcp_timeout_type_t` 等回调相关类型。

### 上下文管理（保持不变）

```c
xylem_tls_ctx_t* xylem_tls_ctx_create(void);
void             xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);
int  xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                             const char* cert, const char* key);
int  xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file);
void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable);
int  xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                            const char** protocols, size_t count);
int  xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path);
```

`xylem_tls_ctx_t` 内部结构不变。

### 连接生命周期

```c
xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts);

xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts);

xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln);

void xylem_tls_close(xylem_tls_conn_t* tls);
void xylem_tls_close_listener(xylem_tls_listener_t* ln);
```

`xylem_tls_dial` 阻塞直到 TCP 连接建立 + TLS 握手完成，`connect_timeout_ms` 覆盖整个过程。返回 NULL 表示失败。

`xylem_tls_accept` 阻塞直到有新连接且 TLS 握手完成。返回 NULL 表示 listener 正在关闭。

### I/O

```c
int64_t xylem_tls_recv(xylem_tls_conn_t* tls,
                       void* buf, size_t len);

int xylem_tls_send(xylem_tls_conn_t* tls,
                   const void* data, size_t len);
```

语义与 TCP 对齐：
- `recv`：根据 framing 模式返回原始字节或完整帧。返回值 >0 字节数，0 表示对端关闭（NONE 模式），-1 表示错误/超时。
- `send`：阻塞直到全部数据发送完成或出错。返回 0 成功，-1 失败。

### Framing

```c
void xylem_tls_set_framing(
    xylem_tls_conn_t*       tls,
    xylem_tcp_frame_opts_t* opts);
```

复用 `xylem_tcp_frame_opts_t` 类型，在 SSL_read 解密后的明文层做帧解析。

### Deadline

```c
void xylem_tls_set_read_deadline(
    xylem_tls_conn_t* tls, uint64_t deadline_ms);
void xylem_tls_set_write_deadline(
    xylem_tls_conn_t* tls, uint64_t deadline_ms);
```

直接透传到 `iowait_set_rd_deadline` / `iowait_set_wr_deadline`。

### 信息查询

```c
xylem_err_t xylem_tls_get_error(xylem_tls_conn_t* tls);

int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char* host, size_t host_len, uint16_t* port);

int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
    char* host, size_t host_len, uint16_t* port);

int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
    char* host, size_t host_len, uint16_t* port);

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls);
```

### 删除的 API

- `xylem_tls_handler_t` 及所有回调
- `xylem_tls_get/set_userdata()`
- `xylem_tls_conn_ref/unref()`
- `xylem_tls_server_get/set_userdata()`
- `xylem_tls_get_loop()`
- `xylem_tls_remote_addr(tls, host[XYLEM_ADDR_MAXHOST], port)` — 改为与 TCP 对齐的签名

## 内部结构

### TLS 连接

```c
struct xylem_tls_conn_s {
    SSL*                   ssl;
    iowait_t*              waiter;
    platform_sock_t        fd;
    xylem_tls_ctx_t*       ctx;
    addr_t                 peer_addr;
    xylem_tcp_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    char                   alpn[256];
    xylem_err_t            err;
    _Atomic bool           closed;
};
```

与 TCP 的 `xylem_tcp_conn_s` 结构对齐，额外增加 `ssl`、`ctx`、`alpn`。

不需要引用计数 — 协程模型下连接的所有者是明确的（一个读协程 + 一个写协程），与 TCP 一致。

### TLS Listener

```c
struct xylem_tls_listener_s {
    iowait_t*        waiter;
    platform_sock_t  fd;
    xylem_tls_ctx_t* ctx;
    xylem_tls_opts_t opts;
    _Atomic bool     closing;
};
```

### TLS 上下文（不变）

```c
struct xylem_tls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
};
```

## 核心数据流

### Socket BIO 模式

使用 `SSL_set_fd(ssl, fd)` 让 OpenSSL 直接操作非阻塞 socket。当 SSL 操作返回 `WANT_READ`/`WANT_WRITE` 时，通过 `iowait_read()`/`iowait_write()` 挂起协程等待 fd 就绪，然后重试。

必须设置 `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`，因为 Socket BIO 下 `SSL_write` 可能部分完成后返回 `WANT_WRITE`/`WANT_READ`，重试时缓冲区指针可能已变（我们在循环中使用偏移后的指针）。

### SSL I/O 驱动辅助函数

```c
/* 驱动 SSL 握手直到完成 */
static int _tls_do_handshake(xylem_tls_conn_t* tls) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(tls->ssl);
        if (ret == 1) return 0;

        int err = SSL_get_error(tls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY) { /* timeout or closed */ return -1; }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY) { return -1; }
        } else {
            return -1; /* handshake failed */
        }
    }
}
```

### Dial 流程

```
1. DNS 解析（复用 TCP 的 addr_resolve 逻辑）
2. platform_socket_dial() 创建非阻塞 socket
3. iowait_write() 等待 connect 完成（带 connect_timeout 转 write deadline）
4. 检查 SO_ERROR 确认连接成功
5. SSL_new() + SSL_set_fd(ssl, fd)
6. SSL_set_connect_state()
7. 设置 SNI: SSL_set_tlsext_host_name + SSL_set1_host
8. _tls_do_handshake() 同步驱动握手
9. 缓存 ALPN 协商结果
10. 返回 xylem_tls_conn_t*
```

connect_timeout_ms 覆盖步骤 3-8 的总时间。实现方式：在步骤 3 前设置 write deadline = now + timeout，步骤 4 后将同一 deadline 设为 read deadline（握手阶段需要双向 I/O）。握手完成后清除两个 deadline。

### Accept 流程

```
1. iowait_read(listener->waiter) 等待连接到达
2. platform_socket_accept() 接受连接
3. iowait_create(client_fd) 为新连接创建 waiter
4. SSL_new() + SSL_set_fd(ssl, client_fd)
5. SSL_set_accept_state()
6. _tls_do_handshake() 同步驱动握手
7. 缓存 ALPN 协商结果
8. 返回 xylem_tls_conn_t*
```

与 TCP accept 相同：EAGAIN 时 iowait_read 挂起，EMFILE/ENFILE 时指数退避 `runtime_sleep`。

### Recv 流程

```c
static int64_t _tls_raw_recv(xylem_tls_conn_t* tls, void* buf, size_t len) {
    for (;;) {
        ERR_clear_error();
        int n = SSL_read(tls->ssl, buf, (int)len);
        if (n > 0) return n;

        int err = SSL_get_error(tls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            tls->err = XYLEM_ERR_PEER_CLOSED;
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        /* SSL_ERROR_SYSCALL or SSL_ERROR_SSL */
        tls->err = XYLEM_ERR_TLS;
        return -1;
    }
}
```

上层 `xylem_tls_recv` 根据 framing 模式调用 `_tls_raw_recv`（对应 TCP 的 `_tcp_raw_recv`），然后通过 `_tls_buffered_read`、`_tls_read_exact`、`_tls_recv_fixed`、`_tls_recv_length`、`_tls_recv_delimiter` 做帧解析。这些函数与 TCP 的实现逻辑完全相同，只是底层从 `platform_socket_recv` 换成 `SSL_read`。

### Send 流程

```c
static int _tls_raw_send(xylem_tls_conn_t* tls,
                         const void* data, size_t len) {
    const char* ptr = (const char*)data;
    size_t      rem = len;
    while (rem > 0) {
        ERR_clear_error();
        int n = SSL_write(tls->ssl, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }
        int err = SSL_get_error(tls->ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY) {
                tls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        tls->err = XYLEM_ERR_TLS;
        return -1;
    }
    return 0;
}
```

上层 `xylem_tls_send` 根据 framing 模式决定是否先发 length header。

### Close 流程

```
1. atomic_exchange(&tls->closed, true) 幂等检查
2. SSL_shutdown(tls->ssl) — 尝试发 close_notify
   - 若返回 WANT_WRITE/WANT_READ: 不重试，直接继续关闭
3. iowait_close(tls->waiter) — 唤醒任何阻塞的 recv/send 协程
4. SSL_free(tls->ssl)
5. iowait_destroy(tls->waiter)
6. platform_socket_close(fd)
7. free(read_buf) + free(tls)
```

`close_listener` 与 TCP 的 `close_listener` 一致：设置 closing 标志 + iowait_close 唤醒 accept 协程。

## 错误码

新增 `XYLEM_ERR_TLS` 到 `xylem_err_t` 枚举，用于 SSL 层错误（握手失败、SSL_read/SSL_write 致命错误等）。现有错误码（TIMEOUT、CLOSED、PEER_CLOSED 等）由 iowait 返回值直接映射。

## Framing 复用

TLS 的 framing 实现与 TCP 完全相同，操作的是 SSL_read 解密后的明文流。两者共享相同的：
- `xylem_tcp_frame_opts_t` 类型定义
- 帧类型枚举（NONE、FIXED、LENGTH、DELIMITER）
- 帧解析逻辑模式（buffered read → read exact → frame decode）

实现为 TLS 内部的 `_tls_buffered_read`、`_tls_read_exact`、`_tls_recv_fixed`、`_tls_recv_length`、`_tls_recv_delimiter` 和 `_tls_send_length`，与 TCP 对应函数结构一致，底层 I/O 从 `platform_socket_recv/send` 替换为 `SSL_read/SSL_write`。

## 线程安全

与 TCP 一致：
- 同一连接允许一个读协程 + 一个写协程并发操作（iowait 的 one-reader/one-writer 模型）
- `xylem_tls_close` 可从任意线程调用（iowait_close 线程安全）
- deadline setter 由拥有该方向的协程调用

## 测试计划

现有测试全部基于旧回调 API，需要全部重写为协程式：

1. **ctx 管理测试** — 保持不变（create/destroy、load cert、set ca、set verify、set alpn）
2. **handshake + echo** — dial + accept，发送 "hello"，recv 回显验证
3. **握手失败** — 错误 CA，验证 dial 返回 NULL
4. **ALPN 协商** — 验证 xylem_tls_get_alpn 返回正确协议
5. **SNI hostname** — 验证设置 hostname 后握手正常
6. **framing** — length-prefix 模式下收发验证
7. **read deadline** — 设置 deadline 后 recv 超时返回 -1
8. **close** — 关闭后 send 返回 -1
9. **close listener** — 关闭 listener 后 accept 返回 NULL
10. **keylog** — 验证 keylog 文件非空

## 文件变更清单

| 文件 | 变更 |
|------|------|
| `include/xylem/net/xylem-tls.h` | 完全重写：新 API 签名 |
| `src/net/xylem-tls.c` | 完全重写：Socket BIO + iowait + framing |
| `include/xylem/xylem-error.h` | 新增 `XYLEM_ERR_TLS` |
| `tests/test-tls.c` | 完全重写：协程式测试 |
| `docs/tls-design.md` | 更新为新设计 |

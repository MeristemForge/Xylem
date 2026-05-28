# HTTP 协程化改造设计文档

## 概述

将 HTTP 模块从回调驱动模型改造为协程阻塞式模型。核心变更：

1. 去掉 `http_transport_vt_t` / `http_transport_cb_t` 整套回调桥接层
2. 去掉 `loop_t*` 参数，改用 `runtime_run()` 协程环境
3. Server 端采用 Go 风格 per-connection 协程，handler 同步读写
4. Client 端连接池内部化，对用户透明（去掉 `xylem_http_session_t`）
5. 统一到单个头文件 `xylem-http.h`，不区分 client/server
6. HTTP/HTTPS 共用同一套接口，通过 `tls_ctx` 区分
7. `xylem_http_res_t` 统一用于 server 写响应和 client 读响应

## 公开 API

### 类型

```c
typedef struct xylem_http_req_s    xylem_http_req_t;
typedef struct xylem_http_res_s    xylem_http_res_t;
typedef struct xylem_http_srv_s    xylem_http_srv_t;
typedef struct xylem_http_router_s xylem_http_router_t;
```

去掉：`xylem_http_conn_s`、`xylem_http_writer_t`（别名）、`xylem_http_session_t`、
`xylem_http_on_request_fn_t`（回调风格）、`xylem_http_on_upgrade_fn_t`。

### Handler 类型

```c
typedef void (*xylem_http_handler_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);
```

每个请求在独立协程中调用 handler。handler 内同步调用 res 写方法。
handler 返回后框架自动 finalize（flush gzip、发送 chunked 终止符）。
userdata 由 `xylem_http_listen` 或 `xylem_http_router_add` 传入。

### Server API

```c
typedef struct {
    xylem_tls_ctx_t* tls_ctx;          /* NULL = HTTP, non-NULL = HTTPS */
    size_t           max_body_size;     /* 0 = default 1 MiB */
    uint64_t         idle_timeout_ms;   /* 0 = default 60000 ms */
    xylem_http_handler_fn_t on_upgrade; /* NULL = reject 501 */
} xylem_http_srv_opts_t;

xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

void xylem_http_close(xylem_http_srv_t* srv);
```

`xylem_http_listen` 内部 spawn accept 协程，立即返回。传 NULL opts 使用默认值。

### Request 读取 API

```c
const char*  xylem_http_req_method(const xylem_http_req_t* req);
const char*  xylem_http_req_url(const xylem_http_req_t* req);
const char*  xylem_http_req_header(const xylem_http_req_t* req,
                                   const char* name);
const void*  xylem_http_req_body(const xylem_http_req_t* req);
size_t       xylem_http_req_body_len(const xylem_http_req_t* req);
const char*  xylem_http_req_param(const xylem_http_req_t* req,
                                  const char* name);
```

### Response API（server 写 + client 读，共用类型）

**Server 端写方法：**

```c
int  xylem_http_res_set_status(xylem_http_res_t* res, int code);
int  xylem_http_res_set_header(xylem_http_res_t* res,
                               const char* name, const char* value);
int  xylem_http_res_write(xylem_http_res_t* res,
                          const void* data, size_t len);
void xylem_http_res_close(xylem_http_res_t* res);
```

- `set_status` / `set_header` 必须在第一次 `write` 之前调用
- 第一次 `write` 自动 flush status line + headers（Transfer-Encoding: chunked）
- handler 返回后框架自动 finalize：flush gzip、发送 `0\r\n\r\n`
- 未调用 `set_status` 默认 200

**Upgrade（server 端）：**

```c
int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport);
```

发送 101 Switching Protocols，拆离底层 conn 给调用者。
返回的 transport 是 `xylem_tcp_conn_t*` 或 `xylem_tls_conn_t*`。
调用后 HTTP 层不再管理此连接。

**Client 端读方法：**

```c
int          xylem_http_res_status(const xylem_http_res_t* res);
const char*  xylem_http_res_header(const xylem_http_res_t* res,
                                   const char* name);
const void*  xylem_http_res_body(const xylem_http_res_t* res);
size_t       xylem_http_res_body_len(const xylem_http_res_t* res);
void         xylem_http_res_destroy(xylem_http_res_t* res);
```

### Client API

```c
typedef struct {
    uint64_t                timeout_ms;    /* 0 = default 30000 ms */
    int                     max_redirects; /* 0 = 不跟随 */
    size_t                  max_body_size; /* 0 = default 10 MiB */
    const xylem_http_hdr_t* headers;
    size_t                  header_count;
    xylem_tls_ctx_t*        tls_ctx;       /* NULL = 全局默认 ctx */
    const char*             range;         /* Range header, NULL = omit */
} xylem_http_opts_t;

xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_opts_t* opts);
xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_opts_t* opts);
xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_opts_t* opts);
xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_opts_t* opts);
xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_opts_t* opts);
```

所有 client 函数必须在协程中调用（会挂起等待响应）。
返回 NULL 表示请求失败。成功时调用者需要 `xylem_http_res_destroy()` 释放。

### 连接池（内部实现，不对外暴露）

```c
/* 模块级全局单例，lazy init，进程退出时清理 */
/* key: "host:port:scheme" */
/* 每个 key 最多保留 max_idle_per_host 个 idle conn（默认 5） */
/* idle conn 超时 idle_timeout_ms（默认 90s）后自动关闭 */
```

Client 请求时：
1. 计算 pool key
2. 查找空闲 conn，有则复用
3. 无则 dial 新连接（tcp 或 tls，取决于 scheme）
4. 请求完成后，keep-alive conn 归还池子

### Router / Middleware

```c
xylem_http_router_t* xylem_http_router_create(void);
void xylem_http_router_destroy(xylem_http_router_t* r);

int xylem_http_router_add(xylem_http_router_t* r,
                          const char* method,
                          const char* pattern,
                          xylem_http_handler_fn_t handler,
                          void* userdata);

typedef int (*xylem_http_middleware_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

int xylem_http_router_use(xylem_http_router_t* r,
                          xylem_http_middleware_fn_t mw,
                          void* userdata);

int xylem_http_router_dispatch(xylem_http_router_t* r,
                               xylem_http_res_t* res,
                               xylem_http_req_t* req);
```

路由匹配规则不变：exact > path-param > wildcard，longer pattern wins。

### Gzip 压缩（Server）

```c
typedef struct {
    bool        enabled;
    int         level;       /* 1-9, 0 = default 6 */
    size_t      min_size;    /* default 1024 */
    const char* mime_types;  /* 逗号分隔，NULL = 内置默认 */
} xylem_http_gzip_opts_t;

void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                             const xylem_http_gzip_opts_t* opts);
```

当 client Accept-Encoding 含 gzip 且 response Content-Type 匹配时，
`xylem_http_res_write` 内部自动流式 gzip 压缩。

### Static File Server

```c
typedef struct {
    const char* root;
    const char* index_file;    /* NULL = "index.html" */
    int         max_age;       /* Cache-Control, 0 = omit */
    bool        precompressed; /* 查找 .gz 预压缩文件 */
} xylem_http_static_opts_t;

int xylem_http_static_serve(xylem_http_router_t* r,
                            const char* prefix,
                            const xylem_http_static_opts_t* opts);
```

### Cookie Jar（Client）

```c
xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);
void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);
```

通过 `xylem_http_opts_t` 传入（需在 opts 中加 `cookie_jar` 字段）：

```c
typedef struct {
    /* ... 前面的字段 ... */
    xylem_http_cookie_jar_t* cookie_jar; /* NULL = 不管理 cookie */
} xylem_http_opts_t;
```

### 工具函数（保留，不变）

```c
char*  xylem_http_url_encode(const char* src, size_t src_len, size_t* out_len);
char*  xylem_http_url_decode(const char* src, size_t src_len, size_t* out_len);
size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                               const char* origin, bool is_preflight,
                               xylem_http_hdr_t* out, size_t out_cap);
xylem_http_multipart_t* xylem_http_multipart_parse(...);
char* xylem_http_sse_build(const char* event, const char* data, size_t* len);
```

## 内部实现结构

### Server 内部

```c
struct xylem_http_srv_s {
    xylem_tcp_listener_t*    tcp_ln;   /* HTTP 模式 */
    xylem_tls_listener_t*    tls_ln;   /* HTTPS 模式 */
    xylem_http_handler_fn_t  handler;
    xylem_http_srv_opts_t    opts;
    xylem_http_gzip_opts_t   gzip_opts;
    _Atomic bool             closed;
};
```

Accept 协程伪代码：

```c
static void _http_accept_coro(void* arg) {
    xylem_http_srv_t* srv = arg;
    while (!srv->closed) {
        void* conn;
        int kind;
        if (srv->tls_ln) {
            conn = xylem_tls_accept(srv->tls_ln);
            kind = HTTP_TRANSPORT_TLS;
        } else {
            conn = xylem_tcp_accept(srv->tcp_ln);
            kind = HTTP_TRANSPORT_TCP;
        }
        if (!conn) break;
        // spawn per-connection coroutine
        _http_conn_ctx_t* ctx = _alloc_conn_ctx(srv, conn, kind);
        runtime_spawn(_http_conn_coro, ctx);
    }
}
```

Per-connection 协程伪代码：

```c
static void _http_conn_coro(void* arg) {
    _http_conn_ctx_t* ctx = arg;
    xylem_reader_t* reader = xylem_reader_create(ctx->conn, ctx->transport);
    
    while (!ctx->closed) {
        // 设置 idle deadline
        _set_read_deadline(ctx, now + idle_timeout);
        
        // 读并解析请求
        xylem_http_req_t req = {0};
        int rc = _http_parse_request(reader, &req, ctx);
        if (rc != 0) break;
        
        // 构造 res
        xylem_http_res_t res = _http_res_init(ctx);
        
        // 调用 handler（在当前协程中同步执行）
        ctx->srv->handler(&res, &req);
        
        // auto-finalize response
        _http_res_finalize(&res);
        _http_req_reset(&req);
        
        if (!res.keep_alive) break;
    }
    
    _close_conn(ctx);
    xylem_reader_destroy(reader);
    free(ctx);
}
```

### Client 内部

```c
/* 全局连接池 */
static struct {
    _Atomic bool       inited;
    xylem_rbtree_t     pool;       /* key -> idle conn list */
    size_t             max_idle;   /* per host, default 5 */
    uint64_t           idle_ms;    /* default 90000 */
    sched_timer_t*     gc_timer;   /* 定期清理过期 idle conn */
} _http_pool;
```

请求流程伪代码：

```c
xylem_http_res_t* xylem_http_get(const char* url, const xylem_http_opts_t* opts) {
    http_url_t parsed;
    _parse_url(url, &parsed);
    
    // 从池中取或新建连接
    _http_conn_t* conn = _pool_acquire(&parsed, opts);
    if (!conn) return NULL;
    
    // 设置 deadline
    uint64_t deadline = now + timeout_ms;
    _set_deadlines(conn, deadline);
    
    // 写请求
    _http_write_request(conn, "GET", &parsed, NULL, 0, NULL, opts);
    
    // 读响应（用 reader + llhttp 解析）
    xylem_http_res_t* res = _http_read_response(conn, opts);
    
    // 归还或关闭连接
    if (res && _is_keep_alive(res)) {
        _pool_release(conn);
    } else {
        _conn_close(conn);
    }
    
    // 处理 redirect
    if (res && _is_redirect(res) && redirects_remaining > 0) {
        // 递归或循环跟随
    }
    
    return res;
}
```

### Transport 抽象（内部 helper）

不使用 vtable，用简单的 inline helper 根据 kind 分发：

```c
typedef struct {
    enum { HTTP_TRANSPORT_TCP, HTTP_TRANSPORT_TLS } kind;
    void* conn;  /* xylem_tcp_conn_t* 或 xylem_tls_conn_t* */
} _http_transport_t;

static inline void _set_read_deadline(_http_transport_t* t, uint64_t ms) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        xylem_tcp_set_read_deadline(t->conn, ms);
    else
        xylem_tls_set_read_deadline(t->conn, ms);
}

static inline void _close_transport(_http_transport_t* t) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        xylem_tcp_close(t->conn);
    else
        xylem_tls_close(t->conn);
}
```

Reader/writer 创建时传入对应的 transport enum：

```c
xylem_reader_t* reader = xylem_reader_create(
    conn->conn,
    conn->kind == HTTP_TRANSPORT_TCP ? XYLEM_READER_TCP : XYLEM_READER_TLS,
    0);
```

## 删除的文件

- `include/xylem/net/http/xylem-http-client.h`
- `include/xylem/net/http/xylem-http-server.h`
- `src/net/http/http-transport.h`
- `src/net/http/http-transport-tcp.c`
- `src/net/http/http-transport-tls.c`
- `src/net/http/http-transport-tls-stub.c`
- `src/net/http/xylem-http-client.c`
- `src/net/http/xylem-http-server.c`

## 新增的文件

- `include/xylem/net/xylem-http.h` — 统一公开头文件
- `src/net/http/xylem-http.c` — 统一实现

## 保留的文件（不变或微调）

- `include/xylem/net/http/xylem-http-common.h` — 移动到 `xylem-http.h` 中合并
- `src/net/http/http-common.h` / `http-common.c` — 内部 header 工具，保留
- `src/net/http/xylem-http-common.c` — URL encode/decode/CORS/multipart，保留
- `src/net/http/llhttp/` — HTTP 解析器，保留不动

## 功能保留清单

| 功能 | 状态 |
|------|------|
| GET/POST/PUT/DELETE/PATCH | 保留 |
| Connection pool (keep-alive) | 保留（内部化） |
| Cookie jar | 保留 |
| Redirect follow | 保留 |
| Request timeout | 保留 |
| Max body size | 保留 |
| Custom headers | 保留 |
| Range requests | 保留 |
| Gzip response compression (server) | 保留 |
| Gzip auto-decompress (client) | 保留 |
| Chunked transfer encoding | 保留（自动） |
| Expect: 100-continue | 保留 |
| Router + path params | 保留 |
| Middleware chain | 保留 |
| Static file server | 保留 |
| CORS headers | 保留 |
| Multipart parser | 保留 |
| SSE build helper | 保留 |
| HTTP Upgrade | 保留 |
| Idle timeout (server) | 保留 |

## 错误处理

- Server：解析错误或 body 过大返回 400/413 并关闭连接
- Client：返回 NULL 表示失败（网络错误、超时、解析错误）
- Deadline 超时通过 iowait 的 IOWAIT_TIMEOUT 传播

## 线程安全

- `xylem_http_listen` 返回的 srv 可跨线程调用 `xylem_http_close`
- Client 函数必须在协程中调用
- 全局连接池使用 scheduler_post 保证 single-owner 语义（池子归属一个 scheduler）

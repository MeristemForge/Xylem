# HTTP Server

`#include <xylem/http/xylem-http-server.h>`

异步 HTTP 服务端，支持路由分发、中间件、SSE、gzip 压缩、静态文件服务和 HTTP Upgrade。

---

## 类型

### xylem_http_req_t {#xylem_http_req_t}

不透明类型，表示一个已解析的 HTTP 请求。在请求回调中通过参数获得。

### xylem_http_conn_t {#xylem_http_conn_t}

不透明类型，表示一个 HTTP 连接句柄。

### xylem_http_srv_t {#xylem_http_srv_t}

不透明类型，表示一个 HTTP 服务器句柄。通过 [`xylem_http_listen()`](#xylem_http_listen) 获得。

### xylem_http_router_t {#xylem_http_router_t}

不透明类型，表示一个路由器句柄。通过 [`xylem_http_router_create()`](#xylem_http_router_create) 获得。

### xylem_http_writer_t {#xylem_http_writer_t}

响应写入器句柄。是 [`xylem_http_conn_t`](#xylem_http_conn_t) 的别名，在请求回调中用于构建和发送 HTTP 响应（设置状态码、头部、写入 body）。概念上等同于 Go 的 `http.ResponseWriter`。

```c
typedef xylem_http_conn_t xylem_http_writer_t;
```

### xylem_http_on_request_fn_t {#xylem_http_on_request_fn_t}

服务器请求回调。当收到完整 HTTP 请求时调用。`writer` 和 `req` 句柄仅在此回调内有效。

```c
typedef void (*xylem_http_on_request_fn_t)(xylem_http_writer_t* writer,
                                           xylem_http_req_t* req,
                                           void* userdata);
```

### xylem_http_on_upgrade_fn_t {#xylem_http_on_upgrade_fn_t}

Upgrade 请求回调。当客户端发送带 `Upgrade` 头和 `Connection: Upgrade` 的请求时调用。要接受升级，在此回调内调用 [`xylem_http_writer_accept_upgrade()`](#xylem_http_writer_accept_upgrade)。若回调返回时未接受，连接将被关闭。

```c
typedef void (*xylem_http_on_upgrade_fn_t)(xylem_http_writer_t* writer,
                                           xylem_http_req_t* req,
                                           void* userdata);
```

### xylem_http_middleware_fn_t {#xylem_http_middleware_fn_t}

中间件回调。在 [`xylem_http_router_dispatch()`](#xylem_http_router_dispatch) 期间，路由处理器之前按注册顺序调用。返回 0 继续链路，返回 -1 中止（中间件必须在返回 -1 前发送响应）。

```c
typedef int (*xylem_http_middleware_fn_t)(xylem_http_writer_t* writer,
                                         xylem_http_req_t* req,
                                         void* userdata);
```

### xylem_http_gzip_opts_t {#xylem_http_gzip_opts_t}

Gzip 响应压缩选项。通过 [`xylem_http_srv_set_gzip()`](#xylem_http_srv_set_gzip) 配置。启用后，Content-Type 匹配可压缩 MIME 类型且 body 超过 `min_size` 的响应，在客户端发送 `Accept-Encoding: gzip` 时自动压缩。

```c
typedef struct {
    bool        enabled;     /* 全局开关，默认 false */
    int         level;       /* 压缩级别 1-9，0 = 默认（6） */
    size_t      min_size;    /* 最小压缩 body 大小，默认 1024 */
    const char* mime_types;  /* 逗号分隔的可压缩 MIME 类型，NULL = 内置默认 */
} xylem_http_gzip_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `enabled` | `false` | 全局开关 |
| `level` | 0（默认 6） | 压缩级别 1-9 |
| `min_size` | 1024 | 最小压缩 body 大小（字节） |
| `mime_types` | `NULL`（内置默认） | 逗号分隔的可压缩 MIME 类型 |

### xylem_http_srv_cfg_t {#xylem_http_srv_cfg_t}

服务器配置。调用者在栈上分配并传给 [`xylem_http_listen()`](#xylem_http_listen)。`host`、`tls_cert` 和 `tls_key` 指向的字符串必须在 [`xylem_http_close_server()`](#xylem_http_close_server) 之前保持有效。

```c
typedef struct xylem_http_srv_cfg_s {
    const char*                  host;            /* 绑定地址，如 "0.0.0.0" */
    uint16_t                     port;            /* 绑定端口 */
    xylem_http_on_request_fn_t   on_request;      /* 请求回调 */
    void*                        userdata;        /* 传给 on_request */
    const char*                  tls_cert;        /* PEM 证书路径，NULL 为 HTTP */
    const char*                  tls_key;         /* PEM 密钥路径，NULL 为 HTTP */
    size_t                       max_body_size;   /* 最大请求体，0 = 默认 1 MiB */
    uint64_t                     idle_timeout_ms; /* 空闲超时，0 = 禁用，默认 60000 */
    xylem_http_on_upgrade_fn_t   on_upgrade;      /* Upgrade 回调，NULL = 以 501 拒绝 */
} xylem_http_srv_cfg_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `host` | — | 绑定地址 |
| `port` | — | 绑定端口 |
| `on_request` | — | 请求回调 |
| `userdata` | `NULL` | 传给回调的用户数据 |
| `tls_cert` | `NULL`（HTTP） | PEM 证书路径，设置后启用 HTTPS |
| `tls_key` | `NULL`（HTTP） | PEM 密钥路径 |
| `max_body_size` | 0（默认 1 MiB） | 最大请求体大小 |
| `idle_timeout_ms` | 0（默认 60000） | 空闲连接超时（毫秒） |
| `on_upgrade` | `NULL`（以 501 拒绝） | HTTP Upgrade 回调 |

### xylem_http_static_opts_t {#xylem_http_static_opts_t}

静态文件服务配置。传给 [`xylem_http_static_serve()`](#xylem_http_static_serve)。

```c
typedef struct {
    const char* root;          /* 文件系统根目录 */
    const char* index_file;    /* 默认文档，NULL = "index.html" */
    int         max_age;       /* Cache-Control max-age 秒数，0 = 省略 */
    bool        precompressed; /* 查找 .gz 预压缩文件 */
} xylem_http_static_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `root` | — | 文件系统根目录 |
| `index_file` | `"index.html"` | 默认文档 |
| `max_age` | 0（省略） | Cache-Control max-age 秒数 |
| `precompressed` | `false` | 查找 `.gz` 预压缩文件 |

---

## 服务器

### xylem_http_listen {#xylem_http_listen}

```c
xylem_http_srv_t* xylem_http_listen(xylem_loop_t* loop,
                                    const xylem_http_srv_cfg_t* cfg);
```

创建并启动 HTTP 服务器。根据 `cfg` 配置服务器，绑定到指定 host 和 port，开始接受连接。根据 `tls_cert` 和 `tls_key` 是否设置，使用 TCP 或 TLS。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `cfg` | `const` [`xylem_http_srv_cfg_t*`](#xylem_http_srv_cfg_t) | 服务器配置 |

**返回值：** 服务器句柄，失败返回 `NULL`。

---

### xylem_http_close_server {#xylem_http_close_server}

```c
void xylem_http_close_server(xylem_http_srv_t* srv);
```

停止 HTTP 服务器并释放所有资源。停止接受新连接，关闭监听器，释放服务器句柄。已有连接继续处理直到完成或关闭。

| 参数 | 类型 | 说明 |
|------|------|------|
| `srv` | [`xylem_http_srv_t*`](#xylem_http_srv_t) | 服务器句柄，`NULL` 为空操作 |

---

### xylem_http_srv_set_gzip {#xylem_http_srv_set_gzip}

```c
void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                             const xylem_http_gzip_opts_t* opts);
```

配置 gzip 响应压缩。启用后，服务器自动压缩匹配配置 MIME 类型且超过最小大小阈值的响应体（前提是客户端发送 `Accept-Encoding: gzip`）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `srv` | [`xylem_http_srv_t*`](#xylem_http_srv_t) | 服务器句柄 |
| `opts` | `const` [`xylem_http_gzip_opts_t*`](#xylem_http_gzip_opts_t) | Gzip 选项。结构体被复制；`mime_types` 字符串在非 `NULL` 时必须在服务器生命周期内有效 |

---

## 请求

### xylem_http_req_method {#xylem_http_req_method}

```c
const char* xylem_http_req_method(const xylem_http_req_t* req);
```

获取请求方法字符串。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const` [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |

**返回值：** 方法字符串（如 `"GET"`、`"POST"`）。

---

### xylem_http_req_url {#xylem_http_req_url}

```c
const char* xylem_http_req_url(const xylem_http_req_t* req);
```

获取请求 URL 路径字符串。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const` [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |

**返回值：** URL 路径字符串。

---

### xylem_http_req_header {#xylem_http_req_header}

```c
const char* xylem_http_req_header(const xylem_http_req_t* req,
                                  const char* name);
```

按名称获取请求头值。按 RFC 7230 执行大小写不敏感的 ASCII 匹配。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const` [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |
| `name` | `const char*` | 要查找的头部名称 |

**返回值：** 头部值字符串，未找到返回 `NULL`。

---

### xylem_http_req_body {#xylem_http_req_body}

```c
const void* xylem_http_req_body(const xylem_http_req_t* req);
```

获取请求体数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const` [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |

**返回值：** 指向 body 字节的指针，无 body 时返回 `NULL`。

---

### xylem_http_req_body_len {#xylem_http_req_body_len}

```c
size_t xylem_http_req_body_len(const xylem_http_req_t* req);
```

获取请求体长度。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const` [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |

**返回值：** body 长度（字节），无 body 时返回 0。

---

### xylem_http_req_param {#xylem_http_req_param}

```c
const char* xylem_http_req_param(const xylem_http_req_t* req,
                                 const char* name);
```

获取路由匹配时提取的路径参数值。当 [`xylem_http_router_dispatch()`](#xylem_http_router_dispatch) 匹配包含 `:name` 段的路由时，捕获的值存储在请求上，可通过名称检索。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | `const` [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |
| `name` | `const char*` | 参数名称（不含前导 `:`） |

**返回值：** 参数值字符串，未找到返回 `NULL`。

!!! note
    返回的指针在请求回调返回前有效。

---

## 响应写入器

### xylem_http_writer_set_header {#xylem_http_writer_set_header}

```c
int xylem_http_writer_set_header(xylem_http_writer_t* writer,
                                 const char* name,
                                 const char* value);
```

缓冲一个响应头。头部在首次 [`xylem_http_writer_write()`](#xylem_http_writer_write) 调用时自动刷新。若同名头部已存在，其值被替换（最后写入生效）。必须在首次 write 之前调用。

| 参数 | 类型 | 说明 |
|------|------|------|
| `writer` | [`xylem_http_writer_t*`](#xylem_http_writer_t) | 响应写入器句柄 |
| `name` | `const char*` | 头部名称（内部复制） |
| `value` | `const char*` | 头部值（内部复制） |

**返回值：** 0 成功，-1 失败（头部已发送）。

---

### xylem_http_writer_set_status {#xylem_http_writer_set_status}

```c
int xylem_http_writer_set_status(xylem_http_writer_t* writer,
                                 int status_code);
```

设置响应状态码。必须在首次 write 之前调用。未调用时默认 200。

| 参数 | 类型 | 说明 |
|------|------|------|
| `writer` | [`xylem_http_writer_t*`](#xylem_http_writer_t) | 响应写入器句柄 |
| `status_code` | `int` | HTTP 状态码（如 200、404） |

**返回值：** 0 成功，-1 失败（头部已发送）。

---

### xylem_http_writer_write {#xylem_http_writer_write}

```c
int xylem_http_writer_write(xylem_http_writer_t* writer,
                            const void* data, size_t len);
```

写入响应体数据。首次调用时自动发送状态行和所有缓冲头部（使用 `Transfer-Encoding: chunked`）。后续调用发送额外的 chunk。请求回调返回时框架自动终结响应。

| 参数 | 类型 | 说明 |
|------|------|------|
| `writer` | [`xylem_http_writer_t*`](#xylem_http_writer_t) | 响应写入器句柄 |
| `data` | `const void*` | body 数据 |
| `len` | `size_t` | 数据长度（字节）。0 为空操作 |

**返回值：** 0 成功，-1 失败。

---

### xylem_http_writer_close {#xylem_http_writer_close}

```c
void xylem_http_writer_close(xylem_http_writer_t* writer);
```

关闭底层连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `writer` | [`xylem_http_writer_t*`](#xylem_http_writer_t) | 响应写入器句柄 |

---

### xylem_http_writer_accept_upgrade {#xylem_http_writer_accept_upgrade}

```c
int xylem_http_writer_accept_upgrade(xylem_http_writer_t* writer,
                                     void** transport);
```

接受 HTTP Upgrade 请求。发送 `101 Switching Protocols` 响应（含 `Upgrade` 和 `Connection: Upgrade` 头）。将底层传输句柄从 HTTP 连接管理中分离：停止空闲定时器、停止 HTTP 解析、将所有权转移给调用者。

必须在 `on_upgrade` 回调内调用。在其他上下文中调用返回 -1。

成功后，调用者拥有传输句柄，负责读取、写入和关闭。

| 参数 | 类型 | 说明 |
|------|------|------|
| `writer` | [`xylem_http_writer_t*`](#xylem_http_writer_t) | 响应写入器句柄（来自 `on_upgrade` 回调） |
| `transport` | `void**` | 输出：底层传输句柄。HTTP 为 `xylem_tcp_conn_t*`；HTTPS 为 `xylem_tls_conn_t*`。按需转换 |

**返回值：** 0 成功，-1 失败。

---

## SSE

### xylem_http_sse_build {#xylem_http_sse_build}

```c
char* xylem_http_sse_build(const char* event,
                           const char* data,
                           size_t* len);
```

构建 SSE 格式的消息字符串。分配并返回 SSE 线格式字符串：`"event: {event}\ndata: {data}\n\n"`。当 `event` 为 `NULL` 时省略 `"event:"` 行。多行 data 按 SSE 规范拆分为多个 `"data:"` 行。

| 参数 | 类型 | 说明 |
|------|------|------|
| `event` | `const char*` | 事件类型字符串，`NULL` 表示仅数据 |
| `data` | `const char*` | 事件数据字符串（不能为 `NULL`） |
| `len` | `size_t*` | 若非 `NULL`，接收结果的字节长度 |

**返回值：** 堆分配的 SSE 字符串，失败返回 `NULL`。

!!! note
    调用者必须使用 `free()` 释放返回的指针。

---

## 路由器

### xylem_http_router_create {#xylem_http_router_create}

```c
xylem_http_router_t* xylem_http_router_create(void);
```

创建路由器，用于按方法和路径分发请求。

**返回值：** 路由器句柄，分配失败返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_router_destroy()`](#xylem_http_router_destroy) 释放。

---

### xylem_http_router_destroy {#xylem_http_router_destroy}

```c
void xylem_http_router_destroy(xylem_http_router_t* router);
```

销毁路由器并释放所有注册的路由。

| 参数 | 类型 | 说明 |
|------|------|------|
| `router` | [`xylem_http_router_t*`](#xylem_http_router_t) | 路由器句柄，`NULL` 为空操作 |

---

### xylem_http_router_add {#xylem_http_router_add}

```c
int xylem_http_router_add(xylem_http_router_t* router,
                          const char* method,
                          const char* pattern,
                          xylem_http_on_request_fn_t handler,
                          void* userdata);
```

注册路由。

模式语法：

- 精确匹配：`"/api/users"`
- 路径参数：`"/user/:id"`（匹配一个段，捕获值）
- 通配符：`"/static/"` + `"*"`（匹配任意后缀）

方法大小写敏感（如 `"GET"`、`"POST"`）。传 `NULL` 匹配所有 HTTP 方法。

| 参数 | 类型 | 说明 |
|------|------|------|
| `router` | [`xylem_http_router_t*`](#xylem_http_router_t) | 路由器句柄 |
| `method` | `const char*` | HTTP 方法字符串，`NULL` 匹配所有方法 |
| `pattern` | `const char*` | URL 路径模式 |
| `handler` | [`xylem_http_on_request_fn_t`](#xylem_http_on_request_fn_t) | 请求处理回调 |
| `userdata` | `void*` | 传给 handler 的用户数据 |

**返回值：** 0 成功，-1 失败（重复路由、参数错误）。

---

### xylem_http_router_use {#xylem_http_router_use}

```c
int xylem_http_router_use(xylem_http_router_t* router,
                          xylem_http_middleware_fn_t fn,
                          void* userdata);
```

注册全局中间件。中间件按注册顺序在 [`xylem_http_router_dispatch()`](#xylem_http_router_dispatch) 期间匹配的路由处理器之前运行。若任何中间件返回 -1，链路中止，路由处理器不被调用。中止的中间件负责发送响应。

| 参数 | 类型 | 说明 |
|------|------|------|
| `router` | [`xylem_http_router_t*`](#xylem_http_router_t) | 路由器句柄 |
| `fn` | [`xylem_http_middleware_fn_t`](#xylem_http_middleware_fn_t) | 中间件回调 |
| `userdata` | `void*` | 传给 fn 的用户数据 |

**返回值：** 0 成功，-1 失败（参数错误、分配失败）。

---

### xylem_http_router_dispatch {#xylem_http_router_dispatch}

```c
int xylem_http_router_dispatch(xylem_http_router_t* router,
                               xylem_http_writer_t* writer,
                               xylem_http_req_t* req);
```

将请求分发到最佳匹配的路由。在调用匹配的路由处理器之前，按顺序运行所有注册的中间件。若任何中间件返回 -1，链路中止，处理器不被调用。

匹配优先级：精确 > 路径参数 > 通配符前缀。同类型中，更长的模式优先。指定方法优先于 `NULL`（全方法）通配符。无路由匹配时发送 404 Not Found 响应。

| 参数 | 类型 | 说明 |
|------|------|------|
| `router` | [`xylem_http_router_t*`](#xylem_http_router_t) | 路由器句柄 |
| `writer` | [`xylem_http_writer_t*`](#xylem_http_writer_t) | 响应写入器句柄 |
| `req` | [`xylem_http_req_t*`](#xylem_http_req_t) | 请求句柄 |

**返回值：** 0 路由匹配成功，-1 发送了 404 或中间件中止。

---

## 静态文件

### xylem_http_static_serve {#xylem_http_static_serve}

```c
int xylem_http_static_serve(xylem_http_router_t* router,
                            const char* prefix,
                            const xylem_http_static_opts_t* opts);
```

在路由器上注册静态文件处理器。将 `prefix` 下的 URL 路径映射到 `opts->root` 下的文件。支持 GET 和 HEAD 方法。缺失文件返回 404，其他方法返回 405，无索引文件的目录返回 403。防止路径遍历攻击。

当 `precompressed` 为 true 且客户端接受 gzip 时，处理器优先查找 `.gz` 兄弟文件。

| 参数 | 类型 | 说明 |
|------|------|------|
| `router` | [`xylem_http_router_t*`](#xylem_http_router_t) | 路由器句柄 |
| `prefix` | `const char*` | URL 前缀（如 `"/static"`）。必须以 `"/"` + `"*"` 结尾，否则函数内部追加 |
| `opts` | `const` [`xylem_http_static_opts_t*`](#xylem_http_static_opts_t) | 静态文件选项。结构体被复制；`root` 和 `index_file` 字符串必须在路由器生命周期内有效 |

**返回值：** 0 成功，-1 失败。

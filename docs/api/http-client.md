# HTTP Client

`#include <xylem/http/xylem-http-client.h>`

同步 HTTP 客户端，支持 GET/POST/PUT/DELETE/PATCH 请求、自动 Cookie 管理和基于连接池的会话复用。

---

## 类型

### xylem_http_res_t {#xylem_http_res_t}

不透明类型，表示一个 HTTP 响应句柄。通过请求函数（如 [`xylem_http_get()`](#xylem_http_get)）获得。

### xylem_http_cookie_jar_t {#xylem_http_cookie_jar_t}

不透明类型，表示一个 Cookie 存储容器。通过 [`xylem_http_cookie_jar_create()`](#xylem_http_cookie_jar_create) 获得。

### xylem_http_session_t {#xylem_http_session_t}

不透明类型，表示一个 HTTP 会话（连接池）。通过 [`xylem_http_session_create()`](#xylem_http_session_create) 获得。

### xylem_http_session_opts_t {#xylem_http_session_opts_t}

会话配置选项。传 `NULL` 给 `xylem_http_session_create()` 使用默认值。零初始化的字段使用默认值。

```c
typedef struct {
    size_t                   max_idle_per_host; /* 每 host:port:scheme 最大空闲连接数，默认 5 */
    uint64_t                 idle_timeout_ms;   /* 空闲连接超时（ms），默认 90000 */
    xylem_http_cookie_jar_t* cookie_jar;        /* 共享 Cookie 容器，NULL 禁用 */
} xylem_http_session_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `max_idle_per_host` | 5 | 每 host:port:scheme 最大空闲连接数 |
| `idle_timeout_ms` | 90000 | 空闲连接超时（毫秒） |
| `cookie_jar` | `NULL`（禁用） | 共享的 Cookie 容器 |

### xylem_http_cli_opts_t {#xylem_http_cli_opts_t}

单次请求选项。传 `NULL` 给任何请求函数使用默认值。零初始化的字段使用默认值。

```c
typedef struct {
    uint64_t                    timeout_ms;
    int                         max_redirects;
    size_t                      max_body_size;
    const xylem_http_hdr_t*     headers;      /* 自定义请求头，NULL 表示无 */
    size_t                      header_count;  /* 自定义请求头数量 */
    xylem_http_cookie_jar_t*    cookie_jar;    /* Cookie 容器，NULL 禁用 */
    const char*                 range;         /* Range 头值（如 "bytes=0-499"），NULL 省略 */
} xylem_http_cli_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `timeout_ms` | 30000 | 请求超时（毫秒） |
| `max_redirects` | 0 | 最大重定向次数 |
| `max_body_size` | 10 MiB | 最大响应体大小 |
| `headers` | `NULL` | 自定义请求头数组 |
| `header_count` | 0 | 自定义请求头数量 |
| `cookie_jar` | `NULL` | 自动 Cookie 管理容器 |
| `range` | `NULL` | Range 头值 |

---

## 独立请求

### xylem_http_get {#xylem_http_get}

```c
xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_cli_opts_t* opts);
```

发送同步 HTTP GET 请求。内部创建临时事件循环，解析主机，根据 URL scheme 通过 TCP 或 TLS 连接，发送请求并阻塞直到收到响应。

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | `const char*` | 完整 URL（如 `"http://example.com/path"`） |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 释放响应。

---

### xylem_http_post {#xylem_http_post}

```c
xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body,
                                  size_t body_len,
                                  const char* content_type,
                                  const xylem_http_cli_opts_t* opts);
```

发送同步 HTTP POST 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | `const char*` | 完整 URL |
| `body` | `const void*` | 请求体，`NULL` 表示空 body |
| `body_len` | `size_t` | 请求体长度（字节） |
| `content_type` | `const char*` | Content-Type 头值 |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 释放响应。

---

### xylem_http_put {#xylem_http_put}

```c
xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body,
                                 size_t body_len,
                                 const char* content_type,
                                 const xylem_http_cli_opts_t* opts);
```

发送同步 HTTP PUT 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | `const char*` | 完整 URL |
| `body` | `const void*` | 请求体，`NULL` 表示空 body |
| `body_len` | `size_t` | 请求体长度（字节） |
| `content_type` | `const char*` | Content-Type 头值 |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 释放响应。

---

### xylem_http_delete {#xylem_http_delete}

```c
xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_cli_opts_t* opts);
```

发送同步 HTTP DELETE 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | `const char*` | 完整 URL |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 释放响应。

---

### xylem_http_patch {#xylem_http_patch}

```c
xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body,
                                   size_t body_len,
                                   const char* content_type,
                                   const xylem_http_cli_opts_t* opts);
```

发送同步 HTTP PATCH 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | `const char*` | 完整 URL |
| `body` | `const void*` | 请求体，`NULL` 表示空 body |
| `body_len` | `size_t` | 请求体长度（字节） |
| `content_type` | `const char*` | Content-Type 头值 |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 释放响应。

---

## 响应

### xylem_http_res_status {#xylem_http_res_status}

```c
int xylem_http_res_status(const xylem_http_res_t* res);
```

获取 HTTP 状态码。

| 参数 | 类型 | 说明 |
|------|------|------|
| `res` | `const` [`xylem_http_res_t*`](#xylem_http_res_t) | 响应句柄 |

**返回值：** HTTP 状态码（如 200、404）。

---

### xylem_http_res_header {#xylem_http_res_header}

```c
const char* xylem_http_res_header(const xylem_http_res_t* res,
                                  const char* name);
```

按名称获取响应头值。按 RFC 7230 执行大小写不敏感的 ASCII 匹配。

| 参数 | 类型 | 说明 |
|------|------|------|
| `res` | `const` [`xylem_http_res_t*`](#xylem_http_res_t) | 响应句柄 |
| `name` | `const char*` | 要查找的头部名称 |

**返回值：** 头部值字符串，未找到返回 `NULL`。

!!! note
    返回的指针在 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 之前有效。

---

### xylem_http_res_body {#xylem_http_res_body}

```c
const void* xylem_http_res_body(const xylem_http_res_t* res);
```

获取响应体数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `res` | `const` [`xylem_http_res_t*`](#xylem_http_res_t) | 响应句柄 |

**返回值：** 指向 body 字节的指针，无 body 时返回 `NULL`。

!!! note
    返回的指针在 [`xylem_http_res_destroy()`](#xylem_http_res_destroy) 之前有效。

---

### xylem_http_res_body_len {#xylem_http_res_body_len}

```c
size_t xylem_http_res_body_len(const xylem_http_res_t* res);
```

获取响应体长度。

| 参数 | 类型 | 说明 |
|------|------|------|
| `res` | `const` [`xylem_http_res_t*`](#xylem_http_res_t) | 响应句柄 |

**返回值：** body 长度（字节）。

---

### xylem_http_res_destroy {#xylem_http_res_destroy}

```c
void xylem_http_res_destroy(xylem_http_res_t* res);
```

销毁响应并释放所有关联内存。

| 参数 | 类型 | 说明 |
|------|------|------|
| `res` | [`xylem_http_res_t*`](#xylem_http_res_t) | 响应句柄，`NULL` 为空操作 |

---

## Cookie 管理

### xylem_http_cookie_jar_create {#xylem_http_cookie_jar_create}

```c
xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);
```

创建 Cookie 容器，用于自动 Cookie 管理。容器存储通过 `Set-Cookie` 响应头接收的 Cookie，并在后续请求中通过 [`xylem_http_cli_opts_t.cookie_jar`](#xylem_http_cli_opts_t) 自动附加匹配的 Cookie。

**返回值：** Cookie 容器句柄，分配失败返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_cookie_jar_destroy()`](#xylem_http_cookie_jar_destroy) 释放。

---

### xylem_http_cookie_jar_destroy {#xylem_http_cookie_jar_destroy}

```c
void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);
```

销毁 Cookie 容器并释放所有存储的 Cookie。

| 参数 | 类型 | 说明 |
|------|------|------|
| `jar` | [`xylem_http_cookie_jar_t*`](#xylem_http_cookie_jar_t) | Cookie 容器句柄，`NULL` 为空操作 |

---

## 会话

### xylem_http_session_create {#xylem_http_session_create}

```c
xylem_http_session_t* xylem_http_session_create(
    const xylem_http_session_opts_t* opts);
```

创建 HTTP 会话（连接池）。会话维护持久事件循环，跨请求复用到相同 host:port:scheme 的 TCP/TLS 连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `opts` | `const` [`xylem_http_session_opts_t*`](#xylem_http_session_opts_t) | 会话选项，`NULL` 使用默认值 |

**返回值：** 会话句柄，失败返回 `NULL`。

---

### xylem_http_session_destroy {#xylem_http_session_destroy}

```c
void xylem_http_session_destroy(xylem_http_session_t* session);
```

销毁会话并关闭所有池化连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | [`xylem_http_session_t*`](#xylem_http_session_t) | 会话句柄，`NULL` 为空操作 |

---

### xylem_http_session_get {#xylem_http_session_get}

```c
xylem_http_res_t* xylem_http_session_get(
    xylem_http_session_t* session,
    const char* url,
    const xylem_http_cli_opts_t* opts);
```

使用会话发送同步 HTTP GET 请求。可用时复用池化连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | [`xylem_http_session_t*`](#xylem_http_session_t) | 会话句柄 |
| `url` | `const char*` | 完整 URL |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

---

### xylem_http_session_post {#xylem_http_session_post}

```c
xylem_http_res_t* xylem_http_session_post(
    xylem_http_session_t* session,
    const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts);
```

使用会话发送同步 HTTP POST 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | [`xylem_http_session_t*`](#xylem_http_session_t) | 会话句柄 |
| `url` | `const char*` | 完整 URL |
| `body` | `const void*` | 请求体，`NULL` 表示空 body |
| `body_len` | `size_t` | 请求体长度（字节） |
| `content_type` | `const char*` | Content-Type 头值 |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

---

### xylem_http_session_put {#xylem_http_session_put}

```c
xylem_http_res_t* xylem_http_session_put(
    xylem_http_session_t* session,
    const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts);
```

使用会话发送同步 HTTP PUT 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | [`xylem_http_session_t*`](#xylem_http_session_t) | 会话句柄 |
| `url` | `const char*` | 完整 URL |
| `body` | `const void*` | 请求体，`NULL` 表示空 body |
| `body_len` | `size_t` | 请求体长度（字节） |
| `content_type` | `const char*` | Content-Type 头值 |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

---

### xylem_http_session_delete {#xylem_http_session_delete}

```c
xylem_http_res_t* xylem_http_session_delete(
    xylem_http_session_t* session,
    const char* url,
    const xylem_http_cli_opts_t* opts);
```

使用会话发送同步 HTTP DELETE 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | [`xylem_http_session_t*`](#xylem_http_session_t) | 会话句柄 |
| `url` | `const char*` | 完整 URL |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

---

### xylem_http_session_patch {#xylem_http_session_patch}

```c
xylem_http_res_t* xylem_http_session_patch(
    xylem_http_session_t* session,
    const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts);
```

使用会话发送同步 HTTP PATCH 请求。

| 参数 | 类型 | 说明 |
|------|------|------|
| `session` | [`xylem_http_session_t*`](#xylem_http_session_t) | 会话句柄 |
| `url` | `const char*` | 完整 URL |
| `body` | `const void*` | 请求体，`NULL` 表示空 body |
| `body_len` | `size_t` | 请求体长度（字节） |
| `content_type` | `const char*` | Content-Type 头值 |
| `opts` | `const` [`xylem_http_cli_opts_t*`](#xylem_http_cli_opts_t) | 请求选项，`NULL` 使用默认值 |

**返回值：** 响应句柄，任何错误返回 `NULL`。

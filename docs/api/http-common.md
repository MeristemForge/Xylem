# HTTP Common

`#include <xylem/http/xylem-http-common.h>`

HTTP 公共类型和工具函数，供客户端和服务端共享。包括 URL 编解码、CORS 头生成和 multipart/form-data 解析。

---

## 类型

### xylem_http_hdr_t {#xylem_http_hdr_t}

HTTP 头部名值对。

```c
typedef struct {
    const char* name;  /* 头部名称（如 "Authorization"） */
    const char* value; /* 头部值（如 "Bearer token"） */
} xylem_http_hdr_t;
```

| 字段 | 说明 |
|------|------|
| `name` | 头部名称 |
| `value` | 头部值 |

!!! note
    非拥有指针：调用者必须确保字符串在接收它们的 API 调用期间保持有效。

### xylem_http_cors_t {#xylem_http_cors_t}

CORS 配置，用于生成 `Access-Control-*` 响应头。

```c
typedef struct {
    const char* allowed_origins;   /* 逗号分隔的源或 "*" */
    const char* allowed_methods;   /* 逗号分隔的方法（如 "GET,POST"） */
    const char* allowed_headers;   /* 逗号分隔的头部（如 "Content-Type,Authorization"） */
    const char* expose_headers;    /* 逗号分隔的暴露头部 */
    int         max_age;           /* 预检缓存时长（秒），0 表示省略 */
    bool        allow_credentials; /* 若为 true，发送 Access-Control-Allow-Credentials: true */
} xylem_http_cors_t;
```

| 字段 | 说明 |
|------|------|
| `allowed_origins` | 逗号分隔的允许源，或 `"*"` 匹配所有 |
| `allowed_methods` | 逗号分隔的允许方法 |
| `allowed_headers` | 逗号分隔的允许头部 |
| `expose_headers` | 逗号分隔的暴露给客户端的头部 |
| `max_age` | 预检缓存时长（秒），0 表示省略 |
| `allow_credentials` | 启用时，`Allow-Origin` 使用实际 origin 值（非 `"*"`） |

### xylem_http_multipart_t {#xylem_http_multipart_t}

不透明类型，表示解析后的 multipart/form-data 消息。通过 [`xylem_http_multipart_parse()`](#xylem_http_multipart_parse) 获得。

---

## URL 编解码

### xylem_http_url_encode {#xylem_http_url_encode}

```c
char* xylem_http_url_encode(const char* src, size_t src_len,
                            size_t* out_len);
```

对字符串进行百分号编码，用于 URL 路径或查询参数。按 RFC 3986 将保留字符和非 ASCII 字节编码为 `%XX` 序列。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const char*` | 源字节 |
| `src_len` | `size_t` | 源长度（字节） |
| `out_len` | `size_t*` | 输出：编码后长度（不含 NUL 终止符） |

**返回值：** 新分配的编码字符串，失败返回 `NULL`。

!!! note
    调用者必须使用 `free()` 释放返回的字符串。

---

### xylem_http_url_decode {#xylem_http_url_decode}

```c
char* xylem_http_url_decode(const char* src, size_t src_len,
                            size_t* out_len);
```

解码百分号编码的字符串。将 `%XX` 序列还原为原始字节值。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const char*` | 源字符串 |
| `src_len` | `size_t` | 源长度（字节） |
| `out_len` | `size_t*` | 输出：解码后长度 |

**返回值：** 新分配的解码字符串，失败返回 `NULL`。

!!! note
    调用者必须使用 `free()` 释放返回的字符串。

---

## CORS

### xylem_http_cors_headers {#xylem_http_cors_headers}

```c
size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                               const char* origin,
                               bool is_preflight,
                               xylem_http_hdr_t* out,
                               size_t out_cap);
```

根据 CORS 配置生成响应头。检查请求 origin 是否匹配 `allowed_origins`（`"*"` 匹配所有，否则逗号分隔精确匹配）。当 `is_preflight` 为 true 时，额外生成 `Allow-Methods`、`Allow-Headers` 和 `Max-Age` 头。当 `allow_credentials` 为 true 时，`Allow-Origin` 使用实际 origin 值（非 `"*"`）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `cors` | `const` [`xylem_http_cors_t*`](#xylem_http_cors_t) | CORS 配置，`NULL` 返回 0 |
| `origin` | `const char*` | 请求 Origin 头值，`NULL` 返回 0 |
| `is_preflight` | `bool` | 是否为 OPTIONS 预检请求 |
| `out` | [`xylem_http_hdr_t*`](#xylem_http_hdr_t) | 输出头部数组，调用者提供存储，至少 7 个条目 |
| `out_cap` | `size_t` | 输出数组容量 |

**返回值：** 写入 `out` 的头部数量，origin 不匹配或参数为 `NULL` 时返回 0。

!!! note
    返回的头部名/值指针引用静态字符串或 `cors`/`origin` 参数中的字段，在这些输入有效期间保持有效。

---

## Multipart 解析

### xylem_http_multipart_parse {#xylem_http_multipart_parse}

```c
xylem_http_multipart_t* xylem_http_multipart_parse(
    const char* content_type, const void* body, size_t body_len);
```

解析 multipart/form-data 请求体。从 `content_type` 提取 boundary，然后将 body 拆分为各个部分，解析每个部分的 Content-Disposition（name、filename）和 Content-Type。

| 参数 | 类型 | 说明 |
|------|------|------|
| `content_type` | `const char*` | Content-Type 头值（必须包含 boundary） |
| `body` | `const void*` | 请求体数据 |
| `body_len` | `size_t` | 请求体长度（字节） |

**返回值：** 解析后的 multipart 句柄，输入无效时返回 `NULL`。

!!! note
    调用者必须使用 [`xylem_http_multipart_destroy()`](#xylem_http_multipart_destroy) 释放。

---

### xylem_http_multipart_count {#xylem_http_multipart_count}

```c
size_t xylem_http_multipart_count(const xylem_http_multipart_t* mp);
```

获取 multipart 消息中的部分数量。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | `const` [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄 |

**返回值：** 部分数量，`mp` 为 `NULL` 时返回 0。

---

### xylem_http_multipart_name {#xylem_http_multipart_name}

```c
const char* xylem_http_multipart_name(
    const xylem_http_multipart_t* mp, size_t index);
```

获取指定部分的 name 字段。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | `const` [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄 |
| `index` | `size_t` | 部分索引（从 0 开始） |

**返回值：** name 字符串，不存在或索引越界时返回 `NULL`。

---

### xylem_http_multipart_filename {#xylem_http_multipart_filename}

```c
const char* xylem_http_multipart_filename(
    const xylem_http_multipart_t* mp, size_t index);
```

获取指定部分的 filename 字段。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | `const` [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄 |
| `index` | `size_t` | 部分索引（从 0 开始） |

**返回值：** filename 字符串，不存在或索引越界时返回 `NULL`。

---

### xylem_http_multipart_content_type {#xylem_http_multipart_content_type}

```c
const char* xylem_http_multipart_content_type(
    const xylem_http_multipart_t* mp, size_t index);
```

获取指定部分的 Content-Type。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | `const` [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄 |
| `index` | `size_t` | 部分索引（从 0 开始） |

**返回值：** Content-Type 字符串，不存在或索引越界时返回 `NULL`。

---

### xylem_http_multipart_data {#xylem_http_multipart_data}

```c
const void* xylem_http_multipart_data(
    const xylem_http_multipart_t* mp, size_t index);
```

获取指定部分的 body 数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | `const` [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄 |
| `index` | `size_t` | 部分索引（从 0 开始） |

**返回值：** 指向部分 body 数据的指针，索引越界时返回 `NULL`。

---

### xylem_http_multipart_data_len {#xylem_http_multipart_data_len}

```c
size_t xylem_http_multipart_data_len(
    const xylem_http_multipart_t* mp, size_t index);
```

获取指定部分的 body 数据长度。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | `const` [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄 |
| `index` | `size_t` | 部分索引（从 0 开始） |

**返回值：** 部分 body 长度（字节），索引越界时返回 0。

---

### xylem_http_multipart_destroy {#xylem_http_multipart_destroy}

```c
void xylem_http_multipart_destroy(xylem_http_multipart_t* mp);
```

销毁 multipart 句柄并释放所有关联内存。

| 参数 | 类型 | 说明 |
|------|------|------|
| `mp` | [`xylem_http_multipart_t*`](#xylem_http_multipart_t) | Multipart 句柄，`NULL` 为空操作 |

# UDS

`#include <xylem/xylem-uds.h>`

异步 Unix Domain Socket 客户端和服务端，内置帧解析、超时管理和心跳检测。设计与 [TCP](tcp.md) 模块对称，但去除了网络相关特性（地址解析、MSS 钳制、重连机制），专注于本地进程间通信。

---

## 类型

### xylem_uds_conn_t {#xylem_uds_conn_t}

不透明类型，表示一个 UDS 连接句柄。通过 [`xylem_uds_dial()`](#xylem_uds_dial) 或 [`on_accept`](#xylem_uds_handler_t) 回调获得。

### xylem_uds_server_t {#xylem_uds_server_t}

不透明类型，表示一个 UDS 服务器句柄。通过 [`xylem_uds_listen()`](#xylem_uds_listen) 获得。

### xylem_uds_framing_type_t {#xylem_uds_framing_type_t}

帧解析策略枚举。

```c
typedef enum xylem_uds_framing_type_e {
    XYLEM_UDS_FRAME_NONE,    /* 无帧，原始字节流 */
    XYLEM_UDS_FRAME_FIXED,   /* 固定长度帧 */
    XYLEM_UDS_FRAME_LENGTH,  /* 长度前缀帧 */
    XYLEM_UDS_FRAME_DELIM,   /* 分隔符帧 */
    XYLEM_UDS_FRAME_CUSTOM,  /* 自定义解析函数 */
} xylem_uds_framing_type_t;
```

| 值 | 说明 |
|----|------|
| `FRAME_NONE` | 将缓冲区中所有可用数据作为一帧返回 |
| `FRAME_FIXED` | 当缓冲区数据 ≥ `frame_size` 时返回一帧 |
| `FRAME_LENGTH` | 根据长度前缀字段提取帧，支持 [FIXEDINT](#xylem_uds_length_coding_t) 和 [VARINT](#xylem_uds_length_coding_t) |
| `FRAME_DELIM` | 按分隔符切分帧（单字节用 `memchr`，多字节用滑动窗口） |
| `FRAME_CUSTOM` | 调用用户提供的 `parse(data, len)` 函数 |

### xylem_uds_timeout_type_t {#xylem_uds_timeout_type_t}

超时类型枚举。

```c
typedef enum xylem_uds_timeout_type_e {
    XYLEM_UDS_TIMEOUT_READ,   /* 读空闲超时 */
    XYLEM_UDS_TIMEOUT_WRITE,  /* 写完成超时 */
} xylem_uds_timeout_type_t;
```

与 TCP 的 [`xylem_tcp_timeout_type_t`](tcp.md#xylem_tcp_timeout_type_t) 相比，UDS 没有 `TIMEOUT_CONNECT`（本地连接几乎瞬时完成）。

### xylem_uds_length_coding_t {#xylem_uds_length_coding_t}

长度字段编码方式（用于 `FRAME_LENGTH`）。

```c
typedef enum xylem_uds_length_coding_e {
    XYLEM_UDS_LENGTH_FIXEDINT,  /* 固定宽度整数（1-8 字节），支持大端/小端 */
    XYLEM_UDS_LENGTH_VARINT,    /* 变长整数编码 */
} xylem_uds_length_coding_t;
```

### xylem_uds_framing_t {#xylem_uds_framing_t}

帧配置结构体。

```c
typedef struct xylem_uds_framing_s {
    xylem_uds_framing_type_t type;
    union {
        struct { size_t frame_size; }                          fixed;
        struct {
            uint32_t                  header_size;
            uint32_t                  field_offset;
            uint32_t                  field_size;
            int32_t                   adjustment;
            xylem_uds_length_coding_t coding;
            bool                      field_big_endian;
        } length;
        struct { const char* delim; size_t delim_len; }        delim;
        struct { int (*parse)(const void* data, size_t len); } custom;
    };
} xylem_uds_framing_t;
```

**LENGTH 字段说明：**

| 字段 | 说明 |
|------|------|
| `header_size` | 固定头部大小（payload 从此偏移开始） |
| `field_offset` | 长度字段在头部中的偏移 |
| `field_size` | 长度字段的字节数（1-8） |
| `adjustment` | 长度调整值（可为负，处理长度含/不含头部的协议差异） |
| `coding` | 编码方式：[FIXEDINT](#xylem_uds_length_coding_t) 或 [VARINT](#xylem_uds_length_coding_t) |
| `field_big_endian` | 字节序（仅 FIXEDINT 有效） |

### xylem_uds_handler_t {#xylem_uds_handler_t}

UDS 事件回调集合。

```c
typedef struct xylem_uds_handler_s {
    void (*on_connect)(xylem_uds_conn_t* conn);
    void (*on_accept)(xylem_uds_server_t* server, xylem_uds_conn_t* conn);
    void (*on_read)(xylem_uds_conn_t* conn, void* data, size_t len);
    void (*on_write_done)(xylem_uds_conn_t* conn,
                          const void* data, size_t len, int status);
    void (*on_timeout)(xylem_uds_conn_t* conn,
                       xylem_uds_timeout_type_t type);
    void (*on_close)(xylem_uds_conn_t* conn, int err, const char* errmsg);
    void (*on_heartbeat_miss)(xylem_uds_conn_t* conn);
} xylem_uds_handler_t;
```

| 回调 | 触发时机 |
|------|---------|
| `on_connect` | 客户端连接建立成功 |
| `on_accept` | 服务端接受新连接 |
| `on_read` | 收到完整帧（经帧解析器提取） |
| `on_write_done` | 写请求完成。`status`: 0=已发送, -1=未发送 |
| `on_timeout` | 超时触发。类型见 [`xylem_uds_timeout_type_t`](#xylem_uds_timeout_type_t) |
| `on_close` | 连接关闭。`err`: 0=正常, -1=内部错误, >0=平台 errno |
| `on_heartbeat_miss` | 在 `heartbeat_ms` 内无数据到达 |

### xylem_uds_opts_t {#xylem_uds_opts_t}

UDS 连接选项。

```c
typedef struct xylem_uds_opts_s {
    xylem_uds_framing_t framing;
    uint64_t            read_timeout_ms;
    uint64_t            write_timeout_ms;
    uint64_t            heartbeat_ms;
    size_t              read_buf_size;
} xylem_uds_opts_t;
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `framing` | `FRAME_NONE` | 帧解析配置 |
| `read_timeout_ms` | 0（无超时） | 读空闲超时 |
| `write_timeout_ms` | 0（无超时） | 写完成超时 |
| `heartbeat_ms` | 0（禁用） | 心跳间隔 |
| `read_buf_size` | 65536 | 读缓冲区大小 |

与 TCP 的 [`xylem_tcp_opts_t`](tcp.md#xylem_tcp_opts_t) 相比，UDS 选项去除了 `connect_timeout_ms`、`reconnect_max` 和 `disable_mss_clamp`。

---

## 服务端

### xylem_uds_listen {#xylem_uds_listen}

```c
xylem_uds_server_t* xylem_uds_listen(xylem_loop_t* loop,
                                     const char* path,
                                     xylem_uds_handler_t* handler,
                                     xylem_uds_opts_t* opts);
```

创建 UDS 服务器并开始监听。绑定前自动 `remove` 已存在的 socket 文件。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `path` | `const char*` | Unix domain socket 路径 |
| `handler` | [`xylem_uds_handler_t*`](#xylem_uds_handler_t) | 回调处理器 |
| `opts` | [`xylem_uds_opts_t*`](#xylem_uds_opts_t) | 选项，`NULL` 使用默认值 |

**返回值：** 服务器句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_uds_close_server {#xylem_uds_close_server}

```c
void xylem_uds_close_server(xylem_uds_server_t* server);
```

关闭服务器，停止接受新连接，关闭所有已接受的连接，删除 socket 文件。

| 参数 | 类型 | 说明 |
|------|------|------|
| `server` | [`xylem_uds_server_t*`](#xylem_uds_server_t) | 服务器句柄 |

---

### xylem_uds_server_get_userdata {#xylem_uds_server_get_userdata}

```c
void* xylem_uds_server_get_userdata(xylem_uds_server_t* server);
```

获取服务器上的用户数据。

---

### xylem_uds_server_set_userdata {#xylem_uds_server_set_userdata}

```c
void xylem_uds_server_set_userdata(xylem_uds_server_t* server, void* ud);
```

设置服务器上的用户数据。

---

## 客户端 / 连接

### xylem_uds_dial {#xylem_uds_dial}

```c
xylem_uds_conn_t* xylem_uds_dial(xylem_loop_t* loop,
                                 const char* path,
                                 xylem_uds_handler_t* handler,
                                 xylem_uds_opts_t* opts);
```

发起异步 UDS 连接。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `path` | `const char*` | Unix domain socket 路径 |
| `handler` | [`xylem_uds_handler_t*`](#xylem_uds_handler_t) | 回调处理器 |
| `opts` | [`xylem_uds_opts_t*`](#xylem_uds_opts_t) | 选项，`NULL` 使用默认值 |

**返回值：** 连接句柄，失败返回 `NULL`。

连接建立后触发 `handler->on_connect`。

!!! note
    必须在事件循环线程上调用。

---

### xylem_uds_send {#xylem_uds_send}

```c
int xylem_uds_send(xylem_uds_conn_t* conn, const void* data, size_t len);
```

发送数据。数据被复制到内部写队列，调用后可立即释放原始缓冲区。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_uds_conn_t*`](#xylem_uds_conn_t) | 连接句柄 |
| `data` | `const void*` | 待发送数据 |
| `len` | `size_t` | 数据长度 |

**返回值：** 0 成功（已入队），-1 失败（连接已关闭）。

!!! tip "线程安全"
    可从任意线程调用。跨线程调用时，数据被复制并通过 `xylem_loop_post` 转发到事件循环线程。

---

### xylem_uds_close {#xylem_uds_close}

```c
void xylem_uds_close(xylem_uds_conn_t* conn);
```

优雅关闭连接。先排空写队列，然后 `shutdown(SHUT_WR)`，最后触发 `on_close`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `conn` | [`xylem_uds_conn_t*`](#xylem_uds_conn_t) | 连接句柄 |

!!! tip "线程安全"
    可从任意线程调用。幂等——重复调用安全。

---

### xylem_uds_conn_acquire {#xylem_uds_conn_acquire}

```c
void xylem_uds_conn_acquire(xylem_uds_conn_t* conn);
```

递增引用计数，防止连接内存被释放。在将连接句柄传递给其他线程前调用。

!!! warning
    必须在事件循环线程上调用（通常在 `on_connect` 或 `on_accept` 中）。

---

### xylem_uds_conn_release {#xylem_uds_conn_release}

```c
void xylem_uds_conn_release(xylem_uds_conn_t* conn);
```

递减引用计数。归零时释放连接内存。可从任意线程调用。

---

### xylem_uds_get_loop {#xylem_uds_get_loop}

```c
xylem_loop_t* xylem_uds_get_loop(xylem_uds_conn_t* conn);
```

获取连接关联的事件循环。

---

### xylem_uds_get_userdata / set_userdata {#xylem_uds_get_userdata}

```c
void* xylem_uds_get_userdata(xylem_uds_conn_t* conn);
void  xylem_uds_set_userdata(xylem_uds_conn_t* conn, void* ud);
```

获取/设置连接上的用户数据指针。

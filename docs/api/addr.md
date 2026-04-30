# Addr

`#include <xylem/xylem-addr.h>`

统一网络地址封装，支持 IPv4/IPv6 地址转换和异步 DNS 解析。

---

## 类型

### xylem_addr_t {#xylem_addr_t}

统一网络地址结构体，封装 `sockaddr_storage`，使调用者无需区分 IPv4 和 IPv6。

```c
typedef struct xylem_addr_s {
    struct sockaddr_storage storage;
} xylem_addr_t;
```

### xylem_addr_resolve_t {#xylem_addr_resolve_t}

不透明类型，表示一个异步 DNS 解析请求句柄。通过 [`xylem_addr_resolve()`](#xylem_addr_resolve) 获得。

### xylem_addr_resolve_fn_t {#xylem_addr_resolve_fn_t}

异步 DNS 解析完成回调。

```c
typedef void (*xylem_addr_resolve_fn_t)(xylem_addr_t* addrs, size_t count,
                                        int status, void* userdata);
```

| 参数 | 说明 |
|------|------|
| `addrs` | 解析结果地址数组，失败时为 NULL |
| `count` | 地址数量 |
| `status` | 0 成功，-1 失败 |
| `userdata` | 用户数据指针 |

---

## 函数

### xylem_addr_pton {#xylem_addr_pton}

```c
int xylem_addr_pton(const char* host, uint16_t port, xylem_addr_t* addr);
```

将主机字符串和端口转换为 `xylem_addr_t`。支持 IPv4 和 IPv6 地址字符串，内部先尝试 `AF_INET`，再尝试 `AF_INET6`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `host` | `const char*` | 主机地址字符串（如 `"127.0.0.1"`、`"::1"`） |
| `port` | `uint16_t` | 端口号 |
| `addr` | [`xylem_addr_t*`](#xylem_addr_t) | 输出地址结构体 |

**返回值：** 0 成功，-1 失败（无效地址）。

---

### xylem_addr_ntop {#xylem_addr_ntop}

```c
int xylem_addr_ntop(const xylem_addr_t* addr,
                    char* host, size_t hostlen, uint16_t* port);
```

将 `xylem_addr_t` 转换为可读的主机字符串和端口。

| 参数 | 类型 | 说明 |
|------|------|------|
| `addr` | [`const xylem_addr_t*`](#xylem_addr_t) | 输入地址结构体 |
| `host` | `char*` | 输出主机字符串缓冲区 |
| `hostlen` | `size_t` | 缓冲区大小 |
| `port` | `uint16_t*` | 输出端口号 |

**返回值：** 0 成功，-1 失败（不支持的地址族）。

---

### xylem_addr_resolve {#xylem_addr_resolve}

```c
xylem_addr_resolve_t* xylem_addr_resolve(xylem_loop_t* loop,
                                         xylem_thrdpool_t* pool,
                                         const char* host,
                                         uint16_t port,
                                         xylem_addr_resolve_fn_t cb,
                                         void* userdata);
```

使用线程池异步解析主机名。解析完成后通过 `xylem_loop_post()` 在事件循环线程上回调。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环（回调投递目标） |
| `pool` | [`xylem_thrdpool_t*`](thrdpool.md#xylem_thrdpool_t) | 线程池（后台解析） |
| `host` | `const char*` | 主机名或 IP 地址字符串 |
| `port` | `uint16_t` | 端口号 |
| `cb` | [`xylem_addr_resolve_fn_t`](#xylem_addr_resolve_fn_t) | 完成回调 |
| `userdata` | `void*` | 传递给回调的用户数据 |

**返回值：** 解析句柄，失败返回 `NULL`。

---

### xylem_addr_resolve_cancel {#xylem_addr_resolve_cancel}

```c
void xylem_addr_resolve_cancel(xylem_addr_resolve_t* req);
```

取消挂起的异步解析请求。若请求尚未投递到事件循环线程，回调不会被调用。若已完成则为空操作。

| 参数 | 类型 | 说明 |
|------|------|------|
| `req` | [`xylem_addr_resolve_t*`](#xylem_addr_resolve_t) | 解析句柄 |

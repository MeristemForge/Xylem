# UDP

`#include <xylem/xylem-udp.h>`

异步 UDP 收发，支持监听（listen）和拨号（dial）两种模式。无帧解析、无写队列——UDP 数据报天然保留消息边界。

---

## 类型

### xylem_udp_t {#xylem_udp_t}

不透明类型，表示一个 UDP 句柄。通过 [`xylem_udp_listen()`](#xylem_udp_listen) 或 [`xylem_udp_dial()`](#xylem_udp_dial) 获得。

### xylem_udp_handler_t {#xylem_udp_handler_t}

UDP 事件回调集合。

```c
typedef struct xylem_udp_handler_s {
    void (*on_read)(xylem_udp_t* udp, void* data, size_t len,
                    xylem_addr_t* addr);
    void (*on_close)(xylem_udp_t* udp, int err,
                     const char* errmsg);
} xylem_udp_handler_t;
```

| 回调 | 触发时机 |
|------|---------|
| `on_read` | 收到数据报。`addr` 为发送方地址 |
| `on_close` | 句柄关闭。`err`: 0=正常, >0=平台 errno |

---

## 监听

### xylem_udp_listen {#xylem_udp_listen}

```c
xylem_udp_t* xylem_udp_listen(xylem_loop_t* loop,
                               xylem_addr_t* addr,
                               xylem_udp_handler_t* handler);
```

绑定 UDP socket 并开始接收（未连接模式）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 绑定地址 |
| `handler` | [`xylem_udp_handler_t*`](#xylem_udp_handler_t) | 回调处理器 |

**返回值：** UDP 句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

## 拨号

### xylem_udp_dial {#xylem_udp_dial}

```c
xylem_udp_t* xylem_udp_dial(xylem_loop_t* loop,
                              xylem_addr_t* addr,
                              xylem_udp_handler_t* handler);
```

创建已连接 UDP socket。后续发送可使用 `dest=NULL`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `loop` | [`xylem_loop_t*`](loop.md#xylem_loop_t) | 事件循环 |
| `addr` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 目标地址 |
| `handler` | [`xylem_udp_handler_t*`](#xylem_udp_handler_t) | 回调处理器 |

**返回值：** UDP 句柄，失败返回 `NULL`。

!!! note
    必须在事件循环线程上调用。

---

## 收发

### xylem_udp_send {#xylem_udp_send}

```c
int xylem_udp_send(xylem_udp_t* udp, xylem_addr_t* dest,
                   const void* data, size_t len);
```

发送 UDP 数据报。

| 参数 | 类型 | 说明 |
|------|------|------|
| `udp` | [`xylem_udp_t*`](#xylem_udp_t) | UDP 句柄 |
| `dest` | [`xylem_addr_t*`](addr.md#xylem_addr_t) | 目标地址，已连接 socket 可传 `NULL` |
| `data` | `const void*` | 待发送数据 |
| `len` | `size_t` | 数据长度 |

**返回值：** 发送字节数，失败返回 -1。

!!! tip "线程安全"
    可从任意线程调用。

---

### xylem_udp_close {#xylem_udp_close}

```c
void xylem_udp_close(xylem_udp_t* udp);
```

关闭 UDP 句柄。关闭 socket，触发 `handler->on_close`。

| 参数 | 类型 | 说明 |
|------|------|------|
| `udp` | [`xylem_udp_t*`](#xylem_udp_t) | UDP 句柄 |

!!! tip "线程安全"
    可从任意线程调用。幂等——重复调用安全。

---

## 引用计数

### xylem_udp_acquire {#xylem_udp_acquire}

```c
void xylem_udp_acquire(xylem_udp_t* udp);
```

递增引用计数，防止句柄内存被释放。在将句柄传递给其他线程前调用。

!!! warning
    必须在事件循环线程上调用。

---

### xylem_udp_release {#xylem_udp_release}

```c
void xylem_udp_release(xylem_udp_t* udp);
```

递减引用计数。归零时释放句柄内存。可从任意线程调用。

---

## 访问器

### xylem_udp_get_loop {#xylem_udp_get_loop}

```c
xylem_loop_t* xylem_udp_get_loop(xylem_udp_t* udp);
```

获取句柄关联的事件循环。

---

### xylem_udp_get_userdata / set_userdata {#xylem_udp_get_userdata}

```c
void* xylem_udp_get_userdata(xylem_udp_t* udp);
void  xylem_udp_set_userdata(xylem_udp_t* udp, void* ud);
```

获取/设置句柄上的用户数据指针。

# 事件循环

`#include <xylem/xylem-loop.h>`

事件循环是 xylem 的核心，驱动所有异步 I/O、定时器和跨线程回调。

---

## 类型

### xylem_loop_t {#xylem_loop_t}

不透明类型，表示一个事件循环实例。

### xylem_loop_timer_t {#xylem_loop_timer_t}

不透明类型，表示一个定时器句柄。

### xylem_loop_io_t {#xylem_loop_io_t}

不透明类型，表示一个 I/O 监听句柄。

### xylem_loop_post_t {#xylem_loop_post_t}

不透明类型，表示一个延迟回调请求。

### xylem_poller_op_t {#xylem_poller_op_t}

I/O 事件掩码。

```c
typedef enum xylem_poller_op_e {
    XYLEM_POLLER_NO_OP = 0,  /* 无事件 */
    XYLEM_POLLER_RD_OP = 1,  /* 可读 */
    XYLEM_POLLER_WR_OP = 2,  /* 可写 */
    XYLEM_POLLER_RW_OP = 3,  /* 可读 + 可写 */
} xylem_poller_op_t;
```

### 回调签名

```c
/* 定时器回调 */
typedef void (*xylem_loop_timer_fn_t)(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud);

/* 延迟回调 */
typedef void (*xylem_loop_post_fn_t)(xylem_loop_t* loop,
                                     xylem_loop_post_t* req,
                                     void* ud);

/* I/O 事件回调 */
typedef void (*xylem_loop_io_fn_t)(xylem_loop_t* loop,
                                   xylem_loop_io_t* io,
                                   xylem_poller_op_t revents,
                                   void* ud);
```

---

## 生命周期

### xylem_loop_create {#xylem_loop_create}

```c
xylem_loop_t* xylem_loop_create(void);
```

创建事件循环。内部初始化 poller（epoll/kqueue/IOCP）、定时器堆和唤醒管道。

**返回值：** 循环句柄，失败返回 `NULL`。

---

### xylem_loop_destroy {#xylem_loop_destroy}

```c
void xylem_loop_destroy(xylem_loop_t* loop);
```

销毁事件循环，释放所有资源。调用前必须关闭所有句柄。

---

### xylem_loop_run {#xylem_loop_run}

```c
int xylem_loop_run(xylem_loop_t* loop);
```

运行事件循环。阻塞直到没有活跃句柄或调用 [`xylem_loop_stop()`](#xylem_loop_stop)。

每次迭代依次执行：

1. 计算最近定时器的超时时间
2. 调用 poller 等待 I/O 事件
3. 处理就绪的 I/O 回调
4. 处理到期的定时器
5. 排空 post 队列

**返回值：** 0 正常退出，-1 错误。

---

### xylem_loop_stop {#xylem_loop_stop}

```c
void xylem_loop_stop(xylem_loop_t* loop);
```

停止事件循环。循环在下一次迭代退出。

!!! tip "线程安全"
    可从任意线程调用。

---

## 定时器

### xylem_loop_create_timer {#xylem_loop_create_timer}

```c
xylem_loop_timer_t* xylem_loop_create_timer(xylem_loop_t* loop);
```

创建定时器句柄。不启动定时器，需调用 [`xylem_loop_start_timer()`](#xylem_loop_start_timer)。

---

### xylem_loop_destroy_timer {#xylem_loop_destroy_timer}

```c
void xylem_loop_destroy_timer(xylem_loop_timer_t* timer);
```

销毁定时器。若活跃则先停止。

---

### xylem_loop_start_timer {#xylem_loop_start_timer}

```c
int xylem_loop_start_timer(xylem_loop_timer_t* timer,
                           xylem_loop_timer_fn_t cb,
                           void* ud,
                           uint64_t timeout_ms,
                           uint64_t repeat_ms);
```

启动定时器。

| 参数 | 说明 |
|------|------|
| `timer` | 定时器句柄 |
| `cb` | 到期回调 |
| `ud` | 用户数据 |
| `timeout_ms` | 首次触发延迟（毫秒） |
| `repeat_ms` | 重复间隔（0 = 一次性） |

---

### xylem_loop_stop_timer {#xylem_loop_stop_timer}

```c
int xylem_loop_stop_timer(xylem_loop_timer_t* timer);
```

停止定时器。句柄仍有效，可重新启动。

---

### xylem_loop_reset_timer {#xylem_loop_reset_timer}

```c
int xylem_loop_reset_timer(xylem_loop_timer_t* timer, uint64_t timeout_ms);
```

重置定时器超时时间。等价于 stop + start，但更高效。

---

## I/O

### xylem_loop_create_io {#xylem_loop_create_io}

```c
xylem_loop_io_t* xylem_loop_create_io(xylem_loop_t* loop,
                                      xylem_poller_fd_t fd);
```

创建 I/O 句柄并绑定到文件描述符。不开始监听。

---

### xylem_loop_destroy_io {#xylem_loop_destroy_io}

```c
void xylem_loop_destroy_io(xylem_loop_io_t* io);
```

销毁 I/O 句柄。若活跃则先停止。

---

### xylem_loop_start_io {#xylem_loop_start_io}

```c
int xylem_loop_start_io(xylem_loop_io_t* io,
                        xylem_poller_op_t op,
                        xylem_loop_io_fn_t cb,
                        void* ud);
```

开始或更新 I/O 监听。

| 参数 | 说明 |
|------|------|
| `io` | I/O 句柄 |
| `op` | 事件掩码（[`XYLEM_POLLER_RD_OP`](#xylem_poller_op_t)、`WR_OP` 或两者） |
| `cb` | 事件回调 |
| `ud` | 用户数据 |

---

### xylem_loop_stop_io {#xylem_loop_stop_io}

```c
int xylem_loop_stop_io(xylem_loop_io_t* io);
```

停止 I/O 监听。句柄仍有效，可重新启动。

---

## 跨线程投递

### xylem_loop_post {#xylem_loop_post}

```c
int xylem_loop_post(xylem_loop_t* loop,
                    xylem_loop_post_fn_t cb,
                    void* ud);
```

将回调投递到事件循环线程执行。内部通过 MPSC 无锁队列 + socketpair 唤醒实现。

!!! tip "线程安全"
    可从任意线程调用。回调在下一次事件循环迭代中执行。

---

### xylem_loop_is_loop_thread {#xylem_loop_is_loop_thread}

```c
bool xylem_loop_is_loop_thread(xylem_loop_t* loop);
```

检查当前线程是否是事件循环线程。仅在 `xylem_loop_run()` 调用后有效。

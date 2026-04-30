# Thread Pool

`#include <xylem/xylem-thrdpool.h>`

固定大小线程池，支持异步任务投递。

---

## 类型

### xylem_thrdpool_t {#xylem_thrdpool_t}

不透明类型，表示一个线程池。通过 [`xylem_thrdpool_create()`](#xylem_thrdpool_create) 获得。

---

## 函数

### xylem_thrdpool_create {#xylem_thrdpool_create}

```c
xylem_thrdpool_t* xylem_thrdpool_create(int nthrds);
```

创建线程池并启动指定数量的工作线程。

| 参数 | 类型 | 说明 |
|------|------|------|
| `nthrds` | `int` | 工作线程数量 |

**返回值：** 线程池句柄，失败返回 `NULL`。

---

### xylem_thrdpool_post {#xylem_thrdpool_post}

```c
int xylem_thrdpool_post(xylem_thrdpool_t* restrict pool,
                        void (*routine)(void*),
                        void* arg);
```

向线程池投递一个异步任务。任务由下一个可用的工作线程执行。

| 参数 | 类型 | 说明 |
|------|------|------|
| `pool` | [`xylem_thrdpool_t*`](#xylem_thrdpool_t) | 线程池 |
| `routine` | `void (*)(void*)` | 任务函数 |
| `arg` | `void*` | 传递给任务函数的参数 |

**返回值：** 0 成功，-1 分配失败。

---

### xylem_thrdpool_destroy {#xylem_thrdpool_destroy}

```c
void xylem_thrdpool_destroy(xylem_thrdpool_t* restrict pool);
```

销毁线程池。通知所有工作线程停止，等待它们退出，释放队列中未处理的任务，销毁同步原语。

| 参数 | 类型 | 说明 |
|------|------|------|
| `pool` | [`xylem_thrdpool_t*`](#xylem_thrdpool_t) | 线程池 |

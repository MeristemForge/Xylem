# Ring Buffer

`#include <xylem/xylem-ringbuf.h>`

固定大小环形缓冲区，容量自动对齐到 2 的幂。

---

## 类型

### xylem_ringbuf_t {#xylem_ringbuf_t}

不透明类型，表示一个环形缓冲区。通过 [`xylem_ringbuf_create()`](#xylem_ringbuf_create) 获得。

---

## 函数

### xylem_ringbuf_create {#xylem_ringbuf_create}

```c
xylem_ringbuf_t* xylem_ringbuf_create(size_t esize, size_t bufsize);
```

创建环形缓冲区。内部容量向下取整到能容纳在 `bufsize` 字节内的最大 2 的幂条目数。

| 参数 | 类型 | 说明 |
|------|------|------|
| `esize` | `size_t` | 每个条目的字节数，必须 > 0 |
| `bufsize` | `size_t` | 总缓冲区大小（字节），必须 >= esize |

**返回值：** 环形缓冲区指针，失败返回 `NULL`。

---

### xylem_ringbuf_destroy {#xylem_ringbuf_destroy}

```c
void xylem_ringbuf_destroy(xylem_ringbuf_t* ring);
```

销毁环形缓冲区并释放内存。

---

### xylem_ringbuf_full {#xylem_ringbuf_full}

```c
bool xylem_ringbuf_full(xylem_ringbuf_t* ring);
```

检查缓冲区是否已满。**返回值：** 满返回 `true`。

---

### xylem_ringbuf_empty {#xylem_ringbuf_empty}

```c
bool xylem_ringbuf_empty(xylem_ringbuf_t* ring);
```

检查缓冲区是否为空。**返回值：** 空返回 `true`。

---

### xylem_ringbuf_len {#xylem_ringbuf_len}

```c
size_t xylem_ringbuf_len(xylem_ringbuf_t* ring);
```

返回当前存储的条目数。

---

### xylem_ringbuf_cap {#xylem_ringbuf_cap}

```c
size_t xylem_ringbuf_cap(xylem_ringbuf_t* ring);
```

返回缓冲区总容量（条目数，始终为 2 的幂）。

---

### xylem_ringbuf_avail {#xylem_ringbuf_avail}

```c
size_t xylem_ringbuf_avail(xylem_ringbuf_t* ring);
```

返回缓冲区满之前可写入的条目数。

---

### xylem_ringbuf_write {#xylem_ringbuf_write}

```c
size_t xylem_ringbuf_write(xylem_ringbuf_t* ring, const void* buf,
                           size_t entry_count);
```

向缓冲区写入条目。若可用空间不足，仅写入可容纳的数量。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ring` | [`xylem_ringbuf_t*`](#xylem_ringbuf_t) | 环形缓冲区 |
| `buf` | `const void*` | 源数据 |
| `entry_count` | `size_t` | 待写入条目数 |

**返回值：** 实际写入的条目数。

---

### xylem_ringbuf_read {#xylem_ringbuf_read}

```c
size_t xylem_ringbuf_read(xylem_ringbuf_t* ring, void* buf,
                          size_t entry_count);
```

从缓冲区读取并消费条目，推进读位置。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ring` | [`xylem_ringbuf_t*`](#xylem_ringbuf_t) | 环形缓冲区 |
| `buf` | `void*` | 目标缓冲区 |
| `entry_count` | `size_t` | 最大读取条目数 |

**返回值：** 实际读取的条目数。

---

### xylem_ringbuf_peek {#xylem_ringbuf_peek}

```c
size_t xylem_ringbuf_peek(xylem_ringbuf_t* ring, void* buf,
                          size_t entry_count);
```

从缓冲区读取条目但不消费（不推进读位置）。数据仍可用于后续 read 或 peek。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ring` | [`xylem_ringbuf_t*`](#xylem_ringbuf_t) | 环形缓冲区 |
| `buf` | `void*` | 目标缓冲区 |
| `entry_count` | `size_t` | 最大 peek 条目数 |

**返回值：** 实际复制的条目数。

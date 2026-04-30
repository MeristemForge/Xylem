# Queue

`#include <xylem/xylem-queue.h>`

侵入式 FIFO 队列，基于双向链表实现。

---

## 宏

### xylem_queue_entry {#xylem_queue_entry}

```c
#define xylem_queue_entry(x, t, m) xylem_list_entry(x, t, m)
```

从队列节点指针恢复包含该节点的外层结构体指针。是 [`xylem_list_entry`](list.md#xylem_list_entry) 的别名。

---

## 类型

### xylem_queue_t {#xylem_queue_t}

FIFO 队列结构体。

```c
struct xylem_queue_s {
    xylem_list_node_t head;
    size_t            nelts;
};
```

### xylem_queue_node_t {#xylem_queue_node_t}

队列节点，是 [`xylem_list_node_t`](list.md#xylem_list_node_t) 的别名。

```c
typedef xylem_list_node_t xylem_queue_node_t;
```

---

## 函数

### xylem_queue_init {#xylem_queue_init}

```c
void xylem_queue_init(xylem_queue_t* queue);
```

初始化 FIFO 队列。

---

### xylem_queue_empty {#xylem_queue_empty}

```c
bool xylem_queue_empty(xylem_queue_t* queue);
```

检查队列是否为空。**返回值：** 空返回 `true`。

---

### xylem_queue_len {#xylem_queue_len}

```c
size_t xylem_queue_len(xylem_queue_t* queue);
```

返回队列中的节点数量。

---

### xylem_queue_enqueue {#xylem_queue_enqueue}

```c
void xylem_queue_enqueue(xylem_queue_t* queue, xylem_queue_node_t* node);
```

将节点入队到队列尾部。

---

### xylem_queue_dequeue {#xylem_queue_dequeue}

```c
xylem_queue_node_t* xylem_queue_dequeue(xylem_queue_t* queue);
```

从队列头部出队并返回节点。

**返回值：** 出队的节点指针，队列为空返回 `NULL`。

---

### xylem_queue_front {#xylem_queue_front}

```c
xylem_queue_node_t* xylem_queue_front(xylem_queue_t* queue);
```

返回队列头部节点（不移除）。

**返回值：** 头部节点指针，队列为空返回 `NULL`。

---

### xylem_queue_back {#xylem_queue_back}

```c
xylem_queue_node_t* xylem_queue_back(xylem_queue_t* queue);
```

返回队列尾部节点（不移除）。

**返回值：** 尾部节点指针，队列为空返回 `NULL`。

---

### xylem_queue_swap {#xylem_queue_swap}

```c
void xylem_queue_swap(xylem_queue_t* queue1, xylem_queue_t* queue2);
```

O(1) 交换两个队列的内容。

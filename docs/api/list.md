# List

`#include <xylem/xylem-list.h>`

侵入式双向链表。

---

## 宏

### xylem_list_entry {#xylem_list_entry}

```c
#define xylem_list_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))
```

从链表节点指针恢复包含该节点的外层结构体指针。

| 参数 | 说明 |
|------|------|
| `x` | 链表节点指针 |
| `t` | 外层结构体类型 |
| `m` | 节点在结构体中的成员名 |

---

## 类型

### xylem_list_t {#xylem_list_t}

双向链表结构体。

```c
struct xylem_list_s {
    xylem_list_node_t head;
    size_t            nelts;
};
```

| 字段 | 说明 |
|------|------|
| `head` | 哨兵节点（内部边界标记） |
| `nelts` | 节点数量 |

### xylem_list_node_t {#xylem_list_node_t}

双向链表节点。

```c
struct xylem_list_node_s {
    struct xylem_list_node_s* prev;
    struct xylem_list_node_s* next;
};
```

---

## 函数

### xylem_list_init {#xylem_list_init}

```c
void xylem_list_init(xylem_list_t* list);
```

初始化双向链表。

---

### xylem_list_empty {#xylem_list_empty}

```c
bool xylem_list_empty(xylem_list_t* list);
```

检查链表是否为空。**返回值：** 空返回 `true`。

---

### xylem_list_len {#xylem_list_len}

```c
size_t xylem_list_len(xylem_list_t* list);
```

返回链表中的节点数量。

---

### xylem_list_insert_head {#xylem_list_insert_head}

```c
void xylem_list_insert_head(xylem_list_t* list, xylem_list_node_t* node);
```

在链表头部插入节点。

---

### xylem_list_insert_tail {#xylem_list_insert_tail}

```c
void xylem_list_insert_tail(xylem_list_t* list, xylem_list_node_t* node);
```

在链表尾部插入节点。

---

### xylem_list_remove {#xylem_list_remove}

```c
void xylem_list_remove(xylem_list_t* list, xylem_list_node_t* node);
```

从链表中移除指定节点。节点必须当前在链表中。

---

### xylem_list_head {#xylem_list_head}

```c
xylem_list_node_t* xylem_list_head(xylem_list_t* list);
```

返回链表第一个节点（不移除）。链表为空返回 `NULL`。

---

### xylem_list_tail {#xylem_list_tail}

```c
xylem_list_node_t* xylem_list_tail(xylem_list_t* list);
```

返回链表最后一个节点（不移除）。链表为空返回 `NULL`。

---

### xylem_list_next {#xylem_list_next}

```c
xylem_list_node_t* xylem_list_next(xylem_list_node_t* node);
```

返回节点的后继。到达链表末尾时返回哨兵节点，使用 [`xylem_list_sentinel()`](#xylem_list_sentinel) 检测。

---

### xylem_list_prev {#xylem_list_prev}

```c
xylem_list_node_t* xylem_list_prev(xylem_list_node_t* node);
```

返回节点的前驱。到达链表开头时返回哨兵节点，使用 [`xylem_list_sentinel()`](#xylem_list_sentinel) 检测。

---

### xylem_list_sentinel {#xylem_list_sentinel}

```c
xylem_list_node_t* xylem_list_sentinel(xylem_list_t* list);
```

返回链表的哨兵节点。遍历时将指针与哨兵比较以检测迭代结束。

---

### xylem_list_swap {#xylem_list_swap}

```c
void xylem_list_swap(xylem_list_t* a, xylem_list_t* b);
```

交换两个链表的内容。交换后 `a` 持有原 `b` 的元素，反之亦然。适用于原子排空链表：与空链表交换后处理非空链表。

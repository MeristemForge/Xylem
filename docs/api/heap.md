# Heap

`#include <xylem/xylem-heap.h>`

侵入式二叉最小堆。

---

## 宏

### xylem_heap_entry {#xylem_heap_entry}

```c
#define xylem_heap_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))
```

从堆节点指针恢复包含该节点的外层结构体指针。

| 参数 | 说明 |
|------|------|
| `x` | 堆节点指针 |
| `t` | 外层结构体类型 |
| `m` | 节点在结构体中的成员名 |

---

## 类型

### xylem_heap_t {#xylem_heap_t}

二叉最小堆结构体。

```c
struct xylem_heap_s {
    struct xylem_heap_node_s* root;
    size_t                    nelts;
    int (*cmp)(const xylem_heap_node_t* child, const xylem_heap_node_t* parent);
};
```

| 字段 | 说明 |
|------|------|
| `root` | 根节点指针 |
| `nelts` | 节点数量 |
| `cmp` | 比较函数。返回负值表示 child 优先级高于 parent（应更靠近根） |

### xylem_heap_node_t {#xylem_heap_node_t}

堆节点。

```c
struct xylem_heap_node_s {
    struct xylem_heap_node_s* left;
    struct xylem_heap_node_s* right;
    struct xylem_heap_node_s* parent;
};
```

---

## 函数

### xylem_heap_init {#xylem_heap_init}

```c
void xylem_heap_init(xylem_heap_t* heap,
                     int (*cmp)(const xylem_heap_node_t* child,
                                const xylem_heap_node_t* parent));
```

初始化二叉最小堆。

| 参数 | 类型 | 说明 |
|------|------|------|
| `heap` | [`xylem_heap_t*`](#xylem_heap_t) | 堆结构体 |
| `cmp` | `int (*)(...)` | 比较函数。返回负值表示 child 优先级高于 parent |

---

### xylem_heap_insert {#xylem_heap_insert}

```c
void xylem_heap_insert(xylem_heap_t* heap, xylem_heap_node_t* node);
```

向堆中插入节点。调用者将节点嵌入自己的结构体中，通过 [`xylem_heap_entry()`](#xylem_heap_entry) 恢复。

---

### xylem_heap_remove {#xylem_heap_remove}

```c
void xylem_heap_remove(xylem_heap_t* heap, xylem_heap_node_t* node);
```

从堆中移除指定节点。节点必须当前在堆中。

---

### xylem_heap_dequeue {#xylem_heap_dequeue}

```c
void xylem_heap_dequeue(xylem_heap_t* heap);
```

移除根节点（最高优先级）。堆为空时为空操作。

---

### xylem_heap_empty {#xylem_heap_empty}

```c
bool xylem_heap_empty(xylem_heap_t* heap);
```

检查堆是否为空。**返回值：** 空返回 `true`。

---

### xylem_heap_root {#xylem_heap_root}

```c
xylem_heap_node_t* xylem_heap_root(xylem_heap_t* heap);
```

返回根节点（最高优先级）但不移除。

**返回值：** 根节点指针，堆为空返回 `NULL`。

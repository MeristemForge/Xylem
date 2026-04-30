# Red-Black Tree

`#include <xylem/xylem-rbtree.h>`

侵入式红黑树，支持节点-节点和键-节点两种比较器。

---

## 宏

### xylem_rbtree_entry {#xylem_rbtree_entry}

```c
#define xylem_rbtree_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))
```

从红黑树节点指针恢复包含该节点的外层结构体指针。

| 参数 | 说明 |
|------|------|
| `x` | 树节点指针 |
| `t` | 外层结构体类型 |
| `m` | 节点在结构体中的成员名 |

---

## 类型

### xylem_rbtree_t {#xylem_rbtree_t}

红黑树结构体。

```c
struct xylem_rbtree_s {
    xylem_rbtree_node_t*     root;
    xylem_rbtree_cmp_nn_fn_t cmp_nn;
    xylem_rbtree_cmp_kn_fn_t cmp_kn;
};
```

| 字段 | 说明 |
|------|------|
| `root` | 根节点指针 |
| `cmp_nn` | 节点-节点比较器（用于插入） |
| `cmp_kn` | 键-节点比较器（用于查找） |

### xylem_rbtree_node_t {#xylem_rbtree_node_t}

红黑树节点。

```c
struct xylem_rbtree_node_s {
    struct xylem_rbtree_node_s* parent;
    struct xylem_rbtree_node_s* right;
    struct xylem_rbtree_node_s* left;
    uint8_t                     color;
};
```

### xylem_rbtree_cmp_nn_fn_t {#xylem_rbtree_cmp_nn_fn_t}

节点-节点比较函数类型。

```c
typedef int (*xylem_rbtree_cmp_nn_fn_t)(const xylem_rbtree_node_t* child,
                                        const xylem_rbtree_node_t* parent);
```

返回负值表示 child 排在 parent 之前。

### xylem_rbtree_cmp_kn_fn_t {#xylem_rbtree_cmp_kn_fn_t}

键-节点比较函数类型，用于 [`xylem_rbtree_find()`](#xylem_rbtree_find) 查找。

```c
typedef int (*xylem_rbtree_cmp_kn_fn_t)(const void* key,
                                        const xylem_rbtree_node_t* parent);
```

---

## 函数

### xylem_rbtree_init {#xylem_rbtree_init}

```c
void xylem_rbtree_init(xylem_rbtree_t* tree,
                       xylem_rbtree_cmp_nn_fn_t cmp_nn,
                       xylem_rbtree_cmp_kn_fn_t cmp_kn);
```

初始化红黑树。

| 参数 | 类型 | 说明 |
|------|------|------|
| `tree` | [`xylem_rbtree_t*`](#xylem_rbtree_t) | 树结构体 |
| `cmp_nn` | [`xylem_rbtree_cmp_nn_fn_t`](#xylem_rbtree_cmp_nn_fn_t) | 节点-节点比较器 |
| `cmp_kn` | [`xylem_rbtree_cmp_kn_fn_t`](#xylem_rbtree_cmp_kn_fn_t) | 键-节点比较器 |

---

### xylem_rbtree_insert {#xylem_rbtree_insert}

```c
void xylem_rbtree_insert(xylem_rbtree_t* tree, xylem_rbtree_node_t* node);
```

向树中插入节点。通过 [`xylem_rbtree_entry()`](#xylem_rbtree_entry) 恢复外层结构体。

---

### xylem_rbtree_erase {#xylem_rbtree_erase}

```c
void xylem_rbtree_erase(xylem_rbtree_t* tree, xylem_rbtree_node_t* node);
```

从树中移除节点。节点必须当前在树中。

---

### xylem_rbtree_empty {#xylem_rbtree_empty}

```c
bool xylem_rbtree_empty(xylem_rbtree_t* tree);
```

检查树是否为空。**返回值：** 空返回 `true`。

---

### xylem_rbtree_find {#xylem_rbtree_find}

```c
xylem_rbtree_node_t* xylem_rbtree_find(xylem_rbtree_t* tree, const void* key);
```

使用 `cmp_kn` 比较器按键查找节点。

| 参数 | 类型 | 说明 |
|------|------|------|
| `tree` | [`xylem_rbtree_t*`](#xylem_rbtree_t) | 树 |
| `key` | `const void*` | 查找键 |

**返回值：** 匹配的节点指针，未找到返回 `NULL`。

---

### xylem_rbtree_next {#xylem_rbtree_next}

```c
xylem_rbtree_node_t* xylem_rbtree_next(xylem_rbtree_node_t* node);
```

返回节点的中序后继。

**返回值：** 后继节点指针，`node` 为最后一个时返回 `NULL`。

---

### xylem_rbtree_prev {#xylem_rbtree_prev}

```c
xylem_rbtree_node_t* xylem_rbtree_prev(xylem_rbtree_node_t* node);
```

返回节点的中序前驱。

**返回值：** 前驱节点指针，`node` 为第一个时返回 `NULL`。

---

### xylem_rbtree_first {#xylem_rbtree_first}

```c
xylem_rbtree_node_t* xylem_rbtree_first(xylem_rbtree_t* tree);
```

返回树中最小（最左）节点。

**返回值：** 最小节点指针，树为空返回 `NULL`。

---

### xylem_rbtree_last {#xylem_rbtree_last}

```c
xylem_rbtree_node_t* xylem_rbtree_last(xylem_rbtree_t* tree);
```

返回树中最大（最右）节点。

**返回值：** 最大节点指针，树为空返回 `NULL`。

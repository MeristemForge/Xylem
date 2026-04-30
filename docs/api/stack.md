# Stack

`#include <xylem/xylem-stack.h>`

侵入式 LIFO 栈。

---

## 宏

### xylem_stack_entry {#xylem_stack_entry}

```c
#define xylem_stack_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))
```

从栈节点指针恢复包含该节点的外层结构体指针。

| 参数 | 说明 |
|------|------|
| `x` | 栈节点指针 |
| `t` | 外层结构体类型 |
| `m` | 节点在结构体中的成员名 |

---

## 类型

### xylem_stack_t {#xylem_stack_t}

LIFO 栈结构体。

```c
struct xylem_stack_s {
    xylem_stack_node_t* top;
    size_t              nelts;
};
```

| 字段 | 说明 |
|------|------|
| `top` | 栈顶节点指针 |
| `nelts` | 节点数量 |

### xylem_stack_node_t {#xylem_stack_node_t}

栈节点（单向链接）。

```c
struct xylem_stack_node_s {
    struct xylem_stack_node_s* next;
};
```

---

## 函数

### xylem_stack_init {#xylem_stack_init}

```c
void xylem_stack_init(xylem_stack_t* stack);
```

初始化 LIFO 栈。

---

### xylem_stack_empty {#xylem_stack_empty}

```c
bool xylem_stack_empty(xylem_stack_t* stack);
```

检查栈是否为空。**返回值：** 空返回 `true`。

---

### xylem_stack_len {#xylem_stack_len}

```c
size_t xylem_stack_len(xylem_stack_t* stack);
```

返回栈中的节点数量。

---

### xylem_stack_push {#xylem_stack_push}

```c
void xylem_stack_push(xylem_stack_t* stack, xylem_stack_node_t* node);
```

将节点压入栈顶。

---

### xylem_stack_pop {#xylem_stack_pop}

```c
xylem_stack_node_t* xylem_stack_pop(xylem_stack_t* stack);
```

弹出并返回栈顶节点。

**返回值：** 栈顶节点指针，栈为空返回 `NULL`。

---

### xylem_stack_peek {#xylem_stack_peek}

```c
xylem_stack_node_t* xylem_stack_peek(xylem_stack_t* stack);
```

返回栈顶节点（不移除）。

**返回值：** 栈顶节点指针，栈为空返回 `NULL`。

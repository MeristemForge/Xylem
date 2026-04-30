# JSON

`#include <xylem/xylem-json.h>`

JSON 解析、序列化和构建器，支持读取和可变操作。

---

## 类型

### xylem_json_type_t {#xylem_json_type_t}

JSON 值类型枚举。

```c
typedef enum xylem_json_type_e {
    XYLEM_JSON_TYPE_NONE = 0,
    XYLEM_JSON_TYPE_NULL,
    XYLEM_JSON_TYPE_BOOL,
    XYLEM_JSON_TYPE_NUM,
    XYLEM_JSON_TYPE_STR,
    XYLEM_JSON_TYPE_ARR,
    XYLEM_JSON_TYPE_OBJ
} xylem_json_type_t;
```

| 值 | 说明 |
|----|------|
| `TYPE_NONE` | 无效或 NULL 句柄 |
| `TYPE_NULL` | JSON null |
| `TYPE_BOOL` | 布尔值 |
| `TYPE_NUM` | 数值 |
| `TYPE_STR` | 字符串 |
| `TYPE_ARR` | 数组 |
| `TYPE_OBJ` | 对象 |

### xylem_json_t {#xylem_json_t}

不透明类型，表示一个 JSON 值句柄。通过 [`xylem_json_parse()`](#xylem_json_parse)、[`xylem_json_new_obj()`](#xylem_json_new_obj) 或 [`xylem_json_new_arr()`](#xylem_json_new_arr) 获得。

---

## 解析与序列化

### xylem_json_parse {#xylem_json_parse}

```c
xylem_json_t* xylem_json_parse(const char* str);
```

解析 JSON 字符串，创建只读句柄。

| 参数 | 类型 | 说明 |
|------|------|------|
| `str` | `const char*` | 以 null 结尾的 JSON 字符串 |

**返回值：** JSON 句柄，解析失败返回 `NULL`。

!!! note
    返回的句柄必须通过 `xylem_json_destroy()` 释放。

---

### xylem_json_destroy {#xylem_json_destroy}

```c
void xylem_json_destroy(xylem_json_t* j);
```

销毁 JSON 句柄并释放所有关联内存。`NULL` 安全。

---

### xylem_json_print {#xylem_json_print}

```c
char* xylem_json_print(const xylem_json_t* j);
```

将 JSON 句柄序列化为紧凑字符串。

**返回值：** 新分配的 null 结尾字符串，错误返回 `NULL`。调用者需用 `free()` 释放。

---

### xylem_json_print_pretty {#xylem_json_print_pretty}

```c
char* xylem_json_print_pretty(const xylem_json_t* j);
```

将 JSON 句柄序列化为格式化字符串。

**返回值：** 新分配的 null 结尾字符串，错误返回 `NULL`。调用者需用 `free()` 释放。

---

### xylem_json_type {#xylem_json_type}

```c
xylem_json_type_t xylem_json_type(const xylem_json_t* j);
```

获取 JSON 值的类型。

**返回值：** 值类型，`j` 为 NULL 时返回 `XYLEM_JSON_TYPE_NONE`。

---

## Getter

### xylem_json_str {#xylem_json_str}

```c
const char* xylem_json_str(const xylem_json_t* j, const char* key);
```

按键获取字符串值。

**返回值：** 字符串指针（在句柄销毁前有效），键不存在或类型不匹配返回 `NULL`。

---

### xylem_json_i32 {#xylem_json_i32}

```c
int32_t xylem_json_i32(const xylem_json_t* j, const char* key);
```

按键获取 32 位有符号整数。键不存在或类型不匹配返回 0。

---

### xylem_json_i64 {#xylem_json_i64}

```c
int64_t xylem_json_i64(const xylem_json_t* j, const char* key);
```

按键获取 64 位有符号整数。键不存在或类型不匹配返回 0。

---

### xylem_json_f64 {#xylem_json_f64}

```c
double xylem_json_f64(const xylem_json_t* j, const char* key);
```

按键获取双精度浮点数。键不存在或类型不匹配返回 0.0。

---

### xylem_json_bool {#xylem_json_bool}

```c
bool xylem_json_bool(const xylem_json_t* j, const char* key);
```

按键获取布尔值。键不存在或类型不匹配返回 `false`。

---

### xylem_json_obj {#xylem_json_obj}

```c
xylem_json_t* xylem_json_obj(const xylem_json_t* j, const char* key);
```

按键获取嵌套对象。

**返回值：** 子对象句柄，未找到返回 `NULL`。

!!! note
    返回的句柄与父句柄共享所有权，不要对其调用 `xylem_json_destroy()`。

---

### xylem_json_arr {#xylem_json_arr}

```c
xylem_json_t* xylem_json_arr(const xylem_json_t* j, const char* key);
```

按键获取嵌套数组。

**返回值：** 子数组句柄，未找到返回 `NULL`。

!!! note
    返回的句柄与父句柄共享所有权，不要对其调用 `xylem_json_destroy()`。

---

## 数组访问

### xylem_json_arr_len {#xylem_json_arr_len}

```c
size_t xylem_json_arr_len(const xylem_json_t* j);
```

获取数组元素数量。`j` 为 NULL 或非数组时返回 0。

---

### xylem_json_arr_get {#xylem_json_arr_get}

```c
xylem_json_t* xylem_json_arr_get(const xylem_json_t* j, size_t index);
```

按索引获取数组元素。

**返回值：** 元素句柄，越界返回 `NULL`。

!!! note
    返回的句柄与父句柄共享所有权，不要对其调用 `xylem_json_destroy()`。

---

## 构建器

### xylem_json_new_obj {#xylem_json_new_obj}

```c
xylem_json_t* xylem_json_new_obj(void);
```

创建空的可变 JSON 对象。

**返回值：** 可变对象句柄，分配失败返回 `NULL`。必须通过 `xylem_json_destroy()` 释放。

---

### xylem_json_new_arr {#xylem_json_new_arr}

```c
xylem_json_t* xylem_json_new_arr(void);
```

创建空的可变 JSON 数组。

**返回值：** 可变数组句柄，分配失败返回 `NULL`。必须通过 `xylem_json_destroy()` 释放。

---

## Setter

### xylem_json_set_str {#xylem_json_set_str}

```c
int xylem_json_set_str(xylem_json_t* j, const char* key, const char* val);
```

在对象上设置字符串值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_i32 {#xylem_json_set_i32}

```c
int xylem_json_set_i32(xylem_json_t* j, const char* key, int32_t val);
```

在对象上设置 32 位整数值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_i64 {#xylem_json_set_i64}

```c
int xylem_json_set_i64(xylem_json_t* j, const char* key, int64_t val);
```

在对象上设置 64 位整数值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_f64 {#xylem_json_set_f64}

```c
int xylem_json_set_f64(xylem_json_t* j, const char* key, double val);
```

在对象上设置双精度浮点值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_bool {#xylem_json_set_bool}

```c
int xylem_json_set_bool(xylem_json_t* j, const char* key, bool val);
```

在对象上设置布尔值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_null {#xylem_json_set_null}

```c
int xylem_json_set_null(xylem_json_t* j, const char* key);
```

在对象上设置 null 值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_obj {#xylem_json_set_obj}

```c
int xylem_json_set_obj(xylem_json_t* j, const char* key, xylem_json_t* child);
```

在对象上设置嵌套对象。`child` 被消费，调用成功后不可再使用或销毁。

**返回值：** 0 成功，-1 失败。

---

### xylem_json_set_arr {#xylem_json_set_arr}

```c
int xylem_json_set_arr(xylem_json_t* j, const char* key, xylem_json_t* child);
```

在对象上设置嵌套数组。`child` 被消费，调用成功后不可再使用或销毁。

**返回值：** 0 成功，-1 失败。

---

## 数组追加

### xylem_json_arr_push {#xylem_json_arr_push}

```c
int xylem_json_arr_push(xylem_json_t* j, xylem_json_t* item);
```

向数组追加一个 JSON 值。`item` 被消费，调用成功后不可再使用或销毁。

**返回值：** 0 成功，-1 失败。

---

### xylem_json_arr_push_str {#xylem_json_arr_push_str}

```c
int xylem_json_arr_push_str(xylem_json_t* j, const char* val);
```

向数组追加字符串值。**返回值：** 0 成功，-1 失败。

---

### xylem_json_arr_push_i64 {#xylem_json_arr_push_i64}

```c
int xylem_json_arr_push_i64(xylem_json_t* j, int64_t val);
```

向数组追加 64 位整数。**返回值：** 0 成功，-1 失败。

---

### xylem_json_arr_push_f64 {#xylem_json_arr_push_f64}

```c
int xylem_json_arr_push_f64(xylem_json_t* j, double val);
```

向数组追加双精度浮点数。**返回值：** 0 成功，-1 失败。

---

### xylem_json_arr_push_bool {#xylem_json_arr_push_bool}

```c
int xylem_json_arr_push_bool(xylem_json_t* j, bool val);
```

向数组追加布尔值。**返回值：** 0 成功，-1 失败。

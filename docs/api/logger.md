# Logger

`#include <xylem/xylem-logger.h>`

日志系统，支持文件输出、异步写入、日志级别过滤和自定义回调。

---

## 类型

### xylem_logger_level_t {#xylem_logger_level_t}

日志级别枚举。

```c
typedef enum xylem_logger_level_e {
    XYLEM_LOGGER_LEVEL_DEBUG,
    XYLEM_LOGGER_LEVEL_INFO,
    XYLEM_LOGGER_LEVEL_WARN,
    XYLEM_LOGGER_LEVEL_ERROR,
} xylem_logger_level_t;
```

---

## 宏

### xylem_logd / xylem_logi / xylem_logw / xylem_loge {#xylem_log_macros}

```c
#define xylem_logd(...)  xylem_logger_log(XYLEM_LOGGER_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define xylem_logi(...)  xylem_logger_log(XYLEM_LOGGER_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define xylem_logw(...)  xylem_logger_log(XYLEM_LOGGER_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define xylem_loge(...)  xylem_logger_log(XYLEM_LOGGER_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
```

便捷日志宏，自动填充源文件名和行号。

---

## 函数

### xylem_logger_init {#xylem_logger_init}

```c
void xylem_logger_init(const char* restrict filename,
                       xylem_logger_level_t level,
                       bool async,
                       size_t max_file_size);
```

初始化日志系统。

| 参数 | 类型 | 说明 |
|------|------|------|
| `filename` | `const char*` | 日志文件路径，`NULL` 输出到 stdout |
| `level` | [`xylem_logger_level_t`](#xylem_logger_level_t) | 最低输出级别 |
| `async` | `bool` | `true` 通过线程池异步写入 |
| `max_file_size` | `size_t` | 日志文件最大大小（字节），超过后截断重写。0 表示无限制。`filename` 为 NULL 时忽略 |

---

### xylem_logger_deinit {#xylem_logger_deinit}

```c
void xylem_logger_deinit(void);
```

反初始化日志系统并释放资源。

---

### xylem_logger_set_callback {#xylem_logger_set_callback}

```c
void xylem_logger_set_callback(
    void (*callback)(xylem_logger_level_t level,
                     const char* restrict msg,
                     void* ud),
    void* ud);
```

设置自定义日志回调。设置后文件输出被旁路。

| 参数 | 类型 | 说明 |
|------|------|------|
| `callback` | `void (*)(...)` | 接收日志消息的回调函数 |
| `ud` | `void*` | 每次调用传递给回调的用户数据 |

---

### xylem_logger_log {#xylem_logger_log}

```c
void xylem_logger_log(xylem_logger_level_t level,
                      const char* restrict file, int line,
                      const char* restrict fmt, ...);
```

记录一条日志消息。通常通过 [`xylem_logd/i/w/e`](#xylem_log_macros) 宏调用。

| 参数 | 类型 | 说明 |
|------|------|------|
| `level` | [`xylem_logger_level_t`](#xylem_logger_level_t) | 日志级别 |
| `file` | `const char*` | 源文件名 |
| `line` | `int` | 源代码行号 |
| `fmt` | `const char*` | printf 风格格式字符串 |
| `...` | | 格式参数 |

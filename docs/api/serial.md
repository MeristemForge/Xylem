# Serial

`#include <xylem/xylem-serial.h>`

跨平台同步串口通信接口，支持 Windows 和 Unix。所有 I/O 均为阻塞式，无事件循环集成。

---

## 类型

### xylem_serial_t {#xylem_serial_t}

不透明类型，表示一个串口句柄。通过 [`xylem_serial_open()`](#xylem_serial_open) 获得。

### xylem_serial_baudrate_t {#xylem_serial_baudrate_t}

波特率枚举。

```c
typedef enum xylem_serial_baudrate_e {
    XYLEM_SERIAL_BAUDRATE_9600,   /* 9600 baud */
    XYLEM_SERIAL_BAUDRATE_19200,  /* 19200 baud */
    XYLEM_SERIAL_BAUDRATE_38400,  /* 38400 baud */
    XYLEM_SERIAL_BAUDRATE_57600,  /* 57600 baud */
    XYLEM_SERIAL_BAUDRATE_115200, /* 115200 baud */
} xylem_serial_baudrate_t;
```

### xylem_serial_parity_t {#xylem_serial_parity_t}

校验模式枚举。

```c
typedef enum xylem_serial_parity_e {
    XYLEM_SERIAL_PARITY_NONE, /* 无校验 */
    XYLEM_SERIAL_PARITY_ODD,  /* 奇校验 */
    XYLEM_SERIAL_PARITY_EVEN, /* 偶校验 */
} xylem_serial_parity_t;
```

### xylem_serial_databits_t {#xylem_serial_databits_t}

数据位枚举。

```c
typedef enum xylem_serial_databits_e {
    XYLEM_SERIAL_DATABITS_7, /* 7 数据位 */
    XYLEM_SERIAL_DATABITS_8, /* 8 数据位 */
} xylem_serial_databits_t;
```

### xylem_serial_stopbits_t {#xylem_serial_stopbits_t}

停止位枚举。

```c
typedef enum xylem_serial_stopbits_e {
    XYLEM_SERIAL_STOPBITS_1, /* 1 停止位 */
    XYLEM_SERIAL_STOPBITS_2, /* 2 停止位 */
} xylem_serial_stopbits_t;
```

### xylem_serial_flowcontrol_t {#xylem_serial_flowcontrol_t}

流控枚举。

```c
typedef enum xylem_serial_flowcontrol_e {
    XYLEM_SERIAL_FLOW_NONE,     /* 无流控 */
    XYLEM_SERIAL_FLOW_HARDWARE, /* 硬件流控 (RTS/CTS) */
} xylem_serial_flowcontrol_t;
```

### xylem_serial_opts_t {#xylem_serial_opts_t}

串口配置结构体。

```c
typedef struct xylem_serial_opts_s {
    const char*                  device;       /* 设备路径 ("COM3", "/dev/ttyUSB0") */
    xylem_serial_baudrate_t      baudrate;     /* 波特率 */
    xylem_serial_parity_t        parity;       /* 校验模式 */
    xylem_serial_databits_t      databits;     /* 数据位 */
    xylem_serial_stopbits_t      stopbits;     /* 停止位 */
    xylem_serial_flowcontrol_t   flowcontrol;  /* 流控，默认 NONE */
    uint32_t                     timeout_ms;   /* 读超时（ms），0 = 阻塞 */
} xylem_serial_opts_t;
```

| 字段 | 说明 |
|------|------|
| `device` | 设备路径，不可为 NULL |
| `baudrate` | 波特率，见 [`xylem_serial_baudrate_t`](#xylem_serial_baudrate_t) |
| `parity` | 校验模式 |
| `databits` | 数据位 |
| `stopbits` | 停止位 |
| `flowcontrol` | 流控模式 |
| `timeout_ms` | 读超时（毫秒）。0 表示阻塞直到至少 1 字节到达 |

---

## 函数

### xylem_serial_open {#xylem_serial_open}

```c
xylem_serial_t* xylem_serial_open(xylem_serial_opts_t* opts);
```

打开并配置串口。

| 参数 | 类型 | 说明 |
|------|------|------|
| `opts` | [`xylem_serial_opts_t*`](#xylem_serial_opts_t) | 串口配置，不可为 NULL |

**返回值：** 串口句柄，失败返回 `NULL`。

!!! note
    所有枚举参数越界时返回 NULL。`opts` 或 `opts->device` 为 NULL 时返回 NULL。

---

### xylem_serial_close {#xylem_serial_close}

```c
void xylem_serial_close(xylem_serial_t* serial);
```

关闭串口并释放资源。

| 参数 | 类型 | 说明 |
|------|------|------|
| `serial` | [`xylem_serial_t*`](#xylem_serial_t) | 串口句柄，NULL 安全 |

!!! warning
    非幂等——不要对同一个非 NULL 句柄调用两次（double-free）。

---

### xylem_serial_read {#xylem_serial_read}

```c
int xylem_serial_read(xylem_serial_t* serial, void* buf, size_t len);
```

从串口阻塞读取数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `serial` | [`xylem_serial_t*`](#xylem_serial_t) | 串口句柄 |
| `buf` | `void*` | 接收缓冲区 |
| `len` | `size_t` | 最大读取字节数 |

**返回值：** 读取的字节数（可能小于 `len`），超时无数据返回 0，错误返回 -1。`serial` 为 NULL 时返回 -1。

---

### xylem_serial_write {#xylem_serial_write}

```c
int xylem_serial_write(xylem_serial_t* serial, const void* buf, size_t len);
```

向串口阻塞写入全部数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| `serial` | [`xylem_serial_t*`](#xylem_serial_t) | 串口句柄 |
| `buf` | `const void*` | 待写入数据 |
| `len` | `size_t` | 写入字节数 |

**返回值：** 成功返回 `len`（全部写入），错误返回 -1。`serial` 为 NULL 时返回 -1。

!!! note
    所有 I/O 均为同步阻塞，无事件循环集成。

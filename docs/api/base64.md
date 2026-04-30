# Base64

`#include <xylem/xylem-base64.h>`

Base64 编码/解码，支持标准 (RFC 4648) 和 URL 安全变体。

---

## 函数

### xylem_base64_encode_size {#xylem_base64_encode_size}

```c
int xylem_base64_encode_size(int slen);
```

计算 Base64 编码输出的精确字节数（含 padding）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `slen` | `int` | 输入数据长度 |

**返回值：** 编码输出大小（字节）。

---

### xylem_base64_decode_size {#xylem_base64_decode_size}

```c
int xylem_base64_decode_size(int slen);
```

计算 Base64 解码输出的最大字节数。实际解码大小可能因 padding 而更小。

| 参数 | 类型 | 说明 |
|------|------|------|
| `slen` | `int` | 编码输入长度 |

**返回值：** 最大解码输出大小（字节）。

---

### xylem_base64_encode_std {#xylem_base64_encode_std}

```c
int xylem_base64_encode_std(const uint8_t* src, int slen,
                            uint8_t* dst, int dlen);
```

使用标准 Base64 字母表（A-Z, a-z, 0-9, '+', '/'）编码，始终追加 '=' padding。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | 输入二进制数据 |
| `slen` | `int` | 输入长度 |
| `dst` | `uint8_t*` | 输出缓冲区（无 null 终止符） |
| `dlen` | `int` | 输出缓冲区大小 |

**返回值：** 写入 `dst` 的字节数，`dlen` 不足返回 -1。

!!! note
    使用 `xylem_base64_encode_size()` 确定所需的 `dlen`。

---

### xylem_base64_decode_std {#xylem_base64_decode_std}

```c
int xylem_base64_decode_std(const uint8_t* src, int slen,
                            uint8_t* dst, int dlen);
```

解码标准 Base64 字符串（使用 '+', '/' 和 '=' padding）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | Base64 编码字符串（可含 '=' padding） |
| `slen` | `int` | 输入长度 |
| `dst` | `uint8_t*` | 输出缓冲区 |
| `dlen` | `int` | 输出缓冲区大小 |

**返回值：** 解码字节数，错误返回 -1（无效字符、padding 错误或 `dlen` 不足）。

!!! note
    使用 `xylem_base64_decode_size()` 确定所需的 `dlen`。

---

### xylem_base64_encode_url {#xylem_base64_encode_url}

```c
int xylem_base64_encode_url(const uint8_t* src, int slen,
                            uint8_t* dst, int dlen, bool padding);
```

使用 URL 安全 Base64 字母表（A-Z, a-z, 0-9, '-', '_'）编码。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | 输入二进制数据 |
| `slen` | `int` | 输入长度 |
| `dst` | `uint8_t*` | 输出缓冲区（无 null 终止符） |
| `dlen` | `int` | 输出缓冲区大小 |
| `padding` | `bool` | `true` 追加 '=' padding，`false` 省略 |

**返回值：** 写入 `dst` 的字节数，`dlen` 不足返回 -1。

---

### xylem_base64_decode_url {#xylem_base64_decode_url}

```c
int xylem_base64_decode_url(const uint8_t* src, int slen,
                            uint8_t* dst, int dlen, bool padding);
```

解码 URL 安全 Base64 字符串（使用 '-' 和 '_'）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | URL 安全 Base64 字符串 |
| `slen` | `int` | 输入长度 |
| `dst` | `uint8_t*` | 输出缓冲区 |
| `dlen` | `int` | 输出缓冲区大小 |
| `padding` | `bool` | `true` 强制 padding 规则（长度必须为 4 的倍数），`false` 接受无 padding 输入 |

**返回值：** 解码字节数，错误返回 -1。

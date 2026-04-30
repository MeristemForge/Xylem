# Varint

`#include <xylem/xylem-varint.h>`

变长整数编码/解码（LEB128 风格）。

---

## 函数

### xylem_varint_compute {#xylem_varint_compute}

```c
size_t xylem_varint_compute(uint64_t value);
```

计算将一个值编码为 varint 所需的字节数。

| 参数 | 类型 | 说明 |
|------|------|------|
| `value` | `uint64_t` | 待编码的无符号 64 位整数 |

**返回值：** 所需字节数（1 到 10）。

---

### xylem_varint_encode {#xylem_varint_encode}

```c
bool xylem_varint_encode(uint64_t value, uint8_t* buf,
                         size_t bufsize, size_t* pos);
```

将 64 位无符号整数编码为 varint 写入缓冲区。

| 参数 | 类型 | 说明 |
|------|------|------|
| `value` | `uint64_t` | 待编码的值 |
| `buf` | `uint8_t*` | 输出缓冲区 |
| `bufsize` | `size_t` | 缓冲区总大小 |
| `pos` | `size_t*` | 输入/输出字节偏移，编码后更新到写入数据之后 |

**返回值：** `true` 成功，`false` 缓冲区空间不足。

---

### xylem_varint_decode {#xylem_varint_decode}

```c
bool xylem_varint_decode(const uint8_t* buf, size_t bufsize,
                         size_t* pos, uint64_t* out_value);
```

从缓冲区解码一个 varint 为 64 位无符号整数。

| 参数 | 类型 | 说明 |
|------|------|------|
| `buf` | `const uint8_t*` | 输入缓冲区 |
| `bufsize` | `size_t` | 缓冲区总大小 |
| `pos` | `size_t*` | 输入/输出字节偏移，解码后更新到消费数据之后 |
| `out_value` | `uint64_t*` | 输出解码值 |

**返回值：** `true` 成功，`false` 输入截断或格式错误。

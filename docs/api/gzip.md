# Gzip

`#include <xylem/xylem-gzip.h>`

Gzip (RFC 1952) 和 raw DEFLATE (RFC 1951) 压缩/解压缩。

---

## 函数

### xylem_gzip_compress_bound {#xylem_gzip_compress_bound}

```c
size_t xylem_gzip_compress_bound(size_t slen);
```

计算 gzip 压缩输出的最大字节数上界。用于预分配输出缓冲区。

| 参数 | 类型 | 说明 |
|------|------|------|
| `slen` | `size_t` | 未压缩输入长度 |

**返回值：** 压缩输出大小上界（字节）。

---

### xylem_gzip_compress {#xylem_gzip_compress}

```c
int xylem_gzip_compress(const uint8_t *src, size_t slen,
                        uint8_t *dst, size_t dlen, int level);
```

使用 gzip 格式压缩数据。输出包含完整的 gzip 头、压缩载荷、CRC-32 和原始大小尾部。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | 输入数据 |
| `slen` | `size_t` | 输入长度 |
| `dst` | `uint8_t*` | 输出缓冲区 |
| `dlen` | `size_t` | 输出缓冲区大小 |
| `level` | `int` | 压缩级别（0=无, 1=最快, 9=最优, -1=默认） |

**返回值：** 写入 `dst` 的字节数，`dlen` 不足或压缩失败返回 -1。

!!! note
    使用 `xylem_gzip_compress_bound()` 确定所需的 `dlen`。

---

### xylem_gzip_decompress {#xylem_gzip_decompress}

```c
int xylem_gzip_decompress(const uint8_t *src, size_t slen,
                          uint8_t *dst, size_t dlen);
```

解压缩 gzip 格式数据。解析 gzip 头，解压载荷，验证 CRC-32 尾部。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | gzip 压缩数据 |
| `slen` | `size_t` | 压缩数据长度 |
| `dst` | `uint8_t*` | 输出缓冲区 |
| `dlen` | `size_t` | 输出缓冲区大小 |

**返回值：** 解压后写入 `dst` 的字节数，错误返回 -1（无效数据、CRC 不匹配或 `dlen` 不足）。

---

### xylem_gzip_deflate_bound {#xylem_gzip_deflate_bound}

```c
size_t xylem_gzip_deflate_bound(size_t slen);
```

计算 raw DEFLATE 压缩输出的最大字节数上界。

| 参数 | 类型 | 说明 |
|------|------|------|
| `slen` | `size_t` | 未压缩输入长度 |

**返回值：** deflate 输出大小上界（字节）。

---

### xylem_gzip_deflate {#xylem_gzip_deflate}

```c
int xylem_gzip_deflate(const uint8_t *src, size_t slen,
                       uint8_t *dst, size_t dlen, int level);
```

使用 raw DEFLATE 格式压缩数据（无头部或尾部）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | 输入数据 |
| `slen` | `size_t` | 输入长度 |
| `dst` | `uint8_t*` | 输出缓冲区 |
| `dlen` | `size_t` | 输出缓冲区大小 |
| `level` | `int` | 压缩级别（0=无, 1=最快, 9=最优, -1=默认） |

**返回值：** 写入 `dst` 的字节数，`dlen` 不足或压缩失败返回 -1。

!!! note
    使用 `xylem_gzip_deflate_bound()` 确定所需的 `dlen`。

---

### xylem_gzip_inflate {#xylem_gzip_inflate}

```c
int xylem_gzip_inflate(const uint8_t *src, size_t slen,
                       uint8_t *dst, size_t dlen);
```

解压缩 raw DEFLATE 数据（无头部或尾部）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `src` | `const uint8_t*` | deflate 数据 |
| `slen` | `size_t` | 数据长度 |
| `dst` | `uint8_t*` | 输出缓冲区 |
| `dlen` | `size_t` | 输出缓冲区大小 |

**返回值：** 解压后写入 `dst` 的字节数，错误返回 -1（无效数据或 `dlen` 不足）。

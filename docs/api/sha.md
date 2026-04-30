# SHA / HMAC

`#include <xylem/xylem-sha1.h>`
`#include <xylem/xylem-sha256.h>`
`#include <xylem/xylem-hmac256.h>`

SHA-1、SHA-256 哈希和 HMAC-SHA256 消息认证码。

---

## SHA-1

### xylem_sha1_t {#xylem_sha1_t}

不透明类型，表示一个 SHA-1 上下文。通过 [`xylem_sha1_create()`](#xylem_sha1_create) 获得。

---

### xylem_sha1_create {#xylem_sha1_create}

```c
xylem_sha1_t* xylem_sha1_create(void);
```

创建并初始化 SHA-1 上下文。

**返回值：** 上下文指针，分配失败返回 `NULL`。

---

### xylem_sha1_update {#xylem_sha1_update}

```c
void xylem_sha1_update(xylem_sha1_t* ctx, const uint8_t* data, size_t len);
```

向 SHA-1 上下文输入数据。可多次调用以增量哈希。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_sha1_t*`](#xylem_sha1_t) | SHA-1 上下文 |
| `data` | `const uint8_t*` | 输入数据 |
| `len` | `size_t` | 数据长度 |

---

### xylem_sha1_final {#xylem_sha1_final}

```c
void xylem_sha1_final(xylem_sha1_t* ctx, uint8_t digest[20]);
```

完成哈希计算，输出 20 字节摘要。调用后上下文不应再用于后续 update。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_sha1_t*`](#xylem_sha1_t) | SHA-1 上下文 |
| `digest` | `uint8_t[20]` | 输出缓冲区（至少 20 字节） |

---

### xylem_sha1_destroy {#xylem_sha1_destroy}

```c
void xylem_sha1_destroy(xylem_sha1_t* ctx);
```

销毁 SHA-1 上下文并清零敏感数据。

---

## SHA-256

### xylem_sha256_t {#xylem_sha256_t}

不透明类型，表示一个 SHA-256 上下文。通过 [`xylem_sha256_create()`](#xylem_sha256_create) 获得。

---

### xylem_sha256_create {#xylem_sha256_create}

```c
xylem_sha256_t* xylem_sha256_create(void);
```

创建并初始化 SHA-256 上下文。

**返回值：** 上下文指针，分配失败返回 `NULL`。

---

### xylem_sha256_update {#xylem_sha256_update}

```c
void xylem_sha256_update(xylem_sha256_t* ctx, const uint8_t* data, size_t len);
```

向 SHA-256 上下文输入数据。可多次调用以增量哈希。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_sha256_t*`](#xylem_sha256_t) | SHA-256 上下文 |
| `data` | `const uint8_t*` | 输入数据 |
| `len` | `size_t` | 数据长度 |

---

### xylem_sha256_final {#xylem_sha256_final}

```c
void xylem_sha256_final(xylem_sha256_t* ctx, uint8_t digest[32]);
```

完成哈希计算，输出 32 字节摘要。调用后上下文不应再用于后续 update。

| 参数 | 类型 | 说明 |
|------|------|------|
| `ctx` | [`xylem_sha256_t*`](#xylem_sha256_t) | SHA-256 上下文 |
| `digest` | `uint8_t[32]` | 输出缓冲区（至少 32 字节） |

---

### xylem_sha256_destroy {#xylem_sha256_destroy}

```c
void xylem_sha256_destroy(xylem_sha256_t* ctx);
```

销毁 SHA-256 上下文并清零敏感数据。

---

## HMAC-SHA256

### xylem_hmac256_compute {#xylem_hmac256_compute}

```c
void xylem_hmac256_compute(const uint8_t* key, size_t key_len,
                           const uint8_t* msg, size_t msg_len,
                           uint8_t out[32]);
```

一次性计算 HMAC-SHA256 消息认证码。

| 参数 | 类型 | 说明 |
|------|------|------|
| `key` | `const uint8_t*` | 密钥 |
| `key_len` | `size_t` | 密钥长度（超过 64 字节会先哈希） |
| `msg` | `const uint8_t*` | 待认证消息 |
| `msg_len` | `size_t` | 消息长度 |
| `out` | `uint8_t[32]` | 输出缓冲区（至少 32 字节） |

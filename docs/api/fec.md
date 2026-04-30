# FEC

`#include <xylem/xylem-fec.h>`

Reed-Solomon 前向纠错编解码器。

---

## 类型

### xylem_fec_t {#xylem_fec_t}

不透明类型，表示一个 Reed-Solomon FEC 编解码器。通过 [`xylem_fec_create()`](#xylem_fec_create) 获得。

---

## 函数

### xylem_fec_create {#xylem_fec_create}

```c
xylem_fec_t* xylem_fec_create(int data_shards, int parity_shards);
```

创建 Reed-Solomon FEC 编解码器。

| 参数 | 类型 | 说明 |
|------|------|------|
| `data_shards` | `int` | 数据分片数（1..254） |
| `parity_shards` | `int` | 奇偶校验分片数（1..255-data_shards） |

**返回值：** 编解码器句柄，参数无效或分配失败返回 `NULL`。

---

### xylem_fec_destroy {#xylem_fec_destroy}

```c
void xylem_fec_destroy(xylem_fec_t* fec);
```

销毁编解码器并释放所有关联内存。`NULL` 安全。

---

### xylem_fec_encode {#xylem_fec_encode}

```c
int xylem_fec_encode(xylem_fec_t* fec, uint8_t** data,
                     uint8_t** parity, size_t shard_size);
```

从数据分片生成奇偶校验分片。每个分片必须恰好为 `shard_size` 字节。

| 参数 | 类型 | 说明 |
|------|------|------|
| `fec` | [`xylem_fec_t*`](#xylem_fec_t) | 编解码器 |
| `data` | `uint8_t**` | data_shards 个指针的数组，每个指向 shard_size 字节的输入数据 |
| `parity` | `uint8_t**` | parity_shards 个指针的数组，每个指向 shard_size 字节的输出缓冲区 |
| `shard_size` | `size_t` | 每个分片的字节数 |

**返回值：** 0 成功，-1 失败。

---

### xylem_fec_reconstruct {#xylem_fec_reconstruct}

```c
int xylem_fec_reconstruct(xylem_fec_t* fec, uint8_t** shards,
                          uint8_t* marks, size_t shard_size);
```

原地重建丢失的数据分片。`shards` 为 `[data_0 .. data_N-1, parity_0 .. parity_M-1]`。`marks` 标记丢失的分片（非零 = 丢失）。仅恢复数据分片；丢失的奇偶校验分片被忽略（需重新编码以再生）。

| 参数 | 类型 | 说明 |
|------|------|------|
| `fec` | [`xylem_fec_t*`](#xylem_fec_t) | 编解码器 |
| `shards` | `uint8_t**` | (data_shards + parity_shards) 个指针的数组 |
| `marks` | `uint8_t*` | (data_shards + parity_shards) 字节的标记数组 |
| `shard_size` | `size_t` | 每个分片的字节数 |

**返回值：** 0 成功，-1 失败（丢失过多或输入无效）。

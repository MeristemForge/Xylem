# Encoding Design

The encoding modules are stateless (mostly) data transforms: Base64, URL
percent-encoding, varint, byte-swap, gzip/DEFLATE, JSON, and Reed-Solomon FEC.
They share a buffer and error convention so they compose predictably and never
own memory the caller didn't hand them.

Sources: public headers in `include/xylem/encoding/`, implementations in
`src/encoding/` (with bundled codecs under `src/encoding/gzip/`,
`src/encoding/json/`, `src/encoding/fec/`).

## 1. The caller-owns-buffers convention

The byte-transform modules (Base64, URL, gzip, varint, FEC) never allocate
output. The pattern is **size-then-fill**:

```c
int need = xylem_base64_encode_size(slen);   /* exact (encode) or upper bound (decode) */
uint8_t* dst = malloc(need);
int n = xylem_base64_encode_std(src, slen, dst, need);   /* n bytes written, or -1 */
```

- A `*_size()` helper returns the **exact** output size for encode paths and a
  **conservative upper bound** for decode/compress paths.
- The transform returns the number of bytes actually written, or `-1` if the
  destination is too small or the input is malformed.
- Outputs are **not** null-terminated — these are byte transforms, not string
  APIs. The caller knows the length from the return value.
- All of these functions are **pure and thread-safe**: no shared state, safe to
  call from any thread or coroutine.

JSON is the exception (it builds an owned object tree); it is covered in §6.

## 2. Base64 (`xylem-base64`)

RFC 4648. Two alphabets, sizing shared:

| Function | Alphabet | Padding |
|----------|----------|---------|
| `encode_std` / `decode_std` | standard (`+`, `/`) | always `=` |
| `encode_url` / `decode_url` | URL-safe (`-`, `_`) | optional (`padding` flag) |

`encode_size(slen)` gives the exact padded size; `decode_size(slen)` gives the
max (actual is smaller when padding is present). Decode returns `-1` on invalid
characters or bad padding.

## 3. URL percent-encoding (`xylem-url`)

RFC 3986. Unreserved characters (`A-Z a-z 0-9 - _ . ~`) pass through; everything
else becomes `%XX`.

- `encode_size` worst case is `3 * slen` (every byte escaped); `decode_size`
  upper bound is `slen` (output never grows).
- **Decode is lenient:** invalid `%XX` sequences pass through unchanged rather
  than failing — handy for tolerant query-string parsing. It returns `-1` only
  when the destination buffer is too small.

## 4. Varint (`xylem-varint`)

LEB128-style variable-length unsigned integers (1–10 bytes for a `uint64_t`).
Uses an in/out `pos` cursor so multiple values can be packed/unpacked in
sequence over one buffer:

```c
size_t pos = 0;
xylem_varint_encode(v, buf, sizeof buf, &pos);   /* advances pos */
...
xylem_varint_decode(buf, len, &pos, &out);       /* advances pos */
```

`compute(value)` returns the encoded length without writing. Both encode and
decode return `bool` (`false` on overflow / truncation / malformed input).

## 5. Byte-swap (`xylem-bswap`)

A C11 `_Generic` macro `xylem_bswap(x)` dispatches to the right typed routine
for `uint{16,32,64}_t`, `int{16,32,64}_t`, `float`, and `double`. Floating-point
swaps reinterpret the bit pattern via `memcpy` (no strict-aliasing UB) and
reconstruct the value. Pairs with `xylem_utils_getendian()` for portable
on-the-wire integer/float serialization. Pure, branch-light, thread-safe.

## 6. JSON (`xylem-json`)

The one stateful module: a parse/build tree behind an opaque `xylem_json_t`.

- **Parse / serialize:** `parse(str)` → handle (read-only view, input not
  modified); `print` / `print_pretty` → newly allocated string the caller
  `free()`s; `destroy` frees the tree.
- **Typed accessors by key:** `str`/`i32`/`i64`/`f64`/`bool`, plus `obj`/`arr`
  for nesting and `arr_len`/`arr_get` for arrays. Missing/mismatched keys return
  a benign zero value (`NULL`, `0`, `0.0`, `false`) rather than erroring.
- **Builders:** `new_obj`/`new_arr` plus `set_*` and `arr_push*`.

Ownership rules are the sharp edges, and they are explicit in the header:

- A handle returned by `obj`/`arr`/`arr_get` **shares ownership** with its
  parent — never `destroy()` it; destroy only the root.
- `set_obj`/`set_arr`/`arr_push` **consume** the child handle — don't use or
  free it after a successful call.

So the rule of thumb: you only `destroy()` a handle you got from `parse`,
`new_obj`, or `new_arr` and did **not** attach to another tree.

## 7. gzip / DEFLATE (`xylem-gzip`)

Two framing levels over the same compressor:

| Pair | Format | Framing |
|------|--------|---------|
| `compress` / `decompress` | gzip (RFC 1952) | header + payload + CRC-32 + size trailer |
| `deflate` / `inflate` | raw DEFLATE (RFC 1951) | payload only |

Each has its own `*_size()` upper bound. `level` is `0`=none, `1`=fastest,
`9`=best, `-1`=default. `decompress` verifies the CRC-32 and returns `-1` on
mismatch / invalid data / short buffer. Raw DEFLATE is what the WebSocket
`permessage-deflate` extension and HTTP content-encoding build on.

## 8. Reed-Solomon FEC (`xylem-fec`)

Forward error correction over GF(256): turn `K` data shards into `M` parity
shards so any `K` of the `K+M` total reconstruct the originals. This is the
erasure code behind RUDP's loss recovery.

- `create(data_shards, parity_shards)` — `data ∈ [1,254]`,
  `parity ∈ [1, 255-data]`; handle or `NULL`.
- `encode(data[], parity[], shard_size)` — fills the parity shards; every shard
  is exactly `shard_size` bytes.
- `reconstruct(shards[], marks[], shard_size)` — `shards` is
  `[data_0..data_{K-1}, parity_0..parity_{M-1}]`; `marks[i] != 0` flags shard
  `i` as lost. Recovers **data** shards in place; lost parity shards are left
  alone (re-encode to regenerate). Returns `-1` if losses exceed `M`.

Unlike the other encoders this one is a handle (it precomputes the coding
matrix), so it follows the `create`/`destroy` lifecycle; a codec instance is not
internally synchronized, so don't share one across threads mid-call.

## 9. Module summary

| Module | Style | Allocates? | Thread-safe |
|--------|-------|-----------|-------------|
| base64 | pure fn | no (caller buffer) | yes |
| url | pure fn | no | yes |
| varint | pure fn + cursor | no | yes |
| bswap | `_Generic` macro | no | yes |
| gzip | pure fn | no | yes |
| json | handle tree | yes (owns tree) | per-handle, not shared |
| fec | handle codec | yes (owns matrix) | per-handle, not shared |

## 10. Related docs

- Conventions (sizing, `0`/`-1`/`NULL`): [`../conventions.md`](../conventions.md)
- FEC consumer: RUDP (design doc deferred).
- DEFLATE consumers: WebSocket / HTTP (design docs deferred).
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*

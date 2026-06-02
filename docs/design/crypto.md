# Crypto Design

Xylem bundles a small set of self-contained crypto primitives: SHA-1, SHA-256,
HMAC-SHA256, and AES-256 (CTR and CBC). They have **no OpenSSL dependency** —
this is the always-available crypto, distinct from the optional OpenSSL-backed
TLS/DTLS layer (gated by `XYLEM_ENABLE_TLS`).

Sources: public headers in `include/xylem/crypto/`, implementations in
`src/crypto/` (AES under `src/crypto/aes256/`).

## 1. Scope and intent

| Primitive | Header | Use in Xylem |
|-----------|--------|--------------|
| SHA-1 | `xylem-sha1` | WebSocket handshake `Sec-WebSocket-Accept` |
| SHA-256 | `xylem-sha256` | general hashing, HMAC building block |
| HMAC-SHA256 | `xylem-hmac256` | message authentication, token signing |
| AES-256-CTR/CBC | `xylem-aes256` | symmetric encryption |

These are general-purpose building blocks for application protocols. They are
**not** a TLS replacement — transport security goes through the OpenSSL-backed
TLS/DTLS modules. (SHA-1 is included specifically because the WebSocket spec
mandates it for the handshake, not as a recommendation for new designs.)

## 2. Two API shapes

### Streaming hashes — `create` / `update` / `final` / `destroy`

SHA-1 and SHA-256 are incremental:

```c
xylem_sha256_t* h = xylem_sha256_create();
xylem_sha256_update(h, data, len);     /* call as many times as needed */
uint8_t digest[32];
xylem_sha256_final(h, digest);         /* 32-byte (SHA-1: 20-byte) digest */
xylem_sha256_destroy(h);               /* zeroes sensitive state */
```

- Digest sizes are fixed and encoded in the signature: `uint8_t digest[32]` for
  SHA-256, `digest[20]` for SHA-1.
- After `final`, the context must not be `update`d again.
- `destroy` **zeroizes** the context before freeing.

### One-shot — HMAC

HMAC-SHA256 is a single call (keys longer than the 64-byte block are pre-hashed
per the HMAC spec):

```c
uint8_t mac[32];
xylem_hmac256_compute(key, key_len, msg, msg_len, mac);
```

### Keyed context with sized buffers — AES

AES-256 expands the key once into a reusable context, then follows the same
size-then-fill buffer convention as the encoding modules
(see [`encoding.md`](encoding.md) §1):

```c
xylem_aes256_t* c = xylem_aes256_create(key32);
size_t need = xylem_aes256_ctr_encrypt_size(slen);
int n = xylem_aes256_ctr_encrypt(c, src, slen, dst, need);   /* IV||ciphertext, or -1 */
xylem_aes256_destroy(c);                                     /* zeroizes key schedule */
```

## 3. AES-256 details

Two modes, both **self-framing with a prepended IV**:

| Mode | Padding | Output layout | Length constraint |
|------|---------|---------------|-------------------|
| CTR | none (stream) | `IV(16) ‖ ciphertext` | any plaintext length |
| CBC | PKCS7 | `IV(16) ‖ padded ciphertext` | block-aligned after pad |

Key points:

- **The IV is generated internally** with the platform CSPRNG
  (`platform_info_getrandom`, i.e. BCryptGenRandom / `/dev/urandom`) and
  prepended to the output on encrypt; decrypt reads it back from the front. The
  caller never supplies or manages an IV.
- A fresh random IV per encryption means encryption is **not deterministic** —
  the same plaintext yields different ciphertext each call (intended).
- `*_encrypt_size()` returns the exact required size (incl. IV and, for CBC,
  padding); `*_decrypt_size()` returns the max, and the actual decrypted length
  is smaller after CBC unpadding. Both return `0` when the input is too short to
  even hold an IV.
- CBC decrypt returns `-1` on invalid PKCS7 padding or non-block-aligned input.
- One context is **key-bound and reusable** across many operations but is not
  internally synchronized — don't share a single context across threads mid-call
  (create one per thread, or serialize).

## 4. Security notes and boundaries

- **No authenticated encryption mode.** CTR and CBC provide confidentiality, not
  integrity. If you need authenticity, combine with HMAC (encrypt-then-MAC) at
  the application layer — the primitives are here, the composition is the
  caller's responsibility.
- **SHA-1 is legacy-only.** Present for the WebSocket handshake; do not use it
  for new integrity or signature schemes (use SHA-256/HMAC-SHA256).
- **Constant-time guarantees are not claimed by these headers.** For
  adversarial transport security, prefer the OpenSSL-backed TLS path.
- **Zeroization** on `destroy` limits how long key material lingers in memory,
  but callers still own the lifetime of their own key/plaintext buffers.

## 5. Relationship to TLS

This module is the dependency-free crypto that ships unconditionally. The
TLS/DTLS modules are a separate, optional layer built on OpenSSL ≥ 3.5 and gated
by `XYLEM_ENABLE_TLS`; when that is off, TLS is stubbed but these primitives
remain fully available. See [`../architecture.md`](../architecture.md) §8 for the
feature-gate overview, and [`tls.md`](tls.md) for the TLS/DTLS design.

## 6. Related docs

- Buffer/size conventions: [`encoding.md`](encoding.md) §1,
  [`../conventions.md`](../conventions.md) §3.
- CSPRNG source: [`platform.md`](platform.md) §7 (`platform_info_getrandom`).
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*

# TLS Backend Abstraction Design

## Status

Proposed. This is a pure internal refactor: the public `xylem_tls_*` and
`xylem_dtls_*` API, ABI, and runtime behavior are unchanged. The goal is to
isolate every OpenSSL dependency behind one backend-neutral internal interface
so an alternative SSL library (wolfSSL via its OpenSSL-compatibility layer, or
mbedTLS with a native implementation) can be added later by writing a single
new backend source file, with zero edits to the engine or the layers above it.

## 1. Motivation and scope

Today OpenSSL is reached directly from three places:

- `src/net/tls/tls.c` + `tls.h` — the TLS engine (`SSL_CTX`/`SSL`/`BIO`/
  `X509`/`EVP_PKEY`).
- `src/net/xylem-dtls.c` — the DTLS engine, which additionally uses DTLS-only
  OpenSSL surface (`DTLS_method`, cookie callbacks, `DTLSv1_get_timeout`/
  `handle_timeout`, `RAND_bytes`).
- `src/platform/{unix,win}/platform-tls.c` + `platform-tls.h` — system root
  certificate loading, whose signature takes a raw `SSL_CTX*` and whose
  mechanism is OpenSSL-specific.

The layers *above* the engine are already clean: `http-transport-tls.c` talks
to the internal `tls_*` engine API and `ws-tls.c` to the public `xylem_tls_*`
shim; neither includes OpenSSL. So the abstraction boundary is drawn at the
engine, not above it.

**Scope (agreed):** TLS, DTLS, and system-CA loading all move behind the
backend interface. After this change, exactly one source file
(`tls-backend-openssl.c`) includes `<openssl/...>`.

**Out of scope:** runtime multi-backend selection. Backend choice is a
**compile-time** decision (one backend per build, no function-pointer
indirection, no per-call cost). The current default and only backend is
OpenSSL.

## 2. The boundary: what stays, what moves

The key enabler is that both engines already drive OpenSSL through a pair of
in-memory BIOs and pump the socket separately (see [`tls.md`](tls.md) §2). That
"feed ciphertext in / drain ciphertext out" model maps cleanly onto every
candidate backend (OpenSSL memory BIOs, mbedTLS `mbedtls_ssl_set_bio`
callbacks, wolfSSL I/O callbacks). So the cut is:

| Concern | Owner after refactor |
|---|---|
| Socket send/recv, `iowait` parking, coroutine scheduling | **Engine** (`tls.c`, `dtls.c`) — written once |
| The three locks (`ssl_mu`/`rd_mu`/`wr_mu`) and the duplex contract | **Engine** |
| Connection refcounting, deadlines, listener/accept loop, DTLS session table / dispatcher / retransmit timer | **Engine** |
| SSL state machine (handshake/read/write), certificate parsing, SNI table, ALPN wire encoding, verify flags, keylog, DTLS cookies, system-CA loading | **Backend** (`tls-backend-openssl.c`) |

The engine includes only `tls-backend.h`. It no longer includes any
`<openssl/...>` header, and `tls_conn_t` no longer holds `SSL*`/`BIO*`.

## 3. Backend-neutral types

All declared in `src/net/tls/tls-backend.h`. No OpenSSL types appear here.

```c
typedef struct tls_backend_ctx_s  tls_backend_ctx_t;   /* opaque */
typedef struct tls_backend_conn_s tls_backend_conn_t;  /* opaque */

/* Result of a handshake/read/write step. Maps from SSL_get_error on
 * OpenSSL; maps from MBEDTLS_ERR_SSL_WANT_READ/WRITE on a future mbedTLS
 * backend. */
typedef enum {
    TLS_BACKEND_OK,
    TLS_BACKEND_WANT_READ,
    TLS_BACKEND_WANT_WRITE,
    TLS_BACKEND_CLOSED,     /* clean peer shutdown (SSL_ERROR_ZERO_RETURN) */
    TLS_BACKEND_ERROR
} tls_backend_state_t;

/* Verify policy, computed by the engine from role + ctx intent. */
typedef enum {
    TLS_BACKEND_VERIFY_NONE,     /* SSL_VERIFY_NONE */
    TLS_BACKEND_VERIFY_PEER,     /* SSL_VERIFY_PEER (client) */
    TLS_BACKEND_VERIFY_REQUIRE   /* PEER | FAIL_IF_NO_PEER_CERT (mTLS server) */
} tls_backend_verify_t;

typedef enum {
    TLS_BACKEND_PROTO_TLS,
    TLS_BACKEND_PROTO_DTLS
} tls_backend_proto_t;

/* One-shot, pre-handshake connection configuration snapshot. The engine
 * fills this from neutral decisions it already owns (role -> verify, and
 * whether server_name is an IP literal / whether the peer is verified,
 * both decided with the project's own addr_pton, not OpenSSL). The
 * backend must COPY any string it needs (e.g. SSL_set1_host copies):
 * the pointers reference engine-owned temporaries. */
typedef struct {
    tls_backend_verify_t verify;
    const char*          sni_name;     /* client, non-IP only; else NULL */
    const char*          verify_host;  /* set only when verify != NONE;  else NULL */
} tls_backend_handshake_cfg_t;
```

### Why three verify states (not two)

`VERIFY_PEER` and `VERIFY_REQUIRE` differ only by OpenSSL's
`SSL_VERIFY_FAIL_IF_NO_PEER_CERT` flag. A client uses `PEER` (the server
always sends a certificate); an mTLS server uses `REQUIRE` to reject a client
that presents none. Collapsing them would silently downgrade an mTLS server to
"verify if presented, allow if absent" — a security regression. The three
states are a faithful map of the current `_tls_apply_verify` logic.

The engine computes the state in one neutral expression (replacing the
duplicated TLS and DTLS copies):

```c
verify = is_server ? (ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                          : TLS_BACKEND_VERIFY_NONE)
                   : (ctx->verify_server ? TLS_BACKEND_VERIFY_PEER
                                         : TLS_BACKEND_VERIFY_NONE);
```

## 4. Backend interface (shared by TLS and DTLS)

```c
/* ---- context: shared configuration ---- */
tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto);
void               tls_backend_ctx_destroy(tls_backend_ctx_t* ctx);

int tls_backend_ctx_load_cert_file(tls_backend_ctx_t* ctx, const char* hostname,
                                   const char* cert_file, const char* key_file);
int tls_backend_ctx_load_cert_mem (tls_backend_ctx_t* ctx, const char* hostname,
                                   const void* cert_pem, size_t cert_len,
                                   const void* key_pem,  size_t key_len);
int tls_backend_ctx_load_ca_file  (tls_backend_ctx_t* ctx, const char* ca_file);
int tls_backend_ctx_load_system_ca(tls_backend_ctx_t* ctx);
int tls_backend_ctx_set_alpn      (tls_backend_ctx_t* ctx,
                                   const char** protocols, size_t count);
int tls_backend_ctx_set_keylog    (tls_backend_ctx_t* ctx, const char* path);

/* ---- connection: one SSL state machine over memory buffers ---- */
tls_backend_conn_t* tls_backend_conn_create(tls_backend_ctx_t* ctx,
                                            bool is_server);
void tls_backend_conn_destroy(tls_backend_conn_t* c);

/* One-shot pre-handshake config (verify + SNI + verify_host). See §3. */
void tls_backend_conn_configure(tls_backend_conn_t* c,
                                const tls_backend_handshake_cfg_t* cfg);

/* Ciphertext transfer to/from the backend's inbound/outbound buffers.
 * feed: hand inbound ciphertext to the state machine (was BIO_write).
 * drain: take pending outbound ciphertext (was BIO_read); returns the
 *        byte count (>0), 0 when empty, -1 on error. */
int tls_backend_conn_feed (tls_backend_conn_t* c, const void* buf, int len);
int tls_backend_conn_drain(tls_backend_conn_t* c, void* buf, int cap);

/* State-machine steps. The engine pumps the socket in the requested
 * direction on WANT_READ/WANT_WRITE, then retries. read/write report the
 * plaintext byte count through out_n on TLS_BACKEND_OK. */
tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c);
tls_backend_state_t tls_backend_conn_read (tls_backend_conn_t* c,
                                           void* buf, int len, int* out_n);
tls_backend_state_t tls_backend_conn_write(tls_backend_conn_t* c,
                                           const void* buf, int len, int* out_n);

/* Best-effort close_notify into the outbound buffer (engine drains it). */
void tls_backend_conn_shutdown(tls_backend_conn_t* c);

/* Copy the negotiated ALPN protocol into a caller buffer (NUL-terminated,
 * empty string if none). The engine caches this once post-handshake. */
void tls_backend_conn_get_alpn(tls_backend_conn_t* c, char* buf, size_t cap);
```

### DTLS-only extensions

DTLS reuses the same `tls_backend_conn_t`; it only adds datagram-specific
steps. These live under the `dtls_backend_` prefix to keep the shared surface
honest:

```c
void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu);
void dtls_backend_conn_set_peer_addr(tls_backend_conn_t* c,
                                     const void* sockaddr, size_t salen);
bool dtls_backend_conn_get_timeout(tls_backend_conn_t* c, uint64_t* out_ms);
void dtls_backend_conn_handle_timeout(tls_backend_conn_t* c);
```

`set_mtu` wraps `DTLS_set_link_mtu` + `SSL_OP_NO_QUERY_MTU`; `set_peer_addr`
hands the cookie machinery the address to bind against; `get_timeout` /
`handle_timeout` wrap `DTLSv1_get_timeout` / `DTLSv1_handle_timeout`.

### DTLS cookies belong to the backend

The HMAC-over-peer-address cookie scheme is Xylem's own policy, but the
*registration mechanism* is OpenSSL-specific (`SSL_CTX_set_cookie_generate_cb`/
`verify_cb`; mbedTLS uses `mbedtls_ssl_conf_dtls_cookies`). So the whole cookie
plumbing moves into the backend's DTLS `ctx_create`: the backend generates the
secret (OpenSSL: `RAND_bytes`) and registers the gen/verify callbacks. The
callback body keeps computing the cookie with `xylem_hmac256` — a non-OpenSSL
primitive a future backend can reuse verbatim.

## 5. Concurrency contract

The backend does **no locking of its own**. The engine continues to hold
`ssl_mu` across every `tls_backend_conn_*` / `dtls_backend_conn_*` call that
touches the state machine, exactly as it holds it across the OpenSSL calls
today. `ssl_mu` is never held across a socket park; `feed`/`drain` are the only
ops called under it from the pump paths, and they never block (memory-buffer
transfers). This contract is documented in `tls-backend.h` as a precondition
every backend implementation may assume — it lets the backend use a plain,
non-thread-safe state-machine object (an OpenSSL `SSL`, an mbedTLS
`mbedtls_ssl_context`) without internal synchronization.

## 6. Engine-side structures after the cut

```c
/* tls.c */
struct tls_ctx_s {
    tls_backend_ctx_t* be;
    bool verify_server;   /* neutral intent, stays in the engine */
    bool verify_client;
};
struct tls_conn_s {
    tls_backend_conn_t* be;          /* replaces SSL*/rbio/wbio */
    char* rbuf; char* wbuf;          /* pump scratch (unchanged) */
    xylem_mutex_t *ssl_mu, *rd_mu, *wr_mu;
    iowait_t* waiter; platform_sock_t fd; tls_ctx_t* ctx;
    addr_t peer_addr; char alpn[32];
    _Atomic int32_t refcnt; _Atomic bool closed;
};
```

- `_tls_pump_in`/`_tls_pump_out`: `BIO_write`/`BIO_read` → `tls_backend_conn_feed`/
  `tls_backend_conn_drain`. Socket I/O and parking unchanged.
- handshake/read/write loops: `SSL_*` + `SSL_get_error` switch →
  `tls_backend_conn_handshake/read/write` returning `tls_backend_state_t`. The
  `WANT_READ`/`WANT_WRITE`/`OK`/`CLOSED`/`ERROR` arms map 1:1 to the existing
  branches.
- `_tls_apply_verify` + `_tls_apply_server_name` → build a
  `tls_backend_handshake_cfg_t` and call `tls_backend_conn_configure` once,
  after `conn_create`, before the first `handshake`.

DTLS mirrors this; its engine additionally calls `dtls_backend_conn_set_mtu`
(after create), `set_peer_addr` (server, for cookies), and uses
`get_timeout`/`handle_timeout` in the retransmit paths. The session rbtree,
dispatcher coroutine, inbox channel, and retransmit/handshake timers stay in
the engine untouched.

## 7. File layout and migration

```
src/net/tls/
├── tls-backend.h           # neutral interface (tls_backend_* / dtls_backend_*)
├── tls-backend-openssl.c   # the ONLY file including <openssl/...>; also
│                           #   absorbs the former platform-tls system-CA code
├── tls.h / tls.c           # TLS engine; includes only tls-backend.h
├── dtls.h / dtls.c         # moved from src/net/xylem-dtls.c, engine half
├── xylem-tls.c             # public TLS shim (unchanged)
├── xylem-dtls.c            # public DTLS shim, thinned to a first-member wrapper
└── xylem-tls-stub.c / ...  # stubs (unchanged)
```

Changes:

- `src/net/xylem-dtls.c` (engine + public API in one ~1800-line file) is split
  to mirror TLS: `dtls.c` (engine) + `xylem-dtls.c` (thin opaque-handle shim),
  and relocated under `src/net/tls/`.
- `src/platform/{win,unix}/platform-tls.c` and `src/platform/platform-tls.h`
  are **removed**. The Windows/Unix system-CA split was OpenSSL-specific and is
  absorbed into the OpenSSL backend.
- The duplicated PEM parsing, SNI table, and ALPN wire encoding in the TLS and
  DTLS engines converge into the single backend implementation (`load_cert_*`,
  `set_alpn`).
- CMake: under `XYLEM_ENABLE_TLS`, add `tls-backend-openssl.c`, add `dtls.c`,
  drop `platform/*/platform-tls.c`. Reserve a `XYLEM_TLS_BACKEND` cache variable
  (currently only accepts `openssl`) to select the backend source at configure
  time.

## 8. Verification

This is a behavior-preserving refactor, so correctness is judged against the
existing suites with no assertion changes:

- `test-tls` and `test-dtls` pass unchanged.
- Run under ASan/UBSan and TSan (the duplex read/write-while-close races are
  exactly what the lock split and refcounting protect — see [`tls.md`](tls.md)
  §3, §9).
- Public headers and the first-member-equivalence shim trick are untouched, so
  no API/ABI drift.

## 9. Adding a backend later

- **wolfSSL:** built with `--enable-opensslextra`, its OpenSSL-compat layer
  satisfies most of the surface; a `tls-backend-wolfssl.c` is largely the
  OpenSSL file with header/init differences.
- **mbedTLS:** native API; `tls-backend-mbedtls.c` reimplements each
  `tls_backend_*`/`dtls_backend_*` op against `mbedtls_ssl_*`, reusing the
  engine's socket pumping, locking, and DTLS session machinery as-is, plus
  `xylem_hmac256` for cookies. System-CA loading uses an mbedTLS-native
  strategy (there is no one-call system trust store), which is precisely why
  that concern is a backend responsibility rather than a leaked `SSL_CTX*`
  platform shim.

## 10. Related docs

- TLS engine design (object model, BIO decoupling, locks, verify, SNI, ALPN):
  [`tls.md`](tls.md).
- System-CA platform shim being removed, and the platform-code rule:
  [`platform.md`](platform.md).
- Feature gate `XYLEM_ENABLE_TLS`: [`../architecture.md`](../architecture.md) §8,
  [`../build.md`](../build.md).

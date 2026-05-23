# TLS Module Design

## Overview

`xylem-tls` provides TLS encrypted transport using a coroutine blocking-style API, mirroring `xylem-tcp` exactly. OpenSSL operates directly on the non-blocking socket fd via `SSL_set_fd` (Socket BIO). When SSL operations return `WANT_READ`/`WANT_WRITE`, the calling coroutine parks via `iowait_read()`/`iowait_write()` until the fd is ready.

## Architecture

```
User code (plaintext)
    |
    v
xylem-tls (SSL_read / SSL_write via Socket BIO)
    |
    v
Non-blocking socket fd + iowait (coroutine park/resume)
    |
    v
Network
```

TLS holds `fd + iowait` directly -- it is a peer of TCP, not layered on top of it.

## Public Types

```c
typedef struct xylem_tls_conn_s     xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s      xylem_tls_ctx_t;
typedef struct xylem_tls_listener_s xylem_tls_listener_t;

typedef struct xylem_tls_opts_s {
    size_t      max_read_buf;       /* Plaintext read buffer size, 0 = default 64KB. */
    bool        disable_mss_clamp;  /* Disable MSS clamping on the socket. */
    uint64_t    handshake_timeout_ms; /* TLS handshake timeout (dial covers TCP+TLS, accept covers TLS only), 0 = none. */
    const char* server_name;        /* Expected peer name (DNS hostname or IP literal); drives SNI and certificate identity verification. */
} xylem_tls_opts_t;
```

## Internal Structures

### TLS Connection

```c
struct xylem_tls_conn_s {
    SSL*                   ssl;
    iowait_t*              waiter;
    platform_sock_t        fd;
    xylem_tls_ctx_t*       ctx;
    addr_t                 peer_addr;
    xylem_tcp_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    char                   alpn[256];
    xylem_err_t            err;
    _Atomic bool           closed;
};
```

### TLS Listener

```c
struct xylem_tls_listener_s {
    iowait_t*        waiter;
    platform_sock_t  fd;
    xylem_tls_ctx_t* ctx;
    xylem_tls_opts_t opts;
    _Atomic bool     closing;
};
```

### TLS Context (unchanged)

```c
struct xylem_tls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
};
```

## Socket BIO Model

`SSL_set_fd(ssl, fd)` lets OpenSSL operate on the non-blocking socket directly. `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` is set because `SSL_write` may return `WANT_WRITE` after a partial write, and we retry with an offset pointer.

All SSL operations share the same pattern:

```
loop:
  ret = SSL_op(ssl, ...)
  err = SSL_get_error(ssl, ret)
  if WANT_READ:  iowait_read(waiter)
  if WANT_WRITE: iowait_write(waiter)
  if error:      return failure
  retry
```

## Dial Flow

1. DNS resolution (reuses addr_resolve)
2. `platform_socket_dial()` creates non-blocking socket
3. `iowait_write()` waits for connect completion (write deadline = timeout)
4. Check `SO_ERROR` for connect result
5. `SSL_new()` + `SSL_set_fd()` + `SSL_set_connect_state()`
6. Set SNI: `SSL_set_tlsext_host_name` + `SSL_set1_host`
7. `_tls_do_handshake()` drives handshake synchronously
8. Cache ALPN result
9. Clear deadlines, return connection

`handshake_timeout_ms` covers the entire dial process (TCP connect + TLS handshake). On the accept side it bounds the TLS handshake on an already-accepted TCP connection.

## Accept Flow

1. `iowait_read()` waits for incoming connection
2. `platform_socket_accept()` accepts client
3. Create iowait for client fd
4. `SSL_new()` + `SSL_set_fd()` + `SSL_set_accept_state()`
5. `_tls_do_handshake()` drives handshake synchronously
6. Cache ALPN result, return connection

EAGAIN parks via iowait. EMFILE/ENFILE triggers exponential backoff via `runtime_sleep`.

## Recv / Send

`_tls_raw_recv` and `_tls_raw_send` mirror TCP's `_tcp_raw_recv` and `_tcp_raw_send`, replacing `platform_socket_recv/send` with `SSL_read/SSL_write`. Both handle `WANT_READ` and `WANT_WRITE` by parking the coroutine.

## Framing

TLS supports the same framing modes as TCP (NONE, FIXED, LENGTH, DELIMITER), operating on the decrypted plaintext stream. The buffered read and frame parsing logic is identical to TCP.

## Deadlines

`xylem_tls_set_read_deadline` and `xylem_tls_set_write_deadline` pass through directly to `iowait_set_rd_deadline` / `iowait_set_wr_deadline`.

## Close

1. `atomic_exchange(&closed, true)` for idempotency
2. `SSL_shutdown()` sends close_notify (best-effort, no retry on WANT_*)
3. `iowait_close()` wakes parked coroutines
4. `SSL_free()`, `iowait_destroy()`, `platform_socket_close()`, free buffers

## Thread Safety

Same as TCP:
- One reader + one writer coroutine per connection (iowait one-reader/one-writer model)
- `xylem_tls_close` can be called from any thread (iowait_close is thread-safe)
- Deadline setters driven by the owning coroutine

## Context Management

`xylem_tls_ctx_t` wraps `SSL_CTX` and is reusable across connections. Features:
- Default: peer verification enabled, TLS 1.2 minimum
- ALPN: wire-format encoding, both client proposal and server selection callback
- Keylog: NSS Key Log format for Wireshark via `SSL_CTX_set_keylog_callback`
- Ex-data index for recovering ctx pointer in keylog callback

## SNI and ALPN

- **SNI:** Set via `opts->server_name` in `xylem_tls_dial`. Calls `SSL_set_tlsext_host_name` (SNI extension, only for DNS names) and `SSL_set1_host` (peer identity verification, accepts both DNS and IP). IP literals skip the SNI call per RFC 6066.
- **ALPN:** Configured on ctx via `xylem_tls_ctx_set_alpn`. Client proposes, server selects via `SSL_select_next_proto`. Result cached in `tls->alpn[256]` after handshake, queryable via `xylem_tls_get_alpn`.

## Error Codes

| Error | Source |
|-------|--------|
| `XYLEM_ERR_TIMEOUT` | iowait deadline exceeded |
| `XYLEM_ERR_CLOSED` | Local close / iowait_close |
| `XYLEM_ERR_PEER_CLOSED` | `SSL_ERROR_ZERO_RETURN` (peer sent close_notify) |
| `XYLEM_ERR_TLS` | SSL layer error (handshake failure, SSL_read/SSL_write fatal) |

## Public API

### Context

```c
xylem_tls_ctx_t* xylem_tls_ctx_create(void);
void             xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);
int  xylem_tls_ctx_load_cert(ctx, cert, key);
int  xylem_tls_ctx_set_ca(ctx, ca_file);
void xylem_tls_ctx_set_verify(ctx, enable);
int  xylem_tls_ctx_set_alpn(ctx, protocols, count);
int  xylem_tls_ctx_set_keylog(ctx, path);
```

### Connection Lifecycle

```c
xylem_tls_conn_t*     xylem_tls_dial(host, port, ctx, opts);
xylem_tls_listener_t* xylem_tls_listen(host, port, ctx, opts);
xylem_tls_conn_t*     xylem_tls_accept(listener);
void                  xylem_tls_close(tls);
void                  xylem_tls_close_listener(listener);
```

### I/O

```c
int64_t xylem_tls_recv(tls, buf, len);
int     xylem_tls_send(tls, data, len);
void    xylem_tls_set_framing(tls, opts);
void    xylem_tls_set_read_deadline(tls, deadline_ms);
void    xylem_tls_set_write_deadline(tls, deadline_ms);
```

### Info

```c
xylem_err_t  xylem_tls_get_error(tls);
int          xylem_tls_remote_addr(tls, host, host_len, port);
int          xylem_tls_local_addr(tls, host, host_len, port);
int          xylem_tls_listener_addr(ln, host, host_len, port);
const char*  xylem_tls_get_alpn(tls);
```

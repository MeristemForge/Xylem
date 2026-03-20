# TLS Module Design

## Architecture

```
User code
  ↕  xylem_tls_* API (plaintext)
xylem-tls.c (OpenSSL SSL_read / SSL_write + memory BIO)
  ↕  xylem_tcp_* internal handler (ciphertext)
xylem-tcp.c (network I/O)
```

TLS wraps TCP. Each `xylem_tls_t` owns a `xylem_tcp_conn_t` internally. OpenSSL never touches the socket fd — all data flows through memory BIOs.

## Types

### Opaque types (struct body in `src/xylem-tls.c`)

| Type | Description |
|------|-------------|
| `xylem_tls_ctx_t` | Wraps `SSL_CTX*`. Reusable across connections. |
| `xylem_tls_t` | Single TLS connection. Wraps `SSL*` + memory BIOs + `xylem_tcp_conn_t*`. |
| `xylem_tls_server_t` | TLS server. Wraps `xylem_tcp_server_t*` + `xylem_tls_ctx_t*`. |

### Public types (in header)

| Type | Description |
|------|-------------|
| `xylem_tls_handler_t` | Callback struct: `on_accept`, `on_connect`, `on_read`, `on_write_done`, `on_close`, `on_timeout`, `on_heartbeat_miss` |

## Internal Struct Layout

```c
struct xylem_tls_ctx_s {
    SSL_CTX*    ssl_ctx;
};

struct xylem_tls_s {
    SSL*                    ssl;
    BIO*                    read_bio;   /* ciphertext in (from network) */
    BIO*                    write_bio;  /* ciphertext out (to network) */
    xylem_tcp_conn_t*       tcp;        /* underlying TCP connection */
    xylem_tls_ctx_t*        ctx;
    xylem_tls_handler_t*    handler;
    xylem_tls_server_t*     server;     /* non-NULL for accepted conns */
    void*                   userdata;
    bool                    handshake_done;
    bool                    closing;
    char*                   hostname;   /* SNI, client only */
};

struct xylem_tls_server_s {
    xylem_tcp_server_t*     tcp_server;
    xylem_tls_ctx_t*        ctx;
    xylem_tls_handler_t*    handler;
    xylem_tcp_opts_t        opts;
    xylem_loop_t*           loop;
    bool                    closing;
};
```

## Data Flow

### Handshake (client dial)

1. `xylem_tls_dial` → `xylem_tcp_dial` with internal TCP handler
2. TCP `on_connect` fires → create `SSL*`, set memory BIOs, call `SSL_connect`
3. `SSL_connect` returns `SSL_ERROR_WANT_READ` → flush write BIO via `xylem_tcp_send`
4. TCP `on_read` (ciphertext from server) → `BIO_write(read_bio, ...)` → `SSL_connect` again
5. Repeat 3-4 until `SSL_connect` returns 1 (success)
6. Set `handshake_done = true` → call user's `on_connect`

### Handshake (server accept)

1. TCP `on_accept` fires → create `xylem_tls_t`, create `SSL*` with `SSL_set_accept_state`
2. TCP `on_read` (ClientHello) → `BIO_write(read_bio, ...)` → `SSL_accept`
3. `SSL_accept` returns `SSL_ERROR_WANT_WRITE` → flush write BIO via `xylem_tcp_send`
4. Repeat until `SSL_accept` returns 1
5. Set `handshake_done = true` → call user's `on_accept`

### Read path (after handshake)

```
TCP on_read(ciphertext)
  → BIO_write(read_bio, ciphertext, len)
  → loop: SSL_read(ssl, plaintext_buf, sizeof(buf))
    → if > 0: call user on_read(tls, plaintext, n)
    → if SSL_ERROR_WANT_READ: break (need more ciphertext)
    → if SSL_ERROR_ZERO_RETURN: peer initiated shutdown
    → if error: close with error
```

### Write path

```
xylem_tls_send(tls, plaintext, len)
  → SSL_write(ssl, plaintext, len)
  → read from write_bio → xylem_tcp_send(tcp, ciphertext, n)
  → TCP on_write_done → call user on_write_done(tls, plaintext, len, 0)
```

### Close / Shutdown

```
xylem_tls_close(tls)
  → SSL_shutdown(ssl) (sends close_notify)
  → flush write BIO → xylem_tcp_send
  → xylem_tcp_close(tcp)
  → TCP on_close → SSL_free, call user on_close
```

## TCP Handler Bridge

TLS registers its own `xylem_tcp_handler_t` on the underlying TCP connection:

```c
static xylem_tcp_handler_t _tls_tcp_handler = {
    .on_accept        = _tls_tcp_accept_cb,
    .on_connect       = _tls_tcp_connect_cb,
    .on_read          = _tls_tcp_read_cb,
    .on_write_done    = _tls_tcp_write_done_cb,
    .on_close         = _tls_tcp_close_cb,
    .on_timeout       = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};
```

Each callback unwraps the `xylem_tls_t*` from `xylem_tcp_get_userdata(conn)` and drives the TLS state machine.

## Framing

Framing (`xylem_tcp_opts_t.framing`) is set to `XYLEM_TCP_FRAME_NONE` on the underlying TCP connection. TLS handles record boundaries internally. User-level framing is applied after decryption in the TLS read path — `_tcp_extract_frame` equivalent logic on plaintext.

**Decision**: TLS module does NOT reuse TCP's framing. Instead, TLS decrypts into its own plaintext ringbuf, then applies framing on the plaintext. This keeps the layers cleanly separated.

## ALPN

- Client: `SSL_CTX_set_alpn_protos` during ctx setup
- Server: `SSL_CTX_set_alpn_select_cb` with callback that matches against configured list
- After handshake: `SSL_get0_alpn_selected` to query result

## SNI

- Client: `SSL_set_tlsext_host_name(ssl, hostname)` before handshake
- Also used for certificate hostname verification: `SSL_set1_host(ssl, hostname)`

## Error Handling

- Handshake failure → `on_close(tls, err)` with OpenSSL error code
- `SSL_read` / `SSL_write` errors → close connection with error
- Certificate verification failure → handshake fails, `on_close` with error

## Files

| File | Content |
|------|---------|
| `include/xylem/xylem-tls.h` | Public API, opaque type forward declarations, handler struct |
| `src/xylem-tls.c` | Full implementation, struct bodies, OpenSSL integration |
| `tests/test-tls.c` | Unit tests |
| `CMakeLists.txt` | `XYLEM_ENABLE_TLS` option, `find_package(OpenSSL)`, source append |
| `tests/CMakeLists.txt` | TLS test with OpenSSL link |

## Build Integration

```cmake
option(XYLEM_ENABLE_TLS "Enable TLS/DTLS support (requires OpenSSL)" OFF)

if(XYLEM_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
    list(APPEND SRCS src/xylem-tls.c src/xylem-dtls.c)
    include_directories(${OPENSSL_INCLUDE_DIR})
endif()
```

Tests:
```cmake
if(XYLEM_ENABLE_TLS)
    xylem_add_test(tls)
    target_link_libraries(test-tls OpenSSL::SSL OpenSSL::Crypto)
    xylem_add_test(dtls)
    target_link_libraries(test-dtls OpenSSL::SSL OpenSSL::Crypto)
endif()
```

## DTLS Architecture

```
User code
  ↕  xylem_dtls_* API (plaintext datagrams)
xylem-dtls.c (OpenSSL SSL_read / SSL_write + memory BIO + DTLS_method)
  ↕  xylem_udp_* internal (ciphertext datagrams)
xylem-udp.c (network I/O)
```

### DTLS Types

| Type | Description |
|------|-------------|
| `xylem_dtls_ctx_t` | Wraps `SSL_CTX*` with `DTLS_method()`. Includes cookie callbacks. |
| `xylem_dtls_t` | Single DTLS session. Wraps `SSL*` + memory BIOs + connected UDP socket. |
| `xylem_dtls_server_t` | DTLS server. Single UDP socket, demuxes by peer address. |

### DTLS Internal Struct Layout

```c
struct xylem_dtls_ctx_s {
    SSL_CTX*    ssl_ctx;
};

struct xylem_dtls_s {
    SSL*                     ssl;
    BIO*                     read_bio;
    BIO*                     write_bio;
    xylem_udp_t*             udp;          /* underlying UDP handle */
    xylem_dtls_ctx_t*        ctx;
    xylem_dtls_handler_t*    handler;
    xylem_dtls_server_t*     server;       /* non-NULL for server sessions */
    xylem_addr_t             peer_addr;    /* remote peer address */
    void*                    userdata;
    bool                     handshake_done;
    bool                     closing;
    xylem_loop_timer_t       retransmit_timer; /* DTLS retransmission */
};

struct xylem_dtls_server_s {
    xylem_udp_t*             udp;          /* shared listening socket */
    xylem_dtls_ctx_t*        ctx;
    xylem_dtls_handler_t*    handler;
    xylem_loop_t*            loop;
    xylem_list_t             sessions;     /* list of xylem_dtls_t */
    bool                     closing;
};
```

### DTLS Handshake (client)

1. `xylem_dtls_dial` → create connected UDP socket via `connect()`, wrap in `xylem_udp_t`
2. Create `SSL*` with `DTLS_method()`, set memory BIOs, call `SSL_connect`
3. `SSL_ERROR_WANT_READ` → flush write BIO via UDP send
4. UDP `on_read` → feed read BIO → `SSL_connect` again
5. Repeat until handshake completes → call user `on_connect`

### DTLS Handshake (server)

1. `xylem_dtls_listen` → bind UDP socket, register `on_read`
2. Incoming packet → lookup session by peer address
3. New peer → `DTLSv1_listen` for cookie exchange → on success create `SSL*` → `SSL_accept`
4. Existing peer → feed read BIO → continue handshake or deliver data
5. Handshake complete → call user `on_accept`

### DTLS Retransmission Timer

- After each handshake step, call `DTLSv1_get_timeout` to get OpenSSL's retransmit deadline
- Start/reset `retransmit_timer` with that value
- On timer fire: call `DTLSv1_handle_timeout`, flush write BIO
- Stop timer after handshake completes

### DTLS vs TLS Differences

| Aspect | TLS | DTLS |
|--------|-----|------|
| Transport | TCP (`xylem_tcp_conn_t`) | UDP (`xylem_udp_t`) |
| Method | `TLS_method()` | `DTLS_method()` |
| Message boundaries | Stream (framing needed) | Datagram (preserved) |
| Retransmission | TCP handles it | OpenSSL timer + `DTLSv1_handle_timeout` |
| Server demux | One conn per accept | Single socket, demux by peer addr |
| Cookie | N/A | `SSL_CTX_set_cookie_*_cb` |

### DTLS Files

| File | Content |
|------|---------|
| `include/xylem/xylem-dtls.h` | Public API, opaque types, handler struct |
| `src/xylem-dtls.c` | Implementation |
| `tests/test-dtls.c` | Unit tests |

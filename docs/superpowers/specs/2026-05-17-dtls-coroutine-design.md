# DTLS Coroutine Refactor Design Spec

## Overview

Port the DTLS module from the old callback/event-loop model to the coroutine
blocking-style API used by TCP, UDP, TLS, and UDS. The public API becomes
synchronous: `dial` blocks until handshake completes, `recv` blocks until a
datagram arrives, `send` blocks until data is sent.

### Key Architectural Decision

DTLS uses a **dual-path architecture**:

- **Client (dial)**: Socket BIO + iowait, identical to TLS. Each client gets a
  connected UDP socket with its own fd.
- **Server (listen/accept)**: Memory BIO + dispatcher coroutine + per-session
  ring buffer. A single UDP socket serves all sessions; a dispatcher coroutine
  loops on `recvfrom` and routes datagrams by peer address.

Socket BIO cannot be used server-side because multiple SSL objects cannot share
a single fd -- OpenSSL would call `recv(fd)` without knowing which session
should consume the datagram.

## Public API

### Context (unchanged)

```c
xylem_dtls_ctx_t* xylem_dtls_ctx_create(void);
void              xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx);
int               xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                                           const char* cert, const char* key);
int               xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx,
                                        const char* ca_file);
void              xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable);
int               xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                                          const char** protocols, size_t count);
int               xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx,
                                            const char* path);
```

### Connection Options

```c
typedef struct xylem_dtls_opts_s {
    uint64_t    connect_timeout_ms; /* UDP connect + DTLS handshake, 0 = 30s default
                                     * (unlike TLS where 0 = no timeout, DTLS over
                                     *  unreliable transport needs a default) */
    const char* hostname;           /* SNI hostname (reserved for DTLS 1.3) */
} xylem_dtls_opts_t;
```

### Client

```c
xylem_dtls_conn_t* xylem_dtls_dial(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts);
```

Suspends the calling coroutine until the connected UDP socket is ready and
the DTLS handshake completes (or `connect_timeout_ms` elapses). Returns
the connection handle or NULL on failure.

### Server

```c
xylem_dtls_listener_t* xylem_dtls_listen(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts);

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln);
```

`listen` creates the bound UDP socket and spawns the internal dispatcher
coroutine. `accept` suspends the calling coroutine until a client completes
the DTLS handshake.

### Data Transfer

```c
int64_t xylem_dtls_recv(
    xylem_dtls_conn_t* dtls, void* buf, size_t len);

int xylem_dtls_send(
    xylem_dtls_conn_t* dtls, const void* data, size_t len);
```

`recv` returns one complete decrypted datagram (datagram semantics, no
framing). `send` sends one complete datagram. Both suspend the calling
coroutine as needed.

### Deadlines

```c
void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms);
void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms);
```

Absolute monotonic timestamps in ms. Pass 0 to clear. When a deadline
expires, in-flight and subsequent recv/send calls fail with
`XYLEM_ERR_TIMEOUT`.

### Close

```c
void xylem_dtls_close(xylem_dtls_conn_t* dtls);
void xylem_dtls_close_listener(xylem_dtls_listener_t* ln);
```

Both are idempotent. `close` wakes any coroutine parked in recv/send.
`close_listener` closes all sessions, wakes the dispatcher, and wakes any
coroutine parked in `accept`.

### Accessors

```c
xylem_err_t xylem_dtls_get_error(xylem_dtls_conn_t* dtls);
const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls);
int xylem_dtls_remote_addr(xylem_dtls_conn_t* dtls,
                           char* host, size_t host_len, uint16_t* port);
int xylem_dtls_local_addr(xylem_dtls_conn_t* dtls,
                          char* host, size_t host_len, uint16_t* port);
int xylem_dtls_listener_addr(xylem_dtls_listener_t* ln,
                             char* host, size_t host_len, uint16_t* port);
```

### Removed API

- `xylem_dtls_handler_t` (entire callback struct)
- `xylem_dtls_conn_ref` / `xylem_dtls_conn_unref`
- `xylem_dtls_get_userdata` / `xylem_dtls_set_userdata`
- `xylem_dtls_server_get_userdata` / `xylem_dtls_server_set_userdata`
- `xylem_dtls_server_t` type (replaced by `xylem_dtls_listener_t`)

## Internal Data Structures

### Client Connection (Socket BIO path)

```c
struct xylem_dtls_conn_s {
    SSL*                    ssl;
    iowait_t*               waiter;
    platform_sock_t          fd;
    xylem_dtls_ctx_t*        ctx;
    addr_t                   peer_addr;
    char                     alpn[256];
    xylem_err_t              err;
    _Atomic bool             closed;

    /* server-side only (NULL/zero for client) */
    dtls_session_inbox_t*    inbox;
    BIO*                     read_bio;
    BIO*                     write_bio;
    sched_timer_t*           retransmit_timer;
    sched_timer_t*           handshake_timer;
    xylem_dtls_listener_t*   listener;
    rbtree_node_t            server_node;
    uint64_t                 rd_deadline_ms;
    uint64_t                 wr_deadline_ms;
};
```

Client fields mirror TLS: `ssl` + `iowait` + `fd`. No read buffer or
framing (datagram semantics).

Server-side sessions add: `inbox` (ring buffer), Memory BIO pair,
sched_timer-based retransmit/handshake timers, `listener` back-pointer, and
rbtree node.

### Datagram Ring Buffer (per-session inbox)

```c
typedef struct dtls_dgram_s {
    size_t len;
    char   data[];  /* flexible array member */
} dtls_dgram_t;

typedef struct dtls_session_inbox_s {
    dtls_dgram_t** slots;     /* power-of-2 ring buffer */
    uint32_t       cap;
    uint32_t       head;      /* consumer index */
    uint32_t       tail;      /* producer index */
    mco_coro*      parked;    /* session coroutine waiting for data */
    scheduler_t*   sched;
    bool           closed;
} dtls_session_inbox_t;
```

Single-producer (dispatcher) / single-consumer (session coroutine or user
coroutine). No lock needed.

- **Push**: `slots[tail++ & (cap-1)] = dgram`. If `parked != NULL`,
  `scheduler_schedule(sched, parked)` to wake.
- **Pop**: if `head == tail`, `scheduler_park()`. Woken by push or close.
  Returns `slots[head++ & (cap-1)]`.
- **Close**: sets `closed = true`, wakes parked coroutine.

### Listener

```c
struct xylem_dtls_listener_s {
    platform_sock_t       fd;
    iowait_t*             waiter;
    xylem_dtls_ctx_t*     ctx;
    xylem_dtls_opts_t     opts;
    rbtree_t              sessions;
    mtx_t                 sessions_mtx;  /* protects rbtree */
    scheduler_t*          sched;

    /* accept queue: completed-handshake sessions */
    xylem_dtls_conn_t**   accept_slots;
    uint32_t              accept_cap;
    uint32_t              accept_head;
    uint32_t              accept_tail;
    mco_coro*             accept_parked;

    _Atomic bool          closing;
};
```

The accept queue is a ring buffer of `xylem_dtls_conn_t*` pointers, using
the same SPSC pattern as the session inbox.

### Thread Safety: Session Rbtree

The sessions rbtree is accessed by:
- Dispatcher coroutine: `rbtree_find` per incoming datagram, `rbtree_insert`
  for new sessions
- Handshake coroutine: `rbtree_remove` on handshake failure/timeout
- User coroutine: `rbtree_remove` via `xylem_dtls_close`
- Listener close: traversal + removal

Under work-stealing, these can run on different worker threads. A C11
`mtx_t` protects all rbtree operations. Critical sections are O(log n)
pointer comparisons/rotations -- negligible overhead.

## Client Path (Socket BIO + iowait)

Symmetric with TLS. Key difference: UDP socket + DTLS_method().

### xylem_dtls_dial

1. Create connected UDP socket: `platform_socket_udp()` + `connect()`.
2. Set non-blocking.
3. `iowait_create(fd)`.
4. `SSL_new(ctx->ssl_ctx)` + `SSL_set_fd(ssl, fd)`.
5. `SSL_set_connect_state()`.
6. Set `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` (required for Socket BIO).
7. Compute deadline from `connect_timeout_ms`, set iowait rd/wr deadlines.
8. Handshake loop (identical to TLS `_tls_do_handshake`):
   ```
   for (;;) {
       ret = SSL_do_handshake(ssl);
       if (ret == 1) break;  /* success */
       err = SSL_get_error(ssl, ret);
       if (WANT_READ) { iowait_read(waiter); continue; }
       if (WANT_WRITE) { iowait_write(waiter); continue; }
       /* error: cleanup, return NULL */
   }
   ```
9. Clear deadlines.
10. Cache ALPN.
11. Return conn.

### xylem_dtls_recv (client)

```
if atomic_load(&closed): return -1 (XYLEM_ERR_CLOSED)
retry:
    ERR_clear_error()
    n = SSL_read(ssl, buf, len)
    if (n > 0) return n
    err = SSL_get_error(ssl, n)
    if ZERO_RETURN: err = XYLEM_ERR_PEER_CLOSED, return 0
    if WANT_READ:
        r = iowait_read(waiter)
        if (r != IOWAIT_READY || closed) return -1
        goto retry
    if WANT_WRITE:
        r = iowait_write(waiter)
        if (r != IOWAIT_READY || closed) return -1
        goto retry
    else: err = XYLEM_ERR_DTLS, return -1
```

### xylem_dtls_send (client)

```
if atomic_load(&closed): return -1
for (;;):
    n = SSL_write(ssl, data, len)
    if (n > 0) return 0   /* datagram-atomic */
    err = SSL_get_error(ssl, n)
    if WANT_WRITE: iowait_write(waiter), continue
    if WANT_READ:  iowait_read(waiter), continue
    else: err = XYLEM_ERR_DTLS, return -1
```

### xylem_dtls_close (client)

```
if atomic_exchange(&closed, true): return  /* idempotent */
SSL_shutdown(ssl)          /* best-effort close_notify */
iowait_close(waiter)      /* wakes parked recv/send */
SSL_free(ssl)
iowait_destroy(waiter)
platform_socket_close(fd)
free(conn)
```

## Server Path (Memory BIO + dispatcher + park/unpark)

### xylem_dtls_listen

1. Create unconnected UDP socket: `platform_socket_udp()` + `bind()`.
2. Set non-blocking.
3. `iowait_create(fd)`.
4. Allocate `xylem_dtls_listener_t`, init rbtree, accept queue.
5. `runtime_spawn(_dtls_dispatcher, listener)`.
6. Return listener.

### Dispatcher Coroutine

Single coroutine, core of server-side multiplexing:

```
_dtls_dispatcher(listener):
    char buf[65535]
    while !atomic_load(&listener->closing):
        n = platform_socket_recvfrom(fd, buf, sizeof(buf), &from_addr)
        if (n < 0):
            if EAGAIN:
                r = iowait_read(listener->waiter)
                if (r != IOWAIT_READY) break
                continue
            continue  /* transient error */

        session = rbtree_find(&listener->sessions, &from_addr)
        if (session):
            dgram = malloc(sizeof(dtls_dgram_t) + n)
            dgram->len = n; memcpy(dgram->data, buf, n)
            inbox_push(session->inbox, dgram)
        else:
            session = _dtls_new_server_session(listener, &from_addr)
            /* feed first datagram */
            dgram = malloc(sizeof(dtls_dgram_t) + n)
            dgram->len = n; memcpy(dgram->data, buf, n)
            inbox_push(session->inbox, dgram)
            rbtree_insert(&listener->sessions, &session->server_node)
            runtime_spawn(_dtls_handshake_coro, session)
```

Uses `platform_socket_recvfrom` directly (not `xylem_udp_recv`) because
we need the raw `sockaddr` for rbtree lookup and the listener owns the fd
directly.

### Handshake Coroutine (per new session, temporary)

```
_dtls_handshake_coro(session):
    SSL_new + Memory BIO pair, SSL_set_accept_state
    SSL_set_ex_data(ssl, peer_addr_idx, &session->peer_addr)
    start handshake_timer (30s, sched_timer one-shot)

    while !handshake_done:
        dgram = inbox_pop(session->inbox)  /* parks if empty */
        if (inbox closed || handshake timed out):
            cleanup: stop timers, rbtree_remove, SSL_free, free
            return

        BIO_write(read_bio, dgram->data, dgram->len); free(dgram)
        ERR_clear_error()
        ret = SSL_do_handshake(ssl)
        if (ret == 1):
            handshake_done, stop timers, cache ALPN
            break
        err = SSL_get_error(ssl, ret)
        if (WANT_READ || WANT_WRITE):
            flush_write_bio(session)  /* BIO_read -> sendto */
            arm_retransmit(session)
            continue
        else:
            flush_write_bio(session)  /* flush alert */
            cleanup, return

    /* push to accept queue, wakes xylem_dtls_accept() */
    accept_queue_push(listener, session)
    /* coroutine exits */
```

### xylem_dtls_accept

```
while !listener->closing:
    session = accept_queue_pop(listener)  /* parks if empty */
    if (session) return session
return NULL  /* listener closing */
```

### xylem_dtls_recv (server session, called by user coroutine)

```
if atomic_load(&closed): return -1 (XYLEM_ERR_CLOSED)
retry:
    dgram = inbox_pop_with_deadline(session->inbox, rd_deadline_ms)
    if (closed): return -1 (XYLEM_ERR_CLOSED)
    if (timed out): return -1 (XYLEM_ERR_TIMEOUT)

    BIO_write(read_bio, dgram->data, dgram->len); free(dgram)
    ERR_clear_error()
    n = SSL_read(ssl, buf, len)
    if (n > 0) return n
    err = SSL_get_error(ssl, n)
    if ZERO_RETURN: err = XYLEM_ERR_PEER_CLOSED, return 0
    if WANT_READ: goto retry  /* need more ciphertext */
    else: err = XYLEM_ERR_DTLS, return -1
```

### xylem_dtls_send (server session)

```
if atomic_load(&closed): return -1
ERR_clear_error()
n = SSL_write(ssl, data, len)  /* Memory BIO: always completes */
if (n <= 0): return -1 (XYLEM_ERR_DTLS)

/* flush write_bio -> sendto(listener->fd, peer_addr) */
char flush_buf[TLS_RECORD_MAX_PLAINTEXT]
while BIO_read(write_bio, flush_buf, sizeof(flush_buf)) > 0:
    sendto(listener->fd, flush_buf, n, 0, &session->peer_addr, ...)
    /* non-blocking sendto; EAGAIN on shared socket is rare for datagrams */
return 0
```

### Retransmit Timer

Uses `sched_timer` (not iowait deadline, since server sessions have no fd).

```
arm_retransmit(session):
    DTLSv1_get_timeout(ssl, &tv) -> ms
    sched_timer_start(retransmit_timer, _retransmit_cb, session, ms, 0)

_retransmit_cb(timer, ud):
    session = ud
    if (session->closed) return
    DTLSv1_handle_timeout(ssl)
    flush_write_bio(session)  /* non-blocking sendto */
    arm_retransmit(session)
```

Timer callbacks run outside coroutine context. `flush_write_bio` uses
non-blocking `sendto`; if EAGAIN, the packet is dropped (next retransmit
will retry -- retransmission is inherently loss-tolerant).

### Handshake Timeout

30-second `sched_timer` one-shot, started when a new server session is
created. If it fires before handshake completes, the callback closes the
inbox (waking the handshake coroutine) and sets `XYLEM_ERR_TIMEOUT`.
Stopped on handshake success or session close.

### Read Deadline for Server Sessions

Server-side `xylem_dtls_set_read_deadline` stores `rd_deadline_ms` in the
connection struct. `inbox_pop_with_deadline` implements the timeout:

1. Start a one-shot `sched_timer` with remaining ms to deadline.
2. `scheduler_park()` the coroutine.
3. On wakeup, check: was it data (inbox non-empty) or timer (deadline
   reached)?
4. Cancel whichever didn't fire.

This is semantically equivalent to `iowait_set_rd_deadline` for client-side
connections, but uses park + sched_timer instead of iowait + poller.

## Close Flows

### Client Close

```
atomic_exchange(&closed, true) → idempotent guard
SSL_shutdown + best-effort (ignore errors)
iowait_close(waiter) → wakes parked recv/send
SSL_free
iowait_destroy
platform_socket_close(fd)
free(conn)
```

### Server Session Close

```
atomic_exchange(&closed, true) → idempotent guard
sched_timer_stop(retransmit_timer)
sched_timer_stop(handshake_timer)
if handshake_done: SSL_shutdown, flush_write_bio
inbox_close(inbox) → wakes parked user recv coroutine
rbtree_remove from listener->sessions
SSL_free
sched_timer_destroy(retransmit_timer)
sched_timer_destroy(handshake_timer)
inbox_destroy(inbox)
free(conn)
```

### Listener Close

```
atomic_exchange(&closing, true) → idempotent guard
traverse rbtree, close each session
iowait_close(waiter) → wakes dispatcher coroutine
accept_queue_close → wakes xylem_dtls_accept()
/* after dispatcher exits: */
iowait_destroy(waiter)
platform_socket_close(fd)
free(listener)
```

## Cookie Mechanism

Retained unchanged from the old implementation. HMAC-SHA256 over peer
address using CSPRNG-generated 32-byte secret stored in `xylem_dtls_ctx_t`.
Cookie generate/verify callbacks access the peer address via
`SSL_get_ex_data`.

## Session Lookup

Retained: rbtree keyed by peer `sockaddr`, using `_dtls_addr_cmp` (compare
family, then port, then address bytes). O(log n) lookup per incoming
datagram. Comparators: node-node for insert, key-node for find.

## Error Handling

### New Error Code

Add `XYLEM_ERR_DTLS = 13` to `xylem_err_t`.

### Error Mapping

| Scenario | Error Code |
|----------|------------|
| Handshake timeout (30s) | `XYLEM_ERR_TIMEOUT` |
| `connect_timeout_ms` elapsed | `XYLEM_ERR_TIMEOUT` |
| recv/send deadline expired | `XYLEM_ERR_TIMEOUT` |
| `SSL_read` ZERO_RETURN | `XYLEM_ERR_PEER_CLOSED` |
| recv/send on closed connection | `XYLEM_ERR_CLOSED` |
| SSL handshake failure | `XYLEM_ERR_DTLS` |
| `SSL_read`/`SSL_write` error | `XYLEM_ERR_DTLS` |
| `iowait_close` wakeup (client) | `XYLEM_ERR_CLOSED` |
| inbox closed (server) | `XYLEM_ERR_CLOSED` |

## Testing Strategy

| # | Test | Description |
|---|------|-------------|
| 1 | Loopback handshake | listener + accept + dial, verify handshake success |
| 2 | Echo round-trip | client send -> server recv -> server send -> client recv |
| 3 | Concurrent sessions | Multiple clients to the same listener |
| 4 | Handshake timeout | Client dials but doesn't complete handshake, verify 30s timeout |
| 5 | Recv deadline | Set read deadline, verify timeout returns XYLEM_ERR_TIMEOUT |
| 6 | Close idempotency | Multiple close calls don't crash |
| 7 | Close wakes recv | One coroutine in recv, another closes, verify recv returns |
| 8 | Listener close | Close listener, verify all sessions and dispatcher clean up |
| 9 | Large datagram | Send near-MTU datagrams |
| 10 | ALPN negotiation | Verify ALPN protocol is correctly negotiated and cached |

## Files Changed

| File | Change |
|------|--------|
| `include/xylem/net/xylem-dtls.h` | Rewrite: new coroutine API |
| `src/net/xylem-dtls.c` | Rewrite: dual-path implementation |
| `include/xylem/xylem-error.h` | Add `XYLEM_ERR_DTLS = 13` |
| `CMakeLists.txt` | Uncomment `src/net/xylem-dtls.c` |
| `tests/test-dtls.c` | Rewrite: coroutine-based tests |
| `examples/dtls-echo-client.c` | Update to coroutine API |
| `examples/dtls-echo-server.c` | Update to coroutine API |
| `docs/dtls-design.md` | Update to reflect coroutine architecture |
| `docs/dtls-test-design.md` | Update test plan |

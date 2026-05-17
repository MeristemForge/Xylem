# DTLS Coroutine Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the DTLS module from callback/event-loop to coroutine blocking-style API, matching TCP/UDP/TLS patterns.

**Architecture:** Dual-path — client uses Socket BIO + iowait (identical to TLS), server uses Memory BIO + dispatcher coroutine + per-session datagram ring buffer + park/unpark. Session rbtree protected by mutex for work-stealing safety.

**Tech Stack:** C11, OpenSSL (DTLS_method), platform sockets, iowait, scheduler park/schedule/timer

---

## File Map

| File | Responsibility |
|------|----------------|
| `include/xylem/xylem-error.h` | Add `XYLEM_ERR_DTLS = 13` |
| `src/xylem-error.c` | Add tostring case for DTLS |
| `include/xylem/net/xylem-dtls.h` | New coroutine public API |
| `src/net/xylem-dtls.c` | Full implementation (ctx, client, server, inbox, dispatcher) |
| `CMakeLists.txt` | Uncomment dtls source |
| `tests/test-dtls.c` | Coroutine-based tests |
| `docs/dtls-design.md` | Updated design doc |

---

### Task 1: Add XYLEM_ERR_DTLS error code

**Files:**
- Modify: `include/xylem/xylem-error.h:44`
- Modify: `src/xylem-error.c:38-39`

- [ ] **Step 1: Add the enum value**

In `include/xylem/xylem-error.h`, after the `XYLEM_ERR_TLS` line:

```c
    XYLEM_ERR_TLS          = 12, /*< TLS/SSL layer error. */
    XYLEM_ERR_DTLS         = 13, /*< DTLS layer error. */
} xylem_err_t;
```

- [ ] **Step 2: Add the tostring case**

In `src/xylem-error.c`, add before the closing `}` of the switch:

```c
    case XYLEM_ERR_TLS:          return "tls error";
    case XYLEM_ERR_DTLS:         return "dtls error";
    }
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build build --target xylem`
Expected: PASS, no errors.

- [ ] **Step 4: Commit**

```bash
git add include/xylem/xylem-error.h src/xylem-error.c
git commit -m "feat(error): add XYLEM_ERR_DTLS error code"
```

---

### Task 2: Rewrite public header `xylem-dtls.h`

**Files:**
- Rewrite: `include/xylem/net/xylem-dtls.h`

- [ ] **Step 1: Write the new header**

Replace the entire contents of `include/xylem/net/xylem-dtls.h`:

```c
_Pragma("once")

#include "xylem/xylem-error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xylem_dtls_conn_s     xylem_dtls_conn_t;
typedef struct xylem_dtls_ctx_s      xylem_dtls_ctx_t;
typedef struct xylem_dtls_listener_s xylem_dtls_listener_t;

typedef struct xylem_dtls_opts_s {
    uint64_t    connect_timeout_ms;
    const char* hostname;
} xylem_dtls_opts_t;

extern xylem_dtls_ctx_t* xylem_dtls_ctx_create(void);
extern void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx);
extern int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                                    const char* cert, const char* key);
extern int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx,
                                 const char* ca_file);
extern void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable);
extern int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                                   const char** protocols, size_t count);
extern int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx,
                                     const char* path);

extern xylem_dtls_conn_t* xylem_dtls_dial(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts);

extern xylem_dtls_listener_t* xylem_dtls_listen(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts);

extern xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln);

extern int64_t xylem_dtls_recv(
    xylem_dtls_conn_t* dtls, void* buf, size_t len);

extern int xylem_dtls_send(
    xylem_dtls_conn_t* dtls, const void* data, size_t len);

extern void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms);
extern void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms);

extern void xylem_dtls_close(xylem_dtls_conn_t* dtls);
extern void xylem_dtls_close_listener(xylem_dtls_listener_t* ln);

extern xylem_err_t xylem_dtls_get_error(xylem_dtls_conn_t* dtls);
extern const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls);

extern int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port);
extern int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port);
extern int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char* host, size_t host_len, uint16_t* port);
```

- [ ] **Step 2: Commit**

```bash
git add include/xylem/net/xylem-dtls.h
git commit -m "feat(dtls): rewrite public header for coroutine API"
```

---

### Task 3: Implement context management + internal types

**Files:**
- Rewrite: `src/net/xylem-dtls.c` (first half — types, ctx, inbox, helpers)

This task creates the foundation: all struct definitions, context API, inbox ring buffer, cookie/ALPN callbacks, and shared helpers. The file will not compile until Task 4 adds the connection/listener functions, but the code is correct in isolation.

- [ ] **Step 1: Write the foundation code**

Replace `src/net/xylem-dtls.c` entirely with the code below. This is the first section — context management, internal types, inbox ring buffer, cookie callbacks, ALPN callback, address comparison, rbtree comparators, and shared flush helpers.

Reference patterns:
- Context API: copy from old `xylem-dtls.c` lines 199-333 (unchanged except `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` added for Socket BIO client path)
- Cookie callbacks: copy from old `xylem-dtls.c` lines 59-181 (unchanged)
- ALPN callback: copy from old `xylem-dtls.c` lines 183-197 (unchanged)
- Address comparison + rbtree comparators: copy from old `xylem-dtls.c` lines 493-542 (unchanged)

New additions:
- `dtls_dgram_t` and `dtls_session_inbox_t` structs + create/destroy/push/pop/pop_with_deadline/close functions
- `xylem_dtls_conn_s` struct with dual-path fields
- `xylem_dtls_listener_s` struct with mutex-protected rbtree + accept queue
- `_dtls_server_flush_write_bio()` helper using `platform_socket_sendto`
- `#define DTLS_DEFAULT_TIMEOUT_MS 30000`
- `#define DTLS_INBOX_CAP 64` (power-of-2 ring buffer default)

Key struct definitions:

```c
typedef struct dtls_dgram_s {
    size_t len;
    char   data[];
} dtls_dgram_t;

typedef struct dtls_session_inbox_s {
    dtls_dgram_t** slots;
    uint32_t       cap;
    uint32_t       head;
    uint32_t       tail;
    mco_coro*      parked;
    scheduler_t*   sched;
    sched_timer_t* deadline_timer;
    bool           closed;
    bool           timed_out;
} dtls_session_inbox_t;

struct xylem_dtls_conn_s {
    SSL*                    ssl;
    xylem_dtls_ctx_t*       ctx;
    addr_t                  peer_addr;
    char                    alpn[256];
    xylem_err_t             err;
    _Atomic bool            closed;
    bool                    handshake_done;

    /* client-side only (Socket BIO path) */
    iowait_t*               waiter;
    platform_sock_t          fd;

    /* server-side only (Memory BIO path) */
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

struct xylem_dtls_listener_s {
    platform_sock_t       fd;
    iowait_t*             waiter;
    xylem_dtls_ctx_t*     ctx;
    xylem_dtls_opts_t     opts;
    rbtree_t              sessions;
    mtx_t                 sessions_mtx;
    scheduler_t*          sched;

    xylem_dtls_conn_t**   accept_slots;
    uint32_t              accept_cap;
    uint32_t              accept_head;
    uint32_t              accept_tail;
    mco_coro*             accept_parked;
    bool                  accept_closed;

    _Atomic bool          closing;
};
```

Inbox functions follow SPSC ring buffer pattern:

```c
static dtls_session_inbox_t* _inbox_create(scheduler_t* sched) {
    dtls_session_inbox_t* ib = calloc(1, sizeof(*ib));
    if (!ib) return NULL;
    ib->cap   = DTLS_INBOX_CAP;
    ib->slots = calloc(ib->cap, sizeof(dtls_dgram_t*));
    if (!ib->slots) { free(ib); return NULL; }
    ib->sched = sched;
    ib->deadline_timer = sched_timer_create(sched);
    return ib;
}

static void _inbox_destroy(dtls_session_inbox_t* ib) {
    if (!ib) return;
    while (ib->head != ib->tail) {
        free(ib->slots[ib->head & (ib->cap - 1)]);
        ib->head++;
    }
    sched_timer_destroy(ib->deadline_timer);
    free(ib->slots);
    free(ib);
}

static void _inbox_push(dtls_session_inbox_t* ib, dtls_dgram_t* dgram) {
    if (ib->closed) { free(dgram); return; }
    uint32_t mask = ib->cap - 1;
    if (ib->tail - ib->head >= ib->cap) {
        free(dgram); return;  /* drop if full */
    }
    ib->slots[ib->tail & mask] = dgram;
    ib->tail++;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}

/* Park callback: store coroutine pointer so push/close/timer can wake it. */
static bool _inbox_park_cb(mco_coro* co, void* arg) {
    dtls_session_inbox_t* ib = arg;
    ib->parked = co;
    return true;
}

static dtls_dgram_t* _inbox_pop(dtls_session_inbox_t* ib) {
    while (ib->head == ib->tail) {
        if (ib->closed) return NULL;
        scheduler_park(ib->sched, _inbox_park_cb, ib);
        if (ib->closed) return NULL;
    }
    uint32_t mask = ib->cap - 1;
    dtls_dgram_t* dgram = ib->slots[ib->head & mask];
    ib->head++;
    return dgram;
}

static void _inbox_deadline_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    dtls_session_inbox_t* ib = ud;
    ib->timed_out = true;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}

/* Pop with optional deadline. Returns NULL on close or timeout. */
static dtls_dgram_t* _inbox_pop_with_deadline(
    dtls_session_inbox_t* ib, uint64_t deadline_ms) {
    while (ib->head == ib->tail) {
        if (ib->closed) return NULL;
        ib->timed_out = false;
        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) return NULL;
            sched_timer_start(ib->deadline_timer,
                              _inbox_deadline_cb, ib,
                              deadline_ms - now, 0);
        }
        scheduler_park(ib->sched, _inbox_park_cb, ib);
        if (deadline_ms > 0) {
            sched_timer_stop(ib->deadline_timer);
        }
        if (ib->timed_out) return NULL;
        if (ib->closed) return NULL;
    }
    uint32_t mask = ib->cap - 1;
    dtls_dgram_t* dgram = ib->slots[ib->head & mask];
    ib->head++;
    return dgram;
}

static void _inbox_close(dtls_session_inbox_t* ib) {
    if (!ib) return;
    ib->closed = true;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}
```

Accept queue helpers (same SPSC pattern, stores `xylem_dtls_conn_t*`):

```c
static void _accept_queue_push(xylem_dtls_listener_t* ln,
                               xylem_dtls_conn_t* conn) {
    uint32_t mask = ln->accept_cap - 1;
    if (ln->accept_tail - ln->accept_head >= ln->accept_cap) {
        return;  /* drop if full — shouldn't happen */
    }
    ln->accept_slots[ln->accept_tail & mask] = conn;
    ln->accept_tail++;
    if (ln->accept_parked) {
        mco_coro* co = ln->accept_parked;
        ln->accept_parked = NULL;
        scheduler_schedule(ln->sched, co);
    }
}

static bool _accept_queue_park_cb(mco_coro* co, void* arg) {
    xylem_dtls_listener_t* ln = arg;
    ln->accept_parked = co;
    return true;
}

static xylem_dtls_conn_t* _accept_queue_pop(xylem_dtls_listener_t* ln) {
    while (ln->accept_head == ln->accept_tail) {
        if (ln->accept_closed) return NULL;
        scheduler_park(ln->sched, _accept_queue_park_cb, ln);
        if (ln->accept_closed) return NULL;
    }
    uint32_t mask = ln->accept_cap - 1;
    xylem_dtls_conn_t* conn = ln->accept_slots[ln->accept_head & mask];
    ln->accept_head++;
    return conn;
}

static void _accept_queue_close(xylem_dtls_listener_t* ln) {
    ln->accept_closed = true;
    if (ln->accept_parked) {
        mco_coro* co = ln->accept_parked;
        ln->accept_parked = NULL;
        scheduler_schedule(ln->sched, co);
    }
}
```

Server-side flush helper using `platform_socket_sendto`:

```c
static void _dtls_server_flush_write_bio(xylem_dtls_conn_t* dtls) {
    char buf[16384];
    int  n;
    while ((n = BIO_read(dtls->write_bio, buf, sizeof(buf))) > 0) {
        socklen_t addrlen =
            (dtls->peer_addr.storage.ss_family == AF_INET6)
                ? (socklen_t)sizeof(struct sockaddr_in6)
                : (socklen_t)sizeof(struct sockaddr_in);
        platform_socket_sendto(
            dtls->listener->fd, buf, n,
            &dtls->peer_addr.storage, addrlen);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add src/net/xylem-dtls.c
git commit -m "feat(dtls): add foundation types, inbox, ctx, and helpers"
```

---

### Task 4: Implement client path (dial, recv, send, close)

**Files:**
- Modify: `src/net/xylem-dtls.c` (append client functions)

- [ ] **Step 1: Add client handshake, recv, send, close**

Append to `src/net/xylem-dtls.c` after the helpers from Task 3. Follow the TLS implementation at `src/net/xylem-tls.c:224-575` as the template:

`_dtls_client_do_handshake` — identical to `_tls_do_handshake`:
```c
static int _dtls_client_do_handshake(xylem_dtls_conn_t* dtls) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) return 0;
        int err = SSL_get_error(dtls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(dtls->waiter);
            if (r != IOWAIT_READY) {
                dtls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(dtls->waiter);
            if (r != IOWAIT_READY) {
                dtls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
        } else {
            unsigned long ssl_err = ERR_peek_error();
            xylem_loge("dtls handshake: ssl_error=%d reason=%s",
                       err,
                       ERR_reason_error_string(ssl_err)
                           ? ERR_reason_error_string(ssl_err)
                           : "unknown");
            dtls->err = XYLEM_ERR_DTLS;
            return -1;
        }
    }
}
```

`_dtls_cache_alpn` — identical to TLS:
```c
static void _dtls_cache_alpn(xylem_dtls_conn_t* dtls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(dtls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(dtls->alpn)) {
        memcpy(dtls->alpn, alpn_proto, alpn_len);
        dtls->alpn[alpn_len] = '\0';
    }
}
```

`_dtls_client_free`:
```c
static void _dtls_client_free(xylem_dtls_conn_t* dtls) {
    if (dtls->ssl) SSL_free(dtls->ssl);
    if (dtls->waiter) iowait_destroy(dtls->waiter);
    if (dtls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(dtls->fd);
    }
    free(dtls);
}
```

`xylem_dtls_dial` — follow TLS `xylem_tls_dial` but with `SOCK_DGRAM`:
```c
xylem_dtls_conn_t* xylem_dtls_dial(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    bool            connected = false;
    platform_sock_t fd = platform_socket_dial(
        host, port_str, SOCK_DGRAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("dtls dial: socket failed for %s:%u", host, port);
        return NULL;
    }

    xylem_dtls_conn_t* dtls = calloc(1, sizeof(*dtls));
    if (!dtls) { platform_socket_close(fd); return NULL; }

    dtls->fd  = fd;
    dtls->ctx = ctx;
    addr_pton(host, port, &dtls->peer_addr);

    dtls->waiter = iowait_create(fd);
    if (!dtls->waiter) {
        platform_socket_close(fd);
        free(dtls);
        return NULL;
    }

    uint64_t timeout = (opts && opts->connect_timeout_ms > 0)
        ? opts->connect_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                        + timeout;
    iowait_set_rd_deadline(dtls->waiter, deadline);
    iowait_set_wr_deadline(dtls->waiter, deadline);

    dtls->ssl = SSL_new(ctx->ssl_ctx);
    if (!dtls->ssl) {
        _dtls_client_free(dtls);
        return NULL;
    }
    SSL_set_fd(dtls->ssl, (int)fd);
    SSL_set_connect_state(dtls->ssl);

    if (_dtls_client_do_handshake(dtls) != 0) {
        _dtls_client_free(dtls);
        return NULL;
    }

    iowait_set_rd_deadline(dtls->waiter, 0);
    iowait_set_wr_deadline(dtls->waiter, 0);

    _dtls_cache_alpn(dtls);
    return dtls;
}
```

Client recv — follow `_tls_raw_recv` pattern:
```c
static int64_t _dtls_client_recv(xylem_dtls_conn_t* dtls,
                                 void* buf, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        dtls->err = XYLEM_ERR_CLOSED;
        return -1;
    }
    for (;;) {
        ERR_clear_error();
        int n = SSL_read(dtls->ssl, buf, (int)len);
        if (n > 0) return n;
        int err = SSL_get_error(dtls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            dtls->err = XYLEM_ERR_PEER_CLOSED;
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(dtls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&dtls->closed,
                                        memory_order_acquire)) {
                dtls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(dtls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&dtls->closed,
                                        memory_order_acquire)) {
                dtls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        dtls->err = XYLEM_ERR_DTLS;
        return -1;
    }
}
```

Client send — follow `_tls_raw_send` but datagram-atomic:
```c
static int _dtls_client_send(xylem_dtls_conn_t* dtls,
                             const void* data, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        dtls->err = XYLEM_ERR_CLOSED;
        return -1;
    }
    for (;;) {
        ERR_clear_error();
        int n = SSL_write(dtls->ssl, data, (int)len);
        if (n > 0) return 0;
        int err = SSL_get_error(dtls->ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(dtls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&dtls->closed,
                                        memory_order_acquire)) {
                dtls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(dtls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&dtls->closed,
                                        memory_order_acquire)) {
                dtls->err = (r == IOWAIT_TIMEOUT)
                    ? XYLEM_ERR_TIMEOUT : XYLEM_ERR_CLOSED;
                return -1;
            }
            continue;
        }
        dtls->err = XYLEM_ERR_DTLS;
        return -1;
    }
}
```

Client close:
```c
static void _dtls_client_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) return;
    iowait_close(dtls->waiter);
    _dtls_client_free(dtls);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/net/xylem-dtls.c
git commit -m "feat(dtls): implement client path (dial, recv, send, close)"
```

---

### Task 5: Implement server path (listen, dispatcher, handshake, accept)

**Files:**
- Modify: `src/net/xylem-dtls.c` (append server functions)

- [ ] **Step 1: Add retransmit timer helpers**

```c
static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls);

static void _dtls_retransmit_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_dtls_conn_t* dtls = ud;
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) return;
    DTLSv1_handle_timeout(dtls->ssl);
    _dtls_server_flush_write_bio(dtls);
    _dtls_arm_retransmit(dtls);
}

static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls) {
    struct timeval tv;
    if (DTLSv1_get_timeout(dtls->ssl, &tv)) {
        uint64_t ms = (uint64_t)tv.tv_sec * 1000 +
                      (uint64_t)tv.tv_usec / 1000;
        if (ms == 0) ms = 1;
        sched_timer_start(dtls->retransmit_timer,
                          _dtls_retransmit_cb, dtls, ms, 0);
    }
}

static void _dtls_stop_retransmit(xylem_dtls_conn_t* dtls) {
    if (dtls->retransmit_timer) sched_timer_stop(dtls->retransmit_timer);
}
```

- [ ] **Step 2: Add handshake timeout callback**

```c
static void _dtls_handshake_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_dtls_conn_t* dtls = ud;
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) return;
    dtls->err = XYLEM_ERR_TIMEOUT;
    _inbox_close(dtls->inbox);
}
```

- [ ] **Step 3: Add handshake coroutine**

```c
static void _dtls_handshake_coro(void* arg) {
    xylem_dtls_conn_t* dtls = arg;
    xylem_dtls_listener_t* ln = dtls->listener;

    if (_dtls_init_ssl(dtls) != 0) {
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        _inbox_destroy(dtls->inbox);
        free(dtls);
        return;
    }

    SSL_set_accept_state(dtls->ssl);
    SSL_set_ex_data(dtls->ssl, _dtls_peer_addr_idx, &dtls->peer_addr);

    sched_timer_start(dtls->handshake_timer,
                      _dtls_handshake_timeout_cb, dtls,
                      DTLS_DEFAULT_TIMEOUT_MS, 0);

    bool success = false;
    while (!dtls->handshake_done) {
        dtls_dgram_t* dgram = _inbox_pop(dtls->inbox);
        if (!dgram) break;

        BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
        free(dgram);

        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) {
            dtls->handshake_done = true;
            success = true;
            break;
        }

        int err = SSL_get_error(dtls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            _dtls_server_flush_write_bio(dtls);
            _dtls_arm_retransmit(dtls);
            continue;
        }

        _dtls_server_flush_write_bio(dtls);
        dtls->err = XYLEM_ERR_DTLS;
        break;
    }

    _dtls_stop_retransmit(dtls);
    sched_timer_stop(dtls->handshake_timer);

    if (!success) {
        SSL_free(dtls->ssl);
        dtls->ssl = NULL;
        sched_timer_destroy(dtls->retransmit_timer);
        sched_timer_destroy(dtls->handshake_timer);
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        _inbox_destroy(dtls->inbox);
        free(dtls);
        return;
    }

    _dtls_cache_alpn(dtls);
    _accept_queue_push(ln, dtls);
}
```

- [ ] **Step 4: Add dispatcher coroutine**

```c
static void _dtls_dispatcher(void* arg) {
    xylem_dtls_listener_t* ln = arg;
    char buf[65535];

    while (!atomic_load_explicit(&ln->closing, memory_order_acquire)) {
        struct sockaddr_storage from_ss;
        socklen_t from_len = sizeof(from_ss);
        ssize_t n = platform_socket_recvfrom(
            ln->fd, buf, sizeof(buf), &from_ss, &from_len);

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                iowait_result_t r = iowait_read(ln->waiter);
                if (r != IOWAIT_READY) break;
                continue;
            }
            continue;
        }

        addr_t from_addr;
        memcpy(&from_addr.storage, &from_ss, sizeof(from_ss));

        mtx_lock(&ln->sessions_mtx);
        xylem_dtls_conn_t* dtls = _dtls_find_session(ln, &from_addr);
        mtx_unlock(&ln->sessions_mtx);

        if (dtls) {
            dtls_dgram_t* dgram = malloc(sizeof(dtls_dgram_t) + (size_t)n);
            if (dgram) {
                dgram->len = (size_t)n;
                memcpy(dgram->data, buf, (size_t)n);
                _inbox_push(dtls->inbox, dgram);
            }
            continue;
        }

        dtls = calloc(1, sizeof(*dtls));
        if (!dtls) continue;
        dtls->ctx       = ln->ctx;
        dtls->peer_addr = from_addr;
        dtls->listener  = ln;
        dtls->inbox     = _inbox_create(ln->sched);
        if (!dtls->inbox) { free(dtls); continue; }

        dtls->retransmit_timer = sched_timer_create(ln->sched);
        dtls->handshake_timer  = sched_timer_create(ln->sched);
        if (!dtls->retransmit_timer || !dtls->handshake_timer) {
            sched_timer_destroy(dtls->retransmit_timer);
            sched_timer_destroy(dtls->handshake_timer);
            _inbox_destroy(dtls->inbox);
            free(dtls);
            continue;
        }

        dtls_dgram_t* dgram = malloc(sizeof(dtls_dgram_t) + (size_t)n);
        if (!dgram) {
            sched_timer_destroy(dtls->retransmit_timer);
            sched_timer_destroy(dtls->handshake_timer);
            _inbox_destroy(dtls->inbox);
            free(dtls);
            continue;
        }
        dgram->len = (size_t)n;
        memcpy(dgram->data, buf, (size_t)n);
        _inbox_push(dtls->inbox, dgram);

        mtx_lock(&ln->sessions_mtx);
        rbtree_insert(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);

        runtime_spawn(_dtls_handshake_coro, dtls);
    }
}
```

- [ ] **Step 5: Add listen and accept**

```c
xylem_dtls_listener_t* xylem_dtls_listen(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("dtls listen: failed for %s:%u", host, port);
        return NULL;
    }

    xylem_dtls_listener_t* ln = calloc(1, sizeof(*ln));
    if (!ln) { platform_socket_close(fd); return NULL; }

    ln->fd    = fd;
    ln->ctx   = ctx;
    ln->sched = runtime_get_scheduler();
    if (opts) ln->opts = *opts;

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    rbtree_init(&ln->sessions,
                _dtls_session_cmp_nn, _dtls_session_cmp_kn);
    mtx_init(&ln->sessions_mtx, mtx_plain);

    ln->accept_cap   = 64;
    ln->accept_slots = calloc(ln->accept_cap,
                              sizeof(xylem_dtls_conn_t*));
    if (!ln->accept_slots) {
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    runtime_spawn(_dtls_dispatcher, ln);
    return ln;
}

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    return _accept_queue_pop(ln);
}
```

- [ ] **Step 6: Commit**

```bash
git add src/net/xylem-dtls.c
git commit -m "feat(dtls): implement server path (listen, dispatcher, handshake, accept)"
```

---

### Task 6: Implement server recv/send, unified close, deadlines, accessors

**Files:**
- Modify: `src/net/xylem-dtls.c` (append remaining functions)

- [ ] **Step 1: Add server recv and send**

```c
static int64_t _dtls_server_recv(xylem_dtls_conn_t* dtls,
                                 void* buf, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        dtls->err = XYLEM_ERR_CLOSED;
        return -1;
    }
retry:;
    dtls_dgram_t* dgram = _inbox_pop_with_deadline(
        dtls->inbox, dtls->rd_deadline_ms);
    if (!dgram) {
        if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
            dtls->err = XYLEM_ERR_CLOSED;
        } else {
            dtls->err = XYLEM_ERR_TIMEOUT;
        }
        return -1;
    }
    BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
    free(dgram);

    ERR_clear_error();
    int n = SSL_read(dtls->ssl, buf, (int)len);
    if (n > 0) return n;

    int err = SSL_get_error(dtls->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
        dtls->err = XYLEM_ERR_PEER_CLOSED;
        return 0;
    }
    if (err == SSL_ERROR_WANT_READ) goto retry;
    dtls->err = XYLEM_ERR_DTLS;
    return -1;
}

static int _dtls_server_send(xylem_dtls_conn_t* dtls,
                             const void* data, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        dtls->err = XYLEM_ERR_CLOSED;
        return -1;
    }
    ERR_clear_error();
    int n = SSL_write(dtls->ssl, data, (int)len);
    if (n <= 0) {
        dtls->err = XYLEM_ERR_DTLS;
        return -1;
    }
    _dtls_server_flush_write_bio(dtls);
    return 0;
}
```

- [ ] **Step 2: Add unified public recv/send dispatchers**

```c
int64_t xylem_dtls_recv(xylem_dtls_conn_t* dtls, void* buf, size_t len) {
    if (dtls->listener) {
        return _dtls_server_recv(dtls, buf, len);
    }
    return _dtls_client_recv(dtls, buf, len);
}

int xylem_dtls_send(xylem_dtls_conn_t* dtls,
                    const void* data, size_t len) {
    if (dtls->listener) {
        return _dtls_server_send(dtls, data, len);
    }
    return _dtls_client_send(dtls, data, len);
}
```

- [ ] **Step 3: Add server session close**

```c
static void _dtls_server_session_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) return;
    _dtls_stop_retransmit(dtls);
    if (dtls->handshake_timer) sched_timer_stop(dtls->handshake_timer);

    if (dtls->handshake_done && dtls->ssl) {
        SSL_shutdown(dtls->ssl);
        _dtls_server_flush_write_bio(dtls);
    }

    _inbox_close(dtls->inbox);

    xylem_dtls_listener_t* ln = dtls->listener;
    mtx_lock(&ln->sessions_mtx);
    rbtree_remove(&ln->sessions, &dtls->server_node);
    mtx_unlock(&ln->sessions_mtx);

    if (dtls->ssl) { SSL_free(dtls->ssl); dtls->ssl = NULL; }
    sched_timer_destroy(dtls->retransmit_timer);
    sched_timer_destroy(dtls->handshake_timer);
    _inbox_destroy(dtls->inbox);
    free(dtls);
}
```

- [ ] **Step 4: Add unified close and listener close**

```c
void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    if (dtls->listener) {
        _dtls_server_session_close(dtls);
    } else {
        _dtls_client_close(dtls);
    }
}

void xylem_dtls_close_listener(xylem_dtls_listener_t* ln) {
    if (atomic_exchange(&ln->closing, true)) return;

    mtx_lock(&ln->sessions_mtx);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_dtls_conn_t* dtls =
            rbtree_entry(node, xylem_dtls_conn_t, server_node);
        mtx_unlock(&ln->sessions_mtx);
        xylem_dtls_close(dtls);
        mtx_lock(&ln->sessions_mtx);
    }
    mtx_unlock(&ln->sessions_mtx);

    iowait_close(ln->waiter);
    _accept_queue_close(ln);
    /* Dispatcher coroutine exits after iowait_close wakes it.
     * Cleanup happens after a small yield to let it finish. */
    runtime_sleep(1);
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    mtx_destroy(&ln->sessions_mtx);
    free(ln->accept_slots);
    free(ln);
}
```

- [ ] **Step 5: Add deadline setters and accessors**

```c
void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->listener) {
        dtls->rd_deadline_ms = deadline_ms;
    } else {
        iowait_set_rd_deadline(dtls->waiter, deadline_ms);
    }
}

void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->listener) {
        dtls->wr_deadline_ms = deadline_ms;
    } else {
        iowait_set_wr_deadline(dtls->waiter, deadline_ms);
    }
}

xylem_err_t xylem_dtls_get_error(xylem_dtls_conn_t* dtls) {
    return dtls->err;
}

const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls) {
    return dtls->alpn[0] ? dtls->alpn : NULL;
}

int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port) {
    return addr_ntop(&dtls->peer_addr, host, host_len, port);
}

int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port) {
    platform_sock_t fd = dtls->listener ? dtls->listener->fd : dtls->fd;
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(fd, (struct sockaddr*)&addr.storage, &len) != 0)
        return -1;
    return addr_ntop(&addr, host, host_len, port);
}

int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char* host, size_t host_len, uint16_t* port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(ln->fd, (struct sockaddr*)&addr.storage, &len) != 0)
        return -1;
    return addr_ntop(&addr, host, host_len, port);
}
```

- [ ] **Step 6: Commit**

```bash
git add src/net/xylem-dtls.c
git commit -m "feat(dtls): implement server recv/send, close, deadlines, accessors"
```

---

### Task 7: Enable build and verify compilation

**Files:**
- Modify: `CMakeLists.txt:114`

- [ ] **Step 1: Uncomment the DTLS source**

In `CMakeLists.txt`, change line 114 from:

```cmake
	# src/net/xylem-dtls.c  # TODO: port to coroutine API
```

to:

```cmake
	src/net/xylem-dtls.c
```

- [ ] **Step 2: Build and fix any compilation errors**

Run: `cmake --build build --target xylem`
Expected: PASS. Fix any missing includes, typos, or linkage issues.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(dtls): re-enable xylem-dtls.c in CMake"
```

---

### Task 8: Write tests — loopback handshake + echo

**Files:**
- Rewrite: `tests/test-dtls.c`

Follow the TLS test patterns from `tests/test-tls.c`. Use the same self-signed cert generation helper (`_gen_self_signed`), `xylem_channel` for synchronization, `xylem_waitgroup` for join, `xylem_timer_after` for watchdog.

- [ ] **Step 1: Write test infrastructure and basic tests**

Rewrite `tests/test-dtls.c` with:

1. `_gen_self_signed()` — copy from `test-tls.c` (with SAN for 127.0.0.1)
2. `_watchdog_cb()` — `ASSERT(0 && "test timed out")`
3. `test_ctx_create_destroy()` — create + destroy ctx
4. `test_load_cert_valid()` — gen self-signed, load, destroy
5. `test_handshake_and_echo()`:
   - Server coroutine: listen, accept, recv, send back, close
   - Client coroutine: wait for ready, dial, send "hello xylem dtls", recv echo, verify, close
   - Main: gen certs, create ctxs (verify=false), spawn server+client, waitgroup_wait
6. `test_alpn_negotiation()`:
   - Both ctxs set ALPN ["h2", "http/1.1"]
   - After connect, both sides verify `xylem_dtls_get_alpn()` returns "h2"
7. `test_close_idempotent()`:
   - Dial, close twice, no crash
8. `main()` calling all tests

Follow exact patterns from `test-tls.c:205-286` for the echo test structure (channel for ready signal, waitgroup for join, port `15433` for DTLS).

- [ ] **Step 2: Build and run tests**

Run: `cmake --build build --target test-dtls && ctest -R test-dtls -V`
Expected: All tests PASS.

- [ ] **Step 3: Commit**

```bash
git add tests/test-dtls.c
git commit -m "test(dtls): add loopback handshake, echo, ALPN, and close tests"
```

---

### Task 9: Write tests — deadline, close-wakes-recv, concurrent sessions

**Files:**
- Modify: `tests/test-dtls.c` (append tests)

- [ ] **Step 1: Add recv deadline test**

```c
/* Server accepts but never sends; client recv with 200ms deadline should timeout. */
```
Server: listen, accept, sleep(2000), close.
Client: dial, set_read_deadline(now+200), recv — expect -1 with XYLEM_ERR_TIMEOUT.

- [ ] **Step 2: Add close-wakes-recv test**

Spawn two coroutines: one calls `xylem_dtls_recv` (blocks), the other sleeps 100ms then calls `xylem_dtls_close`. Recv should return -1 with XYLEM_ERR_CLOSED.

- [ ] **Step 3: Add concurrent sessions test**

Spawn 4 client coroutines, each dials to the same listener. Server loop: accept 4 times, echo each. Verify all 4 get correct echoes.

- [ ] **Step 4: Build and run**

Run: `cmake --build build --target test-dtls && ctest -R test-dtls -V`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/test-dtls.c
git commit -m "test(dtls): add deadline, close-wakes-recv, concurrent sessions tests"
```

---

### Task 10: Update design doc

**Files:**
- Rewrite: `docs/dtls-design.md`

- [ ] **Step 1: Update the design doc**

Rewrite `docs/dtls-design.md` to reflect the coroutine architecture. Follow the structure of `docs/tls-design.md` (if it exists) or the spec at `docs/superpowers/specs/2026-05-17-dtls-coroutine-design.md`. Key sections:

- Overview: coroutine blocking API, dual-path architecture
- Public API: table of all functions with one-line descriptions
- Internal architecture: client (Socket BIO + iowait), server (Memory BIO + dispatcher + inbox + accept queue)
- Cookie mechanism (unchanged)
- Retransmit timer (sched_timer)
- Close flows
- Thread safety (rbtree mutex)
- Differences from TLS

- [ ] **Step 2: Commit**

```bash
git add docs/dtls-design.md
git commit -m "docs(dtls): update design doc for coroutine architecture"
```

# HTTP Transport Function Pointer Refactor

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `#ifdef XYLEM_ENABLE_TLS`-guarded transport union in `http.h` with a function-pointer based `http_transport_t`, making the shared HTTP implementation (`http.c`) completely TLS-agnostic.

**Architecture:** The new `http_transport_t` holds a `void* conn` and five function pointers (read/write/close/set_rd_deadline/set_wr_deadline). Each wrapper (`xylem-http.c` for TCP, `xylem-https.c` for TLS) populates these when creating a transport. The client `http_do_request` accepts a `http_dial_fn_t` callback so it never mentions TLS. The server struct stores a generic `void* listener` with a close callback.

**Tech Stack:** C11, CMake/Ninja, Windows MSVC + Linux GCC

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `src/net/http/http.h` | Modify | New `http_transport_t` struct, new `http_srv_t`, new `http_dial_fn_t` typedef, remove enum/ifdefs |
| `src/net/http/http.c` | Modify | Remove transport helpers, use `t->read(t->conn,...)` directly, accept dial callback |
| `src/net/http/xylem-http.c` | Modify | Provide TCP function pointers for transport + TCP dial function |
| `src/net/http/xylem-https.c` | Modify | Provide TLS function pointers for transport + TLS dial function |

---

### Task 1: Refactor `http.h` — new transport and server structs

**Files:**
- Modify: `src/net/http/http.h`

- [ ] **Step 1: Replace the transport type and server struct**

Replace the entire section from the `#ifdef XYLEM_ENABLE_TLS` / `#include "xylem/net/xylem-tls.h"` through the `http_srv_t` definition and the `http_do_request` signature. The new `http.h` content from line 40 onwards (after `#include "utils.h"` and the stdint/stdbool includes) should be:

```c
/* ─── Transport abstraction ──────────────────────────────────────── */

typedef struct {
    void* conn;
    int  (*read)(void* conn, void* buf, int len);
    int  (*write)(void* conn, const void* data, int len);
    void (*close)(void* conn);
    void (*set_rd_deadline)(void* conn, uint64_t ms);
    void (*set_wr_deadline)(void* conn, uint64_t ms);
} http_transport_t;

/**
 * Dial callback type for client connections.
 * Must allocate and fill a transport (conn + function pointers).
 * Returns 0 on success, -1 on failure.
 */
typedef int (*http_dial_fn_t)(http_transport_t* out,
                              const char* host, uint16_t port,
                              uint64_t timeout_ms, void* dial_ctx);

/* ─── Server internal structure ──────────────────────────────────── */

typedef struct http_srv_s {
    void*                   listener;
    void                    (*close_listener)(void* listener);
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    uint16_t                port;
    xylem_http_gzip_opts_t  gzip_opts;
} http_srv_t;
```

Remove these lines entirely:
- `#ifdef XYLEM_ENABLE_TLS` / `#include "xylem/net/xylem-tls.h"` / `#endif` (lines 41-43 currently)
- The `http_transport_kind_t` enum (lines 47-50)
- The old `http_transport_t` struct with its union (lines 52-60)
- The old `http_srv_t` struct with its `#ifdef` members (lines 64-74)

- [ ] **Step 2: Update `http_do_request` signature**

Replace the current `http_do_request` declaration:

```c
extern xylem_http_res_t* http_do_request(
    const char*              method,
    const char*              url,
    const void*              body,
    size_t                   body_len,
    const char*              content_type,
    const xylem_http_opts_t* opts,
    http_dial_fn_t           dial_fn,
    void*                    dial_ctx);
```

This replaces the old `forced_kind` and `tls_ctx` parameters with `dial_fn` and `dial_ctx`.

- [ ] **Step 3: Verify no `#ifdef XYLEM_ENABLE_TLS` remains in `http.h`**

Visually confirm the file has zero `#ifdef XYLEM_ENABLE_TLS` and no `#include "xylem/net/xylem-tls.h"`.

---

### Task 2: Refactor `http.c` — remove transport helpers and TLS ifdefs

**Files:**
- Modify: `src/net/http/http.c`

- [ ] **Step 1: Delete the three transport dispatch helpers**

Remove the functions `_transport_read`, `_transport_write`, and `_transport_close` (lines 43-76 currently).

- [ ] **Step 2: Replace all `_transport_read` calls with direct function pointer calls**

There are two call sites:
1. `http_srv_conn_coroutine` line 736: `_transport_read(&ctx->transport, readbuf, (int)sizeof(readbuf))`
   Replace with: `ctx->transport.read(ctx->transport.conn, readbuf, (int)sizeof(readbuf))`

2. `http_do_request` line 1452: `_transport_read(&transport, readbuf, (int)sizeof(readbuf))`
   Replace with: `transport.read(transport.conn, readbuf, (int)sizeof(readbuf))`

- [ ] **Step 3: Replace all `_transport_write` calls with direct function pointer calls**

There are multiple call sites:
1. `_flush_headers` line 484: `_transport_write(res->_transport, buf, off)`
   Replace with: `res->_transport->write(res->_transport->conn, buf, off)`

2. `_write_chunk` line 499: `_transport_write(res->_transport, chunk_hdr, hdr_len)`
   Replace with: `res->_transport->write(res->_transport->conn, chunk_hdr, hdr_len)`

3. `_write_chunk` line 502: `_transport_write(res->_transport, data, (int)len)`
   Replace with: `res->_transport->write(res->_transport->conn, data, (int)len)`

4. `_write_chunk` line 505: `_transport_write(res->_transport, "\r\n", 2)`
   Replace with: `res->_transport->write(res->_transport->conn, "\r\n", 2)`

5. `_finalize_response` line 593: `_transport_write(res->_transport, "0\r\n\r\n", 5)`
   Replace with: `res->_transport->write(res->_transport->conn, "0\r\n\r\n", 5)`

6. `http_srv_conn_coroutine` line 750: `_transport_write(&ctx->transport, bad, (int)strlen(bad))`
   Replace with: `ctx->transport.write(ctx->transport.conn, bad, (int)strlen(bad))`

7. `http_do_request` line 1433: `_transport_write(&transport, req_buf, (int)req_len)`
   Replace with: `transport.write(transport.conn, req_buf, (int)req_len)`

- [ ] **Step 4: Replace all `_transport_close` calls with direct function pointer calls**

Call sites:
1. `_pool_acquire` line 131: `_transport_close(&ic->transport)`
   Replace with: `ic->transport.close(ic->transport.conn)`

2. `_pool_release` line 160: `_transport_close(t)`
   Replace with: `t->close(t->conn)`

3. `_pool_release` line 172: `_transport_close(&entry->conns[0].transport)`
   Replace with: `entry->conns[0].transport.close(entry->conns[0].transport.conn)`

4. `_pool_release` line 189: `_transport_close(t)`
   Replace with: `t->close(t->conn)`

5. `http_srv_conn_coroutine` line 789: `_transport_close(&ctx->transport)`
   Replace with: `ctx->transport.close(ctx->transport.conn)`

6. `http_do_request` multiple locations (lines 1340, 1358, 1402, 1429, 1435, 1463, 1473, 1487):
   Each `_transport_close(&transport)` becomes `transport.close(transport.conn)`

- [ ] **Step 5: Refactor the dial logic in `http_do_request`**

Replace the entire dial block (lines 1305-1359 approximately, from `http_transport_kind_t hop_kind = forced_kind;` through the end of the `else` block that dials TCP) with a call to the dial callback:

```c
    /* Try to reuse a pooled connection. */
    http_transport_t transport;
    memset(&transport, 0, sizeof(transport));
    bool from_pool = _pool_acquire(&parsed, &transport);

    if (!from_pool) {
        /* No pooled connection available -- dial a new one. */
        uint64_t timeout = 10000;
        if (opts && opts->timeout_ms > 0) {
            timeout = opts->timeout_ms;
        }
        if (dial_fn(&transport, parsed.host, parsed.port, timeout, dial_ctx) != 0) {
            return NULL;
        }
    }
```

- [ ] **Step 6: Refactor the deadline-setting block**

Replace the current `if (transport.kind == HTTP_TRANSPORT_TCP) { ... } #ifdef ... else { ... } #endif` block (lines 1369-1378) with:

```c
    /* Set deadlines. */
    uint64_t deadline_ms = 0;
    if (opts && opts->timeout_ms > 0) {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                      + opts->timeout_ms;
    } else {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 30000;
    }
    if (transport.set_rd_deadline) {
        transport.set_rd_deadline(transport.conn, deadline_ms);
    }
    if (transport.set_wr_deadline) {
        transport.set_wr_deadline(transport.conn, deadline_ms);
    }
```

- [ ] **Step 7: Update `http_do_request` function signature**

Change the function definition signature to match the new declaration:

```c
xylem_http_res_t* http_do_request(
    const char*              method,
    const char*              url,
    const void*              body,
    size_t                   body_len,
    const char*              content_type,
    const xylem_http_opts_t* opts,
    http_dial_fn_t           dial_fn,
    void*                    dial_ctx) {
```

- [ ] **Step 8: Remove the `redirect_loop` hop_kind logic**

The current code at `redirect_loop:` determines `hop_kind` based on the scheme. Since the dial function is scheme-aware (the TCP dial function is passed by `xylem-http.c`, the TLS dial function by `xylem-https.c`), this logic is no longer needed. However, there is a subtlety: HTTP-to-HTTPS redirects. The dial function provided by `xylem-http.c` can only dial TCP. If an HTTP client follows a redirect to an HTTPS URL, it will fail gracefully (the TCP dial is called and the HTTPS server will reject the plaintext). This is acceptable behavior for now — cross-scheme redirects require the caller to use the HTTPS client.

Simply remove the `hop_kind` variable and the block that sets it:
```c
redirect_loop:
    ;  /* empty statement for label before declaration */
```
Keep this label and empty statement. Remove:
```c
    /* Determine transport kind for this hop.
     * On redirect, scheme may change -- respect it. */
    http_transport_kind_t hop_kind = forced_kind;
    if (strcmp(parsed.scheme, "https") == 0) {
        hop_kind = HTTP_TRANSPORT_TLS;
    } else if (strcmp(parsed.scheme, "http") == 0) {
        hop_kind = HTTP_TRANSPORT_TCP;
    }
```

- [ ] **Step 9: Verify no `#ifdef XYLEM_ENABLE_TLS` remains in `http.c`**

Confirm zero occurrences.

- [ ] **Step 10: Commit**

```
git add src/net/http/http.h src/net/http/http.c
git commit -m "refactor(http): replace transport union with function pointers in http.h/http.c"
```

---

### Task 3: Update `xylem-http.c` — TCP transport wiring

**Files:**
- Modify: `src/net/http/xylem-http.c`

- [ ] **Step 1: Add a static TCP dial function**

Add before `_http_accept_coroutine`:

```c
static int _tcp_dial(http_transport_t* out, const char* host, uint16_t port,
                     uint64_t timeout_ms, void* dial_ctx) {
    (void)dial_ctx;
    xylem_tcp_conn_t* c = xylem_tcp_dial(host, port, timeout_ms, NULL);
    if (!c) {
        return -1;
    }
    out->conn = c;
    out->read = (int (*)(void*, void*, int))xylem_tcp_read;
    out->write = (int (*)(void*, const void*, int))xylem_tcp_write;
    out->close = (void (*)(void*))xylem_tcp_close;
    out->set_rd_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline;
    out->set_wr_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline;
    return 0;
}
```

- [ ] **Step 2: Add a helper to fill transport from an existing TCP connection**

Add after `_tcp_dial`:

```c
static void _tcp_transport_init(http_transport_t* t, xylem_tcp_conn_t* conn) {
    t->conn = conn;
    t->read = (int (*)(void*, void*, int))xylem_tcp_read;
    t->write = (int (*)(void*, const void*, int))xylem_tcp_write;
    t->close = (void (*)(void*))xylem_tcp_close;
    t->set_rd_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline;
    t->set_wr_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline;
}
```

- [ ] **Step 3: Update `_http_accept_coroutine` to use function pointers**

Replace the current transport initialization:

```c
        ctx->srv = srv;
        ctx->transport.kind = HTTP_TRANSPORT_TCP;
        ctx->transport.conn.tcp = conn;
```

With:

```c
        ctx->srv = srv;
        _tcp_transport_init(&ctx->transport, conn);
```

- [ ] **Step 4: Update `xylem_http_listen` to use new server struct fields**

Replace:
```c
    srv->transport_kind = HTTP_TRANSPORT_TCP;
    srv->tcp_listener   = ln;
    srv->handler        = handler;
    srv->userdata       = userdata;
```

With:
```c
    srv->listener       = ln;
    srv->close_listener = (void (*)(void*))xylem_tcp_close_listener;
    srv->handler        = handler;
    srv->userdata       = userdata;
```

- [ ] **Step 5: Update `xylem_http_close` to use generic close**

Replace:
```c
    http_srv_t* s = (http_srv_t*)srv;
    /* Closing the listener wakes the accept coroutine which then exits. */
    xylem_tcp_close_listener(s->tcp_listener);
    free(s);
```

With:
```c
    http_srv_t* s = (http_srv_t*)srv;
    s->close_listener(s->listener);
    free(s);
```

- [ ] **Step 6: Update `_http_accept_coroutine` to use generic listener**

Replace:
```c
        xylem_tcp_conn_t* conn = xylem_tcp_accept(srv->tcp_listener);
```

With:
```c
        xylem_tcp_conn_t* conn = xylem_tcp_accept(
            (xylem_tcp_listener_t*)srv->listener);
```

- [ ] **Step 7: Update all client functions to pass `_tcp_dial`**

Replace all calls to `http_do_request` that pass `HTTP_TRANSPORT_TCP, NULL` with `_tcp_dial, NULL`:

```c
xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_opts_t* opts) {
    return http_do_request("GET", url, NULL, 0, NULL, opts,
                           _tcp_dial, NULL);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_opts_t* opts) {
    return http_do_request("POST", url, body, body_len, content_type, opts,
                           _tcp_dial, NULL);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_opts_t* opts) {
    return http_do_request("PUT", url, body, body_len, content_type, opts,
                           _tcp_dial, NULL);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_opts_t* opts) {
    return http_do_request("DELETE", url, NULL, 0, NULL, opts,
                           _tcp_dial, NULL);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_opts_t* opts) {
    return http_do_request("PATCH", url, body, body_len, content_type, opts,
                           _tcp_dial, NULL);
}
```

- [ ] **Step 8: Commit**

```
git add src/net/http/xylem-http.c
git commit -m "refactor(http): wire TCP function pointers in xylem-http.c"
```

---

### Task 4: Update `xylem-https.c` — TLS transport wiring

**Files:**
- Modify: `src/net/http/xylem-https.c`

- [ ] **Step 1: Add a static TLS dial function**

Add before `_https_accept_coroutine`:

```c
static int _tls_dial(http_transport_t* out, const char* host, uint16_t port,
                     uint64_t timeout_ms, void* dial_ctx) {
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)dial_ctx;
    bool owns_ctx = false;
    if (!ctx) {
        ctx = xylem_tls_ctx_create();
        if (!ctx) {
            return -1;
        }
        owns_ctx = true;
    }
    xylem_tls_opts_t tls_opts = {0};
    tls_opts.server_name = host;
    tls_opts.handshake_timeout_ms = timeout_ms;
    xylem_tls_conn_t* c = xylem_tls_dial(host, port, ctx, &tls_opts);
    if (owns_ctx) {
        xylem_tls_ctx_destroy(ctx);
    }
    if (!c) {
        return -1;
    }
    out->conn = c;
    out->read = (int (*)(void*, void*, int))xylem_tls_read;
    out->write = (int (*)(void*, const void*, int))xylem_tls_write;
    out->close = (void (*)(void*))xylem_tls_close;
    out->set_rd_deadline = (void (*)(void*, uint64_t))xylem_tls_set_read_deadline;
    out->set_wr_deadline = (void (*)(void*, uint64_t))xylem_tls_set_write_deadline;
    return 0;
}
```

- [ ] **Step 2: Add a helper to fill transport from an existing TLS connection**

Add after `_tls_dial`:

```c
static void _tls_transport_init(http_transport_t* t, xylem_tls_conn_t* conn) {
    t->conn = conn;
    t->read = (int (*)(void*, void*, int))xylem_tls_read;
    t->write = (int (*)(void*, const void*, int))xylem_tls_write;
    t->close = (void (*)(void*))xylem_tls_close;
    t->set_rd_deadline = (void (*)(void*, uint64_t))xylem_tls_set_read_deadline;
    t->set_wr_deadline = (void (*)(void*, uint64_t))xylem_tls_set_write_deadline;
}
```

- [ ] **Step 3: Update `_https_accept_coroutine` to use function pointers and generic listener**

Replace:
```c
        xylem_tls_conn_t* conn = xylem_tls_accept(srv->tls_listener);
        if (!conn) {
            break; /* listener closed */
        }

        http_srv_conn_ctx_t* ctx =
            (http_srv_conn_ctx_t*)calloc(1, sizeof(*ctx));
        if (!ctx) {
            xylem_tls_close(conn);
            continue;
        }
        ctx->srv = srv;
        ctx->transport.kind = HTTP_TRANSPORT_TLS;
        ctx->transport.conn.tls = conn;
```

With:
```c
        xylem_tls_conn_t* conn = xylem_tls_accept(
            (xylem_tls_listener_t*)srv->listener);
        if (!conn) {
            break; /* listener closed */
        }

        http_srv_conn_ctx_t* ctx =
            (http_srv_conn_ctx_t*)calloc(1, sizeof(*ctx));
        if (!ctx) {
            xylem_tls_close(conn);
            continue;
        }
        ctx->srv = srv;
        _tls_transport_init(&ctx->transport, conn);
```

- [ ] **Step 4: Update `xylem_https_listen` to use new server struct fields**

Replace:
```c
    srv->transport_kind = HTTP_TRANSPORT_TLS;
    srv->tls_listener   = ln;
    srv->handler        = handler;
    srv->userdata       = userdata;
```

With:
```c
    srv->listener       = ln;
    srv->close_listener = (void (*)(void*))xylem_tls_close_listener;
    srv->handler        = handler;
    srv->userdata       = userdata;
```

- [ ] **Step 5: Update `xylem_https_close` to use generic close**

Replace:
```c
    http_srv_t* s = (http_srv_t*)srv;
    xylem_tls_close_listener(s->tls_listener);
    /* Note: in-flight coroutines will exit when read returns <=0. */
    free(s);
```

With:
```c
    http_srv_t* s = (http_srv_t*)srv;
    s->close_listener(s->listener);
    free(s);
```

- [ ] **Step 6: Update all client functions to pass `_tls_dial`**

Replace all calls to `http_do_request` that pass `HTTP_TRANSPORT_TLS, tls_ctx` with `_tls_dial, tls_ctx`:

```c
xylem_http_res_t* xylem_https_get(const char* url,
                                  xylem_tls_ctx_t* tls_ctx,
                                  const xylem_http_opts_t* opts) {
    return http_do_request("GET", url, NULL, 0, NULL, opts,
                           _tls_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_post(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   xylem_tls_ctx_t* tls_ctx,
                                   const xylem_http_opts_t* opts) {
    return http_do_request("POST", url, body, body_len, content_type, opts,
                           _tls_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_put(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  xylem_tls_ctx_t* tls_ctx,
                                  const xylem_http_opts_t* opts) {
    return http_do_request("PUT", url, body, body_len, content_type, opts,
                           _tls_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_delete(const char* url,
                                     xylem_tls_ctx_t* tls_ctx,
                                     const xylem_http_opts_t* opts) {
    return http_do_request("DELETE", url, NULL, 0, NULL, opts,
                           _tls_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_patch(const char* url,
                                    const void* body, size_t body_len,
                                    const char* content_type,
                                    xylem_tls_ctx_t* tls_ctx,
                                    const xylem_http_opts_t* opts) {
    return http_do_request("PATCH", url, body, body_len, content_type, opts,
                           _tls_dial, tls_ctx);
}
```

- [ ] **Step 7: Commit**

```
git add src/net/http/xylem-https.c
git commit -m "refactor(http): wire TLS function pointers in xylem-https.c"
```

---

### Task 5: Build and test

**Files:**
- None (verification only)

- [ ] **Step 1: Build Debug configuration**

Run: `cmake --build build --config Debug`
Expected: Clean compile with zero errors, zero warnings related to http.

- [ ] **Step 2: Run HTTP tests**

Run: `ctest --test-dir build -C Debug -R http --output-on-failure`
Expected: All HTTP tests pass.

- [ ] **Step 3: Verify no ifdefs remain**

Search for `XYLEM_ENABLE_TLS` in `src/net/http/http.h` and `src/net/http/http.c`:
Run: `grep -n "XYLEM_ENABLE_TLS" src/net/http/http.h src/net/http/http.c`
Expected: Zero matches.

- [ ] **Step 4: Final commit (squash if desired, or leave as-is)**

If all tasks were committed separately, the history is already clean. Optionally squash into a single commit:

```
git add -A src/net/http/
git commit -m "refactor(http): replace transport union with function pointers

Eliminate all #ifdef XYLEM_ENABLE_TLS from http.h and http.c by
introducing a function-pointer-based http_transport_t. Each wrapper
(xylem-http.c / xylem-https.c) provides its own transport callbacks
and dial function."
```

---

## Notes

- **ABI safety:** The function pointer casts (`(int (*)(void*, void*, int))xylem_tcp_read`) are safe because `xylem_tcp_conn_t*` and `void*` have identical representation on all supported platforms (same size/alignment pointer types).
- **Cross-scheme redirects:** After this refactor, an `xylem_http_get("http://...")` that encounters a 302 redirect to `https://...` will attempt to TCP-dial the HTTPS host, which will fail at the protocol level. This is acceptable — callers needing HTTPS redirect support should use `xylem_https_get`. A future enhancement could accept both dial functions.
- **Connection pool keying:** The pool key includes the scheme (`"host:port:scheme"`), so TCP and TLS connections are never mixed, even though `http_transport_t` is now uniform.

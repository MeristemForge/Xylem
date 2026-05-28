# HTTP 协程化改造 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace callback-based HTTP client/server with coroutine-based unified API in a single `xylem-http.h/c`, supporting HTTP and HTTPS through the same interface.

**Architecture:** Server spawns per-connection coroutines that synchronously parse requests via llhttp and call user handlers. Client uses `xylem_tcp_read`/`xylem_tls_read` (via `xylem_reader_t`) with an internal transparent connection pool. Transport selection (TCP vs TLS) is determined at runtime by `tls_ctx` presence (server) or URL scheme (client).

**Tech Stack:** C11, llhttp (bundled), minicoro (coroutines), iowait (I/O suspension), xylem_reader/writer (buffered I/O), OpenSSL (optional TLS)

---

## File Structure

| Action | Path | Responsibility |
|--------|------|----------------|
| Create | `include/xylem/net/xylem-http.h` | Unified public API |
| Create | `src/net/http/xylem-http.c` | All implementation (server, client, router, pool, gzip, static) |
| Keep | `src/net/http/http-common.h` | Internal URL parse, header utils, req serialize |
| Keep | `src/net/http/http-common.c` | Internal helper implementations |
| Keep | `src/net/http/xylem-http-common.c` | URL encode/decode, CORS, multipart |
| Keep | `src/net/http/llhttp/` | HTTP parser (untouched) |
| Delete | `include/xylem/net/http/xylem-http-client.h` | Replaced by xylem-http.h |
| Delete | `include/xylem/net/http/xylem-http-server.h` | Replaced by xylem-http.h |
| Delete | `include/xylem/net/http/xylem-http-common.h` | Merged into xylem-http.h |
| Delete | `src/net/http/http-transport.h` | No longer needed |
| Delete | `src/net/http/http-transport-tcp.c` | No longer needed |
| Delete | `src/net/http/http-transport-tls.c` | No longer needed |
| Delete | `src/net/http/http-transport-tls-stub.c` | No longer needed |
| Delete | `src/net/http/xylem-http-client.c` | Replaced by xylem-http.c |
| Delete | `src/net/http/xylem-http-server.c` | Replaced by xylem-http.c |
| Modify | `include/xylem.h` | Update HTTP includes |
| Modify | `CMakeLists.txt` | Update source file list |
| Modify | `tests/test-http.c` | Rewrite for coroutine API |

---

### Task 1: Create Unified Public Header

**Files:**
- Create: `include/xylem/net/xylem-http.h`

- [ ] **Step 1: Write the header file with all type definitions and function declarations**

```c
_Pragma("once")

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations for optional TLS */
typedef struct xylem_tls_ctx_s xylem_tls_ctx_t;

/* --- Common types --- */

typedef struct {
    const char* name;
    const char* value;
} xylem_http_hdr_t;

typedef struct xylem_http_req_s    xylem_http_req_t;
typedef struct xylem_http_res_s    xylem_http_res_t;
typedef struct xylem_http_srv_s    xylem_http_srv_t;
typedef struct xylem_http_router_s xylem_http_router_t;
typedef struct xylem_http_cookie_jar_s xylem_http_cookie_jar_t;
typedef struct xylem_http_multipart_s  xylem_http_multipart_t;

/* --- Handler types --- */

typedef void (*xylem_http_handler_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

typedef int (*xylem_http_middleware_fn_t)(
    xylem_http_res_t* res,
    xylem_http_req_t* req,
    void*             userdata);

/* --- Server --- */

typedef struct {
    xylem_tls_ctx_t*        tls_ctx;
    size_t                  max_body_size;
    uint64_t                idle_timeout_ms;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
} xylem_http_srv_opts_t;

extern xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts);

extern void xylem_http_close(xylem_http_srv_t* srv);

/* --- Server: Gzip --- */

typedef struct {
    bool        enabled;
    int         level;
    size_t      min_size;
    const char* mime_types;
} xylem_http_gzip_opts_t;

extern void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                                    const xylem_http_gzip_opts_t* opts);

/* --- Request accessors --- */

extern const char*  xylem_http_req_method(const xylem_http_req_t* req);
extern const char*  xylem_http_req_url(const xylem_http_req_t* req);
extern const char*  xylem_http_req_header(const xylem_http_req_t* req,
                                          const char* name);
extern const void*  xylem_http_req_body(const xylem_http_req_t* req);
extern size_t       xylem_http_req_body_len(const xylem_http_req_t* req);
extern const char*  xylem_http_req_param(const xylem_http_req_t* req,
                                         const char* name);

/* --- Response (server write + client read) --- */

extern int  xylem_http_res_set_status(xylem_http_res_t* res, int code);
extern int  xylem_http_res_set_header(xylem_http_res_t* res,
                                      const char* name, const char* value);
extern int  xylem_http_res_write(xylem_http_res_t* res,
                                 const void* data, size_t len);
extern void xylem_http_res_close(xylem_http_res_t* res);
extern int  xylem_http_res_upgrade(xylem_http_res_t* res, void** transport);

extern int          xylem_http_res_status(const xylem_http_res_t* res);
extern const char*  xylem_http_res_header(const xylem_http_res_t* res,
                                          const char* name);
extern const void*  xylem_http_res_body(const xylem_http_res_t* res);
extern size_t       xylem_http_res_body_len(const xylem_http_res_t* res);
extern void         xylem_http_res_destroy(xylem_http_res_t* res);

/* --- Client --- */

typedef struct {
    uint64_t                 timeout_ms;
    int                      max_redirects;
    size_t                   max_body_size;
    const xylem_http_hdr_t*  headers;
    size_t                   header_count;
    xylem_tls_ctx_t*         tls_ctx;
    const char*              range;
    xylem_http_cookie_jar_t* cookie_jar;
} xylem_http_opts_t;

extern xylem_http_res_t* xylem_http_get(const char* url,
                                        const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_post(const char* url,
                                         const void* body, size_t body_len,
                                         const char* content_type,
                                         const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_put(const char* url,
                                        const void* body, size_t body_len,
                                        const char* content_type,
                                        const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_delete(const char* url,
                                           const xylem_http_opts_t* opts);
extern xylem_http_res_t* xylem_http_patch(const char* url,
                                          const void* body, size_t body_len,
                                          const char* content_type,
                                          const xylem_http_opts_t* opts);

/* --- Router --- */

extern xylem_http_router_t* xylem_http_router_create(void);
extern void xylem_http_router_destroy(xylem_http_router_t* r);
extern int  xylem_http_router_add(xylem_http_router_t* r,
                                  const char* method,
                                  const char* pattern,
                                  xylem_http_handler_fn_t handler,
                                  void* userdata);
extern int  xylem_http_router_use(xylem_http_router_t* r,
                                  xylem_http_middleware_fn_t mw,
                                  void* userdata);
extern int  xylem_http_router_dispatch(xylem_http_router_t* r,
                                       xylem_http_res_t* res,
                                       xylem_http_req_t* req);

/* --- Static file server --- */

typedef struct {
    const char* root;
    const char* index_file;
    int         max_age;
    bool        precompressed;
} xylem_http_static_opts_t;

extern int xylem_http_static_serve(xylem_http_router_t* r,
                                   const char* prefix,
                                   const xylem_http_static_opts_t* opts);

/* --- Cookie jar --- */

extern xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void);
extern void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar);

/* --- Utilities --- */

extern char* xylem_http_url_encode(const char* src, size_t src_len,
                                   size_t* out_len);
extern char* xylem_http_url_decode(const char* src, size_t src_len,
                                   size_t* out_len);

typedef struct {
    const char* allowed_origins;
    const char* allowed_methods;
    const char* allowed_headers;
    const char* expose_headers;
    int         max_age;
    bool        allow_credentials;
} xylem_http_cors_t;

extern size_t xylem_http_cors_headers(const xylem_http_cors_t* cors,
                                      const char* origin,
                                      bool is_preflight,
                                      xylem_http_hdr_t* out,
                                      size_t out_cap);

extern xylem_http_multipart_t* xylem_http_multipart_parse(
    const char* content_type, const void* body, size_t body_len);
extern size_t       xylem_http_multipart_count(const xylem_http_multipart_t* mp);
extern const char*  xylem_http_multipart_name(const xylem_http_multipart_t* mp, size_t index);
extern const char*  xylem_http_multipart_filename(const xylem_http_multipart_t* mp, size_t index);
extern const char*  xylem_http_multipart_content_type(const xylem_http_multipart_t* mp, size_t index);
extern const void*  xylem_http_multipart_data(const xylem_http_multipart_t* mp, size_t index);
extern size_t       xylem_http_multipart_data_len(const xylem_http_multipart_t* mp, size_t index);
extern void         xylem_http_multipart_destroy(xylem_http_multipart_t* mp);

extern char* xylem_http_sse_build(const char* event, const char* data,
                                  size_t* len);
```

- [ ] **Step 2: Commit**

```bash
git add include/xylem/net/xylem-http.h
git commit -m "feat(http): add unified coroutine HTTP header"
```

---

### Task 2: Server Core — Accept Loop and Connection Coroutine

**Files:**
- Create: `src/net/http/xylem-http.c` (initial section: includes, structs, accept, conn loop)

- [ ] **Step 1: Write test — basic server listen and close**

In `tests/test-http.c`, add (after existing URL encode/decode tests):

```c
#include "xylem.h"
#include "assert.h"
#include <string.h>

static void _test_listen_close_main(void* arg) {
    (void)arg;
    xylem_http_srv_t* srv = xylem_http_listen("127.0.0.1", 0, NULL, NULL, NULL);
    ASSERT(srv != NULL);
    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_listen_close(void) {
    xylem_run(_test_listen_close_main, NULL, NULL);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test-http && ctest --test-dir build -R http`
Expected: link error (xylem_http_listen not defined)

- [ ] **Step 3: Implement server structs, listen, close, accept coroutine**

Create `src/net/http/xylem-http.c` with:

```c
#include "xylem/net/xylem-http.h"
#include "xylem/net/xylem-tcp.h"
#include "xylem/net/xylem-reader.h"
#include "xylem/net/xylem-writer.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-logger.h"
#include "runtime/runtime.h"
#include "http-common.h"
#include "net/http/llhttp/llhttp.h"
#include "encoding/gzip/miniz/miniz.h"
#include "platform/platform-io.h"
#include "platform/platform-info.h"

#ifdef XYLEM_ENABLE_TLS
#include "xylem/net/xylem-tls.h"
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Transport helper --- */

typedef enum {
    HTTP_TRANSPORT_TCP = 0,
    HTTP_TRANSPORT_TLS = 1,
} _http_transport_kind_t;

typedef struct {
    _http_transport_kind_t kind;
    void*                  conn;
} _http_transport_t;

static inline int _transport_read(_http_transport_t* t, void* buf, int len) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        return xylem_tcp_read(t->conn, buf, len);
#ifdef XYLEM_ENABLE_TLS
    return xylem_tls_read(t->conn, buf, len);
#else
    return -1;
#endif
}

static inline int _transport_write(_http_transport_t* t, const void* data, int len) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        return xylem_tcp_write(t->conn, data, len);
#ifdef XYLEM_ENABLE_TLS
    return xylem_tls_write(t->conn, data, len);
#else
    return -1;
#endif
}

static inline void _transport_close(_http_transport_t* t) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        xylem_tcp_close(t->conn);
#ifdef XYLEM_ENABLE_TLS
    else
        xylem_tls_close(t->conn);
#endif
}

static inline void _transport_set_read_deadline(_http_transport_t* t, uint64_t ms) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        xylem_tcp_set_read_deadline(t->conn, ms);
#ifdef XYLEM_ENABLE_TLS
    else
        xylem_tls_set_read_deadline(t->conn, ms);
#endif
}

static inline void _transport_set_write_deadline(_http_transport_t* t, uint64_t ms) {
    if (t->kind == HTTP_TRANSPORT_TCP)
        xylem_tcp_set_write_deadline(t->conn, ms);
#ifdef XYLEM_ENABLE_TLS
    else
        xylem_tls_set_write_deadline(t->conn, ms);
#endif
}

/* --- Request struct --- */

struct xylem_http_req_s {
    char           method[16];
    char*          url;
    size_t         url_len;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    http_header_t* params;
    size_t         param_count;
};

/* --- Response struct (shared server-write / client-read) --- */

struct xylem_http_res_s {
    /* Read fields (client response / after server finalize) */
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;

    /* Server-write state (NULL for client-side res) */
    _http_transport_t* transport;
    bool               headers_sent;
    bool               chunked_active;
    bool               closed;
    xylem_http_hdr_t*  resp_headers;
    size_t             resp_header_count;
    size_t             resp_header_cap;

    /* Gzip streaming state */
    mz_stream*         gzip_stream;
    bool               gzip_active;
    bool               cl_mode;

    /* Upgrade state */
    bool               in_upgrade_cb;
    bool               upgrade_accepted;
};

/* --- Server struct --- */

#define HTTP_DEFAULT_MAX_BODY    (1024 * 1024)
#define HTTP_DEFAULT_IDLE_MS     60000

struct xylem_http_srv_s {
    xylem_tcp_listener_t*   tcp_ln;
#ifdef XYLEM_ENABLE_TLS
    xylem_tls_listener_t*   tls_ln;
#endif
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    xylem_http_srv_opts_t   opts;
    xylem_http_gzip_opts_t  gzip_opts;
    size_t                  max_body_size;
    uint64_t                idle_timeout_ms;
    _Atomic bool            closed;
};

/* Per-connection context */
typedef struct {
    xylem_http_srv_t*  srv;
    _http_transport_t  transport;
} _http_conn_ctx_t;

/* Forward declarations */
static void _http_conn_coro(void* arg);

static void _http_accept_coro(void* arg) {
    xylem_http_srv_t* srv = arg;

    while (!atomic_load_explicit(&srv->closed, memory_order_acquire)) {
        void* conn = NULL;
        _http_transport_kind_t kind = HTTP_TRANSPORT_TCP;

#ifdef XYLEM_ENABLE_TLS
        if (srv->tls_ln) {
            conn = xylem_tls_accept(srv->tls_ln);
            kind = HTTP_TRANSPORT_TLS;
        } else
#endif
        {
            conn = xylem_tcp_accept(srv->tcp_ln);
            kind = HTTP_TRANSPORT_TCP;
        }

        if (!conn) break;

        _http_conn_ctx_t* ctx = calloc(1, sizeof(_http_conn_ctx_t));
        if (!ctx) {
            if (kind == HTTP_TRANSPORT_TCP)
                xylem_tcp_close(conn);
#ifdef XYLEM_ENABLE_TLS
            else
                xylem_tls_close(conn);
#endif
            continue;
        }
        ctx->srv = srv;
        ctx->transport.kind = kind;
        ctx->transport.conn = conn;
        runtime_spawn(_http_conn_coro, ctx);
    }
}

xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {

    xylem_http_srv_t* srv = calloc(1, sizeof(xylem_http_srv_t));
    if (!srv) return NULL;

    srv->handler  = handler;
    srv->userdata = userdata;
    if (opts) srv->opts = *opts;

    srv->max_body_size  = (opts && opts->max_body_size)
                          ? opts->max_body_size : HTTP_DEFAULT_MAX_BODY;
    srv->idle_timeout_ms = (opts && opts->idle_timeout_ms)
                           ? opts->idle_timeout_ms : HTTP_DEFAULT_IDLE_MS;

#ifdef XYLEM_ENABLE_TLS
    if (opts && opts->tls_ctx) {
        srv->tls_ln = xylem_tls_listen(host, port, opts->tls_ctx, NULL);
        if (!srv->tls_ln) { free(srv); return NULL; }
    } else
#endif
    {
        srv->tcp_ln = xylem_tcp_listen(host, port, NULL);
        if (!srv->tcp_ln) { free(srv); return NULL; }
    }

    runtime_spawn(_http_accept_coro, srv);
    return srv;
}

void xylem_http_close(xylem_http_srv_t* srv) {
    if (!srv) return;
    atomic_store_explicit(&srv->closed, true, memory_order_release);
#ifdef XYLEM_ENABLE_TLS
    if (srv->tls_ln) xylem_tls_close_listener(srv->tls_ln);
#endif
    if (srv->tcp_ln) xylem_tcp_close_listener(srv->tcp_ln);
    free(srv);
}
```

- [ ] **Step 4: Add stub `_http_conn_coro` (empty for now, just close)**

```c
static void _http_conn_coro(void* arg) {
    _http_conn_ctx_t* ctx = arg;
    _transport_close(&ctx->transport);
    free(ctx);
}
```

- [ ] **Step 5: Update CMakeLists.txt — replace old source files**

Change the `if(XYLEM_ENABLE_HTTP)` block to:

```cmake
if(XYLEM_ENABLE_HTTP)
    list(APPEND SRCS
        src/net/http/http-common.c
        src/net/http/xylem-http.c
        src/net/http/xylem-http-common.c
        src/net/http/llhttp/llhttp.c
        src/net/http/llhttp/http.c
        src/net/http/llhttp/api.c
    )
endif()
```

Remove the TLS transport conditional entirely.

- [ ] **Step 6: Update `include/xylem.h` — replace old HTTP includes**

```c
#ifdef XYLEM_ENABLE_HTTP
#include "xylem/net/xylem-http.h"
#endif
```

- [ ] **Step 7: Build and run listen/close test**

Run: `cmake --build build --target test-http && ctest --test-dir build -R http`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add include/xylem/net/xylem-http.h src/net/http/xylem-http.c \
        CMakeLists.txt include/xylem.h tests/test-http.c
git commit -m "feat(http): server listen/close with accept coroutine"
```

---

### Task 3: Server Request Parsing

**Files:**
- Modify: `src/net/http/xylem-http.c`
- Modify: `tests/test-http.c`

- [ ] **Step 1: Write test — server receives GET request, handler sees method/url/headers**

```c
static xylem_http_srv_t* _test_srv;
static bool _handler_called;
static char _handler_method[16];
static char _handler_url[256];

static void _test_echo_handler(xylem_http_res_t* res, xylem_http_req_t* req, void* ud) {
    (void)ud;
    _handler_called = true;
    snprintf(_handler_method, sizeof(_handler_method), "%s", xylem_http_req_method(req));
    snprintf(_handler_url, sizeof(_handler_url), "%s", xylem_http_req_url(req));
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, "OK", 2);
}

static void _test_get_handler_main(void* arg) {
    (void)arg;
    _handler_called = false;
    _test_srv = xylem_http_listen("127.0.0.1", 0, _test_echo_handler, NULL, NULL);
    ASSERT(_test_srv != NULL);

    /* Determine assigned port (need listener addr helper or use fixed port) */
    /* Use a client GET to trigger the handler */
    xylem_http_res_t* res = xylem_http_get("http://127.0.0.1:<port>/hello", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(_handler_called);
    ASSERT(strcmp(_handler_method, "GET") == 0);
    ASSERT(strcmp(_handler_url, "/hello") == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(_test_srv);
    xylem_shutdown();
}
```

Note: The test will need to know the server port. Add `xylem_http_srv_port()` accessor or use a fixed port. Use a fixed test port (e.g., 18090) for simplicity:

```c
#define HTTP_TEST_PORT 18090

static void _test_get_handler_main(void* arg) {
    (void)arg;
    _handler_called = false;
    _test_srv = xylem_http_listen("127.0.0.1", HTTP_TEST_PORT,
                                  _test_echo_handler, NULL, NULL);
    ASSERT(_test_srv != NULL);

    xylem_http_res_t* res = xylem_http_get("http://127.0.0.1:18090/hello", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(_handler_called);
    ASSERT(strcmp(_handler_method, "GET") == 0);
    ASSERT(strcmp(_handler_url, "/hello") == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(_test_srv);
    xylem_shutdown();
}

static void test_get_handler(void) {
    xylem_run(_test_get_handler_main, NULL, NULL);
}
```

- [ ] **Step 2: Implement `_http_conn_coro` with llhttp parsing loop**

Replace the stub with full parsing logic. The connection coroutine:
1. Creates a read buffer (4096 bytes)
2. Loops: read bytes → feed to llhttp → on message_complete, call handler
3. Handle keep-alive (re-parse) or close

Key implementation pattern — use direct `_transport_read` instead of `xylem_reader_t` for request parsing (llhttp needs incremental feeding, not line-buffered reads):

```c
static void _http_req_reset(xylem_http_req_t* req) {
    free(req->url);
    http_headers_free(req->headers, req->header_count);
    free(req->body);
    http_headers_free(req->params, req->param_count);
    memset(req, 0, sizeof(*req));
}

/* llhttp callbacks — same logic as old server, adapted for new structs */
typedef struct {
    xylem_http_req_t  req;
    char*             cur_header_name;
    size_t            cur_header_name_len;
    bool              keep_alive;
    bool              expect_continue;
    bool              message_complete;
    size_t            max_body_size;
    _http_transport_t* transport;
} _http_parse_ctx_t;

/* ... llhttp callback implementations (on_url, on_header_field, etc.) ... */
/* These are carried over from xylem-http-server.c with minimal changes */

static void _http_conn_coro(void* arg) {
    _http_conn_ctx_t* ctx = arg;
    xylem_http_srv_t* srv = ctx->srv;

    llhttp_t parser;
    llhttp_settings_t settings;
    _http_parse_ctx_t parse_ctx = {0};
    parse_ctx.max_body_size = srv->max_body_size;
    parse_ctx.transport = &ctx->transport;

    /* Init llhttp callbacks */
    llhttp_settings_init(&settings);
    settings.on_url              = _on_url_cb;
    settings.on_method_complete  = _on_method_complete_cb;
    settings.on_header_field     = _on_header_field_cb;
    settings.on_header_value     = _on_header_value_cb;
    settings.on_headers_complete = _on_headers_complete_cb;
    settings.on_body             = _on_body_cb;
    settings.on_message_complete = _on_message_complete_cb;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = &parse_ctx;

    uint8_t buf[4096];

    while (!atomic_load_explicit(&srv->closed, memory_order_acquire)) {
        /* Set idle deadline */
        if (srv->idle_timeout_ms > 0) {
            uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                              + srv->idle_timeout_ms;
            _transport_set_read_deadline(&ctx->transport, deadline);
        }

        int n = _transport_read(&ctx->transport, buf, sizeof(buf));
        if (n <= 0) break;

        /* Clear read deadline after successful read */
        _transport_set_read_deadline(&ctx->transport, 0);

        const char* p = (const char*)buf;
        int remaining = n;

        while (remaining > 0) {
            parse_ctx.message_complete = false;
            enum llhttp_errno err = llhttp_execute(&parser, p, remaining);

            if (err == HPE_PAUSED) {
                size_t consumed = (size_t)(llhttp_get_error_pos(&parser) - p);
                p += consumed;
                remaining -= (int)consumed;

                /* Request complete — invoke handler */
                xylem_http_res_t res = {0};
                res.status_code = 200;
                res.transport = &ctx->transport;

                if (srv->handler) {
                    srv->handler(&res, &parse_ctx.req, srv->userdata);
                }

                /* Auto-finalize */
                _http_res_finalize(&res);
                _http_req_reset(&parse_ctx.req);

                if (!parse_ctx.keep_alive) goto done;

                llhttp_resume(&parser);
                llhttp_init(&parser, HTTP_REQUEST, &settings);
                parser.data = &parse_ctx;
                continue;
            }

            if (err != HPE_OK) {
                /* Parse error — send 400 and close */
                _transport_write(&ctx->transport,
                    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n", 71);
                goto done;
            }

            remaining = 0;
        }
    }

done:
    free(parse_ctx.cur_header_name);
    _http_req_reset(&parse_ctx.req);
    _transport_close(&ctx->transport);
    free(ctx);
}
```

- [ ] **Step 3: Implement request accessor functions**

```c
const char* xylem_http_req_method(const xylem_http_req_t* req) {
    return req ? req->method : NULL;
}

const char* xylem_http_req_url(const xylem_http_req_t* req) {
    return req ? req->url : NULL;
}

const char* xylem_http_req_header(const xylem_http_req_t* req, const char* name) {
    if (!req || !name) return NULL;
    return http_header_find(req->headers, req->header_count, name);
}

const void* xylem_http_req_body(const xylem_http_req_t* req) {
    return req ? req->body : NULL;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    return req ? req->body_len : 0;
}

const char* xylem_http_req_param(const xylem_http_req_t* req, const char* name) {
    if (!req || !name) return NULL;
    for (size_t i = 0; i < req->param_count; i++) {
        if (http_header_eq(req->params[i].name, name))
            return req->params[i].value;
    }
    return NULL;
}
```

- [ ] **Step 4: Build and verify test compiles (client not yet implemented, test deferred)**

Run: `cmake --build build --target xylem`
Expected: compiles without error

- [ ] **Step 5: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): server request parsing with llhttp in conn coroutine"
```

---

### Task 4: Server Response Writing

**Files:**
- Modify: `src/net/http/xylem-http.c`

- [ ] **Step 1: Implement `xylem_http_res_set_status`, `xylem_http_res_set_header`, `xylem_http_res_write`**

Carry over logic from old `xylem-http-server.c` (`_http_srv_send`, flush headers on first write, chunked encoding):

```c
int xylem_http_res_set_status(xylem_http_res_t* res, int code) {
    if (!res || res->headers_sent) return -1;
    res->status_code = code;
    return 0;
}

int xylem_http_res_set_header(xylem_http_res_t* res,
                              const char* name, const char* value) {
    if (!res || !name || !value || res->headers_sent) return -1;
    /* Grow array, store copies */
    if (res->resp_header_count >= res->resp_header_cap) {
        size_t new_cap = res->resp_header_cap ? res->resp_header_cap * 2 : 8;
        xylem_http_hdr_t* tmp = realloc(res->resp_headers,
                                        new_cap * sizeof(xylem_http_hdr_t));
        if (!tmp) return -1;
        res->resp_headers = tmp;
        res->resp_header_cap = new_cap;
    }
    /* Replace existing header with same name */
    for (size_t i = 0; i < res->resp_header_count; i++) {
        if (http_header_eq(res->resp_headers[i].name, name)) {
            free((char*)res->resp_headers[i].value);
            res->resp_headers[i].value = strdup(value);
            return 0;
        }
    }
    res->resp_headers[res->resp_header_count].name = strdup(name);
    res->resp_headers[res->resp_header_count].value = strdup(value);
    res->resp_header_count++;
    return 0;
}

static int _http_res_flush_headers(xylem_http_res_t* res) {
    /* Build status line + headers + CRLFCRLF, write to transport */
    /* Use Transfer-Encoding: chunked unless Content-Length was set */
    /* ... (carried from old _http_srv_flush_resp_headers logic) ... */
    res->headers_sent = true;
    res->chunked_active = true; /* unless CL mode */
    return 0;
}

int xylem_http_res_write(xylem_http_res_t* res, const void* data, size_t len) {
    if (!res || !res->transport || res->closed) return -1;
    if (len == 0) return 0;

    if (!res->headers_sent) {
        if (_http_res_flush_headers(res) != 0) return -1;
    }

    if (res->chunked_active) {
        /* Write chunk: hex-len CRLF data CRLF */
        char hdr[32];
        int hdr_len = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
        if (_transport_write(res->transport, hdr, hdr_len) != 0) return -1;
        if (_transport_write(res->transport, data, (int)len) != 0) return -1;
        if (_transport_write(res->transport, "\r\n", 2) != 0) return -1;
    } else {
        if (_transport_write(res->transport, data, (int)len) != 0) return -1;
    }
    return 0;
}

void xylem_http_res_close(xylem_http_res_t* res) {
    if (!res || !res->transport) return;
    res->closed = true;
    _transport_close(res->transport);
}

static void _http_res_finalize(xylem_http_res_t* res) {
    if (!res->transport || res->closed) return;

    /* If nothing was written, send empty 200 */
    if (!res->headers_sent) {
        _http_res_flush_headers(res);
    }

    /* Finalize gzip if active */
    if (res->gzip_active && res->gzip_stream) {
        /* flush MZ_FINISH ... */
    }

    /* Send chunked terminator */
    if (res->chunked_active) {
        _transport_write(res->transport, "0\r\n\r\n", 5);
        res->chunked_active = false;
    }

    /* Free response header copies */
    for (size_t i = 0; i < res->resp_header_count; i++) {
        free((char*)res->resp_headers[i].name);
        free((char*)res->resp_headers[i].value);
    }
    free(res->resp_headers);
    res->resp_headers = NULL;
    res->resp_header_count = 0;
}
```

- [ ] **Step 2: Implement `xylem_http_res_upgrade`**

```c
int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport) {
    if (!res || !res->transport || !transport) return -1;
    if (res->headers_sent) return -1;

    /* Send 101 Switching Protocols */
    const char* resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
    if (_transport_write(res->transport, resp, (int)strlen(resp)) != 0)
        return -1;

    *transport = res->transport->conn;
    res->upgrade_accepted = true;
    res->transport = NULL;
    return 0;
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build --target xylem`
Expected: compiles

- [ ] **Step 4: Commit**

```bash
git add src/net/http/xylem-http.c
git commit -m "feat(http): server response writer with chunked encoding"
```

---

### Task 5: Client Core — Request/Response

**Files:**
- Modify: `src/net/http/xylem-http.c`

- [ ] **Step 1: Write test — client GET to echo server**

```c
static void _test_client_echo_handler(xylem_http_res_t* res,
                                      xylem_http_req_t* req, void* ud) {
    (void)ud;
    const void* body = xylem_http_req_body(req);
    size_t len = xylem_http_req_body_len(req);
    xylem_http_res_set_status(res, 200);
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    if (body && len > 0) {
        xylem_http_res_write(res, body, len);
    } else {
        xylem_http_res_write(res, "hello", 5);
    }
}

static void _test_client_get_main(void* arg) {
    (void)arg;
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 18091, _test_client_echo_handler, NULL, NULL);
    ASSERT(srv != NULL);

    xylem_http_res_t* res = xylem_http_get("http://127.0.0.1:18091/test", NULL);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(xylem_http_res_body_len(res) == 5);
    ASSERT(memcmp(xylem_http_res_body(res), "hello", 5) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_client_get(void) {
    xylem_run(_test_client_get_main, NULL, NULL);
}
```

- [ ] **Step 2: Implement client `xylem_http_get` and supporting functions**

Core client flow:
1. Parse URL
2. Dial TCP (or TLS for https)
3. Serialize HTTP request using `http_req_serialize()` from http-common.h
4. Write request via `_transport_write`
5. Read response via `_transport_read` + llhttp (response mode)
6. Return `xylem_http_res_t*`

```c
/* Client response read accessors */
int xylem_http_res_status(const xylem_http_res_t* res) {
    return res ? res->status_code : 0;
}

const char* xylem_http_res_header(const xylem_http_res_t* res, const char* name) {
    if (!res || !name) return NULL;
    return http_header_find(res->headers, res->header_count, name);
}

const void* xylem_http_res_body(const xylem_http_res_t* res) {
    return res ? res->body : NULL;
}

size_t xylem_http_res_body_len(const xylem_http_res_t* res) {
    return res ? res->body_len : 0;
}

void xylem_http_res_destroy(xylem_http_res_t* res) {
    if (!res) return;
    http_headers_free(res->headers, res->header_count);
    free(res->body);
    /* Free server-write state if any */
    for (size_t i = 0; i < res->resp_header_count; i++) {
        free((char*)res->resp_headers[i].name);
        free((char*)res->resp_headers[i].value);
    }
    free(res->resp_headers);
    if (res->gzip_stream) { mz_deflateEnd(res->gzip_stream); free(res->gzip_stream); }
    free(res);
}

#define HTTP_DEFAULT_TIMEOUT_MS  30000
#define HTTP_DEFAULT_MAX_BODY    (10 * 1024 * 1024)

static xylem_http_res_t* _http_do_request(
    const char* method, const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_opts_t* opts) {

    http_url_t parsed;
    if (http_url_parse(url, &parsed) != 0) return NULL;

    uint64_t timeout_ms = (opts && opts->timeout_ms)
                          ? opts->timeout_ms : HTTP_DEFAULT_TIMEOUT_MS;
    size_t max_body = (opts && opts->max_body_size)
                      ? opts->max_body_size : HTTP_DEFAULT_MAX_BODY;

    /* Dial connection */
    _http_transport_t transport = {0};
    if (strcmp(parsed.scheme, "https") == 0) {
#ifdef XYLEM_ENABLE_TLS
        xylem_tls_ctx_t* ctx = (opts && opts->tls_ctx)
                               ? opts->tls_ctx : NULL;
        if (!ctx) { ctx = xylem_tls_ctx_create(); /* default */ }
        xylem_tls_opts_t tls_opts = { .server_name = parsed.host };
        transport.conn = xylem_tls_dial(parsed.host, parsed.port, ctx, &tls_opts);
        transport.kind = HTTP_TRANSPORT_TLS;
        if (!opts || !opts->tls_ctx) xylem_tls_ctx_destroy(ctx);
#else
        return NULL;
#endif
    } else {
        transport.conn = xylem_tcp_dial(parsed.host, parsed.port, timeout_ms, NULL);
        transport.kind = HTTP_TRANSPORT_TCP;
    }
    if (!transport.conn) return NULL;

    /* Set deadline */
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    _transport_set_read_deadline(&transport, deadline);
    _transport_set_write_deadline(&transport, deadline);

    /* Serialize and send request */
    size_t req_len;
    char* req_buf = http_req_serialize(method, &parsed, body, body_len,
                                       content_type, false, &req_len,
                                       opts ? opts->headers : NULL,
                                       opts ? opts->header_count : 0);
    if (!req_buf) { _transport_close(&transport); return NULL; }

    int wrc = _transport_write(&transport, req_buf, (int)req_len);
    free(req_buf);
    if (wrc != 0) { _transport_close(&transport); return NULL; }

    /* Read and parse response */
    xylem_http_res_t* res = calloc(1, sizeof(xylem_http_res_t));
    if (!res) { _transport_close(&transport); return NULL; }

    /* Set up llhttp in response mode */
    llhttp_t parser;
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_header_field     = _http_cli_header_field_cb;
    settings.on_header_value     = _http_cli_header_value_cb;
    settings.on_headers_complete = _http_cli_headers_complete_cb;
    settings.on_body             = _http_cli_body_cb;
    settings.on_message_complete = _http_cli_message_complete_cb;

    typedef struct { xylem_http_res_t* res; char* cur_name; size_t cur_name_len;
                     bool done; size_t max_body; } _cli_parse_t;
    _cli_parse_t pctx = { .res = res, .max_body = max_body };
    llhttp_init(&parser, HTTP_RESPONSE, &settings);
    parser.data = &pctx;

    uint8_t buf[4096];
    while (!pctx.done) {
        int n = _transport_read(&transport, buf, sizeof(buf));
        if (n <= 0) break;
        enum llhttp_errno err = llhttp_execute(&parser, (char*)buf, n);
        if (err != HPE_OK && err != HPE_PAUSED) {
            free(pctx.cur_name);
            xylem_http_res_destroy(res);
            _transport_close(&transport);
            return NULL;
        }
    }
    free(pctx.cur_name);

    _transport_close(&transport);

    /* Auto-decompress gzip */
    /* ... (carry from old client) ... */

    return res;
}

xylem_http_res_t* xylem_http_get(const char* url, const xylem_http_opts_t* opts) {
    return _http_do_request("GET", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_post(const char* url, const void* body,
                                  size_t body_len, const char* content_type,
                                  const xylem_http_opts_t* opts) {
    return _http_do_request("POST", url, body, body_len, content_type, opts);
}

xylem_http_res_t* xylem_http_put(const char* url, const void* body,
                                 size_t body_len, const char* content_type,
                                 const xylem_http_opts_t* opts) {
    return _http_do_request("PUT", url, body, body_len, content_type, opts);
}

xylem_http_res_t* xylem_http_delete(const char* url, const xylem_http_opts_t* opts) {
    return _http_do_request("DELETE", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_patch(const char* url, const void* body,
                                   size_t body_len, const char* content_type,
                                   const xylem_http_opts_t* opts) {
    return _http_do_request("PATCH", url, body, body_len, content_type, opts);
}
```

- [ ] **Step 3: Run test**

Run: `cmake --build build --target test-http && ctest --test-dir build -R http`
Expected: test_client_get PASS

- [ ] **Step 4: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): client request/response with coroutine I/O"
```

---

### Task 6: Client Connection Pool

**Files:**
- Modify: `src/net/http/xylem-http.c`
- Modify: `tests/test-http.c`

- [ ] **Step 1: Write test — two sequential requests reuse connection**

```c
static int _test_pool_handler_count;

static void _test_pool_handler(xylem_http_res_t* res, xylem_http_req_t* req, void* ud) {
    (void)req; (void)ud;
    _test_pool_handler_count++;
    xylem_http_res_set_status(res, 200);
    xylem_http_res_write(res, "ok", 2);
}

static void _test_pool_main(void* arg) {
    (void)arg;
    _test_pool_handler_count = 0;
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 18092, _test_pool_handler, NULL, NULL);
    ASSERT(srv != NULL);

    xylem_http_res_t* r1 = xylem_http_get("http://127.0.0.1:18092/a", NULL);
    ASSERT(r1 != NULL);
    xylem_http_res_destroy(r1);

    xylem_http_res_t* r2 = xylem_http_get("http://127.0.0.1:18092/b", NULL);
    ASSERT(r2 != NULL);
    xylem_http_res_destroy(r2);

    ASSERT(_test_pool_handler_count == 2);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_connection_pool(void) {
    xylem_run(_test_pool_main, NULL, NULL);
}
```

- [ ] **Step 2: Implement internal connection pool**

Use a simple structure: rbtree keyed by `host:port:scheme`, each entry holds a list of idle `_http_transport_t`. Acquire checks pool first, release returns conn to pool if keep-alive.

```c
#include "xylem/container/xylem-rbtree.h"
#include "xylem/container/xylem-list.h"

typedef struct {
    _http_transport_t transport;
    uint64_t          idle_since;
    xylem_list_node_t node;
} _pool_idle_conn_t;

typedef struct {
    char              key[320];
    xylem_list_t      idle;
    size_t            idle_count;
} _pool_entry_t;

static struct {
    bool           inited;
    xylem_rbtree_t tree;
    size_t         max_idle_per_host;
    uint64_t       idle_timeout_ms;
} _http_pool;

static void _pool_init(void) { /* lazy init */ }
static _http_transport_t* _pool_acquire(const http_url_t* url) { /* ... */ }
static void _pool_release(_http_transport_t* t, const http_url_t* url) { /* ... */ }
```

Modify `_http_do_request` to use `_pool_acquire` / `_pool_release` instead of always dialing fresh and closing.

- [ ] **Step 3: Run test**

Run: `cmake --build build --target test-http && ctest --test-dir build -R http`
Expected: PASS (pool reuses connection, second request handled on same conn)

- [ ] **Step 4: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): transparent internal connection pool"
```

---

### Task 7: Router and Middleware

**Files:**
- Modify: `src/net/http/xylem-http.c`
- Modify: `tests/test-http.c`

- [ ] **Step 1: Write test — router dispatches by method + path**

```c
static void _test_router_get(xylem_http_res_t* res, xylem_http_req_t* req, void* ud) {
    (void)req; (void)ud;
    xylem_http_res_write(res, "get", 3);
}
static void _test_router_post(xylem_http_res_t* res, xylem_http_req_t* req, void* ud) {
    (void)req; (void)ud;
    xylem_http_res_write(res, "post", 4);
}

static void _test_router_dispatch_handler(xylem_http_res_t* res,
                                          xylem_http_req_t* req, void* ud) {
    xylem_http_router_t* r = ud;
    xylem_http_router_dispatch(r, res, req);
}

static void _test_router_main(void* arg) {
    (void)arg;
    xylem_http_router_t* r = xylem_http_router_create();
    ASSERT(r != NULL);
    ASSERT(xylem_http_router_add(r, "GET", "/api", _test_router_get, NULL) == 0);
    ASSERT(xylem_http_router_add(r, "POST", "/api", _test_router_post, NULL) == 0);

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 18093, _test_router_dispatch_handler, r, NULL);
    ASSERT(srv != NULL);

    xylem_http_res_t* res = xylem_http_get("http://127.0.0.1:18093/api", NULL);
    ASSERT(res != NULL);
    ASSERT(memcmp(xylem_http_res_body(res), "get", 3) == 0);
    xylem_http_res_destroy(res);

    xylem_http_res_t* res2 = xylem_http_post("http://127.0.0.1:18093/api",
                                             "x", 1, "text/plain", NULL);
    ASSERT(res2 != NULL);
    ASSERT(memcmp(xylem_http_res_body(res2), "post", 4) == 0);
    xylem_http_res_destroy(res2);

    xylem_http_close(srv);
    xylem_http_router_destroy(r);
    xylem_shutdown();
}

static void test_router(void) {
    xylem_run(_test_router_main, NULL, NULL);
}
```

- [ ] **Step 2: Implement router (carry logic from old server, adapt handler signature)**

The router implementation (radix tree matching, path params, wildcard) is carried from `xylem-http-server.c`. Key changes:
- Handler signature: `(xylem_http_res_t*, xylem_http_req_t*, void*)`
- Middleware signature: `int (xylem_http_res_t*, xylem_http_req_t*, void*)`
- `xylem_http_router_dispatch` runs middleware chain then matched handler

```c
typedef struct {
    char*                       method;
    char*                       pattern;
    xylem_http_handler_fn_t     handler;
    void*                       userdata;
} _route_entry_t;

typedef struct {
    xylem_http_middleware_fn_t  fn;
    void*                       userdata;
} _middleware_entry_t;

struct xylem_http_router_s {
    _route_entry_t*      routes;
    size_t               route_count;
    size_t               route_cap;
    _middleware_entry_t* middlewares;
    size_t               mw_count;
    size_t               mw_cap;
};

/* ... create, destroy, add, use, dispatch implementations ... */
/* dispatch sets req->params for :name segments */
```

- [ ] **Step 3: Run test**

Run: `cmake --build build --target test-http && ctest --test-dir build -R http`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): router and middleware with coroutine handler signature"
```

---

### Task 8: Gzip Compression (Server) and Auto-Decompress (Client)

**Files:**
- Modify: `src/net/http/xylem-http.c`
- Modify: `tests/test-http.c`

- [ ] **Step 1: Write test — server gzip compresses, client auto-decompresses**

```c
static void _test_gzip_handler(xylem_http_res_t* res, xylem_http_req_t* req, void* ud) {
    (void)req; (void)ud;
    xylem_http_res_set_header(res, "Content-Type", "text/plain");
    const char* body = "hello gzip world, this is a test string that should compress";
    xylem_http_res_write(res, body, strlen(body));
}

static void _test_gzip_main(void* arg) {
    (void)arg;
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 18094, _test_gzip_handler, NULL, NULL);
    ASSERT(srv != NULL);
    xylem_http_gzip_opts_t gzip = { .enabled = true, .min_size = 1 };
    xylem_http_srv_set_gzip(srv, &gzip);

    xylem_http_hdr_t hdr = { "Accept-Encoding", "gzip" };
    xylem_http_opts_t opts = { .headers = &hdr, .header_count = 1 };
    xylem_http_res_t* res = xylem_http_get("http://127.0.0.1:18094/", &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    /* Client auto-decompresses, so body should be original text */
    const char* expected = "hello gzip world, this is a test string that should compress";
    ASSERT(xylem_http_res_body_len(res) == strlen(expected));
    ASSERT(memcmp(xylem_http_res_body(res), expected, strlen(expected)) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_gzip(void) {
    xylem_run(_test_gzip_main, NULL, NULL);
}
```

- [ ] **Step 2: Implement server gzip in `xylem_http_res_write`**

When gzip is enabled on the server and the client sent `Accept-Encoding: gzip`:
- On first `write`, check if Content-Type matches gzip MIME types
- If yes, init `mz_stream`, set `Content-Encoding: gzip`, stream-compress each write chunk
- On finalize, flush with `MZ_FINISH`

Carry compression logic from old `xylem-http-server.c`.

- [ ] **Step 3: Implement client auto-decompress**

After response is fully read, check `Content-Encoding: gzip` header. If present, decompress body in-place using `xylem_gzip_decompress`. Carry from old client.

- [ ] **Step 4: Run test**

Run: `cmake --build build --target test-http && ctest --test-dir build -R http`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): server gzip compression and client auto-decompress"
```

---

### Task 9: Static File Server

**Files:**
- Modify: `src/net/http/xylem-http.c`
- Modify: `tests/test-http.c`

- [ ] **Step 1: Write test — serve a file from disk**

```c
static void _test_static_main(void* arg) {
    (void)arg;
    xylem_http_router_t* r = xylem_http_router_create();
    xylem_http_static_opts_t opts = { .root = "." };
    ASSERT(xylem_http_static_serve(r, "/static/*", &opts) == 0);

    /* Write a temp file for the test */
    FILE* f = fopen("_test_static.txt", "w");
    ASSERT(f != NULL);
    fprintf(f, "static content");
    fclose(f);

    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 18095,
        (xylem_http_handler_fn_t)xylem_http_router_dispatch, r, NULL);
    /* Note: won't work directly — need a wrapper handler. See router test pattern. */

    /* ... cleanup ... */
}
```

- [ ] **Step 2: Implement static file handler**

Carry logic from old `xylem-http-server.c` static file implementation:
- Path traversal prevention
- MIME type detection
- Cache-Control headers
- Precompressed .gz file lookup
- GET/HEAD support, 405 for others

Adapt file reading to use `runtime_submit()` for blocking file I/O on the dynpool.

- [ ] **Step 3: Run test**

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): static file server with path traversal protection"
```

---

### Task 10: Cookie Jar and Redirect Following

**Files:**
- Modify: `src/net/http/xylem-http.c`
- Modify: `tests/test-http.c`

- [ ] **Step 1: Write test — redirect follow**

```c
static void _test_redirect_handler(xylem_http_res_t* res,
                                   xylem_http_req_t* req, void* ud) {
    (void)ud;
    if (strcmp(xylem_http_req_url(req), "/old") == 0) {
        xylem_http_res_set_status(res, 301);
        xylem_http_res_set_header(res, "Location", "/new");
        xylem_http_res_write(res, "", 0);
    } else {
        xylem_http_res_write(res, "arrived", 7);
    }
}

static void _test_redirect_main(void* arg) {
    (void)arg;
    xylem_http_srv_t* srv = xylem_http_listen(
        "127.0.0.1", 18096, _test_redirect_handler, NULL, NULL);
    ASSERT(srv != NULL);

    xylem_http_opts_t opts = { .max_redirects = 3 };
    xylem_http_res_t* res = xylem_http_get("http://127.0.0.1:18096/old", &opts);
    ASSERT(res != NULL);
    ASSERT(xylem_http_res_status(res) == 200);
    ASSERT(memcmp(xylem_http_res_body(res), "arrived", 7) == 0);
    xylem_http_res_destroy(res);

    xylem_http_close(srv);
    xylem_shutdown();
}

static void test_redirect(void) {
    xylem_run(_test_redirect_main, NULL, NULL);
}
```

- [ ] **Step 2: Implement redirect following in `_http_do_request`**

After receiving response, if status is 301/302/303/307/308 and `max_redirects > 0`, extract `Location` header, resolve relative URL, and re-issue request (decrement counter).

- [ ] **Step 3: Carry cookie jar implementation from old client**

The cookie jar struct and logic (parse Set-Cookie, domain/path matching, build Cookie header) are carried verbatim from `xylem-http-client.c`. Integrate into `_http_do_request`: before sending, build Cookie header from jar; after response, collect Set-Cookie headers into jar.

- [ ] **Step 4: Run tests**

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/net/http/xylem-http.c tests/test-http.c
git commit -m "feat(http): client redirect follow and cookie jar"
```

---

### Task 11: Remove Old Files and Final Cleanup

**Files:**
- Delete: `include/xylem/net/http/xylem-http-client.h`
- Delete: `include/xylem/net/http/xylem-http-server.h`
- Delete: `include/xylem/net/http/xylem-http-common.h`
- Delete: `src/net/http/http-transport.h`
- Delete: `src/net/http/http-transport-tcp.c`
- Delete: `src/net/http/http-transport-tls.c`
- Delete: `src/net/http/http-transport-tls-stub.c`
- Delete: `src/net/http/xylem-http-client.c`
- Delete: `src/net/http/xylem-http-server.c`
- Modify: `src/net/http/http-common.h` (remove include of deleted header)
- Modify: `tests/test-http.c` (remove old test includes, update main)

- [ ] **Step 1: Delete old files**

```bash
git rm include/xylem/net/http/xylem-http-client.h
git rm include/xylem/net/http/xylem-http-server.h
git rm include/xylem/net/http/xylem-http-common.h
git rm src/net/http/http-transport.h
git rm src/net/http/http-transport-tcp.c
git rm src/net/http/http-transport-tls.c
git rm src/net/http/http-transport-tls-stub.c
git rm src/net/http/xylem-http-client.c
git rm src/net/http/xylem-http-server.c
```

- [ ] **Step 2: Update `src/net/http/http-common.h` include**

Change:
```c
#include "xylem/net/http/xylem-http-common.h"
```
To:
```c
#include "xylem/net/xylem-http.h"
```

- [ ] **Step 3: Update `src/net/http/xylem-http-common.c` include**

Ensure it includes `xylem/net/xylem-http.h` instead of the old common header.

- [ ] **Step 4: Update `tests/test-http.c` — clean up includes and main()**

```c
#include "xylem.h"
#include "assert.h"
#include <stdlib.h>
#include <string.h>

/* ... all test functions ... */

int main(void) {
    /* Unit tests (no runtime needed) */
    test_url_encode_unreserved();
    test_url_encode_reserved();
    test_url_encode_empty();
    test_url_decode_basic();
    test_url_decode_passthrough();
    test_url_encode_decode_round_trip();
    test_res_destroy_null();
    test_cookie_jar_create_destroy();
    test_cookie_jar_destroy_null();

    /* Integration tests (run in runtime) */
    test_listen_close();
    test_client_get();
    test_connection_pool();
    test_router();
    test_gzip();
    test_redirect();

    return 0;
}
```

- [ ] **Step 5: Full build and test run**

Run: `cmake --build build && ctest --test-dir build -R http`
Expected: all tests PASS, no link errors

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(http): remove old callback-based implementation"
```

---

### Task 12: SSE, Multipart, CORS, URL Encode/Decode Integration

**Files:**
- Modify: `src/net/http/xylem-http-common.c` (update includes only)
- Modify: `tests/test-http.c`

- [ ] **Step 1: Verify existing utility functions still compile**

The implementations in `xylem-http-common.c` (URL encode/decode, CORS, multipart, SSE) should need only an include path change from `xylem/net/http/xylem-http-common.h` to `xylem/net/xylem-http.h`.

- [ ] **Step 2: Run full test suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: ALL tests pass (not just HTTP — verify no regressions in WS or other modules that import HTTP headers)

- [ ] **Step 3: Fix any remaining references to old headers in WS module**

Check `src/net/ws/` for includes of old HTTP headers. The WS transport files (`ws-transport-tcp.c`, `ws-transport-tls.c`) likely reference `http-transport.h` — these need to be updated if WS still uses the old callback model, or left alone if WS has its own transport abstraction.

```bash
grep -r "http-transport\|xylem-http-client\|xylem-http-server\|xylem-http-common" src/ include/
```

Fix all remaining references.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "fix(http): update all internal references to new unified header"
```

---

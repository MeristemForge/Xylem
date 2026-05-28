# WebSocket Coroutine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the callback-based WS/WSS implementation with a coroutine-based API built on `http_transport_t`.

**Architecture:** Coroutine WS reuses the existing `http_transport_t` vtable (read/write/close/deadlines) and the pure-computation modules (ws-frame, ws-handshake, ws-utf8). Server-side connects via HTTP upgrade; client-side performs the upgrade handshake directly. Each WS connection is used within a single coroutine — `recv` parks on transport reads, `send` writes through the transport.

**Tech Stack:** C11, minicoro coroutines, xylem runtime (scheduler + iowait), llhttp (for upgrade detection), OpenSSL (optional TLS)

---

## File Structure

| File | Responsibility |
|------|---------------|
| `include/xylem/net/xylem-ws.h` | Public WS API (types, dial, listen, send, recv, close) |
| `include/xylem/net/xylem-wss.h` | Public WSS API (wss_dial, wss_listen, wss_accept) |
| `src/net/ws/ws.h` | Internal `xylem_ws_conn_s` struct + shared function declarations |
| `src/net/ws/ws.c` | Core logic: accept_impl, recv loop, send, close handshake, ping |
| `src/net/ws/xylem-ws.c` | Plain WS entries: dial (TCP), listen (TCP), accept |
| `src/net/ws/xylem-wss.c` | WSS entries: dial (TLS), listen (TLS), accept |
| `src/net/ws/ws-frame.h` | Kept: frame header encode/decode, mask |
| `src/net/ws/ws-frame.c` | Kept: frame implementation |
| `src/net/ws/ws-handshake.h` | Kept: key gen, accept compute, request/response build |
| `src/net/ws/ws-handshake.c` | Kept: handshake implementation |
| `src/net/ws/ws-utf8.h` | Kept: UTF-8 validation |
| `src/net/ws/ws-utf8.c` | Kept: UTF-8 implementation |
| `src/net/http/http.h` | Modified: add on_upgrade + upgrade_userdata to http_srv_t |
| `src/net/http/http.c` | Modified: implement xylem_http_res_upgrade, upgrade dispatch |
| `CMakeLists.txt` | Modified: update WS source list |
| `tests/test-ws.c` | Rewritten: coroutine-based integration tests |

---

### Task 1: Implement `xylem_http_res_upgrade` (Prerequisite)

**Files:**
- Modify: `src/net/http/http.h`
- Modify: `src/net/http/http.c`
- Modify: `src/net/http/xylem-http.c`
- Modify: `src/net/http/xylem-https.c`

The WS accept path needs `xylem_http_res_upgrade()` to send 101 and detach the transport. Also need to wire the `on_upgrade` field from `xylem_http_srv_opts_t` into the server coroutine.

- [ ] **Step 1: Add on_upgrade fields to http_srv_t**

In `src/net/http/http.h`, add upgrade handler fields to `http_srv_t`:

```c
typedef struct http_srv_s {
    void*                   listener;
    void                    (*close_listener)(void* listener);
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    uint16_t                port;
    xylem_http_gzip_opts_t  gzip_opts;
    xylem_http_handler_fn_t on_upgrade;
    void*                   upgrade_userdata;
} http_srv_t;
```

- [ ] **Step 2: Wire opts into xylem_http_listen**

In `src/net/http/xylem-http.c`, update `xylem_http_listen` to store opts fields:

```c
xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {

    if (!handler && !(opts && opts->on_upgrade)) {
        return NULL;
    }

    xylem_tcp_listener_t* ln = xylem_tcp_listen(host, port, NULL);
    if (!ln) {
        return NULL;
    }

    http_srv_t* srv = (http_srv_t*)calloc(1, sizeof(*srv));
    if (!srv) {
        xylem_tcp_close_listener(ln);
        return NULL;
    }
    srv->listener       = ln;
    srv->close_listener = (void (*)(void*))xylem_tcp_close_listener;
    srv->handler        = handler;
    srv->userdata       = userdata;

    if (opts) {
        srv->on_upgrade       = opts->on_upgrade;
        srv->upgrade_userdata = opts->upgrade_userdata;
    }

    char host_buf[46];
    uint16_t actual_port = 0;
    xylem_tcp_listener_addr(ln, host_buf, sizeof(host_buf), &actual_port);
    srv->port = actual_port;

    runtime_spawn(_http_accept_coroutine, srv);

    return (xylem_http_srv_t*)srv;
}
```

- [ ] **Step 3: Do the same for xylem_https_listen**

In `src/net/http/xylem-https.c`, mirror the same change — store `opts->on_upgrade` and `opts->upgrade_userdata` in `http_srv_t`.

- [ ] **Step 4: Implement xylem_http_res_upgrade in http.c**

In `src/net/http/http.c`, add the implementation after the existing response functions:

```c
int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport) {
    if (!res || !res->_transport || !transport) return -1;
    if (res->_headers_sent) return -1;

    const char* resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n";
    int n = res->_transport->write(res->_transport->conn,
                                   resp, (int)strlen(resp));
    if (n < 0) return -1;

    /* Write any additional headers set by the caller */
    for (size_t i = 0; i < res->header_count; i++) {
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr), "%s: %s\r\n",
                            res->headers[i].name, res->headers[i].value);
        if (hlen > 0) {
            res->_transport->write(res->_transport->conn, hdr, hlen);
        }
    }

    /* End headers */
    res->_transport->write(res->_transport->conn, "\r\n", 2);

    /* Detach transport — caller now owns the connection */
    *transport = res->_transport;
    res->_transport = NULL;
    res->_headers_sent = true;

    return 0;
}
```

- [ ] **Step 5: Add upgrade dispatch in http_srv_conn_coroutine**

In `src/net/http/http.c`, modify `http_srv_conn_coroutine` (around line 696-714) to check for upgrade requests:

```c
        keep_alive = llhttp_should_keep_alive(&sp.parser) != 0;

        xylem_http_res_t res;
        memset(&res, 0, sizeof(res));
        res._transport = &ctx->transport;
        res.status_code = 200;

        const char* ae = http_header_find(sp.req.headers, sp.req.header_count,
                                          "Accept-Encoding");
        if (ae && strstr(ae, "gzip")) {
            res.accept_gzip = true;
        }
        if (ctx->srv->gzip_opts.enabled) {
            res._gzip_opts = &ctx->srv->gzip_opts;
        }

        /* Check for HTTP Upgrade */
        bool is_upgrade = llhttp_get_upgrade(&sp.parser) != 0;

        if (is_upgrade && ctx->srv->on_upgrade) {
            ctx->srv->on_upgrade(&res, &sp.req, ctx->srv->upgrade_userdata);
            http_headers_free(res.headers, res.header_count);
            _srv_parser_destroy(&sp);
            /* If upgrade succeeded, transport was detached — don't close */
            if (!res._transport) {
                free(ctx);
                return;
            }
            /* Upgrade handler didn't actually upgrade; close normally */
            _transport_close(&ctx->transport);
            free(ctx);
            return;
        }

        ctx->srv->handler(&res, &sp.req, ctx->srv->userdata);

        _finalize_response(&res);
```

- [ ] **Step 6: Build and verify**

Run:
```bash
cmake --build build --target xylem
```
Expected: compiles without errors.

- [ ] **Step 7: Commit**

```bash
git add src/net/http/http.h src/net/http/http.c src/net/http/xylem-http.c src/net/http/xylem-https.c
git commit -m "feat(http): implement xylem_http_res_upgrade and upgrade dispatch"
```

---

### Task 2: Delete Old Callback WS Files + Update CMake

**Files:**
- Delete: `include/xylem/net/ws/` (entire directory)
- Delete: `src/net/ws/ws-common.h`
- Delete: `src/net/ws/ws-common.c`
- Delete: `src/net/ws/ws-transport.h`
- Delete: `src/net/ws/ws-transport-tcp.c`
- Delete: `src/net/ws/ws-transport-tls.c`
- Delete: `src/net/ws/ws-transport-tls-stub.c`
- Delete: `src/net/ws/xylem-ws-client.c`
- Delete: `src/net/ws/xylem-ws-server.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Delete old files**

```bash
rm -rf include/xylem/net/ws/
rm -f src/net/ws/ws-common.h src/net/ws/ws-common.c
rm -f src/net/ws/ws-transport.h
rm -f src/net/ws/ws-transport-tcp.c src/net/ws/ws-transport-tls.c src/net/ws/ws-transport-tls-stub.c
rm -f src/net/ws/xylem-ws-client.c src/net/ws/xylem-ws-server.c
```

- [ ] **Step 2: Update CMakeLists.txt**

Replace the WS source block (lines 142-157):

```cmake
if(XYLEM_ENABLE_WS)
	list(APPEND SRCS
		src/net/ws/ws.c
		src/net/ws/xylem-ws.c
		src/net/ws/ws-utf8.c
		src/net/ws/ws-frame.c
		src/net/ws/ws-handshake.c
	)
	if(XYLEM_ENABLE_TLS)
		list(APPEND SRCS src/net/ws/xylem-wss.c)
	endif()
endif()
```

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "refactor(ws): remove callback-based WS implementation"
```

---

### Task 3: Create Public Headers

**Files:**
- Create: `include/xylem/net/xylem-ws.h`
- Create: `include/xylem/net/xylem-wss.h`

- [ ] **Step 1: Create xylem-ws.h**

```c
/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

_Pragma("once")

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_ws_conn_s     xylem_ws_conn_t;
typedef struct xylem_ws_listener_s xylem_ws_listener_t;

typedef enum {
    XYLEM_WS_TEXT   = 0x1,
    XYLEM_WS_BINARY = 0x2,
} xylem_ws_opcode_t;

typedef struct {
    xylem_ws_opcode_t opcode;
    void*             data;
    size_t            len;
} xylem_ws_msg_t;

typedef void (*xylem_ws_handler_fn_t)(xylem_ws_conn_t* conn, void* userdata);

typedef struct {
    size_t   max_msg_size;
    size_t   fragment_threshold;
    uint64_t handshake_timeout_ms;
    uint64_t close_timeout_ms;
} xylem_ws_opts_t;

/* --- Server: HTTP upgrade path --- */

struct xylem_http_res_s;
struct xylem_http_req_s;

extern xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                         struct xylem_http_req_s* req,
                                         const xylem_ws_opts_t* opts);

/* --- Server: standalone listener --- */

extern xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                             xylem_ws_handler_fn_t handler,
                                             void* userdata,
                                             const xylem_ws_opts_t* opts);
extern void     xylem_ws_close_listener(xylem_ws_listener_t* listener);
extern uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener);

/* --- Client --- */

extern xylem_ws_conn_t* xylem_ws_dial(const char* url,
                                       const xylem_ws_opts_t* opts);

/* --- Connection operations --- */

extern int  xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                          const void* data, size_t len);
extern int  xylem_ws_recv(xylem_ws_conn_t* conn, xylem_ws_msg_t* msg);
extern int  xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len);
extern int  xylem_ws_close(xylem_ws_conn_t* conn, uint16_t code,
                           const char* reason, size_t reason_len);
extern void xylem_ws_msg_free(xylem_ws_msg_t* msg);

/* --- Utilities --- */

extern uint16_t xylem_ws_close_code(xylem_ws_conn_t* conn);
extern void*    xylem_ws_get_userdata(xylem_ws_conn_t* conn);
extern void     xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
```

- [ ] **Step 2: Create xylem-wss.h**

```c
/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

_Pragma("once")

#include "xylem/net/xylem-ws.h"
#include "xylem/net/xylem-tls.h"

typedef struct xylem_wss_listener_s xylem_wss_listener_t;

/* --- Server: HTTPS upgrade path --- */

extern xylem_ws_conn_t* xylem_wss_accept(struct xylem_http_res_s* res,
                                          struct xylem_http_req_s* req,
                                          const xylem_ws_opts_t* opts);

/* --- Server: standalone listener --- */

extern xylem_wss_listener_t* xylem_wss_listen(const char* host, uint16_t port,
                                               xylem_ws_handler_fn_t handler,
                                               void* userdata,
                                               xylem_tls_ctx_t* tls_ctx,
                                               const xylem_ws_opts_t* opts);
extern void     xylem_wss_close_listener(xylem_wss_listener_t* listener);
extern uint16_t xylem_wss_listener_port(xylem_wss_listener_t* listener);

/* --- Client --- */

extern xylem_ws_conn_t* xylem_wss_dial(const char* url,
                                        xylem_tls_ctx_t* tls_ctx,
                                        const xylem_ws_opts_t* opts);
```

- [ ] **Step 3: Commit**

```bash
git add include/xylem/net/xylem-ws.h include/xylem/net/xylem-wss.h
git commit -m "feat(ws): add coroutine WS/WSS public headers"
```

---

### Task 4: Create Internal Header (ws.h)

**Files:**
- Create: `src/net/ws/ws.h`

- [ ] **Step 1: Create ws.h**

```c
_Pragma("once")

#include "net/http/http.h"
#include "xylem/net/xylem-ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_DEFAULT_MAX_MSG_SIZE       (16 * 1024 * 1024)
#define WS_DEFAULT_FRAGMENT_THRESHOLD (16 * 1024)
#define WS_DEFAULT_HANDSHAKE_TIMEOUT  10000
#define WS_DEFAULT_CLOSE_TIMEOUT      5000
#define WS_RECV_BUF_INIT              4096

struct xylem_ws_conn_s {
    http_transport_t  transport;
    bool              is_client;
    bool              _standalone;    /* true = wrapper owns free */
    uint8_t*          recv_buf;
    size_t            recv_len;
    size_t            recv_cap;
    uint8_t*          frag_buf;
    size_t            frag_len;
    size_t            frag_cap;
    uint8_t           frag_opcode;
    bool              frag_active;
    size_t            max_msg_size;
    size_t            fragment_threshold;
    uint64_t          close_timeout_ms;
    uint16_t          close_code;
    bool              close_sent;
    bool              close_received;
    void*             userdata;
};

xylem_ws_conn_t* ws_accept_impl(struct xylem_http_res_s* res,
                                 struct xylem_http_req_s* req,
                                 const xylem_ws_opts_t* opts);

xylem_ws_conn_t* ws_conn_create(http_transport_t transport,
                                 bool is_client,
                                 const xylem_ws_opts_t* opts);

void ws_conn_free(xylem_ws_conn_t* conn);

xylem_ws_conn_t* ws_dial_impl(http_transport_t transport,
                               const char* host, uint16_t port,
                               const char* path,
                               const xylem_ws_opts_t* opts);
```

- [ ] **Step 2: Commit**

```bash
git add src/net/ws/ws.h
git commit -m "feat(ws): add internal ws.h with conn struct"
```

---

### Task 5: Implement Core Logic (ws.c)

**Files:**
- Create: `src/net/ws/ws.c`

This is the largest task — implements `ws_accept_impl`, `ws_conn_create`, `ws_dial_impl`, `xylem_ws_recv`, `xylem_ws_send`, `xylem_ws_close`, `xylem_ws_ping`, and helpers.

- [ ] **Step 1: Create ws.c with includes and helpers**

```c
#include "ws.h"
#include "ws-frame.h"
#include "ws-handshake.h"
#include "ws-utf8.h"
#include "xylem/net/xylem-http.h"

#include <stdlib.h>
#include <string.h>

static void _ws_opts_apply(xylem_ws_conn_t* conn, const xylem_ws_opts_t* opts) {
    if (opts) {
        conn->max_msg_size = opts->max_msg_size ? opts->max_msg_size
                                                : WS_DEFAULT_MAX_MSG_SIZE;
        conn->fragment_threshold = opts->fragment_threshold ? opts->fragment_threshold
                                                           : WS_DEFAULT_FRAGMENT_THRESHOLD;
        conn->close_timeout_ms = opts->close_timeout_ms ? opts->close_timeout_ms
                                                        : WS_DEFAULT_CLOSE_TIMEOUT;
    } else {
        conn->max_msg_size       = WS_DEFAULT_MAX_MSG_SIZE;
        conn->fragment_threshold = WS_DEFAULT_FRAGMENT_THRESHOLD;
        conn->close_timeout_ms   = WS_DEFAULT_CLOSE_TIMEOUT;
    }
}

xylem_ws_conn_t* ws_conn_create(http_transport_t transport,
                                 bool is_client,
                                 const xylem_ws_opts_t* opts) {
    xylem_ws_conn_t* conn = (xylem_ws_conn_t*)calloc(1, sizeof(*conn));
    if (!conn) return NULL;

    conn->transport = transport;
    conn->is_client = is_client;
    conn->recv_buf  = (uint8_t*)malloc(WS_RECV_BUF_INIT);
    if (!conn->recv_buf) { free(conn); return NULL; }
    conn->recv_cap = WS_RECV_BUF_INIT;

    _ws_opts_apply(conn, opts);
    return conn;
}

void ws_conn_free(xylem_ws_conn_t* conn) {
    if (!conn) return;
    free(conn->recv_buf);
    free(conn->frag_buf);
    free(conn);
}
```

- [ ] **Step 2: Implement xylem_ws_send**

```c
static int _ws_write_frame(xylem_ws_conn_t* conn, bool fin, uint8_t opcode,
                           const void* data, size_t len) {
    uint8_t hdr_buf[14];
    uint8_t mask_key[4] = {0};

    if (conn->is_client) {
        uint32_t r = (uint32_t)rand();
        memcpy(mask_key, &r, 4);
    }

    size_t hdr_len = ws_frame_encode_header(hdr_buf, fin, opcode,
                                            conn->is_client, mask_key, len);

    int n = conn->transport.write(conn->transport.conn, hdr_buf, (int)hdr_len);
    if (n < 0) return -1;

    if (len > 0) {
        if (conn->is_client) {
            uint8_t* masked = (uint8_t*)malloc(len);
            if (!masked) return -1;
            memcpy(masked, data, len);
            ws_frame_apply_mask(masked, len, mask_key, 0);
            n = conn->transport.write(conn->transport.conn, masked, (int)len);
            free(masked);
        } else {
            n = conn->transport.write(conn->transport.conn, data, (int)len);
        }
        if (n < 0) return -1;
    }
    return 0;
}

int xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                  const void* data, size_t len) {
    if (!conn || conn->close_sent || conn->close_received) return -1;
    if (opcode != XYLEM_WS_TEXT && opcode != XYLEM_WS_BINARY) return -1;

    const uint8_t* p = (const uint8_t*)data;
    size_t threshold = conn->fragment_threshold;

    if (len <= threshold) {
        return _ws_write_frame(conn, true, (uint8_t)opcode, p, len);
    }

    /* Fragment */
    size_t offset = 0;
    bool first = true;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > threshold) chunk = threshold;
        bool fin = (offset + chunk >= len);
        uint8_t op = first ? (uint8_t)opcode : 0x0; /* continuation */

        if (_ws_write_frame(conn, fin, op, p + offset, chunk) != 0)
            return -1;

        offset += chunk;
        first = false;
    }
    return 0;
}
```

- [ ] **Step 3: Implement xylem_ws_ping and close frame helper**

```c
int xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len) {
    if (!conn || conn->close_sent || conn->close_received) return -1;
    if (len > 125) return -1;
    return _ws_write_frame(conn, true, 0x9, data, len);
}

static int _ws_send_close_frame(xylem_ws_conn_t* conn, uint16_t code,
                                const char* reason, size_t reason_len) {
    uint8_t payload[125];
    int plen = ws_frame_close_encode(code, reason, reason_len,
                                     payload, sizeof(payload));
    if (plen < 0) plen = 0;
    return _ws_write_frame(conn, true, 0x8, payload, (size_t)plen);
}
```

- [ ] **Step 4: Implement xylem_ws_recv**

```c
static int _ws_ensure_recv_buf(xylem_ws_conn_t* conn, size_t needed) {
    if (conn->recv_cap >= needed) return 0;
    size_t new_cap = conn->recv_cap;
    while (new_cap < needed) new_cap *= 2;
    uint8_t* nb = (uint8_t*)realloc(conn->recv_buf, new_cap);
    if (!nb) return -1;
    conn->recv_buf = nb;
    conn->recv_cap = new_cap;
    return 0;
}

static int _ws_frag_append(xylem_ws_conn_t* conn, const void* data, size_t len) {
    size_t needed = conn->frag_len + len;
    if (conn->max_msg_size && needed > conn->max_msg_size) return -1;
    if (needed > conn->frag_cap) {
        size_t new_cap = conn->frag_cap ? conn->frag_cap : 4096;
        while (new_cap < needed) new_cap *= 2;
        uint8_t* nb = (uint8_t*)realloc(conn->frag_buf, new_cap);
        if (!nb) return -1;
        conn->frag_buf = nb;
        conn->frag_cap = new_cap;
    }
    memcpy(conn->frag_buf + conn->frag_len, data, len);
    conn->frag_len += len;
    return 0;
}

int xylem_ws_recv(xylem_ws_conn_t* conn, xylem_ws_msg_t* msg) {
    if (!conn || !msg) return -1;
    if (conn->close_received) return -1;

    for (;;) {
        /* Try to decode a frame from recv_buf */
        while (conn->recv_len > 0) {
            ws_frame_header_t fh;
            int rc = ws_frame_decode_header(conn->recv_buf, conn->recv_len, &fh);
            if (rc == -1) break; /* need more data */
            if (rc == -2) { conn->close_code = 1002; return -1; }

            size_t frame_total = fh.header_size + fh.payload_len;
            if (conn->recv_len < frame_total) break; /* need more data */

            uint8_t* payload = conn->recv_buf + fh.header_size;
            if (fh.masked) {
                ws_frame_apply_mask(payload, (size_t)fh.payload_len, fh.mask_key, 0);
            }

            /* Consume frame from recv_buf */
            size_t remaining = conn->recv_len - frame_total;
            if (remaining > 0) {
                memmove(conn->recv_buf, conn->recv_buf + frame_total, remaining);
            }
            conn->recv_len = remaining;

            /* Handle by opcode */
            if (fh.opcode == 0x8) { /* Close */
                uint16_t code; const char* reason; size_t rlen;
                ws_frame_close_decode(payload, (size_t)fh.payload_len,
                                      &code, &reason, &rlen);
                conn->close_code = code;
                conn->close_received = true;
                if (!conn->close_sent) {
                    _ws_send_close_frame(conn, code, reason, rlen);
                    conn->close_sent = true;
                }
                return -1;
            } else if (fh.opcode == 0x9) { /* Ping → auto-pong */
                _ws_write_frame(conn, true, 0xA, payload, (size_t)fh.payload_len);
            } else if (fh.opcode == 0xA) { /* Pong → discard */
                /* noop */
            } else if (fh.opcode == 0x0) { /* Continuation */
                if (!conn->frag_active) { conn->close_code = 1002; return -1; }
                if (_ws_frag_append(conn, payload, (size_t)fh.payload_len) != 0) {
                    conn->close_code = 1009; return -1;
                }
                if (fh.fin) {
                    /* Deliver assembled message */
                    if (conn->frag_opcode == 0x1) {
                        if (ws_utf8_validate(conn->frag_buf, conn->frag_len) != 0) {
                            conn->close_code = 1007; return -1;
                        }
                    }
                    msg->opcode = (xylem_ws_opcode_t)conn->frag_opcode;
                    msg->data   = conn->frag_buf;
                    msg->len    = conn->frag_len;
                    conn->frag_buf = NULL;
                    conn->frag_len = 0;
                    conn->frag_cap = 0;
                    conn->frag_active = false;
                    return 0;
                }
            } else { /* Text or Binary */
                if (conn->frag_active) { conn->close_code = 1002; return -1; }
                if (fh.fin) {
                    /* Single-frame message */
                    if (conn->max_msg_size && fh.payload_len > conn->max_msg_size) {
                        conn->close_code = 1009; return -1;
                    }
                    if (fh.opcode == 0x1) {
                        if (ws_utf8_validate(payload, (size_t)fh.payload_len) != 0) {
                            conn->close_code = 1007; return -1;
                        }
                    }
                    void* copy = malloc(fh.payload_len ? fh.payload_len : 1);
                    if (!copy) return -1;
                    if (fh.payload_len) memcpy(copy, payload, (size_t)fh.payload_len);
                    msg->opcode = (xylem_ws_opcode_t)fh.opcode;
                    msg->data   = copy;
                    msg->len    = (size_t)fh.payload_len;
                    return 0;
                } else {
                    /* Start fragmented message */
                    conn->frag_active = true;
                    conn->frag_opcode = fh.opcode;
                    conn->frag_len = 0;
                    if (_ws_frag_append(conn, payload, (size_t)fh.payload_len) != 0) {
                        conn->close_code = 1009; return -1;
                    }
                }
            }
        }

        /* Need more data from transport */
        if (_ws_ensure_recv_buf(conn, conn->recv_len + 4096) != 0) return -1;
        int n = conn->transport.read(conn->transport.conn,
                                     conn->recv_buf + conn->recv_len,
                                     (int)(conn->recv_cap - conn->recv_len));
        if (n <= 0) {
            conn->close_code = 1006;
            return -1;
        }
        conn->recv_len += (size_t)n;
    }
}
```

- [ ] **Step 5: Implement xylem_ws_close**

```c
int xylem_ws_close(xylem_ws_conn_t* conn, uint16_t code,
                   const char* reason, size_t reason_len) {
    if (!conn) return -1;

    if (!conn->close_sent) {
        if (code && ws_frame_close_validate_send(code) != 0) {
            code = 1000;
        }
        _ws_send_close_frame(conn, code ? code : 1000, reason, reason_len);
        conn->close_sent = true;
    }

    /* If we haven't received close yet, wait for it */
    if (!conn->close_received) {
        conn->transport.set_rd_deadline(conn->transport.conn,
                                        conn->close_timeout_ms);
        /* Drain until close frame or timeout/error */
        for (;;) {
            if (_ws_ensure_recv_buf(conn, conn->recv_len + 4096) != 0) break;
            int n = conn->transport.read(conn->transport.conn,
                                         conn->recv_buf + conn->recv_len,
                                         (int)(conn->recv_cap - conn->recv_len));
            if (n <= 0) break;
            conn->recv_len += (size_t)n;

            /* Check for close frame */
            ws_frame_header_t fh;
            size_t scan = 0;
            while (scan < conn->recv_len) {
                int rc = ws_frame_decode_header(conn->recv_buf + scan,
                                               conn->recv_len - scan, &fh);
                if (rc != 0) break;
                size_t ftotal = fh.header_size + fh.payload_len;
                if (scan + ftotal > conn->recv_len) break;
                if (fh.opcode == 0x8) {
                    conn->close_received = true;
                    goto close_done;
                }
                scan += ftotal;
            }
        }
    }

close_done:
    conn->transport.close(conn->transport.conn);
    if (!conn->_standalone) {
        ws_conn_free(conn);
    }
    return 0;
}
```

- [ ] **Step 6: Implement ws_accept_impl**

```c
xylem_ws_conn_t* ws_accept_impl(struct xylem_http_res_s* res,
                                 struct xylem_http_req_s* req,
                                 const xylem_ws_opts_t* opts) {
    if (!res || !req) return NULL;

    /* Validate upgrade request headers */
    const char* ws_key = xylem_http_req_header(req, "Sec-WebSocket-Key");
    const char* ws_ver = xylem_http_req_header(req, "Sec-WebSocket-Version");
    if (!ws_key || !ws_ver || strcmp(ws_ver, "13") != 0) return NULL;

    /* Compute accept value */
    char accept_val[29];
    if (ws_handshake_compute_accept(ws_key, accept_val, sizeof(accept_val)) != 0)
        return NULL;

    /* Set Sec-WebSocket-Accept header before upgrade */
    xylem_http_res_set_header(res, "Sec-WebSocket-Accept", accept_val);

    /* Perform HTTP upgrade — sends 101, detaches transport */
    void* transport_ptr = NULL;
    if (xylem_http_res_upgrade(res, &transport_ptr) != 0) return NULL;

    http_transport_t* tp = (http_transport_t*)transport_ptr;
    xylem_ws_conn_t* conn = ws_conn_create(*tp, false, opts);
    return conn;
}
```

- [ ] **Step 7: Implement ws_dial_impl**

```c
xylem_ws_conn_t* ws_dial_impl(http_transport_t transport,
                               const char* host, uint16_t port,
                               const char* path,
                               const xylem_ws_opts_t* opts) {
    /* Generate key and build request */
    char key[25];
    if (ws_handshake_gen_key(key, sizeof(key)) != 0) {
        transport.close(transport.conn);
        return NULL;
    }

    size_t req_len;
    char* req = ws_handshake_build_request(host, port, path, key, &req_len);
    if (!req) {
        transport.close(transport.conn);
        return NULL;
    }

    /* Send upgrade request */
    uint64_t hs_timeout = (opts && opts->handshake_timeout_ms)
                          ? opts->handshake_timeout_ms
                          : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    transport.set_wr_deadline(transport.conn, hs_timeout);
    int n = transport.write(transport.conn, req, (int)req_len);
    free(req);
    if (n < 0) {
        transport.close(transport.conn);
        return NULL;
    }
    transport.set_wr_deadline(transport.conn, 0);

    /* Read response */
    transport.set_rd_deadline(transport.conn, hs_timeout);
    char resp_buf[1024];
    size_t resp_len = 0;

    while (resp_len < sizeof(resp_buf) - 1) {
        int r = transport.read(transport.conn,
                               resp_buf + resp_len,
                               (int)(sizeof(resp_buf) - 1 - resp_len));
        if (r <= 0) {
            transport.close(transport.conn);
            return NULL;
        }
        resp_len += (size_t)r;
        resp_buf[resp_len] = '\0';

        if (strstr(resp_buf, "\r\n\r\n")) break;
    }
    transport.set_rd_deadline(transport.conn, 0);

    /* Validate 101 response */
    if (strncmp(resp_buf, "HTTP/1.1 101", 12) != 0) {
        transport.close(transport.conn);
        return NULL;
    }

    /* Extract Sec-WebSocket-Accept and validate */
    const char* acc = strstr(resp_buf, "Sec-WebSocket-Accept: ");
    if (!acc) acc = strstr(resp_buf, "sec-websocket-accept: ");
    if (!acc) {
        transport.close(transport.conn);
        return NULL;
    }
    acc += strlen("Sec-WebSocket-Accept: ");
    const char* acc_end = strstr(acc, "\r\n");
    if (!acc_end) {
        transport.close(transport.conn);
        return NULL;
    }
    char accept_got[64];
    size_t acc_len = (size_t)(acc_end - acc);
    if (acc_len >= sizeof(accept_got)) {
        transport.close(transport.conn);
        return NULL;
    }
    memcpy(accept_got, acc, acc_len);
    accept_got[acc_len] = '\0';

    if (ws_handshake_validate_accept(key, accept_got) != 0) {
        transport.close(transport.conn);
        return NULL;
    }

    /* Create connection */
    xylem_ws_conn_t* conn = ws_conn_create(transport, true, opts);
    if (!conn) {
        transport.close(transport.conn);
        return NULL;
    }

    /* Move any leftover data after \r\n\r\n into recv_buf */
    const char* body_start = strstr(resp_buf, "\r\n\r\n") + 4;
    size_t leftover = resp_len - (size_t)(body_start - resp_buf);
    if (leftover > 0) {
        memcpy(conn->recv_buf, body_start, leftover);
        conn->recv_len = leftover;
    }

    return conn;
}
```

- [ ] **Step 8: Implement utility functions**

```c
void xylem_ws_msg_free(xylem_ws_msg_t* msg) {
    if (msg && msg->data) {
        free(msg->data);
        msg->data = NULL;
        msg->len = 0;
    }
}

uint16_t xylem_ws_close_code(xylem_ws_conn_t* conn) {
    return conn ? conn->close_code : 0;
}

void* xylem_ws_get_userdata(xylem_ws_conn_t* conn) {
    return conn ? conn->userdata : NULL;
}

void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud) {
    if (conn) conn->userdata = ud;
}
```

- [ ] **Step 9: Commit**

```bash
git add src/net/ws/ws.c
git commit -m "feat(ws): implement coroutine WS core logic (recv/send/close/accept/dial)"
```

---

### Task 6: Implement xylem-ws.c (TCP Entries)

**Files:**
- Create: `src/net/ws/xylem-ws.c`

- [ ] **Step 1: Create xylem-ws.c**

```c
#include "ws.h"
#include "xylem/net/xylem-http.h"
#include "xylem/net/xylem-tcp.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

/* --- URL parsing helper --- */

static int _ws_parse_url(const char* url, char* host, size_t host_cap,
                         uint16_t* port, char* path, size_t path_cap) {
    if (!url || strncmp(url, "ws://", 5) != 0) return -1;
    const char* p = url + 5;

    const char* colon = NULL;
    const char* slash = strchr(p, '/');
    const char* host_end = slash ? slash : p + strlen(p);

    for (const char* c = p; c < host_end; c++) {
        if (*c == ':') { colon = c; break; }
    }

    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (hlen >= host_cap) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = 80;
    if (colon) {
        *port = (uint16_t)atoi(colon + 1);
    }

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= path_cap) return -1;
        memcpy(path, slash, plen + 1);
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* --- Transport construction --- */

static http_transport_t _ws_make_tcp_transport(xylem_tcp_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))xylem_tcp_read,
        .write           = (int (*)(void*, const void*, int))xylem_tcp_write,
        .close           = (void (*)(void*))xylem_tcp_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline,
    };
}

/* --- Client --- */

xylem_ws_conn_t* xylem_ws_dial(const char* url, const xylem_ws_opts_t* opts) {
    char host[256], path[1024];
    uint16_t port;
    if (_ws_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return NULL;

    uint64_t timeout = (opts && opts->handshake_timeout_ms)
                       ? opts->handshake_timeout_ms
                       : WS_DEFAULT_HANDSHAKE_TIMEOUT;

    xylem_tcp_conn_t* tcp = xylem_tcp_dial(host, port, timeout, NULL);
    if (!tcp) return NULL;

    http_transport_t transport = _ws_make_tcp_transport(tcp);
    return ws_dial_impl(transport, host, port, path, opts);
}

/* --- Server: accept --- */

xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                  struct xylem_http_req_s* req,
                                  const xylem_ws_opts_t* opts) {
    return ws_accept_impl(res, req, opts);
}

/* --- Server: standalone listener --- */

typedef struct {
    xylem_http_srv_t*      http_srv;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
    xylem_ws_opts_t        opts;
} ws_listener_t;

typedef struct {
    xylem_ws_conn_t*       conn;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
} _ws_conn_ctx_t;

static void _ws_conn_coroutine(void* arg) {
    _ws_conn_ctx_t* ctx = (_ws_conn_ctx_t*)arg;
    ctx->conn->_standalone = true;
    ctx->handler(ctx->conn, ctx->userdata);
    if (!ctx->conn->close_sent) {
        xylem_ws_close(ctx->conn, 1000, NULL, 0);
    }
    ws_conn_free(ctx->conn);
    free(ctx);
}

static void _ws_upgrade_handler(xylem_http_res_t* res, xylem_http_req_t* req,
                                void* ud) {
    ws_listener_t* l = (ws_listener_t*)ud;
    xylem_ws_conn_t* conn = ws_accept_impl(res, req, &l->opts);
    if (!conn) return;

    _ws_conn_ctx_t* ctx = (_ws_conn_ctx_t*)malloc(sizeof(*ctx));
    if (!ctx) { xylem_ws_close(conn, 1011, NULL, 0); return; }
    ctx->conn     = conn;
    ctx->handler  = l->handler;
    ctx->userdata = l->userdata;
    runtime_spawn(_ws_conn_coroutine, ctx);
}

xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                      xylem_ws_handler_fn_t handler,
                                      void* userdata,
                                      const xylem_ws_opts_t* opts) {
    if (!handler) return NULL;

    ws_listener_t* l = (ws_listener_t*)calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->handler  = handler;
    l->userdata = userdata;
    if (opts) l->opts = *opts;

    xylem_http_srv_opts_t srv_opts = {0};
    srv_opts.on_upgrade      = _ws_upgrade_handler;
    srv_opts.upgrade_userdata = l;

    l->http_srv = xylem_http_listen(host, port, NULL, NULL, &srv_opts);
    if (!l->http_srv) { free(l); return NULL; }

    return (xylem_ws_listener_t*)l;
}

void xylem_ws_close_listener(xylem_ws_listener_t* listener) {
    if (!listener) return;
    ws_listener_t* l = (ws_listener_t*)listener;
    xylem_http_close(l->http_srv);
    free(l);
}

uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener) {
    if (!listener) return 0;
    ws_listener_t* l = (ws_listener_t*)listener;
    return xylem_http_srv_port(l->http_srv);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/net/ws/xylem-ws.c
git commit -m "feat(ws): implement plain WS entries (dial/listen/accept)"
```

---

### Task 7: Implement xylem-wss.c (TLS Entries)

**Files:**
- Create: `src/net/ws/xylem-wss.c`

- [ ] **Step 1: Create xylem-wss.c**

```c
#include "ws.h"
#include "xylem/net/xylem-https.h"
#include "xylem/net/xylem-tls.h"
#include "xylem/net/xylem-wss.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

/* --- URL parsing helper --- */

static int _wss_parse_url(const char* url, char* host, size_t host_cap,
                          uint16_t* port, char* path, size_t path_cap) {
    if (!url || strncmp(url, "wss://", 6) != 0) return -1;
    const char* p = url + 6;

    const char* colon = NULL;
    const char* slash = strchr(p, '/');
    const char* host_end = slash ? slash : p + strlen(p);

    for (const char* c = p; c < host_end; c++) {
        if (*c == ':') { colon = c; break; }
    }

    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (hlen >= host_cap) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = 443;
    if (colon) {
        *port = (uint16_t)atoi(colon + 1);
    }

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= path_cap) return -1;
        memcpy(path, slash, plen + 1);
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* --- Transport construction --- */

static http_transport_t _wss_make_tls_transport(xylem_tls_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))xylem_tls_read,
        .write           = (int (*)(void*, const void*, int))xylem_tls_write,
        .close           = (void (*)(void*))xylem_tls_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tls_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tls_set_write_deadline,
    };
}

/* --- Client --- */

xylem_ws_conn_t* xylem_wss_dial(const char* url,
                                 xylem_tls_ctx_t* tls_ctx,
                                 const xylem_ws_opts_t* opts) {
    char host[256], path[1024];
    uint16_t port;
    if (_wss_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return NULL;

    uint64_t timeout = (opts && opts->handshake_timeout_ms)
                       ? opts->handshake_timeout_ms
                       : WS_DEFAULT_HANDSHAKE_TIMEOUT;

    bool owns_ctx = false;
    if (!tls_ctx) {
        tls_ctx = xylem_tls_ctx_create(NULL);
        if (!tls_ctx) return NULL;
        owns_ctx = true;
    }

    xylem_tls_opts_t tls_opts = {0};
    tls_opts.server_name = host;
    tls_opts.handshake_timeout_ms = timeout;

    xylem_tls_conn_t* tls = xylem_tls_dial(host, port, tls_ctx, &tls_opts);
    if (owns_ctx) xylem_tls_ctx_destroy(tls_ctx);
    if (!tls) return NULL;

    http_transport_t transport = _wss_make_tls_transport(tls);
    return ws_dial_impl(transport, host, port, path, opts);
}

/* --- Server: accept --- */

xylem_ws_conn_t* xylem_wss_accept(struct xylem_http_res_s* res,
                                   struct xylem_http_req_s* req,
                                   const xylem_ws_opts_t* opts) {
    return ws_accept_impl(res, req, opts);
}

/* --- Server: standalone listener --- */

typedef struct {
    xylem_https_srv_t*     https_srv;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
    xylem_ws_opts_t        opts;
} wss_listener_t;

typedef struct {
    xylem_ws_conn_t*       conn;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
} _wss_conn_ctx_t;

static void _wss_conn_coroutine(void* arg) {
    _wss_conn_ctx_t* ctx = (_wss_conn_ctx_t*)arg;
    ctx->conn->_standalone = true;
    ctx->handler(ctx->conn, ctx->userdata);
    if (!ctx->conn->close_sent) {
        xylem_ws_close(ctx->conn, 1000, NULL, 0);
    }
    ws_conn_free(ctx->conn);
    free(ctx);
}

static void _wss_upgrade_handler(xylem_http_res_t* res, xylem_http_req_t* req,
                                 void* ud) {
    wss_listener_t* l = (wss_listener_t*)ud;
    xylem_ws_conn_t* conn = ws_accept_impl(res, req, &l->opts);
    if (!conn) return;

    _wss_conn_ctx_t* ctx = (_wss_conn_ctx_t*)malloc(sizeof(*ctx));
    if (!ctx) { xylem_ws_close(conn, 1011, NULL, 0); return; }
    ctx->conn     = conn;
    ctx->handler  = l->handler;
    ctx->userdata = l->userdata;
    runtime_spawn(_wss_conn_coroutine, ctx);
}

xylem_wss_listener_t* xylem_wss_listen(const char* host, uint16_t port,
                                        xylem_ws_handler_fn_t handler,
                                        void* userdata,
                                        xylem_tls_ctx_t* tls_ctx,
                                        const xylem_ws_opts_t* opts) {
    if (!handler || !tls_ctx) return NULL;

    wss_listener_t* l = (wss_listener_t*)calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->handler  = handler;
    l->userdata = userdata;
    if (opts) l->opts = *opts;

    xylem_https_srv_opts_t srv_opts = {0};
    srv_opts.on_upgrade      = _wss_upgrade_handler;
    srv_opts.upgrade_userdata = l;

    l->https_srv = xylem_https_listen(host, port, NULL, NULL, tls_ctx, &srv_opts);
    if (!l->https_srv) { free(l); return NULL; }

    return (xylem_wss_listener_t*)l;
}

void xylem_wss_close_listener(xylem_wss_listener_t* listener) {
    if (!listener) return;
    wss_listener_t* l = (wss_listener_t*)listener;
    xylem_https_close(l->https_srv);
    free(l);
}

uint16_t xylem_wss_listener_port(xylem_wss_listener_t* listener) {
    if (!listener) return 0;
    wss_listener_t* l = (wss_listener_t*)listener;
    return xylem_https_srv_port(l->https_srv);
}
```

- [ ] **Step 2: Commit**

```bash
git add src/net/ws/xylem-wss.c
git commit -m "feat(ws): implement WSS entries (dial/listen/accept)"
```

---

### Task 8: Update CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Replace WS source block**

Replace lines 142-157 with:

```cmake
if(XYLEM_ENABLE_WS)
	list(APPEND SRCS
		src/net/ws/ws.c
		src/net/ws/xylem-ws.c
		src/net/ws/ws-utf8.c
		src/net/ws/ws-frame.c
		src/net/ws/ws-handshake.c
	)
	if(XYLEM_ENABLE_TLS)
		list(APPEND SRCS src/net/ws/xylem-wss.c)
	endif()
endif()
```

- [ ] **Step 2: Build**

```bash
cmake -B build -DXYLEM_ENABLE_WS=ON -DXYLEM_ENABLE_TLS=ON -DXYLEM_ENABLE_TESTING=ON
cmake --build build
```

Expected: compiles cleanly.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(ws): update CMakeLists for coroutine WS sources"
```

---

### Task 9: Write Integration Tests

**Files:**
- Rewrite: `tests/test-ws.c`

- [ ] **Step 1: Write test-ws.c**

```c
#include "xylem.h"
#include "xylem/net/xylem-ws.h"
#include "xylem/sync/xylem-channel.h"
#include "assert.h"

#include <stdio.h>
#include <string.h>

/* --- Echo server handler --- */
static void echo_handler(xylem_ws_conn_t* ws, void* ud) {
    (void)ud;
    xylem_ws_msg_t msg;
    while (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_send(ws, msg.opcode, msg.data, msg.len);
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, NULL, 0);
}

/* --- Test: basic text echo --- */
static void test_text_echo(void* arg) {
    (void)arg;
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, NULL);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);

    xylem_ws_conn_t* c = xylem_ws_dial(url, NULL);
    ASSERT(c != NULL);

    const char* text = "hello websocket";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: binary echo --- */
static void test_binary_echo(void* arg) {
    (void)arg;
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, NULL);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, NULL);
    ASSERT(c != NULL);

    uint8_t data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE};
    ASSERT(xylem_ws_send(c, XYLEM_WS_BINARY, data, sizeof(data)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_BINARY);
    ASSERT(msg.len == sizeof(data));
    ASSERT(memcmp(msg.data, data, sizeof(data)) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: multiple messages --- */
static void test_multiple_messages(void* arg) {
    (void)arg;
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, NULL);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, NULL);
    ASSERT(c != NULL);

    for (int i = 0; i < 10; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "msg-%d", i);
        ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, buf, (size_t)len) == 0);

        xylem_ws_msg_t msg;
        ASSERT(xylem_ws_recv(c, &msg) == 0);
        ASSERT(msg.len == (size_t)len);
        ASSERT(memcmp(msg.data, buf, msg.len) == 0);
        xylem_ws_msg_free(&msg);
    }

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: fragmentation (large message) --- */
static void test_large_message(void* arg) {
    (void)arg;
    xylem_ws_opts_t opts = { .fragment_threshold = 1024 };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              echo_handler, NULL, &opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, &opts);
    ASSERT(c != NULL);

    /* 8KB message, fragment_threshold=1KB → multiple frames */
    size_t big_len = 8192;
    uint8_t* big = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    for (size_t i = 0; i < big_len; i++) big[i] = (uint8_t)(i & 0xFF);

    ASSERT(xylem_ws_send(c, XYLEM_WS_BINARY, big, big_len) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_BINARY);
    ASSERT(msg.len == big_len);
    ASSERT(memcmp(msg.data, big, big_len) == 0);
    xylem_ws_msg_free(&msg);

    free(big);
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: server closes first --- */
static void close_handler(xylem_ws_conn_t* ws, void* ud) {
    (void)ud;
    xylem_ws_msg_t msg;
    if (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, "bye", 3);
}

static void test_server_close(void* arg) {
    (void)arg;
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                              close_handler, NULL, NULL);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%u/", port);
    xylem_ws_conn_t* c = xylem_ws_dial(url, NULL);
    ASSERT(c != NULL);

    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, "trigger", 7) == 0);

    xylem_ws_msg_t msg;
    int rc = xylem_ws_recv(c, &msg);
    ASSERT(rc == -1);
    ASSERT(xylem_ws_close_code(c) == 1000);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    xylem_shutdown();
}

/* --- Test: NULL parameter guards --- */
static void test_null_guards(void* arg) {
    (void)arg;
    ASSERT(xylem_ws_dial(NULL, NULL) == NULL);
    ASSERT(xylem_ws_dial("http://bad", NULL) == NULL);
    ASSERT(xylem_ws_send(NULL, XYLEM_WS_TEXT, "x", 1) == -1);
    ASSERT(xylem_ws_recv(NULL, NULL) == -1);
    ASSERT(xylem_ws_ping(NULL, NULL, 0) == -1);
    ASSERT(xylem_ws_close(NULL, 1000, NULL, 0) == -1);
    ASSERT(xylem_ws_close_code(NULL) == 0);
    ASSERT(xylem_ws_get_userdata(NULL) == NULL);
    xylem_ws_set_userdata(NULL, NULL); /* no crash */
    xylem_ws_msg_free(NULL);           /* no crash */
    xylem_ws_close_listener(NULL);     /* no crash */
    ASSERT(xylem_ws_listener_port(NULL) == 0);
    xylem_shutdown();
}

/* --- Runner --- */
typedef void (*test_fn_t)(void*);

static test_fn_t tests[] = {
    test_null_guards,
    test_text_echo,
    test_binary_echo,
    test_multiple_messages,
    test_large_message,
    test_server_close,
};

static int test_idx = 0;
static int test_count = (int)(sizeof(tests) / sizeof(tests[0]));

static void run_next(void* arg) {
    (void)arg;
    if (test_idx < test_count) {
        tests[test_idx++](NULL);
    }
}

int main(void) {
    for (int i = 0; i < test_count; i++) {
        test_idx = i;
        xylem_run(run_next, NULL, NULL);
    }
    printf("All %d WS tests passed.\n", test_count);
    return 0;
}
```

- [ ] **Step 2: Build and run tests**

```bash
cmake --build build
ctest --test-dir build -R ws --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Commit**

```bash
git add tests/test-ws.c
git commit -m "test(ws): rewrite tests for coroutine-based WS API"
```

---

### Task 10: Final Build Verification

- [ ] **Step 1: Full build with all sanitizers**

```bash
cmake -B build -DXYLEM_ENABLE_WS=ON -DXYLEM_ENABLE_TLS=ON \
      -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_ASAN=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: all tests pass with no sanitizer reports.

- [ ] **Step 2: Build without TLS (stub path)**

```bash
cmake -B build-no-tls -DXYLEM_ENABLE_WS=ON -DXYLEM_ENABLE_TLS=OFF \
      -DXYLEM_ENABLE_TESTING=ON
cmake --build build-no-tls
```

Expected: compiles without `xylem-wss.c` (gated by `XYLEM_ENABLE_TLS`), no linker errors.

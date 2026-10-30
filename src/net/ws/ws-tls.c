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

#include "xylem/net/xylem-ws.h"

#include "xylem/net/xylem-tls.h"
#include "xylem/xylem-utils.h"

#include "ws-tls.h"
#include "ws.h"

#include <stdlib.h>

typedef struct {
    xylem_tls_ctx_t* ctx;
    _Atomic uint32_t refs;
} _ws_tls_owner_t;

typedef struct {
    xylem_tls_conn_t* conn;
    _ws_tls_owner_t*  owner;
} _ws_tls_conn_t;

typedef struct {
    xylem_tls_listener_t* ln;
    _ws_tls_owner_t*      owner;
} _ws_tls_listener_t;

static _ws_tls_owner_t* _ws_tls_owner_create(void) {
    _ws_tls_owner_t* owner =
        (_ws_tls_owner_t*)calloc(1, sizeof(_ws_tls_owner_t));
    if (!owner) {
        return NULL;
    }
    owner->ctx = xylem_tls_ctx_create();
    if (!owner->ctx) {
        free(owner);
        return NULL;
    }
    atomic_init(&owner->refs, 1);
    return owner;
}

static void _ws_tls_owner_release(_ws_tls_owner_t* owner) {
    if (owner && atomic_fetch_sub(&owner->refs, 1) == 1) {
        xylem_tls_ctx_destroy(owner->ctx);
        free(owner);
    }
}

static int _ws_tls_conn_read_cb(void* p, void* b, int n) {
    return xylem_tls_read(((_ws_tls_conn_t*)p)->conn, b, n);
}

static int _ws_tls_conn_write_cb(void* p, const void* b, int n) {
    return xylem_tls_write(((_ws_tls_conn_t*)p)->conn, b, n);
}

static void _ws_tls_conn_read_deadline_cb(void* p, uint64_t ms) {
    xylem_tls_set_read_deadline(((_ws_tls_conn_t*)p)->conn, ms);
}

static void _ws_tls_conn_write_deadline_cb(void* p, uint64_t ms) {
    xylem_tls_set_write_deadline(((_ws_tls_conn_t*)p)->conn, ms);
}

static void _ws_tls_conn_destroy_cb(void* p) {
    if (!p) {
        return;
    }
    _ws_tls_conn_t* c = p;
    xylem_tls_destroy(c->conn);
    _ws_tls_owner_release(c->owner);
    free(c);
}

static void _ws_tls_conn_close_cb(void* p) {
    xylem_tls_close(((_ws_tls_conn_t*)p)->conn);
}

static int _ws_tls_conn_remote_addr_cb(
    void*     p,
    char*     host,
    size_t    host_len,
    uint16_t* port) {
    return xylem_tls_remote_addr(
        ((_ws_tls_conn_t*)p)->conn,
        host,
        host_len,
        port);
}

static int _ws_tls_conn_local_addr_cb(
    void*     p,
    char*     host,
    size_t    host_len,
    uint16_t* port) {
    return xylem_tls_local_addr(
        ((_ws_tls_conn_t*)p)->conn,
        host,
        host_len,
        port);
}

static ws_transport_t
_ws_tls_create_transport(xylem_tls_conn_t* conn, _ws_tls_owner_t* owner) {
    _ws_tls_conn_t* c = (_ws_tls_conn_t*)malloc(sizeof(_ws_tls_conn_t));
    if (!c) {
        xylem_tls_destroy(conn);
        _ws_tls_owner_release(owner);
        return (ws_transport_t){0};
    }
    c->conn  = conn;
    c->owner = owner;
    return (ws_transport_t){
        .conn            = c,
        .read            = _ws_tls_conn_read_cb,
        .write           = _ws_tls_conn_write_cb,
        .close           = _ws_tls_conn_close_cb,
        .destroy         = _ws_tls_conn_destroy_cb,
        .remote_addr     = _ws_tls_conn_remote_addr_cb,
        .local_addr      = _ws_tls_conn_local_addr_cb,
        .set_rd_deadline = _ws_tls_conn_read_deadline_cb,
        .set_wr_deadline = _ws_tls_conn_write_deadline_cb};
}

static ws_transport_t _ws_tls_listener_accept_cb(void* p) {
    _ws_tls_listener_t* l    = p;
    xylem_tls_conn_t*   conn = xylem_tls_accept(l->ln);
    if (!conn) {
        return (ws_transport_t){0};
    }
    atomic_fetch_add(&l->owner->refs, 1);
    return _ws_tls_create_transport(conn, l->owner);
}

static void _ws_tls_listener_close_cb(void* p) {
    xylem_tls_close_listener(((_ws_tls_listener_t*)p)->ln);
}

static void _ws_tls_listener_destroy_cb(void* p) {
    if (!p) {
        return;
    }
    _ws_tls_listener_t* l = p;
    xylem_tls_destroy_listener(l->ln);
    _ws_tls_owner_release(l->owner);
    free(l);
}

static int
_ws_tls_listener_addr_cb(void* p, char* host, size_t host_len, uint16_t* port) {
    return xylem_tls_listener_addr(
        ((_ws_tls_listener_t*)p)->ln,
        host,
        host_len,
        port);
}

xylem_ws_conn_t* ws_tls_dial(
    const char*            host,
    uint16_t               port,
    const char*            path,
    const xylem_ws_tls_t*  config,
    const xylem_ws_opts_t* opts) {
    _ws_tls_owner_t* owner = _ws_tls_owner_create();
    if (!owner) {
        return NULL;
    }
    xylem_tls_ctx_t* ctx = owner->ctx;
    if (config) {
        if ((config->ca && xylem_tls_ctx_load_ca(ctx, config->ca)) ||
            (!!config->cert != !!config->key) ||
            (config->cert &&
             xylem_tls_ctx_load_cert(ctx, NULL, config->cert, config->key))) {
            _ws_tls_owner_release(owner);
            return NULL;
        }
        xylem_tls_ctx_verify_server(ctx, !config->skip_verify);
    }
    uint64_t timeout  = opts && opts->handshake_timeout_ms
                            ? opts->handshake_timeout_ms
                            : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;
    xylem_tls_opts_t tls_opts = {
        .server_name        = host,
        .connect_timeout_ms = timeout};
    xylem_tls_conn_t* conn = xylem_tls_dial(host, port, ctx, &tls_opts);
    if (!conn) {
        _ws_tls_owner_release(owner);
        return NULL;
    }
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (now >= deadline) {
        xylem_tls_destroy(conn);
        _ws_tls_owner_release(owner);
        return NULL;
    }
    xylem_ws_opts_t remaining      = opts ? *opts : (xylem_ws_opts_t){0};
    remaining.handshake_timeout_ms = deadline - now;
    ws_transport_t t               = _ws_tls_create_transport(conn, owner);
    return t.conn ? ws_dial_impl(t, host, port, path, &remaining) : NULL;
}

xylem_ws_listener_t*
ws_tls_listen(const char* host, uint16_t port, const xylem_ws_opts_t* opts) {
    const xylem_ws_tls_t* config = opts->tls;
    if (!config->cert || !config->key) {
        return NULL;
    }
    _ws_tls_listener_t* l =
        (_ws_tls_listener_t*)calloc(1, sizeof(_ws_tls_listener_t));
    if (!l) {
        return NULL;
    }
    l->owner = _ws_tls_owner_create();
    if (!l->owner) {
        free(l);
        return NULL;
    }
    if (xylem_tls_ctx_load_cert(
            l->owner->ctx,
            NULL,
            config->cert,
            config->key) ||
        (config->ca && xylem_tls_ctx_load_ca(l->owner->ctx, config->ca))) {
        _ws_tls_listener_destroy_cb(l);
        return NULL;
    }
    if (config->ca) {
        xylem_tls_ctx_verify_client(l->owner->ctx, true);
    }
    l->ln = xylem_tls_listen(host, port, l->owner->ctx, NULL);
    if (!l->ln) {
        _ws_tls_listener_destroy_cb(l);
        return NULL;
    }
    return ws_listener_create(
        l,
        _ws_tls_listener_accept_cb,
        _ws_tls_listener_close_cb,
        _ws_tls_listener_destroy_cb,
        _ws_tls_listener_addr_cb,
        opts);
}

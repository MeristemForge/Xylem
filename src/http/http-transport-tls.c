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

#include "http-transport.h"
#include "xylem/xylem-tls.h"

#include <stdlib.h>

/**
 * Per-connection context that bridges xylem_tls callbacks to the
 * generic http_transport_cb_t interface.
 */
typedef struct {
    http_transport_cb_t* cb;
    void*                ctx;
    xylem_tls_ctx_t*     tls_ctx;
} _tls_bridge_t;

static void _tls_connect_cb(xylem_tls_conn_t* tls) {
    _tls_bridge_t* br = xylem_tls_get_userdata(tls);
    br->cb->on_connect(tls, br->ctx);
}

static void _tls_accept_cb(xylem_tls_server_t* server,
                           xylem_tls_conn_t* tls) {
    _tls_bridge_t* br = xylem_tls_server_get_userdata(server);
    xylem_tls_set_userdata(tls, br);
    br->cb->on_accept(tls, br->ctx);
}

static void _tls_read_cb(xylem_tls_conn_t* tls, void* data, size_t len) {
    _tls_bridge_t* br = xylem_tls_get_userdata(tls);
    br->cb->on_read(tls, br->ctx, data, len);
}

static void _tls_close_cb(xylem_tls_conn_t* tls, int err,
                          const char* errmsg) {
    _tls_bridge_t* br = xylem_tls_get_userdata(tls);
    br->cb->on_close(tls, br->ctx, err, errmsg);
    free(br);
}

static void* _tls_dial(xylem_loop_t* loop, xylem_addr_t* addr,
                       http_transport_cb_t* cb, void* ctx,
                       xylem_tcp_opts_t* opts) {
    _tls_bridge_t* br = (_tls_bridge_t*)calloc(1, sizeof(_tls_bridge_t));
    if (!br) {
        return NULL;
    }
    br->cb      = cb;
    br->ctx     = ctx;
    br->tls_ctx = xylem_tls_ctx_create();
    if (!br->tls_ctx) {
        free(br);
        return NULL;
    }

    xylem_tls_handler_t handler = {
        .on_connect = _tls_connect_cb,
        .on_read    = _tls_read_cb,
        .on_close   = _tls_close_cb,
    };

    xylem_tls_opts_t tls_opts = {0};
    if (opts) {
        tls_opts.tcp = *opts;
    }

    xylem_tls_conn_t* tls = xylem_tls_dial(loop, addr, br->tls_ctx,
                                       &handler,
                                       opts ? &tls_opts : NULL);
    if (!tls) {
        xylem_tls_ctx_destroy(br->tls_ctx);
        free(br);
        return NULL;
    }
    xylem_tls_set_userdata(tls, br);
    return tls;
}

static void* _tls_listen(xylem_loop_t* loop, xylem_addr_t* addr,
                         http_transport_cb_t* cb, void* ctx,
                         xylem_tcp_opts_t* opts,
                         const char* tls_cert, const char* tls_key) {
    _tls_bridge_t* br = (_tls_bridge_t*)calloc(1, sizeof(_tls_bridge_t));
    if (!br) {
        return NULL;
    }
    br->cb      = cb;
    br->ctx     = ctx;
    br->tls_ctx = xylem_tls_ctx_create();
    if (!br->tls_ctx) {
        free(br);
        return NULL;
    }
    if (xylem_tls_ctx_load_cert(br->tls_ctx, tls_cert, tls_key) != 0) {
        xylem_tls_ctx_destroy(br->tls_ctx);
        free(br);
        return NULL;
    }

    xylem_tls_handler_t handler = {
        .on_accept = _tls_accept_cb,
        .on_read   = _tls_read_cb,
        .on_close  = _tls_close_cb,
    };

    xylem_tls_opts_t tls_opts = {0};
    if (opts) {
        tls_opts.tcp = *opts;
    }

    xylem_tls_server_t* srv = xylem_tls_listen(loop, addr, br->tls_ctx,
                                                &handler,
                                                opts ? &tls_opts : NULL);
    if (!srv) {
        xylem_tls_ctx_destroy(br->tls_ctx);
        free(br);
        return NULL;
    }
    xylem_tls_server_set_userdata(srv, br);
    return srv;
}

static int _tls_send(void* handle, const void* data, size_t len) {
    return xylem_tls_send(handle, data, len);
}

static void _tls_close_conn(void* handle) {
    xylem_tls_close(handle);
}

static void _tls_close_server(void* handle) {
    xylem_tls_close_server(handle);
}

static void _tls_set_userdata(void* handle, void* ud) {
    xylem_tls_set_userdata(handle, ud);
}

static void* _tls_get_userdata(void* handle) {
    return xylem_tls_get_userdata(handle);
}

static const http_transport_vt_t _tls_vt = {
    .dial         = _tls_dial,
    .listen       = _tls_listen,
    .send         = _tls_send,
    .close_conn   = _tls_close_conn,
    .close_server = _tls_close_server,
    .set_userdata = _tls_set_userdata,
    .get_userdata = _tls_get_userdata,
};

const http_transport_vt_t* http_transport_tls(void) {
    return &_tls_vt;
}

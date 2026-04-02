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

#include "ws-transport.h"

#include <stdlib.h>

/**
 * Per-connection context that bridges xylem_tcp callbacks to the
 * generic ws_transport_cb_t interface.
 */
typedef struct {
    ws_transport_cb_t* cb;
    void*              ctx;
} _tcp_bridge_t;

static void _tcp_connect_cb(xylem_tcp_conn_t* conn) {
    _tcp_bridge_t* br = xylem_tcp_get_userdata(conn);
    br->cb->on_connect(conn, br->ctx);
}

static void _tcp_accept_cb(xylem_tcp_server_t* server,
                           xylem_tcp_conn_t* conn) {
    _tcp_bridge_t* br = xylem_tcp_server_get_userdata(server);
    xylem_tcp_set_userdata(conn, br);
    br->cb->on_accept(conn, br->ctx);
}

static void _tcp_read_cb(xylem_tcp_conn_t* conn,
                         void* data, size_t len) {
    _tcp_bridge_t* br = xylem_tcp_get_userdata(conn);
    br->cb->on_read(conn, br->ctx, data, len);
}

static void _tcp_write_done_cb(xylem_tcp_conn_t* conn,
                               void* data, size_t len, int status) {
    _tcp_bridge_t* br = xylem_tcp_get_userdata(conn);
    if (br->cb->on_write_done) {
        br->cb->on_write_done(conn, br->ctx, data, len, status);
    }
}

static void _tcp_close_cb(xylem_tcp_conn_t* conn, int err,
                          const char* errmsg) {
    _tcp_bridge_t* br = xylem_tcp_get_userdata(conn);
    br->cb->on_close(conn, br->ctx, err, errmsg);
    free(br);
}

static void* _tcp_dial(xylem_loop_t* loop, xylem_addr_t* addr,
                       ws_transport_cb_t* cb, void* ctx,
                       xylem_tcp_opts_t* opts) {
    _tcp_bridge_t* br = malloc(sizeof(*br));
    if (!br) {
        return NULL;
    }
    br->cb  = cb;
    br->ctx = ctx;

    xylem_tcp_handler_t handler = {
        .on_connect    = _tcp_connect_cb,
        .on_read       = _tcp_read_cb,
        .on_write_done = _tcp_write_done_cb,
        .on_close      = _tcp_close_cb,
    };

    xylem_tcp_conn_t* conn = xylem_tcp_dial(loop, addr, &handler, opts);
    if (!conn) {
        free(br);
        return NULL;
    }
    xylem_tcp_set_userdata(conn, br);
    return conn;
}

static void* _tcp_listen(xylem_loop_t* loop, xylem_addr_t* addr,
                         ws_transport_cb_t* cb, void* ctx,
                         xylem_tcp_opts_t* opts,
                         const char* tls_cert, const char* tls_key) {
    (void)tls_cert;
    (void)tls_key;

    _tcp_bridge_t* br = malloc(sizeof(*br));
    if (!br) {
        return NULL;
    }
    br->cb  = cb;
    br->ctx = ctx;

    xylem_tcp_handler_t handler = {
        .on_accept     = _tcp_accept_cb,
        .on_read       = _tcp_read_cb,
        .on_write_done = _tcp_write_done_cb,
        .on_close      = _tcp_close_cb,
    };

    xylem_tcp_server_t* srv = xylem_tcp_listen(loop, addr, &handler, opts);
    if (!srv) {
        free(br);
        return NULL;
    }
    xylem_tcp_server_set_userdata(srv, br);
    return srv;
}

static int _tcp_send(void* handle, const void* data, size_t len) {
    return xylem_tcp_send(handle, data, len);
}

static void _tcp_close_conn(void* handle) {
    xylem_tcp_close(handle);
}

static void _tcp_close_server(void* handle) {
    _tcp_bridge_t* br = xylem_tcp_server_get_userdata(handle);
    xylem_tcp_close_server(handle);
    free(br);
}

static void _tcp_set_userdata(void* handle, void* ud) {
    xylem_tcp_set_userdata(handle, ud);
}

static void* _tcp_get_userdata(void* handle) {
    return xylem_tcp_get_userdata(handle);
}

static const xylem_addr_t* _tcp_get_peer_addr(void* handle) {
    return xylem_tcp_get_peer_addr(handle);
}

static const ws_transport_vt_t _tcp_vt = {
    .dial          = _tcp_dial,
    .listen        = _tcp_listen,
    .send          = _tcp_send,
    .close_conn    = _tcp_close_conn,
    .close_server  = _tcp_close_server,
    .set_userdata  = _tcp_set_userdata,
    .get_userdata  = _tcp_get_userdata,
    .get_peer_addr = _tcp_get_peer_addr,
};

const ws_transport_vt_t* ws_transport_tcp(void) {
    return &_tcp_vt;
}

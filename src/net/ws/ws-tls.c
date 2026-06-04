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

/*
 * Secure WebSocket (wss) dial factory. Establishes a TLS client
 * connection and runs the WebSocket handshake over it. Compiled only when
 * XYLEM_ENABLE_TLS is set; ws-tls-stub.c provides a NULL-returning stub
 * otherwise, so the unconditionally-built public dial (xylem-ws.c) never
 * references TLS symbols directly.
 */

#include "ws-tls.h"

#include "ws.h"

#include "xylem/net/xylem-tls.h"

#include <stdbool.h>


static http_transport_t _ws_make_tls_transport(xylem_tls_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))xylem_tls_read,
        .write           = (int (*)(void*, const void*, int))xylem_tls_write,
        .close           = (void (*)(void*))xylem_tls_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tls_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tls_set_write_deadline,
    };
}


xylem_ws_conn_t* ws_tls_dial(const char* host, uint16_t port,
                             const char* path,
                             const xylem_ws_tls_t* tls,
                             const xylem_ws_opts_t* opts) {
    uint64_t timeout = (opts && opts->handshake_timeout_ms)
                       ? opts->handshake_timeout_ms
                       : WS_DEFAULT_HANDSHAKE_TIMEOUT;

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    if (!ctx) {
        return NULL;
    }

    if (tls) {
        if (tls->ca) {
            xylem_tls_ctx_load_ca(ctx, tls->ca);
        }
        if (tls->cert && tls->key) {
            xylem_tls_ctx_load_cert(ctx, NULL, tls->cert, tls->key);
        }
        if (tls->skip_verify) {
            xylem_tls_ctx_verify_server(ctx, false);
        }
    }

    xylem_tls_opts_t tls_opts = {0};
    tls_opts.server_name          = host;
    tls_opts.handshake_timeout_ms = timeout;

    xylem_tls_conn_t* conn = xylem_tls_dial(host, port, ctx, &tls_opts);
    xylem_tls_ctx_destroy(ctx);
    if (!conn) {
        return NULL;
    }

    http_transport_t transport = _ws_make_tls_transport(conn);
    return ws_dial_impl(transport, host, port, path, opts);
}

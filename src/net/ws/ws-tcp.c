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

/**
 * Plain WebSocket (ws) dial factory. Establishes a plain-TCP client
 * connection and runs the WebSocket handshake over it. Always built; the
 * mirror of ws-tls.c, which provides the secure (wss) dial factory. The
 * public dial in xylem-ws.c parses the URL scheme and dispatches here for
 * ws:// or to ws_tls_dial() for wss://.
 */

#include "ws-tcp.h"

#include "ws.h"

#include "xylem/net/xylem-tcp.h"


static http_transport_t _ws_make_tcp_transport(xylem_tcp_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))xylem_tcp_read,
        .write           = (int (*)(void*, const void*, int))xylem_tcp_write,
        .close           = (void (*)(void*))xylem_tcp_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline,
        .remote_addr     = (int (*)(void*, char*, size_t, uint16_t*))xylem_tcp_remote_addr,
        .local_addr      = (int (*)(void*, char*, size_t, uint16_t*))xylem_tcp_local_addr,
        .shutdown_wr     = (int (*)(void*))xylem_tcp_shutdown_wr,
    };
}


xylem_ws_conn_t* ws_tcp_dial(const char* host, uint16_t port,
                             const char* path,
                             const xylem_ws_opts_t* opts) {
    uint64_t timeout = (opts && opts->handshake_timeout_ms)
                       ? opts->handshake_timeout_ms
                       : WS_DEFAULT_HANDSHAKE_TIMEOUT;

    xylem_tcp_conn_t* tcp = xylem_tcp_dial(host, port, timeout, NULL);
    if (!tcp) {
        return NULL;
    }

    http_transport_t transport = _ws_make_tcp_transport(tcp);
    return ws_dial_impl(transport, host, port, path, opts);
}

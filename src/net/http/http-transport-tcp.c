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

#include "xylem/net/http/xylem-http.h"

#include "xylem/net/xylem-tcp.h"

#include "http-transport-tcp.h"
#include "http-utils.h"
#include "runtime/runtime.h"

#include <stdlib.h>

/**
 * Plain-TCP transport factory for the HTTP engine. Builds an
 * http_transport_t over xylem_tcp for client dialing. Always built.
 */

static void _http_tcp_conn_close_cb(void* conn) {
    xylem_tcp_destroy((xylem_tcp_conn_t*)conn);
}

static http_transport_t _http_tcp_create_transport(xylem_tcp_conn_t* conn) {
    return (http_transport_t){
        .conn  = conn,
        .read  = (int (*)(void*, void*, int))xylem_tcp_read,
        .write = (int (*)(void*, const void*, int))xylem_tcp_write,
        .close = _http_tcp_conn_close_cb,
        .set_rd_deadline =
            (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline,
        .set_wr_deadline =
            (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline,
        .remote_addr =
            (int (*)(void*, char*, size_t, uint16_t*))xylem_tcp_remote_addr,
        .local_addr =
            (int (*)(void*, char*, size_t, uint16_t*))xylem_tcp_local_addr,
        .shutdown_wr = (int (*)(void*))xylem_tcp_shutdown_wr,
    };
}

/**
 * When dialing through a plain-HTTP proxy, ctx carries the proxy
 * descriptor: the TCP connection targets the proxy (not the origin) and
 * the request line uses absolute-form (handled by http_req_serialize via
 * absolute_form). A NULL ctx means a direct connection to the origin.
 */
http_transport_t http_tcp_dial(
    const char*                  host,
    uint16_t                     port,
    uint64_t                     timeout_ms,
    const xylem_http_cli_opts_t* options) {
    const xylem_http_proxy_t* proxy = options ? options->proxy : NULL;

    const char* dial_host = host;
    uint16_t    dial_port = port;
    if (proxy && proxy->host) {
        dial_host = proxy->host;
        dial_port = proxy->port;
    }

    xylem_tcp_opts_t  opts = {.connect_timeout_ms = timeout_ms};
    xylem_tcp_conn_t* conn = xylem_tcp_dial(dial_host, dial_port, &opts);
    if (!conn) {
        return (http_transport_t){0};
    }
    return _http_tcp_create_transport(conn);
}

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

#include "http-transport-tls.h"
#include "http-tunnel.h"
#include "net/tls/tls-context.h"
#include "net/tls/tls-stream.h"

#include <stdlib.h>

typedef struct {
    tls_conn_t* conn;
    tls_ctx_t*  ctx; /* Connections borrow the context: keep it until close. */
} _http_tls_conn_t;

static int _http_tls_conn_read_cb(void* p, void* b, int n) {
    return tls_read(((_http_tls_conn_t*)p)->conn, b, n);
}

static int _http_tls_conn_write_cb(void* p, const void* b, int n) {
    return tls_write(((_http_tls_conn_t*)p)->conn, b, n);
}

static void _http_tls_conn_read_deadline_cb(void* p, uint64_t ms) {
    tls_set_read_deadline(((_http_tls_conn_t*)p)->conn, ms);
}

static void _http_tls_conn_write_deadline_cb(void* p, uint64_t ms) {
    tls_set_write_deadline(((_http_tls_conn_t*)p)->conn, ms);
}

static void _http_tls_conn_close_cb(void* p) {
    if (!p) {
        return;
    }
    _http_tls_conn_t* c = p;
    tls_destroy(c->conn);
    tls_ctx_destroy(c->ctx);
    free(c);
}

http_transport_t http_tls_dial(
    const char*                  host,
    uint16_t                     port,
    uint64_t                     timeout_ms,
    const xylem_http_cli_opts_t* options) {
    _http_tls_conn_t* c =
        (_http_tls_conn_t*)calloc(1, sizeof(_http_tls_conn_t));
    if (!c) {
        return (http_transport_t){0};
    }
    c->ctx = tls_ctx_create();
    if (!c->ctx) {
        free(c);
        return (http_transport_t){0};
    }
    const xylem_http_tls_t* config = options ? options->tls : NULL;
    if (config) {
        if ((config->ca && tls_ctx_load_ca(c->ctx, config->ca) != 0) ||
            (!!config->cert != !!config->key) ||
            (config->cert &&
             tls_ctx_load_cert(c->ctx, NULL, config->cert, config->key) != 0)) {
            _http_tls_conn_close_cb(c);
            return (http_transport_t){0};
        }
        tls_ctx_verify_server(c->ctx, !config->skip_verify);
    }
    xylem_tls_opts_t opts = {
        .server_name        = host,
        .connect_timeout_ms = timeout_ms};
    const xylem_http_proxy_t* proxy = options ? options->proxy : NULL;
    if (proxy && proxy->host) {
        uint64_t deadline =
            timeout_ms
                ? xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms
                : 0;
        platform_sock_t fd = http_tunnel_connect(
            proxy->host,
            proxy->port,
            host,
            port,
            timeout_ms,
            proxy->username,
            proxy->password);
        if (fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (deadline && now >= deadline) {
                platform_socket_close(fd);
            } else {
                if (deadline) {
                    opts.connect_timeout_ms = deadline - now;
                }
                c->conn = tls_client_handshake_fd(fd, c->ctx, &opts);
            }
        }
    } else {
        c->conn = tls_dial(host, port, c->ctx, &opts);
    }
    if (!c->conn) {
        _http_tls_conn_close_cb(c);
        return (http_transport_t){0};
    }
    return (http_transport_t){
        .conn            = c,
        .read            = _http_tls_conn_read_cb,
        .write           = _http_tls_conn_write_cb,
        .close           = _http_tls_conn_close_cb,
        .set_rd_deadline = _http_tls_conn_read_deadline_cb,
        .set_wr_deadline = _http_tls_conn_write_deadline_cb};
}

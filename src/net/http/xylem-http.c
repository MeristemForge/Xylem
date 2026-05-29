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

#include "xylem/net/xylem-http.h"
#include "xylem/net/xylem-tcp.h"

#include "http.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

static http_transport_t _http_make_transport(xylem_tcp_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))xylem_tcp_read,
        .write           = (int (*)(void*, const void*, int))xylem_tcp_write,
        .close           = (void (*)(void*))xylem_tcp_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline,
    };
}

static http_transport_t _http_dial(const char* host, uint16_t port,
                                   uint64_t timeout_ms, void* ctx) {
    (void)ctx;
    xylem_tcp_conn_t* conn = xylem_tcp_dial(host, port, timeout_ms, NULL);
    if (!conn) {
        return (http_transport_t){0};
    }
    return _http_make_transport(conn);
}

static void _http_accept_coroutine(void* arg) {
    http_srv_t* srv = (http_srv_t*)arg;

    for (;;) {
        xylem_tcp_conn_t* conn =
            xylem_tcp_accept((xylem_tcp_listener_t*)srv->listener);
        if (!conn) {
            break;
        }

        http_srv_conn_ctx_t* ctx =
            (http_srv_conn_ctx_t*)calloc(1, sizeof(*ctx));
        if (!ctx) {
            xylem_tcp_close(conn);
            continue;
        }
        ctx->srv = srv;
        ctx->transport = _http_make_transport(conn);

        runtime_spawn(http_srv_conn_coroutine, ctx);
    }
}

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

    /* port 0 lets the OS assign; resolve for xylem_http_srv_port(). */
    char host_buf[46];
    uint16_t actual_port = 0;
    xylem_tcp_listener_addr(ln, host_buf, sizeof(host_buf), &actual_port);
    srv->port = actual_port;

    runtime_spawn(_http_accept_coroutine, srv);

    return (xylem_http_srv_t*)srv;
}

void xylem_http_close(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    http_srv_t* s = (http_srv_t*)srv;
    /* Wakes the accept coroutine which then exits on NULL return. */
    s->close_listener(s->listener);
    free(s);
}

uint16_t xylem_http_srv_port(xylem_http_srv_t* srv) {
    return srv ? ((http_srv_t*)srv)->port : 0;
}

xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_opts_t* opts) {
    return http_do_request("GET", url, NULL, 0, NULL, opts,
                           _http_dial, NULL);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_opts_t* opts) {
    return http_do_request("POST", url, body, body_len, content_type, opts,
                           _http_dial, NULL);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_opts_t* opts) {
    return http_do_request("PUT", url, body, body_len, content_type, opts,
                           _http_dial, NULL);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_opts_t* opts) {
    return http_do_request("DELETE", url, NULL, 0, NULL, opts,
                           _http_dial, NULL);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_opts_t* opts) {
    return http_do_request("PATCH", url, body, body_len, content_type, opts,
                           _http_dial, NULL);
}

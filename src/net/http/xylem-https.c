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

#include "http.h"
#include "xylem/net/xylem-https.h"
#include "xylem/net/xylem-tls.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

static http_transport_t _https_make_transport(xylem_tls_conn_t* conn) {
    return (http_transport_t){
        .conn           = conn,
        .read           = (int (*)(void*, void*, int))xylem_tls_read,
        .write          = (int (*)(void*, const void*, int))xylem_tls_write,
        .close          = (void (*)(void*))xylem_tls_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tls_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tls_set_write_deadline,
    };
}

static http_transport_t _https_dial(const char* host, uint16_t port,
                                    uint64_t timeout_ms, void* ctx) {
    xylem_tls_ctx_t* tls_ctx = (xylem_tls_ctx_t*)ctx;
    bool owns_ctx = false;
    if (!tls_ctx) {
        tls_ctx = xylem_tls_ctx_create();
        if (!tls_ctx) {
            return (http_transport_t){0};
        }
        owns_ctx = true;
    }
    xylem_tls_opts_t tls_opts = {0};
    tls_opts.server_name = host;
    tls_opts.handshake_timeout_ms = (timeout_ms > 0) ? timeout_ms : 10000;
    xylem_tls_conn_t* conn = xylem_tls_dial(host, port, tls_ctx, &tls_opts);
    if (owns_ctx) {
        xylem_tls_ctx_destroy(tls_ctx);
    }
    if (!conn) {
        return (http_transport_t){0};
    }
    return _https_make_transport(conn);
}

static void _https_accept_coroutine(void* arg) {
    http_srv_t* srv = (http_srv_t*)arg;

    for (;;) {
        xylem_tls_conn_t* conn =
            xylem_tls_accept((xylem_tls_listener_t*)srv->listener);
        if (!conn) {
            break;
        }

        http_srv_conn_ctx_t* ctx =
            (http_srv_conn_ctx_t*)calloc(1, sizeof(*ctx));
        if (!ctx) {
            xylem_tls_close(conn);
            continue;
        }
        ctx->srv = srv;
        ctx->transport = _https_make_transport(conn);

        runtime_spawn(http_srv_conn_coroutine, ctx);
    }
}

xylem_https_srv_t* xylem_https_listen(
    const char*                        host,
    uint16_t                           port,
    xylem_http_handler_fn_t            handler,
    void*                              userdata,
    xylem_tls_ctx_t*                   tls_ctx,
    const xylem_https_srv_opts_t* opts) {

    if ((!handler && !(opts && opts->on_upgrade)) || !tls_ctx) {
        return NULL;
    }

    xylem_tls_listener_t* ln = xylem_tls_listen(host, port, tls_ctx, NULL);
    if (!ln) {
        return NULL;
    }

    http_srv_t* srv = (http_srv_t*)calloc(1, sizeof(*srv));
    if (!srv) {
        xylem_tls_close_listener(ln);
        return NULL;
    }
    srv->listener       = ln;
    srv->close_listener = (void (*)(void*))xylem_tls_close_listener;
    srv->handler        = handler;
    srv->userdata       = userdata;
    if (opts) {
        srv->on_upgrade       = opts->on_upgrade;
        srv->upgrade_userdata = opts->upgrade_userdata;
    }

    /* port 0 lets the OS assign; resolve for xylem_https_srv_port(). */
    char host_buf[46];
    uint16_t actual_port = 0;
    xylem_tls_listener_addr(ln, host_buf, sizeof(host_buf), &actual_port);
    srv->port = actual_port;

    runtime_spawn(_https_accept_coroutine, srv);

    return (xylem_https_srv_t*)srv;
}

void xylem_https_close(xylem_https_srv_t* srv) {
    if (!srv) {
        return;
    }
    http_srv_t* s = (http_srv_t*)srv;
    s->close_listener(s->listener);
    /* Wakes the accept coroutine which then exits on NULL return. */
    free(s);
}

uint16_t xylem_https_srv_port(xylem_https_srv_t* srv) {
    return srv ? ((http_srv_t*)srv)->port : 0;
}

void xylem_https_srv_set_gzip(xylem_https_srv_t* srv,
                                   const xylem_http_gzip_opts_t* opts) {
    if (!srv || !opts) {
        return;
    }
    ((http_srv_t*)srv)->gzip_opts = *opts;
}

xylem_http_res_t* xylem_https_get(const char* url,
                                  xylem_tls_ctx_t* tls_ctx,
                                  const xylem_http_opts_t* opts) {
    return http_do_request("GET", url, NULL, 0, NULL, opts,
                           _https_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_post(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   xylem_tls_ctx_t* tls_ctx,
                                   const xylem_http_opts_t* opts) {
    return http_do_request("POST", url, body, body_len, content_type, opts,
                           _https_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_put(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  xylem_tls_ctx_t* tls_ctx,
                                  const xylem_http_opts_t* opts) {
    return http_do_request("PUT", url, body, body_len, content_type, opts,
                           _https_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_delete(const char* url,
                                     xylem_tls_ctx_t* tls_ctx,
                                     const xylem_http_opts_t* opts) {
    return http_do_request("DELETE", url, NULL, 0, NULL, opts,
                           _https_dial, tls_ctx);
}

xylem_http_res_t* xylem_https_patch(const char* url,
                                    const void* body, size_t body_len,
                                    const char* content_type,
                                    xylem_tls_ctx_t* tls_ctx,
                                    const xylem_http_opts_t* opts) {
    return http_do_request("PATCH", url, body, body_len, content_type, opts,
                           _https_dial, tls_ctx);
}

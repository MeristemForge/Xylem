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

#include "xylem/net/xylem-wss.h"

#include "xylem/net/http/xylem-http.h"
#include "xylem/net/xylem-tls.h"

#include "ws.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>


static int _wss_parse_url(const char* url, char* host, size_t host_cap,
                          uint16_t* port, char* path, size_t path_cap) {
    if (!url || strncmp(url, "wss://", 6) != 0) {
        return -1;
    }
    const char* p = url + 6;

    const char* colon = NULL;
    const char* slash = strchr(p, '/');
    const char* host_end = slash ? slash : p + strlen(p);

    for (const char* c = p; c < host_end; c++) {
        if (*c == ':') {
            colon = c;
            break;
        }
    }

    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (hlen >= host_cap) {
        return -1;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = 443;
    if (colon) {
        *port = (uint16_t)strtol(colon + 1, NULL, 10);
    }

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= path_cap) {
            return -1;
        }
        memcpy(path, slash, plen + 1);
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}


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


xylem_ws_conn_t* xylem_wss_dial(const char* url,
                                 xylem_tls_ctx_t* tls_ctx,
                                 const xylem_ws_opts_t* opts) {
    char host[256], path[1024];
    uint16_t port;
    if (_wss_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        return NULL;
    }

    uint64_t timeout = (opts && opts->handshake_timeout_ms)
                       ? opts->handshake_timeout_ms
                       : WS_DEFAULT_HANDSHAKE_TIMEOUT;

    bool owns_ctx = false;
    if (!tls_ctx) {
        tls_ctx = xylem_tls_ctx_create();
        if (!tls_ctx) {
            return NULL;
        }
        owns_ctx = true;
    }

    xylem_tls_opts_t tls_opts = {0};
    tls_opts.server_name = host;
    tls_opts.handshake_timeout_ms = timeout;

    xylem_tls_conn_t* tls = xylem_tls_dial(host, port, tls_ctx, &tls_opts);
    if (owns_ctx) {
        xylem_tls_ctx_destroy(tls_ctx);
    }
    if (!tls) {
        return NULL;
    }

    http_transport_t transport = _wss_make_tls_transport(tls);
    return ws_dial_impl(transport, host, port, path, opts);
}


xylem_ws_conn_t* xylem_wss_accept(struct xylem_http_res_s* res,
                                   struct xylem_http_req_s* req,
                                   const xylem_ws_opts_t* opts) {
    return ws_accept_impl(res, req, opts);
}


typedef struct wss_listener_s {
    xylem_http_srv_t*      https_srv;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
    xylem_ws_opts_t        opts;
} wss_listener_t;

typedef struct _wss_conn_ctx_s {
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
    if (!conn) {
        return;
    }

    _wss_conn_ctx_t* ctx = (_wss_conn_ctx_t*)malloc(sizeof(*ctx));
    if (!ctx) {
        xylem_ws_close(conn, 1011, NULL, 0);
        return;
    }
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
    if (!handler || !tls_ctx) {
        return NULL;
    }

    wss_listener_t* l = (wss_listener_t*)calloc(1, sizeof(*l));
    if (!l) {
        return NULL;
    }
    l->handler  = handler;
    l->userdata = userdata;
    if (opts) {
        l->opts = *opts;
    }

    xylem_http_srv_opts_t srv_opts = {0};
    srv_opts.on_upgrade       = _wss_upgrade_handler;
    srv_opts.upgrade_userdata = l;
    srv_opts.tls_ctx          = tls_ctx;

    l->https_srv = xylem_https_listen(host, port, NULL, NULL, &srv_opts);
    if (!l->https_srv) {
        free(l);
        return NULL;
    }

    return (xylem_wss_listener_t*)l;
}

void xylem_wss_close_listener(xylem_wss_listener_t* listener) {
    if (!listener) {
        return;
    }
    wss_listener_t* l = (wss_listener_t*)listener;
    xylem_http_close(l->https_srv);
    free(l);
}

uint16_t xylem_wss_listener_port(xylem_wss_listener_t* listener) {
    if (!listener) {
        return 0;
    }
    wss_listener_t* l = (wss_listener_t*)listener;
    uint16_t port = 0;
    xylem_http_srv_addr(l->https_srv, NULL, 0, &port);
    return port;
}

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

#include "xylem/net/http/xylem-http.h"

#include "ws.h"
#include "ws-tcp.h"
#include "ws-tls.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>


static int _ws_parse_url(const char* url, bool* is_tls,
                         char* host, size_t host_cap,
                         uint16_t* port, char* path, size_t path_cap) {
    if (!url) {
        return -1;
    }

    uint16_t default_port;
    const char* p;
    if (strncmp(url, "wss://", 6) == 0) {
        *is_tls      = true;
        default_port = 443;
        p            = url + 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        *is_tls      = false;
        default_port = 80;
        p            = url + 5;
    } else {
        return -1;
    }

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

    *port = default_port;
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


xylem_ws_conn_t* xylem_ws_dial(const char* url, const xylem_ws_opts_t* opts) {
    char host[256], path[1024];
    uint16_t port;
    bool is_tls;
    if (_ws_parse_url(url, &is_tls, host, sizeof(host),
                      &port, path, sizeof(path)) != 0) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_dial");

    /**
     * Dispatch on scheme to the matching dial factory: wss -> ws_tls_dial
     * (ws-tls.c, NULL stub when TLS is off), ws -> ws_tcp_dial (ws-tcp.c).
     */
    if (is_tls) {
        return ws_tls_dial(host, port, path, opts ? opts->tls : NULL, opts);
    }
    return ws_tcp_dial(host, port, path, opts);
}


xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                  struct xylem_http_req_s* req,
                                  const xylem_ws_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_accept");

    return ws_accept_impl(res, req, opts);
}


typedef struct ws_listener_s {
    xylem_http_srv_t*      http_srv;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
    xylem_ws_opts_t        opts;
} ws_listener_t;

typedef struct _ws_conn_ctx_s {
    xylem_ws_conn_t*       conn;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
} _ws_conn_ctx_t;

static void _ws_conn_coroutine(void* arg) {
    _ws_conn_ctx_t* ctx = (_ws_conn_ctx_t*)arg;
    ctx->conn->_standalone = true;
    ctx->handler(ctx->conn, ctx->userdata);
    if (!ctx->conn->close_sent) {
        xylem_ws_close(ctx->conn, 1000, NULL, 0);
    }
    ws_conn_free(ctx->conn);
    free(ctx);
}

static void _ws_upgrade_handler(xylem_http_res_t* res, xylem_http_req_t* req,
                                void* ud) {
    ws_listener_t* l = (ws_listener_t*)ud;
    xylem_ws_conn_t* conn = ws_accept_impl(res, req, &l->opts);
    if (!conn) {
        return;
    }

    _ws_conn_ctx_t* ctx = (_ws_conn_ctx_t*)malloc(sizeof(*ctx));
    if (!ctx) {
        xylem_ws_close(conn, 1011, NULL, 0);
        return;
    }
    ctx->conn     = conn;
    ctx->handler  = l->handler;
    ctx->userdata = l->userdata;
    if (runtime_spawn(_ws_conn_coroutine, ctx) != 0) {
        xylem_ws_close(conn, 1011, NULL, 0);
        free(ctx);
    }
}

xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                      xylem_ws_handler_fn_t handler,
                                      void* userdata,
                                      const xylem_ws_opts_t* opts) {
    if (!handler) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_listen");

    ws_listener_t* l = (ws_listener_t*)calloc(1, sizeof(*l));
    if (!l) {
        return NULL;
    }
    l->handler  = handler;
    l->userdata = userdata;
    if (opts) {
        l->opts = *opts;
    }

    xylem_http_srv_opts_t srv_opts = {0};
    srv_opts.on_upgrade      = _ws_upgrade_handler;
    srv_opts.upgrade_userdata = l;

    /**
     * wss: translate the ws TLS config to the http server's cert config so
     * xylem_http_listen builds an HTTPS (TLS) listener. Plain ws leaves
     * srv_opts.tls NULL. The struct is kept alive for the listen call only;
     * http copies what it needs (paths are caller-owned strings).
     */
    xylem_http_tls_t http_tls;
    if (opts && opts->tls) {
        http_tls.cert        = opts->tls->cert;
        http_tls.key         = opts->tls->key;
        http_tls.ca          = opts->tls->ca;
        http_tls.skip_verify = opts->tls->skip_verify;
        srv_opts.tls         = &http_tls;
    }

    l->http_srv = xylem_http_listen(host, port, NULL, NULL, &srv_opts);
    if (!l->http_srv) { free(l); return NULL; }

    return (xylem_ws_listener_t*)l;
}

void xylem_ws_close_listener(xylem_ws_listener_t* listener) {
    if (!listener) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_close_listener");

    ws_listener_t* l = (ws_listener_t*)listener;
    xylem_http_close(l->http_srv);
    free(l);
}

uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener) {
    if (!listener) {
        return 0;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_listener_port");

    ws_listener_t* l = (ws_listener_t*)listener;
    uint16_t port = 0;
    xylem_http_srv_addr(l->http_srv, NULL, 0, &port);
    return port;
}

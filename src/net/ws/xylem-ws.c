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
#include "xylem/net/xylem-tcp.h"

#include "ws.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>


static int _ws_parse_url(const char* url, char* host, size_t host_cap,
                         uint16_t* port, char* path, size_t path_cap) {
    if (!url || strncmp(url, "ws://", 5) != 0) {
        return -1;
    }
    const char* p = url + 5;

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

    *port = 80;
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


static http_transport_t _ws_make_tcp_transport(xylem_tcp_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))xylem_tcp_read,
        .write           = (int (*)(void*, const void*, int))xylem_tcp_write,
        .close           = (void (*)(void*))xylem_tcp_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))xylem_tcp_set_write_deadline,
    };
}


xylem_ws_conn_t* xylem_ws_dial(const char* url, const xylem_ws_opts_t* opts) {
    char host[256], path[1024];
    uint16_t port;
    if (_ws_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        return NULL;
    }

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


xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                  struct xylem_http_req_s* req,
                                  const xylem_ws_opts_t* opts) {
    return ws_accept_impl(res, req, opts);
}


typedef struct {
    xylem_http_srv_t*      http_srv;
    xylem_ws_handler_fn_t  handler;
    void*                  userdata;
    xylem_ws_opts_t        opts;
} ws_listener_t;

typedef struct {
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
    runtime_spawn(_ws_conn_coroutine, ctx);
}

xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                      xylem_ws_handler_fn_t handler,
                                      void* userdata,
                                      const xylem_ws_opts_t* opts) {
    if (!handler) {
        return NULL;
    }

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

    l->http_srv = xylem_http_listen(host, port, NULL, NULL, &srv_opts);
    if (!l->http_srv) { free(l); return NULL; }

    return (xylem_ws_listener_t*)l;
}

void xylem_ws_close_listener(xylem_ws_listener_t* listener) {
    if (!listener) {
        return;
    }
    ws_listener_t* l = (ws_listener_t*)listener;
    xylem_http_close(l->http_srv);
    free(l);
}

uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener) {
    if (!listener) {
        return 0;
    }
    ws_listener_t* l = (ws_listener_t*)listener;
    uint16_t port = 0;
    xylem_http_srv_addr(l->http_srv, NULL, 0, &port);
    return port;
}

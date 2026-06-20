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
 * Plain-TCP transport factory for the HTTP engine. Builds an
 * http_transport_t over xylem_tcp and drives accept/dial. Always built.
 */

#include "http-transport-tcp.h"
#include "http-utils.h"

#include "xylem/net/xylem-tcp.h"

#include "runtime/runtime.h"

#include <stdlib.h>

static http_transport_t _http_make_transport(xylem_tcp_conn_t* conn) {
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

/**
 * When dialing through a plain-HTTP proxy, ctx carries the proxy
 * descriptor: the TCP connection targets the proxy (not the origin) and
 * the request line uses absolute-form (handled by http_req_serialize via
 * absolute_form). A NULL ctx means a direct connection to the origin.
 */
static http_transport_t _http_dial(const char* host, uint16_t port,
                                   uint64_t timeout_ms, void* ctx) {
    const xylem_http_proxy_t* proxy = (const xylem_http_proxy_t*)ctx;

    const char* dial_host = host;
    uint16_t    dial_port = port;
    if (proxy && proxy->host) {
        dial_host = proxy->host;
        dial_port = proxy->port;
    }

    xylem_tcp_conn_t* conn = xylem_tcp_dial(dial_host, dial_port,
                                            timeout_ms, NULL);
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
            (http_srv_conn_ctx_t*)malloc(sizeof(*ctx));
        if (!ctx) {
            xylem_tcp_close(conn);
            continue;
        }
        ctx->srv       = srv;
        ctx->transport = _http_make_transport(conn);
        ctx->transport.remote_addr(
            ctx->transport.conn, ctx->remote_host,
            sizeof(ctx->remote_host), &ctx->remote_port);

        /**
         * Count the connection before spawning it, so a concurrent
         * xylem_http_close that drains active_conns can never observe
         * zero while this conn still references srv.
         */
        atomic_fetch_add(&srv->active_conns, 1);
        if (runtime_spawn(http_srv_conn_coroutine, ctx) != 0) {
            ctx->transport.close(ctx->transport.conn);
            free(ctx);
            http_srv_unref(srv);
        }
    }

    /**
     * Drop the accept coroutine's own reference (taken in http_tcp_listen);
     * lets a draining close proceed once accept has stopped touching srv.
     */
    http_srv_unref(srv);
}

xylem_http_srv_t* http_tcp_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {

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
    http_srv_init(srv, opts);

    xylem_tcp_listener_addr(ln, srv->host, sizeof(srv->host), &srv->port);

    /**
     * Reference count starts at two: the owner handle (released by
     * xylem_http_close / xylem_http_shutdown) and the accept coroutine
     * (released when it returns). Each connection coroutine adds its own.
     */
    atomic_store(&srv->active_conns, 2);
    if (runtime_spawn(_http_accept_coroutine, srv) != 0) {
        xylem_tcp_close_listener(ln);
        free(srv);
        return NULL;
    }

    return (xylem_http_srv_t*)srv;
}

xylem_http_res_t* http_tcp_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {

    /**
     * Resolve the proxy: explicit opts->proxy wins, else the environment
     * (http_proxy / no_proxy). A plain-HTTP proxy forwards via absolute-
     * form, so dial the proxy and request absolute-form; no CONNECT
     * tunnel.
     */
    const xylem_http_proxy_t* proxy = opts ? opts->proxy : NULL;
    xylem_http_proxy_t* env_proxy = NULL;
    if (!proxy) {
        env_proxy = http_proxy_from_env(url);
        proxy = env_proxy;
    }

    bool absolute_form = (proxy && proxy->host);

    xylem_http_res_t* res = http_do_request(
        method, url, body, body_len, content_type,
        headers, header_count, opts, absolute_form, _http_dial,
        (void*)proxy);

    http_proxy_from_env_free(env_proxy);
    return res;
}

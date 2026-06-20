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
 * TLS transport factory for the HTTP engine. Builds an http_transport_t
 * over the internal TLS engine and drives accept/dial. Compiled only
 * when TLS is enabled; the stub (http-transport-tls-stub.c) replaces it
 * otherwise.
 *
 * This factory talks to the engine directly (tls.h, tls_* API) rather
 * than the public xylem_tls_* shim: it needs tls_client_handshake_fd to
 * run the client handshake over a proxy-tunnel fd, which is internal.
 */

#include "http-transport-tls.h"
#include "http-utils.h"
#include "http-tunnel.h"

#include "net/tls/tls.h"
#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

static http_transport_t _https_make_transport(tls_conn_t* conn) {
    return (http_transport_t){
        .conn            = conn,
        .read            = (int (*)(void*, void*, int))tls_read,
        .write           = (int (*)(void*, const void*, int))tls_write,
        .close           = (void (*)(void*))tls_close,
        .set_rd_deadline = (void (*)(void*, uint64_t))tls_set_read_deadline,
        .set_wr_deadline = (void (*)(void*, uint64_t))tls_set_write_deadline,
        .remote_addr     = (int (*)(void*, char*, size_t, uint16_t*))tls_remote_addr,
        .local_addr      = (int (*)(void*, char*, size_t, uint16_t*))tls_local_addr,
        .shutdown_wr     = NULL,
    };
}

typedef struct _https_dial_ctx_s {
    tls_ctx_t*                tls_ctx;
    const xylem_http_proxy_t* proxy;
} _https_dial_ctx_t;

static http_transport_t _https_dial(const char* host, uint16_t port,
                                    uint64_t timeout_ms, void* ctx) {
    _https_dial_ctx_t* dctx = (_https_dial_ctx_t*)ctx;
    tls_ctx_t* tls_ctx = dctx ? dctx->tls_ctx : NULL;
    const xylem_http_proxy_t* proxy = dctx ? dctx->proxy : NULL;

    bool owns_ctx = false;
    if (!tls_ctx) {
        tls_ctx = tls_ctx_create();
        if (!tls_ctx) {
            return (http_transport_t){0};
        }
        owns_ctx = true;
    }

    xylem_tls_opts_t tls_opts = {0};
    tls_opts.server_name = host;
    tls_opts.handshake_timeout_ms = (timeout_ms > 0) ? timeout_ms : 10000;

    tls_conn_t* conn = NULL;

    if (proxy && proxy->host) {
        platform_sock_t fd = http_tunnel_connect(
            proxy->host, proxy->port, host, port,
            timeout_ms, proxy->username, proxy->password);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            if (owns_ctx) {
                tls_ctx_destroy(tls_ctx);
            }
            return (http_transport_t){0};
        }
        conn = tls_client_handshake_fd(fd, tls_ctx, &tls_opts);
    } else {
        conn = tls_dial(host, port, tls_ctx, &tls_opts);
    }

    if (owns_ctx) {
        tls_ctx_destroy(tls_ctx);
    }
    if (!conn) {
        return (http_transport_t){0};
    }
    return _https_make_transport(conn);
}

static void _https_accept_coroutine(void* arg) {
    http_srv_t* srv = (http_srv_t*)arg;

    for (;;) {
        tls_conn_t* conn =
            tls_accept((tls_listener_t*)srv->listener);
        if (!conn) {
            break;
        }

        http_srv_conn_ctx_t* ctx =
            (http_srv_conn_ctx_t*)malloc(sizeof(*ctx));
        if (!ctx) {
            tls_close(conn);
            continue;
        }
        ctx->srv       = srv;
        ctx->transport = _https_make_transport(conn);
        ctx->transport.remote_addr(
            ctx->transport.conn, ctx->remote_host,
            sizeof(ctx->remote_host), &ctx->remote_port);

        /* Count the connection before spawning it (see the TCP transport). */
        atomic_fetch_add(&srv->active_conns, 1);
        if (runtime_spawn(http_srv_conn_coroutine, ctx) != 0) {
            ctx->transport.close(ctx->transport.conn);
            free(ctx);
            http_srv_unref(srv);
        }
    }

    /* Drop the accept coroutine's own reference (taken in http_tls_listen). */
    http_srv_unref(srv);
}

xylem_http_srv_t* http_tls_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {

    const xylem_http_tls_t* tls = opts ? opts->tls : NULL;
    if (!tls || !tls->cert || !tls->key) {
        return NULL;
    }

    tls_ctx_t* tls_ctx = tls_ctx_create();
    if (!tls_ctx) {
        return NULL;
    }
    if (tls_ctx_load_cert(tls_ctx, NULL, tls->cert, tls->key) != 0) {
        tls_ctx_destroy(tls_ctx);
        return NULL;
    }
    if (tls->ca) {
        tls_ctx_load_ca(tls_ctx, tls->ca);
        tls_ctx_verify_client(tls_ctx, true);
    }

    tls_listener_t* ln = tls_listen(host, port, tls_ctx, NULL);
    if (!ln) {
        tls_ctx_destroy(tls_ctx);
        return NULL;
    }

    http_srv_t* srv = (http_srv_t*)calloc(1, sizeof(*srv));
    if (!srv) {
        tls_close_listener(ln);
        tls_ctx_destroy(tls_ctx);
        return NULL;
    }
    srv->listener       = ln;
    srv->close_listener = (void (*)(void*))tls_close_listener;
    srv->transport_ctx  = tls_ctx;
    srv->transport_ctx_free = (void (*)(void*))tls_ctx_destroy;
    srv->handler        = handler;
    srv->userdata       = userdata;
    http_srv_init(srv, opts);

    tls_listener_addr(ln, srv->host, sizeof(srv->host), &srv->port);

    /**
     * Reference count starts at two: owner handle + accept coroutine
     * (see the TCP transport). Each connection coroutine adds its own.
     */
    atomic_store_explicit(&srv->active_conns, 2, memory_order_relaxed);
    if (runtime_spawn(_https_accept_coroutine, srv) != 0) {
        tls_close_listener(ln);
        tls_ctx_destroy(tls_ctx);
        free(srv);
        return NULL;
    }
    return (xylem_http_srv_t*)srv;
}

xylem_http_res_t* http_tls_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts) {

    tls_ctx_t* tls_ctx = NULL;
    bool owns_ctx = false;

    if (opts && opts->tls) {
        tls_ctx = tls_ctx_create();
        if (tls_ctx) {
            owns_ctx = true;
            if (opts->tls->ca) {
                tls_ctx_load_ca(tls_ctx, opts->tls->ca);
            }
            if (opts->tls->cert && opts->tls->key) {
                tls_ctx_load_cert(
                    tls_ctx, NULL, opts->tls->cert, opts->tls->key);
            }
            if (opts->tls->skip_verify) {
                tls_ctx_verify_server(tls_ctx, false);
            }
        }
    }

    const xylem_http_proxy_t* proxy = opts ? opts->proxy : NULL;
    xylem_http_proxy_t* env_proxy = NULL;
    if (!proxy) {
        env_proxy = http_proxy_from_env(url);
        proxy = env_proxy;
    }

    _https_dial_ctx_t dctx = {tls_ctx, proxy};
    xylem_http_res_t* res = http_do_request(
        method, url, body, body_len, content_type,
        headers, header_count, opts, false, _https_dial, &dctx);

    if (owns_ctx && tls_ctx) {
        tls_ctx_destroy(tls_ctx);
    }
    http_proxy_from_env_free(env_proxy);
    return res;
}

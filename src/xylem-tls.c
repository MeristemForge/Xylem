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

#include "xylem/xylem-tls.h"

#include "xylem/xylem-list.h"
#include "xylem/xylem-logger.h"

#include "platform/platform-socket.h"
#include "deprecated/c11-threads.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum TLS record payload (RFC 8446 section 5.1). */
#define TLS_RECORD_MAX_PLAINTEXT 16384

static int _tls_ex_data_idx = -1;
static once_flag _tls_ex_data_once = ONCE_FLAG_INIT;

static void _tls_init_ex_data(void) {
    _tls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

struct xylem_tls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;   /* wire-format ALPN for client protos */
    size_t   alpn_wire_len;
    FILE*    keylog_file;
};

struct xylem_tls_conn_s {
    SSL*                  ssl;
    BIO*                  read_bio;
    BIO*                  write_bio;
    xylem_tcp_conn_t*     tcp;
    xylem_tls_ctx_t*      ctx;
    xylem_tls_handler_t*  handler;
    xylem_tls_server_t*   server;
    void*                 userdata;
    bool                  handshake_done;
    bool                  closing;
    int                   close_err;
    const char*           close_errmsg;
    char*                 hostname;
    char                  alpn[256];
    xylem_list_node_t     server_node;
};

struct xylem_tls_server_s {
    xylem_tcp_server_t*   tcp_server;
    xylem_tls_ctx_t*      ctx;
    xylem_tls_handler_t*  handler;
    xylem_tls_opts_t      opts;
    xylem_loop_t*         loop;
    xylem_list_t          connections;
    void*                 userdata;
    bool                  closing;
};

static void _tls_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    xylem_tls_ctx_t* ctx =
        (xylem_tls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _tls_ex_data_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _tls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                               unsigned char* outlen,
                               const unsigned char* in,
                               unsigned int inlen, void* arg) {
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)arg;
    (void)ssl;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

xylem_tls_ctx_t* xylem_tls_ctx_create(void) {
    xylem_tls_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /**
     * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER is not needed here because we
     * use memory BIOs. SSL_write only writes into an in-memory buffer,
     * never directly to a socket, so it always completes in a single call
     * and never returns SSL_ERROR_WANT_WRITE during data transfer. The
     * flag is only necessary with socket BIOs where SSL_write may
     * partially complete and require a retry with the same buffer pointer.
     */

    /* Enable peer verification by default. */
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);

    /* Register ex_data index once for keylog callback to recover ctx. */
    call_once(&_tls_ex_data_once, _tls_init_ex_data);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _tls_ex_data_idx, ctx);

    return ctx;
}

void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}

int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }

    /* Close any previously opened keylog file. */
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
        ctx->keylog_file = NULL;
    }

    if (!path) {
        SSL_CTX_set_keylog_callback(ctx->ssl_ctx, NULL);
        return 0;
    }

    ctx->keylog_file = fopen(path, "a");
    if (!ctx->keylog_file) {
        return -1;
    }

    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tls_keylog_cb);
    return 0;
}

int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                            const char* cert, const char* key) {
    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert) != 1) {
        xylem_loge("tls ctx: failed to load cert %s", cert);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        xylem_loge("tls ctx: failed to load key %s", key);
        return -1;
    }
    return 0;
}

int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("tls ctx: failed to load CA %s", ca_file);
        return -1;
    }
    return 0;
}

void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable) {
    int mode = enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx->ssl_ctx, mode, NULL);
}

int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                           const char** protocols, size_t count) {
    /* Build wire-format: each protocol prefixed by its length byte. */
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += 1 + strlen(protocols[i]);
    }

    uint8_t* wire = malloc(total);
    if (!wire) {
        return -1;
    }

    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t plen = strlen(protocols[i]);
        wire[off++] = (uint8_t)plen;
        memcpy(wire + off, protocols[i], plen);
        off += plen;
    }

    free(ctx->alpn_wire);
    ctx->alpn_wire     = wire;
    ctx->alpn_wire_len = total;

    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _tls_alpn_select_cb, ctx);

    return 0;
}

static void _tls_flush_write_bio(xylem_tls_conn_t* tls) {
    char buf[TLS_RECORD_MAX_PLAINTEXT];
    int  n;

    while ((n = BIO_read(tls->write_bio, buf, sizeof(buf))) > 0) {
        xylem_tcp_send(tls->tcp, buf, (size_t)n);
    }
}

static void _tls_feed_read_bio(xylem_tls_conn_t* tls, void* data, size_t len) {
    BIO_write(tls->read_bio, data, (int)len);
}

static void _tls_do_handshake(xylem_tls_conn_t* tls) {
    int rc  = SSL_do_handshake(tls->ssl);
    int err = SSL_get_error(tls->ssl, rc);

    if (rc == 1) {
        tls->handshake_done = true;
        _tls_flush_write_bio(tls);

        /* Cache negotiated ALPN as a null-terminated string. */
        const unsigned char* alpn_proto = NULL;
        unsigned int         alpn_len   = 0;
        SSL_get0_alpn_selected(tls->ssl, &alpn_proto, &alpn_len);
        if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(tls->alpn)) {
            memcpy(tls->alpn, alpn_proto, alpn_len);
            tls->alpn[alpn_len] = '\0';
        }

        if (tls->server) {
            xylem_logi("tls conn %p handshake complete (server)", (void*)tls);
            if (tls->handler && tls->handler->on_accept) {
                tls->handler->on_accept(tls->server, tls);
            }
        } else {
            xylem_logi("tls conn %p handshake complete (client)", (void*)tls);
            if (tls->handler && tls->handler->on_connect) {
                tls->handler->on_connect(tls);
            }
        }
        return;
    }

    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        _tls_flush_write_bio(tls);
        return;
    }

    /* Flush any pending alert before tearing down the TCP connection. */
    unsigned long ssl_err_code = ERR_peek_error();
    const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
    xylem_logw("tls conn %p handshake failed ssl_err=%d (%s)",
               (void*)tls, err,
               ssl_err_str ? ssl_err_str : "unknown");
    tls->close_err    = err;
    tls->close_errmsg = ssl_err_str ? ssl_err_str : "handshake failed";
    _tls_flush_write_bio(tls);
    xylem_tls_close(tls);
}

static int _tls_init_ssl(xylem_tls_conn_t* tls) {
    tls->ssl = SSL_new(tls->ctx->ssl_ctx);
    if (!tls->ssl) {
        xylem_loge("tls conn %p SSL_new failed", (void*)tls);
        return -1;
    }

    tls->read_bio  = BIO_new(BIO_s_mem());
    tls->write_bio = BIO_new(BIO_s_mem());
    if (!tls->read_bio || !tls->write_bio) {
        xylem_loge("tls conn %p BIO_new failed", (void*)tls);
        /* BIO_free accepts NULL safely. */
        BIO_free(tls->read_bio);
        BIO_free(tls->write_bio);
        SSL_free(tls->ssl);
        tls->ssl       = NULL;
        tls->read_bio  = NULL;
        tls->write_bio = NULL;
        return -1;
    }

    SSL_set_bio(tls->ssl, tls->read_bio, tls->write_bio);
    return 0;
}

static void _tls_tcp_connect_cb(xylem_tcp_conn_t* conn) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(conn);

    xylem_logd("tls conn %p tcp connected, starting handshake", (void*)tls);

    if (_tls_init_ssl(tls) != 0) {
        xylem_tcp_close(conn);
        return;
    }

    SSL_set_connect_state(tls->ssl);

    if (tls->hostname) {
        SSL_set_tlsext_host_name(tls->ssl, tls->hostname);
        SSL_set1_host(tls->ssl, tls->hostname);
    }

    _tls_do_handshake(tls);
}

static void _tls_tcp_accept_cb(xylem_tcp_server_t* tcp_server,
                               xylem_tcp_conn_t* conn) {
    xylem_tls_server_t* server =
        (xylem_tls_server_t*)xylem_tcp_server_get_userdata(tcp_server);

    xylem_tls_conn_t* tls = calloc(1, sizeof(*tls));
    if (!tls) {
        xylem_loge("tls server accept: conn alloc failed");
        xylem_tcp_set_userdata(conn, NULL);
        xylem_tcp_close(conn);
        return;
    }

    tls->tcp     = conn;
    tls->ctx     = server->ctx;
    tls->handler = server->handler;
    tls->server  = server;

    xylem_tcp_set_userdata(conn, tls);

    xylem_list_insert_tail(&server->connections, &tls->server_node);

    if (_tls_init_ssl(tls) != 0) {
        xylem_tcp_close(conn);
        return;
    }

    SSL_set_accept_state(tls->ssl);
    _tls_do_handshake(tls);
}

static void _tls_tcp_read_cb(xylem_tcp_conn_t* conn,
                             void* data, size_t len) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(conn);

    xylem_logd("tls conn %p tcp_read_cb len=%zu handshake_done=%d",
               (void*)tls, len, tls->handshake_done);

    _tls_feed_read_bio(tls, data, len);

    if (!tls->handshake_done) {
        _tls_do_handshake(tls);
        if (!tls->handshake_done) {
            return;
        }
    }
    if (tls->closing) {
        return;
    }
    char buf[TLS_RECORD_MAX_PLAINTEXT];
    int  n;

    while ((n = SSL_read(tls->ssl, buf, sizeof(buf))) > 0) {
        if (tls->handler && tls->handler->on_read) {
            tls->handler->on_read(tls, buf, (size_t)n);
        }
        if (tls->closing) {
            return;
        }
    }

    int err = SSL_get_error(tls->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
        xylem_logi("tls conn %p peer sent close_notify", (void*)tls);
        xylem_tls_close(tls);
        return;
    }

    if (err == SSL_ERROR_WANT_WRITE) {
        /* Renegotiation needs to send data; flush and wait for next read. */
        _tls_flush_write_bio(tls);
        return;
    }

    if (err != SSL_ERROR_WANT_READ) {
        unsigned long ssl_err_code = ERR_peek_error();
        const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
        xylem_logw("tls conn %p SSL_read error=%d (%s)",
                   (void*)tls, err,
                   ssl_err_str ? ssl_err_str : "unknown");
        tls->close_err    = err;
        tls->close_errmsg = ssl_err_str ? ssl_err_str : "unknown";
        xylem_tls_close(tls);
    }
}

/* Post callback: free a TLS handle after the current iteration. */
static void _tls_free_cb(xylem_loop_t* loop,
                         xylem_loop_post_t* req,
                         void* ud) {
    (void)loop;
    (void)req;
    free(ud);
}

static void _tls_tcp_close_cb(xylem_tcp_conn_t* conn, int err,
                              const char* errmsg) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(conn);
    if (!tls) {
        return;
    }

    xylem_logd("tls conn %p close err=%d (%s)",
               (void*)tls, err, errmsg);

    if (tls->server) {
        xylem_list_remove(&tls->server->connections, &tls->server_node);
        tls->server = NULL;
    }

    if (tls->handler && tls->handler->on_close) {
        int         ce = tls->close_err    ? tls->close_err    : err;
        const char* cm = tls->close_errmsg ? tls->close_errmsg : errmsg;
        tls->handler->on_close(tls, ce, cm);
    }

    if (tls->ssl) {
        SSL_free(tls->ssl);
    }
    free(tls->hostname);

    /**
     * Defer free so callers higher on the stack (_tls_tcp_read_cb,
     * _tls_do_handshake) can still safely read tls->closing /
     * tls->handshake_done after a user callback triggers close.
     */
    xylem_loop_post(xylem_tcp_get_loop(conn), _tls_free_cb, tls);
}

static void _tls_tcp_timeout_cb(xylem_tcp_conn_t* conn,
                                xylem_tcp_timeout_type_t type) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(conn);

    xylem_logw("tls conn %p timeout type=%d", (void*)tls, (int)type);

    if (tls->handler && tls->handler->on_timeout) {
        tls->handler->on_timeout(tls, type);
    }
}

static void _tls_tcp_heartbeat_cb(xylem_tcp_conn_t* conn) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(conn);

    xylem_logw("tls conn %p heartbeat miss", (void*)tls);

    if (tls->handler && tls->handler->on_heartbeat_miss) {
        tls->handler->on_heartbeat_miss(tls);
    }
}

/**
 * Per-server TCP handler used for accepted connections.
 * The TLS server pointer is stored via xylem_tcp_server_set_userdata
 * and recovered in the accept callback.
 */
static xylem_tcp_handler_t _tls_tcp_server_handler = {
    .on_accept         = _tls_tcp_accept_cb,
    .on_read           = _tls_tcp_read_cb,
    .on_close          = _tls_tcp_close_cb,
    .on_timeout        = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};

static xylem_tcp_handler_t _tls_tcp_client_handler = {
    .on_connect        = _tls_tcp_connect_cb,
    .on_read           = _tls_tcp_read_cb,
    .on_close          = _tls_tcp_close_cb,
    .on_timeout        = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};

int xylem_tls_send(xylem_tls_conn_t* tls, const void* data, size_t len) {
    if (!tls->handshake_done || tls->closing) {
        xylem_logd("tls conn %p send rejected (handshake=%d closing=%d)",
                   (void*)tls, tls->handshake_done, tls->closing);
        return -1;
    }

    int n = SSL_write(tls->ssl, data, (int)len);
    if (n <= 0) {
        unsigned long ssl_err_code = ERR_peek_error();
        const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
        xylem_logw("tls conn %p SSL_write failed (%s)",
                   (void*)tls, ssl_err_str ? ssl_err_str : "unknown");
        return -1;
    }

    _tls_flush_write_bio(tls);

    /**
     * NOTE: on_write_done fires after ciphertext is enqueued into the
     * TCP write queue, not after it has been written to the socket.
     * This is intentional -- with memory BIOs, SSL_write always
     * completes in one call, and the TCP layer handles actual delivery.
     */
    if (tls->handler && tls->handler->on_write_done) {
        tls->handler->on_write_done(tls, data, len, 0);
    }

    return 0;
}

xylem_tls_conn_t* xylem_tls_dial(xylem_loop_t* loop,
                            xylem_addr_t* addr,
                            xylem_tls_ctx_t* ctx,
                            xylem_tls_handler_t* handler,
                            xylem_tls_opts_t* opts) {
    xylem_tls_conn_t* tls = calloc(1, sizeof(*tls));
    if (!tls) {
        return NULL;
    }

    tls->ctx     = ctx;
    tls->handler = handler;

    if (opts && opts->hostname) {
        tls->hostname = strdup(opts->hostname);
        if (!tls->hostname) {
            free(tls);
            return NULL;
        }
    }

    xylem_tcp_opts_t* tcp_opts = opts ? &opts->tcp : NULL;
    xylem_tcp_conn_t* tcp = xylem_tcp_dial(loop, addr,
                                           &_tls_tcp_client_handler,
                                           tcp_opts);
    if (!tcp) {
        free(tls->hostname);
        free(tls);
        return NULL;
    }

    tls->tcp = tcp;
    xylem_tcp_set_userdata(tcp, tls);
    return tls;
}

void xylem_tls_close(xylem_tls_conn_t* tls) {
    if (tls->closing) {
        return;
    }
    tls->closing = true;

    xylem_logi("tls conn %p close requested", (void*)tls);

    if (tls->ssl && tls->handshake_done) {
        SSL_shutdown(tls->ssl);
        _tls_flush_write_bio(tls);
    }

    xylem_tcp_close(tls->tcp);
}

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls) {
    return tls->alpn[0] ? tls->alpn : NULL;
}

void* xylem_tls_get_userdata(xylem_tls_conn_t* tls) {
    return tls->userdata;
}

const xylem_addr_t* xylem_tls_get_peer_addr(xylem_tls_conn_t* tls) {
    return xylem_tcp_get_peer_addr(tls->tcp);
}

xylem_loop_t* xylem_tls_get_loop(xylem_tls_conn_t* tls) {
    return xylem_tcp_get_loop(tls->tcp);
}

void xylem_tls_set_userdata(xylem_tls_conn_t* tls, void* ud) {
    tls->userdata = ud;
}

xylem_tls_server_t* xylem_tls_listen(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_tls_ctx_t* ctx,
                                     xylem_tls_handler_t* handler,
                                     xylem_tls_opts_t* opts) {
    xylem_tls_server_t* server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    server->ctx     = ctx;
    server->handler = handler;
    server->loop    = loop;
    xylem_list_init(&server->connections);

    if (opts) {
        server->opts = *opts;
    }

    xylem_tcp_opts_t* tcp_opts = opts ? &opts->tcp : NULL;
    xylem_tcp_server_t* tcp_server =
        xylem_tcp_listen(loop, addr, &_tls_tcp_server_handler, tcp_opts);
    if (!tcp_server) {
        free(server);
        return NULL;
    }

    server->tcp_server = tcp_server;
    xylem_tcp_server_set_userdata(tcp_server, server);

    xylem_logi("tls server %p listening", (void*)server);
    return server;
}

void* xylem_tls_server_get_userdata(xylem_tls_server_t* server) {
    return server->userdata;
}

void xylem_tls_server_set_userdata(xylem_tls_server_t* server, void* ud) {
    server->userdata = ud;
}

void xylem_tls_close_server(xylem_tls_server_t* server) {
    if (!server || server->closing) {
        return;
    }
    server->closing = true;

    xylem_logi("tls server %p closing", (void*)server);

    /**
     * Detach all TLS sessions from the server before closing them.
     * xylem_tls_close is async -- _tls_tcp_close_cb may fire after
     * server is freed, so tls->server must be NULL by then.
     *
     * We must remove the node from the list ourselves because setting
     * tls->server = NULL prevents _tls_tcp_close_cb from doing it.
     */
    while (!xylem_list_empty(&server->connections)) {
        xylem_list_node_t* node = xylem_list_head(&server->connections);
        xylem_tls_conn_t* tls = xylem_list_entry(node, xylem_tls_conn_t, server_node);
        xylem_list_remove(&server->connections, node);
        tls->server = NULL;
        xylem_tls_close(tls);
    }

    xylem_tcp_close_server(server->tcp_server);
    xylem_loop_post(server->loop, _tls_free_cb, server);
}

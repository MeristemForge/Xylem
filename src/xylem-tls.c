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

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct _tls_write_req_s {
    const void* data;
    size_t      len;
} _tls_write_req_t;

struct xylem_tls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;   /* wire-format ALPN for client protos */
    size_t   alpn_wire_len;
};

struct xylem_tls_s {
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
    char*                 hostname;
    _tls_write_req_t      pending_write;
    xylem_list_node_t     server_node;
};

struct xylem_tls_server_s {
    xylem_tcp_server_t*   tcp_server;
    xylem_tls_ctx_t*      ctx;
    xylem_tls_handler_t*  handler;
    xylem_tcp_opts_t      opts;
    xylem_loop_t*         loop;
    xylem_list_t          connections;
    bool                  closing;
};

/* ------------------------------------------------------------------ */
/* Forward declarations for internal TCP handler callbacks             */
/* ------------------------------------------------------------------ */

static void _tls_tcp_connect_cb(xylem_tcp_conn_t* conn);
static void _tls_tcp_accept_cb(xylem_tcp_conn_t* conn);
static void _tls_tcp_read_cb(xylem_tcp_conn_t* conn,
                             void* data, size_t len);
static void _tls_tcp_write_done_cb(xylem_tcp_conn_t* conn,
                                   void* data, size_t len, int status);
static void _tls_tcp_close_cb(xylem_tcp_conn_t* conn, int err);
static void _tls_tcp_timeout_cb(xylem_tcp_conn_t* conn,
                                xylem_tcp_timeout_type_t type);
static void _tls_tcp_heartbeat_cb(xylem_tcp_conn_t* conn);

/**
 * Per-server TCP handler used for accepted connections.
 * The TLS server pointer is stored via xylem_tcp_server_set_userdata
 * and recovered in the accept callback via xylem_tcp_conn_t's
 * server->userdata.
 */
static xylem_tcp_handler_t _tls_tcp_server_handler = {
    .on_accept        = _tls_tcp_accept_cb,
    .on_connect       = _tls_tcp_connect_cb,
    .on_read          = _tls_tcp_read_cb,
    .on_write_done    = _tls_tcp_write_done_cb,
    .on_close         = _tls_tcp_close_cb,
    .on_timeout       = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};

/* Client-side handler (no server context needed). */
static xylem_tcp_handler_t _tls_tcp_client_handler = {
    .on_connect       = _tls_tcp_connect_cb,
    .on_read          = _tls_tcp_read_cb,
    .on_write_done    = _tls_tcp_write_done_cb,
    .on_close         = _tls_tcp_close_cb,
    .on_timeout       = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};

/* ------------------------------------------------------------------ */
/* ALPN server select callback                                         */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Context API                                                         */
/* ------------------------------------------------------------------ */

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

    /* Enable peer verification by default. */
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    return ctx;
}

void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}

int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                            const char* cert, const char* key) {
    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert) != 1) {
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        return -1;
    }
    return 0;
}

int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_path) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_path, NULL) != 1) {
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

    /* Client side: set offered protocols. */
    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);

    /* Server side: set select callback. */
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _tls_alpn_select_cb, ctx);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Memory BIO helpers (Task 4)                                         */
/* ------------------------------------------------------------------ */

/* Read pending ciphertext from write BIO and send via TCP. */
static void _tls_flush_write_bio(xylem_tls_t* tls) {
    char buf[16384];
    int  n;

    while ((n = BIO_read(tls->write_bio, buf, sizeof(buf))) > 0) {
        xylem_tcp_send(tls->tcp, buf, (size_t)n);
    }
}

/* Feed received ciphertext into the read BIO. */
static void _tls_feed_read_bio(xylem_tls_t* tls, void* data, size_t len) {
    BIO_write(tls->read_bio, data, (int)len);
}

/* ------------------------------------------------------------------ */
/* Handshake state machine (Task 5)                                    */
/* ------------------------------------------------------------------ */

static void _tls_do_handshake(xylem_tls_t* tls) {
    int rc  = SSL_do_handshake(tls->ssl);
    int err = SSL_get_error(tls->ssl, rc);

    if (rc == 1) {
        /* Handshake complete. */
        tls->handshake_done = true;
        _tls_flush_write_bio(tls);

        if (tls->server) {
            if (tls->handler->on_accept) {
                tls->handler->on_accept(tls);
            }
        } else {
            if (tls->handler->on_connect) {
                tls->handler->on_connect(tls);
            }
        }
        return;
    }

    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        _tls_flush_write_bio(tls);
        return;
    }

    /* Handshake failure. */
    _tls_flush_write_bio(tls);
    xylem_tcp_close(tls->tcp);
}

/* Create SSL object with memory BIOs for a connection. */
static int _tls_init_ssl(xylem_tls_t* tls) {
    tls->ssl = SSL_new(tls->ctx->ssl_ctx);
    if (!tls->ssl) {
        return -1;
    }

    tls->read_bio  = BIO_new(BIO_s_mem());
    tls->write_bio = BIO_new(BIO_s_mem());
    if (!tls->read_bio || !tls->write_bio) {
        SSL_free(tls->ssl);
        tls->ssl = NULL;
        return -1;
    }

    SSL_set_bio(tls->ssl, tls->read_bio, tls->write_bio);
    return 0;
}

/* ------------------------------------------------------------------ */
/* TCP handler bridge callbacks (Tasks 5-7)                            */
/* ------------------------------------------------------------------ */

/* TCP connected — client side: init SSL, set SNI, start handshake. */
static void _tls_tcp_connect_cb(xylem_tcp_conn_t* conn) {
    xylem_tls_t* tls = (xylem_tls_t*)xylem_tcp_get_userdata(conn);

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

/* TCP accepted — server side: init SSL, start server handshake. */
static void _tls_tcp_accept_cb(xylem_tcp_conn_t* conn) {
    xylem_tls_server_t* server =
        (xylem_tls_server_t*)xylem_tcp_get_userdata(conn);

    xylem_tls_t* tls = calloc(1, sizeof(*tls));
    if (!tls) {
        xylem_tcp_close(conn);
        return;
    }

    tls->tcp     = conn;
    tls->ctx     = server->ctx;
    tls->handler = server->handler;
    tls->server  = server;

    /* Re-bind TCP userdata to the new TLS connection. */
    xylem_tcp_set_userdata(conn, tls);

    /* Track in server's connection list. */
    xylem_list_insert_tail(&server->connections, &tls->server_node);

    if (_tls_init_ssl(tls) != 0) {
        xylem_list_remove(&server->connections, &tls->server_node);
        free(tls);
        xylem_tcp_close(conn);
        return;
    }

    SSL_set_accept_state(tls->ssl);
    _tls_do_handshake(tls);
}

/* TCP data received — feed to BIO, then handshake or decrypt. */
static void _tls_tcp_read_cb(xylem_tcp_conn_t* conn,
                             void* data, size_t len) {
    xylem_tls_t* tls = (xylem_tls_t*)xylem_tcp_get_userdata(conn);

    _tls_feed_read_bio(tls, data, len);

    if (!tls->handshake_done) {
        _tls_do_handshake(tls);
        if (!tls->handshake_done) {
            return;
        }
        /* Handshake just completed — fall through to drain any
         * application data that arrived in the same TCP read. */
    }

    /* Post-handshake: decrypt and deliver plaintext. */
    char buf[16384];
    int  n;

    while ((n = SSL_read(tls->ssl, buf, sizeof(buf))) > 0) {
        if (tls->handler->on_read) {
            tls->handler->on_read(tls, buf, (size_t)n);
        }
    }

    int err = SSL_get_error(tls->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
        /* Peer sent close_notify. */
        xylem_tls_close(tls);
        return;
    }

    if (err != SSL_ERROR_WANT_READ) {
        xylem_tcp_close(tls->tcp);
    }
}

/* TCP write completed — bridge to TLS on_write_done. */
static void _tls_tcp_write_done_cb(xylem_tcp_conn_t* conn,
                                   void* data, size_t len, int status) {
    xylem_tls_t* tls = (xylem_tls_t*)xylem_tcp_get_userdata(conn);
    (void)data;
    (void)len;

    if (!tls->handshake_done) {
        return;
    }

    if (tls->pending_write.data && tls->handler->on_write_done) {
        tls->handler->on_write_done(tls, (void*)tls->pending_write.data,
                                    tls->pending_write.len, status);
        tls->pending_write.data = NULL;
        tls->pending_write.len  = 0;
    }
}

/* TCP closed — clean up SSL state, notify user. */
static void _tls_tcp_close_cb(xylem_tcp_conn_t* conn, int err) {
    xylem_tls_t* tls = (xylem_tls_t*)xylem_tcp_get_userdata(conn);

    if (tls->server) {
        xylem_list_remove(&tls->server->connections, &tls->server_node);
    }

    if (tls->handler->on_close) {
        tls->handler->on_close(tls, err);
    }

    if (tls->ssl) {
        SSL_free(tls->ssl);
    }
    free(tls->hostname);
    free(tls);
}

/* TCP timeout — forward to TLS handler. */
static void _tls_tcp_timeout_cb(xylem_tcp_conn_t* conn,
                                xylem_tcp_timeout_type_t type) {
    xylem_tls_t* tls = (xylem_tls_t*)xylem_tcp_get_userdata(conn);

    if (tls->handler->on_timeout) {
        tls->handler->on_timeout(tls, type);
    }
}

/* TCP heartbeat miss — forward to TLS handler. */
static void _tls_tcp_heartbeat_cb(xylem_tcp_conn_t* conn) {
    xylem_tls_t* tls = (xylem_tls_t*)xylem_tcp_get_userdata(conn);

    if (tls->handler->on_heartbeat_miss) {
        tls->handler->on_heartbeat_miss(tls);
    }
}

/* ------------------------------------------------------------------ */
/* Public connection API (Tasks 7-8)                                   */
/* ------------------------------------------------------------------ */

int xylem_tls_send(xylem_tls_t* tls, const void* data, size_t len) {
    if (!tls->handshake_done || tls->closing) {
        return -1;
    }

    int n = SSL_write(tls->ssl, data, (int)len);
    if (n <= 0) {
        return -1;
    }

    tls->pending_write.data = data;
    tls->pending_write.len  = len;

    _tls_flush_write_bio(tls);
    return 0;
}

xylem_tls_t* xylem_tls_dial(xylem_loop_t* loop,
                            xylem_addr_t* addr,
                            xylem_tls_ctx_t* ctx,
                            xylem_tls_handler_t* handler,
                            xylem_tcp_opts_t* opts) {
    xylem_tls_t* tls = calloc(1, sizeof(*tls));
    if (!tls) {
        return NULL;
    }

    tls->ctx     = ctx;
    tls->handler = handler;

    xylem_tcp_conn_t* tcp = xylem_tcp_dial(loop, addr,
                                           &_tls_tcp_client_handler, opts);
    if (!tcp) {
        free(tls);
        return NULL;
    }

    tls->tcp = tcp;
    xylem_tcp_set_userdata(tcp, tls);
    return tls;
}

void xylem_tls_close(xylem_tls_t* tls) {
    if (tls->closing) {
        return;
    }
    tls->closing = true;

    if (tls->ssl && tls->handshake_done) {
        SSL_shutdown(tls->ssl);
        _tls_flush_write_bio(tls);
    }

    xylem_tcp_close(tls->tcp);
}

int xylem_tls_set_hostname(xylem_tls_t* tls, const char* hostname) {
    free(tls->hostname);
    tls->hostname = strdup(hostname);
    if (!tls->hostname) {
        return -1;
    }
    return 0;
}

const char* xylem_tls_get_alpn(xylem_tls_t* tls) {
    if (!tls->ssl) {
        return NULL;
    }

    const unsigned char* proto = NULL;
    unsigned int         len   = 0;
    SSL_get0_alpn_selected(tls->ssl, &proto, &len);

    if (!proto || len == 0) {
        return NULL;
    }
    return (const char*)proto;
}

void* xylem_tls_get_userdata(xylem_tls_t* tls) {
    return tls->userdata;
}

void xylem_tls_set_userdata(xylem_tls_t* tls, void* ud) {
    tls->userdata = ud;
}

/* ------------------------------------------------------------------ */
/* Server API (Task 9)                                                 */
/* ------------------------------------------------------------------ */

xylem_tls_server_t* xylem_tls_listen(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_tls_ctx_t* ctx,
                                     xylem_tls_handler_t* handler,
                                     xylem_tcp_opts_t* opts) {
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

    xylem_tcp_server_t* tcp_server =
        xylem_tcp_listen(loop, addr, &_tls_tcp_server_handler, opts);
    if (!tcp_server) {
        free(server);
        return NULL;
    }

    server->tcp_server = tcp_server;
    xylem_tcp_server_set_userdata(tcp_server, server);
    return server;
}

void xylem_tls_close_server(xylem_tls_server_t* server) {
    if (server->closing) {
        return;
    }
    server->closing = true;

    /* Close all active TLS connections. */
    xylem_list_node_t* node = xylem_list_head(&server->connections);
    xylem_list_node_t* sentinel = xylem_list_sentinel(&server->connections);
    while (node && node != sentinel) {
        xylem_tls_t* tls = xylem_list_entry(node, xylem_tls_t, server_node);
        node = xylem_list_next(node);
        xylem_tls_close(tls);
    }

    xylem_tcp_close_server(server->tcp_server);
    free(server);
}

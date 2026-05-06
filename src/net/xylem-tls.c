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

#include "xylem/net/xylem-tls.h"
#include "runtime/runtime.h"
#include "addr.h"

#include "container/list.h"
#include "xylem/xylem-logger.h"
#include "container/queue.h"

#include "platform/platform-socket.h"
#include "c11-threads.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum TLS record payload (RFC 8446 section 5.1). */
#define TLS_RECORD_MAX_PLAINTEXT 16384

#define ERRMSG_UNKNOWN    "unknown"
#define ERRMSG_HS_FAILED  "handshake failed"

typedef enum {
    TLS_STATE_CONNECTING,
    TLS_STATE_CONNECTED,
    TLS_STATE_CLOSING,
    TLS_STATE_CLOSED,
} _tls_state_t;

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
    _tls_state_t          state;
    _Atomic int32_t       refcount;
    int                   close_err;
    const char*           close_errmsg;
    char*                 hostname;
    char                  alpn[256];
    list_node_t     server_node;
    queue_t         write_queue; /* pending TLS write requests */
};

/**
 * Tracks one xylem_tls_send call through the TCP write pipeline.
 * Zero-copy: stores the caller's pointer directly. The caller must
 * keep the buffer alive until on_write_done fires.
 */
typedef struct _tls_write_req_s {
    queue_node_t node;
    const void*        data; /* caller's original pointer */
    size_t             len;
} _tls_write_req_t;

struct xylem_tls_server_s {
    xylem_tcp_listener_t*   tcp_server;
    xylem_tls_ctx_t*      ctx;
    xylem_tls_handler_t*  handler;
    xylem_tls_opts_t      opts;
    loop_t*         loop;
    list_t          connections;
    void*                 userdata;
    bool                  closing;
};

static void _tls_destroy_conn(xylem_tls_conn_t* tls);

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
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)calloc(1, sizeof(xylem_tls_ctx_t));
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

    /* Enforce TLS 1.2 as minimum version. */
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

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

    uint8_t* wire = (uint8_t*)malloc(total);
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

/**
 * Flush ciphertext from write BIO to TCP. Returns the number of TCP sends.
 *
 * Each ciphertext chunk is heap-allocated because the underlying TCP layer
 * uses zero-copy sends -- it holds a pointer to the data until its own
 * on_write_done fires. The allocation is freed in _tls_tcp_write_done_cb.
 */
static bool _tls_flush_write_bio(xylem_tls_conn_t* tls) {
    size_t pending = BIO_ctrl_pending(tls->write_bio);
    if (pending == 0) {
        return false;
    }

    char* ct = (char*)malloc(pending);
    if (!ct) {
        xylem_loge("tls conn %p flush_write_bio: alloc failed",
                   (void*)tls);
        return false;
    }

    int n = BIO_read(tls->write_bio, ct, (int)pending);
    if (n <= 0) {
        free(ct);
        return false;
    }

    xylem_tcp_send(tls->tcp, ct, (size_t)n);
    return true;
}

static void _tls_feed_read_bio(xylem_tls_conn_t* tls, void* data, size_t len) {
    BIO_write(tls->read_bio, data, (int)len);
}

static void _tls_do_handshake(xylem_tls_conn_t* tls) {
    ERR_clear_error();
    int rc  = SSL_do_handshake(tls->ssl);
    int err = SSL_get_error(tls->ssl, rc);

    if (rc == 1) {
        tls->state = TLS_STATE_CONNECTED;
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
    tls->close_errmsg = ssl_err_str ? ssl_err_str : ERRMSG_HS_FAILED;
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

static void _tls_tcp_connect_cb(xylem_tcp_conn_t* tcp) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(tcp);

    xylem_logd("tls conn %p tcp connected, starting handshake", (void*)tls);

    if (_tls_init_ssl(tls) != 0) {
        xylem_tcp_close(tcp);
        return;
    }

    SSL_set_connect_state(tls->ssl);

    if (tls->hostname) {
        SSL_set_tlsext_host_name(tls->ssl, tls->hostname);
        SSL_set1_host(tls->ssl, tls->hostname);
    }

    _tls_do_handshake(tls);
}

static void _tls_tcp_accept_cb(xylem_tcp_listener_t* tcp_server,
                               xylem_tcp_conn_t* tcp) {
    xylem_tls_server_t* server =
        (xylem_tls_server_t*)xylem_tcp_listener_get_userdata(tcp_server);

    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)calloc(1, sizeof(xylem_tls_conn_t));
    if (!tls) {
        xylem_loge("tls server accept: conn alloc failed");
        xylem_tcp_set_userdata(tcp, NULL);
        xylem_tcp_close(tcp);
        return;
    }

    tls->tcp     = tcp;
    tls->ctx     = server->ctx;
    tls->handler = server->handler;
    tls->server  = server;
    atomic_store(&tls->refcount, 1);
    queue_init(&tls->write_queue);

    xylem_tcp_set_userdata(tcp, tls);

    list_insert_tail(&server->connections, &tls->server_node);

    if (_tls_init_ssl(tls) != 0) {
        xylem_tcp_close(tcp);
        return;
    }

    SSL_set_accept_state(tls->ssl);
    _tls_do_handshake(tls);
}

static void _tls_tcp_read_cb(xylem_tcp_conn_t* tcp,
                             void* data, size_t len) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(tcp);

    xylem_logd("tls conn %p tcp_read_cb len=%zu state=%d",
               (void*)tls, len, (int)tls->state);

    _tls_feed_read_bio(tls, data, len);

    if (tls->state == TLS_STATE_CONNECTING) {
        _tls_do_handshake(tls);
        if (tls->state != TLS_STATE_CONNECTED) {
            return;
        }
    }
    if (tls->state != TLS_STATE_CONNECTED) {
        return;
    }
    char buf[TLS_RECORD_MAX_PLAINTEXT];
    int  n;

    ERR_clear_error();
    while ((n = SSL_read(tls->ssl, buf, sizeof(buf))) > 0) {
        if (tls->handler && tls->handler->on_read) {
            tls->handler->on_read(tls, buf, (size_t)n);
        }
        if (tls->state != TLS_STATE_CONNECTED) {
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
        tls->close_errmsg = ssl_err_str ? ssl_err_str : ERRMSG_UNKNOWN;
        xylem_tls_close(tls);
    }
}

/* Decrement refcount; free the TLS handle when it reaches zero. */
static void _tls_conn_unref(xylem_tls_conn_t* tls) {
    if (atomic_fetch_sub(&tls->refcount, 1) == 1) {
        free(tls);
    }
}

/* Post callback: decrement refcount after the current iteration. */
static void _tls_free_cb(loop_t* loop,
                         loop_post_t* req,
                         void* ud) {
    (void)loop;
    (void)req;
    _tls_conn_unref((xylem_tls_conn_t*)ud);
}

static void _tls_tcp_close_cb(xylem_tcp_conn_t* tcp, int err,
                              const char* errmsg) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(tcp);
    if (!tls) {
        return;
    }

    tls->state = TLS_STATE_CLOSED;

    xylem_logd("tls conn %p close err=%d (%s)",
               (void*)tls, err, errmsg);

    /* Drain pending TLS write requests that will never complete. */
    queue_node_t* qn;
    while ((qn = xylem_queue_dequeue(&tls->write_queue)) != NULL) {
        _tls_write_req_t* req =
            queue_entry(qn, _tls_write_req_t, node);
        if (tls->handler && tls->handler->on_write_done) {
            tls->handler->on_write_done(tls, req->data, req->len, -1);
        }
        free(req);
    }

    if (tls->server) {
        list_remove(&tls->server->connections, &tls->server_node);
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

    loop_post(runtime_get_loop(), _tls_free_cb, tls);
}

static void _tls_tcp_timeout_cb(xylem_tcp_conn_t* tcp,
                                xylem_tcp_timeout_type_t type) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(tcp);
    if (!tls || tls->state != TLS_STATE_CONNECTED) {
        return;
    }

    xylem_logw("tls conn %p timeout type=%d", (void*)tls, (int)type);

    if (tls->handler && tls->handler->on_timeout) {
        tls->handler->on_timeout(tls, type);
    }
}

static void _tls_tcp_heartbeat_cb(xylem_tcp_conn_t* tcp) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(tcp);
    if (!tls || tls->state != TLS_STATE_CONNECTED) {
        return;
    }

    xylem_logw("tls conn %p heartbeat miss", (void*)tls);

    if (tls->handler && tls->handler->on_heartbeat_miss) {
        tls->handler->on_heartbeat_miss(tls);
    }
}

/**
 * TCP on_write_done callback. Each TLS send produces exactly one
 * xylem_tcp_send (the full ciphertext from BIO_ctrl_pending). When
 * the TCP write completes we dequeue the front write request and
 * fire the TLS on_write_done.
 *
 * Handshake flushes also produce TCP writes, but the write_queue is
 * empty during handshake so those are silently ignored.
 *
 * The data pointer is a heap-allocated ciphertext buffer from
 * _tls_flush_write_bio and must be freed here.
 */
static void _tls_tcp_write_done_cb(xylem_tcp_conn_t* tcp,
                                   const void* data, size_t len,
                                   int status) {
    (void)len;
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)xylem_tcp_get_userdata(tcp);

    free((void*)data);

    if (!tls) {
        return;
    }

    queue_node_t* front = xylem_queue_front(&tls->write_queue);
    if (!front) {
        return;
    }

    _tls_write_req_t* req =
        queue_entry(front, _tls_write_req_t, node);
    xylem_queue_dequeue(&tls->write_queue);

    if (tls->handler && tls->handler->on_write_done) {
        tls->handler->on_write_done(tls, req->data, req->len, status);
    }
    free(req);

    if (tls->state == TLS_STATE_CLOSING &&
        queue_empty(&tls->write_queue)) {
        _tls_destroy_conn(tls);
    }
}

/**
 * Per-server TCP handler used for accepted connections.
 * The TLS server pointer is stored via xylem_tcp_listener_set_userdata
 * and recovered in the accept callback.
 */
static xylem_tcp_handler_t _tls_tcp_server_handler = {
    .on_accept         = _tls_tcp_accept_cb,
    .on_read           = _tls_tcp_read_cb,
    .on_write_done     = _tls_tcp_write_done_cb,
    .on_close          = _tls_tcp_close_cb,
    .on_timeout        = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};

static xylem_tcp_handler_t _tls_tcp_client_handler = {
    .on_connect        = _tls_tcp_connect_cb,
    .on_read           = _tls_tcp_read_cb,
    .on_write_done     = _tls_tcp_write_done_cb,
    .on_close          = _tls_tcp_close_cb,
    .on_timeout        = _tls_tcp_timeout_cb,
    .on_heartbeat_miss = _tls_tcp_heartbeat_cb,
};

/**
 * Deferred on_write_done for the defensive tcp_sends==0 path.
 * Firing synchronously would risk stack overflow if the user
 * calls xylem_tls_send again from inside on_write_done.
 */
typedef struct _tls_write_done_s {
    xylem_tls_conn_t* tls;
    const void*       data;
    size_t            len;
} _tls_write_done_t;

static void _tls_write_done_cb(loop_t* loop,
                               loop_post_t* post,
                               void* ud) {
    (void)loop;
    (void)post;
    _tls_write_done_t* wd = (_tls_write_done_t*)ud;

    /**
     * Post callbacks that invoke user handlers must check liveness:
     * the post queue is not cancellable, so the callback may fire
     * after on_close has already been delivered to the user.
     */
    if (wd->tls->state == TLS_STATE_CLOSED) {
        free(wd);
        return;
    }

    if (wd->tls->handler && wd->tls->handler->on_write_done) {
        wd->tls->handler->on_write_done(wd->tls, wd->data, wd->len, 0);
    }

    free(wd);
}

static int _tls_enqueue_write(xylem_tls_conn_t* tls,
                              const void* data,
                              size_t len) {
    _tls_write_req_t* req =
        (_tls_write_req_t*)malloc(sizeof(_tls_write_req_t));
    if (!req) {
        return -1;
    }
    req->data = data;
    req->len  = len;

    xylem_queue_enqueue(&tls->write_queue, &req->node);
    return 0;
}

static int _tls_process_write(xylem_tls_conn_t* tls,
                             const void* data,
                             size_t len) {
    ERR_clear_error();
    /* Stream protocol: fatal SSL_write failure corrupts session state. */
    int n = SSL_write(tls->ssl, data, (int)len);
    if (n <= 0) {
        unsigned long ssl_err_code = ERR_peek_error();
        const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
        xylem_logw("tls conn %p SSL_write failed (%s)",
                   (void*)tls, ssl_err_str ? ssl_err_str : "unknown");
        xylem_tls_close(tls);
        return -1;
    }

    if (!_tls_flush_write_bio(tls)) {
        /**
         * SSL_write succeeded but BIO produced no output -- should not
         * happen with memory BIOs. Defer on_write_done to the next loop
         * iteration to prevent stack overflow from recursive sends.
         */
        if (tls->handler && tls->handler->on_write_done) {
            _tls_write_done_t* wd =
                (_tls_write_done_t*)malloc(sizeof(_tls_write_done_t));
            if (!wd) {
                return -1;
            }
            wd->tls  = tls;
            wd->data = data;
            wd->len  = len;
            if (loop_post(runtime_get_loop(),
                                _tls_write_done_cb, wd) != 0) {
                free(wd);
                return -1;
            }
        }
        return 0;
    }

    return _tls_enqueue_write(tls, data, len);
}

int xylem_tls_send(xylem_tls_conn_t* tls, const void* data, size_t len) {
    if (tls->state != TLS_STATE_CONNECTED) {
        xylem_logd("tls conn %p send rejected (state=%d)",
                   (void*)tls, (int)tls->state);
        return -1;
    }

    if (!data || len == 0) {
        return 0;
    }

    return _tls_process_write(tls, data, len);
}

xylem_tls_conn_t* xylem_tls_dial(loop_t* loop,
                            const char* host,
                            uint16_t port,
                            xylem_tls_ctx_t* ctx,
                            xylem_tls_handler_t* handler,
                            xylem_tls_opts_t* opts) {
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)calloc(1, sizeof(xylem_tls_conn_t));
    if (!tls) {
        return NULL;
    }

    tls->ctx     = ctx;
    tls->handler = handler;
    atomic_store(&tls->refcount, 1);
    queue_init(&tls->write_queue);

    if (opts && opts->hostname) {
        tls->hostname = strdup(opts->hostname);
        if (!tls->hostname) {
            free(tls);
            return NULL;
        }
    }

    uint64_t connect_ms = opts ? opts->connect_timeout_ms : 0;
    xylem_tcp_opts_t* tcp_opts = opts ? &opts->tcp : NULL;
    xylem_tcp_conn_t* tcp = xylem_tcp_dial(loop, addr,
                                           &_tls_tcp_client_handler,
                                           connect_ms, tcp_opts);
    if (!tcp) {
        free(tls->hostname);
        free(tls);
        return NULL;
    }

    tls->tcp = tcp;
    xylem_tcp_set_userdata(tcp, tls);
    return tls;
}

static void _tls_destroy_conn(xylem_tls_conn_t* tls) {
    if (tls->ssl) {
        SSL_shutdown(tls->ssl);
        _tls_flush_write_bio(tls);
    }

    xylem_tcp_close(tls->tcp);
}

static void _tls_graceful_close_cb(loop_t* loop,
                                   loop_post_t* req,
                                   void* ud) {
    (void)loop;
    (void)req;
    xylem_tls_conn_t* tls = (xylem_tls_conn_t*)ud;
    if (tls->state == TLS_STATE_CLOSED) {
        return;
    }
    _tls_destroy_conn(tls);
}

void xylem_tls_close(xylem_tls_conn_t* tls) {
    if (tls->state == TLS_STATE_CLOSING ||
        tls->state == TLS_STATE_CLOSED) {
        return;
    }

    xylem_logi("tls conn %p close requested (state=%d)",
               (void*)tls, (int)tls->state);

    if (tls->state == TLS_STATE_CONNECTING) {
        _tls_destroy_conn(tls);
        return;
    }

    tls->state = TLS_STATE_CLOSING;

    if (queue_empty(&tls->write_queue)) {
        loop_post(runtime_get_loop(),
                        _tls_graceful_close_cb, tls);
    }
}

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls) {
    return tls->alpn[0] ? tls->alpn : NULL;
}

void* xylem_tls_get_userdata(xylem_tls_conn_t* tls) {
    return tls->userdata;
}

int xylem_tls_remote_addr(xylem_tls_conn_t* tls,
                          char host[INET6_ADDRSTRLEN],
                          uint16_t* port) {
    return xylem_tcp_remote_addr(tls->tcp, host, port);
}

loop_t* xylem_tls_get_loop(xylem_tls_conn_t* tls) {
    return runtime_get_loop();
}

void xylem_tls_set_userdata(xylem_tls_conn_t* tls, void* ud) {
    tls->userdata = ud;
}

void xylem_tls_conn_ref(xylem_tls_conn_t* tls) {
    atomic_fetch_add(&tls->refcount, 1);
}

void xylem_tls_conn_unref(xylem_tls_conn_t* tls) {
    _tls_conn_unref(tls);
}

xylem_tls_server_t* xylem_tls_listen(loop_t* loop,
                                     const char* host,
                                     uint16_t port,
                                     xylem_tls_ctx_t* ctx,
                                     xylem_tls_handler_t* handler,
                                     xylem_tls_opts_t* opts) {
    xylem_tls_server_t* server = (xylem_tls_server_t*)calloc(1, sizeof(xylem_tls_server_t));
    if (!server) {
        return NULL;
    }

    server->ctx     = ctx;
    server->handler = handler;
    server->loop    = loop;
    list_init(&server->connections);

    if (opts) {
        server->opts = *opts;
    }

    xylem_tcp_opts_t* tcp_opts = opts ? &opts->tcp : NULL;
    xylem_tcp_listener_t* tcp_server =
        xylem_tcp_listen(loop, addr, &_tls_tcp_server_handler, tcp_opts);
    if (!tcp_server) {
        free(server);
        return NULL;
    }

    server->tcp_server = tcp_server;
    xylem_tcp_listener_set_userdata(tcp_server, server);

    xylem_logi("tls server %p listening", (void*)server);
    return server;
}

void* xylem_tls_server_get_userdata(xylem_tls_server_t* server) {
    return server->userdata;
}

void xylem_tls_server_set_userdata(xylem_tls_server_t* server, void* ud) {
    server->userdata = ud;
}

/* Post callback: free a server after the current iteration. */
static void _tls_server_free_cb(loop_t* loop,
                                loop_post_t* req,
                                void* ud) {
    (void)loop;
    (void)req;
    free(ud);
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
    while (!list_empty(&server->connections)) {
        list_node_t* node = list_head(&server->connections);
        xylem_tls_conn_t* tls = list_entry(node, xylem_tls_conn_t, server_node);
        list_remove(&server->connections, node);
        tls->server = NULL;
        xylem_tls_close(tls);
    }

    xylem_tcp_close_listener(server->tcp_server);
    loop_post(server->loop, _tls_server_free_cb, server);
}

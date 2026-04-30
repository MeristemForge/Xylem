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

#include "xylem/xylem-dtls.h"
#include "xylem/xylem-hmac256.h"
#include "xylem/xylem-rbtree.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-udp.h"

#include "deprecated/c11-threads.h"
#include "platform/platform-socket.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum TLS record payload (RFC 8446 section 5.1). */
#define TLS_RECORD_MAX_PLAINTEXT 16384

/**
 * Server-side sessions that don't complete the handshake within this
 * window are automatically closed to prevent resource exhaustion from
 * abandoned or malicious ClientHellos.
 */
#define DTLS_HANDSHAKE_TIMEOUT_MS 30000

/* HMAC-SHA256 output size in bytes. */
#define DTLS_COOKIE_SIZE 32

static int _dtls_ex_data_idx = -1;
static int _dtls_peer_addr_idx = -1;
static once_flag _dtls_ex_data_once = ONCE_FLAG_INIT;

static void _dtls_init_ex_data(void) {
    _dtls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _dtls_peer_addr_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

struct xylem_dtls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
    uint8_t  cookie_secret[DTLS_COOKIE_SIZE];
};

struct xylem_dtls_conn_s {
    SSL*                   ssl;
    BIO*                   read_bio;
    BIO*                   write_bio;
    xylem_udp_t*           udp;
    xylem_dtls_ctx_t*      ctx;
    xylem_dtls_handler_t*  handler;
    xylem_dtls_server_t*   server;
    xylem_addr_t           peer_addr;
    void*                  userdata;
    _Atomic bool           handshake_done;
    _Atomic bool           closing;
    _Atomic int32_t        refcount;
    int                    close_err;
    const char*            close_errmsg;
    char                   alpn[256];
    xylem_loop_t*          loop;
    xylem_loop_timer_t*    retransmit_timer;
    xylem_loop_timer_t*    handshake_timer;  /* server-side only */
    xylem_rbtree_node_t    server_node;
};

struct xylem_dtls_server_s {
    xylem_udp_t*           udp;
    xylem_dtls_ctx_t*      ctx;
    xylem_dtls_handler_t*  handler;
    xylem_loop_t*          loop;
    xylem_rbtree_t         sessions;
    void*                  userdata;
    bool                   closing;
};

typedef struct _dtls_deferred_send_s {
    xylem_dtls_conn_t* dtls;
    size_t        len;
    char          data[];
} _dtls_deferred_send_t;

static xylem_dtls_ctx_t* _dtls_get_ctx(SSL* ssl) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    return (xylem_dtls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _dtls_ex_data_idx);
}

static int _dtls_get_peer_addr(SSL* ssl, const uint8_t** out,
                               size_t* out_len) {
    xylem_addr_t* addr =
        (xylem_addr_t*)SSL_get_ex_data(ssl, _dtls_peer_addr_idx);
    if (!addr) {
        return -1;
    }

    if (addr->storage.ss_family == AF_INET) {
        *out     = (const uint8_t*)&addr->storage;
        *out_len = sizeof(struct sockaddr_in);
    } else if (addr->storage.ss_family == AF_INET6) {
        *out     = (const uint8_t*)&addr->storage;
        *out_len = sizeof(struct sockaddr_in6);
    } else {
        return -1;
    }
    return 0;
}

static void _dtls_keylog_cb(const SSL* ssl, const char* line) {
    xylem_dtls_ctx_t* ctx = _dtls_get_ctx((SSL*)ssl);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _dtls_cookie_generate_cb(SSL* ssl, unsigned char* cookie,
                                    unsigned int* cookie_len) {
    xylem_dtls_ctx_t* ctx = _dtls_get_ctx(ssl);
    if (!ctx) {
        return 0;
    }

    const uint8_t* msg;
    size_t         msg_len;
    if (_dtls_get_peer_addr(ssl, &msg, &msg_len) < 0) {
        return 0;
    }

    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, cookie);
    *cookie_len = DTLS_COOKIE_SIZE;
    return 1;
}

static int _dtls_cookie_verify_cb(SSL* ssl, const unsigned char* cookie,
                                  unsigned int cookie_len) {
    xylem_dtls_ctx_t* ctx = _dtls_get_ctx(ssl);
    if (!ctx) {
        return 0;
    }

    const uint8_t* msg;
    size_t         msg_len;
    if (_dtls_get_peer_addr(ssl, &msg, &msg_len) < 0) {
        return 0;
    }

    uint8_t expected[DTLS_COOKIE_SIZE];
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, expected);

    if (cookie_len != DTLS_COOKIE_SIZE) {
        return 0;
    }
    return CRYPTO_memcmp(cookie, expected, DTLS_COOKIE_SIZE) == 0 ? 1 : 0;
}

static int _dtls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                                unsigned char* outlen,
                                const unsigned char* in,
                                unsigned int inlen, void* arg) {
    xylem_dtls_ctx_t* ctx = (xylem_dtls_ctx_t*)arg;
    (void)ssl;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
    xylem_dtls_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(DTLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /* Generate cookie HMAC key with CSPRNG. */
    if (RAND_bytes(ctx->cookie_secret, sizeof(ctx->cookie_secret)) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
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

    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_cookie_generate_cb(ctx->ssl_ctx, _dtls_cookie_generate_cb);
    SSL_CTX_set_cookie_verify_cb(ctx->ssl_ctx, _dtls_cookie_verify_cb);

    /* Enforce DTLS 1.2 as minimum version. */
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);

    /* Register ex_data index once for keylog callback to recover ctx. */
    call_once(&_dtls_ex_data_once, _dtls_init_ex_data);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _dtls_ex_data_idx, ctx);

    return ctx;
}

void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx) {
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

int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path) {
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

    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _dtls_keylog_cb);
    return 0;
}

int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                             const char* cert, const char* key) {
    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert) != 1) {
        xylem_loge("dtls ctx: failed to load cert %s", cert);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        xylem_loge("dtls ctx: failed to load key %s", key);
        return -1;
    }
    return 0;
}

int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("dtls ctx: failed to load CA %s", ca_file);
        return -1;
    }
    return 0;
}

void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable) {
    int mode = enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx->ssl_ctx, mode, NULL);
}

int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                            const char** protocols, size_t count) {
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
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _dtls_alpn_select_cb, ctx);

    return 0;
}


static void _dtls_flush_write_bio(xylem_dtls_conn_t* dtls) {
    char buf[TLS_RECORD_MAX_PLAINTEXT];
    int  n;

    while ((n = BIO_read(dtls->write_bio, buf, sizeof(buf))) > 0) {
        xylem_udp_send(dtls->udp, &dtls->peer_addr, buf, (size_t)n);
    }
}

static void _dtls_feed_read_bio(xylem_dtls_conn_t* dtls,
                                void* data, size_t len) {
    BIO_write(dtls->read_bio, data, (int)len);
}

static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls);

static void _dtls_retransmit_timeout_cb(xylem_loop_t* loop,
                                        xylem_loop_timer_t* timer,
                                        void* ud) {
    (void)loop;
    (void)timer;
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)ud;

    /**
     * Guard against a timer callback already queued in the current
     * loop iteration when xylem_dtls_close stopped the timer.
     */
    if (atomic_load(&dtls->closing)) {
        return;
    }

    DTLSv1_handle_timeout(dtls->ssl);
    _dtls_flush_write_bio(dtls);
    _dtls_arm_retransmit(dtls);
}

static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls) {
    struct timeval tv;
    if (DTLSv1_get_timeout(dtls->ssl, &tv)) {
        uint64_t ms = (uint64_t)tv.tv_sec * 1000 +
                      (uint64_t)tv.tv_usec / 1000;
        if (ms == 0) {
            ms = 1;
        }
        xylem_loop_stop_timer(dtls->retransmit_timer);
        xylem_loop_start_timer(dtls->retransmit_timer,
                               _dtls_retransmit_timeout_cb, dtls, ms, 0);
    }
}

static void _dtls_stop_retransmit(xylem_dtls_conn_t* dtls) {
    xylem_loop_stop_timer(dtls->retransmit_timer);
}

static void _dtls_handshake_timeout_cb(xylem_loop_t* loop,
                                       xylem_loop_timer_t* timer,
                                       void* ud) {
    (void)loop;
    (void)timer;
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)ud;

    /* Session may already be closing from another path. */
    if (atomic_load(&dtls->closing)) {
        return;
    }

    xylem_logw("dtls session %p handshake timed out", (void*)dtls);
    dtls->close_err    = -1;
    dtls->close_errmsg = "handshake timeout";
    xylem_dtls_close(dtls);
}

static int _dtls_init_ssl(xylem_dtls_conn_t* dtls) {
    dtls->ssl = SSL_new(dtls->ctx->ssl_ctx);
    if (!dtls->ssl) {
        xylem_loge("dtls session %p SSL_new failed", (void*)dtls);
        return -1;
    }

    dtls->read_bio  = BIO_new(BIO_s_mem());
    dtls->write_bio = BIO_new(BIO_s_mem());
    if (!dtls->read_bio || !dtls->write_bio) {
        xylem_loge("dtls session %p BIO_new failed", (void*)dtls);
        /* BIO_free accepts NULL safely. */
        BIO_free(dtls->read_bio);
        BIO_free(dtls->write_bio);
        SSL_free(dtls->ssl);
        dtls->ssl       = NULL;
        dtls->read_bio  = NULL;
        dtls->write_bio = NULL;
        return -1;
    }

    SSL_set_bio(dtls->ssl, dtls->read_bio, dtls->write_bio);
    return 0;
}

static void _dtls_do_handshake(xylem_dtls_conn_t* dtls) {
    ERR_clear_error();
    int rc  = SSL_do_handshake(dtls->ssl);
    int err = SSL_get_error(dtls->ssl, rc);

    if (rc == 1) {
        atomic_store(&dtls->handshake_done, true);
        _dtls_flush_write_bio(dtls);
        _dtls_stop_retransmit(dtls);

        if (dtls->handshake_timer) {
            xylem_loop_stop_timer(dtls->handshake_timer);
        }

        /* Cache negotiated ALPN as a null-terminated string. */
        const unsigned char* alpn_proto = NULL;
        unsigned int         alpn_len   = 0;
        SSL_get0_alpn_selected(dtls->ssl, &alpn_proto, &alpn_len);
        if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(dtls->alpn)) {
            memcpy(dtls->alpn, alpn_proto, alpn_len);
            dtls->alpn[alpn_len] = '\0';
        }

        xylem_logi("dtls session %p handshake complete (%s)",
                   (void*)dtls, dtls->server ? "server" : "client");

        if (dtls->server) {
            if (dtls->handler && dtls->handler->on_accept) {
                dtls->handler->on_accept(dtls->server, dtls);
            }
        } else {
            if (dtls->handler && dtls->handler->on_connect) {
                dtls->handler->on_connect(dtls);
            }
        }
        return;
    }

    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        _dtls_flush_write_bio(dtls);
        _dtls_arm_retransmit(dtls);
        return;
    }

    /* Flush any pending alert before tearing down. */
    unsigned long ssl_err_code = ERR_peek_error();
    const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
    xylem_logw("dtls session %p handshake failed ssl_err=%d (%s)",
               (void*)dtls, err,
               ssl_err_str ? ssl_err_str : "unknown");
    dtls->close_err    = err;
    dtls->close_errmsg = ssl_err_str ? ssl_err_str : "handshake failed";
    _dtls_flush_write_bio(dtls);
    xylem_dtls_close(dtls);
}

/**
 * Compare two sockaddr by family, then port, then address.
 * Returns negative/zero/positive like memcmp.
 */
static int _dtls_addr_cmp(const xylem_addr_t* a, const xylem_addr_t* b) {
    if (a->storage.ss_family != b->storage.ss_family) {
        return (int)a->storage.ss_family - (int)b->storage.ss_family;
    }
    if (a->storage.ss_family == AF_INET) {
        const struct sockaddr_in* sa = (const struct sockaddr_in*)&a->storage;
        const struct sockaddr_in* sb = (const struct sockaddr_in*)&b->storage;
        if (sa->sin_port != sb->sin_port) {
            return (int)ntohs(sa->sin_port) - (int)ntohs(sb->sin_port);
        }
        return memcmp(&sa->sin_addr, &sb->sin_addr, 4);
    }
    if (a->storage.ss_family == AF_INET6) {
        const struct sockaddr_in6* sa = (const struct sockaddr_in6*)&a->storage;
        const struct sockaddr_in6* sb = (const struct sockaddr_in6*)&b->storage;
        if (sa->sin6_port != sb->sin6_port) {
            return (int)ntohs(sa->sin6_port) - (int)ntohs(sb->sin6_port);
        }
        return memcmp(&sa->sin6_addr, &sb->sin6_addr, 16);
    }
    return 0;
}

/* node-node comparator for rbtree insert. */
static int _dtls_session_cmp_nn(const xylem_rbtree_node_t* a,
                                const xylem_rbtree_node_t* b) {
    const xylem_dtls_conn_t* da =
        xylem_rbtree_entry(a, xylem_dtls_conn_t, server_node);
    const xylem_dtls_conn_t* db =
        xylem_rbtree_entry(b, xylem_dtls_conn_t, server_node);
    return _dtls_addr_cmp(&da->peer_addr, &db->peer_addr);
}

/* key(xylem_addr_t*)-node comparator for rbtree find. */
static int _dtls_session_cmp_kn(const void* key,
                                const xylem_rbtree_node_t* node) {
    const xylem_addr_t* addr = (const xylem_addr_t*)key;
    const xylem_dtls_conn_t* dtls =
        xylem_rbtree_entry(node, xylem_dtls_conn_t, server_node);
    return _dtls_addr_cmp(addr, &dtls->peer_addr);
}

static xylem_dtls_conn_t* _dtls_find_session(xylem_dtls_server_t* server,
                                        xylem_addr_t* addr) {
    xylem_rbtree_node_t* node = xylem_rbtree_find(&server->sessions, addr);
    if (!node) {
        return NULL;
    }
    return xylem_rbtree_entry(node, xylem_dtls_conn_t, server_node);
}


static void _dtls_client_read_cb(xylem_udp_t* udp, void* data,
                                 size_t len, xylem_addr_t* addr) {
    (void)addr;
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)xylem_udp_get_userdata(udp);

    _dtls_feed_read_bio(dtls, data, len);

    if (!atomic_load(&dtls->handshake_done)) {
        _dtls_do_handshake(dtls);
        if (!atomic_load(&dtls->handshake_done)) {
            return;
        }
    }
    /**
     * User may have called xylem_dtls_close in on_connect.
     * Client-side close calls xylem_udp_close which fires
     * _dtls_client_close_cb synchronously (UDP has no write
     * queue), freeing dtls->ssl. Must bail out before SSL_read.
     */
    if (atomic_load(&dtls->closing)) {
        return;
    }
    char buf[TLS_RECORD_MAX_PLAINTEXT];
    int  n;

    ERR_clear_error();
    while ((n = SSL_read(dtls->ssl, buf, sizeof(buf))) > 0) {
        if (dtls->handler && dtls->handler->on_read) {
            dtls->handler->on_read(dtls, buf, (size_t)n);
        }
        if (atomic_load(&dtls->closing)) {
            return;
        }
    }

    int err = SSL_get_error(dtls->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
        xylem_logi("dtls session %p peer sent close_notify", (void*)dtls);
        xylem_dtls_close(dtls);
        return;
    }

    if (err != SSL_ERROR_WANT_READ) {
        unsigned long ssl_err_code = ERR_peek_error();
        const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
        xylem_logw("dtls session %p SSL_read error=%d (%s)",
                   (void*)dtls, err,
                   ssl_err_str ? ssl_err_str : "unknown");
        dtls->close_err    = err;
        dtls->close_errmsg = ssl_err_str ? ssl_err_str : "unknown";
        xylem_dtls_close(dtls);
    }
}

/* Decrement refcount; free the session when it reaches zero. */
static void _dtls_conn_decref(xylem_dtls_conn_t* dtls) {
    if (atomic_fetch_sub(&dtls->refcount, 1) == 1) {
        free(dtls);
    }
}

/**
 * Deferred free so the session pointer stays valid through the
 * current loop iteration's callback chain.
 */
static void _dtls_free_cb(xylem_loop_t* loop, xylem_loop_post_t* req,
                          void* ud) {
    (void)loop;
    (void)req;
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)ud;
    xylem_loop_destroy_timer(dtls->retransmit_timer);
    if (dtls->handshake_timer) {
        xylem_loop_destroy_timer(dtls->handshake_timer);
    }
    _dtls_conn_decref(dtls);
}

static void _dtls_client_close_cb(xylem_udp_t* udp, int err,
                                  const char* errmsg) {
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)xylem_udp_get_userdata(udp);
    if (!dtls) {
        return;
    }

    /**
     * Mark closing and stop timers to prevent _dtls_retransmit_timeout_cb
     * or _dtls_handshake_timeout_cb from firing after SSL is freed.
     * On Linux/macOS a connected UDP socket may receive ECONNREFUSED
     * (ICMP port unreachable) before any timer fires.
     */
    atomic_store(&dtls->closing, true);
    _dtls_stop_retransmit(dtls);
    if (dtls->handshake_timer) {
        xylem_loop_stop_timer(dtls->handshake_timer);
    }

    xylem_logd("dtls session %p close err=%d (%s)",
               (void*)dtls, dtls->close_err,
               dtls->close_errmsg ? dtls->close_errmsg : "ok");

    /* Propagate UDP-layer error only when DTLS has not set its own. */
    if (dtls->close_err == 0 && err != 0) {
        dtls->close_err    = err;
        dtls->close_errmsg = errmsg;
    }

    if (dtls->ssl) {
        SSL_free(dtls->ssl);
        dtls->ssl = NULL;
    }
    if (dtls->handler && dtls->handler->on_close) {
        dtls->handler->on_close(dtls, dtls->close_err, dtls->close_errmsg);
    }
    /* Defer free to next loop iteration so close_node stays valid. */
    xylem_loop_post(dtls->loop, _dtls_free_cb, dtls);
}

static void _dtls_server_read_cb(xylem_udp_t* udp, void* data,
                                 size_t len, xylem_addr_t* addr) {
    xylem_dtls_server_t* server =
        (xylem_dtls_server_t*)xylem_udp_get_userdata(udp);

    if (server->closing) {
        return;
    }

    xylem_dtls_conn_t* dtls = _dtls_find_session(server, addr);

    if (dtls) {
        _dtls_feed_read_bio(dtls, data, len);

        if (!atomic_load(&dtls->handshake_done)) {
            _dtls_do_handshake(dtls);
            if (!atomic_load(&dtls->handshake_done)) {
                return;
            }
        }

        /**
         * User may have called xylem_dtls_close in on_accept.
         * Server-side close path calls SSL_free synchronously
         * (no async UDP close), so dtls->ssl is already NULL.
         * Must bail out before SSL_read.
         */
        if (atomic_load(&dtls->closing)) {
            return;
        }

        char buf[TLS_RECORD_MAX_PLAINTEXT];
        int  n;

        ERR_clear_error();
        while ((n = SSL_read(dtls->ssl, buf, sizeof(buf))) > 0) {
            if (dtls->handler && dtls->handler->on_read) {
                dtls->handler->on_read(dtls, buf, (size_t)n);
            }
            if (atomic_load(&dtls->closing)) {
                return;
            }
        }

        int err = SSL_get_error(dtls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            xylem_logi("dtls session %p peer sent close_notify",
                       (void*)dtls);
            xylem_dtls_close(dtls);
        } else if (err != SSL_ERROR_WANT_READ) {
            unsigned long ssl_err_code = ERR_peek_error();
            const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
            xylem_logw("dtls session %p SSL_read error=%d (%s)",
                       (void*)dtls, err,
                       ssl_err_str ? ssl_err_str : "unknown");
            dtls->close_err    = err;
            dtls->close_errmsg = ssl_err_str ? ssl_err_str : "unknown";
            xylem_dtls_close(dtls);
        }
        return;
    }

    dtls = calloc(1, sizeof(*dtls));
    if (!dtls) {
        xylem_loge("dtls server: session alloc failed");
        return;
    }

    dtls->udp       = server->udp;
    dtls->ctx       = server->ctx;
    dtls->handler   = server->handler;
    dtls->server    = server;
    dtls->peer_addr = *addr;
    dtls->loop      = server->loop;
    atomic_store(&dtls->refcount, 1);

    dtls->retransmit_timer = xylem_loop_create_timer(server->loop);
    dtls->handshake_timer  = xylem_loop_create_timer(server->loop);

    if (_dtls_init_ssl(dtls) != 0) {
        xylem_loop_destroy_timer(dtls->retransmit_timer);
        xylem_loop_destroy_timer(dtls->handshake_timer);
        free(dtls);
        return;
    }

    SSL_set_accept_state(dtls->ssl);

    /* Store peer addr in SSL ex_data so cookie callbacks can access it. */
    SSL_set_ex_data(dtls->ssl, _dtls_peer_addr_idx, &dtls->peer_addr);

    xylem_rbtree_insert(&server->sessions, &dtls->server_node);

    xylem_loop_start_timer(dtls->handshake_timer,
                           _dtls_handshake_timeout_cb, dtls,
                           DTLS_HANDSHAKE_TIMEOUT_MS, 0);

    /* Feed the first packet so the handshake can begin. */
    _dtls_feed_read_bio(dtls, data, len);
    _dtls_do_handshake(dtls);
}

static void _dtls_server_close_cb(xylem_udp_t* udp, int err,
                                  const char* errmsg) {
    (void)err;
    (void)errmsg;
    xylem_dtls_server_t* server =
        (xylem_dtls_server_t*)xylem_udp_get_userdata(udp);
    free(server);
}

static xylem_udp_handler_t _dtls_client_udp_handler = {
    .on_read  = _dtls_client_read_cb,
    .on_close = _dtls_client_close_cb,
};

/**
 * Roll back a partially initialised dial session.
 * Each field is NULL-safe: calloc zeroes everything, so only
 * resources that were actually created get released.
 *
 * Detach before close: xylem_udp_close fires on_close synchronously
 * (UDP has no write queue), and dtls is about to be freed.
 */
static void _dtls_dial_cleanup(xylem_dtls_conn_t* dtls,
                               xylem_udp_t* udp) {
    if (dtls->retransmit_timer) {
        xylem_loop_destroy_timer(dtls->retransmit_timer);
    }
    if (udp) {
        xylem_udp_set_userdata(udp, NULL);
        xylem_udp_close(udp);
    }
    free(dtls);
}

xylem_dtls_conn_t* xylem_dtls_dial(xylem_loop_t* loop,
                              xylem_addr_t* addr,
                              xylem_dtls_ctx_t* ctx,
                              xylem_dtls_handler_t* handler) {
    xylem_dtls_conn_t* dtls = calloc(1, sizeof(*dtls));
    if (!dtls) {
        return NULL;
    }

    dtls->ctx       = ctx;
    dtls->handler   = handler;
    dtls->peer_addr = *addr;
    dtls->loop      = loop;
    atomic_store(&dtls->refcount, 1);

    xylem_udp_t* udp = xylem_udp_dial(loop, addr,
                                      &_dtls_client_udp_handler);
    if (!udp) {
        free(dtls);
        return NULL;
    }

    dtls->udp = udp;
    xylem_udp_set_userdata(udp, dtls);

    dtls->retransmit_timer = xylem_loop_create_timer(loop);

    if (_dtls_init_ssl(dtls) != 0) {
        _dtls_dial_cleanup(dtls, udp);
        return NULL;
    }

    SSL_set_connect_state(dtls->ssl);
    _dtls_do_handshake(dtls);

    /**
     * If the initial handshake attempt triggered a fatal error,
     * _dtls_do_handshake called xylem_dtls_close which (for client-side)
     * synchronously fires _dtls_client_close_cb via xylem_udp_close.
     * The session is now a zombie scheduled for deferred free -- do not
     * return it to the caller.
     */
    if (atomic_load(&dtls->closing)) {
        return NULL;
    }

    return dtls;
}

/* Perform the actual DTLS send (loop thread only). */
static int _dtls_do_send(xylem_dtls_conn_t* dtls,
                         const void* data, size_t len) {
    ERR_clear_error();
    int n = SSL_write(dtls->ssl, data, (int)len);
    if (n <= 0) {
        unsigned long ssl_err_code = ERR_peek_error();
        const char*   ssl_err_str  = ERR_reason_error_string(ssl_err_code);
        xylem_logw("dtls session %p SSL_write failed (%s)",
                   (void*)dtls, ssl_err_str ? ssl_err_str : "unknown");
        return -1;
    }

    _dtls_flush_write_bio(dtls);
    return 0;
}

static void _dtls_deferred_send_cb(xylem_loop_t* loop,
                                    xylem_loop_post_t* req,
                                    void* ud) {
    (void)loop;
    (void)req;
    _dtls_deferred_send_t* ds = (_dtls_deferred_send_t*)ud;

    if (atomic_load(&ds->dtls->handshake_done) &&
        !atomic_load(&ds->dtls->closing)) {
        _dtls_do_send(ds->dtls, ds->data, ds->len);
    }

    _dtls_conn_decref(ds->dtls);
    free(ds);
}

int xylem_dtls_send(xylem_dtls_conn_t* dtls,
                    const void* data, size_t len) {
    if (!atomic_load(&dtls->handshake_done) ||
        atomic_load(&dtls->closing)) {
        xylem_logd("dtls session %p send rejected (handshake=%d closing=%d)",
                   (void*)dtls,
                   (int)atomic_load(&dtls->handshake_done),
                   (int)atomic_load(&dtls->closing));
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    /* Cross-thread: copy data and post to loop thread. */
    if (!xylem_loop_is_loop_thread(dtls->loop)) {
        _dtls_deferred_send_t* ds = (_dtls_deferred_send_t*)malloc(
            sizeof(_dtls_deferred_send_t) + len);
        if (!ds) {
            return -1;
        }
        ds->dtls = dtls;
        ds->len  = len;
        memcpy(ds->data, data, len);

        atomic_fetch_add(&dtls->refcount, 1);
        if (xylem_loop_post(dtls->loop, _dtls_deferred_send_cb, ds) != 0) {
            _dtls_conn_decref(dtls);
            free(ds);
            return -1;
        }
        return 0;
    }

    /* Same thread: send directly. */
    return _dtls_do_send(dtls, data, len);
}

/**
 * Close logic that runs on the loop thread. Extracted so that
 * _dtls_deferred_close_cb can call it directly instead of re-entering
 * xylem_dtls_close (which would re-post when loop->tid is unset).
 */
static void _dtls_do_close(xylem_dtls_conn_t* dtls) {
    if (atomic_load(&dtls->closing)) {
        return;
    }
    atomic_store(&dtls->closing, true);

    xylem_logi("dtls session %p close requested", (void*)dtls);

    _dtls_stop_retransmit(dtls);

    if (dtls->handshake_timer) {
        xylem_loop_stop_timer(dtls->handshake_timer);
    }

    if (dtls->ssl && atomic_load(&dtls->handshake_done)) {
        SSL_shutdown(dtls->ssl);
        _dtls_flush_write_bio(dtls);
    }

    if (dtls->server) {
        /**
         * Server-side session: detach from server list, clean up
         * SSL state, notify user. The shared UDP socket stays open.
         */
        xylem_rbtree_erase(&dtls->server->sessions, &dtls->server_node);

        if (dtls->ssl) {
            SSL_free(dtls->ssl);
            dtls->ssl = NULL;
        }

        if (atomic_load(&dtls->handshake_done) &&
            dtls->handler && dtls->handler->on_close) {
            dtls->handler->on_close(dtls, dtls->close_err,
                                    dtls->close_errmsg);
        }

        /* Defer free to next loop iteration so close_node stays valid. */
        xylem_loop_post(dtls->loop, _dtls_free_cb, dtls);
    } else {
        /**
         * Client-side: close the dedicated UDP socket. The
         * _dtls_client_close_cb will free SSL and notify user.
         */
        xylem_udp_close(dtls->udp);
    }
}

static void _dtls_deferred_close_cb(xylem_loop_t* loop,
                                     xylem_loop_post_t* req,
                                     void* ud) {
    (void)loop;
    (void)req;
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)ud;
    _dtls_do_close(dtls);
    _dtls_conn_decref(dtls);
}

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    if (atomic_load(&dtls->closing)) {
        return;
    }

    /* Cross-thread: post to loop thread. */
    if (!xylem_loop_is_loop_thread(dtls->loop)) {
        atomic_fetch_add(&dtls->refcount, 1);
        if (xylem_loop_post(dtls->loop, _dtls_deferred_close_cb, dtls) != 0) {
            _dtls_conn_decref(dtls);
        }
        return;
    }

    _dtls_do_close(dtls);
}

const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls) {
    return dtls->alpn[0] ? dtls->alpn : NULL;
}

const xylem_addr_t* xylem_dtls_get_peer_addr(xylem_dtls_conn_t* dtls) {
    return &dtls->peer_addr;
}

xylem_loop_t* xylem_dtls_get_loop(xylem_dtls_conn_t* dtls) {
    return dtls->loop;
}

void* xylem_dtls_get_userdata(xylem_dtls_conn_t* dtls) {
    return dtls->userdata;
}

void xylem_dtls_set_userdata(xylem_dtls_conn_t* dtls, void* ud) {
    dtls->userdata = ud;
}

void xylem_dtls_conn_acquire(xylem_dtls_conn_t* dtls) {
    atomic_fetch_add(&dtls->refcount, 1);
}

void xylem_dtls_conn_release(xylem_dtls_conn_t* dtls) {
    _dtls_conn_decref(dtls);
}

static xylem_udp_handler_t _dtls_server_udp_handler = {
    .on_read  = _dtls_server_read_cb,
    .on_close = _dtls_server_close_cb,
};

xylem_dtls_server_t* xylem_dtls_listen(xylem_loop_t* loop,
                                       xylem_addr_t* addr,
                                       xylem_dtls_ctx_t* ctx,
                                       xylem_dtls_handler_t* handler) {
    xylem_dtls_server_t* server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    server->ctx     = ctx;
    server->handler = handler;
    server->loop    = loop;
    xylem_rbtree_init(&server->sessions, _dtls_session_cmp_nn,
                      _dtls_session_cmp_kn);

    xylem_udp_t* udp = xylem_udp_listen(loop, addr,
                                       &_dtls_server_udp_handler);
    if (!udp) {
        free(server);
        return NULL;
    }

    server->udp = udp;
    xylem_udp_set_userdata(udp, server);

    xylem_logi("dtls server %p listening", (void*)server);
    return server;
}

void xylem_dtls_close_server(xylem_dtls_server_t* server) {
    if (server->closing) {
        return;
    }
    server->closing = true;

    xylem_logi("dtls server %p closing", (void*)server);

    while (!xylem_rbtree_empty(&server->sessions)) {
        xylem_rbtree_node_t* node = xylem_rbtree_first(&server->sessions);
        xylem_dtls_conn_t* dtls =
            xylem_rbtree_entry(node, xylem_dtls_conn_t, server_node);
        xylem_dtls_close(dtls);
    }

    /* _dtls_server_close_cb frees server. */
    xylem_udp_close(server->udp);
}

void* xylem_dtls_server_get_userdata(xylem_dtls_server_t* server) {
    return server->userdata;
}

void xylem_dtls_server_set_userdata(xylem_dtls_server_t* server, void* ud) {
    server->userdata = ud;
}

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


#include "xylem/net/xylem-dtls.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/crypto/xylem-hmac256.h"
#include "net/addr.h"
#include "container/rbtree.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "thrds.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#define TLS_RECORD_MAX_PLAINTEXT 16384
#define DTLS_DEFAULT_TIMEOUT_MS  30000
#define DTLS_COOKIE_SIZE         32
#define DTLS_INBOX_CAP           64



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

typedef struct dtls_dgram_s {
    size_t len;
    char   data[];
} dtls_dgram_t;

typedef struct dtls_session_inbox_s {
    dtls_dgram_t** slots;
    uint32_t       cap;
    uint32_t       head;
    uint32_t       tail;
    mco_coro*      parked;
    scheduler_t*   sched;
    sched_timer_t* deadline_timer;
    bool           closed;
    bool           timed_out;
} dtls_session_inbox_t;

struct xylem_dtls_conn_s {
    SSL*                    ssl;
    xylem_dtls_ctx_t*       ctx;
    addr_t                  peer_addr;
    char                    alpn[256];
    _Atomic bool            closed;
    bool                    handshake_done;

    /* client-side only (Socket BIO path) */
    iowait_t*               waiter;
    platform_sock_t          fd;

    /* server-side only (Memory BIO path) */
    dtls_session_inbox_t*    inbox;
    BIO*                     read_bio;
    BIO*                     write_bio;
    sched_timer_t*           retransmit_timer;
    sched_timer_t*           handshake_timer;
    xylem_dtls_listener_t*   listener;
    rbtree_node_t            server_node;
    uint64_t                 rd_deadline_ms;
    uint64_t                 wr_deadline_ms;
};

struct xylem_dtls_listener_s {
    platform_sock_t       fd;
    iowait_t*             waiter;
    xylem_dtls_ctx_t*     ctx;
    xylem_dtls_opts_t     opts;
    rbtree_t              sessions;
    mtx_t                 sessions_mtx;
    scheduler_t*          sched;

    xylem_dtls_conn_t**   accept_slots;
    uint32_t              accept_cap;
    uint32_t              accept_head;
    uint32_t              accept_tail;
    mco_coro*             accept_parked;
    bool                  accept_closed;

    _Atomic bool          closed;
};



static xylem_dtls_ctx_t* _dtls_get_ctx(SSL* ssl) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    return (xylem_dtls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _dtls_ex_data_idx);
}

static int _dtls_get_peer_addr(SSL* ssl, const uint8_t** out,
                               size_t* out_len) {
    addr_t* addr =
        (addr_t*)SSL_get_ex_data(ssl, _dtls_peer_addr_idx);
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
    xylem_dtls_ctx_t* ctx = (xylem_dtls_ctx_t*)calloc(1, sizeof(xylem_dtls_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(DTLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    if (RAND_bytes(ctx->cookie_secret, sizeof(ctx->cookie_secret)) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /**
     * Socket BIO client path may partially complete SSL_write; this
     * flag lets the retry use a different buffer pointer for the
     * same write.
     */
    SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_cookie_generate_cb(ctx->ssl_ctx, _dtls_cookie_generate_cb);
    SSL_CTX_set_cookie_verify_cb(ctx->ssl_ctx, _dtls_cookie_verify_cb);

    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);

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
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _dtls_alpn_select_cb, ctx);

    return 0;
}



static int _dtls_addr_cmp(const addr_t* a, const addr_t* b) {
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

static int _dtls_session_cmp_nn(const rbtree_node_t* a,
                                const rbtree_node_t* b) {
    const xylem_dtls_conn_t* da =
        rbtree_entry(a, xylem_dtls_conn_t, server_node);
    const xylem_dtls_conn_t* db =
        rbtree_entry(b, xylem_dtls_conn_t, server_node);
    return _dtls_addr_cmp(&da->peer_addr, &db->peer_addr);
}

static int _dtls_session_cmp_kn(const void* key,
                                const rbtree_node_t* node) {
    const addr_t* addr = (const addr_t*)key;
    const xylem_dtls_conn_t* dtls =
        rbtree_entry(node, xylem_dtls_conn_t, server_node);
    return _dtls_addr_cmp(addr, &dtls->peer_addr);
}

static xylem_dtls_conn_t* _dtls_find_session(xylem_dtls_listener_t* ln,
                                              addr_t* addr) {
    rbtree_node_t* node = rbtree_find(&ln->sessions, addr);
    if (!node) {
        return NULL;
    }
    return rbtree_entry(node, xylem_dtls_conn_t, server_node);
}



static dtls_session_inbox_t* _inbox_create(scheduler_t* sched) {
    dtls_session_inbox_t* ib = calloc(1, sizeof(*ib));
    if (!ib) {
        return NULL;
    }
    ib->cap   = DTLS_INBOX_CAP;
    ib->slots = calloc(ib->cap, sizeof(dtls_dgram_t*));
    if (!ib->slots) {
        free(ib);
        return NULL;
    }
    ib->sched = sched;
    ib->deadline_timer = sched_timer_create(sched);
    return ib;
}

static void _inbox_destroy(dtls_session_inbox_t* ib) {
    if (!ib) {
        return;
    }
    while (ib->head != ib->tail) {
        free(ib->slots[ib->head & (ib->cap - 1)]);
        ib->head++;
    }
    sched_timer_destroy(ib->deadline_timer);
    free(ib->slots);
    free(ib);
}

static void _inbox_push(dtls_session_inbox_t* ib, dtls_dgram_t* dgram) {
    if (ib->closed) {
        free(dgram);
        return;
    }
    uint32_t mask = ib->cap - 1;
    if (ib->tail - ib->head >= ib->cap) {
        free(dgram); return;
    }
    ib->slots[ib->tail & mask] = dgram;
    ib->tail++;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}

static bool _inbox_park_cb(mco_coro* co, void* arg) {
    dtls_session_inbox_t* ib = arg;
    ib->parked = co;
    return true;
}

static dtls_dgram_t* _inbox_pop(dtls_session_inbox_t* ib) {
    while (ib->head == ib->tail) {
        if (ib->closed) {
            return NULL;
        }
        scheduler_park(ib->sched, _inbox_park_cb, ib);
        if (ib->closed) {
            return NULL;
        }
    }
    uint32_t mask = ib->cap - 1;
    dtls_dgram_t* dgram = ib->slots[ib->head & mask];
    ib->head++;
    return dgram;
}

static void _inbox_deadline_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    dtls_session_inbox_t* ib = ud;
    ib->timed_out = true;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}

static dtls_dgram_t* _inbox_pop_with_deadline(
    dtls_session_inbox_t* ib, uint64_t deadline_ms) {
    while (ib->head == ib->tail) {
        if (ib->closed) {
            return NULL;
        }
        ib->timed_out = false;
        if (deadline_ms > 0) {
            uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now >= deadline_ms) {
                return NULL;
            }
            sched_timer_start(ib->deadline_timer,
                              _inbox_deadline_cb, ib,
                              deadline_ms - now, 0);
        }
        scheduler_park(ib->sched, _inbox_park_cb, ib);
        if (deadline_ms > 0) {
            sched_timer_stop(ib->deadline_timer);
        }
        if (ib->timed_out) {
            return NULL;
        }
        if (ib->closed) {
            return NULL;
        }
    }
    uint32_t mask = ib->cap - 1;
    dtls_dgram_t* dgram = ib->slots[ib->head & mask];
    ib->head++;
    return dgram;
}

static void _inbox_close(dtls_session_inbox_t* ib) {
    if (!ib) {
        return;
    }
    ib->closed = true;
    if (ib->parked) {
        mco_coro* co = ib->parked;
        ib->parked = NULL;
        scheduler_schedule(ib->sched, co);
    }
}



static void _accept_queue_push(xylem_dtls_listener_t* ln,
                               xylem_dtls_conn_t* conn) {
    uint32_t mask = ln->accept_cap - 1;
    if (ln->accept_tail - ln->accept_head >= ln->accept_cap) {
        return;
    }
    ln->accept_slots[ln->accept_tail & mask] = conn;
    ln->accept_tail++;
    if (ln->accept_parked) {
        mco_coro* co = ln->accept_parked;
        ln->accept_parked = NULL;
        scheduler_schedule(ln->sched, co);
    }
}

static bool _accept_queue_park_cb(mco_coro* co, void* arg) {
    xylem_dtls_listener_t* ln = arg;
    ln->accept_parked = co;
    return true;
}

static xylem_dtls_conn_t* _accept_queue_pop(xylem_dtls_listener_t* ln) {
    while (ln->accept_head == ln->accept_tail) {
        if (ln->accept_closed) {
            return NULL;
        }
        scheduler_park(ln->sched, _accept_queue_park_cb, ln);
        if (ln->accept_closed) {
            return NULL;
        }
    }
    uint32_t mask = ln->accept_cap - 1;
    xylem_dtls_conn_t* conn = ln->accept_slots[ln->accept_head & mask];
    ln->accept_head++;
    return conn;
}

static void _accept_queue_close(xylem_dtls_listener_t* ln) {
    ln->accept_closed = true;
    if (ln->accept_parked) {
        mco_coro* co = ln->accept_parked;
        ln->accept_parked = NULL;
        scheduler_schedule(ln->sched, co);
    }
}



static void _dtls_server_flush_write_bio(xylem_dtls_conn_t* dtls) {
    char buf[16384];
    int  n;
    while ((n = BIO_read(dtls->write_bio, buf, sizeof(buf))) > 0) {
        socklen_t addrlen =
            (dtls->peer_addr.storage.ss_family == AF_INET6)
                ? (socklen_t)sizeof(struct sockaddr_in6)
                : (socklen_t)sizeof(struct sockaddr_in);
        platform_socket_sendto(
            dtls->listener->fd, buf, n,
            &dtls->peer_addr.storage, addrlen);
    }
}



static int _dtls_init_ssl(xylem_dtls_conn_t* dtls) {
    dtls->ssl = SSL_new(dtls->ctx->ssl_ctx);
    if (!dtls->ssl) {
        return -1;
    }
    dtls->read_bio  = BIO_new(BIO_s_mem());
    dtls->write_bio = BIO_new(BIO_s_mem());
    if (!dtls->read_bio || !dtls->write_bio) {
        BIO_free(dtls->read_bio);
        BIO_free(dtls->write_bio);
        SSL_free(dtls->ssl);
        dtls->ssl = NULL; dtls->read_bio = NULL; dtls->write_bio = NULL;
        return -1;
    }
    SSL_set_bio(dtls->ssl, dtls->read_bio, dtls->write_bio);
    return 0;
}



/**
 * Park on the right direction for an SSL_get_error result, with the
 * close-flag double-check that DTLS recv/send share. Returns:
 *   0  retry the SSL op
 *   1  peer closed (SSL_ERROR_ZERO_RETURN)
 *  -1  fatal -- abort
 */
static int _dtls_handle_io_block(xylem_dtls_conn_t* dtls, int ssl_err,
                                 const char* op_name) {
    iowait_result_t r;
    switch (ssl_err) {
    case SSL_ERROR_ZERO_RETURN:
        return 1;
    case SSL_ERROR_WANT_READ:
        r = iowait_read(dtls->waiter);
        break;
    case SSL_ERROR_WANT_WRITE:
        r = iowait_write(dtls->waiter);
        break;
    default: {
        unsigned long e = ERR_peek_error();
        xylem_loge("dtls %s: ssl_error=%d reason=%s",
                   op_name, ssl_err,
                   ERR_reason_error_string(e)
                       ? ERR_reason_error_string(e) : "unknown");
        return -1;
    }
    }
    if (r != IOWAIT_READY
        || atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return -1;
    }
    return 0;
}

static int _dtls_client_do_handshake(xylem_dtls_conn_t* dtls) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) {
            return 0;
        }
        int err = SSL_get_error(dtls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(dtls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(dtls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
        } else {
            unsigned long ssl_err = ERR_peek_error();
            xylem_loge("dtls handshake: ssl_error=%d reason=%s",
                       err,
                       ERR_reason_error_string(ssl_err)
                           ? ERR_reason_error_string(ssl_err)
                           : "unknown");
            return -1;
        }
    }
}

static void _dtls_cache_alpn(xylem_dtls_conn_t* dtls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(dtls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(dtls->alpn)) {
        memcpy(dtls->alpn, alpn_proto, alpn_len);
        dtls->alpn[alpn_len] = '\0';
    }
}

static void _dtls_client_free(xylem_dtls_conn_t* dtls) {
    if (dtls->ssl) {
        SSL_free(dtls->ssl);
    }
    if (dtls->waiter) {
        iowait_destroy(dtls->waiter);
    }
    if (dtls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(dtls->fd);
    }
    free(dtls);
}

xylem_dtls_conn_t* xylem_dtls_dial(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    bool            connected = false;
    platform_sock_t fd = platform_socket_dial(
        host, port_str, SOCK_DGRAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("dtls dial: socket failed for %s:%u", host, port);
        return NULL;
    }

    xylem_dtls_conn_t* dtls = calloc(1, sizeof(*dtls));
    if (!dtls) {
        platform_socket_close(fd);
        return NULL;
    }

    dtls->fd  = fd;
    dtls->ctx = ctx;
    addr_pton(host, port, &dtls->peer_addr);

    dtls->waiter = iowait_create(fd);
    if (!dtls->waiter) {
        platform_socket_close(fd);
        free(dtls);
        return NULL;
    }

    uint64_t timeout = (opts && opts->connect_timeout_ms > 0)
        ? opts->connect_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                        + timeout;
    iowait_set_rd_deadline(dtls->waiter, deadline);
    iowait_set_wr_deadline(dtls->waiter, deadline);

    dtls->ssl = SSL_new(ctx->ssl_ctx);
    if (!dtls->ssl) {
        _dtls_client_free(dtls);
        return NULL;
    }
    SSL_set_fd(dtls->ssl, (int)fd);
    SSL_set_connect_state(dtls->ssl);

    const char* server_name = opts ? opts->server_name : NULL;
    if (server_name) {
        /* RFC 6066 forbids IP literals in the SNI HostName extension. */
        addr_t tmp;
        bool is_ip = (addr_pton(server_name, 0, &tmp) == 0);
        if (!is_ip) {
            SSL_set_tlsext_host_name(dtls->ssl, server_name);
        }
        int vmode = SSL_get_verify_mode(dtls->ssl);
        if (vmode & SSL_VERIFY_PEER) {
            SSL_set1_host(dtls->ssl, server_name);
        }
    } else if (SSL_get_verify_mode(dtls->ssl) & SSL_VERIFY_PEER) {
        xylem_logw("dtls dial: verify_peer enabled but opts->server_name "
                   "is NULL; peer identity is not checked, only "
                   "certificate chain trust (MITM risk)");
    }

    if (_dtls_client_do_handshake(dtls) != 0) {
        _dtls_client_free(dtls);
        return NULL;
    }

    iowait_set_rd_deadline(dtls->waiter, 0);
    iowait_set_wr_deadline(dtls->waiter, 0);

    _dtls_cache_alpn(dtls);
    return dtls;
}

static int64_t _dtls_client_recv(xylem_dtls_conn_t* dtls,
                                 void* buf, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return -1;
    }
    for (;;) {
        ERR_clear_error();
        size_t chunk = len > INT_MAX ? (size_t)INT_MAX : len;
        int    n     = SSL_read(dtls->ssl, buf, (int)chunk);
        if (n > 0) {
            return n;
        }
        int rc = _dtls_handle_io_block(
            dtls, SSL_get_error(dtls->ssl, n), "SSL_read");
        if (rc == 1) {
            return 0;
        }
        if (rc < 0) {
            return -1;
        }
    }
}

static int _dtls_client_send(xylem_dtls_conn_t* dtls,
                             const void* data, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return -1;
    }
    if (len > INT_MAX) {
        xylem_loge("dtls send: len %zu exceeds INT_MAX", len);
        return -1;
    }
    for (;;) {
        ERR_clear_error();
        int n = SSL_write(dtls->ssl, data, (int)len);
        if (n > 0) {
            return 0;
        }
        int rc = _dtls_handle_io_block(
            dtls, SSL_get_error(dtls->ssl, n), "SSL_write");
        if (rc != 0) {
            return -1;
        }
    }
}

static void _dtls_client_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }
    iowait_close(dtls->waiter);
    _dtls_client_free(dtls);
}



static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls);

static void _dtls_retransmit_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_dtls_conn_t* dtls = ud;
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return;
    }
    DTLSv1_handle_timeout(dtls->ssl);
    _dtls_server_flush_write_bio(dtls);
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
        sched_timer_start(dtls->retransmit_timer,
                          _dtls_retransmit_cb, dtls, ms, 0);
    }
}

static void _dtls_stop_retransmit(xylem_dtls_conn_t* dtls) {
    if (dtls->retransmit_timer) {
        sched_timer_stop(dtls->retransmit_timer);
    }
}

static void _dtls_handshake_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)timer;
    xylem_dtls_conn_t* dtls = ud;
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return;
    }
    _inbox_close(dtls->inbox);
}

static void _dtls_handshake_coro(void* arg) {
    xylem_dtls_conn_t* dtls = arg;
    xylem_dtls_listener_t* ln = dtls->listener;

    if (_dtls_init_ssl(dtls) != 0) {
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        _inbox_destroy(dtls->inbox);
        free(dtls);
        return;
    }

    SSL_set_accept_state(dtls->ssl);
    SSL_set_ex_data(dtls->ssl, _dtls_peer_addr_idx, &dtls->peer_addr);

    sched_timer_start(dtls->handshake_timer,
                      _dtls_handshake_timeout_cb, dtls,
                      DTLS_DEFAULT_TIMEOUT_MS, 0);

    bool success = false;
    while (!dtls->handshake_done) {
        dtls_dgram_t* dgram = _inbox_pop(dtls->inbox);
        if (!dgram) {
            break;
        }

        BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
        free(dgram);

        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) {
            /**
             * SSL returned success but Server Finished is still
             * buffered in the write BIO; flush so the client can
             * complete.
             */
            _dtls_server_flush_write_bio(dtls);
            dtls->handshake_done = true;
            success = true;
            break;
        }

        int err = SSL_get_error(dtls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            _dtls_server_flush_write_bio(dtls);
            _dtls_arm_retransmit(dtls);
            continue;
        }

        _dtls_server_flush_write_bio(dtls);
        break;
    }

    _dtls_stop_retransmit(dtls);
    sched_timer_stop(dtls->handshake_timer);

    if (!success) {
        SSL_free(dtls->ssl);
        dtls->ssl = NULL;
        sched_timer_destroy(dtls->retransmit_timer);
        sched_timer_destroy(dtls->handshake_timer);
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        _inbox_destroy(dtls->inbox);
        free(dtls);
        return;
    }

    _dtls_cache_alpn(dtls);
    _accept_queue_push(ln, dtls);
}

static void _dtls_dispatcher(void* arg) {
    xylem_dtls_listener_t* ln = arg;
    char buf[65535];

    while (!atomic_load_explicit(&ln->closed, memory_order_acquire)) {
        struct sockaddr_storage from_ss;
        socklen_t from_len = sizeof(from_ss);
        ssize_t n = platform_socket_recvfrom(
            ln->fd, buf, sizeof(buf), &from_ss, &from_len);

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                iowait_result_t r = iowait_read(ln->waiter);
                if (r != IOWAIT_READY) {
                    break;
                }
                continue;
            }
            continue;
        }

        addr_t from_addr;
        memcpy(&from_addr.storage, &from_ss, sizeof(from_ss));

        mtx_lock(&ln->sessions_mtx);
        xylem_dtls_conn_t* dtls = _dtls_find_session(ln, &from_addr);
        mtx_unlock(&ln->sessions_mtx);

        if (dtls) {
            dtls_dgram_t* dgram = malloc(sizeof(dtls_dgram_t) + (size_t)n);
            if (dgram) {
                dgram->len = (size_t)n;
                memcpy(dgram->data, buf, (size_t)n);
                _inbox_push(dtls->inbox, dgram);
            }
            continue;
        }

        dtls = calloc(1, sizeof(*dtls));
        if (!dtls) {
            continue;
        }
        dtls->ctx       = ln->ctx;
        dtls->peer_addr = from_addr;
        dtls->listener  = ln;
        dtls->inbox     = _inbox_create(ln->sched);
        if (!dtls->inbox) {
            free(dtls);
            continue;
        }

        dtls->retransmit_timer = sched_timer_create(ln->sched);
        dtls->handshake_timer  = sched_timer_create(ln->sched);
        if (!dtls->retransmit_timer || !dtls->handshake_timer) {
            sched_timer_destroy(dtls->retransmit_timer);
            sched_timer_destroy(dtls->handshake_timer);
            _inbox_destroy(dtls->inbox);
            free(dtls);
            continue;
        }

        dtls_dgram_t* dgram = malloc(sizeof(dtls_dgram_t) + (size_t)n);
        if (!dgram) {
            sched_timer_destroy(dtls->retransmit_timer);
            sched_timer_destroy(dtls->handshake_timer);
            _inbox_destroy(dtls->inbox);
            free(dtls);
            continue;
        }
        dgram->len = (size_t)n;
        memcpy(dgram->data, buf, (size_t)n);
        _inbox_push(dtls->inbox, dgram);

        mtx_lock(&ln->sessions_mtx);
        rbtree_insert(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);

        runtime_spawn(_dtls_handshake_coro, dtls);
    }
}

xylem_dtls_listener_t* xylem_dtls_listen(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("dtls listen: failed for %s:%u", host, port);
        return NULL;
    }

    xylem_dtls_listener_t* ln = calloc(1, sizeof(*ln));
    if (!ln) {
        platform_socket_close(fd);
        return NULL;
    }

    ln->fd    = fd;
    ln->ctx   = ctx;
    ln->sched = runtime_get_scheduler();
    if (opts) {
        ln->opts = *opts;
    }

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    rbtree_init(&ln->sessions,
                _dtls_session_cmp_nn, _dtls_session_cmp_kn);
    mtx_init(&ln->sessions_mtx, mtx_plain);

    ln->accept_cap   = 64;
    ln->accept_slots = calloc(ln->accept_cap,
                              sizeof(xylem_dtls_conn_t*));
    if (!ln->accept_slots) {
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    runtime_spawn(_dtls_dispatcher, ln);
    return ln;
}

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    return _accept_queue_pop(ln);
}



static int64_t _dtls_server_recv(xylem_dtls_conn_t* dtls,
                                 void* buf, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return -1;
    }
    for (;;) {
        dtls_dgram_t* dgram = _inbox_pop_with_deadline(
            dtls->inbox, dtls->rd_deadline_ms);
        if (!dgram) {
            return -1;
        }
        BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
        free(dgram);

        ERR_clear_error();
        size_t rchunk = len > INT_MAX ? (size_t)INT_MAX : len;
        int    n      = SSL_read(dtls->ssl, buf, (int)rchunk);
        if (n > 0) {
            return n;
        }

        int err = SSL_get_error(dtls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (err != SSL_ERROR_WANT_READ) {
            return -1;
        }
        /* WANT_READ: pull next datagram from the inbox and feed BIO. */
    }
}

static int _dtls_server_send(xylem_dtls_conn_t* dtls,
                             const void* data, size_t len) {
    if (atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return -1;
    }
    if (len > INT_MAX) {
        xylem_loge("dtls send: len %zu exceeds INT_MAX", len);
        return -1;
    }
    ERR_clear_error();
    int n = SSL_write(dtls->ssl, data, (int)len);
    if (n <= 0) {
        return -1;
    }
    _dtls_server_flush_write_bio(dtls);
    return 0;
}

int64_t xylem_dtls_recv(xylem_dtls_conn_t* dtls, void* buf, size_t len) {
    if (dtls->listener) {
        return _dtls_server_recv(dtls, buf, len);
    }
    return _dtls_client_recv(dtls, buf, len);
}

int xylem_dtls_send(xylem_dtls_conn_t* dtls,
                    const void* data, size_t len) {
    if (dtls->listener) {
        return _dtls_server_send(dtls, data, len);
    }
    return _dtls_client_send(dtls, data, len);
}

static void _dtls_server_session_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }
    _dtls_stop_retransmit(dtls);
    if (dtls->handshake_timer) {
        sched_timer_stop(dtls->handshake_timer);
    }

    if (dtls->handshake_done && dtls->ssl) {
        SSL_shutdown(dtls->ssl);
        _dtls_server_flush_write_bio(dtls);
    }

    _inbox_close(dtls->inbox);

    xylem_dtls_listener_t* ln = dtls->listener;
    mtx_lock(&ln->sessions_mtx);
    rbtree_remove(&ln->sessions, &dtls->server_node);
    mtx_unlock(&ln->sessions_mtx);

    if (dtls->ssl) {
        SSL_free(dtls->ssl);
        dtls->ssl = NULL;
    }
    sched_timer_destroy(dtls->retransmit_timer);
    sched_timer_destroy(dtls->handshake_timer);
    _inbox_destroy(dtls->inbox);
    free(dtls);
}

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    if (dtls->listener) {
        _dtls_server_session_close(dtls);
    } else {
        _dtls_client_close(dtls);
    }
}

void xylem_dtls_close_listener(xylem_dtls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    mtx_lock(&ln->sessions_mtx);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_dtls_conn_t* dtls =
            rbtree_entry(node, xylem_dtls_conn_t, server_node);
        mtx_unlock(&ln->sessions_mtx);
        xylem_dtls_close(dtls);
        mtx_lock(&ln->sessions_mtx);
    }
    mtx_unlock(&ln->sessions_mtx);

    iowait_close(ln->waiter);
    _accept_queue_close(ln);
    /* Let the dispatcher coroutine exit after iowait_close wakes it. */
    runtime_sleep(1);
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    mtx_destroy(&ln->sessions_mtx);
    free(ln->accept_slots);
    free(ln);
}

void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->listener) {
        dtls->rd_deadline_ms = deadline_ms;
    } else {
        iowait_set_rd_deadline(dtls->waiter, deadline_ms);
    }
}

void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->listener) {
        dtls->wr_deadline_ms = deadline_ms;
    } else {
        iowait_set_wr_deadline(dtls->waiter, deadline_ms);
    }
}

const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls) {
    return dtls->alpn[0] ? dtls->alpn : NULL;
}

int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port) {
    return addr_ntop(&dtls->peer_addr, host, host_len, port);
}

int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port) {
    platform_sock_t fd = dtls->listener ? dtls->listener->fd : dtls->fd;
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char* host, size_t host_len, uint16_t* port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(ln->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}
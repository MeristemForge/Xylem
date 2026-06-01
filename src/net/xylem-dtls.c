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
#include "xylem/sync/xylem-mutex.h"
#include "xylem/sync/xylem-channel.h"

#include "xylem/crypto/xylem-hmac256.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "container/rbtree.h"
#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "thrds.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DTLS_DEFAULT_TIMEOUT_MS  30000
#define DTLS_COOKIE_SIZE         32
#define DTLS_INBOX_CAP           64
#define DTLS_PKT_BUF_SIZE        1500

typedef struct _dtls_dgram_s {
    size_t len;
    char   data[];
} _dtls_dgram_t;

struct xylem_dtls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
    uint8_t  cookie_secret[DTLS_COOKIE_SIZE];
};

struct xylem_dtls_conn_s {
    SSL*                    ssl;
    addr_t                  peer_addr;
    char                    alpn[256];
    _Atomic bool            closed;
    _Atomic int32_t         refcnt;
    bool                    handshake_done;

    /**
     * Memory BIOs decouple the SSL state machine from socket parking.
     * Both client and server feed inbound ciphertext into read_bio and
     * drain outbound ciphertext from write_bio; SSL never touches a
     * socket directly. The server is fed by the listener dispatcher;
     * the client pumps its own connected socket (below).
     */
    BIO*                     read_bio;
    BIO*                     write_bio;

    /* client-side only */
    iowait_t*               waiter;
    platform_sock_t          fd;
    xylem_mutex_t*           ssl_mu;   /*< serializes all SSL/BIO access. */
    xylem_mutex_t*           rd_mu;    /*< sole parker on iowait read dir.  */
    xylem_mutex_t*           wr_mu;    /*< sole parker on iowait write dir. */

    /* server-side only */
    xylem_channel_t*         inbox;
    _Atomic int32_t          inbox_len;
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
    xylem_mutex_t*        write_mu;
    scheduler_t*          sched;

    xylem_channel_t*      accept_ch;

    _Atomic bool          closed;
    _Atomic int32_t       refcnt;
};

static int _dtls_ex_data_idx = -1;
static int _dtls_peer_addr_idx = -1;
static once_flag _dtls_ex_data_once = ONCE_FLAG_INIT;

static void _dtls_init_ex_data(void) {
    _dtls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _dtls_peer_addr_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

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
        *out_len = sizeof(struct sockaddr_in);
    } else if (addr->storage.ss_family == AF_INET6) {
        *out_len = sizeof(struct sockaddr_in6);
    } else {
        return -1;
    }
    *out = (const uint8_t*)&addr->storage;
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

static void _dtls_conn_ref(xylem_dtls_conn_t* dtls) {
    atomic_fetch_add_explicit(&dtls->refcnt, 1, memory_order_relaxed);
}

static void _dtls_conn_unref(xylem_dtls_conn_t* dtls) {
    if (atomic_fetch_sub_explicit(&dtls->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (dtls->ssl) {
        SSL_free(dtls->ssl);
    }
    if (dtls->waiter) {
        iowait_destroy(dtls->waiter);
    }
    if (dtls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(dtls->fd);
    }
    xylem_mutex_destroy(dtls->ssl_mu);
    xylem_mutex_destroy(dtls->rd_mu);
    xylem_mutex_destroy(dtls->wr_mu);
    sched_timer_destroy(dtls->retransmit_timer);
    sched_timer_destroy(dtls->handshake_timer);
    if (dtls->inbox) {
        /* Drain residual datagrams (freeing their payloads, which the
         * channel itself does not own) then destroy the channel. The
         * inbox was already closed by xylem_dtls_close, so recv never
         * parks here; it pops leftovers and returns NULL once empty. */
        _dtls_dgram_t* dgram;
        while ((dgram = (_dtls_dgram_t*)xylem_channel_recv(dtls->inbox))
               != NULL) {
            free(dgram);
        }
        xylem_channel_destroy(dtls->inbox);
    }
    free(dtls);
}

static void _dtls_listener_ref(xylem_dtls_listener_t* ln) {
    atomic_fetch_add_explicit(&ln->refcnt, 1, memory_order_relaxed);
}

static void _dtls_listener_unref(xylem_dtls_listener_t* ln) {
    if (atomic_fetch_sub_explicit(&ln->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    mtx_destroy(&ln->sessions_mtx);
    xylem_mutex_destroy(ln->write_mu);
    if (ln->accept_ch) {
        xylem_channel_destroy(ln->accept_ch);
    }
    free(ln);
}

/**
 * Copy a datagram into the session inbox channel. The receiver frees
 * it. Bounded by DTLS_INBOX_CAP via dtls->inbox_len so a slow or
 * absent reader cannot grow the channel without bound; on overflow
 * the datagram is dropped (DTLS tolerates loss, the peer retransmits).
 * The dispatcher holds sessions_mtx across find+push, so the session
 * cannot be freed underneath this call.
 */
static void _dtls_inbox_push(xylem_dtls_conn_t* dtls, _dtls_dgram_t* dgram) {
    if (atomic_load_explicit(&dtls->inbox_len, memory_order_relaxed)
        >= (int32_t)DTLS_INBOX_CAP) {
        free(dgram);
        return;
    }
    atomic_fetch_add_explicit(&dtls->inbox_len, 1, memory_order_relaxed);
    if (xylem_channel_send(dtls->inbox, dgram) != 0) {
        atomic_fetch_sub_explicit(&dtls->inbox_len, 1, memory_order_relaxed);
        free(dgram);
    }
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

static void _dtls_sessions_init(rbtree_t* tree) {
    rbtree_init(tree, _dtls_session_cmp_nn, _dtls_session_cmp_kn);
}

static void _dtls_server_flush_write_bio(xylem_dtls_conn_t* dtls) {
    char buf[DTLS_PKT_BUF_SIZE];
    int  n;
    socklen_t addrlen =
        (dtls->peer_addr.storage.ss_family == AF_INET6)
            ? (socklen_t)sizeof(struct sockaddr_in6)
            : (socklen_t)sizeof(struct sockaddr_in);

    while ((n = BIO_read(dtls->write_bio, buf, sizeof(buf))) > 0) {
        platform_socket_sendto(
            dtls->listener->fd, buf, n,
            &dtls->peer_addr.storage, addrlen);
    }
}

static int _dtls_server_send_record(xylem_dtls_conn_t* dtls,
                                   const void* data, int len) {
    socklen_t addrlen =
        (dtls->peer_addr.storage.ss_family == AF_INET6)
            ? (socklen_t)sizeof(struct sockaddr_in6)
            : (socklen_t)sizeof(struct sockaddr_in);

    ERR_clear_error();
    int n = SSL_write(dtls->ssl, data, len);
    if (n <= 0) {
        return -1;
    }

    /* DTLS: one SSL_write produces exactly one datagram. */
    char buf[DTLS_PKT_BUF_SIZE];
    int rd = BIO_read(dtls->write_bio, buf, sizeof(buf));
    if (rd <= 0) {
        return -1;
    }

    for (;;) {
        ssize_t sent = platform_socket_sendto(
            dtls->listener->fd, buf, rd,
            &dtls->peer_addr.storage, addrlen);
        if (sent >= 0) {
            return 0;
        }
        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            return -1;
        }
        iowait_result_t r = iowait_write(dtls->listener->waiter);
        if (r != IOWAIT_READY) {
            return -1;
        }
    }
}

static int _dtls_init_ssl(xylem_dtls_conn_t* dtls, SSL_CTX* ssl_ctx) {
    dtls->ssl = SSL_new(ssl_ctx);
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

/* Sentinel returned by _dtls_client_pump_in when the read direction
 * timed out (deadline reached) rather than failing outright. */
#define DTLS_PUMP_TIMEOUT (-2)

/**
 * Drain pending outbound ciphertext from write_bio to the connected
 * socket, one datagram per BIO_read. Holds wr_mu so it is the sole
 * parker on the iowait write direction, and takes ssl_mu only for the
 * BIO_read itself -- never across a socket park -- so a concurrent
 * reader can still touch the SSL state. Returns 0 once write_bio is
 * empty, -1 on socket error or close.
 */
static int _dtls_client_pump_out(xylem_dtls_conn_t* dtls) {
    int  ret = 0;
    char buf[DTLS_PKT_BUF_SIZE];

    xylem_mutex_lock(dtls->wr_mu);
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        int n = BIO_read(dtls->write_bio, buf, sizeof(buf));
        xylem_mutex_unlock(dtls->ssl_mu);
        if (n <= 0) {
            break;
        }
        for (;;) {
            ssize_t sent = platform_socket_send(dtls->fd, buf, n);
            if (sent >= 0) {
                break;
            }
            int err = platform_socket_get_lasterror();
            if (err != PLATFORM_SO_ERROR_EAGAIN
                && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
                ret = -1;
                goto out;
            }
            iowait_result_t r = iowait_write(dtls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&dtls->closed,
                                        memory_order_acquire)) {
                ret = -1;
                goto out;
            }
        }
    }
out:
    xylem_mutex_unlock(dtls->wr_mu);
    return ret;
}

/**
 * Read one inbound datagram from the connected socket into read_bio.
 * Holds rd_mu so it is the sole parker on the iowait read direction,
 * and takes ssl_mu only for the BIO_write -- never across a socket
 * park. Returns the byte count fed (>0), 0 on peer EOF,
 * DTLS_PUMP_TIMEOUT when the read deadline passed, -1 on error/close.
 */
static int _dtls_client_pump_in(xylem_dtls_conn_t* dtls) {
    int  ret = -1;
    char buf[DTLS_PKT_BUF_SIZE];

    xylem_mutex_lock(dtls->rd_mu);
    for (;;) {
        ssize_t n = platform_socket_recv(dtls->fd, buf, sizeof(buf));
        if (n > 0) {
            xylem_mutex_lock(dtls->ssl_mu);
            BIO_write(dtls->read_bio, buf, (int)n);
            xylem_mutex_unlock(dtls->ssl_mu);
            ret = (int)n;
            break;
        }
        if (n == 0) {
            ret = 0;
            break;
        }
        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            ret = -1;
            break;
        }
        iowait_result_t r = iowait_read(dtls->waiter);
        if (r == IOWAIT_TIMEOUT) {
            ret = DTLS_PUMP_TIMEOUT;
            break;
        }
        if (r != IOWAIT_READY
            || atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
            ret = -1;
            break;
        }
    }
    xylem_mutex_unlock(dtls->rd_mu);
    return ret;
}

/**
 * Drive the client handshake on the memory BIOs, pumping ciphertext
 * to/from the connected socket as the SSL state machine demands. The
 * read wait is bounded by both the overall handshake deadline and the
 * DTLS retransmit timer (DTLSv1_get_timeout); on the latter expiring
 * DTLSv1_handle_timeout retransmits the last flight.
 */
static int _dtls_client_do_handshake(xylem_dtls_conn_t* dtls,
                                    uint64_t deadline) {
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        int err = (ret == 1) ? 0 : SSL_get_error(dtls->ssl, ret);
        xylem_mutex_unlock(dtls->ssl_mu);

        /* Flush any handshake flight produced before waiting on input. */
        if (_dtls_client_pump_out(dtls) != 0) {
            return -1;
        }

        if (ret == 1) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            uint64_t       rd_dl = deadline;
            struct timeval tv;
            xylem_mutex_lock(dtls->ssl_mu);
            int have_to = DTLSv1_get_timeout(dtls->ssl, &tv);
            xylem_mutex_unlock(dtls->ssl_mu);
            if (have_to) {
                uint64_t ms = (uint64_t)tv.tv_sec * 1000
                            + (uint64_t)tv.tv_usec / 1000;
                if (ms == 0) {
                    ms = 1;
                }
                uint64_t rt_dl =
                    xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + ms;
                if (rd_dl == 0 || rt_dl < rd_dl) {
                    rd_dl = rt_dl;
                }
            }
            iowait_set_rd_deadline(dtls->waiter, rd_dl);

            int rc = _dtls_client_pump_in(dtls);
            if (rc == DTLS_PUMP_TIMEOUT) {
                uint64_t now =
                    xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                if (deadline > 0 && now >= deadline) {
                    return -1;
                }
                xylem_mutex_lock(dtls->ssl_mu);
                DTLSv1_handle_timeout(dtls->ssl);
                xylem_mutex_unlock(dtls->ssl_mu);
                continue;
            }
            if (rc <= 0) {
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            /* Memory BIO never blocks writes; pump_out already flushed. */
            continue;
        }

        unsigned long e = ERR_peek_error();
        xylem_loge("dtls handshake: ssl_error=%d reason=%s",
                   err,
                   ERR_reason_error_string(e)
                       ? ERR_reason_error_string(e) : "unknown");
        return -1;
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

static void _dtls_retransmit_cb(sched_timer_t* timer, void* ud);

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

static int _dtls_client_recv(xylem_dtls_conn_t* dtls, void* buf, int len) {
    _dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            xylem_mutex_lock(dtls->ssl_mu);
            ERR_clear_error();
            int n   = SSL_read(dtls->ssl, buf, len);
            int err = (n > 0) ? 0 : SSL_get_error(dtls->ssl, n);
            xylem_mutex_unlock(dtls->ssl_mu);

            if (n > 0) {
                ret = n;
                break;
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                ret = 0;
                break;
            }
            if (err == SSL_ERROR_WANT_READ) {
                /* Need more ciphertext; fetch one datagram. A read
                 * deadline surfaces as DTLS_PUMP_TIMEOUT (-2) and ends
                 * the read with -1, matching the documented contract. */
                int rc = _dtls_client_pump_in(dtls);
                if (rc <= 0) {
                    break;
                }
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                /* Post-handshake message (rekey) must flush first. */
                if (_dtls_client_pump_out(dtls) != 0) {
                    break;
                }
                continue;
            }
            unsigned long e = ERR_peek_error();
            xylem_loge("dtls SSL_read: ssl_error=%d reason=%s",
                       err,
                       ERR_reason_error_string(e)
                           ? ERR_reason_error_string(e) : "unknown");
            break;
        }
    }

    _dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_client_send(xylem_dtls_conn_t* dtls,
                            const void* data, int len) {
    _dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            xylem_mutex_lock(dtls->ssl_mu);
            ERR_clear_error();
            int n   = SSL_write(dtls->ssl, data, len);
            int err = (n > 0) ? 0 : SSL_get_error(dtls->ssl, n);
            xylem_mutex_unlock(dtls->ssl_mu);

            if (n > 0) {
                /* Flush the datagram SSL just buffered into write_bio. */
                ret = (_dtls_client_pump_out(dtls) == 0) ? 0 : -1;
                break;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (_dtls_client_pump_out(dtls) != 0) {
                    break;
                }
                continue;
            }
            if (err == SSL_ERROR_WANT_READ) {
                /* Rekey needs inbound data before the write completes. */
                if (_dtls_client_pump_out(dtls) != 0) {
                    break;
                }
                if (_dtls_client_pump_in(dtls) <= 0) {
                    break;
                }
                continue;
            }
            unsigned long e = ERR_peek_error();
            xylem_loge("dtls SSL_write: ssl_error=%d reason=%s",
                       err,
                       ERR_reason_error_string(e)
                           ? ERR_reason_error_string(e) : "unknown");
            break;
        }
    }

    _dtls_conn_unref(dtls);
    return ret;
}

static void _dtls_client_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }
    /**
     * Do not touch the SSL object here: a concurrent recv/send may be
     * inside SSL_read/SSL_write under ssl_mu. Flipping closed + waking
     * both iowait directions makes those calls return -1 and drop their
     * ref; SSL_free then runs once at the final unref, with no parker
     * left. (close_notify is best-effort on a datagram socket and is
     * intentionally skipped, matching the TLS close path.)
     */
    iowait_close(dtls->waiter);
    _dtls_conn_unref(dtls);
}

static void _dtls_handshake_coro(void* arg) {
    xylem_dtls_conn_t* dtls = arg;
    xylem_dtls_listener_t* ln = dtls->listener;

    _dtls_conn_ref(dtls);

    if (_dtls_init_ssl(dtls, ln->ctx->ssl_ctx) != 0) {
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        _dtls_conn_unref(dtls);
        _dtls_conn_unref(dtls);
        return;
    }

    SSL_set_accept_state(dtls->ssl);
    SSL_set_ex_data(dtls->ssl, _dtls_peer_addr_idx, &dtls->peer_addr);

    uint64_t hs_timeout = ln->opts.handshake_timeout_ms > 0
        ? ln->opts.handshake_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t hs_deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + hs_timeout;

    bool success = false;
    while (!dtls->handshake_done) {
        /**
         * Bound the wait with the handshake deadline directly on the
         * channel recv, instead of an external timer closing the
         * inbox: closing the inbox while the session is still in the
         * listener's tree would let the dispatcher send into a closed
         * channel (which aborts). On timeout recv returns NULL and we
         * fall through to the failure path below.
         */
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        uint64_t remaining = (now >= hs_deadline) ? 0 : hs_deadline - now;
        if (remaining == 0) {
            break;
        }
        _dtls_dgram_t* dgram = (_dtls_dgram_t*)xylem_channel_recv_timeout(
            dtls->inbox, remaining);
        if (!dgram) {
            break;
        }
        atomic_fetch_sub_explicit(&dtls->inbox_len, 1,
                                  memory_order_relaxed);

        BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
        free(dgram);

        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) {
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

    sched_timer_destroy(dtls->retransmit_timer);
    sched_timer_destroy(dtls->handshake_timer);
    dtls->retransmit_timer = NULL;
    dtls->handshake_timer  = NULL;

    if (!success) {
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        _dtls_conn_unref(dtls);
        _dtls_conn_unref(dtls);
        return;
    }

    _dtls_cache_alpn(dtls);
    xylem_channel_send(ln->accept_ch, dtls);
    _dtls_conn_unref(dtls);
}

static void _dtls_dispatcher(void* arg) {
    xylem_dtls_listener_t* ln = arg;
    char buf[DTLS_PKT_BUF_SIZE];

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
        if (dtls) {
            /* Push under the lock so a concurrent xylem_dtls_close
             * cannot remove + free the session between the lookup and
             * the push. _dtls_inbox_push only enqueues (it never
             * parks), so the critical section stays short. */
            _dtls_dgram_t* dgram =
                (_dtls_dgram_t*)malloc(sizeof(_dtls_dgram_t) + (size_t)n);
            if (dgram) {
                dgram->len = (size_t)n;
                memcpy(dgram->data, buf, (size_t)n);
                _dtls_inbox_push(dtls, dgram);
            }
            mtx_unlock(&ln->sessions_mtx);
            continue;
        }
        mtx_unlock(&ln->sessions_mtx);

        _dtls_dgram_t* dgram =
            (_dtls_dgram_t*)malloc(sizeof(_dtls_dgram_t) + (size_t)n);
        if (!dgram) {
            continue;
        }
        dgram->len = (size_t)n;
        memcpy(dgram->data, buf, (size_t)n);

        dtls = (xylem_dtls_conn_t*)calloc(1, sizeof(xylem_dtls_conn_t));
        if (!dtls) {
            free(dgram);
            continue;
        }
        atomic_store_explicit(&dtls->refcnt, 1, memory_order_relaxed);
        dtls->fd               = PLATFORM_SO_ERROR_INVALID_SOCKET;
        dtls->peer_addr        = from_addr;
        dtls->listener         = ln;
        dtls->inbox            = xylem_channel_create();
        dtls->retransmit_timer = sched_timer_create(ln->sched);
        dtls->handshake_timer  = sched_timer_create(ln->sched);

        if (!dtls->inbox
            || !dtls->retransmit_timer
            || !dtls->handshake_timer) {
            sched_timer_destroy(dtls->retransmit_timer);
            sched_timer_destroy(dtls->handshake_timer);
            if (dtls->inbox) {
                xylem_channel_destroy(dtls->inbox);
            }
            free(dtls);
            free(dgram);
            continue;
        }

        _dtls_inbox_push(dtls, dgram);

        mtx_lock(&ln->sessions_mtx);
        rbtree_insert(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);

        runtime_spawn(_dtls_handshake_coro, dtls);
    }

    _dtls_listener_unref(ln);
}

static int _dtls_server_recv(xylem_dtls_conn_t* dtls, void* buf, int len) {
    _dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            _dtls_dgram_t* dgram = NULL;
            if (dtls->rd_deadline_ms > 0) {
                uint64_t now =
                    xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                uint64_t remaining = (now >= dtls->rd_deadline_ms)
                    ? 0 : dtls->rd_deadline_ms - now;
                if (remaining == 0) {
                    break;
                }
                dgram = (_dtls_dgram_t*)xylem_channel_recv_timeout(
                    dtls->inbox, remaining);
            } else {
                dgram = (_dtls_dgram_t*)xylem_channel_recv(dtls->inbox);
            }
            if (!dgram) {
                break;
            }
            atomic_fetch_sub_explicit(&dtls->inbox_len, 1,
                                      memory_order_relaxed);
            BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
            free(dgram);

            ERR_clear_error();
            int n = SSL_read(dtls->ssl, buf, len);
            if (n > 0) {
                ret = n;
                break;
            }

            int err = SSL_get_error(dtls->ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) {
                ret = 0;
                break;
            }
            if (err != SSL_ERROR_WANT_READ) {
                break;
            }
        }
    }

    _dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_server_send(xylem_dtls_conn_t* dtls,
                            const void* data, int len) {
    _dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        xylem_mutex_lock(dtls->listener->write_mu);
        if (dtls->wr_deadline_ms > 0) {
            iowait_set_wr_deadline(
                dtls->listener->waiter, dtls->wr_deadline_ms);
        }
        ret = _dtls_server_send_record(dtls, data, len);
        if (dtls->wr_deadline_ms > 0) {
            iowait_set_wr_deadline(dtls->listener->waiter, 0);
        }
        xylem_mutex_unlock(dtls->listener->write_mu);
    }

    _dtls_conn_unref(dtls);
    return ret;
}

static void _dtls_server_close_conn(xylem_dtls_conn_t* dtls) {
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

    /* Unlink from the session tree FIRST so the dispatcher can no
     * longer find this session and therefore cannot xylem_channel_send
     * into the inbox after we close it (send-on-closed aborts). The
     * dispatcher does find+push under sessions_mtx, so once the remove
     * commits no further push can target this inbox. */
    xylem_dtls_listener_t* ln = dtls->listener;
    mtx_lock(&ln->sessions_mtx);
    rbtree_remove(&ln->sessions, &dtls->server_node);
    mtx_unlock(&ln->sessions_mtx);

    /* Now close the inbox to wake a parked reader; the reader's
     * in-flight recv holds a channel reference, so the channel stays
     * alive until _dtls_conn_unref drains and destroys it. */
    if (dtls->inbox) {
        xylem_channel_close(dtls->inbox);
    }

    _dtls_conn_unref(dtls);
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

    xylem_dtls_conn_t* dtls =
        (xylem_dtls_conn_t*)calloc(1, sizeof(xylem_dtls_conn_t));
    if (!dtls) {
        platform_socket_close(fd);
        return NULL;
    }

    atomic_store_explicit(&dtls->refcnt, 1, memory_order_relaxed);
    dtls->fd  = fd;
    addr_pton(host, port, &dtls->peer_addr);

    dtls->waiter = iowait_create(fd);
    dtls->ssl_mu = xylem_mutex_create();
    dtls->rd_mu  = xylem_mutex_create();
    dtls->wr_mu  = xylem_mutex_create();
    if (!dtls->waiter || !dtls->ssl_mu || !dtls->rd_mu || !dtls->wr_mu) {
        _dtls_conn_unref(dtls);
        return NULL;
    }

    uint64_t timeout = (opts && opts->handshake_timeout_ms > 0)
        ? opts->handshake_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                        + timeout;
    iowait_set_rd_deadline(dtls->waiter, deadline);
    iowait_set_wr_deadline(dtls->waiter, deadline);

    /**
     * Memory BIOs (not SSL_set_fd): the connection pumps its own socket
     * via _dtls_client_pump_in/out, which hold rd_mu/wr_mu so each
     * iowait direction has a single parker. This lets one coroutine
     * read while another writes the same connection without tripping
     * the iowait one-parker-per-direction rule.
     */
    if (_dtls_init_ssl(dtls, ctx->ssl_ctx) != 0) {
        _dtls_conn_unref(dtls);
        return NULL;
    }
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

    if (_dtls_client_do_handshake(dtls, deadline) != 0) {
        _dtls_conn_unref(dtls);
        return NULL;
    }

    iowait_set_rd_deadline(dtls->waiter, 0);
    iowait_set_wr_deadline(dtls->waiter, 0);

    _dtls_cache_alpn(dtls);
    return dtls;
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

    xylem_dtls_listener_t* ln =
        (xylem_dtls_listener_t*)calloc(1, sizeof(xylem_dtls_listener_t));
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

    atomic_store_explicit(&ln->refcnt, 2, memory_order_relaxed);

    _dtls_sessions_init(&ln->sessions);
    mtx_init(&ln->sessions_mtx, mtx_plain);
    ln->write_mu = xylem_mutex_create();

    ln->accept_ch = xylem_channel_create();
    if (!ln->accept_ch) {
        xylem_mutex_destroy(ln->write_mu);
        mtx_destroy(&ln->sessions_mtx);
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    runtime_spawn(_dtls_dispatcher, ln);
    return ln;
}

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    _dtls_listener_ref(ln);
    xylem_dtls_conn_t* conn =
        (xylem_dtls_conn_t*)xylem_channel_recv(ln->accept_ch);
    _dtls_listener_unref(ln);
    return conn;
}

int xylem_dtls_read(xylem_dtls_conn_t* dtls, void* buf, int len) {
    if (dtls->listener) {
        return _dtls_server_recv(dtls, buf, len);
    }
    return _dtls_client_recv(dtls, buf, len);
}

int xylem_dtls_write(xylem_dtls_conn_t* dtls,
                     const void* data, int len) {
    if (dtls->listener) {
        return _dtls_server_send(dtls, data, len);
    }
    return _dtls_client_send(dtls, data, len);
}

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    if (dtls->listener) {
        _dtls_server_close_conn(dtls);
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
    xylem_channel_close(ln->accept_ch);
    _dtls_listener_unref(ln);
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

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

#include "tls.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"

#include "container/rbtree.h"
#include "net/addr.h"
#include "net/datagram.h"
#include "net/stream.h"
#include "net/tls/tls-backend.h"
#include "platform/platform-socket.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * TLS protocol invariant: the maximum *plaintext* a single record can
 * carry (2^14, mandated by every TLS version; OpenSSL spells it
 * SSL3_RT_MAX_PLAIN_LENGTH, mbedTLS MBEDTLS_SSL_IN_CONTENT_LEN -- the
 * value is the same backend-neutral constant, so no SSL header leaks
 * into the engine).
 *
 * The write path feeds the backend at most this many plaintext bytes per
 * step. Direct transport BIOs write ciphertext to the stream during the
 * backend call, so chunking keeps one record at a time in flight.
 */
#define TLS_MAX_PLAINTEXT (16 * 1024)

#define DTLS_DEFAULT_TIMEOUT_MS  30000
#define DTLS_INBOX_CAP           64
#define DTLS_DEFAULT_MTU         1500
#define DTLS_DGRAM_POOL_CAP      1024

typedef struct _dtls_dgram_s _dtls_dgram_t;

struct xylem_tls_ctx_s {
    tls_backend_ctx_t* be;
    bool               verify_server;
    bool               verify_client;
};

struct xylem_tls_conn_s {
    tls_backend_conn_t* be;
    xylem_mutex_t*      ssl_mu;
    xylem_mutex_t*      rd_mu;
    xylem_mutex_t*      wr_mu;
    xylem_mutex_t*      hs_mu;          /* elects one lazy-handshake driver */
    stream_t*           stream;
    tls_ctx_t*          ctx;
    char                alpn[256];
    _Atomic uint64_t    rd_deadline;
    _Atomic uint64_t    wr_deadline;
    _Atomic int         hs_state;       /* HS_DONE / HS_PENDING / HS_FAILED */
    _Atomic bool        closed;
};

struct xylem_tls_listener_s {
    listener_t*  listener;
    tls_ctx_t*   ctx;
    _Atomic bool closed;
};

struct xylem_dtls_conn_s {
    tls_backend_conn_t* be;
    addr_t              peer_addr;
    char                alpn[256];
    _Atomic bool        closed;
    _Atomic bool        closing;
    _Atomic int32_t     refcnt;
    bool                handshake_done;
    _dtls_dgram_t*      pending_dgram;
    datagram_t*         datagram;
    xylem_mutex_t*      ssl_mu;
    xylem_mutex_t*      rd_mu;
    xylem_mutex_t*      wr_mu;
    xylem_channel_t*    inbox;
    _Atomic int32_t     inbox_len;
    scheduler_timer_t*  handshake_timer;
    dtls_listener_t*    listener;
    rbtree_node_t       server_node;
    bool                in_sessions;
    uint64_t            rd_deadline_ms;
    uint64_t            wr_deadline_ms;
};

struct xylem_dtls_listener_s {
    datagram_t*        datagram;
    tls_ctx_t*         ctx;
    xylem_dtls_opts_t  opts;
    rbtree_t           sessions;
    xylem_mutex_t*     sessions_mu;
    xylem_mutex_t*     write_mu;
    xylem_mutex_t*     dgram_pool_mu;
    scheduler_t*       sched;
    _dtls_dgram_t*     dgram_pool;
    size_t             dgram_pool_len;
    size_t             dgram_bufsz;
    xylem_channel_t*   accept_ch;
    _Atomic bool       closed;
    _Atomic int32_t    refcnt;
};

/**
 * Effective datagram receive buffer size. The backend sizes DTLS records
 * to the link MTU set via dtls_backend_conn_set_mtu, so the dispatcher
 * buffer must be at least that large or an inbound record gets truncated.
 * A zero MTU keeps the historical 1500-byte default.
 */
static inline size_t _dtls_record_bufsz(uint16_t mtu) {
    return (mtu > DTLS_DEFAULT_MTU) ? (size_t)mtu
                                    : (size_t)DTLS_DEFAULT_MTU;
}

struct _dtls_dgram_s {
    struct _dtls_dgram_s* next;
    size_t                len;
    char                  data[];
};

/* Lazy server-handshake state; client connections are eagerly handshaked. */
typedef enum _tls_hs_state_e {
    HS_DONE    = 0,
    HS_PENDING = 1,
    HS_FAILED  = 2
} _tls_hs_state_t;

static void _tls_consume_io_budget(void) {
    if (runtime_consume_time()) {
        runtime_yield();
    }
}

static int _tls_stream_io_read(
    void* user,
    void* buf,
    int   len) {
    tls_conn_t* tls = (tls_conn_t*)user;
    int         n   = stream_read(tls->stream, buf, len);
    return n == STREAM_IO_AGAIN ? TLS_BACKEND_IO_AGAIN : n;
}

static int _tls_stream_io_write(
    void*       user,
    const void* buf,
    int         len) {
    tls_conn_t* tls = (tls_conn_t*)user;
    int         n   = stream_write(tls->stream, buf, len);
    return n == STREAM_IO_AGAIN ? TLS_BACKEND_IO_AGAIN : n;
}

static tls_conn_t* _tls_conn_create(stream_t* stream) {
    tls_conn_t* tls = (tls_conn_t*)calloc(1, sizeof(tls_conn_t));
    if (!tls) {
        return NULL;
    }

    atomic_init(&tls->rd_deadline, 0);
    atomic_init(&tls->wr_deadline, 0);
    atomic_init(&tls->hs_state, HS_DONE);
    atomic_init(&tls->closed, false);

    tls->stream = stream;
    tls->ssl_mu = xylem_mutex_create();
    tls->rd_mu  = xylem_mutex_create();
    tls->wr_mu  = xylem_mutex_create();
    tls->hs_mu  = xylem_mutex_create();
    if (!tls->ssl_mu || !tls->rd_mu || !tls->wr_mu || !tls->hs_mu) {
        xylem_mutex_destroy(tls->ssl_mu);
        xylem_mutex_destroy(tls->rd_mu);
        xylem_mutex_destroy(tls->wr_mu);
        xylem_mutex_destroy(tls->hs_mu);
        free(tls);
        return NULL;
    }

    return tls;
}

/**
 * Convert a timeout in milliseconds to an absolute deadline. Returns 0
 * (no deadline) when timeout_ms is 0, matching the stream convention.
 */
static uint64_t _tls_make_deadline(uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    return timeout_ms >= UINT64_MAX - now ? UINT64_MAX : now + timeout_ms;
}

/* Apply the same deadline to both stream directions (0 clears it). */
static void _tls_set_deadline(tls_conn_t* tls, uint64_t deadline) {
    atomic_store(&tls->rd_deadline, deadline);
    atomic_store(&tls->wr_deadline, deadline);
    stream_set_read_deadline(tls->stream, deadline);
    stream_set_write_deadline(tls->stream, deadline);
}

static bool _tls_deadline_expired(_Atomic uint64_t* deadline) {
    uint64_t d = atomic_load(deadline);
    return d > 0 && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= d;
}

/**
 * Release every resource except the backend connection, which the
 * callers tear down differently (graceful shutdown vs. plain free)
 * before delegating here. Frees tls itself.
 */
static void _tls_conn_free(tls_conn_t* tls) {
    if (tls->stream) {
        _tls_set_deadline(tls, 0);
        stream_destroy(tls->stream);
    }
    xylem_mutex_destroy(tls->ssl_mu);
    xylem_mutex_destroy(tls->rd_mu);
    xylem_mutex_destroy(tls->wr_mu);
    xylem_mutex_destroy(tls->hs_mu);
    free(tls);
}

static void _tls_conn_destroy(tls_conn_t* tls) {
    if (tls->be) {
        tls_backend_conn_destroy(tls->be);
    }
    _tls_conn_free(tls);
}

static int _tls_wait_write(tls_conn_t* tls) {
    xylem_mutex_lock(tls->wr_mu);
    int rc = stream_wait_write(tls->stream) == IOWAIT_READY ? 0 : -1;
    xylem_mutex_unlock(tls->wr_mu);
    return rc;
}

static int _tls_wait_read(tls_conn_t* tls) {
    xylem_mutex_lock(tls->rd_mu);
    int rc = stream_wait_read(tls->stream) == IOWAIT_READY ? 0 : -1;
    xylem_mutex_unlock(tls->rd_mu);
    return rc;
}

static int _tls_do_handshake(tls_conn_t* tls) {
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(tls->be);
        xylem_mutex_unlock(tls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return 0;
            case TLS_BACKEND_WANT_READ:
                if (_tls_wait_read(tls) != 0) {
                    return -1;
                }
                continue;
            case TLS_BACKEND_WANT_WRITE:
                if (_tls_wait_write(tls) != 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
}

static int _tls_configure_client(
    tls_ctx_t*                   ctx,
    const char*                  identity,
    const char*                  module,
    tls_backend_handshake_cfg_t* cfg) {
    cfg->verify =
        ctx->verify_server ? TLS_BACKEND_VERIFY_PEER : TLS_BACKEND_VERIFY_NONE;
    bool verify_peer = (cfg->verify != TLS_BACKEND_VERIFY_NONE);

    if (!identity && verify_peer) {
        xylem_loge(
            "<%s> peer identity unchecked verify=enabled risk=mitm",
            module);
    }
    if (!identity) {
        return 0;
    }

    size_t identity_len = strlen(identity);
    if (identity_len >= sizeof(cfg->identity)) {
        return -1;
    }
    memcpy(cfg->identity, identity, identity_len + 1);
    cfg->identity_type = TLS_BACKEND_IDENTITY_DNS;

    addr_t tmp;
    char*  zone = strchr(cfg->identity, '%');
    if (!zone || zone == cfg->identity || !zone[1]
        || !strchr(cfg->identity, ':')) {
        zone = NULL;
    }
    if (zone) {
        /* A scope selects an interface, not a certificate identity. */
        *zone = '\0';
    }
    if (addr_pton(cfg->identity, 0, &tmp) == 0) {
        cfg->identity_type = TLS_BACKEND_IDENTITY_IP;
        return 0;
    }
    if (zone) {
        *zone = '%';
    }
    if (identity_len > 1 && cfg->identity[identity_len - 1] == '.') {
        cfg->identity[identity_len - 1] = '\0';
    }
    return 0;
}

static void _tls_configure_server(
    tls_ctx_t*                   ctx,
    tls_backend_handshake_cfg_t* cfg) {
    cfg->verify = ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                     : TLS_BACKEND_VERIFY_NONE;
}

static void _tls_cache_alpn(
    tls_backend_conn_t* be,
    char*               alpn,
    size_t              alpn_len) {
    tls_backend_conn_get_alpn(be, alpn, alpn_len);
}

static int _tls_client_handshake(tls_conn_t* tls, const char* server_name) {
    tls_backend_io_t io = {
        .user  = tls,
        .read  = _tls_stream_io_read,
        .write = _tls_stream_io_write,
    };
    tls_backend_conn_t* be = tls_backend_conn_create(tls->ctx->be, false, &io);
    if (!be) {
        return -1;
    }
    tls->be = be;

    tls_backend_handshake_cfg_t cfg = {0};
    if (_tls_configure_client(tls->ctx, server_name, "tls", &cfg) != 0) {
        return -1;
    }
    if (tls_backend_conn_configure(tls->be, &cfg) != 0) {
        return -1;
    }

    if (_tls_do_handshake(tls) != 0) {
        return -1;
    }
    _tls_cache_alpn(tls->be, tls->alpn, sizeof(tls->alpn));
    return 0;
}

/**
 * Drive the backend read to completion. The backend owns stream ciphertext
 * I/O through its transport BIO; this loop only parks on WANT states.
 * Returns bytes read (>0), 0 on clean peer shutdown, or -1 on error/close.
 */
static int _tls_read_loop(tls_conn_t* tls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    for (;;) {
        if (_tls_deadline_expired(&tls->rd_deadline)) {
            return -1;
        }

        int n = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_read(tls->be, buf, len, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return n;
            case TLS_BACKEND_CLOSED:
                return 0;
            case TLS_BACKEND_WANT_READ:
                if (_tls_wait_read(tls) != 0) {
                    return -1;
                }
                continue;
            case TLS_BACKEND_WANT_WRITE:
                if (_tls_wait_write(tls) != 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
}

/**
 * Drive the backend write of the whole buffer to completion. The backend
 * writes ciphertext through its transport BIO during each accepted chunk.
 * Returns 0 once all len bytes are written, -1 on error/close.
 */
static int _tls_write_loop(
    tls_conn_t* tls,
    const void* data,
    int         len) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    const char* ptr = (const char*)data;
    int         rem = len;
    int         ret = -1;

    /**
     * One public write owns the TLS write side until its whole buffer is
     * accepted. Otherwise two coroutines can interleave plaintext chunks
     * into the same TLS record stream. WANT_WRITE waits below keep this
     * lock held, so that path waits on stream_wait_write() directly instead
     * of calling _tls_wait_write(), which would lock wr_mu again.
     */
    xylem_mutex_lock(tls->wr_mu);
    while (rem > 0) {
        if (_tls_deadline_expired(&tls->wr_deadline)) {
            break;
        }

        int chunk = rem < TLS_MAX_PLAINTEXT ? rem : TLS_MAX_PLAINTEXT;
        int n     = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_write(tls->be, ptr, chunk, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (st == TLS_BACKEND_OK) {
            _tls_consume_io_budget();
            ptr += n;
            rem -= n;
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            if (stream_wait_write(tls->stream) != IOWAIT_READY) {
                break;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_wait_read(tls) != 0) {
                break;
            }
            continue;
        }
        break;
    }
    if (rem == 0) {
        ret = 0;
    }

    xylem_mutex_unlock(tls->wr_mu);
    return ret;
}

/**
 * Drive the server-side TLS handshake on an accepted stream.
 * Unlike the eager path, this does NOT tear the connection down on
 * failure: it runs under a ref held by tls_read/tls_write/tls_handshake,
 * and the owning handler's tls_close performs teardown. Returns 0 on a
 * completed handshake, -1 on failure (bad cert, protocol mismatch, peer
 * disconnect, or handshake timeout).
 */
static int _tls_server_handshake(tls_conn_t* tls) {
    tls_backend_io_t io = {
        .user  = tls,
        .read  = _tls_stream_io_read,
        .write = _tls_stream_io_write,
    };
    tls_backend_conn_t* be = tls_backend_conn_create(tls->ctx->be, true, &io);
    if (!be) {
        xylem_loge("<tls> accept ssl init failed");
        return -1;
    }
    tls->be = be;
    tls_backend_handshake_cfg_t cfg = {0};
    _tls_configure_server(tls->ctx, &cfg);
    if (tls_backend_conn_configure(tls->be, &cfg) != 0) {
        return -1;
    }

    int rc = _tls_do_handshake(tls);

    if (rc == 0) {
        tls_backend_conn_get_alpn(tls->be, tls->alpn, sizeof(tls->alpn));
    }
    return rc;
}

/**
 * Ensure the (lazy server) handshake has completed before app I/O.
 *
 * Fast path: HS_DONE (every client conn, and any server conn already
 * handshaked) returns immediately without locking.
 *
 * Otherwise exactly one coroutine must drive the handshake: it is a
 * single state machine pumping BOTH stream directions, and stream parks allow
 * only one parker per direction, so two drivers would double-step the
 * backend and double-park a direction. hs_mu elects that driver; a
 * second coroutine (xylem permits one reader + one writer on a conn) just
 * blocks on the lock until the driver publishes the result, then reads
 * it. hs_mu is a coroutine mutex (a contended lock yields), and the
 * non-driver takes only hs_mu -- never nested inside ssl_mu/rd_mu/wr_mu
 * -- so there is no lock-order inversion against the driver's inner
 * locks. Holding hs_mu across the handshake's stream parks is therefore
 * fine. Returns 0 once handshaked, -1 on failure.
 */
static int _tls_ensure_handshake(tls_conn_t* tls) {
    /* Lock-free fast path: avoid locking once handshaked. */
    int st = atomic_load(&tls->hs_state);
    if (st == HS_DONE) {
        return 0;
    }
    if (st == HS_FAILED) {
        return -1;
    }

    /* Re-check under hs_mu: a concurrent driver may have finished meanwhile. */
    xylem_mutex_lock(tls->hs_mu);
    st = atomic_load(&tls->hs_state);
    if (st == HS_DONE) {
        xylem_mutex_unlock(tls->hs_mu);
        return 0;
    }
    if (st == HS_FAILED) {
        xylem_mutex_unlock(tls->hs_mu);
        return -1;
    }

    int rc = _tls_server_handshake(tls);
    atomic_store(&tls->hs_state, rc == 0 ? HS_DONE : HS_FAILED);
    xylem_mutex_unlock(tls->hs_mu);
    return rc;
}

/**
 * Best-effort close_notify so the peer can tell a clean close from a
 * truncation. Direct BIO shutdown may write immediately, but does not park:
 * a slow/stalled peer can never pin teardown.
 */
static void _tls_flush_close_notify(tls_conn_t* tls) {
    if (!tls->be) {
        return;
    }
    xylem_mutex_lock(tls->ssl_mu);
    tls_backend_conn_shutdown(tls->be);
    xylem_mutex_unlock(tls->ssl_mu);
}

static void _dtls_listener_ref(dtls_listener_t* ln) {
    atomic_fetch_add(&ln->refcnt, 1);
}

static void _dtls_listener_unref(dtls_listener_t* ln) {
    if (atomic_fetch_sub(&ln->refcnt, 1) != 1) {
        return;
    }
    datagram_destroy(ln->datagram);
    xylem_mutex_destroy(ln->sessions_mu);
    xylem_mutex_destroy(ln->write_mu);
    _dtls_dgram_t* dgram = ln->dgram_pool;
    while (dgram) {
        _dtls_dgram_t* next = dgram->next;
        free(dgram);
        dgram = next;
    }
    xylem_mutex_destroy(ln->dgram_pool_mu);
    if (ln->accept_ch) {
        xylem_channel_destroy(ln->accept_ch);
    }
    free(ln);
}

static void _dtls_conn_ref(dtls_conn_t* dtls) {
    atomic_fetch_add(&dtls->refcnt, 1);
}

static void _dtls_conn_unref(dtls_conn_t* dtls) {
    if (atomic_fetch_sub(&dtls->refcnt, 1) != 1) {
        return;
    }
    dtls_listener_t* ln = dtls->listener;
    if (dtls->be) {
        tls_backend_conn_destroy(dtls->be);
    }
    if (dtls->datagram) {
        datagram_set_read_deadline(dtls->datagram, 0);
        datagram_set_write_deadline(dtls->datagram, 0);
        datagram_destroy(dtls->datagram);
    }
    xylem_mutex_destroy(dtls->ssl_mu);
    xylem_mutex_destroy(dtls->rd_mu);
    xylem_mutex_destroy(dtls->wr_mu);
    free(dtls->pending_dgram);
    scheduler_timer_destroy(dtls->handshake_timer);
    if (dtls->inbox) {
        /**
         * Drain residual datagrams (freeing their payloads, which the
         * channel itself does not own) then destroy the channel. The
         * inbox was already closed by dtls_close, so recv never
         * parks here; it pops leftovers and returns NULL once empty.
         */
        _dtls_dgram_t* dgram;
        while ((dgram = (_dtls_dgram_t*)xylem_channel_recv(dtls->inbox)) !=
               NULL) {
            free(dgram);
        }
        xylem_channel_destroy(dtls->inbox);
    }
    free(dtls);
    if (ln) {
        _dtls_listener_unref(ln);
    }
}

static _dtls_dgram_t* _dtls_dgram_alloc(dtls_listener_t* ln) {
    _dtls_dgram_t* dgram = NULL;

    xylem_mutex_lock(ln->dgram_pool_mu);
    if (ln->dgram_pool) {
        dgram          = ln->dgram_pool;
        ln->dgram_pool = dgram->next;
        ln->dgram_pool_len--;
    }
    xylem_mutex_unlock(ln->dgram_pool_mu);

    if (!dgram) {
        dgram = (_dtls_dgram_t*)malloc(sizeof(_dtls_dgram_t) + ln->dgram_bufsz);
        if (!dgram) {
            return NULL;
        }
    }
    dgram->next = NULL;
    dgram->len  = 0;
    return dgram;
}

static void _dtls_dgram_release(dtls_listener_t* ln, _dtls_dgram_t* dgram) {
    if (!dgram) {
        return;
    }

    xylem_mutex_lock(ln->dgram_pool_mu);
    if (ln->dgram_pool_len < DTLS_DGRAM_POOL_CAP) {
        dgram->next    = ln->dgram_pool;
        ln->dgram_pool = dgram;
        ln->dgram_pool_len++;
        dgram = NULL;
    }
    xylem_mutex_unlock(ln->dgram_pool_mu);

    free(dgram);
}

/**
 * Copy a datagram into the session inbox channel. The receiver frees
 * it. Bounded by DTLS_INBOX_CAP via dtls->inbox_len so a slow or
 * absent reader cannot grow the channel without bound; on overflow
 * the datagram is dropped (DTLS tolerates loss, the peer retransmits).
 * The dispatcher holds sessions_mu across find+push, so the session
 * cannot be freed underneath this call.
 */
static void _dtls_inbox_push(dtls_conn_t* dtls, _dtls_dgram_t* dgram) {
    if (atomic_load(&dtls->inbox_len) >= (int32_t)DTLS_INBOX_CAP) {
        _dtls_dgram_release(dtls->listener, dgram);
        return;
    }
    atomic_fetch_add(&dtls->inbox_len, 1);
    if (xylem_channel_send(dtls->inbox, dgram) != 0) {
        atomic_fetch_sub(&dtls->inbox_len, 1);
        _dtls_dgram_release(dtls->listener, dgram);
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

static int _dtls_session_cmp_nn(
    const rbtree_node_t* a,
    const rbtree_node_t* b) {
    const dtls_conn_t* da = rbtree_entry(a, dtls_conn_t, server_node);
    const dtls_conn_t* db = rbtree_entry(b, dtls_conn_t, server_node);
    return _dtls_addr_cmp(&da->peer_addr, &db->peer_addr);
}

static int _dtls_session_cmp_kn(const void* key, const rbtree_node_t* node) {
    const addr_t*      addr = (const addr_t*)key;
    const dtls_conn_t* dtls = rbtree_entry(node, dtls_conn_t, server_node);
    return _dtls_addr_cmp(addr, &dtls->peer_addr);
}

static dtls_conn_t* _dtls_find_session(dtls_listener_t* ln, addr_t* addr) {
    rbtree_node_t* node = rbtree_find(&ln->sessions, addr);
    if (!node) {
        return NULL;
    }
    return rbtree_entry(node, dtls_conn_t, server_node);
}

static bool _dtls_server_unlink(dtls_conn_t* dtls) {
    dtls_listener_t* ln = dtls->listener;
    if (!ln) {
        return false;
    }

    bool unlinked = false;
    xylem_mutex_lock(ln->sessions_mu);
    if (dtls->in_sessions) {
        rbtree_remove(&ln->sessions, &dtls->server_node);
        dtls->in_sessions = false;
        unlinked          = true;
    }
    xylem_mutex_unlock(ln->sessions_mu);
    return unlinked;
}

static void _dtls_server_shutdown(dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closing, true)) {
        return;
    }
    atomic_store(&dtls->closed, true);
    if (dtls->handshake_timer) {
        scheduler_timer_stop(dtls->handshake_timer);
    }

    bool drop_session_ref = _dtls_server_unlink(dtls);

    /**
     * Close the inbox before dropping the session ref. A parked reader
     * holds its own conn ref and will release it after wakeup.
     */
    if (dtls->inbox) {
        xylem_channel_close(dtls->inbox);
    }

    if (drop_session_ref) {
        _dtls_conn_unref(dtls);
    }
}

static int _dtls_copy_dgram(
    dtls_conn_t*   dtls,
    _dtls_dgram_t* dgram,
    void*          buf,
    int            len) {
    if (!dgram) {
        return -1;
    }
    if (len <= 0) {
        _dtls_dgram_release(dtls->listener, dgram);
        return -1;
    }
    if (dgram->len > (size_t)len) {
        _dtls_dgram_release(dtls->listener, dgram);
        return -1;
    }
    int n = (int)dgram->len;
    memcpy(buf, dgram->data, dgram->len);
    _dtls_dgram_release(dtls->listener, dgram);
    return n;
}

static _dtls_dgram_t* _dtls_take_dgram(dtls_conn_t* dtls) {
    _dtls_dgram_t* dgram = dtls->pending_dgram;
    dtls->pending_dgram  = NULL;
    return dgram;
}

static int _dtls_server_io_read(void* user, void* buf, int len) {
    dtls_conn_t*   dtls  = (dtls_conn_t*)user;
    _dtls_dgram_t* dgram = _dtls_take_dgram(dtls);
    if (!dgram) {
        return TLS_BACKEND_IO_AGAIN;
    }
    return _dtls_copy_dgram(dtls, dgram, buf, len);
}

static int _dtls_server_io_write(void* user, const void* buf, int len) {
    dtls_conn_t*     dtls = (dtls_conn_t*)user;
    dtls_listener_t* ln   = dtls->listener;
    int n = datagram_send(ln->datagram, buf, len, &dtls->peer_addr);
    return n == DATAGRAM_IO_AGAIN ? TLS_BACKEND_IO_AGAIN : n;
}

static int _dtls_client_io_read(void* user, void* buf, int len) {
    dtls_conn_t* dtls = (dtls_conn_t*)user;
    int          n    = datagram_recv(dtls->datagram, buf, len, NULL);
    return n == DATAGRAM_IO_AGAIN ? TLS_BACKEND_IO_AGAIN : n;
}

static int _dtls_client_io_write(void* user, const void* buf, int len) {
    dtls_conn_t* dtls = (dtls_conn_t*)user;
    int          n    = datagram_send(dtls->datagram, buf, len, NULL);
    return n == DATAGRAM_IO_AGAIN ? TLS_BACKEND_IO_AGAIN : n;
}

/**
 * Sentinel returned by wait helpers when the read/write deadline fired
 * rather than a socket or channel error.
 */
#define DTLS_WAIT_TIMEOUT (-2)

static int _dtls_client_wait_read(dtls_conn_t* dtls) {
    int ret = -1;

    xylem_mutex_lock(dtls->rd_mu);
    iowait_result_t r = datagram_wait_read(dtls->datagram);
    if (r == IOWAIT_READY) {
        ret = 0;
    } else if (r == IOWAIT_TIMEOUT) {
        ret = DTLS_WAIT_TIMEOUT;
    }
    if (atomic_load(&dtls->closed)) {
        ret = -1;
    }
    xylem_mutex_unlock(dtls->rd_mu);
    return ret;
}

static int _dtls_client_wait_write(dtls_conn_t* dtls) {
    int ret = -1;

    xylem_mutex_lock(dtls->wr_mu);
    iowait_result_t r = datagram_wait_write(dtls->datagram);
    if (r == IOWAIT_READY) {
        ret = 0;
    } else if (r == IOWAIT_TIMEOUT) {
        ret = DTLS_WAIT_TIMEOUT;
    }
    if (atomic_load(&dtls->closed)) {
        ret = -1;
    }
    xylem_mutex_unlock(dtls->wr_mu);
    return ret;
}

static int _dtls_server_wait_read(dtls_conn_t* dtls, uint64_t timeout_ms) {
    if (dtls->pending_dgram) {
        return 0;
    }
    _dtls_dgram_t* dgram =
        (_dtls_dgram_t*)xylem_channel_recv_timeout(dtls->inbox, timeout_ms);
    if (!dgram) {
        return timeout_ms == (uint64_t)-1 ? -1 : DTLS_WAIT_TIMEOUT;
    }
    atomic_fetch_sub(&dtls->inbox_len, 1);
    dtls->pending_dgram = dgram;
    return 0;
}

static int _dtls_server_wait_write(dtls_conn_t* dtls) {
    int ret = -1;

    xylem_mutex_lock(dtls->listener->write_mu);
    iowait_result_t r = datagram_wait_write(dtls->listener->datagram);
    if (r == IOWAIT_READY) {
        ret = 0;
    } else if (r == IOWAIT_TIMEOUT) {
        ret = DTLS_WAIT_TIMEOUT;
    }
    xylem_mutex_unlock(dtls->listener->write_mu);
    return ret;
}

static int _dtls_server_send_record(
    dtls_conn_t* dtls,
    const void*  data,
    int          len) {
    for (;;) {
        int                 n = 0;
        tls_backend_state_t st =
            tls_backend_conn_write(dtls->be, data, len, &n);
        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return 0;
            case TLS_BACKEND_WANT_WRITE: {
                if (datagram_wait_write(dtls->listener->datagram) !=
                    IOWAIT_READY) {
                    return -1;
                }
                continue;
            }
            default:
                return -1;
        }
    }
}

/**
 * Drive the client handshake on the backend state machine. The
 * read wait is bounded by both the overall handshake deadline and the
 * DTLS retransmit timer (dtls_backend_conn_get_timeout); on the latter
 * expiring dtls_backend_conn_handle_timeout retransmits the last flight.
 */
static int _dtls_client_do_handshake(dtls_conn_t* dtls, uint64_t deadline) {
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        xylem_mutex_unlock(dtls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return 0;
            case TLS_BACKEND_WANT_READ: {
                uint64_t rd_dl = deadline;
                uint64_t to_ms;
                xylem_mutex_lock(dtls->ssl_mu);
                bool have_to = dtls_backend_conn_get_timeout(dtls->be, &to_ms);
                xylem_mutex_unlock(dtls->ssl_mu);
                if (have_to) {
                    uint64_t rt_dl =
                        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + to_ms;
                    if (rd_dl == 0 || rt_dl < rd_dl) {
                        rd_dl = rt_dl;
                    }
                }
                datagram_set_read_deadline(dtls->datagram, rd_dl);

                int rc = _dtls_client_wait_read(dtls);
                if (rc == DTLS_WAIT_TIMEOUT) {
                    uint64_t now =
                        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                    if (deadline > 0 && now >= deadline) {
                        return -1;
                    }
                    /**
                     * Retransmit timer fired, not the deadline: resend the
                     * last flight and wait again.
                     */
                    xylem_mutex_lock(dtls->ssl_mu);
                    dtls_backend_conn_handle_timeout(dtls->be);
                    xylem_mutex_unlock(dtls->ssl_mu);
                    continue;
                }
                if (rc != 0) {
                    return -1;
                }
                continue;
            }
            case TLS_BACKEND_WANT_WRITE:
                if (_dtls_client_wait_write(dtls) != 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
}

static int _dtls_client_recv_loop(dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    for (;;) {
        int n = 0;
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_read(dtls->be, buf, len, &n);
        xylem_mutex_unlock(dtls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return n;
            case TLS_BACKEND_CLOSED:
                return 0;
            case TLS_BACKEND_WANT_READ:
                if (_dtls_client_wait_read(dtls) != 0) {
                    return -1;
                }
                continue;
            case TLS_BACKEND_WANT_WRITE:
                if (_dtls_client_wait_write(dtls) != 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
}

static int _dtls_client_recv(dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    _dtls_conn_ref(dtls);
    int ret = -1;
    if (!atomic_load(&dtls->closed)) {
        ret = _dtls_client_recv_loop(dtls, buf, len);
    }
    _dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_client_send_loop(
    dtls_conn_t* dtls,
    const void*  data,
    int          len) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    for (;;) {
        int n = 0;
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st =
            tls_backend_conn_write(dtls->be, data, len, &n);
        xylem_mutex_unlock(dtls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return 0;
            case TLS_BACKEND_WANT_WRITE:
                if (_dtls_client_wait_write(dtls) != 0) {
                    return -1;
                }
                continue;
            case TLS_BACKEND_WANT_READ:
                if (_dtls_client_wait_read(dtls) != 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
}

static int _dtls_client_send(dtls_conn_t* dtls, const void* data, int len) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    _dtls_conn_ref(dtls);
    int ret = -1;
    if (!atomic_load(&dtls->closed)) {
        ret = _dtls_client_send_loop(dtls, data, len);
    }
    _dtls_conn_unref(dtls);
    return ret;
}

static void _dtls_client_close(dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }
    /**
     * Do not touch the backend object here: a concurrent recv/send may
     * be inside a backend read/write under ssl_mu. Flipping closed +
     * waking both readiness directions makes those calls return -1 and drop
     * their ref; the backend is destroyed once at the final unref, with
     * no parker left. (Unlike the TLS close path, which flushes a
     * best-effort close_notify, we skip it here: it is best-effort on a
     * datagram socket and not worth the backend access.)
     */
    datagram_close(dtls->datagram);
}

static void _dtls_handshake_coro(void* arg) {
    dtls_conn_t*     dtls = arg;
    dtls_listener_t* ln   = dtls->listener;

    _dtls_conn_ref(dtls);

    tls_backend_io_t io = {
        .user  = dtls,
        .read  = _dtls_server_io_read,
        .write = _dtls_server_io_write,
    };
    dtls->be = tls_backend_conn_create(ln->ctx->be, true, &io);
    if (!dtls->be) {
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        return;
    }
    {
        socklen_t salen = (dtls->peer_addr.storage.ss_family == AF_INET6)
                              ? (socklen_t)sizeof(struct sockaddr_in6)
                              : (socklen_t)sizeof(struct sockaddr_in);
        dtls_backend_conn_set_peer_addr(
            dtls->be,
            &dtls->peer_addr.storage,
            salen);
    }
    dtls_backend_conn_set_mtu(dtls->be, ln->opts.mtu);

    tls_backend_handshake_cfg_t cfg = {0};
    _tls_configure_server(ln->ctx, &cfg);
    if (tls_backend_conn_configure(dtls->be, &cfg) != 0) {
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        return;
    }

    uint64_t hs_timeout = ln->opts.handshake_timeout_ms > 0
                              ? ln->opts.handshake_timeout_ms
                              : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t hs_deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + hs_timeout;

    bool success = false;
    while (!dtls->handshake_done) {
        /**
         * Bound the channel wait by the nearer of two deadlines: the
         * overall handshake deadline and the DTLS retransmit timeout.
         * The retransmit timer is driven inline here (not via an
         * external sched_timer) so the handshake coroutine stays the
         * sole owner of dtls->be -- the server session has no ssl_mu,
         * and a cross-thread timer callback touching the same backend
         * SSL object would be a data race. On a recv timeout we
         * distinguish the two: the handshake deadline fails the
         * handshake, the retransmit timeout resends the last flight and
         * waits again.
         */
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (hs_deadline > 0 && now >= hs_deadline) {
            break;
        }
        uint64_t wait_ms = hs_deadline - now;

        uint64_t rt_ms;
        bool     have_rt = dtls_backend_conn_get_timeout(dtls->be, &rt_ms);
        if (have_rt && rt_ms < wait_ms) {
            wait_ms = rt_ms;
        }

        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        if (st == TLS_BACKEND_OK) {
            _tls_consume_io_budget();
            dtls->handshake_done = true;
            success              = true;
            break;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            if (_dtls_server_wait_write(dtls) != 0) {
                break;
            }
            continue;
        }
        if (st != TLS_BACKEND_WANT_READ) {
            break;
        }

        int wait_rc = _dtls_server_wait_read(dtls, wait_ms);
        if (wait_rc != 0) {
            /**
             * recv timed out. If the overall handshake deadline has
             * passed, give up; otherwise it was the retransmit timer --
             * resend the last flight and wait again.
             */
            now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (hs_deadline > 0 && now >= hs_deadline) {
                break;
            }
            if (have_rt) {
                dtls_backend_conn_handle_timeout(dtls->be);
            }
            continue;
        }
    }

    scheduler_timer_destroy(dtls->handshake_timer);
    dtls->handshake_timer = NULL;

    if (!success) {
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        return;
    }

    _tls_cache_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
    xylem_mutex_lock(ln->sessions_mu);
    if (atomic_load(&ln->closed)) {
        xylem_mutex_unlock(ln->sessions_mu);
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        return;
    }
    _dtls_conn_ref(dtls);
    if (xylem_channel_send(ln->accept_ch, dtls) != 0) {
        _dtls_conn_unref(dtls);
    }
    xylem_mutex_unlock(ln->sessions_mu);
    _dtls_conn_unref(dtls);
}

static void _dtls_dispatcher(void* arg) {
    dtls_listener_t* ln = arg;

    while (!atomic_load(&ln->closed)) {
        _dtls_dgram_t* dgram = _dtls_dgram_alloc(ln);
        if (!dgram) {
            xylem_loge(
                "<dtls> dispatcher dgram alloc failed size=%zu",
                ln->dgram_bufsz);
            break;
        }

        addr_t from_addr;
        int n;
        for (;;) {
            n = datagram_recv(
                ln->datagram,
                dgram->data,
                (int)ln->dgram_bufsz,
                &from_addr);
            if (n >= 0) {
                _tls_consume_io_budget();
                break;
            }
            if (n != DATAGRAM_IO_AGAIN
                || datagram_wait_read(ln->datagram) != IOWAIT_READY) {
                break;
            }
        }

        if (n < 0) {
            _dtls_dgram_release(ln, dgram);
            break;
        }
        dgram->len = (size_t)n;

        xylem_mutex_lock(ln->sessions_mu);
        dtls_conn_t* dtls = _dtls_find_session(ln, &from_addr);
        if (dtls) {
            /**
             * Push under the lock so a concurrent dtls_close
             * cannot remove + free the session between the lookup and
             * the push. _dtls_inbox_push only enqueues (it never
             * parks), so the critical section stays short.
             */
            _dtls_inbox_push(dtls, dgram);
            xylem_mutex_unlock(ln->sessions_mu);
            continue;
        }
        xylem_mutex_unlock(ln->sessions_mu);

        dtls = (dtls_conn_t*)calloc(1, sizeof(dtls_conn_t));
        if (!dtls) {
            _dtls_dgram_release(ln, dgram);
            continue;
        }
        atomic_store(&dtls->refcnt, 1);
        dtls->peer_addr       = from_addr;
        dtls->inbox           = xylem_channel_create();
        dtls->handshake_timer = scheduler_timer_create(ln->sched);

        if (!dtls->inbox || !dtls->handshake_timer) {
            scheduler_timer_destroy(dtls->handshake_timer);
            if (dtls->inbox) {
                xylem_channel_destroy(dtls->inbox);
            }
            free(dtls);
            _dtls_dgram_release(ln, dgram);
            continue;
        }

        _dtls_listener_ref(ln);
        dtls->listener = ln;
        _dtls_inbox_push(dtls, dgram);

        xylem_mutex_lock(ln->sessions_mu);
        dtls->in_sessions = true;
        rbtree_insert(&ln->sessions, &dtls->server_node);
        xylem_mutex_unlock(ln->sessions_mu);

        if (runtime_spawn(_dtls_handshake_coro, dtls) != 0) {
            _dtls_server_shutdown(dtls);
            _dtls_conn_unref(dtls);
        }
    }

    _dtls_listener_unref(ln);
}

static int _dtls_server_recv_loop(dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    for (;;) {
        int                 n  = 0;
        tls_backend_state_t st = tls_backend_conn_read(dtls->be, buf, len, &n);
        switch (st) {
            case TLS_BACKEND_OK:
                _tls_consume_io_budget();
                return n;
            case TLS_BACKEND_CLOSED:
                return 0;
            case TLS_BACKEND_WANT_READ: {
                uint64_t wait_ms = (uint64_t)-1;
                if (dtls->rd_deadline_ms > 0) {
                    uint64_t now =
                        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                    if (now >= dtls->rd_deadline_ms) {
                        return -1;
                    }
                    wait_ms = dtls->rd_deadline_ms - now;
                }
                if (_dtls_server_wait_read(dtls, wait_ms) != 0) {
                    return -1;
                }
                continue;
            }
            default:
                return -1;
        }
    }
}

static int _dtls_server_recv(dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    _dtls_conn_ref(dtls);
    int ret = -1;
    if (!atomic_load(&dtls->closed)) {
        ret = _dtls_server_recv_loop(dtls, buf, len);
    }
    _dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_server_send(dtls_conn_t* dtls, const void* data, int len) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    _dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load(&dtls->closed)) {
        xylem_mutex_lock(dtls->listener->write_mu);
        if (dtls->wr_deadline_ms > 0) {
            datagram_set_write_deadline(
                dtls->listener->datagram,
                dtls->wr_deadline_ms);
        }
        ret = _dtls_server_send_record(dtls, data, len);
        if (dtls->wr_deadline_ms > 0) {
            datagram_set_write_deadline(dtls->listener->datagram, 0);
        }
        xylem_mutex_unlock(dtls->listener->write_mu);
    }

    _dtls_conn_unref(dtls);
    return ret;
}

static void _dtls_server_close(dtls_conn_t* dtls) {
    /**
     * No close_notify: it is best-effort on a datagram transport and the
     * server session has no ssl_mu, so touching dtls->be here would race
     * a concurrent reader/writer still in the backend. DTLS records are
     * independently authenticated with explicit boundaries, so there is
     * no stream-truncation concern to guard against -- just drop it,
     * matching the client close path.
     */
    _dtls_server_shutdown(dtls);
}

static tls_ctx_t* _tls_ctx_create(tls_backend_proto_t proto) {
    tls_ctx_t* ctx = (tls_ctx_t*)calloc(1, sizeof(tls_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->be = tls_backend_ctx_create(proto);
    if (!ctx->be) {
        free(ctx);
        return NULL;
    }
    ctx->verify_server = true;
    ctx->verify_client = false;
    return ctx;
}

tls_ctx_t* tls_ctx_create(void) {
    return _tls_ctx_create(TLS_BACKEND_PROTO_TLS);
}

tls_ctx_t* dtls_ctx_create(void) {
    return _tls_ctx_create(TLS_BACKEND_PROTO_DTLS);
}

void tls_ctx_destroy(tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    tls_backend_ctx_destroy(ctx->be);
    free(ctx);
}

int tls_ctx_set_keylog(tls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    return tls_backend_ctx_set_keylog(ctx->be, path);
}

int tls_ctx_load_cert(
    tls_ctx_t*  ctx,
    const char* hostname,
    const char* cert,
    const char* key) {
    return tls_backend_ctx_load_cert_file(ctx->be, hostname, cert, key);
}

int tls_ctx_load_cert_mem(
    tls_ctx_t*  ctx,
    const char* hostname,
    const void* cert_pem,
    size_t      cert_len,
    const void* key_pem,
    size_t      key_len) {
    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return -1;
    }
    return tls_backend_ctx_load_cert_mem(ctx->be, hostname, cert_pem, cert_len,
                                         key_pem, key_len);
}

int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file) {
    return tls_backend_ctx_load_ca_file(ctx->be, ca_file);
}

int tls_ctx_load_system_ca(tls_ctx_t* ctx, const char* fallback_ca_file) {
    return tls_backend_ctx_load_system_ca(ctx->be, fallback_ca_file);
}

void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable) {
    ctx->verify_server = enable;
}

void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable) {
    ctx->verify_client = enable;
}

int tls_ctx_set_alpn(tls_ctx_t* ctx, const char** protocols, size_t count) {
    return tls_backend_ctx_set_alpn(ctx->be, protocols, count);
}

tls_conn_t* tls_dial(
    const char*       host,
    uint16_t          port,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts) {
    uint64_t timeout_ms = opts ? opts->connect_timeout_ms : 0;
    uint64_t deadline   = _tls_make_deadline(timeout_ms);
    stream_t* stream
        = stream_dial(host, port, timeout_ms, opts && opts->enable_mss_clamp);
    if (!stream) {
        return NULL;
    }

    tls_conn_t* tls = _tls_conn_create(stream);
    if (!tls) {
        stream_destroy(stream);
        return NULL;
    }

    tls->ctx = ctx;

    /* The same absolute deadline bounds connect plus handshake. */
    _tls_set_deadline(tls, deadline);
    const char* server_name = (opts && opts->server_name)
                                  ? opts->server_name
                                  : host;
    if (_tls_client_handshake(tls, server_name) != 0) {
        xylem_loge("<tls> dial handshake failed host=%s port=%u", host, port);
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    return tls;
}

void tls_close(tls_conn_t* tls) {
    if (atomic_exchange(&tls->closed, true)) {
        return;
    }

    /**
     * An incomplete handshake cannot send close_notify safely; hard-close
     * the stream to interrupt it without racing backend setup. After the
     * handshake, close_notify needs exclusive ownership of wr_mu. Acquire
     * it with trylock because a writer may hold it while parked, waiting
     * for the stream interrupt below. This mirrors Go's tls.Conn.Close,
     * which skips close_notify before handshake completion or while a
     * Write is in flight.
     */
    if (atomic_load(&tls->hs_state) == HS_DONE
        && xylem_mutex_trylock(tls->wr_mu)) {
        _tls_flush_close_notify(tls);
        xylem_mutex_unlock(tls->wr_mu);
    }

    stream_close(tls->stream);
}

void tls_destroy(tls_conn_t* tls) {
    if (!tls) {
        return;
    }
    tls_close(tls);
    _tls_conn_destroy(tls);
}

tls_listener_t* tls_listen(
    const char*       host,
    uint16_t          port,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts) {
    listener_t* listener
        = listener_listen(host, port, opts && opts->enable_mss_clamp);
    if (!listener) {
        return NULL;
    }

    tls_listener_t* ln = (tls_listener_t*)calloc(1, sizeof(tls_listener_t));
    if (!ln) {
        listener_destroy(listener);
        return NULL;
    }

    atomic_init(&ln->closed, false);

    ln->listener = listener;
    ln->ctx      = ctx;

    return ln;
}

tls_conn_t* tls_accept(tls_listener_t* ln) {
    for (;;) {
        if (atomic_load(&ln->closed)) {
            break;
        }

        stream_t* stream = listener_accept(ln->listener);
        if (!stream) {
            break;
        }

        tls_conn_t* conn = _tls_conn_create(stream);
        if (!conn) {
            stream_destroy(stream);
            break;
        }

        conn->ctx = ln->ctx;

        /**
         * Defer the handshake to first I/O so it runs in the handler
         * coroutine and parallelizes, instead of serializing every
         * client's multi-round-trip handshake behind this acceptor.
         */
        atomic_store(&conn->hs_state, HS_PENDING);

        return conn;
    }

    return NULL;
}

void tls_close_listener(tls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    listener_close(ln->listener);
}

void tls_destroy_listener(tls_listener_t* ln) {
    if (!ln) {
        return;
    }
    tls_close_listener(ln);
    listener_destroy(ln->listener);
    free(ln);
}

int tls_read(tls_conn_t* tls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    int ret = -1;
    if (!atomic_load(&tls->closed)
        && _tls_ensure_handshake(tls) == 0) {
        ret = _tls_read_loop(tls, buf, len);
    }
    return ret;
}

int tls_write(tls_conn_t* tls, const void* data, int len) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    int ret = -1;
    if (!atomic_load(&tls->closed)
        && _tls_ensure_handshake(tls) == 0) {
        ret = _tls_write_loop(tls, data, len);
    }
    return ret;
}

int tls_handshake(tls_conn_t* tls) {
    int ret = -1;
    if (!atomic_load(&tls->closed)) {
        ret = _tls_ensure_handshake(tls);
    }
    return ret;
}

void tls_set_read_deadline(tls_conn_t* tls, uint64_t deadline_ms) {
    if (!atomic_load(&tls->closed)) {
        atomic_store(&tls->rd_deadline, deadline_ms);
        stream_set_read_deadline(tls->stream, deadline_ms);
    }
}

void tls_set_write_deadline(tls_conn_t* tls, uint64_t deadline_ms) {
    if (!atomic_load(&tls->closed)) {
        atomic_store(&tls->wr_deadline, deadline_ms);
        stream_set_write_deadline(tls->stream, deadline_ms);
    }
}

int tls_remote_addr(
    tls_conn_t* tls,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    int ret = -1;
    if (!atomic_load(&tls->closed)) {
        ret = stream_remote_addr(tls->stream, host, host_len, port);
    }
    return ret;
}

int tls_local_addr(
    tls_conn_t* tls,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    int ret = -1;
    if (!atomic_load(&tls->closed)) {
        ret = stream_local_addr(tls->stream, host, host_len, port);
    }
    return ret;
}

int tls_listener_addr(
    tls_listener_t* ln,
    char*           host,
    size_t          host_len,
    uint16_t*       port) {
    int ret = -1;
    if (!atomic_load(&ln->closed)) {
        ret = listener_addr(ln->listener, host, host_len, port);
    }
    return ret;
}

const char* tls_get_alpn(tls_conn_t* tls) {
    if (atomic_load(&tls->closed)
        || atomic_load(&tls->hs_state) != HS_DONE) {
        return NULL;
    }
    return tls->alpn[0] ? tls->alpn : NULL;
}

tls_conn_t* tls_client_handshake_fd(
    platform_sock_t   fd,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts) {
    stream_t* stream = stream_from_fd(fd);
    if (!stream) {
        platform_socket_close(fd);
        return NULL;
    }

    tls_conn_t* tls = _tls_conn_create(stream);
    if (!tls) {
        stream_destroy(stream);
        return NULL;
    }
    tls->ctx = ctx;

    /* Arm the handshake deadline; disarm on success. */
    _tls_set_deadline(tls,
                      _tls_make_deadline(opts ? opts->connect_timeout_ms
                                              : 0));

    if (_tls_client_handshake(tls, opts ? opts->server_name : NULL) != 0) {
        xylem_loge("<tls> client handshake failed");
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    return tls;
}

dtls_conn_t* dtls_dial(
    const char*        host,
    uint16_t           port,
    tls_ctx_t*         ctx,
    xylem_dtls_opts_t* opts) {
    datagram_t* datagram = datagram_dial(host, port);
    if (!datagram) {
        xylem_loge("<dtls> dial failed host=%s port=%u", host, port);
        return NULL;
    }

    dtls_conn_t* dtls = (dtls_conn_t*)calloc(1, sizeof(dtls_conn_t));
    if (!dtls) {
        datagram_destroy(datagram);
        return NULL;
    }

    atomic_store(&dtls->refcnt, 1);
    dtls->datagram = datagram;

    dtls->ssl_mu = xylem_mutex_create();
    dtls->rd_mu  = xylem_mutex_create();
    dtls->wr_mu  = xylem_mutex_create();
    if (!dtls->ssl_mu || !dtls->rd_mu || !dtls->wr_mu) {
        _dtls_conn_unref(dtls);
        return NULL;
    }

    uint64_t timeout  = (opts && opts->handshake_timeout_ms > 0)
                            ? opts->handshake_timeout_ms
                            : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;
    datagram_set_read_deadline(dtls->datagram, deadline);
    datagram_set_write_deadline(dtls->datagram, deadline);

    tls_backend_io_t io = {
        .user  = dtls,
        .read  = _dtls_client_io_read,
        .write = _dtls_client_io_write,
    };
    dtls->be = tls_backend_conn_create(ctx->be, false, &io);
    if (!dtls->be) {
        _dtls_conn_unref(dtls);
        return NULL;
    }
    dtls_backend_conn_set_mtu(dtls->be, opts ? opts->mtu : 0);

    tls_backend_handshake_cfg_t cfg         = {0};
    const char*                 server_name = opts ? opts->server_name : NULL;
    if (_tls_configure_client(ctx, server_name, "dtls", &cfg) != 0) {
        _dtls_conn_unref(dtls);
        return NULL;
    }
    if (tls_backend_conn_configure(dtls->be, &cfg) != 0) {
        _dtls_conn_unref(dtls);
        return NULL;
    }

    if (_dtls_client_do_handshake(dtls, deadline) != 0) {
        _dtls_conn_unref(dtls);
        return NULL;
    }

    datagram_set_read_deadline(dtls->datagram, 0);
    datagram_set_write_deadline(dtls->datagram, 0);

    _tls_cache_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
    return dtls;
}

dtls_listener_t* dtls_listen(
    const char*        host,
    uint16_t           port,
    tls_ctx_t*         ctx,
    xylem_dtls_opts_t* opts) {
    datagram_t* datagram = datagram_listen(host, port);
    if (!datagram) {
        xylem_loge("<dtls> listen failed host=%s port=%u", host, port);
        return NULL;
    }

    dtls_listener_t* ln = (dtls_listener_t*)calloc(1, sizeof(dtls_listener_t));
    if (!ln) {
        datagram_destroy(datagram);
        return NULL;
    }

    ln->datagram = datagram;
    ln->ctx      = ctx;
    ln->sched    = runtime_get_scheduler();
    if (opts) {
        ln->opts = *opts;
    }

    ln->dgram_bufsz = _dtls_record_bufsz(ln->opts.mtu);

    _dtls_listener_ref(ln); /* caller's reference */

    rbtree_init(&ln->sessions, _dtls_session_cmp_nn, _dtls_session_cmp_kn);
    ln->sessions_mu   = xylem_mutex_create();
    ln->write_mu      = xylem_mutex_create();
    ln->dgram_pool_mu = xylem_mutex_create();
    if (!ln->sessions_mu || !ln->write_mu || !ln->dgram_pool_mu) {
        _dtls_listener_unref(ln);
        return NULL;
    }

    ln->accept_ch = xylem_channel_create();
    if (!ln->accept_ch) {
        _dtls_listener_unref(ln);
        return NULL;
    }

    _dtls_listener_ref(ln); /* dispatcher's reference (released on exit) */
    if (runtime_spawn(_dtls_dispatcher, ln) != 0) {
        _dtls_listener_unref(ln);
        _dtls_listener_unref(ln);
        return NULL;
    }
    return ln;
}

dtls_conn_t* dtls_accept(dtls_listener_t* ln) {
    _dtls_listener_ref(ln);
    dtls_conn_t* conn = (dtls_conn_t*)xylem_channel_recv(ln->accept_ch);
    _dtls_listener_unref(ln);
    return conn;
}

int dtls_read(dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    if (dtls->listener) {
        return _dtls_server_recv(dtls, buf, len);
    }
    return _dtls_client_recv(dtls, buf, len);
}

int dtls_write(dtls_conn_t* dtls, const void* data, int len) {
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    if (dtls->listener) {
        return _dtls_server_send(dtls, data, len);
    }
    return _dtls_client_send(dtls, data, len);
}

void dtls_close(dtls_conn_t* dtls) {
    if (dtls->listener) {
        _dtls_server_close(dtls);
    } else {
        _dtls_client_close(dtls);
    }
}

void dtls_destroy(dtls_conn_t* dtls) {
    if (!dtls) {
        return;
    }
    dtls_close(dtls);
    _dtls_conn_unref(dtls);
}

void dtls_close_listener(dtls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    xylem_mutex_lock(ln->sessions_mu);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        dtls_conn_t*   dtls = rbtree_entry(node, dtls_conn_t, server_node);
        _dtls_conn_ref(dtls);
        xylem_mutex_unlock(ln->sessions_mu);
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        xylem_mutex_lock(ln->sessions_mu);
    }
    xylem_mutex_unlock(ln->sessions_mu);

    datagram_close(ln->datagram);
    xylem_channel_close(ln->accept_ch);
}

void dtls_destroy_listener(dtls_listener_t* ln) {
    if (!ln) {
        return;
    }
    dtls_close_listener(ln);
    _dtls_listener_unref(ln);
}

void dtls_set_read_deadline(dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->listener) {
        dtls->rd_deadline_ms = deadline_ms;
    } else {
        datagram_set_read_deadline(dtls->datagram, deadline_ms);
    }
}

void dtls_set_write_deadline(dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->listener) {
        dtls->wr_deadline_ms = deadline_ms;
    } else {
        datagram_set_write_deadline(dtls->datagram, deadline_ms);
    }
}

const char* dtls_get_alpn(dtls_conn_t* dtls) {
    return dtls->alpn[0] ? dtls->alpn : NULL;
}

int dtls_remote_addr(
    dtls_conn_t* dtls,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    if (!dtls->listener) {
        return datagram_remote_addr(dtls->datagram, host, host_len, port);
    }
    return addr_ntop(&dtls->peer_addr, host, host_len, port);
}

int dtls_local_addr(
    dtls_conn_t* dtls,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    if (dtls->listener) {
        return datagram_local_addr(
            dtls->listener->datagram,
            host,
            host_len,
            port);
    }
    return datagram_local_addr(dtls->datagram, host, host_len, port);
}

int dtls_listener_addr(
    dtls_listener_t* ln,
    char*            host,
    size_t           host_len,
    uint16_t*        port) {
    return datagram_local_addr(ln->datagram, host, host_len, port);
}

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

#include "tls-stream.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/sync/xylem-mutex.h"

#include "net/stream.h"
#include "net/tls/tls-backend.h"
#include "net/tls/tls-context.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define TLS_MAX_PLAINTEXT (16 * 1024)

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
    tls_backend_conn_t* be = tls_backend_conn_create(tls_ctx_get_backend(tls->ctx), false, &io);
    if (!be) {
        return -1;
    }
    tls->be = be;

    tls_backend_handshake_cfg_t cfg = {0};
    if (tls_ctx_build_client_config(tls->ctx, server_name, "tls", &cfg) != 0) {
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
    tls_backend_conn_t* be = tls_backend_conn_create(tls_ctx_get_backend(tls->ctx), true, &io);
    if (!be) {
        xylem_loge("<tls> accept ssl init failed");
        return -1;
    }
    tls->be = be;
    tls_backend_handshake_cfg_t cfg = {0};
    tls_ctx_build_server_config(tls->ctx, &cfg);
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

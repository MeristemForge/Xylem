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
#include "xylem/sync/xylem-mutex.h"

#include "net/addr.h"
#include "net/tcp/stream.h"
#include "net/tls/tls-backend.h"
#include "platform/platform-socket.h"
#include "runtime/precond.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * TLS protocol invariant: the maximum ciphertext record on the wire, and
 * the size of the per-connection ciphertext scratch buffers (rbuf, wbuf).
 *
 * 16 KiB plaintext (2^14, mandated by every TLS version) plus framing and
 * AEAD overhead. RFC 5246 6.2.3 caps TLS 1.2 ciphertext at 2^14 + 2048;
 * TLS 1.3's 2^14 + 256 fits under it, so this bounds one record for any
 * version. Backend-neutral: every conforming backend (OpenSSL, mbedTLS,
 * ...) is held to the same limit, so no SSL-library header leaks into the
 * engine.
 *
 * Both scratch buffers are exactly one record: a full record fits per pump,
 * so it never splits across two sends (sizing to the 16 KiB plaintext cap
 * left the overhead tail for a second tiny send and doubled write syscalls).
 * rbuf recvs one record's worth of ciphertext at a time -- the buffer holds
 * no state between pumps (feed copies it straight into the backend), so a
 * fixed one-record size keeps per-connection memory bounded regardless of
 * connection count, the way Go's crypto/tls caps rawInput at a single record.
 */
#define TLS_MAX_RECORD (16 * 1024 + 2048)

/**
 * TLS protocol invariant: the maximum *plaintext* a single record can
 * carry (2^14, mandated by every TLS version; OpenSSL spells it
 * SSL3_RT_MAX_PLAIN_LENGTH, mbedTLS MBEDTLS_SSL_IN_CONTENT_LEN -- the
 * value is the same backend-neutral constant, so no SSL header leaks
 * into the engine).
 *
 * The write path feeds the backend at most this many plaintext bytes per
 * step. A larger buffer (e.g. a 64 KiB app write) would make the backend
 * encrypt every record up front and buffer the whole message's
 * ciphertext in its scratch BIO before _tls_pump_out drains the first
 * record -- memory scaling with the message size times the connection
 * count. Chunking to one record bounds that to a single record of
 * in-flight ciphertext and pipelines encrypt-then-send.
 */
#define TLS_MAX_PLAINTEXT (16 * 1024)

/**
 * Bound for the best-effort close_notify flush. Long enough to ride out
 * a transiently full send buffer (e.g. close right after a large write),
 * short enough that a slow/stalled/malicious peer can never pin the
 * connection's teardown.
 */
#define TLS_CLOSE_NOTIFY_TIMEOUT_MS 1000

/* Lazy server-handshake state; calloc 0-value HS_DONE = no handshake needed. */
typedef enum _tls_hs_state_e {
    HS_DONE    = 0,
    HS_PENDING = 1,
    HS_FAILED  = 2
} _tls_hs_state_t;

static void _tls_conn_ref(tls_conn_t* tls) {
    atomic_fetch_add_explicit(&tls->refcnt, 1, memory_order_relaxed);
}

static tls_conn_t* _tls_conn_create(stream_t* stream) {
    tls_conn_t* tls = (tls_conn_t*)calloc(1, sizeof(tls_conn_t));
    if (!tls) {
        return NULL;
    }

    tls->stream = stream;
    tls->ssl_mu = xylem_mutex_create();
    tls->rd_mu  = xylem_mutex_create();
    tls->wr_mu  = xylem_mutex_create();
    tls->hs_mu  = xylem_mutex_create();
    tls->rbuf   = (char*)malloc(TLS_MAX_RECORD);
    tls->wbuf   = (char*)malloc(TLS_MAX_RECORD);
    if (!tls->ssl_mu || !tls->rd_mu || !tls->wr_mu || !tls->hs_mu
        || !tls->rbuf || !tls->wbuf) {
        xylem_mutex_destroy(tls->ssl_mu);
        xylem_mutex_destroy(tls->rd_mu);
        xylem_mutex_destroy(tls->wr_mu);
        xylem_mutex_destroy(tls->hs_mu);
        free(tls->rbuf);
        free(tls->wbuf);
        free(tls);
        return NULL;
    }

    _tls_conn_ref(tls);
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
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
}

/* Apply the same deadline to both stream directions (0 clears it). */
static void _tls_set_deadline(tls_conn_t* tls, uint64_t deadline) {
    stream_set_read_deadline(tls->stream, deadline);
    stream_set_write_deadline(tls->stream, deadline);
}

/**
 * Release every resource except the backend connection, which the
 * callers tear down differently (graceful shutdown vs. plain free)
 * before delegating here. Frees tls itself.
 */
static void _tls_conn_free(tls_conn_t* tls) {
    if (tls->stream) {
        _tls_set_deadline(tls, 0);
        stream_release(tls->stream);
    }
    xylem_mutex_destroy(tls->ssl_mu);
    xylem_mutex_destroy(tls->rd_mu);
    xylem_mutex_destroy(tls->wr_mu);
    xylem_mutex_destroy(tls->hs_mu);
    free(tls->rbuf);
    free(tls->wbuf);
    free(tls);
}

static void _tls_conn_destroy(tls_conn_t* tls) {
    if (tls->be) {
        tls_backend_conn_destroy(tls->be);
    }
    _tls_conn_free(tls);
}

static void _tls_conn_unref(tls_conn_t* tls) {
    /* Graceful close_notify lives in tls_close; refcount drop only destroys. */
    if (atomic_fetch_sub_explicit(&tls->refcnt, 1, memory_order_acq_rel)
        == 1) {
        _tls_conn_destroy(tls);
    }
}

/**
 * Send exactly len bytes to the stream. Caller must hold wr_mu so this
 * is the sole TLS writer. Returns 0 on success, -1 on error or close.
 */
static int _tls_send_all(tls_conn_t* tls, const char* buf, int len) {
    if (atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        return -1;
    }
    return stream_write(tls->stream, buf, len);
}

/**
 * Drain pending outbound ciphertext from the backend to the stream.
 * Holds wr_mu so it is the sole TLS writer, and takes ssl_mu only for
 * the drain itself -- never across a stream park -- so a concurrent
 * reader can still touch the backend state. Returns 0 once the backend
 * is empty, -1 on stream error or close.
 */
static int _tls_pump_out(tls_conn_t* tls) {
    int ret = 0;

    xylem_mutex_lock(tls->wr_mu);
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        int n = tls_backend_conn_drain(tls->be, tls->wbuf, TLS_MAX_RECORD);
        xylem_mutex_unlock(tls->ssl_mu);
        if (n <= 0) {
            break;
        }
        if (_tls_send_all(tls, tls->wbuf, n) != 0) {
            ret = -1;
            break;
        }
    }
    xylem_mutex_unlock(tls->wr_mu);
    return ret;
}

/**
 * Read one chunk of inbound ciphertext from the stream into the backend.
 * Holds rd_mu so it is the sole TLS reader, and takes ssl_mu only for
 * the feed -- never across a stream park. Returns the byte count fed
 * (>0), 0 on peer EOF, -1 on stream error or close.
 */
static int _tls_pump_in(tls_conn_t* tls) {
    int ret = -1;

    xylem_mutex_lock(tls->rd_mu);
    int n = stream_read(tls->stream, tls->rbuf, TLS_MAX_RECORD);
    if (n > 0) {
        xylem_mutex_lock(tls->ssl_mu);
        int fed = tls_backend_conn_feed(tls->be, tls->rbuf, n);
        xylem_mutex_unlock(tls->ssl_mu);
        if (fed == 0) {
            ret = n;
        }
    } else if (n == 0) {
        ret = 0;
    }
    xylem_mutex_unlock(tls->rd_mu);
    return ret;
}

static int _tls_do_handshake(tls_conn_t* tls) {
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(tls->be);
        xylem_mutex_unlock(tls->ssl_mu);

        /**
         * Every handshake step may queue a flight (incl. a fatal alert on
         * error), so flush before dispatching on the state.
         */
        if (_tls_pump_out(tls) != 0) {
            return -1;
        }
        switch (st) {
            case TLS_BACKEND_OK:
                return 0;
            case TLS_BACKEND_WANT_READ:
                if (_tls_pump_in(tls) <= 0) {
                    return -1;
                }
                continue;
            case TLS_BACKEND_WANT_WRITE:
                continue;
            default:
                return -1;
        }
    }
}

static int _tls_client_handshake(tls_conn_t* tls, const char* server_name) {
    tls->be = tls_backend_conn_create(tls->ctx->be, false);
    if (!tls->be) {
        return -1;
    }

    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = tls->ctx->verify_server ? TLS_BACKEND_VERIFY_PEER
                                         : TLS_BACKEND_VERIFY_NONE;
    bool verify_peer = (cfg.verify != TLS_BACKEND_VERIFY_NONE);

    if (!server_name && verify_peer) {
        xylem_loge("<tls> dial server_name=NULL with verify_peer; "
                   "peer identity unchecked (MITM risk)");
    }
    if (server_name) {
        addr_t tmp;
        if (addr_pton(server_name, 0, &tmp) != 0) {   /* not an IP literal */
            cfg.sni_name = server_name;
        }
        if (verify_peer) {
            cfg.verify_host = server_name;
        }
    }
    tls_backend_conn_configure(tls->be, &cfg);

    if (_tls_do_handshake(tls) != 0) {
        return -1;
    }
    tls_backend_conn_get_alpn(tls->be, tls->alpn, sizeof(tls->alpn));
    return 0;
}

static void _tls_listener_ref(tls_listener_t* ln) {
    atomic_fetch_add_explicit(&ln->refcnt, 1, memory_order_relaxed);
}

static void _tls_listener_unref(tls_listener_t* ln) {
    if (atomic_fetch_sub_explicit(&ln->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (ln->listener) {
        listener_release(ln->listener);
    }
    free(ln);
}

/**
 * Drive the backend read to completion, pumping ciphertext to/from the
 * stream as the backend state machine demands. Returns bytes read (>0),
 * 0 on clean peer shutdown, or -1 on error/close.
 */
static int _tls_read_loop(tls_conn_t* tls, void* buf, int len) {
    for (;;) {
        int n = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_read(tls->be, buf, len, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
                return n;
            case TLS_BACKEND_CLOSED:
                return 0;
            case TLS_BACKEND_WANT_READ:
                if (_tls_pump_in(tls) <= 0) {
                    return -1;
                }
                continue;
            case TLS_BACKEND_WANT_WRITE:
                /* Rekey during read: flush the handshake flight first. */
                if (_tls_pump_out(tls) != 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
}

/**
 * Drive the backend write of the whole buffer to completion, flushing
 * the ciphertext produced after each accepted chunk. Returns 0 once all
 * len bytes are written and flushed, -1 on error/close.
 */
static int _tls_write_loop(
    tls_conn_t* tls,
    const void* data,
    int         len) {
    const char* ptr = (const char*)data;
    int         rem = len;

    while (rem > 0) {
        /**
         * Feed at most one record's worth of plaintext per step so the
         * backend only ever holds a single record of ciphertext, which
         * _tls_pump_out flushes below before the next chunk is encrypted.
         */
        int chunk = rem < TLS_MAX_PLAINTEXT ? rem : TLS_MAX_PLAINTEXT;
        int n     = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_write(tls->be, ptr, chunk, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        /**
         * A write step may queue ciphertext (the encrypted chunk, a
         * rekey flight, or a fatal alert on error), so flush before
         * dispatching on the state.
         */
        if (_tls_pump_out(tls) != 0) {
            return -1;
        }
        switch (st) {
            case TLS_BACKEND_OK:
                ptr += n;
                rem -= n;
                continue;
            case TLS_BACKEND_WANT_WRITE:
                continue;
            case TLS_BACKEND_WANT_READ:
                /**
                 * Rekey: our flight is already flushed above; now wait
                 * on the peer's, or both sides block forever.
                 */
                if (_tls_pump_in(tls) <= 0) {
                    return -1;
                }
                continue;
            default:
                return -1;
        }
    }
    return 0;
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
    tls->be = tls_backend_conn_create(tls->ctx->be, true);
    if (!tls->be) {
        xylem_loge("<tls> accept ssl init failed");
        return -1;
    }
    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = tls->ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                         : TLS_BACKEND_VERIFY_NONE;
    tls_backend_conn_configure(tls->be, &cfg);

    /* Arm the handshake deadline; disarm regardless of outcome. */
    _tls_set_deadline(tls, _tls_make_deadline(tls->hs_timeout_ms));
    int rc = _tls_do_handshake(tls);
    _tls_set_deadline(tls, 0);

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
    int st = atomic_load_explicit(&tls->hs_state, memory_order_acquire);
    if (st == HS_DONE) {
        return 0;
    }
    if (st == HS_FAILED) {
        return -1;
    }

    /* Re-check under hs_mu: a concurrent driver may have finished meanwhile. */
    xylem_mutex_lock(tls->hs_mu);
    st = atomic_load_explicit(&tls->hs_state, memory_order_acquire);
    if (st == HS_DONE) {
        xylem_mutex_unlock(tls->hs_mu);
        return 0;
    }
    if (st == HS_FAILED) {
        xylem_mutex_unlock(tls->hs_mu);
        return -1;
    }

    int rc = _tls_server_handshake(tls);
    atomic_store_explicit(&tls->hs_state, rc == 0 ? HS_DONE : HS_FAILED,
                          memory_order_release);
    xylem_mutex_unlock(tls->hs_mu);
    return rc;
}

/**
 * Best-effort close_notify so the peer can tell a clean close from a
 * truncation. The caller must hold wr_mu (so this is the sole parker on
 * the write direction) and must not yet have interrupted the stream.
 * Bounded park, not unbounded, so a stalled peer can never pin teardown.
 */
static void _tls_flush_close_notify(tls_conn_t* tls) {
    if (!tls->be) {
        return;
    }
    xylem_mutex_lock(tls->ssl_mu);
    tls_backend_conn_shutdown(tls->be);
    int n = tls_backend_conn_drain(tls->be, tls->wbuf, TLS_MAX_RECORD);
    xylem_mutex_unlock(tls->ssl_mu);

    stream_set_write_deadline(
        tls->stream,
        _tls_make_deadline(TLS_CLOSE_NOTIFY_TIMEOUT_MS));
    if (n > 0) {
        stream_write(tls->stream, tls->wbuf, n);
    }
    stream_set_write_deadline(tls->stream, 0);
}

tls_ctx_t* tls_ctx_create(void) {
    tls_ctx_t* ctx = (tls_ctx_t*)calloc(1, sizeof(tls_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->be = tls_backend_ctx_create(TLS_BACKEND_PROTO_TLS);
    if (!ctx->be) {
        free(ctx);
        return NULL;
    }
    ctx->verify_server = true;
    ctx->verify_client = false;
    return ctx;
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
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_dial");

    uint64_t timeout_ms = opts ? opts->handshake_timeout_ms : 0;
    uint64_t deadline   = _tls_make_deadline(timeout_ms);
    stream_t* stream
        = stream_dial(host, port, timeout_ms, opts && opts->enable_mss_clamp);
    if (!stream) {
        return NULL;
    }

    tls_conn_t* tls = _tls_conn_create(stream);
    if (!tls) {
        stream_release(stream);
        return NULL;
    }

    tls->ctx = ctx;

    /* The same absolute deadline bounds connect plus handshake. */
    _tls_set_deadline(tls, deadline);
    if (_tls_client_handshake(tls, opts ? opts->server_name : NULL) != 0) {
        xylem_loge("<tls> dial handshake failed host=%s port=%u", host, port);
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    return tls;
}

void tls_close(tls_conn_t* tls) {
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_close");

    if (atomic_exchange(&tls->closed, true)) {
        return;
    }

    /**
     * Graceful close_notify parks on the write direction and needs
     * exclusive ownership of wr_mu. Acquire wr_mu with trylock, never a
     * blocking lock: another coroutine (possibly on a different worker)
     * parked in tls_write holds wr_mu across its (possibly unbounded)
     * park, and a blocking lock here would wait behind the very
     * coroutine that stream_interrupt must wake -- a teardown deadlock. A
     * busy wr_mu means a writer is active, so skip the notify and go
     * straight to the hard close, whose stream interrupt wakes any parked
     * reader/writer. This mirrors Go's tls.Conn.Close, which skips
     * close_notify when a Write is in flight.
     */
    if (xylem_mutex_trylock(tls->wr_mu)) {
        _tls_flush_close_notify(tls);
        xylem_mutex_unlock(tls->wr_mu);
    }

    stream_interrupt(tls->stream);
    _tls_conn_unref(tls);
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
        listener_release(listener);
        return NULL;
    }

    ln->listener = listener;
    ln->ctx      = ctx;
    if (opts) {
        ln->opts = *opts;
    }

    _tls_listener_ref(ln);
    return ln;
}

tls_conn_t* tls_accept(tls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_accept");

    _tls_listener_ref(ln);

    tls_conn_t* ready = NULL;
    for (;;) {
        if (atomic_load_explicit(&ln->closed, memory_order_acquire)) {
            break;
        }

        stream_t* stream = listener_accept(ln->listener);
        if (!stream) {
            break;
        }

        tls_conn_t* conn = _tls_conn_create(stream);
        if (!conn) {
            stream_release(stream);
            break;
        }

        conn->ctx = ln->ctx;

        /**
         * Defer the handshake to first I/O so it runs in the handler
         * coroutine and parallelizes, instead of serializing every
         * client's multi-round-trip handshake behind this acceptor.
         */
        conn->hs_timeout_ms = ln->opts.handshake_timeout_ms;
        atomic_store_explicit(&conn->hs_state, HS_PENDING,
                              memory_order_release);
        ready = conn;
        break;
    }

    _tls_listener_unref(ln);
    return ready;
}

void tls_close_listener(tls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    listener_interrupt(ln->listener);
    _tls_listener_unref(ln);
}

int tls_read(tls_conn_t* tls, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_read");

    /**
     * Take the reference before testing `closed`: a concurrent
     * tls_close() from another coroutine (possibly on another worker)
     * may drop the last reference and free tls in the window between
     * the test and the ref, otherwise. Holding a ref first caps a
     * racing close at refcnt 2->1 (no free); our own unref does the
     * final teardown.
     */
    _tls_conn_ref(tls);
    int ret = -1;
    if (!atomic_load_explicit(&tls->closed, memory_order_acquire)
        && _tls_ensure_handshake(tls) == 0) {
        ret = _tls_read_loop(tls, buf, len);
    }
    _tls_conn_unref(tls);
    return ret;
}

int tls_write(tls_conn_t* tls, const void* data, int len) {
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_write");

    _tls_conn_ref(tls);
    int ret = -1;
    if (!atomic_load_explicit(&tls->closed, memory_order_acquire)
        && _tls_ensure_handshake(tls) == 0) {
        ret = _tls_write_loop(tls, data, len);
    }
    _tls_conn_unref(tls);
    return ret;
}

int tls_handshake(tls_conn_t* tls) {
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_handshake");

    _tls_conn_ref(tls);
    int ret = -1;
    if (!atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        ret = _tls_ensure_handshake(tls);
    }
    _tls_conn_unref(tls);
    return ret;
}

void tls_set_read_deadline(tls_conn_t* tls, uint64_t deadline_ms) {
    stream_set_read_deadline(tls->stream, deadline_ms);
}

void tls_set_write_deadline(tls_conn_t* tls, uint64_t deadline_ms) {
    stream_set_write_deadline(tls->stream, deadline_ms);
}

int tls_remote_addr(
    tls_conn_t* tls,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    return stream_remote_addr(tls->stream, host, host_len, port);
}

int tls_local_addr(
    tls_conn_t* tls,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    return stream_local_addr(tls->stream, host, host_len, port);
}

int tls_listener_addr(
    tls_listener_t* ln,
    char*           host,
    size_t          host_len,
    uint16_t*       port) {
    return listener_addr(ln->listener, host, host_len, port);
}

const char* tls_get_alpn(tls_conn_t* tls) {
    return tls->alpn[0] ? tls->alpn : NULL;
}

tls_conn_t* tls_client_handshake_fd(
    platform_sock_t   fd,
    tls_ctx_t*        ctx,
    xylem_tls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("tls", "tls_client_handshake_fd");

    stream_t* stream = stream_from_fd(fd);
    if (!stream) {
        platform_socket_close(fd);
        return NULL;
    }

    tls_conn_t* tls = _tls_conn_create(stream);
    if (!tls) {
        stream_release(stream);
        return NULL;
    }
    tls->ctx = ctx;

    /* Arm the handshake deadline; disarm on success. */
    _tls_set_deadline(tls,
                      _tls_make_deadline(opts ? opts->handshake_timeout_ms
                                              : 0));

    if (_tls_client_handshake(tls, opts ? opts->server_name : NULL) != 0) {
        xylem_loge("<tls> client handshake failed");
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    return tls;
}

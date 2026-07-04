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

#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "container/rbtree.h"
#include "net/addr.h"
#include "net/datagram.h"
#include "net/tls/tls-backend.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DTLS_DEFAULT_TIMEOUT_MS  30000
#define DTLS_INBOX_CAP           64
#define DTLS_DEFAULT_MTU         1500
#define DTLS_DGRAM_POOL_CAP      1024

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

typedef struct _dtls_dgram_s {
    struct _dtls_dgram_s* next;
    size_t                len;
    char                  data[];
} _dtls_dgram_t;

struct xylem_dtls_ctx_s {
    tls_backend_ctx_t* be;
    bool               verify_server;
    bool               verify_client;
};

struct xylem_dtls_conn_s {
    tls_backend_conn_t*     be;
    addr_t                  peer_addr;
    char                    alpn[32];
    _Atomic bool            closed;
    _Atomic bool            closing;
    _Atomic int32_t         refcnt;
    bool                    handshake_done;
    _dtls_dgram_t*          pending_dgram;

    /* client-side only */
    datagram_t*             datagram;
    xylem_mutex_t*           ssl_mu;   /* serializes all backend access.  */
    xylem_mutex_t*           rd_mu;    /* sole parker on read readiness.   */
    xylem_mutex_t*           wr_mu;    /* sole parker on write readiness.  */

    /* server-side only */
    xylem_channel_t*         inbox;
    _Atomic int32_t          inbox_len;
    scheduler_timer_t*      handshake_timer;
    xylem_dtls_listener_t*   listener;
    rbtree_node_t            server_node;
    bool                     in_sessions;
    uint64_t                 rd_deadline_ms;
    uint64_t                 wr_deadline_ms;
};

struct xylem_dtls_listener_s {
    datagram_t*           datagram;
    xylem_dtls_ctx_t*     ctx;
    xylem_dtls_opts_t     opts;
    rbtree_t              sessions;
    xylem_mutex_t*        sessions_mu;
    xylem_mutex_t*        write_mu;
    xylem_mutex_t*        dgram_pool_mu;
    scheduler_t*          sched;
    _dtls_dgram_t*        dgram_pool;
    size_t                dgram_pool_len;
    size_t                dgram_bufsz;

    xylem_channel_t*      accept_ch;

    _Atomic bool          closed;
    _Atomic int32_t       refcnt;
};

xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_create");

    xylem_dtls_ctx_t* ctx = (xylem_dtls_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->be = tls_backend_ctx_create(TLS_BACKEND_PROTO_DTLS);
    if (!ctx->be) {
        free(ctx);
        return NULL;
    }
    ctx->verify_server = true;
    ctx->verify_client = false;
    return ctx;
}

void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_destroy");

    tls_backend_ctx_destroy(ctx->be);
    free(ctx);
}

int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_set_keylog");

    return tls_backend_ctx_set_keylog(ctx->be, path);
}

int xylem_dtls_ctx_load_cert(
    xylem_dtls_ctx_t* ctx,
    const char*       hostname,
    const char*       cert,
    const char*       key) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_cert");

    return tls_backend_ctx_load_cert_file(ctx->be, hostname, cert, key);
}

int xylem_dtls_ctx_load_cert_mem(
    xylem_dtls_ctx_t* ctx,
    const char*       hostname,
    const void*       cert_pem,
    size_t            cert_len,
    const void*       key_pem,
    size_t            key_len) {
    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_cert_mem");

    return tls_backend_ctx_load_cert_mem(ctx->be, hostname, cert_pem, cert_len,
                                         key_pem, key_len);
}

int xylem_dtls_ctx_load_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_ca");

    return tls_backend_ctx_load_ca_file(ctx->be, ca_file);
}

int xylem_dtls_ctx_load_system_ca(
    xylem_dtls_ctx_t* ctx,
    const char*       fallback_ca_file) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_system_ca");

    return tls_backend_ctx_load_system_ca(ctx->be, fallback_ca_file);
}

void xylem_dtls_ctx_verify_server(xylem_dtls_ctx_t* ctx, bool enable) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_verify_server");

    ctx->verify_server = enable;
}

void xylem_dtls_ctx_verify_client(xylem_dtls_ctx_t* ctx, bool enable) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_verify_client");

    ctx->verify_client = enable;
}

int xylem_dtls_ctx_set_alpn(
    xylem_dtls_ctx_t* ctx,
    const char**      protocols,
    size_t            count) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_set_alpn");

    return tls_backend_ctx_set_alpn(ctx->be, protocols, count);
}

static void _dtls_listener_ref(xylem_dtls_listener_t* ln) {
    atomic_fetch_add(&ln->refcnt, 1);
}

static void _dtls_listener_unref(xylem_dtls_listener_t* ln) {
    if (atomic_fetch_sub(&ln->refcnt, 1)
        != 1) {
        return;
    }
    datagram_release(ln->datagram);
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

static void _dtls_conn_ref(xylem_dtls_conn_t* dtls) {
    atomic_fetch_add(&dtls->refcnt, 1);
}

static void _dtls_conn_unref(xylem_dtls_conn_t* dtls) {
    if (atomic_fetch_sub(&dtls->refcnt, 1)
        != 1) {
        return;
    }
    xylem_dtls_listener_t* ln = dtls->listener;
    if (dtls->be) {
        tls_backend_conn_destroy(dtls->be);
    }
    if (dtls->datagram) {
        datagram_set_read_deadline(dtls->datagram, 0);
        datagram_set_write_deadline(dtls->datagram, 0);
        datagram_release(dtls->datagram);
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
         * inbox was already closed by xylem_dtls_close, so recv never
         * parks here; it pops leftovers and returns NULL once empty.
         */
        _dtls_dgram_t* dgram;
        while ((dgram = (_dtls_dgram_t*)xylem_channel_recv(dtls->inbox))
               != NULL) {
            free(dgram);
        }
        xylem_channel_destroy(dtls->inbox);
    }
    free(dtls);
    if (ln) {
        _dtls_listener_unref(ln);
    }
}

static _dtls_dgram_t* _dtls_dgram_alloc(xylem_dtls_listener_t* ln) {
    _dtls_dgram_t* dgram = NULL;

    xylem_mutex_lock(ln->dgram_pool_mu);
    if (ln->dgram_pool) {
        dgram = ln->dgram_pool;
        ln->dgram_pool = dgram->next;
        ln->dgram_pool_len--;
    }
    xylem_mutex_unlock(ln->dgram_pool_mu);

    if (!dgram) {
        dgram =
            (_dtls_dgram_t*)malloc(sizeof(_dtls_dgram_t) + ln->dgram_bufsz);
        if (!dgram) {
            return NULL;
        }
    }
    dgram->next = NULL;
    dgram->len  = 0;
    return dgram;
}

static void _dtls_dgram_release(
    xylem_dtls_listener_t* ln,
    _dtls_dgram_t*         dgram) {
    if (!dgram) {
        return;
    }

    xylem_mutex_lock(ln->dgram_pool_mu);
    if (ln->dgram_pool_len < DTLS_DGRAM_POOL_CAP) {
        dgram->next = ln->dgram_pool;
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
static void _dtls_inbox_push(xylem_dtls_conn_t* dtls, _dtls_dgram_t* dgram) {
    if (atomic_load(&dtls->inbox_len)
        >= (int32_t)DTLS_INBOX_CAP) {
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
    const xylem_dtls_conn_t* da =
        rbtree_entry(a, xylem_dtls_conn_t, server_node);
    const xylem_dtls_conn_t* db =
        rbtree_entry(b, xylem_dtls_conn_t, server_node);
    return _dtls_addr_cmp(&da->peer_addr, &db->peer_addr);
}

static int _dtls_session_cmp_kn(
    const void*          key,
    const rbtree_node_t* node) {
    const addr_t* addr = (const addr_t*)key;
    const xylem_dtls_conn_t* dtls =
        rbtree_entry(node, xylem_dtls_conn_t, server_node);
    return _dtls_addr_cmp(addr, &dtls->peer_addr);
}

static xylem_dtls_conn_t* _dtls_find_session(
    xylem_dtls_listener_t* ln,
    addr_t*                addr) {
    rbtree_node_t* node = rbtree_find(&ln->sessions, addr);
    if (!node) {
        return NULL;
    }
    return rbtree_entry(node, xylem_dtls_conn_t, server_node);
}

static bool _dtls_server_unlink(xylem_dtls_conn_t* dtls) {
    xylem_dtls_listener_t* ln = dtls->listener;
    if (!ln) {
        return false;
    }

    bool unlinked = false;
    xylem_mutex_lock(ln->sessions_mu);
    if (dtls->in_sessions) {
        rbtree_remove(&ln->sessions, &dtls->server_node);
        dtls->in_sessions = false;
        unlinked = true;
    }
    xylem_mutex_unlock(ln->sessions_mu);
    return unlinked;
}

static void _dtls_server_shutdown(xylem_dtls_conn_t* dtls) {
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
    xylem_dtls_conn_t* dtls,
    _dtls_dgram_t*     dgram,
    void*              buf,
    int                len) {
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

static _dtls_dgram_t* _dtls_take_dgram(xylem_dtls_conn_t* dtls) {
    if (dtls->pending_dgram) {
        _dtls_dgram_t* dgram = dtls->pending_dgram;
        dtls->pending_dgram = NULL;
        return dgram;
    }
    _dtls_dgram_t* dgram =
        (_dtls_dgram_t*)xylem_channel_recv_timeout(dtls->inbox, 0);
    if (dgram) {
        atomic_fetch_sub(&dtls->inbox_len, 1);
    }
    return dgram;
}

static int _dtls_server_io_read(
    void* user,
    void* buf,
    int   len,
    bool* again) {
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)user;
    _dtls_dgram_t* dgram = _dtls_take_dgram(dtls);
    if (!dgram) {
        *again = true;
        return -1;
    }
    return _dtls_copy_dgram(dtls, dgram, buf, len);
}

static int _dtls_server_io_write(
    void*       user,
    const void* buf,
    int         len,
    bool*       again) {
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)user;
    xylem_dtls_listener_t* ln = dtls->listener;
    return datagram_try_send(ln->datagram, buf, len, &dtls->peer_addr, again);
}

static int _dtls_client_io_read(
    void* user,
    void* buf,
    int   len,
    bool* again) {
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)user;
    return datagram_try_recv(dtls->datagram, buf, len, NULL, again);
}

static int _dtls_client_io_write(
    void*       user,
    const void* buf,
    int         len,
    bool*       again) {
    xylem_dtls_conn_t* dtls = (xylem_dtls_conn_t*)user;
    return datagram_try_send(dtls->datagram, buf, len, NULL, again);
}

/**
 * Sentinel returned by wait helpers when the read/write deadline fired
 * rather than a socket or channel error.
 */
#define DTLS_WAIT_TIMEOUT (-2)

static int _dtls_client_wait_read(xylem_dtls_conn_t* dtls) {
    int ret = -1;

    xylem_mutex_lock(dtls->rd_mu);
    iowait_result_t r = datagram_wait_read_result(dtls->datagram);
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

static int _dtls_client_wait_write(xylem_dtls_conn_t* dtls) {
    int ret = -1;

    xylem_mutex_lock(dtls->wr_mu);
    iowait_result_t r = datagram_wait_write_result(dtls->datagram);
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

static int _dtls_server_wait_read(
    xylem_dtls_conn_t* dtls,
    uint64_t           timeout_ms) {
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

static int _dtls_server_wait_write(xylem_dtls_conn_t* dtls) {
    int ret = -1;

    xylem_mutex_lock(dtls->listener->write_mu);
    iowait_result_t r = datagram_wait_write_result(dtls->listener->datagram);
    if (r == IOWAIT_READY) {
        ret = 0;
    } else if (r == IOWAIT_TIMEOUT) {
        ret = DTLS_WAIT_TIMEOUT;
    }
    xylem_mutex_unlock(dtls->listener->write_mu);
    return ret;
}

static int _dtls_server_send_record(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len) {
    for (;;) {
        int n = 0;
        tls_backend_state_t st = tls_backend_conn_write(dtls->be, data, len,
                                                        &n);
        switch (st) {
            case TLS_BACKEND_OK:
                return 0;
            case TLS_BACKEND_WANT_WRITE: {
                if (datagram_wait_write(dtls->listener->datagram) != 0) {
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
static int _dtls_client_do_handshake(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline) {
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        xylem_mutex_unlock(dtls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
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

static void _dtls_cache_alpn(xylem_dtls_conn_t* dtls) {
    tls_backend_conn_get_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
}

static int _dtls_client_recv_loop(xylem_dtls_conn_t* dtls, void* buf, int len) {
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

static int _dtls_client_recv(xylem_dtls_conn_t* dtls, void* buf, int len) {
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
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len) {
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
        tls_backend_state_t st = tls_backend_conn_write(dtls->be, data, len,
                                                        &n);
        xylem_mutex_unlock(dtls->ssl_mu);

        switch (st) {
            case TLS_BACKEND_OK:
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

static int _dtls_client_send(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len) {
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

static void _dtls_client_close(xylem_dtls_conn_t* dtls) {
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
    datagram_interrupt(dtls->datagram);
    _dtls_conn_unref(dtls);
}

static void _dtls_handshake_coro(void* arg) {
    xylem_dtls_conn_t* dtls = arg;
    xylem_dtls_listener_t* ln = dtls->listener;

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
        socklen_t salen =
            (dtls->peer_addr.storage.ss_family == AF_INET6)
                ? (socklen_t)sizeof(struct sockaddr_in6)
                : (socklen_t)sizeof(struct sockaddr_in);
        dtls_backend_conn_set_peer_addr(dtls->be, &dtls->peer_addr.storage,
                                        salen);
    }
    dtls_backend_conn_set_mtu(dtls->be, ln->opts.mtu);

    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = ln->ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                        : TLS_BACKEND_VERIFY_NONE;
    tls_backend_conn_configure(dtls->be, &cfg);

    uint64_t hs_timeout = ln->opts.handshake_timeout_ms > 0
        ? ln->opts.handshake_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
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
        bool have_rt = dtls_backend_conn_get_timeout(dtls->be, &rt_ms);
        if (have_rt && rt_ms < wait_ms) {
            wait_ms = rt_ms;
        }

        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        if (st == TLS_BACKEND_OK) {
            dtls->handshake_done = true;
            success = true;
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
    dtls->handshake_timer  = NULL;

    if (!success) {
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        return;
    }

    _dtls_cache_alpn(dtls);
    xylem_mutex_lock(ln->sessions_mu);
    if (atomic_load(&ln->closed)) {
        xylem_mutex_unlock(ln->sessions_mu);
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        return;
    }
    _dtls_conn_ref(dtls);
    xylem_channel_send(ln->accept_ch, dtls);
    xylem_mutex_unlock(ln->sessions_mu);
    _dtls_conn_unref(dtls);
}

static void _dtls_dispatcher(void* arg) {
    xylem_dtls_listener_t* ln = arg;

    while (!atomic_load(&ln->closed)) {
        _dtls_dgram_t* dgram = _dtls_dgram_alloc(ln);
        if (!dgram) {
            xylem_loge("<dtls> dispatcher dgram alloc failed size=%zu",
                       ln->dgram_bufsz);
            break;
        }

        addr_t from_addr;
        int n = datagram_recv(
            ln->datagram, dgram->data, (int)ln->dgram_bufsz, &from_addr);

        if (n < 0) {
            _dtls_dgram_release(ln, dgram);
            break;
        }
        dgram->len = (size_t)n;

        xylem_mutex_lock(ln->sessions_mu);
        xylem_dtls_conn_t* dtls = _dtls_find_session(ln, &from_addr);
        if (dtls) {
            /**
             * Push under the lock so a concurrent xylem_dtls_close
             * cannot remove + free the session between the lookup and
             * the push. _dtls_inbox_push only enqueues (it never
             * parks), so the critical section stays short.
             */
            _dtls_inbox_push(dtls, dgram);
            xylem_mutex_unlock(ln->sessions_mu);
            continue;
        }
        xylem_mutex_unlock(ln->sessions_mu);

        dtls = (xylem_dtls_conn_t*)calloc(1, sizeof(xylem_dtls_conn_t));
        if (!dtls) {
            _dtls_dgram_release(ln, dgram);
            continue;
        }
        atomic_store(&dtls->refcnt, 1);
        dtls->peer_addr        = from_addr;
        dtls->inbox            = xylem_channel_create();
        dtls->handshake_timer  = scheduler_timer_create(ln->sched);

        if (!dtls->inbox
            || !dtls->handshake_timer) {
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

static int _dtls_server_recv_loop(xylem_dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    for (;;) {
        int n = 0;
        tls_backend_state_t st = tls_backend_conn_read(dtls->be, buf, len, &n);
        switch (st) {
            case TLS_BACKEND_OK:
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

static int _dtls_server_recv(xylem_dtls_conn_t* dtls, void* buf, int len) {
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

static int _dtls_server_send(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len) {
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
                dtls->listener->datagram, dtls->wr_deadline_ms);
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

static void _dtls_server_close(xylem_dtls_conn_t* dtls) {
    /**
     * No close_notify: it is best-effort on a datagram transport and the
     * server session has no ssl_mu, so touching dtls->be here would race
     * a concurrent reader/writer still in the backend. DTLS records are
     * independently authenticated with explicit boundaries, so there is
     * no stream-truncation concern to guard against -- just drop it,
     * matching the client close path.
     */
    _dtls_server_shutdown(dtls);
    _dtls_conn_unref(dtls);
}

xylem_dtls_conn_t* xylem_dtls_dial(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_dial");

    datagram_t* datagram = datagram_dial(host, port);
    if (!datagram) {
        xylem_loge("<dtls> dial failed host=%s port=%u", host, port);
        return NULL;
    }

    xylem_dtls_conn_t* dtls =
        (xylem_dtls_conn_t*)calloc(1, sizeof(xylem_dtls_conn_t));
    if (!dtls) {
        datagram_release(datagram);
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

    uint64_t timeout = (opts && opts->handshake_timeout_ms > 0)
        ? opts->handshake_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                        + timeout;
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

    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = ctx->verify_server ? TLS_BACKEND_VERIFY_PEER
                                    : TLS_BACKEND_VERIFY_NONE;
    const char* server_name = opts ? opts->server_name : NULL;
    if (server_name) {
        addr_t tmp;
        if (addr_pton(server_name, 0, &tmp) != 0) {   /* not an IP literal */
            cfg.sni_name = server_name;
        }
        if (ctx->verify_server) {
            cfg.verify_host = server_name;
        }
    } else if (ctx->verify_server) {
        xylem_loge("<dtls> dial server_name=NULL with verify_server; "
                   "peer identity unchecked (MITM risk)");
    }
    tls_backend_conn_configure(dtls->be, &cfg);

    if (_dtls_client_do_handshake(dtls, deadline) != 0) {
        _dtls_conn_unref(dtls);
        return NULL;
    }

    datagram_set_read_deadline(dtls->datagram, 0);
    datagram_set_write_deadline(dtls->datagram, 0);

    _dtls_cache_alpn(dtls);
    return dtls;
}

xylem_dtls_listener_t* xylem_dtls_listen(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_listen");

    datagram_t* datagram = datagram_listen(host, port);
    if (!datagram) {
        xylem_loge("<dtls> listen failed host=%s port=%u", host, port);
        return NULL;
    }

    xylem_dtls_listener_t* ln =
        (xylem_dtls_listener_t*)calloc(1, sizeof(xylem_dtls_listener_t));
    if (!ln) {
        datagram_release(datagram);
        return NULL;
    }

    ln->datagram = datagram;
    ln->ctx      = ctx;
    ln->sched    = runtime_get_scheduler();
    if (opts) {
        ln->opts = *opts;
    }

    ln->dgram_bufsz = _dtls_record_bufsz(ln->opts.mtu);

    _dtls_listener_ref(ln); /* caller's reference (released by close) */

    rbtree_init(&ln->sessions, _dtls_session_cmp_nn, _dtls_session_cmp_kn);
    ln->sessions_mu = xylem_mutex_create();
    ln->write_mu    = xylem_mutex_create();
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

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_accept");

    _dtls_listener_ref(ln);
    xylem_dtls_conn_t* conn =
        (xylem_dtls_conn_t*)xylem_channel_recv(ln->accept_ch);
    _dtls_listener_unref(ln);
    return conn;
}

int xylem_dtls_read(xylem_dtls_conn_t* dtls, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_read");

    if (!buf || len <= 0) {
        return -1;
    }

    if (dtls->listener) {
        return _dtls_server_recv(dtls, buf, len);
    }
    return _dtls_client_recv(dtls, buf, len);
}

int xylem_dtls_write(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_write");

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

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_close");

    if (dtls->listener) {
        _dtls_server_close(dtls);
    } else {
        _dtls_client_close(dtls);
    }
}

void xylem_dtls_close_listener(xylem_dtls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_close_listener");

    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    xylem_mutex_lock(ln->sessions_mu);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_dtls_conn_t* dtls =
            rbtree_entry(node, xylem_dtls_conn_t, server_node);
        _dtls_conn_ref(dtls);
        xylem_mutex_unlock(ln->sessions_mu);
        _dtls_server_shutdown(dtls);
        _dtls_conn_unref(dtls);
        xylem_mutex_lock(ln->sessions_mu);
    }
    xylem_mutex_unlock(ln->sessions_mu);

    datagram_interrupt(ln->datagram);
    xylem_channel_close(ln->accept_ch);
    _dtls_listener_unref(ln);
}

void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_set_read_deadline");

    if (dtls->listener) {
        dtls->rd_deadline_ms = deadline_ms;
    } else {
        datagram_set_read_deadline(dtls->datagram, deadline_ms);
    }
}

void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_set_write_deadline");

    if (dtls->listener) {
        dtls->wr_deadline_ms = deadline_ms;
    } else {
        datagram_set_write_deadline(dtls->datagram, deadline_ms);
    }
}

const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_get_alpn");

    return dtls->alpn[0] ? dtls->alpn : NULL;
}

int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_remote_addr");

    if (!dtls->listener) {
        return datagram_remote_addr(dtls->datagram, host, host_len, port);
    }
    return addr_ntop(&dtls->peer_addr, host, host_len, port);
}

int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_local_addr");

    if (dtls->listener) {
        return datagram_local_addr(
            dtls->listener->datagram, host, host_len, port);
    }
    return datagram_local_addr(dtls->datagram, host, host_len, port);
}

int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char*                  host,
    size_t                 host_len,
    uint16_t*              port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_listener_addr");

    return datagram_local_addr(ln->datagram, host, host_len, port);
}

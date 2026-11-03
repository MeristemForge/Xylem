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

#include "tls-datagram.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"
#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"

#include "container/rbtree.h"
#include "net/addr.h"
#include "net/datagram.h"
#include "net/tls/tls-backend.h"
#include "net/tls/tls-context.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DTLS_DEFAULT_TIMEOUT_MS 30000
#define DTLS_INBOX_CAP          64
#define DTLS_DEFAULT_MTU        1500
#define DTLS_DGRAM_POOL_CAP     1024
#define DTLS_PENDING_CAP        128

typedef struct _dtls_dgram_s     _dtls_dgram_t;
typedef struct _dtls_transport_s _dtls_transport_t;

typedef enum {
    DTLS_HS_PENDING,
    DTLS_HS_DONE,
    DTLS_HS_FAILED
} _dtls_hs_state_t;

struct xylem_dtls_conn_s {
    tls_backend_conn_t* be;
    addr_t              peer_addr;
    char                alpn[256];
    _Atomic bool        closed;
    _Atomic int32_t     refcnt;
    _Atomic int         hs_state;
    _dtls_dgram_t*      pending_dgram;
    _dtls_dgram_t*      consumed_dgram;
    datagram_t*         datagram;
    xylem_mutex_t*      ssl_mu;
    xylem_mutex_t*      rd_mu;
    xylem_mutex_t*      wr_mu;
    xylem_channel_t*    inbox;
    _Atomic int32_t     inbox_len;
    _Atomic bool        deadline_wake_pending;
    _dtls_transport_t*  transport;
    rbtree_node_t       server_node;
    bool                in_sessions;
    dtls_conn_t*        pending_next;
    xylem_mutex_t*      hs_mu;
    _Atomic uint64_t    hs_deadline_ms;
    _Atomic uint64_t    rd_deadline_ms;
    _Atomic uint64_t    wr_deadline_ms;
};

struct _dtls_transport_s {
    datagram_t*       datagram;
    dtls_ctx_t*       ctx;
    xylem_dtls_opts_t opts;
    rbtree_t          sessions;
    xylem_mutex_t*    sessions_mu;
    xylem_mutex_t*    write_wait_mu;
    xylem_mutex_t*    dgram_pool_mu;
    _dtls_dgram_t*    dgram_pool;
    size_t            dgram_pool_len;
    size_t            dgram_bufsz;
    xylem_channel_t*  accept_ch;
    bool              accepting;
    dtls_conn_t*      pending_head;
    dtls_conn_t*      pending_tail;
    size_t            pending_count;
    xylem_mutex_t*    write_state_mu;
    dtls_conn_t*      writer;
    _Atomic int32_t   refcnt;
};

struct xylem_dtls_listener_s {
    _dtls_transport_t* transport;
};

/**
 * Effective datagram receive buffer size. The backend sizes DTLS records
 * to the link MTU set via dtls_backend_conn_set_mtu, so the dispatcher
 * buffer must be at least that large or an inbound record gets truncated.
 * A zero MTU keeps the historical 1500-byte default.
 */
static inline size_t _dtls_record_bufsz(uint16_t mtu) {
    return (mtu > DTLS_DEFAULT_MTU) ? (size_t)mtu : (size_t)DTLS_DEFAULT_MTU;
}

struct _dtls_dgram_s {
    struct _dtls_dgram_s* next;
    size_t                len;
    char                  data[];
};

static void _dtls_consume_io_budget(void) {
    if (runtime_consume_time()) {
        runtime_yield();
    }
}

static void _dtls_transport_ref(_dtls_transport_t* ln) {
    atomic_fetch_add(&ln->refcnt, 1);
}

static void _dtls_transport_unref(_dtls_transport_t* ln) {
    if (atomic_fetch_sub(&ln->refcnt, 1) != 1) {
        return;
    }
    datagram_destroy(ln->datagram);
    xylem_mutex_destroy(ln->sessions_mu);
    xylem_mutex_destroy(ln->write_wait_mu);
    xylem_mutex_destroy(ln->write_state_mu);
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
    _dtls_transport_t* ln = dtls->transport;
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
    xylem_mutex_destroy(dtls->hs_mu);
    free(dtls->pending_dgram);
    free(dtls->consumed_dgram);
    if (dtls->inbox) {
        /**
         * Drain residual datagrams (freeing their payloads, which the
         * channel itself does not own) then destroy the channel. The
         * inbox was already closed by dtls_close, so recv never
         * parks here; it pops leftovers and returns NULL once empty.
         */
        void* message;
        while ((message = xylem_channel_recv(dtls->inbox)) != NULL) {
            if (message != dtls) {
                free(message);
            }
        }
        xylem_channel_destroy(dtls->inbox);
    }
    free(dtls);
    if (ln) {
        _dtls_transport_unref(ln);
    }
}

static _dtls_dgram_t* _dtls_dgram_alloc(_dtls_transport_t* ln) {
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

static void _dtls_dgram_release(_dtls_transport_t* ln, _dtls_dgram_t* dgram) {
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
        _dtls_dgram_release(dtls->transport, dgram);
        return;
    }
    atomic_fetch_add(&dtls->inbox_len, 1);
    if (xylem_channel_send(dtls->inbox, dgram) != 0) {
        atomic_fetch_sub(&dtls->inbox_len, 1);
        _dtls_dgram_release(dtls->transport, dgram);
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

static dtls_conn_t* _dtls_find_session(_dtls_transport_t* ln, addr_t* addr) {
    rbtree_node_t* node = rbtree_find(&ln->sessions, addr);
    if (!node) {
        return NULL;
    }
    return rbtree_entry(node, dtls_conn_t, server_node);
}

static bool _dtls_server_unlink(dtls_conn_t* dtls) {
    _dtls_transport_t* ln = dtls->transport;
    if (!ln) {
        return false;
    }

    bool unlinked = false;
    xylem_mutex_lock(ln->sessions_mu);
    if (dtls->in_sessions) {
        rbtree_remove(&ln->sessions, &dtls->server_node);
        dtls->in_sessions = false;
        unlinked          = true;
        if (!ln->accepting && rbtree_empty(&ln->sessions)) {
            datagram_close(ln->datagram);
        }
    }
    xylem_mutex_unlock(ln->sessions_mu);
    return unlinked;
}

static void _dtls_server_shutdown(dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }

    bool drop_session_ref = _dtls_server_unlink(dtls);

    /**
     * Close the inbox before dropping the session ref. A parked reader
     * holds its own conn ref and will release it after wakeup.
     */
    if (dtls->inbox) {
        xylem_channel_close(dtls->inbox);
    }

    _dtls_transport_t* transport = dtls->transport;
    xylem_mutex_lock(transport->write_state_mu);
    if (transport->writer == dtls) {
        datagram_set_write_deadline(
            transport->datagram,
            xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC));
    }
    xylem_mutex_unlock(transport->write_state_mu);

    if (drop_session_ref) {
        _dtls_conn_unref(dtls);
    }
}

/**
 * Pending sessions belong only to the transport. Accepted sessions retain
 * their own handles and keep the shared UDP endpoint alive after close.
 */
static void _dtls_transport_stop_accepting(_dtls_transport_t* transport) {
    xylem_mutex_lock(transport->sessions_mu);
    if (!transport->accepting) {
        xylem_mutex_unlock(transport->sessions_mu);
        return;
    }
    transport->accepting     = false;
    dtls_conn_t* pending     = transport->pending_head;
    transport->pending_head  = NULL;
    transport->pending_tail  = NULL;
    transport->pending_count = 0;
    for (dtls_conn_t* conn = pending; conn; conn = conn->pending_next) {
        rbtree_remove(&transport->sessions, &conn->server_node);
        conn->in_sessions = false;
    }
    xylem_channel_close(transport->accept_ch);
    if (rbtree_empty(&transport->sessions)) {
        datagram_close(transport->datagram);
    }
    xylem_mutex_unlock(transport->sessions_mu);

    while (pending) {
        dtls_conn_t* next = pending->pending_next;
        _dtls_server_shutdown(pending);
        _dtls_conn_unref(pending);
        pending = next;
    }
}

static uint64_t _dtls_nearer_deadline(uint64_t a, uint64_t b) {
    return a == 0 || (b != 0 && b < a) ? b : a;
}

static uint64_t _dtls_server_write_deadline(dtls_conn_t* dtls) {
    uint64_t handshake = atomic_load(&dtls->hs_deadline_ms);
    uint64_t deadline =
        _dtls_nearer_deadline(atomic_load(&dtls->wr_deadline_ms), handshake);
    if (handshake != 0) {
        deadline =
            _dtls_nearer_deadline(deadline, atomic_load(&dtls->rd_deadline_ms));
    }
    return deadline;
}

/**
 * Recycle consumed datagrams after the backend returns so BIO callbacks
 * never acquire coroutine locks or yield while inside the crypto library.
 */
static void _dtls_server_backend_lock(dtls_conn_t* dtls) {
    xylem_mutex_lock(dtls->ssl_mu);
}

static void _dtls_server_backend_unlock(dtls_conn_t* dtls) {
    _dtls_dgram_t* consumed = dtls->consumed_dgram;
    dtls->consumed_dgram    = NULL;
    xylem_mutex_unlock(dtls->ssl_mu);
    _dtls_dgram_release(dtls->transport, consumed);
}

static int _dtls_copy_dgram(
    dtls_conn_t* dtls,
    _dtls_dgram_t* dgram,
    void* buf,
    int len) {
    if (!dgram) {
        return -1;
    }
    dtls->consumed_dgram = dgram;
    if (len <= 0) {
        return -1;
    }
    if (dgram->len > (size_t)len) {
        return -1;
    }
    int n = (int)dgram->len;
    memcpy(buf, dgram->data, dgram->len);
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
    dtls_conn_t*       dtls      = (dtls_conn_t*)user;
    _dtls_transport_t* transport = dtls->transport;
    int                n =
        atomic_load(&dtls->closed)
            ? -1
            : datagram_send_with_deadline(transport->datagram,
                                          buf,
                                          len,
                                          &dtls->peer_addr,
                                          _dtls_server_write_deadline(dtls));
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
    int ret = -1;
    xylem_mutex_lock(dtls->rd_mu);
    xylem_mutex_lock(dtls->ssl_mu);
    bool pending = dtls->pending_dgram != NULL;
    xylem_mutex_unlock(dtls->ssl_mu);
    if (atomic_load(&dtls->closed)) {
        xylem_mutex_unlock(dtls->rd_mu);
        return -1;
    }
    if (pending) {
        xylem_mutex_unlock(dtls->rd_mu);
        return 0;
    }
    void* message = xylem_channel_recv_timeout(dtls->inbox, timeout_ms);
    if (message == dtls) {
        atomic_store(&dtls->deadline_wake_pending, false);
        ret = 0;
    } else if (message) {
        atomic_fetch_sub(&dtls->inbox_len, 1);
        xylem_mutex_lock(dtls->ssl_mu);
        dtls->pending_dgram = (_dtls_dgram_t*)message;
        xylem_mutex_unlock(dtls->ssl_mu);
        ret = 0;
    } else if (!atomic_load(&dtls->closed)) {
        ret = DTLS_WAIT_TIMEOUT;
    }
    xylem_mutex_unlock(dtls->rd_mu);
    return ret;
}

static int _dtls_server_wait_write(dtls_conn_t* dtls) {
    _dtls_transport_t* transport = dtls->transport;
    xylem_mutex_lock(transport->write_wait_mu);
    xylem_mutex_lock(transport->write_state_mu);
    if (atomic_load(&dtls->closed)) {
        xylem_mutex_unlock(transport->write_state_mu);
        xylem_mutex_unlock(transport->write_wait_mu);
        return -1;
    }
    uint64_t deadline = _dtls_server_write_deadline(dtls);
    transport->writer = dtls;
    datagram_set_write_deadline(transport->datagram, deadline);
    xylem_mutex_unlock(transport->write_state_mu);

    iowait_result_t result = datagram_wait_write(transport->datagram);

    xylem_mutex_lock(transport->write_state_mu);
    transport->writer = NULL;
    datagram_set_write_deadline(transport->datagram, 0);
    xylem_mutex_unlock(transport->write_state_mu);
    xylem_mutex_unlock(transport->write_wait_mu);
    if (atomic_load(&dtls->closed)) {
        return -1;
    }
    return result == IOWAIT_READY ? 0 : -1;
}
/**
 * Drive the client handshake on the backend state machine. The
 * read wait is bounded by both the overall handshake deadline and the
 * DTLS retransmit timer (dtls_backend_conn_get_timeout); on the latter
 * expiring dtls_backend_conn_handle_timeout retransmits the last flight.
 */
static int _dtls_client_do_handshake(dtls_conn_t* dtls, uint64_t deadline) {
    for (;;) {
        /* A retransmit wait must not leave an expired transport deadline. */
        datagram_set_read_deadline(dtls->datagram, deadline);
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        xylem_mutex_unlock(dtls->ssl_mu);

        switch (st) {
        case TLS_BACKEND_OK:
            _dtls_consume_io_budget();
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
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
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
            _dtls_consume_io_budget();
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
    const void* data,
    int len) {
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
            _dtls_consume_io_budget();
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

static int _dtls_server_do_handshake(dtls_conn_t* dtls) {
    _dtls_transport_t* transport = dtls->transport;
    tls_backend_io_t   io        = {
        .user  = dtls,
        .read  = _dtls_server_io_read,
        .write = _dtls_server_io_write,
    };
    dtls->be = tls_backend_conn_create(dtls_ctx_get_backend(transport->ctx),
                                       true,
                                       &io);
    if (!dtls->be) {
        return -1;
    }
    socklen_t salen = dtls->peer_addr.storage.ss_family == AF_INET6
                          ? (socklen_t)sizeof(struct sockaddr_in6)
                          : (socklen_t)sizeof(struct sockaddr_in);
    dtls_backend_conn_set_peer_addr(dtls->be, &dtls->peer_addr.storage, salen);
    dtls_backend_conn_set_mtu(dtls->be, transport->opts.mtu);

    tls_backend_handshake_cfg_t cfg = {0};
    dtls_ctx_build_server_config(transport->ctx, &cfg);
    if (tls_backend_conn_configure(dtls->be, &cfg) != 0) {
        return -1;
    }
    uint64_t timeout = transport->opts.handshake_timeout_ms;
    if (timeout == 0) {
        timeout = DTLS_DEFAULT_TIMEOUT_MS;
    }
    dtls->hs_deadline_ms =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;

    for (;;) {
        if (atomic_load(&dtls->closed)) {
            return -1;
        }
        uint64_t deadline =
            _dtls_nearer_deadline(dtls->hs_deadline_ms,
                                  atomic_load(&dtls->rd_deadline_ms));
        deadline =
            _dtls_nearer_deadline(deadline, atomic_load(&dtls->wr_deadline_ms));
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            return -1;
        }

        _dtls_server_backend_lock(dtls);
        tls_backend_state_t state = tls_backend_conn_handshake(dtls->be);
        uint64_t            retransmit_ms = 0;
        bool                retransmit =
            dtls_backend_conn_get_timeout(dtls->be, &retransmit_ms);
        _dtls_server_backend_unlock(dtls);
        if (state == TLS_BACKEND_OK) {
            tls_backend_conn_get_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
            return 0;
        }
        if (state == TLS_BACKEND_WANT_WRITE) {
            if (_dtls_server_wait_write(dtls) != 0) {
                return -1;
            }
            continue;
        }
        if (state != TLS_BACKEND_WANT_READ) {
            return -1;
        }

        /* Query after stepping the backend: this flight may arm a new timer. */
        now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            return -1;
        }
        uint64_t wait_ms = deadline - now;
        if (retransmit && retransmit_ms < wait_ms) {
            wait_ms = retransmit_ms;
        }
        int result = _dtls_server_wait_read(dtls, wait_ms);
        if (result == DTLS_WAIT_TIMEOUT) {
            if (xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline) {
                return -1;
            }
            _dtls_server_backend_lock(dtls);
            dtls_backend_conn_handle_timeout(dtls->be);
            _dtls_server_backend_unlock(dtls);
        } else if (result != 0) {
            return -1;
        }
    }
}

static int _dtls_ensure_handshake(dtls_conn_t* dtls) {
    if (atomic_load(&dtls->closed)) {
        return -1;
    }
    if (atomic_load(&dtls->hs_state) == DTLS_HS_DONE) {
        return 0;
    }
    xylem_mutex_lock(dtls->hs_mu);
    if (atomic_load(&dtls->hs_state) == DTLS_HS_PENDING) {
        int result =
            atomic_load(&dtls->closed) ? -1 : _dtls_server_do_handshake(dtls);
        dtls->hs_deadline_ms = 0;
        atomic_store(&dtls->hs_state,
                     result == 0 ? DTLS_HS_DONE : DTLS_HS_FAILED);
        if (result != 0) {
            _dtls_server_shutdown(dtls);
        }
    }
    int result = atomic_load(&dtls->hs_state) == DTLS_HS_DONE &&
                         !atomic_load(&dtls->closed)
                     ? 0
                     : -1;
    xylem_mutex_unlock(dtls->hs_mu);
    return result;
}

/* Only plausible initial ClientHello records may allocate a new session. */
static bool _dtls_is_client_hello(const _dtls_dgram_t* dgram) {
    if (dgram->len < 25) {
        return false;
    }
    const uint8_t* data       = (const uint8_t*)dgram->data;
    size_t         record_len = ((size_t)data[11] << 8) | data[12];
    return data[0] == 22 && data[1] == 0xfe && data[3] == 0 && data[4] == 0 &&
           data[13] == 1 && record_len >= 12 && record_len <= dgram->len - 13;
}

static dtls_conn_t* _dtls_create_session(
    _dtls_transport_t* transport,
    const addr_t*      peer) {
    dtls_conn_t* conn = (dtls_conn_t*)calloc(1, sizeof(dtls_conn_t));
    if (!conn) {
        return NULL;
    }
    atomic_store(&conn->refcnt, 1);
    conn->peer_addr = *peer;
    conn->ssl_mu    = xylem_mutex_create();
    conn->rd_mu     = xylem_mutex_create();
    conn->wr_mu     = xylem_mutex_create();
    conn->hs_mu     = xylem_mutex_create();
    conn->inbox     = xylem_channel_create();
    if (!conn->ssl_mu || !conn->rd_mu || !conn->wr_mu || !conn->hs_mu ||
        !conn->inbox) {
        if (conn->inbox) {
            xylem_channel_close(conn->inbox);
        }
        _dtls_conn_unref(conn);
        return NULL;
    }
    _dtls_transport_ref(transport);
    conn->transport = transport;
    return conn;
}

static void _dtls_dispatcher(void* arg) {
    _dtls_transport_t* transport = (_dtls_transport_t*)arg;
    for (;;) {
        _dtls_dgram_t* dgram = _dtls_dgram_alloc(transport);
        if (!dgram) {
            break;
        }
        addr_t peer;
        int    n;
        for (;;) {
            n = datagram_recv(transport->datagram,
                              dgram->data,
                              (int)transport->dgram_bufsz,
                              &peer);
            if (n >= 0) {
                break;
            }
            if (n != DATAGRAM_IO_AGAIN ||
                datagram_wait_read(transport->datagram) != IOWAIT_READY) {
                break;
            }
        }
        if (n < 0) {
            _dtls_dgram_release(transport, dgram);
            break;
        }
        dgram->len = (size_t)n;

        xylem_mutex_lock(transport->sessions_mu);
        dtls_conn_t* conn = _dtls_find_session(transport, &peer);
        if (conn) {
            _dtls_inbox_push(conn, dgram);
        } else if (transport->accepting &&
                   transport->pending_count < DTLS_PENDING_CAP &&
                   _dtls_is_client_hello(dgram)) {
            conn = _dtls_create_session(transport, &peer);
            if (conn) {
                /* The table owns the initial reference before publication. */
                conn->in_sessions = true;
                rbtree_insert(&transport->sessions, &conn->server_node);
                _dtls_inbox_push(conn, dgram);
                if (xylem_channel_send(transport->accept_ch, transport) == 0) {
                    if (transport->pending_tail) {
                        transport->pending_tail->pending_next = conn;
                    } else {
                        transport->pending_head = conn;
                    }
                    transport->pending_tail = conn;
                    transport->pending_count++;
                } else {
                    rbtree_remove(&transport->sessions, &conn->server_node);
                    conn->in_sessions = false;
                    atomic_store(&conn->closed, true);
                    xylem_channel_close(conn->inbox);
                    _dtls_conn_unref(conn);
                }
            } else {
                _dtls_dgram_release(transport, dgram);
            }
        } else {
            _dtls_dgram_release(transport, dgram);
        }
        xylem_mutex_unlock(transport->sessions_mu);
        _dtls_consume_io_budget();
    }

    _dtls_transport_stop_accepting(transport);
    /* Transport failure must also wake readers on accepted sessions. */
    xylem_mutex_lock(transport->sessions_mu);
    while (!rbtree_empty(&transport->sessions)) {
        dtls_conn_t* conn = rbtree_entry(rbtree_min(&transport->sessions),
                                         dtls_conn_t,
                                         server_node);
        _dtls_conn_ref(conn);
        xylem_mutex_unlock(transport->sessions_mu);
        _dtls_server_shutdown(conn);
        _dtls_conn_unref(conn);
        xylem_mutex_lock(transport->sessions_mu);
    }
    xylem_mutex_unlock(transport->sessions_mu);
    _dtls_transport_unref(transport);
}

static int _dtls_server_recv_loop(dtls_conn_t* dtls, void* buf, int len) {
    for (;;) {
        if (atomic_load(&dtls->closed)) {
            return -1;
        }
        int n = 0;
        _dtls_server_backend_lock(dtls);
        tls_backend_state_t state =
            tls_backend_conn_read(dtls->be, buf, len, &n);
        _dtls_server_backend_unlock(dtls);
        switch (state) {
        case TLS_BACKEND_OK:
            _dtls_consume_io_budget();
            return n;
        case TLS_BACKEND_CLOSED:
            return 0;
        case TLS_BACKEND_WANT_READ: {
            uint64_t wait_ms  = UINT64_MAX;
            uint64_t deadline = atomic_load(&dtls->rd_deadline_ms);
            if (deadline > 0) {
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                if (now >= deadline) {
                    return -1;
                }
                wait_ms = deadline - now;
            }
            if (_dtls_server_wait_read(dtls, wait_ms) != 0) {
                return -1;
            }
            continue;
        }
        case TLS_BACKEND_WANT_WRITE:
            if (_dtls_server_wait_write(dtls) != 0) {
                return -1;
            }
            continue;
        default:
            return -1;
        }
    }
}

static int _dtls_server_recv(dtls_conn_t* dtls, void* buf, int len) {
    _dtls_conn_ref(dtls);
    int ret = _dtls_ensure_handshake(dtls);
    if (ret == 0) {
        ret = _dtls_server_recv_loop(dtls, buf, len);
    }
    if (ret < 0) {
        _dtls_server_shutdown(dtls);
    }
    _dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_server_send_loop(
    dtls_conn_t* dtls,
    const void* data,
    int len) {
    for (;;) {
        if (atomic_load(&dtls->closed)) {
            return -1;
        }
        int n = 0;
        _dtls_server_backend_lock(dtls);
        tls_backend_state_t state =
            tls_backend_conn_write(dtls->be, data, len, &n);
        _dtls_server_backend_unlock(dtls);
        switch (state) {
        case TLS_BACKEND_OK:
            _dtls_consume_io_budget();
            return 0;
        case TLS_BACKEND_WANT_WRITE:
            if (_dtls_server_wait_write(dtls) != 0) {
                return -1;
            }
            continue;
        default:
            return -1;
        }
    }
}

static int _dtls_server_send(dtls_conn_t* dtls, const void* data, int len) {
    _dtls_conn_ref(dtls);
    int ret = _dtls_ensure_handshake(dtls);
    if (ret == 0) {
        xylem_mutex_lock(dtls->wr_mu);
        ret = _dtls_server_send_loop(dtls, data, len);
        xylem_mutex_unlock(dtls->wr_mu);
    }
    if (ret != 0) {
        _dtls_server_shutdown(dtls);
    }
    _dtls_conn_unref(dtls);
    return ret;
}

static void _dtls_server_deadline_changed(dtls_conn_t* dtls) {
    _dtls_transport_t* transport = dtls->transport;
    xylem_mutex_lock(transport->sessions_mu);
    if (dtls->in_sessions && !atomic_load(&dtls->closed) &&
        !atomic_exchange(&dtls->deadline_wake_pending, true)) {
        /* A single control message wakes a receiver to recompute its wait. */
        if (xylem_channel_send(dtls->inbox, dtls) != 0) {
            atomic_store(&dtls->deadline_wake_pending, false);
        }
    }
    xylem_mutex_unlock(transport->sessions_mu);

    xylem_mutex_lock(transport->write_state_mu);
    if (transport->writer == dtls && !atomic_load(&dtls->closed)) {
        datagram_set_write_deadline(transport->datagram,
                                    _dtls_server_write_deadline(dtls));
    }
    xylem_mutex_unlock(transport->write_state_mu);
}
dtls_conn_t* dtls_dial(
    const char*        host,
    uint16_t           port,
    dtls_ctx_t*        ctx,
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
    dtls->be = tls_backend_conn_create(dtls_ctx_get_backend(ctx), false, &io);
    if (!dtls->be) {
        _dtls_conn_unref(dtls);
        return NULL;
    }
    dtls_backend_conn_set_mtu(dtls->be, opts ? opts->mtu : 0);

    tls_backend_handshake_cfg_t cfg         = {0};
    const char*                 server_name = opts ? opts->server_name : NULL;
    if (dtls_ctx_build_client_config(ctx, server_name, "dtls", &cfg) != 0) {
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

    tls_backend_conn_get_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
    atomic_store(&dtls->hs_state, DTLS_HS_DONE);
    return dtls;
}

dtls_listener_t* dtls_listen(
    const char*        host,
    uint16_t           port,
    dtls_ctx_t*        ctx,
    xylem_dtls_opts_t* opts) {
    datagram_t* datagram = datagram_listen(host, port);
    if (!datagram) {
        return NULL;
    }
    dtls_listener_t* listener =
        (dtls_listener_t*)calloc(1, sizeof(dtls_listener_t));
    _dtls_transport_t* transport =
        (_dtls_transport_t*)calloc(1, sizeof(_dtls_transport_t));
    if (!listener || !transport) {
        free(listener);
        free(transport);
        datagram_destroy(datagram);
        return NULL;
    }
    atomic_store(&transport->refcnt, 1);
    transport->datagram = datagram;
    transport->ctx      = ctx;
    if (opts) {
        transport->opts = *opts;
    }
    transport->dgram_bufsz    = _dtls_record_bufsz(transport->opts.mtu);
    transport->sessions_mu    = xylem_mutex_create();
    transport->write_wait_mu  = xylem_mutex_create();
    transport->write_state_mu = xylem_mutex_create();
    transport->dgram_pool_mu  = xylem_mutex_create();
    transport->accept_ch      = xylem_channel_create();
    if (!transport->sessions_mu || !transport->write_wait_mu ||
        !transport->write_state_mu || !transport->dgram_pool_mu ||
        !transport->accept_ch) {
        _dtls_transport_unref(transport);
        free(listener);
        return NULL;
    }
    rbtree_init(&transport->sessions,
                _dtls_session_cmp_nn,
                _dtls_session_cmp_kn);
    transport->accepting = true;
    listener->transport  = transport;
    _dtls_transport_ref(transport);
    if (runtime_spawn(_dtls_dispatcher, transport) != 0) {
        _dtls_transport_unref(transport);
        _dtls_transport_unref(transport);
        free(listener);
        return NULL;
    }
    return listener;
}

dtls_conn_t* dtls_accept(dtls_listener_t* listener) {
    _dtls_transport_t* transport = listener->transport;
    _dtls_transport_ref(transport);
    dtls_conn_t* conn = NULL;
    if (xylem_channel_recv(transport->accept_ch)) {
        xylem_mutex_lock(transport->sessions_mu);
        if (transport->accepting) {
            conn = transport->pending_head;
            if (conn) {
                transport->pending_head = conn->pending_next;
                if (!transport->pending_head) {
                    transport->pending_tail = NULL;
                }
                transport->pending_count--;
                conn->pending_next = NULL;
                /* The caller retains its own connection reference. */
                _dtls_conn_ref(conn);
            }
        }
        xylem_mutex_unlock(transport->sessions_mu);
    }
    _dtls_transport_unref(transport);
    return conn;
}

int dtls_handshake(dtls_conn_t* dtls) {
    _dtls_conn_ref(dtls);
    int result = _dtls_ensure_handshake(dtls);
    _dtls_conn_unref(dtls);
    return result;
}

int dtls_read(dtls_conn_t* dtls, void* buf, int len) {
    if (!buf || len <= 0) {
        return -1;
    }

    if (dtls->transport) {
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

    if (dtls->transport) {
        return _dtls_server_send(dtls, data, len);
    }
    return _dtls_client_send(dtls, data, len);
}

void dtls_close(dtls_conn_t* dtls) {
    if (dtls->transport) {
        _dtls_server_shutdown(dtls);
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

void dtls_close_listener(dtls_listener_t* listener) {
    _dtls_transport_stop_accepting(listener->transport);
}

void dtls_destroy_listener(dtls_listener_t* listener) {
    if (!listener) {
        return;
    }
    dtls_close_listener(listener);
    _dtls_transport_unref(listener->transport);
    free(listener);
}

void dtls_set_read_deadline(dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->transport) {
        atomic_store(&dtls->rd_deadline_ms, deadline_ms);
        _dtls_server_deadline_changed(dtls);
    } else {
        datagram_set_read_deadline(dtls->datagram, deadline_ms);
    }
}

void dtls_set_write_deadline(dtls_conn_t* dtls, uint64_t deadline_ms) {
    if (dtls->transport) {
        atomic_store(&dtls->wr_deadline_ms, deadline_ms);
        _dtls_server_deadline_changed(dtls);
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
    if (!dtls->transport) {
        return datagram_remote_addr(dtls->datagram, host, host_len, port);
    }
    return addr_ntop(&dtls->peer_addr, host, host_len, port);
}

int dtls_local_addr(
    dtls_conn_t* dtls,
    char*        host,
    size_t       host_len,
    uint16_t*    port) {
    if (dtls->transport) {
        return datagram_local_addr(dtls->transport->datagram,
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
    return datagram_local_addr(ln->transport->datagram, host, host_len, port);
}

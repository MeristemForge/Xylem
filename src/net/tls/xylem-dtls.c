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

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "container/rbtree.h"
#include "net/addr.h"
#include "net/tls/tls-backend.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DTLS_DEFAULT_TIMEOUT_MS  30000
#define DTLS_INBOX_CAP           64
#define DTLS_DEFAULT_MTU         1500

/**
 * Effective ciphertext buffer size for a connection. The DTLS backend
 * sizes a record (and therefore the datagram it emits or expects) to
 * the link MTU set via dtls_backend_conn_set_mtu, so the scratch
 * buffers that pump those datagrams to/from the socket must be at least
 * that large or a record gets truncated on send / silently dropped on
 * recv. A zero mtu keeps the historical 1500-byte default that matches
 * the backend's own conservative default-path sizing.
 */
static inline size_t _dtls_record_bufsz(uint16_t mtu) {
    return (mtu > DTLS_DEFAULT_MTU) ? (size_t)mtu
                                    : (size_t)DTLS_DEFAULT_MTU;
}

typedef struct _dtls_dgram_s {
    size_t len;
    char   data[];
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
    _Atomic int32_t         refcnt;
    bool                    handshake_done;

    /**
     * Client-side ciphertext scratch buffers, sized to the link MTU
     * (dtls_backend_conn_set_mtu) so a record is never truncated on its
     * way to or from the socket. wr_buf backs _dtls_client_pump_out
     * (owned by wr_mu) and rd_buf backs _dtls_client_pump_in (owned by
     * rd_mu), so the two pump directions never share a buffer and each
     * is serialized by the mutex guarding its iowait direction. Unused
     * by server-side connections, which drain via the listener buffer
     * and feed inbound datagrams straight into the backend. buf_sz is
     * the allocated size of each buffer.
     */
    char*                    rd_buf;
    char*                    wr_buf;
    size_t                   buf_sz;

    /* client-side only */
    iowait_t*               waiter;
    platform_sock_t          fd;
    xylem_mutex_t*           ssl_mu;   /* serializes all backend access.  */
    xylem_mutex_t*           rd_mu;    /* sole parker on iowait read dir.  */
    xylem_mutex_t*           wr_mu;    /* sole parker on iowait write dir. */

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
    xylem_mutex_t*        sessions_mu;
    xylem_mutex_t*        write_mu;
    scheduler_t*          sched;

    /**
     * Outbound ciphertext scratch for the server data path, sized to
     * the link MTU and guarded by write_mu (which already serializes
     * _dtls_server_send_record across all sessions sharing this
     * socket). Keeps the per-write hot path allocation-free even when
     * a large MTU makes a stack buffer impractical.
     */
    char*                 send_buf;
    size_t                send_buf_sz;

    xylem_channel_t*      accept_ch;

    _Atomic bool          closed;
    _Atomic int32_t       refcnt;
};

xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
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
    tls_backend_ctx_destroy(ctx->be);
    free(ctx);
}

int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    return tls_backend_ctx_set_keylog(ctx->be, path);
}

int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                             const char* hostname,
                             const char* cert, const char* key) {
    return tls_backend_ctx_load_cert_file(ctx->be, hostname, cert, key);
}

int xylem_dtls_ctx_load_cert_mem(xylem_dtls_ctx_t* ctx,
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

int xylem_dtls_ctx_load_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    return tls_backend_ctx_load_ca_file(ctx->be, ca_file);
}

int xylem_dtls_ctx_load_system_ca(xylem_dtls_ctx_t* ctx) {
    return tls_backend_ctx_load_system_ca(ctx->be);
}

void xylem_dtls_ctx_verify_server(xylem_dtls_ctx_t* ctx, bool enable) {
    ctx->verify_server = enable;
}

void xylem_dtls_ctx_verify_client(xylem_dtls_ctx_t* ctx, bool enable) {
    ctx->verify_client = enable;
}

int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                            const char** protocols, size_t count) {
    return tls_backend_ctx_set_alpn(ctx->be, protocols, count);
}

static void _dtls_conn_ref(xylem_dtls_conn_t* dtls) {
    atomic_fetch_add_explicit(&dtls->refcnt, 1, memory_order_relaxed);
}

static void _dtls_conn_unref(xylem_dtls_conn_t* dtls) {
    if (atomic_fetch_sub_explicit(&dtls->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (dtls->be) {
        tls_backend_conn_destroy(dtls->be);
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
    free(dtls->rd_buf);
    free(dtls->wr_buf);
    sched_timer_destroy(dtls->retransmit_timer);
    sched_timer_destroy(dtls->handshake_timer);
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
    xylem_mutex_destroy(ln->sessions_mu);
    xylem_mutex_destroy(ln->write_mu);
    free(ln->send_buf);
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
 * The dispatcher holds sessions_mu across find+push, so the session
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

static void _dtls_server_flush_write_bio(xylem_dtls_conn_t* dtls) {
    size_t bufsz = _dtls_record_bufsz(dtls->listener->opts.mtu);
    char*  buf   = (char*)malloc(bufsz);
    if (!buf) {
        return;
    }
    socklen_t addrlen =
        (dtls->peer_addr.storage.ss_family == AF_INET6)
            ? (socklen_t)sizeof(struct sockaddr_in6)
            : (socklen_t)sizeof(struct sockaddr_in);

    int n;
    while ((n = tls_backend_conn_drain(dtls->be, buf, (int)bufsz)) > 0) {
        platform_socket_sendto(dtls->listener->fd, buf, n,
                               &dtls->peer_addr.storage, addrlen);
    }
    free(buf);
}

static int _dtls_server_send_record(xylem_dtls_conn_t* dtls,
                                   const void* data, int len) {
    socklen_t addrlen =
        (dtls->peer_addr.storage.ss_family == AF_INET6)
            ? (socklen_t)sizeof(struct sockaddr_in6)
            : (socklen_t)sizeof(struct sockaddr_in);

    int wn = 0;
    if (tls_backend_conn_write(dtls->be, data, len, &wn) != TLS_BACKEND_OK) {
        return -1;
    }

    /**
     * DTLS: one backend write produces exactly one datagram. The
     * listener send buffer is sized to the link MTU and guarded by
     * write_mu, which the caller already holds.
     */
    xylem_dtls_listener_t* ln = dtls->listener;
    int rd = tls_backend_conn_drain(dtls->be, ln->send_buf,
                                    (int)ln->send_buf_sz);
    if (rd <= 0) {
        return -1;
    }

    for (;;) {
        ssize_t sent = platform_socket_sendto(
            ln->fd, ln->send_buf, rd,
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

/**
 * Sentinel returned by _dtls_client_pump_in when the read direction
 * timed out (deadline reached) rather than failing outright.
 */
#define DTLS_PUMP_TIMEOUT (-2)

/**
 * Drain pending outbound ciphertext from the backend to the connected
 * socket, one datagram per drain. Holds wr_mu so it is the sole parker
 * on the iowait write direction, and takes ssl_mu only for the drain
 * itself -- never across a socket park -- so a concurrent reader can
 * still touch the backend state. Returns 0 once the backend is empty,
 * -1 on socket error or close.
 */
static int _dtls_client_pump_out(xylem_dtls_conn_t* dtls) {
    int  ret = 0;
    char* buf    = dtls->wr_buf;
    size_t bufsz = dtls->buf_sz;

    xylem_mutex_lock(dtls->wr_mu);
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        int n = tls_backend_conn_drain(dtls->be, buf, (int)bufsz);
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
 * Read one inbound datagram from the connected socket into the backend.
 * Holds rd_mu so it is the sole parker on the iowait read direction,
 * and takes ssl_mu only for the feed -- never across a socket
 * park. Returns the byte count fed (>0), 0 on peer EOF,
 * DTLS_PUMP_TIMEOUT when the read deadline passed, -1 on error/close.
 */
static int _dtls_client_pump_in(xylem_dtls_conn_t* dtls) {
    int  ret = -1;
    char* buf    = dtls->rd_buf;
    size_t bufsz = dtls->buf_sz;

    xylem_mutex_lock(dtls->rd_mu);
    for (;;) {
        ssize_t n = platform_socket_recv(dtls->fd, buf, bufsz);
        if (n > 0) {
            xylem_mutex_lock(dtls->ssl_mu);
            tls_backend_conn_feed(dtls->be, buf, (int)n);
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
 * Drive the client handshake on the backend state machine, pumping
 * ciphertext to/from the connected socket as the backend demands. The
 * read wait is bounded by both the overall handshake deadline and the
 * DTLS retransmit timer (dtls_backend_conn_get_timeout); on the latter
 * expiring dtls_backend_conn_handle_timeout retransmits the last flight.
 */
static int _dtls_client_do_handshake(xylem_dtls_conn_t* dtls,
                                     uint64_t deadline) {
    for (;;) {
        xylem_mutex_lock(dtls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        xylem_mutex_unlock(dtls->ssl_mu);

        if (_dtls_client_pump_out(dtls) != 0) {
            return -1;
        }
        if (st == TLS_BACKEND_OK) {
            return 0;
        }
        if (st == TLS_BACKEND_WANT_READ) {
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
            iowait_set_rd_deadline(dtls->waiter, rd_dl);

            int rc = _dtls_client_pump_in(dtls);
            if (rc == DTLS_PUMP_TIMEOUT) {
                uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                if (deadline > 0 && now >= deadline) {
                    return -1;
                }
                xylem_mutex_lock(dtls->ssl_mu);
                dtls_backend_conn_handle_timeout(dtls->be);
                xylem_mutex_unlock(dtls->ssl_mu);
                continue;
            }
            if (rc <= 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            continue;
        }
        return -1;
    }
}

static void _dtls_cache_alpn(xylem_dtls_conn_t* dtls) {
    tls_backend_conn_get_alpn(dtls->be, dtls->alpn, sizeof(dtls->alpn));
}

static void _dtls_retransmit_cb(sched_timer_t* timer, void* ud);

static void _dtls_arm_retransmit(xylem_dtls_conn_t* dtls) {
    uint64_t ms;
    if (dtls_backend_conn_get_timeout(dtls->be, &ms)) {
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
    dtls_backend_conn_handle_timeout(dtls->be);
    _dtls_server_flush_write_bio(dtls);
    _dtls_arm_retransmit(dtls);
}

static int _dtls_client_recv(xylem_dtls_conn_t* dtls, void* buf, int len) {
    _dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            int n = 0;
            xylem_mutex_lock(dtls->ssl_mu);
            tls_backend_state_t st =
                tls_backend_conn_read(dtls->be, buf, len, &n);
            xylem_mutex_unlock(dtls->ssl_mu);

            if (st == TLS_BACKEND_OK) {
                ret = n;
                break;
            }
            if (st == TLS_BACKEND_CLOSED) {
                ret = 0;
                break;
            }
            if (st == TLS_BACKEND_WANT_READ) {
                /**
                 * Need more ciphertext; fetch one datagram. A read
                 * deadline surfaces as DTLS_PUMP_TIMEOUT (-2) and ends
                 * the read with -1, matching the documented contract.
                 */
                if (_dtls_client_pump_in(dtls) <= 0) {
                    break;
                }
                continue;
            }
            if (st == TLS_BACKEND_WANT_WRITE) {
                /* Post-handshake message (rekey) must flush first. */
                if (_dtls_client_pump_out(dtls) != 0) {
                    break;
                }
                continue;
            }
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
            int n = 0;
            xylem_mutex_lock(dtls->ssl_mu);
            tls_backend_state_t st =
                tls_backend_conn_write(dtls->be, data, len, &n);
            xylem_mutex_unlock(dtls->ssl_mu);

            if (st == TLS_BACKEND_OK) {
                /* Flush the datagram the backend just buffered. */
                ret = (_dtls_client_pump_out(dtls) == 0) ? 0 : -1;
                break;
            }
            if (st == TLS_BACKEND_WANT_WRITE) {
                if (_dtls_client_pump_out(dtls) != 0) {
                    break;
                }
                continue;
            }
            if (st == TLS_BACKEND_WANT_READ) {
                /* Rekey needs inbound data before the write completes. */
                if (_dtls_client_pump_out(dtls) != 0) {
                    break;
                }
                if (_dtls_client_pump_in(dtls) <= 0) {
                    break;
                }
                continue;
            }
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
     * Do not touch the backend object here: a concurrent recv/send may
     * be inside a backend read/write under ssl_mu. Flipping closed +
     * waking both iowait directions makes those calls return -1 and drop
     * their ref; the backend is destroyed once at the final unref, with
     * no parker left. (close_notify is best-effort on a datagram socket
     * and is intentionally skipped, matching the TLS close path.)
     */
    iowait_close(dtls->waiter);
    _dtls_conn_unref(dtls);
}

static void _dtls_handshake_coro(void* arg) {
    xylem_dtls_conn_t* dtls = arg;
    xylem_dtls_listener_t* ln = dtls->listener;

    _dtls_conn_ref(dtls);

    dtls->be = tls_backend_conn_create(ln->ctx->be, true);
    if (!dtls->be) {
        xylem_mutex_lock(ln->sessions_mu);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        xylem_mutex_unlock(ln->sessions_mu);
        _dtls_conn_unref(dtls);
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

        tls_backend_conn_feed(dtls->be, dgram->data, (int)dgram->len);
        free(dgram);

        tls_backend_state_t st = tls_backend_conn_handshake(dtls->be);
        if (st == TLS_BACKEND_OK) {
            _dtls_server_flush_write_bio(dtls);
            dtls->handshake_done = true;
            success = true;
            break;
        }
        if (st == TLS_BACKEND_WANT_READ || st == TLS_BACKEND_WANT_WRITE) {
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
        xylem_mutex_lock(ln->sessions_mu);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        xylem_mutex_unlock(ln->sessions_mu);
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
    size_t bufsz = _dtls_record_bufsz(ln->opts.mtu);
    char*  buf   = (char*)malloc(bufsz);
    if (!buf) {
        xylem_loge("<dtls> dispatcher recv buf alloc failed size=%zu", bufsz);
        _dtls_listener_unref(ln);
        return;
    }

    while (!atomic_load_explicit(&ln->closed, memory_order_acquire)) {
        addr_t from_addr;
        socklen_t from_len = sizeof(from_addr.storage);
        ssize_t n = platform_socket_recvfrom(
            ln->fd, buf, bufsz, &from_addr.storage, &from_len);

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

        xylem_mutex_lock(ln->sessions_mu);
        xylem_dtls_conn_t* dtls = _dtls_find_session(ln, &from_addr);
        if (dtls) {
            /**
             * Push under the lock so a concurrent xylem_dtls_close
             * cannot remove + free the session between the lookup and
             * the push. _dtls_inbox_push only enqueues (it never
             * parks), so the critical section stays short.
             */
            _dtls_dgram_t* dgram =
                (_dtls_dgram_t*)malloc(sizeof(_dtls_dgram_t) + (size_t)n);
            if (dgram) {
                dgram->len = (size_t)n;
                memcpy(dgram->data, buf, (size_t)n);
                _dtls_inbox_push(dtls, dgram);
            }
            xylem_mutex_unlock(ln->sessions_mu);
            continue;
        }
        xylem_mutex_unlock(ln->sessions_mu);

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

        xylem_mutex_lock(ln->sessions_mu);
        rbtree_insert(&ln->sessions, &dtls->server_node);
        xylem_mutex_unlock(ln->sessions_mu);

        runtime_spawn(_dtls_handshake_coro, dtls);
    }

    free(buf);
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
            tls_backend_conn_feed(dtls->be, dgram->data, (int)dgram->len);
            free(dgram);

            int n = 0;
            tls_backend_state_t st =
                tls_backend_conn_read(dtls->be, buf, len, &n);
            if (st == TLS_BACKEND_OK) {
                ret = n;
                break;
            }
            if (st == TLS_BACKEND_CLOSED) {
                ret = 0;
                break;
            }
            if (st != TLS_BACKEND_WANT_READ) {
                break;
            }
            /* WANT_READ: loop for the next datagram */
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

    if (dtls->handshake_done && dtls->be) {
        tls_backend_conn_shutdown(dtls->be);
        _dtls_server_flush_write_bio(dtls);
    }

    /**
     * Unlink from the session tree FIRST so the dispatcher can no
     * longer find this session and therefore cannot xylem_channel_send
     * into the inbox after we close it (send-on-closed aborts). The
     * dispatcher does find+push under sessions_mu, so once the remove
     * commits no further push can target this inbox.
     */
    xylem_dtls_listener_t* ln = dtls->listener;
    xylem_mutex_lock(ln->sessions_mu);
    rbtree_remove(&ln->sessions, &dtls->server_node);
    xylem_mutex_unlock(ln->sessions_mu);

    /**
     * Now close the inbox to wake a parked reader; the reader's
     * in-flight recv holds a channel reference, so the channel stays
     * alive until _dtls_conn_unref drains and destroys it.
     */
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
        xylem_loge("<dtls> dial socket failed host=%s port=%u", host, port);
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

    dtls->buf_sz = _dtls_record_bufsz(opts ? opts->mtu : 0);
    dtls->rd_buf = (char*)malloc(dtls->buf_sz);
    dtls->wr_buf = (char*)malloc(dtls->buf_sz);
    dtls->waiter = iowait_create(fd);
    dtls->ssl_mu = xylem_mutex_create();
    dtls->rd_mu  = xylem_mutex_create();
    dtls->wr_mu  = xylem_mutex_create();
    if (!dtls->rd_buf || !dtls->wr_buf
        || !dtls->waiter || !dtls->ssl_mu || !dtls->rd_mu || !dtls->wr_mu) {
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
     * Memory-buffer backend (not a socket BIO): the connection pumps
     * its own socket via _dtls_client_pump_in/out, which hold rd_mu/wr_mu
     * so each iowait direction has a single parker. This lets one
     * coroutine read while another writes the same connection without
     * tripping the iowait one-parker-per-direction rule.
     */
    dtls->be = tls_backend_conn_create(ctx->be, false);
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
        xylem_loge("<dtls> listen failed host=%s port=%u", host, port);
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

    ln->send_buf_sz = _dtls_record_bufsz(ln->opts.mtu);
    ln->send_buf    = (char*)malloc(ln->send_buf_sz);
    if (!ln->send_buf) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln->send_buf);
        free(ln);
        return NULL;
    }

    _dtls_listener_ref(ln); /* caller's reference (released by close) */

    rbtree_init(&ln->sessions, _dtls_session_cmp_nn, _dtls_session_cmp_kn);
    ln->sessions_mu = xylem_mutex_create();
    ln->write_mu    = xylem_mutex_create();
    if (!ln->sessions_mu || !ln->write_mu) {
        _dtls_listener_unref(ln);
        return NULL;
    }

    ln->accept_ch = xylem_channel_create();
    if (!ln->accept_ch) {
        _dtls_listener_unref(ln);
        return NULL;
    }

    _dtls_listener_ref(ln); /* dispatcher's reference (released on exit) */
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

    xylem_mutex_lock(ln->sessions_mu);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_dtls_conn_t* dtls =
            rbtree_entry(node, xylem_dtls_conn_t, server_node);
        xylem_mutex_unlock(ln->sessions_mu);
        xylem_dtls_close(dtls);
        xylem_mutex_lock(ln->sessions_mu);
    }
    xylem_mutex_unlock(ln->sessions_mu);

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

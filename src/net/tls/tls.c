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
#include "net/tls/tls-backend.h"
#include "platform/platform-io.h"
#include "platform/platform-socket.h"
#include "platform/platform-string.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "thrds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Per-connection scratch (rbuf/wbuf) for moving ciphertext between the
 * backend state machine and the socket. Sized to the 16 KiB TLS record
 * cap so a full record moves per pump; the backend reassembles records
 * that span chunks.
 */
#define TLS_IO_CHUNK (16 * 1024)

static tls_conn_t* _tls_conn_create(platform_sock_t fd) {
    tls_conn_t* tls =
        (tls_conn_t*)calloc(1, sizeof(tls_conn_t));
    if (!tls) {
        return NULL;
    }

    tls->fd     = fd;
    tls->waiter = iowait_create(fd);
    tls->ssl_mu = xylem_mutex_create();
    tls->rd_mu  = xylem_mutex_create();
    tls->wr_mu  = xylem_mutex_create();
    tls->rbuf   = (char*)malloc(TLS_IO_CHUNK);
    tls->wbuf   = (char*)malloc(TLS_IO_CHUNK);
    if (!tls->waiter || !tls->ssl_mu || !tls->rd_mu || !tls->wr_mu
        || !tls->rbuf || !tls->wbuf) {
        if (tls->waiter) {
            iowait_destroy(tls->waiter);
        }
        xylem_mutex_destroy(tls->ssl_mu);
        xylem_mutex_destroy(tls->rd_mu);
        xylem_mutex_destroy(tls->wr_mu);
        free(tls->rbuf);
        free(tls->wbuf);
        free(tls);
        return NULL;
    }

    atomic_store_explicit(&tls->refcnt, 1, memory_order_relaxed);
    return tls;
}

static void _tls_conn_ref(tls_conn_t* tls) {
    atomic_fetch_add_explicit(&tls->refcnt, 1, memory_order_relaxed);
}

/**
 * Convert a timeout in milliseconds to an absolute deadline. Returns 0
 * (no deadline) when timeout_ms is 0, matching the iowait convention.
 */
static uint64_t _tls_make_deadline(uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    return xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
}

/* Apply the same deadline to both iowait directions (0 clears it). */
static void _tls_set_deadline(tls_conn_t* tls, uint64_t deadline) {
    iowait_set_rd_deadline(tls->waiter, deadline);
    iowait_set_wr_deadline(tls->waiter, deadline);
}

/**
 * Release every resource except the backend connection, which the
 * callers tear down differently (graceful shutdown vs. plain free)
 * before delegating here. Frees tls itself.
 */
static void _tls_conn_free(tls_conn_t* tls) {
    if (tls->waiter) {
        /**
         * Disarm any in-flight deadline timer before teardown. iowait
         * close/destroy do not cancel timers, and an armed timer holds
         * an iowait reference -- without this the waiter would linger
         * until the timer fires (e.g. a failed handshake would keep the
         * connection alive for the whole handshake timeout).
         */
        _tls_set_deadline(tls, 0);
        iowait_destroy(tls->waiter);
    }
    if (tls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(tls->fd, PLATFORM_SHUT_WR);
        platform_socket_close(tls->fd);
    }
    xylem_mutex_destroy(tls->ssl_mu);
    xylem_mutex_destroy(tls->rd_mu);
    xylem_mutex_destroy(tls->wr_mu);
    free(tls->rbuf);
    free(tls->wbuf);
    free(tls);
}

static void _tls_conn_unref(tls_conn_t* tls) {
    if (atomic_fetch_sub_explicit(&tls->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (tls->be) {
        tls_backend_conn_shutdown(tls->be);
        tls_backend_conn_destroy(tls->be);
    }
    _tls_conn_free(tls);
}

static void _tls_conn_destroy(tls_conn_t* tls) {
    if (tls->be) {
        tls_backend_conn_destroy(tls->be);
    }
    _tls_conn_free(tls);
}

/**
 * Send exactly len bytes to the socket, parking on the iowait write
 * direction when the kernel buffer is full. Caller must hold wr_mu so
 * this is the sole parker on that direction. Returns 0 on success, -1
 * on socket error or close.
 */
static int _tls_send_all(tls_conn_t* tls, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        ssize_t n = platform_socket_send(tls->fd, buf + sent, len - sent);
        if (n > 0) {
            sent += (int)n;
            continue;
        }
        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            xylem_loge("<tls> send failed fd=%d err=%s",
                       (int)tls->fd, platform_socket_tostring(err));
            return -1;
        }
        if (iowait_write(tls->waiter) != IOWAIT_READY
            || atomic_load_explicit(&tls->closed, memory_order_acquire)) {
            return -1;
        }
    }
    return 0;
}

/**
 * Drain pending outbound ciphertext from the backend to the socket.
 * Holds wr_mu so it is the sole parker on the iowait write direction,
 * and takes ssl_mu only for the drain itself -- never across a socket
 * park -- so a concurrent reader can still touch the backend state.
 * Returns 0 once the backend is empty, -1 on socket error or close.
 */
static int _tls_pump_out(tls_conn_t* tls) {
    int ret = 0;

    xylem_mutex_lock(tls->wr_mu);
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        int n = tls_backend_conn_drain(tls->be, tls->wbuf, TLS_IO_CHUNK);
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
 * Read one chunk of inbound ciphertext from the socket into the backend.
 * Holds rd_mu so it is the sole parker on the iowait read direction, and
 * takes ssl_mu only for the feed -- never across a socket park. Returns
 * the byte count fed (>0), 0 on peer EOF, -1 on socket error or close.
 */
static int _tls_pump_in(tls_conn_t* tls) {
    int ret = -1;

    xylem_mutex_lock(tls->rd_mu);
    for (;;) {
        ssize_t n = platform_socket_recv(tls->fd, tls->rbuf, TLS_IO_CHUNK);
        if (n > 0) {
            xylem_mutex_lock(tls->ssl_mu);
            tls_backend_conn_feed(tls->be, tls->rbuf, (int)n);
            xylem_mutex_unlock(tls->ssl_mu);
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
            xylem_loge("<tls> recv failed fd=%d err=%s",
                       (int)tls->fd, platform_socket_tostring(err));
            ret = -1;
            break;
        }
        iowait_result_t r = iowait_read(tls->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&tls->closed, memory_order_acquire)) {
            ret = -1;
            break;
        }
    }
    xylem_mutex_unlock(tls->rd_mu);
    return ret;
}

static int _tls_do_handshake(tls_conn_t* tls) {
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_handshake(tls->be);
        xylem_mutex_unlock(tls->ssl_mu);

        if (_tls_pump_out(tls) != 0) {
            return -1;
        }
        if (st == TLS_BACKEND_OK) {
            return 0;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_pump_in(tls) <= 0) {
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
    if (ln->waiter) {
        iowait_destroy(ln->waiter);
    }
    if (ln->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(ln->fd);
    }
    free(ln);
}

/**
 * Resolve host to a numeric dial target. A numeric literal is used
 * as-is; otherwise the name is resolved via DNS and the first result is
 * written to ip_buf. On return dial_host points at host or ip_buf, and
 * out_addr holds the chosen peer address. Returns 0 on success, -1 on
 * resolution failure.
 */
static int _tls_resolve(const char* host, uint16_t port, char* ip_buf,
                        size_t ip_buf_len, const char** dial_host,
                        addr_t* out_addr) {
    if (addr_pton(host, port, out_addr) == 0) {
        *dial_host = host;
        return 0;
    }

    addr_t* addrs = NULL;
    size_t  count = 0;
    if (addr_resolve(host, port, &addrs, &count) != 0 || count == 0) {
        xylem_loge("<tls> dial dns failed host=%s", host);
        return -1;
    }

    *out_addr = addrs[0];
    free(addrs);

    uint16_t rport;
    addr_ntop(out_addr, ip_buf, ip_buf_len, &rport);
    *dial_host = ip_buf;
    return 0;
}

/**
 * Wait for a non-blocking connect to finish and surface any pending
 * socket error. The caller arms the deadline before calling. Returns 0
 * once the socket is writable with no error, -1 on timeout or connect
 * failure.
 */
static int _tls_wait_connect(tls_conn_t* tls) {
    if (iowait_write(tls->waiter) != IOWAIT_READY) {
        return -1;
    }

    int32_t   err    = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(tls->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
    if (err != 0) {
        xylem_loge("<tls> dial connect failed fd=%d err=%d (%s)",
                   (int)tls->fd, err, platform_socket_tostring(err));
        return -1;
    }
    return 0;
}

/**
 * Drive the backend read to completion, pumping ciphertext to/from the
 * socket as the backend state machine demands. Returns bytes read (>0),
 * 0 on clean peer shutdown, or -1 on error/close.
 */
static int _tls_read_loop(tls_conn_t* tls, void* buf, int len) {
    for (;;) {
        int n = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_read(tls->be, buf, len, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (st == TLS_BACKEND_OK) {
            return n;
        }
        if (st == TLS_BACKEND_CLOSED) {
            return 0;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

/**
 * Drive the backend write of the whole buffer to completion, flushing
 * the ciphertext produced after each accepted chunk. Returns 0 once all
 * len bytes are written and flushed, -1 on error/close.
 */
static int _tls_write_loop(tls_conn_t* tls, const void* data,
                           int len) {
    const char* ptr = (const char*)data;
    int         rem = len;

    while (rem > 0) {
        int n = 0;
        xylem_mutex_lock(tls->ssl_mu);
        tls_backend_state_t st = tls_backend_conn_write(tls->be, ptr, rem, &n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (st == TLS_BACKEND_OK) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            ptr += n;
            rem -= n;
            continue;
        }
        if (st == TLS_BACKEND_WANT_WRITE) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            continue;
        }
        if (st == TLS_BACKEND_WANT_READ) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}

/**
 * Pull one connection off the listen socket, parking on the iowait read
 * direction while none is pending and backing off on transient accept
 * errors. Returns a connected fd, or PLATFORM_SO_ERROR_INVALID_SOCKET
 * when the listener is closing or accept keeps failing.
 */
static platform_sock_t _tls_accept_fd(tls_listener_t* ln) {
    uint64_t backoff_ms = 5;
    int      retries    = 0;

    while (!atomic_load_explicit(&ln->closed, memory_order_acquire)) {
        platform_sock_t fd = platform_socket_accept(ln->fd, true);
        if (fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
            return fd;
        }

        int err = platform_socket_get_lasterror();
        if (err == PLATFORM_SO_ERROR_EAGAIN
            || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
            if (iowait_read(ln->waiter) != IOWAIT_READY) {
                break;
            }
            continue;
        }

        xylem_loge("<tls> accept failed fd=%d err=%d (%s)",
                   (int)ln->fd, err, platform_socket_tostring(err));
        if (++retries > 8) {
            break;
        }
        runtime_sleep(backoff_ms);
        if (backoff_ms < 1000) {
            backoff_ms *= 2;
        }
    }
    return PLATFORM_SO_ERROR_INVALID_SOCKET;
}

/**
 * Drive the server-side handshake on an accepted connection. Takes
 * ownership of tls and returns the ready connection, or NULL on a
 * per-connection failure (tls is destroyed). A NULL return is never a
 * listener-level error -- the caller keeps accepting.
 */
static tls_conn_t* _tls_server_handshake(tls_listener_t* ln,
                                               tls_conn_t* tls) {
    tls->ctx = ln->ctx;

    socklen_t peer_len = sizeof(tls->peer_addr.storage);
    getpeername(tls->fd, (struct sockaddr*)&tls->peer_addr.storage,
                &peer_len);

    tls->be = tls_backend_conn_create(ln->ctx->be, true);
    if (!tls->be) {
        xylem_loge("<tls> accept ssl init failed");
        _tls_conn_destroy(tls);
        return NULL;
    }
    tls_backend_handshake_cfg_t cfg = {0};
    cfg.verify = ln->ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                        : TLS_BACKEND_VERIFY_NONE;
    tls_backend_conn_configure(tls->be, &cfg);

    /* Arm the handshake deadline; disarm on success. */
    _tls_set_deadline(tls, _tls_make_deadline(ln->opts.handshake_timeout_ms));

    if (_tls_do_handshake(tls) != 0) {
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    tls_backend_conn_get_alpn(tls->be, tls->alpn, sizeof(tls->alpn));
    return tls;
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

int tls_ctx_load_cert(tls_ctx_t* ctx,
                            const char* hostname,
                            const char* cert,
                            const char* key) {
    return tls_backend_ctx_load_cert_file(ctx->be, hostname, cert, key);
}

int tls_ctx_load_cert_mem(tls_ctx_t* ctx,
                                const char* hostname,
                                const void* cert_pem,
                                size_t cert_len,
                                const void* key_pem,
                                size_t key_len) {
    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return -1;
    }
    return tls_backend_ctx_load_cert_mem(ctx->be, hostname, cert_pem, cert_len,
                                         key_pem, key_len);
}

int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file) {
    return tls_backend_ctx_load_ca_file(ctx->be, ca_file);
}

int tls_ctx_load_system_ca(tls_ctx_t* ctx) {
    return tls_backend_ctx_load_system_ca(ctx->be);
}

void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable) {
    ctx->verify_server = enable;
}

void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable) {
    ctx->verify_client = enable;
}

int tls_ctx_set_alpn(tls_ctx_t* ctx,
                           const char** protocols, size_t count) {
    return tls_backend_ctx_set_alpn(ctx->be, protocols, count);
}

tls_conn_t* tls_dial(
    const char*       host,
    uint16_t          port,
    tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = NULL;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;
    if (_tls_resolve(host, port, resolved_ip, sizeof(resolved_ip),
                     &dial_host, &resolved_addr) != 0) {
        return NULL;
    }

    bool            connected = false;
    platform_sock_t fd        = platform_socket_dial(
        dial_host, port_str, SOCK_STREAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<tls> dial socket failed host=%s port=%s",
                   host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    tls_conn_t* tls = _tls_conn_create(fd);
    if (!tls) {
        platform_socket_close(fd);
        return NULL;
    }

    tls->ctx       = ctx;
    tls->peer_addr = resolved_addr;

    uint64_t connect_ms = opts ? opts->handshake_timeout_ms : 0;

    /* Arm the deadline once; it bounds both connect and handshake. */
    _tls_set_deadline(tls, _tls_make_deadline(connect_ms));

    if (!connected && _tls_wait_connect(tls) != 0) {
        _tls_conn_destroy(tls);
        return NULL;
    }

    if (_tls_client_handshake(tls, opts ? opts->server_name : NULL) != 0) {
        xylem_loge("<tls> dial handshake failed host=%s port=%s", host, port_str);
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

    iowait_close(tls->waiter);
    _tls_conn_unref(tls);
}

tls_listener_t* tls_listen(
    const char*       host,
    uint16_t          port,
    tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd
        = platform_socket_listen(host, port_str, SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<tls> listen failed host=%s port=%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    tls_listener_t* ln = (tls_listener_t*)calloc(
        1, sizeof(tls_listener_t));
    if (!ln) {
        platform_socket_close(fd);
        return NULL;
    }

    ln->fd  = fd;
    ln->ctx = ctx;
    if (opts) {
        ln->opts = *opts;
    }

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    _tls_listener_ref(ln);
    return ln;
}

tls_conn_t* tls_accept(tls_listener_t* ln) {
    _tls_listener_ref(ln);

    tls_conn_t* conn = NULL;
    for (;;) {
        platform_sock_t fd = _tls_accept_fd(ln);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            break;
        }

        tls_conn_t* tls = _tls_conn_create(fd);
        if (!tls) {
            platform_socket_close(fd);
            break;
        }

        /**
         * A failed handshake (bad cert, protocol mismatch, peer
         * disconnect, or handshake timeout) is per-connection and must
         * NOT tear down the listener: callers such as the HTTPS accept
         * coroutine treat a NULL return as "listener dead" and stop
         * accepting, so a single bad client would otherwise DoS the
         * whole server. Drop it and keep accepting; _tls_accept_fd's
         * closed check breaks the loop on real shutdown.
         */
        conn = _tls_server_handshake(ln, tls);
        if (conn) {
            break;
        }
    }

    _tls_listener_unref(ln);
    return conn;
}

void tls_close_listener(tls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    iowait_close(ln->waiter);
    _tls_listener_unref(ln);
}

int tls_read(tls_conn_t* tls, void* buf, int len) {
    /**
     * Take the reference before testing `closed`: a concurrent
     * tls_close() on another thread may drop the last reference
     * and free tls in the window between the test and the ref,
     * otherwise. Holding a ref first caps a racing close at refcnt 2->1
     * (no free); our own unref does the final teardown.
     */
    _tls_conn_ref(tls);
    int ret = -1;
    if (!atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        ret = _tls_read_loop(tls, buf, len);
    }
    _tls_conn_unref(tls);
    return ret;
}

int tls_write(tls_conn_t* tls, const void* data, int len) {
    _tls_conn_ref(tls);
    int ret = -1;
    if (!atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        ret = _tls_write_loop(tls, data, len);
    }
    _tls_conn_unref(tls);
    return ret;
}

void tls_set_read_deadline(
    tls_conn_t* tls, uint64_t deadline_ms) {
    iowait_set_rd_deadline(tls->waiter, deadline_ms);
}

void tls_set_write_deadline(
    tls_conn_t* tls, uint64_t deadline_ms) {
    iowait_set_wr_deadline(tls->waiter, deadline_ms);
}

int tls_remote_addr(
    tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    return addr_ntop(&tls->peer_addr, host, host_len, port);
}

int tls_local_addr(
    tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    addr_t addr;
    socklen_t alen = sizeof(addr.storage);
    if (getsockname(tls->fd, (struct sockaddr*)&addr.storage, &alen) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int tls_listener_addr(
    tls_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port) {
    addr_t addr;
    socklen_t alen = sizeof(addr.storage);
    if (getsockname(ln->fd, (struct sockaddr*)&addr.storage, &alen) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

const char* tls_get_alpn(tls_conn_t* tls) {
    return tls->alpn[0] ? tls->alpn : NULL;
}

tls_conn_t* tls_client_handshake_fd(platform_sock_t fd,
                                          tls_ctx_t* ctx,
                                          xylem_tls_opts_t* opts) {
    tls_conn_t* tls = _tls_conn_create(fd);
    if (!tls) {
        platform_socket_close(fd);
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

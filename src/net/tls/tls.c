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
#include "platform/platform-io.h"
#include "platform/platform-socket.h"
#include "platform/platform-string.h"
#include "platform/platform-tls.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "thrds.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Per-connection scratch (rbuf/wbuf) for moving ciphertext between the
 * memory BIOs and the socket. Sized to the 16 KiB TLS record cap so a
 * full record moves per pump; SSL reassembles records that span chunks.
 */
#define TLS_IO_CHUNK (16 * 1024)

static int _tls_ex_data_idx = -1;
static once_flag _tls_ex_data_once = ONCE_FLAG_INIT;

static void _tls_init_ex_data(void) {
    _tls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

/**
 * SNI servername callback. Selects the per-host certificate by setting
 * it directly on the SSL connection -- the single ctx (and thus its
 * keylog / ALPN / verify config) stays in force. Mirrors Go's
 * GetCertificate / rustls' cert resolver: SNI only picks a cert, never
 * swaps the whole configuration. When no host matches, the ctx default
 * certificate is left untouched.
 */
static int _tls_ctx_sni_cb(SSL* ssl, int* al, void* arg) {
    (void)al;
    tls_ctx_t* ctx = (tls_ctx_t*)arg;
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name) {
        return SSL_TLSEXT_ERR_OK;
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        _tls_sni_entry_t* e = &ctx->sni_entries[i];
        if (platform_strcasecmp(name, e->hostname) != 0) {
            continue;
        }
        /**
         * use/set1 variants bump the refcount on the stored objects,
         * so the ctx keeps ownership and each connection holds its own
         * reference. Validated key/cert pairing at load time.
         */
        if (SSL_use_certificate(ssl, e->cert) != 1
            || SSL_use_PrivateKey(ssl, e->key) != 1) {
            xylem_loge("<tls> sni apply cert failed host=%s", e->hostname);
            return SSL_TLSEXT_ERR_OK;
        }
        if (e->chain) {
            SSL_set1_chain(ssl, e->chain);
        }
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_OK;
}

static void _tls_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    tls_ctx_t* ctx =
        (tls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _tls_ex_data_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _tls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                               unsigned char* outlen,
                               const unsigned char* in,
                               unsigned int inlen, void* arg) {
    tls_ctx_t* ctx = (tls_ctx_t*)arg;
    (void)ssl;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

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

/**
 * Bind a fresh SSL to a pair of memory BIOs. Network I/O is driven
 * separately via _tls_pump_in / _tls_pump_out so SSL_read and SSL_write
 * never touch the socket directly; this decouples the SSL state machine
 * from the read/write parking directions. On success SSL owns both BIOs
 * and frees them in SSL_free.
 */
static int _tls_init_ssl(tls_conn_t* tls, SSL_CTX* ssl_ctx) {
    tls->ssl = SSL_new(ssl_ctx);
    if (!tls->ssl) {
        xylem_loge("<tls> SSL_new failed");
        return -1;
    }
    tls->rbio = BIO_new(BIO_s_mem());
    tls->wbio = BIO_new(BIO_s_mem());
    if (!tls->rbio || !tls->wbio) {
        BIO_free(tls->rbio);
        BIO_free(tls->wbio);
        SSL_free(tls->ssl);
        tls->ssl  = NULL;
        tls->rbio = NULL;
        tls->wbio = NULL;
        return -1;
    }
    SSL_set_bio(tls->ssl, tls->rbio, tls->wbio);
    return 0;
}

/**
 * Apply the connection's verify policy by role. Set per SSL (not on the
 * shared SSL_CTX) so a single ctx reused as both client and server keeps
 * the correct, opposite policy for each role:
 *   - client: verify the server cert unless ctx->verify_server is off.
 *   - server: request and require a client cert (mTLS) only when
 *     ctx->verify_client is on; otherwise ask for none.
 */
static void _tls_apply_verify(tls_conn_t* tls, bool is_server) {
    int mode;
    if (is_server) {
        mode = tls->ctx->verify_client
                   ? (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
                   : SSL_VERIFY_NONE;
    } else {
        mode = tls->ctx->verify_server ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    }
    SSL_set_verify(tls->ssl, mode, NULL);
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
 * Release every resource except the SSL object, which the callers tear
 * down differently (graceful shutdown vs. plain free) before delegating
 * here. Frees tls itself.
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
    if (tls->ssl) {
        ERR_clear_error();
        SSL_shutdown(tls->ssl);
        SSL_free(tls->ssl);
    }
    _tls_conn_free(tls);
}

static void _tls_conn_destroy(tls_conn_t* tls) {
    if (tls->ssl) {
        SSL_free(tls->ssl);
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
 * Drain pending outbound ciphertext from wbio to the socket. Holds
 * wr_mu so it is the sole parker on the iowait write direction, and
 * takes ssl_mu only for the BIO_read itself -- never across a socket
 * park -- so a concurrent reader can still touch the SSL state. Returns
 * 0 once wbio is empty, -1 on socket error or close.
 */
static int _tls_pump_out(tls_conn_t* tls) {
    int ret = 0;

    xylem_mutex_lock(tls->wr_mu);
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        int n = BIO_read(tls->wbio, tls->wbuf, TLS_IO_CHUNK);
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
 * Read one chunk of inbound ciphertext from the socket into rbio. Holds
 * rd_mu so it is the sole parker on the iowait read direction, and takes
 * ssl_mu only for the BIO_write -- never across a socket park. Returns
 * the byte count fed (>0), 0 on peer EOF, -1 on socket error or close.
 */
static int _tls_pump_in(tls_conn_t* tls) {
    int ret = -1;

    xylem_mutex_lock(tls->rd_mu);
    for (;;) {
        ssize_t n = platform_socket_recv(tls->fd, tls->rbuf, TLS_IO_CHUNK);
        if (n > 0) {
            xylem_mutex_lock(tls->ssl_mu);
            BIO_write(tls->rbio, tls->rbuf, (int)n);
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
        ERR_clear_error();
        int ret = SSL_do_handshake(tls->ssl);
        int err = (ret == 1) ? 0 : SSL_get_error(tls->ssl, ret);
        xylem_mutex_unlock(tls->ssl_mu);

        /* Flush any handshake records produced before waiting on input. */
        if (_tls_pump_out(tls) != 0) {
            return -1;
        }

        if (ret == 1) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            continue;
        }

        unsigned long e = ERR_peek_error();
        xylem_loge("<tls> handshake failed ssl_err=%d reason=%s",
                   err,
                   ERR_reason_error_string(e)
                       ? ERR_reason_error_string(e)
                       : "unknown");
        return -1;
    }
}

static void _tls_apply_server_name(SSL* ssl, const char* server_name) {
    int verify_peer = SSL_get_verify_mode(ssl) & SSL_VERIFY_PEER;

    if (!server_name && verify_peer) {
        xylem_loge("<tls> dial server_name=NULL with verify_peer; "
                   "peer identity unchecked (MITM risk)");
    }
    if (!server_name) {
        return;
    }

    /* RFC 6066 forbids IP literals in SNI. */
    addr_t tmp;
    if (addr_pton(server_name, 0, &tmp) != 0) {
        SSL_set_tlsext_host_name(ssl, server_name);
    }
    if (verify_peer) {
        SSL_set1_host(ssl, server_name);
    }
}

static void _tls_cache_alpn(tls_conn_t* tls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(tls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(tls->alpn)) {
        memcpy(tls->alpn, alpn_proto, alpn_len);
        tls->alpn[alpn_len] = '\0';
    }
}

static int _tls_client_handshake(tls_conn_t* tls, SSL_CTX* ssl_ctx,
                                     const char* server_name) {
    if (_tls_init_ssl(tls, ssl_ctx) != 0) {
        return -1;
    }
    SSL_set_connect_state(tls->ssl);

    _tls_apply_verify(tls, false);

    _tls_apply_server_name(tls->ssl, server_name);

    if (_tls_do_handshake(tls) != 0) {
        return -1;
    }

    _tls_cache_alpn(tls);
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
 * Drive SSL_read to completion, pumping ciphertext to/from the socket as
 * the SSL state machine demands. Returns bytes read (>0), 0 on clean
 * peer shutdown, or -1 on error/close.
 */
static int _tls_read_loop(tls_conn_t* tls, void* buf, int len) {
    for (;;) {
        xylem_mutex_lock(tls->ssl_mu);
        ERR_clear_error();
        int n   = SSL_read(tls->ssl, buf, len);
        int err = (n > 0) ? 0 : SSL_get_error(tls->ssl, n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (n > 0) {
            return n;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            /**
             * SSL needs more ciphertext; fetch a chunk from the socket.
             * EOF (0) or error (-1) both end the read.
             */
            if (_tls_pump_in(tls) <= 0) {
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            /**
             * Post-handshake message (e.g. TLS 1.3 KeyUpdate) must be
             * flushed before SSL_read can proceed.
             */
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

/**
 * Drive SSL_write of the whole buffer to completion, flushing the
 * ciphertext produced after each accepted chunk. Returns 0 once all len
 * bytes are written and flushed, -1 on error/close.
 */
static int _tls_write_loop(tls_conn_t* tls, const void* data,
                           int len) {
    const char* ptr = (const char*)data;
    int         rem = len;

    while (rem > 0) {
        xylem_mutex_lock(tls->ssl_mu);
        ERR_clear_error();
        int n   = SSL_write(tls->ssl, ptr, rem);
        int err = (n > 0) ? 0 : SSL_get_error(tls->ssl, n);
        xylem_mutex_unlock(tls->ssl_mu);

        if (n > 0) {
            /* Flush the ciphertext SSL just buffered into wbio. */
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            ptr += n;
            rem -= n;
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            if (_tls_pump_out(tls) != 0) {
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            /**
             * Rare: renegotiation / KeyUpdate needs inbound data before
             * the write can complete. Flush first, then feed one chunk
             * of ciphertext.
             */
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

    if (_tls_init_ssl(tls, ln->ctx->ssl_ctx) != 0) {
        xylem_loge("<tls> accept ssl init failed");
        _tls_conn_destroy(tls);
        return NULL;
    }
    SSL_set_accept_state(tls->ssl);

    _tls_apply_verify(tls, true);

    /* Arm the handshake deadline; disarm on success. */
    _tls_set_deadline(tls, _tls_make_deadline(ln->opts.handshake_timeout_ms));

    if (_tls_do_handshake(tls) != 0) {
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    _tls_cache_alpn(tls);
    return tls;
}

tls_ctx_t* tls_ctx_create(void) {
    tls_ctx_t* ctx =
        (tls_ctx_t*)calloc(1, sizeof(tls_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /**
     * Verification is applied per connection by role at handshake time
     * (see _tls_client_handshake / _tls_server_handshake), not on the
     * shared SSL_CTX, so one ctx reused as both client and server keeps
     * the correct policy for each. Defaults: verify the server (secure
     * client), do not request a client cert (plain server).
     */
    ctx->verify_server = true;
    ctx->verify_client = false;
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    /**
     * Install the SNI callback once at ctx creation. The callback is a
     * no-op until SNI entries are added (it returns early on an empty
     * table), so registering it unconditionally is safe and keeps all
     * ctx-level config in one place. Doing it here also means the
     * SSL_CTX stays read-only once connections start.
     */
    SSL_CTX_set_tlsext_servername_callback(ctx->ssl_ctx, _tls_ctx_sni_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx->ssl_ctx, ctx);

    call_once(&_tls_ex_data_once, _tls_init_ex_data);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _tls_ex_data_idx, ctx);

    return ctx;
}

void tls_ctx_destroy(tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        X509_free(ctx->sni_entries[i].cert);
        EVP_PKEY_free(ctx->sni_entries[i].key);
        sk_X509_pop_free(ctx->sni_entries[i].chain, X509_free);
    }
    free(ctx->sni_entries);
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}

int tls_ctx_set_keylog(tls_ctx_t* ctx, const char* path) {
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
    ctx->keylog_file = platform_io_fopen(path, "a");
    if (!ctx->keylog_file) {
        return -1;
    }
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tls_keylog_cb);
    return 0;
}

/**
 * Parse a TLS identity (leaf cert + intermediate chain + matching key)
 * from two already-open PEM BIOs. The cert BIO holds the leaf first,
 * any following certs form the chain. The BIOs are not freed here. On
 * success out_* are set (caller owns them); on failure nothing is left
 * allocated to the caller.
 */
static int _tls_parse_pem_identity(BIO* cbio, BIO* kbio,
                                   X509** out_cert, EVP_PKEY** out_key,
                                   STACK_OF(X509)** out_chain) {
    *out_cert  = NULL;
    *out_key   = NULL;
    *out_chain = NULL;

    X509* leaf = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    if (!leaf) {
        xylem_loge("<tls> parse leaf cert failed");
        return -1;
    }

    STACK_OF(X509)* chain = NULL;
    for (;;) {
        X509* extra = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
        if (!extra) {
            /* EOF is the only expected stop; clear the residual error. */
            ERR_clear_error();
            break;
        }
        if (!chain) {
            chain = sk_X509_new_null();
            if (!chain) {
                X509_free(extra);
                X509_free(leaf);
                return -1;
            }
        }
        if (sk_X509_push(chain, extra) <= 0) {
            X509_free(extra);
            sk_X509_pop_free(chain, X509_free);
            X509_free(leaf);
            return -1;
        }
    }

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(kbio, NULL, NULL, NULL);
    if (!pkey) {
        xylem_loge("<tls> parse private key failed");
        sk_X509_pop_free(chain, X509_free);
        X509_free(leaf);
        return -1;
    }

    /* Reject a mismatched cert/key pair up front, not mid-handshake. */
    if (X509_check_private_key(leaf, pkey) != 1) {
        xylem_loge("<tls> cert and key mismatch");
        EVP_PKEY_free(pkey);
        sk_X509_pop_free(chain, X509_free);
        X509_free(leaf);
        return -1;
    }

    *out_cert  = leaf;
    *out_key   = pkey;
    *out_chain = chain;
    return 0;
}

/* Load a TLS identity from PEM files. See _tls_parse_pem_identity. */
static int _tls_load_pem_identity(const char* cert_file,
                                  const char* key_file,
                                  X509** out_cert,
                                  EVP_PKEY** out_key,
                                  STACK_OF(X509)** out_chain) {
    BIO* cbio = BIO_new_file(cert_file, "r");
    if (!cbio) {
        xylem_loge("<tls> open cert failed path=%s", cert_file);
        return -1;
    }
    BIO* kbio = BIO_new_file(key_file, "r");
    if (!kbio) {
        xylem_loge("<tls> open key failed path=%s", key_file);
        BIO_free(cbio);
        return -1;
    }
    int rc = _tls_parse_pem_identity(cbio, kbio, out_cert, out_key, out_chain);
    BIO_free(cbio);
    BIO_free(kbio);
    return rc;
}

/* Load a TLS identity from in-memory PEM buffers. */
static int _tls_load_pem_identity_mem(const void* cert_pem, size_t cert_len,
                                      const void* key_pem, size_t key_len,
                                      X509** out_cert, EVP_PKEY** out_key,
                                      STACK_OF(X509)** out_chain) {
    BIO* cbio = BIO_new_mem_buf(cert_pem, (int)cert_len);
    BIO* kbio = BIO_new_mem_buf(key_pem, (int)key_len);
    if (!cbio || !kbio) {
        BIO_free(cbio);
        BIO_free(kbio);
        return -1;
    }
    int rc = _tls_parse_pem_identity(cbio, kbio, out_cert, out_key, out_chain);
    BIO_free(cbio);
    BIO_free(kbio);
    return rc;
}

/**
 * Take ownership of (leaf, key, chain) into a new SNI entry bound to
 * hostname. On allocation failure the identity is freed and -1 is
 * returned, so the caller never has to clean up on error.
 */
static int _tls_store_sni_identity(tls_ctx_t* ctx, const char* hostname,
                                   X509* leaf, EVP_PKEY* key,
                                   STACK_OF(X509)* chain) {
    if (ctx->sni_count == ctx->sni_cap) {
        size_t new_cap = ctx->sni_cap == 0 ? 4 : ctx->sni_cap * 2;
        _tls_sni_entry_t* entries = (_tls_sni_entry_t*)realloc(
            ctx->sni_entries, new_cap * sizeof(_tls_sni_entry_t));
        if (!entries) {
            EVP_PKEY_free(key);
            sk_X509_pop_free(chain, X509_free);
            X509_free(leaf);
            return -1;
        }
        ctx->sni_entries = entries;
        ctx->sni_cap     = new_cap;
    }

    _tls_sni_entry_t* entry = &ctx->sni_entries[ctx->sni_count];
    snprintf(entry->hostname, sizeof(entry->hostname), "%s", hostname);
    entry->cert  = leaf;
    entry->key   = key;
    entry->chain = chain;
    ctx->sni_count++;
    return 0;
}

/**
 * Install (leaf, key, chain) as the ctx default identity. use/set1 bump
 * the refcount on each object, so the caller keeps ownership of its own
 * references and must free them afterwards.
 */
static int _tls_apply_default_identity(tls_ctx_t* ctx, X509* leaf,
                                       EVP_PKEY* key, STACK_OF(X509)* chain) {
    if (SSL_CTX_use_certificate(ctx->ssl_ctx, leaf) != 1
        || SSL_CTX_use_PrivateKey(ctx->ssl_ctx, key) != 1) {
        return -1;
    }
    if (chain && SSL_CTX_set1_chain(ctx->ssl_ctx, chain) != 1) {
        return -1;
    }
    return 0;
}

/**
 * Install a parsed identity into the ctx: as the default identity when
 * hostname is NULL, or as an SNI-selected identity otherwise. Always
 * consumes (leaf, key, chain) -- it takes ownership for the SNI case and
 * frees its references for the default case (which only bumps refcounts).
 */
static int _tls_install_identity(tls_ctx_t* ctx, const char* hostname,
                                 X509* leaf, EVP_PKEY* key,
                                 STACK_OF(X509)* chain) {
    if (hostname) {
        return _tls_store_sni_identity(ctx, hostname, leaf, key, chain);
    }
    int rc = _tls_apply_default_identity(ctx, leaf, key, chain);
    EVP_PKEY_free(key);
    sk_X509_pop_free(chain, X509_free);
    X509_free(leaf);
    return rc;
}

int tls_ctx_load_cert(tls_ctx_t* ctx,
                            const char* hostname,
                            const char* cert,
                            const char* key) {
    X509*           leaf  = NULL;
    EVP_PKEY*       pkey  = NULL;
    STACK_OF(X509)* chain = NULL;
    if (_tls_load_pem_identity(cert, key, &leaf, &pkey, &chain) != 0) {
        return -1;
    }
    return _tls_install_identity(ctx, hostname, leaf, pkey, chain);
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
    X509*           leaf  = NULL;
    EVP_PKEY*       pkey  = NULL;
    STACK_OF(X509)* chain = NULL;
    if (_tls_load_pem_identity_mem(cert_pem, cert_len, key_pem, key_len,
                                   &leaf, &pkey, &chain) != 0) {
        return -1;
    }
    return _tls_install_identity(ctx, hostname, leaf, pkey, chain);
}

int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("<tls> load ca failed path=%s", ca_file);
        return -1;
    }
    return 0;
}

int tls_ctx_load_system_ca(tls_ctx_t* ctx) {
    if (platform_tls_load_system_ca(ctx->ssl_ctx) != 0) {
        xylem_loge("<tls> load system ca failed");
        return -1;
    }
    return 0;
}

void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable) {
    ctx->verify_server = enable;
}

void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable) {
    ctx->verify_client = enable;
}

int tls_ctx_set_alpn(tls_ctx_t* ctx,
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

    /**
     * One list serves both roles: the client offers it, the server uses
     * the select cb to pick from it. The unused half is inert per role.
     */
    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _tls_alpn_select_cb, ctx);

    return 0;
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

    if (_tls_client_handshake(tls, ctx->ssl_ctx,
                                  opts ? opts->server_name : NULL) != 0) {
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

    if (_tls_client_handshake(tls, ctx->ssl_ctx,
                                  opts ? opts->server_name : NULL) != 0) {
        xylem_loge("<tls> client handshake failed");
        _tls_conn_destroy(tls);
        return NULL;
    }

    _tls_set_deadline(tls, 0);
    return tls;
}

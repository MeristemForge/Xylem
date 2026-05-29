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

#include "net/addr.h"

#include <openssl/err.h>
#include <stdlib.h>

xylem_tls_conn_t* tls_conn_create(platform_sock_t fd) {
    xylem_tls_conn_t* tls =
        (xylem_tls_conn_t*)calloc(1, sizeof(xylem_tls_conn_t));
    if (!tls) {
        return NULL;
    }

    tls->fd     = fd;
    tls->waiter = iowait_create(fd);
    if (!tls->waiter) {
        free(tls);
        return NULL;
    }

    atomic_store_explicit(&tls->refcnt, 1, memory_order_relaxed);
    return tls;
}

void tls_conn_ref(xylem_tls_conn_t* tls) {
    atomic_fetch_add_explicit(&tls->refcnt, 1, memory_order_relaxed);
}

void tls_conn_unref(xylem_tls_conn_t* tls) {
    if (atomic_fetch_sub_explicit(&tls->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (tls->ssl) {
        ERR_clear_error();
        SSL_shutdown(tls->ssl);
        SSL_free(tls->ssl);
    }
    if (tls->waiter) {
        iowait_destroy(tls->waiter);
    }
    if (tls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(tls->fd, PLATFORM_SHUT_WR);
        platform_socket_close(tls->fd);
    }
    free(tls);
}

void tls_conn_destroy(xylem_tls_conn_t* tls) {
    if (tls->ssl) {
        SSL_free(tls->ssl);
    }
    if (tls->waiter) {
        iowait_destroy(tls->waiter);
    }
    if (tls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(tls->fd, PLATFORM_SHUT_WR);
        platform_socket_close(tls->fd);
    }
    free(tls);
}

int tls_do_handshake(xylem_tls_conn_t* tls) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(tls->ssl);
        if (ret == 1) {
            return 0;
        }

        int err = SSL_get_error(tls->ssl, ret);
        switch (err) {
        case SSL_ERROR_WANT_READ: {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
            break;
        }
        case SSL_ERROR_WANT_WRITE: {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
            break;
        }
        default: {
            unsigned long ssl_err = ERR_peek_error();
            xylem_loge("tls handshake: ssl_error=%d reason=%s",
                       err,
                       ERR_reason_error_string(ssl_err)
                           ? ERR_reason_error_string(ssl_err)
                           : "unknown");
            return -1;
        }
        }
    }
}

void tls_apply_server_name(SSL* ssl, const char* server_name) {
    if (server_name) {
        /* RFC 6066 forbids IP literals in SNI. */
        addr_t tmp;
        if (addr_pton(server_name, 0, &tmp) != 0) {
            SSL_set_tlsext_host_name(ssl, server_name);
        }
        if (SSL_get_verify_mode(ssl) & SSL_VERIFY_PEER) {
            SSL_set1_host(ssl, server_name);
        }
    } else if (SSL_get_verify_mode(ssl) & SSL_VERIFY_PEER) {
        xylem_logw("tls dial: verify_peer enabled but server_name "
                   "is NULL; peer identity is not checked (MITM risk)");
    }
}

void tls_cache_alpn(xylem_tls_conn_t* tls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(tls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(tls->alpn)) {
        memcpy(tls->alpn, alpn_proto, alpn_len);
        tls->alpn[alpn_len] = '\0';
    }
}

int tls_handle_io_block(xylem_tls_conn_t* tls, int ssl_err,
                        const char* op_name) {
    iowait_result_t r;
    switch (ssl_err) {
    case SSL_ERROR_ZERO_RETURN:
        return 1;
    case SSL_ERROR_WANT_READ:
        r = iowait_read(tls->waiter);
        break;
    case SSL_ERROR_WANT_WRITE:
        r = iowait_write(tls->waiter);
        break;
    default: {
        unsigned long e = ERR_peek_error();
        xylem_loge("tls %s: ssl_error=%d reason=%s",
                   op_name, ssl_err,
                   ERR_reason_error_string(e)
                       ? ERR_reason_error_string(e) : "unknown");
        return -1;
    }
    }
    if (r != IOWAIT_READY
        || atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        return -1;
    }
    return 0;
}

xylem_tls_conn_t* tls_handshake(platform_sock_t fd,
                                 xylem_tls_ctx_t* ctx,
                                 xylem_tls_opts_t* opts) {
    xylem_tls_conn_t* tls = tls_conn_create(fd);
    if (!tls) {
        platform_socket_close(fd);
        return NULL;
    }
    tls->ctx = ctx;

    uint64_t deadline = 0;
    if (opts && opts->handshake_timeout_ms > 0) {
        deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                   + opts->handshake_timeout_ms;
        iowait_set_rd_deadline(tls->waiter, deadline);
        iowait_set_wr_deadline(tls->waiter, deadline);
    }

    tls->ssl = SSL_new(ctx->ssl_ctx);
    if (!tls->ssl) {
        xylem_loge("tls dial_conn: SSL_new failed");
        tls_conn_destroy(tls);
        return NULL;
    }
    SSL_set_fd(tls->ssl, (int)fd);
    SSL_set_connect_state(tls->ssl);

    tls_apply_server_name(tls->ssl, opts ? opts->server_name : NULL);

    if (tls_do_handshake(tls) != 0) {
        xylem_loge("tls dial_conn: handshake failed");
        tls_conn_destroy(tls);
        return NULL;
    }

    iowait_set_rd_deadline(tls->waiter, 0);
    iowait_set_wr_deadline(tls->waiter, 0);

    tls_cache_alpn(tls);
    return tls;
}

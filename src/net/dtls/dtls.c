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

#include "dtls.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include <openssl/err.h>
#include <stdlib.h>
#include <string.h>

void dtls_conn_ref(xylem_dtls_conn_t* dtls) {
    atomic_fetch_add_explicit(&dtls->refcnt, 1, memory_order_relaxed);
}

void dtls_conn_unref(xylem_dtls_conn_t* dtls) {
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

void dtls_listener_ref(xylem_dtls_listener_t* ln) {
    atomic_fetch_add_explicit(&ln->refcnt, 1, memory_order_relaxed);
}

void dtls_listener_unref(xylem_dtls_listener_t* ln) {
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
void dtls_inbox_push(xylem_dtls_conn_t* dtls, _dtls_dgram_t* dgram) {
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

xylem_dtls_conn_t* dtls_find_session(xylem_dtls_listener_t* ln,
                                      addr_t* addr) {
    rbtree_node_t* node = rbtree_find(&ln->sessions, addr);
    if (!node) {
        return NULL;
    }
    return rbtree_entry(node, xylem_dtls_conn_t, server_node);
}

void dtls_sessions_init(rbtree_t* tree) {
    rbtree_init(tree, _dtls_session_cmp_nn, _dtls_session_cmp_kn);
}

void dtls_server_flush_write_bio(xylem_dtls_conn_t* dtls) {
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

int dtls_server_send_record(xylem_dtls_conn_t* dtls,
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

    char buf[DTLS_PKT_BUF_SIZE];
    int  rd;
    while ((rd = BIO_read(dtls->write_bio, buf, sizeof(buf))) > 0) {
        for (;;) {
            ssize_t sent = platform_socket_sendto(
                dtls->listener->fd, buf, rd,
                &dtls->peer_addr.storage, addrlen);
            if (sent >= 0) {
                break;
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
    return 0;
}

int dtls_init_ssl(xylem_dtls_conn_t* dtls, SSL_CTX* ssl_ctx) {
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

int dtls_handle_io_block(xylem_dtls_conn_t* dtls, int ssl_err,
                         const char* op_name) {
    iowait_result_t r;
    switch (ssl_err) {
    case SSL_ERROR_ZERO_RETURN:
        return 1;
    case SSL_ERROR_WANT_READ:
        r = iowait_read(dtls->waiter);
        break;
    case SSL_ERROR_WANT_WRITE:
        r = iowait_write(dtls->waiter);
        break;
    default: {
        unsigned long e = ERR_peek_error();
        xylem_loge("dtls %s: ssl_error=%d reason=%s",
                   op_name, ssl_err,
                   ERR_reason_error_string(e)
                       ? ERR_reason_error_string(e) : "unknown");
        return -1;
    }
    }
    if (r != IOWAIT_READY
        || atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        return -1;
    }
    return 0;
}

int dtls_client_do_handshake(xylem_dtls_conn_t* dtls, uint64_t deadline) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) {
            return 0;
        }

        switch (SSL_get_error(dtls->ssl, ret)) {
        case SSL_ERROR_WANT_READ: {
            uint64_t rd_dl = deadline;
            struct timeval tv;
            if (DTLSv1_get_timeout(dtls->ssl, &tv)) {
                uint64_t ms = (uint64_t)tv.tv_sec * 1000
                            + (uint64_t)tv.tv_usec / 1000;
                if (ms == 0) {
                    ms = 1;
                }
                uint64_t now =
                    xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                uint64_t rt_dl = now + ms;
                if (rd_dl == 0 || rt_dl < rd_dl) {
                    rd_dl = rt_dl;
                }
            }
            iowait_set_rd_deadline(dtls->waiter, rd_dl);
            iowait_result_t r = iowait_read(dtls->waiter);
            if (r == IOWAIT_TIMEOUT) {
                uint64_t now =
                    xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
                if (deadline > 0 && now >= deadline) {
                    return -1;
                }
                DTLSv1_handle_timeout(dtls->ssl);
                continue;
            }
            if (r != IOWAIT_READY) {
                return -1;
            }
            break;
        }
        case SSL_ERROR_WANT_WRITE: {
            iowait_result_t r = iowait_write(dtls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
            break;
        }
        default: {
            unsigned long e = ERR_peek_error();
            xylem_loge("dtls handshake: ssl_error=%d reason=%s",
                       SSL_get_error(dtls->ssl, ret),
                       ERR_reason_error_string(e)
                           ? ERR_reason_error_string(e) : "unknown");
            return -1;
        }
        }
    }
}

void dtls_cache_alpn(xylem_dtls_conn_t* dtls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(dtls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(dtls->alpn)) {
        memcpy(dtls->alpn, alpn_proto, alpn_len);
        dtls->alpn[alpn_len] = '\0';
    }
}

static void _dtls_retransmit_cb(sched_timer_t* timer, void* ud);

void dtls_arm_retransmit(xylem_dtls_conn_t* dtls) {
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

void dtls_stop_retransmit(xylem_dtls_conn_t* dtls) {
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
    dtls_server_flush_write_bio(dtls);
    dtls_arm_retransmit(dtls);
}

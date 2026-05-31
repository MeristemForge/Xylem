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

#include "xylem/crypto/xylem-hmac256.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/addr.h"
#include "runtime/runtime.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _dtls_ex_data_idx = -1;
static int _dtls_peer_addr_idx = -1;
static once_flag _dtls_ex_data_once = ONCE_FLAG_INIT;

static void _dtls_init_ex_data(void) {
    _dtls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _dtls_peer_addr_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

static xylem_dtls_ctx_t* _dtls_get_ctx(SSL* ssl) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    return (xylem_dtls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _dtls_ex_data_idx);
}

static int _dtls_get_peer_addr(SSL* ssl, const uint8_t** out,
                               size_t* out_len) {
    addr_t* addr =
        (addr_t*)SSL_get_ex_data(ssl, _dtls_peer_addr_idx);
    if (!addr) {
        return -1;
    }
    if (addr->storage.ss_family == AF_INET) {
        *out_len = sizeof(struct sockaddr_in);
    } else if (addr->storage.ss_family == AF_INET6) {
        *out_len = sizeof(struct sockaddr_in6);
    } else {
        return -1;
    }
    *out = (const uint8_t*)&addr->storage;
    return 0;
}

static void _dtls_keylog_cb(const SSL* ssl, const char* line) {
    xylem_dtls_ctx_t* ctx = _dtls_get_ctx((SSL*)ssl);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _dtls_cookie_generate_cb(SSL* ssl, unsigned char* cookie,
                                    unsigned int* cookie_len) {
    xylem_dtls_ctx_t* ctx = _dtls_get_ctx(ssl);
    if (!ctx) {
        return 0;
    }

    const uint8_t* msg;
    size_t         msg_len;
    if (_dtls_get_peer_addr(ssl, &msg, &msg_len) < 0) {
        return 0;
    }

    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, cookie);
    *cookie_len = DTLS_COOKIE_SIZE;
    return 1;
}

static int _dtls_cookie_verify_cb(SSL* ssl, const unsigned char* cookie,
                                  unsigned int cookie_len) {
    xylem_dtls_ctx_t* ctx = _dtls_get_ctx(ssl);
    if (!ctx) {
        return 0;
    }

    const uint8_t* msg;
    size_t         msg_len;
    if (_dtls_get_peer_addr(ssl, &msg, &msg_len) < 0) {
        return 0;
    }

    uint8_t expected[DTLS_COOKIE_SIZE];
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, expected);

    if (cookie_len != DTLS_COOKIE_SIZE) {
        return 0;
    }
    return CRYPTO_memcmp(cookie, expected, DTLS_COOKIE_SIZE) == 0 ? 1 : 0;
}

static int _dtls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                                unsigned char* outlen,
                                const unsigned char* in,
                                unsigned int inlen, void* arg) {
    xylem_dtls_ctx_t* ctx = (xylem_dtls_ctx_t*)arg;
    (void)ssl;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
    xylem_dtls_ctx_t* ctx = (xylem_dtls_ctx_t*)calloc(1, sizeof(xylem_dtls_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(DTLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    if (RAND_bytes(ctx->cookie_secret, sizeof(ctx->cookie_secret)) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /**
     * Socket BIO client path may partially complete SSL_write; this
     * flag lets the retry use a different buffer pointer for the
     * same write.
     */
    SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_cookie_generate_cb(ctx->ssl_ctx, _dtls_cookie_generate_cb);
    SSL_CTX_set_cookie_verify_cb(ctx->ssl_ctx, _dtls_cookie_verify_cb);

    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);

    call_once(&_dtls_ex_data_once, _dtls_init_ex_data);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _dtls_ex_data_idx, ctx);

    return ctx;
}

void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}

int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path) {
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

    ctx->keylog_file = fopen(path, "a");
    if (!ctx->keylog_file) {
        return -1;
    }

    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _dtls_keylog_cb);
    return 0;
}

int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                             const char* cert, const char* key) {
    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert) != 1) {
        xylem_loge("dtls ctx: failed to load cert %s", cert);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        xylem_loge("dtls ctx: failed to load key %s", key);
        return -1;
    }
    return 0;
}

int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("dtls ctx: failed to load CA %s", ca_file);
        return -1;
    }
    return 0;
}

void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable) {
    int mode = enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx->ssl_ctx, mode, NULL);
}

int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
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

    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _dtls_alpn_select_cb, ctx);

    return 0;
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
        xylem_loge("dtls dial: socket failed for %s:%u", host, port);
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

    dtls->waiter = iowait_create(fd);
    if (!dtls->waiter) {
        platform_socket_close(fd);
        free(dtls);
        return NULL;
    }

    uint64_t timeout = (opts && opts->handshake_timeout_ms > 0)
        ? opts->handshake_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                        + timeout;
    iowait_set_rd_deadline(dtls->waiter, deadline);
    iowait_set_wr_deadline(dtls->waiter, deadline);

    dtls->ssl = SSL_new(ctx->ssl_ctx);
    if (!dtls->ssl) {
        dtls_conn_unref(dtls);
        return NULL;
    }
    SSL_set_fd(dtls->ssl, (int)fd);
    SSL_set_connect_state(dtls->ssl);

    const char* server_name = opts ? opts->server_name : NULL;
    if (server_name) {
        /* RFC 6066 forbids IP literals in the SNI HostName extension. */
        addr_t tmp;
        bool is_ip = (addr_pton(server_name, 0, &tmp) == 0);
        if (!is_ip) {
            SSL_set_tlsext_host_name(dtls->ssl, server_name);
        }
        int vmode = SSL_get_verify_mode(dtls->ssl);
        if (vmode & SSL_VERIFY_PEER) {
            SSL_set1_host(dtls->ssl, server_name);
        }
    } else if (SSL_get_verify_mode(dtls->ssl) & SSL_VERIFY_PEER) {
        xylem_logw("dtls dial: verify_peer enabled but opts->server_name "
                   "is NULL; peer identity is not checked, only "
                   "certificate chain trust (MITM risk)");
    }

    if (dtls_client_do_handshake(dtls, deadline) != 0) {
        dtls_conn_unref(dtls);
        return NULL;
    }

    iowait_set_rd_deadline(dtls->waiter, 0);
    iowait_set_wr_deadline(dtls->waiter, 0);

    dtls_cache_alpn(dtls);
    return dtls;
}

static int _dtls_client_recv(xylem_dtls_conn_t* dtls,
                             void* buf, int len) {
    dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            ERR_clear_error();
            int n = SSL_read(dtls->ssl, buf, len);
            if (n > 0) {
                ret = n;
                break;
            }
            int rc = dtls_handle_io_block(
                dtls, SSL_get_error(dtls->ssl, n), "SSL_read");
            if (rc == 1) {
                ret = 0;
                break;
            }
            if (rc < 0) {
                break;
            }
        }
    }

    dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_client_send(xylem_dtls_conn_t* dtls,
                             const void* data, int len) {
    dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            ERR_clear_error();
            int n = SSL_write(dtls->ssl, data, len);
            if (n > 0) {
                ret = 0;
                break;
            }
            int rc = dtls_handle_io_block(
                dtls, SSL_get_error(dtls->ssl, n), "SSL_write");
            if (rc != 0) {
                break;
            }
        }
    }

    dtls_conn_unref(dtls);
    return ret;
}

static void _dtls_client_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }
    if (dtls->ssl) {
        ERR_clear_error();
        SSL_shutdown(dtls->ssl);
    }
    iowait_close(dtls->waiter);
    dtls_conn_unref(dtls);
}

static void _dtls_handshake_coro(void* arg) {
    xylem_dtls_conn_t* dtls = arg;
    xylem_dtls_listener_t* ln = dtls->listener;

    dtls_conn_ref(dtls);

    if (dtls_init_ssl(dtls, ln->ctx->ssl_ctx) != 0) {
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        dtls_conn_unref(dtls);
        dtls_conn_unref(dtls);
        return;
    }

    SSL_set_accept_state(dtls->ssl);
    SSL_set_ex_data(dtls->ssl, _dtls_peer_addr_idx, &dtls->peer_addr);

    uint64_t hs_timeout = ln->opts.handshake_timeout_ms > 0
        ? ln->opts.handshake_timeout_ms : DTLS_DEFAULT_TIMEOUT_MS;
    uint64_t hs_deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + hs_timeout;

    bool success = false;
    while (!dtls->handshake_done) {
        /* Bound the wait with the handshake deadline directly on the
         * channel recv, instead of an external timer closing the
         * inbox: closing the inbox while the session is still in the
         * listener's tree would let the dispatcher send into a closed
         * channel (which aborts). On timeout recv returns NULL and we
         * fall through to the failure path below. */
        _dtls_dgram_t* dgram = (_dtls_dgram_t*)xylem_channel_recv_timeout(
            dtls->inbox, hs_deadline);
        if (!dgram) {
            break;
        }
        atomic_fetch_sub_explicit(&dtls->inbox_len, 1,
                                  memory_order_relaxed);

        BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
        free(dgram);

        ERR_clear_error();
        int ret = SSL_do_handshake(dtls->ssl);
        if (ret == 1) {
            dtls_server_flush_write_bio(dtls);
            dtls->handshake_done = true;
            success = true;
            break;
        }

        int err = SSL_get_error(dtls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            dtls_server_flush_write_bio(dtls);
            dtls_arm_retransmit(dtls);
            continue;
        }

        dtls_server_flush_write_bio(dtls);
        break;
    }

    dtls_stop_retransmit(dtls);

    sched_timer_destroy(dtls->retransmit_timer);
    sched_timer_destroy(dtls->handshake_timer);
    dtls->retransmit_timer = NULL;
    dtls->handshake_timer  = NULL;

    if (!success) {
        mtx_lock(&ln->sessions_mtx);
        rbtree_remove(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);
        dtls_conn_unref(dtls);
        dtls_conn_unref(dtls);
        return;
    }

    dtls_cache_alpn(dtls);
    xylem_channel_send(ln->accept_ch, dtls);
    dtls_conn_unref(dtls);
}

static void _dtls_dispatcher(void* arg) {
    xylem_dtls_listener_t* ln = arg;
    char buf[DTLS_PKT_BUF_SIZE];

    while (!atomic_load_explicit(&ln->closed, memory_order_acquire)) {
        struct sockaddr_storage from_ss;
        socklen_t from_len = sizeof(from_ss);
        ssize_t n = platform_socket_recvfrom(
            ln->fd, buf, sizeof(buf), &from_ss, &from_len);

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

        addr_t from_addr;
        memcpy(&from_addr.storage, &from_ss, sizeof(from_ss));

        mtx_lock(&ln->sessions_mtx);
        xylem_dtls_conn_t* dtls = dtls_find_session(ln, &from_addr);
        if (dtls) {
            /* Push under the lock so a concurrent xylem_dtls_close
             * cannot remove + free the session between the lookup and
             * the push. dtls_inbox_push only enqueues (it never
             * parks), so the critical section stays short. */
            _dtls_dgram_t* dgram =
                (_dtls_dgram_t*)malloc(sizeof(_dtls_dgram_t) + (size_t)n);
            if (dgram) {
                dgram->len = (size_t)n;
                memcpy(dgram->data, buf, (size_t)n);
                dtls_inbox_push(dtls, dgram);
            }
            mtx_unlock(&ln->sessions_mtx);
            continue;
        }
        mtx_unlock(&ln->sessions_mtx);

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

        dtls_inbox_push(dtls, dgram);

        mtx_lock(&ln->sessions_mtx);
        rbtree_insert(&ln->sessions, &dtls->server_node);
        mtx_unlock(&ln->sessions_mtx);

        runtime_spawn(_dtls_handshake_coro, dtls);
    }

    dtls_listener_unref(ln);
}

xylem_dtls_listener_t* xylem_dtls_listen(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd =
        platform_socket_listen(host, port_str, SOCK_DGRAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("dtls listen: failed for %s:%u", host, port);
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

    ln->waiter = iowait_create(fd);
    if (!ln->waiter) {
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    atomic_store_explicit(&ln->refcnt, 2, memory_order_relaxed);

    dtls_sessions_init(&ln->sessions);
    mtx_init(&ln->sessions_mtx, mtx_plain);
    ln->write_mu = xylem_mutex_create();

    ln->accept_ch = xylem_channel_create();
    if (!ln->accept_ch) {
        xylem_mutex_destroy(ln->write_mu);
        mtx_destroy(&ln->sessions_mtx);
        iowait_destroy(ln->waiter);
        platform_socket_close(fd);
        free(ln);
        return NULL;
    }

    runtime_spawn(_dtls_dispatcher, ln);
    return ln;
}

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    dtls_listener_ref(ln);
    xylem_dtls_conn_t* conn =
        (xylem_dtls_conn_t*)xylem_channel_recv(ln->accept_ch);
    dtls_listener_unref(ln);
    return conn;
}

static int _dtls_server_recv(xylem_dtls_conn_t* dtls,
                             void* buf, int len) {
    dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        for (;;) {
            _dtls_dgram_t* dgram =
                (dtls->rd_deadline_ms > 0)
                    ? (_dtls_dgram_t*)xylem_channel_recv_timeout(
                          dtls->inbox, dtls->rd_deadline_ms)
                    : (_dtls_dgram_t*)xylem_channel_recv(dtls->inbox);
            if (!dgram) {
                break;
            }
            atomic_fetch_sub_explicit(&dtls->inbox_len, 1,
                                      memory_order_relaxed);
            BIO_write(dtls->read_bio, dgram->data, (int)dgram->len);
            free(dgram);

            ERR_clear_error();
            int n = SSL_read(dtls->ssl, buf, len);
            if (n > 0) {
                ret = n;
                break;
            }

            int err = SSL_get_error(dtls->ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) {
                ret = 0;
                break;
            }
            if (err != SSL_ERROR_WANT_READ) {
                break;
            }
        }
    }

    dtls_conn_unref(dtls);
    return ret;
}

static int _dtls_server_send(xylem_dtls_conn_t* dtls,
                             const void* data, int len) {
    dtls_conn_ref(dtls);
    int ret = -1;

    if (!atomic_load_explicit(&dtls->closed, memory_order_acquire)) {
        xylem_mutex_lock(dtls->listener->write_mu);
        ret = dtls_server_send_record(dtls, data, len);
        xylem_mutex_unlock(dtls->listener->write_mu);
    }

    dtls_conn_unref(dtls);
    return ret;
}

int xylem_dtls_recv(xylem_dtls_conn_t* dtls, void* buf, int len) {
    if (dtls->listener) {
        return _dtls_server_recv(dtls, buf, len);
    }
    return _dtls_client_recv(dtls, buf, len);
}

int xylem_dtls_send(xylem_dtls_conn_t* dtls,
                    const void* data, int len) {
    if (dtls->listener) {
        return _dtls_server_send(dtls, data, len);
    }
    return _dtls_client_send(dtls, data, len);
}

static void _dtls_server_session_close(xylem_dtls_conn_t* dtls) {
    if (atomic_exchange(&dtls->closed, true)) {
        return;
    }
    dtls_stop_retransmit(dtls);
    if (dtls->handshake_timer) {
        sched_timer_stop(dtls->handshake_timer);
    }

    if (dtls->handshake_done && dtls->ssl) {
        SSL_shutdown(dtls->ssl);
        dtls_server_flush_write_bio(dtls);
    }

    /* Unlink from the session tree FIRST so the dispatcher can no
     * longer find this session and therefore cannot xylem_channel_send
     * into the inbox after we close it (send-on-closed aborts). The
     * dispatcher does find+push under sessions_mtx, so once the remove
     * commits no further push can target this inbox. */
    xylem_dtls_listener_t* ln = dtls->listener;
    mtx_lock(&ln->sessions_mtx);
    rbtree_remove(&ln->sessions, &dtls->server_node);
    mtx_unlock(&ln->sessions_mtx);

    /* Now close the inbox to wake a parked reader; the reader's
     * in-flight recv holds a channel reference, so the channel stays
     * alive until dtls_conn_unref drains and destroys it. */
    if (dtls->inbox) {
        xylem_channel_close(dtls->inbox);
    }

    dtls_conn_unref(dtls);
}

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    if (dtls->listener) {
        _dtls_server_session_close(dtls);
    } else {
        _dtls_client_close(dtls);
    }
}

void xylem_dtls_close_listener(xylem_dtls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    mtx_lock(&ln->sessions_mtx);
    while (!rbtree_empty(&ln->sessions)) {
        rbtree_node_t* node = rbtree_min(&ln->sessions);
        xylem_dtls_conn_t* dtls =
            rbtree_entry(node, xylem_dtls_conn_t, server_node);
        mtx_unlock(&ln->sessions_mtx);
        xylem_dtls_close(dtls);
        mtx_lock(&ln->sessions_mtx);
    }
    mtx_unlock(&ln->sessions_mtx);

    iowait_close(ln->waiter);
    xylem_channel_close(ln->accept_ch);
    dtls_listener_unref(ln);
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
    if (!dtls->listener) {
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

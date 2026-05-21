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

#include "xylem/net/xylem-tls.h"

#include "xylem/encoding/xylem-varint.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"
#include "thrds.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_READ_BUF_SIZE 65536

static int _tls_ex_data_idx = -1;
static once_flag _tls_ex_data_once = ONCE_FLAG_INIT;

static void _tls_init_ex_data(void) {
    _tls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

struct xylem_tls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
};

struct xylem_tls_conn_s {
    SSL*                   ssl;
    iowait_t*              waiter;
    platform_sock_t        fd;
    xylem_tls_ctx_t*       ctx;
    addr_t                 peer_addr;
    xylem_tcp_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    char                   alpn[256];
    _Atomic int32_t        refcnt;
    _Atomic bool           closed;
};

struct xylem_tls_listener_s {
    iowait_t*        waiter;
    platform_sock_t  fd;
    xylem_tls_ctx_t* ctx;
    xylem_tls_opts_t opts;
    _Atomic bool     closed;
};


static void _tls_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    xylem_tls_ctx_t* ctx =
        (xylem_tls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _tls_ex_data_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _tls_alpn_select_cb(SSL* ssl, const unsigned char** out,
                               unsigned char* outlen,
                               const unsigned char* in,
                               unsigned int inlen, void* arg) {
    xylem_tls_ctx_t* ctx = (xylem_tls_ctx_t*)arg;
    (void)ssl;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

xylem_tls_ctx_t* xylem_tls_ctx_create(void) {
    xylem_tls_ctx_t* ctx =
        (xylem_tls_ctx_t*)calloc(1, sizeof(xylem_tls_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(TLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /* Socket BIO may partially complete SSL_write; this flag lets the
     * retry use a different buffer pointer for the same logical write. */
    SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    call_once(&_tls_ex_data_once, _tls_init_ex_data);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _tls_ex_data_idx, ctx);

    return ctx;
}

void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx) {
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

int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path) {
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
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tls_keylog_cb);
    return 0;
}

int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                            const char* cert, const char* key) {
    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert) != 1) {
        xylem_loge("tls ctx: failed to load cert %s", cert);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        xylem_loge("tls ctx: failed to load key %s", key);
        return -1;
    }
    return 0;
}

int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("tls ctx: failed to load CA %s", ca_file);
        return -1;
    }
    return 0;
}

void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable) {
    int mode = enable ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(ctx->ssl_ctx, mode, NULL);
}

int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
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
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _tls_alpn_select_cb, ctx);

    return 0;
}


static int _tls_do_handshake(xylem_tls_conn_t* tls) {
    for (;;) {
        ERR_clear_error();
        int ret = SSL_do_handshake(tls->ssl);
        if (ret == 1) {
            return 0;
        }

        int err = SSL_get_error(tls->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY) {
                return -1;
            }
        } else {
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

static void _tls_cache_alpn(xylem_tls_conn_t* tls) {
    const unsigned char* alpn_proto = NULL;
    unsigned int         alpn_len   = 0;
    SSL_get0_alpn_selected(tls->ssl, &alpn_proto, &alpn_len);
    if (alpn_proto && alpn_len > 0 && alpn_len < sizeof(tls->alpn)) {
        memcpy(tls->alpn, alpn_proto, alpn_len);
        tls->alpn[alpn_len] = '\0';
    }
}

static int64_t _tls_raw_recv(xylem_tls_conn_t* tls, void* buf, size_t len) {
    if (atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        return -1;
    }

    for (;;) {
        ERR_clear_error();
        int n = SSL_read(tls->ssl, buf, (int)len);
        if (n > 0) {
            return n;
        }

        int err = SSL_get_error(tls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                return -1;
            }
            continue;
        }
        unsigned long ssl_err = ERR_peek_error();
        xylem_loge("tls SSL_read: ssl_error=%d reason=%s",
                   err,
                   ERR_reason_error_string(ssl_err)
                       ? ERR_reason_error_string(ssl_err)
                       : "unknown");
        return -1;
    }
}

static int _tls_raw_send(xylem_tls_conn_t* tls,
                         const void* data, size_t len) {
    if (atomic_load_explicit(&tls->closed, memory_order_acquire)) {
        return -1;
    }

    const char* ptr = (const char*)data;
    size_t      rem = len;

    while (rem > 0) {
        ERR_clear_error();
        int n = SSL_write(tls->ssl, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }

        int err = SSL_get_error(tls->ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            iowait_result_t r = iowait_write(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                return -1;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            iowait_result_t r = iowait_read(tls->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&tls->closed,
                                        memory_order_acquire)) {
                return -1;
            }
            continue;
        }
        unsigned long ssl_err = ERR_peek_error();
        xylem_loge("tls SSL_write: ssl_error=%d reason=%s",
                   err,
                   ERR_reason_error_string(ssl_err)
                       ? ERR_reason_error_string(ssl_err)
                       : "unknown");
        return -1;
    }
    return 0;
}


static xylem_tls_conn_t* _tls_conn_alloc(
    platform_sock_t fd, size_t max_read_buf) {
    xylem_tls_conn_t* tls
        = (xylem_tls_conn_t*)calloc(1, sizeof(xylem_tls_conn_t));
    if (!tls) {
        return NULL;
    }

    tls->fd     = fd;
    tls->waiter = iowait_create(fd);
    if (!tls->waiter) {
        free(tls);
        return NULL;
    }

    tls->read_buf_cap
        = max_read_buf > 0 ? max_read_buf : DEFAULT_READ_BUF_SIZE;
    atomic_store_explicit(&tls->refcnt, 1, memory_order_relaxed);

    return tls;
}

static void _tls_conn_ref(xylem_tls_conn_t* tls) {
    atomic_fetch_add_explicit(&tls->refcnt, 1, memory_order_relaxed);
}

static void _tls_conn_unref(xylem_tls_conn_t* tls) {
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
    free(tls->read_buf);
    free(tls);
}

static void _tls_conn_free(xylem_tls_conn_t* tls) {
    if (tls->ssl) {
        SSL_free(tls->ssl);
    }
    if (tls->waiter) {
        iowait_destroy(tls->waiter);
    }
    if (tls->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        /* Graceful TCP shutdown ensures buffered TLS records (including
         * close_notify) are delivered before the kernel tears down the
         * connection. Without this, closesocket may send RST. */
        shutdown(tls->fd, PLATFORM_SHUT_WR);
        platform_socket_close(tls->fd);
    }
    free(tls->read_buf);
    free(tls);
}


xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        if (addr_resolve(host, &addrs, &count) != 0 || count == 0) {
            xylem_loge("tls dial: DNS resolution failed for %s", host);
            return NULL;
        }
        resolved_addr = addrs[0];
        free(addrs);
        uint16_t rport;
        addr_ntop(&resolved_addr, resolved_ip, sizeof(resolved_ip), &rport);
        dial_host = resolved_ip;
    }

    bool            connected = false;
    platform_sock_t fd        = platform_socket_dial(
        dial_host, port_str, SOCK_STREAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tls dial: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    size_t             max_buf = opts ? opts->max_read_buf : 0;
    xylem_tls_conn_t*  tls     = _tls_conn_alloc(fd, max_buf);
    if (!tls) {
        platform_socket_close(fd);
        return NULL;
    }

    tls->ctx       = ctx;
    tls->peer_addr = resolved_addr;

    uint64_t connect_ms = opts ? opts->connect_timeout_ms : 0;
    uint64_t deadline   = 0;
    if (connect_ms > 0) {
        deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                   + connect_ms;
    }

    /* Wait for TCP connect completion. */
    if (!connected) {
        if (deadline > 0) {
            iowait_set_wr_deadline(tls->waiter, deadline);
        }
        iowait_result_t r = iowait_write(tls->waiter);
        if (r != IOWAIT_READY) {
            _tls_conn_free(tls);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            xylem_loge("tls dial fd=%d connect error=%d (%s)",
                       (int)fd, err, platform_socket_tostring(err));
            _tls_conn_free(tls);
            return NULL;
        }
    }

    /* Handshake needs both read and write deadlines. */
    if (deadline > 0) {
        iowait_set_rd_deadline(tls->waiter, deadline);
        iowait_set_wr_deadline(tls->waiter, deadline);
    }

    tls->ssl = SSL_new(ctx->ssl_ctx);
    if (!tls->ssl) {
        xylem_loge("tls dial: SSL_new failed");
        _tls_conn_free(tls);
        return NULL;
    }
    SSL_set_fd(tls->ssl, (int)fd);
    SSL_set_connect_state(tls->ssl);

    const char* hostname = opts ? opts->hostname : NULL;
    if (hostname) {
        SSL_set_tlsext_host_name(tls->ssl, hostname);
        int vmode = SSL_get_verify_mode(tls->ssl);
        if (vmode & SSL_VERIFY_PEER) {
            SSL_set1_host(tls->ssl, hostname);
        }
    }
    if (_tls_do_handshake(tls) != 0) {
        xylem_loge("tls dial: handshake failed for %s:%s", host, port_str);
        _tls_conn_free(tls);
        return NULL;
    }

    /* Clear deadlines so subsequent I/O starts with no deadline. */
    iowait_set_rd_deadline(tls->waiter, 0);
    iowait_set_wr_deadline(tls->waiter, 0);

    _tls_cache_alpn(tls);
    return tls;
}


void xylem_tls_close(xylem_tls_conn_t* tls) {
    if (atomic_exchange(&tls->closed, true)) {
        return;
    }

    iowait_close(tls->waiter);
    _tls_conn_unref(tls);
}


xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd
        = platform_socket_listen(host, port_str, SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tls listen: failed for %s:%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    xylem_tls_listener_t* ln = (xylem_tls_listener_t*)calloc(
        1, sizeof(xylem_tls_listener_t));
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

    return ln;
}


xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln) {
    uint64_t backoff_ms = 5;

    for (;;) {
        if (atomic_load_explicit(&ln->closed, memory_order_acquire)) {
            return NULL;
        }

        platform_sock_t fd = platform_socket_accept(ln->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(ln->waiter) != IOWAIT_READY) {
                    return NULL;
                }
                continue;
            }

            xylem_logw("tls listener fd=%d accept error=%d (%s)",
                       (int)ln->fd, err,
                       platform_socket_tostring(err));
            runtime_sleep(backoff_ms);
            if (backoff_ms < 1000) {
                backoff_ms *= 2;
            }
            continue;
        }

        backoff_ms = 5;

        size_t max_buf = ln->opts.max_read_buf;
        xylem_tls_conn_t* tls = _tls_conn_alloc(fd, max_buf);
        if (!tls) {
            platform_socket_close(fd);
            continue;
        }

        tls->ctx = ln->ctx;

        socklen_t peer_len = sizeof(tls->peer_addr.storage);
        getpeername(
            fd, (struct sockaddr*)&tls->peer_addr.storage, &peer_len);

        tls->ssl = SSL_new(ln->ctx->ssl_ctx);
        if (!tls->ssl) {
            xylem_loge("tls accept: SSL_new failed");
            _tls_conn_free(tls);
            continue;
        }
        SSL_set_fd(tls->ssl, (int)fd);
        SSL_set_accept_state(tls->ssl);

        if (_tls_do_handshake(tls) != 0) {
            _tls_conn_free(tls);
            return NULL;
        }

        _tls_cache_alpn(tls);
        return tls;
    }
}


void xylem_tls_close_listener(xylem_tls_listener_t* ln) {
    if (atomic_exchange(&ln->closed, true)) {
        return;
    }

    iowait_close(ln->waiter);
    iowait_destroy(ln->waiter);
    platform_socket_close(ln->fd);
    free(ln);
}


static int _tls_read_exact(xylem_tls_conn_t* tls, void* buf, size_t len) {
    char*  ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        size_t avail = tls->read_buf_len - tls->read_buf_pos;
        if (avail > 0) {
            size_t copy = avail < rem ? avail : rem;
            memcpy(ptr, tls->read_buf + tls->read_buf_pos, copy);
            tls->read_buf_pos += copy;
            ptr += copy;
            rem -= copy;
            continue;
        }

        tls->read_buf_pos = 0;
        tls->read_buf_len = 0;

        int64_t n = _tls_raw_recv(tls, tls->read_buf, tls->read_buf_cap);
        if (n <= 0) {
            return -1;
        }
        tls->read_buf_len = (size_t)n;
    }
    return 0;
}

static int64_t
_tls_recv_fixed(xylem_tls_conn_t* tls, void* buf, size_t len) {
    size_t frame_len = tls->frame_opts.fixed.len;
    if (frame_len > len) {
        xylem_loge("tls fd=%d recv: fixed frame %zu exceeds buffer %zu", (int)tls->fd, frame_len, len);
        return -1;
    }
    if (_tls_read_exact(tls, buf, frame_len) != 0) {
        return -1;
    }
    return (int64_t)frame_len;
}

static int64_t
_tls_recv_length(xylem_tls_conn_t* tls, void* buf, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = tls->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        xylem_loge("tls fd=%d recv: header_size %u exceeds limit", (int)tls->fd, hdr_sz);
        return -1;
    }

    if (_tls_read_exact(tls, hdr, hdr_sz) != 0) {
        return -1;
    }

    uint64_t body_len = 0;

    if (tls->frame_opts.length.coding == XYLEM_TCP_LENGTH_VARINT) {
        size_t pos = (size_t)tls->frame_opts.length.field_offset;
        if (!xylem_varint_decode(hdr, hdr_sz, &pos, &body_len)) {
            xylem_loge("tls fd=%d recv: varint decode failed", (int)tls->fd);
            return -1;
        }
    } else {
        uint8_t* field = hdr + tls->frame_opts.length.field_offset;
        if (tls->frame_opts.length.big_endian) {
            for (uint32_t i = 0; i < tls->frame_opts.length.field_size; i++) {
                body_len = (body_len << 8) | field[i];
            }
        } else {
            for (uint32_t i = 0; i < tls->frame_opts.length.field_size; i++) {
                body_len |= (uint64_t)field[i] << (i * 8);
            }
        }
    }

    int64_t adjusted
        = (int64_t)body_len + tls->frame_opts.length.adjustment;
    if (adjusted < 0) {
        xylem_loge("tls fd=%d recv: negative payload length", (int)tls->fd);
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        xylem_loge("tls fd=%d recv: payload %zu exceeds buffer %zu", (int)tls->fd, payload_len, len);
        return -1;
    }

    if (payload_len > 0 && _tls_read_exact(tls, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t
_tls_recv_delimiter(xylem_tls_conn_t* tls, void* buf, size_t len) {
    const char* delim     = tls->frame_opts.delimiter.delim;
    size_t      delim_len = tls->frame_opts.delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(delim);
    }

    char*  dst = (char*)buf;
    size_t pos = 0;

    while (pos < len) {
        size_t avail = tls->read_buf_len - tls->read_buf_pos;
        if (avail == 0) {
            tls->read_buf_pos = 0;
            tls->read_buf_len = 0;
            int64_t n
                = _tls_raw_recv(tls, tls->read_buf, tls->read_buf_cap);
            if (n <= 0) {
                return -1;
            }
            tls->read_buf_len = (size_t)n;
            avail             = (size_t)n;
        }

        char* src = tls->read_buf + tls->read_buf_pos;
        for (size_t i = 0; i < avail && pos < len; i++) {
            dst[pos++] = src[i];
            tls->read_buf_pos++;

            if (pos >= delim_len
                && memcmp(dst + pos - delim_len, delim, delim_len) == 0) {
                pos -= delim_len;
                dst[pos] = '\0';
                return (int64_t)pos;
            }
        }
    }

    xylem_loge("tls fd=%d recv: delimiter not found within buffer", (int)tls->fd);
    return -1;
}

static int
_tls_send_length(xylem_tls_conn_t* tls, const void* data, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = tls->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        xylem_loge("tls fd=%d send: header_size %u exceeds limit", (int)tls->fd, hdr_sz);
        return -1;
    }

    int64_t wire_len = (int64_t)len - tls->frame_opts.length.adjustment;
    if (wire_len < 0) {
        xylem_loge("tls fd=%d send: negative wire length", (int)tls->fd);
        return -1;
    }

    memset(hdr, 0, hdr_sz);

    if (tls->frame_opts.length.coding == XYLEM_TCP_LENGTH_VARINT) {
        size_t pos = (size_t)tls->frame_opts.length.field_offset;
        if (!xylem_varint_encode((uint64_t)wire_len, hdr, hdr_sz, &pos)) {
            xylem_loge("tls fd=%d send: varint encode failed", (int)tls->fd);
            return -1;
        }
        if (_tls_raw_send(tls, hdr, pos) != 0) {
            return -1;
        }
        return _tls_raw_send(tls, data, len);
    }

    uint8_t* field = hdr + tls->frame_opts.length.field_offset;
    uint64_t val   = (uint64_t)wire_len;

    if (tls->frame_opts.length.big_endian) {
        for (int32_t i = (int32_t)tls->frame_opts.length.field_size - 1;
             i >= 0;
             i--) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    } else {
        for (uint32_t i = 0; i < tls->frame_opts.length.field_size; i++) {
            field[i] = (uint8_t)(val & 0xFF);
            val >>= 8;
        }
    }

    if (_tls_raw_send(tls, hdr, hdr_sz) != 0) {
        return -1;
    }
    return _tls_raw_send(tls, data, len);
}


void xylem_tls_set_framing(
    xylem_tls_conn_t* tls, xylem_tcp_frame_opts_t* opts) {
    if (opts) {
        tls->frame_opts = *opts;
    } else {
        memset(&tls->frame_opts, 0, sizeof(tls->frame_opts));
    }
}

int64_t
xylem_tls_recv(xylem_tls_conn_t* tls, void* buf, size_t len) {
    _tls_conn_ref(tls);

    if (tls->frame_opts.type == XYLEM_TCP_FRAME_NONE) {
        int64_t ret = _tls_raw_recv(tls, buf, len);
        _tls_conn_unref(tls);
        return ret;
    }

    if (!tls->read_buf) {
        tls->read_buf = (char*)malloc(tls->read_buf_cap);
        if (!tls->read_buf) {
            _tls_conn_unref(tls);
            return -1;
        }
    }

    int64_t ret;
    switch (tls->frame_opts.type) {
    case XYLEM_TCP_FRAME_FIXED:
        ret = _tls_recv_fixed(tls, buf, len);
        break;
    case XYLEM_TCP_FRAME_LENGTH:
        ret = _tls_recv_length(tls, buf, len);
        break;
    case XYLEM_TCP_FRAME_DELIMITER:
        ret = _tls_recv_delimiter(tls, buf, len);
        break;
    default:
        ret = -1;
        break;
    }
    _tls_conn_unref(tls);
    return ret;
}

int xylem_tls_send(xylem_tls_conn_t* tls, const void* data, size_t len) {
    _tls_conn_ref(tls);
    int ret;
    switch (tls->frame_opts.type) {
    case XYLEM_TCP_FRAME_LENGTH:
        ret = _tls_send_length(tls, data, len);
        break;
    default:
        ret = _tls_raw_send(tls, data, len);
        break;
    }
    _tls_conn_unref(tls);
    return ret;
}


void xylem_tls_set_read_deadline(
    xylem_tls_conn_t* tls, uint64_t deadline_ms) {
    iowait_set_rd_deadline(tls->waiter, deadline_ms);
}

void xylem_tls_set_write_deadline(
    xylem_tls_conn_t* tls, uint64_t deadline_ms) {
    iowait_set_wr_deadline(tls->waiter, deadline_ms);
}


int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    return addr_ntop(&tls->peer_addr, host, host_len, port);
}

int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
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

int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
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

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls) {
    return tls->alpn[0] ? tls->alpn : NULL;
}

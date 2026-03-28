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

#include "xylem/xylem-dtls.h"
#include "xylem/xylem-list.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-udp.h"

#include "platform/platform-socket.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _dtls_ex_data_idx = -1;

struct xylem_dtls_ctx_s {
    SSL_CTX* ssl_ctx;
    uint8_t* alpn_wire;
    size_t   alpn_wire_len;
    FILE*    keylog_file;
};

struct xylem_dtls_s {
    SSL*                   ssl;
    BIO*                   read_bio;
    BIO*                   write_bio;
    xylem_udp_t*           udp;
    xylem_dtls_ctx_t*      ctx;
    xylem_dtls_handler_t*  handler;
    xylem_dtls_server_t*   server;
    xylem_addr_t           peer_addr;
    void*                  userdata;
    bool                   handshake_done;
    bool                   closing;
    xylem_loop_t*          loop;
    xylem_loop_timer_t*    retransmit_timer;
    xylem_list_node_t      server_node;
};

struct xylem_dtls_server_s {
    xylem_udp_t*           udp;
    xylem_dtls_ctx_t*      ctx;
    xylem_dtls_handler_t*  handler;
    xylem_loop_t*          loop;
    xylem_list_t           sessions;
    bool                   closing;
};

static void _dtls_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    xylem_dtls_ctx_t* ctx =
        (xylem_dtls_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _dtls_ex_data_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _dtls_cookie_generate_cb(SSL* ssl, unsigned char* cookie,
                                    unsigned int* cookie_len) {
    (void)ssl;
    RAND_bytes(cookie, 16);
    *cookie_len = 16;
    return 1;
}

static int _dtls_cookie_verify_cb(SSL* ssl, const unsigned char* cookie,
                                  unsigned int cookie_len) {
    (void)ssl;
    (void)cookie;
    (void)cookie_len;
    /** Accept all cookies -- the cookie exchange itself provides
     * sufficient DoS protection by verifying the client's address. */
    return 1;
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
    xylem_dtls_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->ssl_ctx = SSL_CTX_new(DTLS_method());
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /**
     * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER is not needed here because we
     * use memory BIOs. SSL_write only writes into an in-memory buffer,
     * never directly to a socket, so it always completes in a single call
     * and never returns SSL_ERROR_WANT_WRITE during data transfer. The
     * flag is only necessary with socket BIOs where SSL_write may
     * partially complete and require a retry with the same buffer pointer.
     */

    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_cookie_generate_cb(ctx->ssl_ctx, _dtls_cookie_generate_cb);
    SSL_CTX_set_cookie_verify_cb(ctx->ssl_ctx, _dtls_cookie_verify_cb);

    /* Register ex_data index once for keylog callback to recover ctx. */
    if (_dtls_ex_data_idx == -1) {
        _dtls_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL,
                                                      NULL, NULL, NULL);
    }
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

    /* Close any previously opened keylog file. */
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
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key,
                                    SSL_FILETYPE_PEM) != 1) {
        return -1;
    }
    return 0;
}

int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
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

    uint8_t* wire = malloc(total);
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


static void _dtls_flush_write_bio(xylem_dtls_t* dtls) {
    char buf[16384];
    int  n;

    while ((n = BIO_read(dtls->write_bio, buf, sizeof(buf))) > 0) {
        xylem_udp_send(dtls->udp, &dtls->peer_addr, buf, (size_t)n);
    }
}

static void _dtls_feed_read_bio(xylem_dtls_t* dtls,
                                void* data, size_t len) {
    BIO_write(dtls->read_bio, data, (int)len);
}

static void _dtls_retransmit_timeout_cb(xylem_loop_t* loop,
                                        xylem_loop_timer_t* timer,
                                        void* ud) {
    (void)loop;
    (void)timer;
    xylem_dtls_t* dtls = (xylem_dtls_t*)ud;

    DTLSv1_handle_timeout(dtls->ssl);
    _dtls_flush_write_bio(dtls);
}

static void _dtls_arm_retransmit(xylem_dtls_t* dtls) {
    struct timeval tv;
    if (DTLSv1_get_timeout(dtls->ssl, &tv)) {
        uint64_t ms = (uint64_t)tv.tv_sec * 1000 +
                      (uint64_t)tv.tv_usec / 1000;
        if (ms == 0) {
            ms = 1;
        }
        xylem_loop_stop_timer(dtls->retransmit_timer);
        xylem_loop_start_timer(dtls->retransmit_timer,
                               _dtls_retransmit_timeout_cb, ms, 0);
    }
}

static void _dtls_stop_retransmit(xylem_dtls_t* dtls) {
    xylem_loop_stop_timer(dtls->retransmit_timer);
}

static int _dtls_init_ssl(xylem_dtls_t* dtls) {
    dtls->ssl = SSL_new(dtls->ctx->ssl_ctx);
    if (!dtls->ssl) {
        return -1;
    }

    dtls->read_bio  = BIO_new(BIO_s_mem());
    dtls->write_bio = BIO_new(BIO_s_mem());
    if (!dtls->read_bio || !dtls->write_bio) {
        /* BIO_free accepts NULL safely. */
        BIO_free(dtls->read_bio);
        BIO_free(dtls->write_bio);
        SSL_free(dtls->ssl);
        dtls->ssl       = NULL;
        dtls->read_bio  = NULL;
        dtls->write_bio = NULL;
        return -1;
    }

    SSL_set_bio(dtls->ssl, dtls->read_bio, dtls->write_bio);
    return 0;
}

static void _dtls_do_handshake(xylem_dtls_t* dtls) {
    int rc  = SSL_do_handshake(dtls->ssl);
    int err = SSL_get_error(dtls->ssl, rc);

    if (rc == 1) {
        dtls->handshake_done = true;
        _dtls_flush_write_bio(dtls);
        _dtls_stop_retransmit(dtls);

        if (dtls->server) {
            if (dtls->handler && dtls->handler->on_accept) {
                dtls->handler->on_accept(dtls);
            }
        } else {
            if (dtls->handler && dtls->handler->on_connect) {
                dtls->handler->on_connect(dtls);
            }
        }
        return;
    }

    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        _dtls_flush_write_bio(dtls);
        _dtls_arm_retransmit(dtls);
        return;
    }

    /* Flush any pending alert before tearing down. */
    _dtls_flush_write_bio(dtls);
    xylem_dtls_close(dtls);
}

static bool _dtls_addr_equal(xylem_addr_t* a, xylem_addr_t* b) {
    if (a->storage.ss_family != b->storage.ss_family) {
        return false;
    }
    if (a->storage.ss_family == AF_INET) {
        struct sockaddr_in* sa = (struct sockaddr_in*)&a->storage;
        struct sockaddr_in* sb = (struct sockaddr_in*)&b->storage;
        return sa->sin_port == sb->sin_port &&
               sa->sin_addr.s_addr == sb->sin_addr.s_addr;
    }
    if (a->storage.ss_family == AF_INET6) {
        struct sockaddr_in6* sa = (struct sockaddr_in6*)&a->storage;
        struct sockaddr_in6* sb = (struct sockaddr_in6*)&b->storage;
        return sa->sin6_port == sb->sin6_port &&
               memcmp(&sa->sin6_addr, &sb->sin6_addr, 16) == 0;
    }
    return false;
}

static xylem_dtls_t* _dtls_find_session(xylem_dtls_server_t* server,
                                        xylem_addr_t* addr) {
    if (xylem_list_empty(&server->sessions)) {
        return NULL;
    }

    xylem_list_node_t* node = xylem_list_head(&server->sessions);
    xylem_list_node_t* sentinel = xylem_list_sentinel(&server->sessions);
    while (node != sentinel) {
        xylem_dtls_t* dtls =
            xylem_list_entry(node, xylem_dtls_t, server_node);
        if (_dtls_addr_equal(&dtls->peer_addr, addr)) {
            return dtls;
        }
        node = xylem_list_next(node);
    }
    return NULL;
}


static void _dtls_client_read_cb(xylem_udp_t* udp, void* data,
                                 size_t len, xylem_addr_t* addr) {
    (void)addr;
    xylem_dtls_t* dtls = (xylem_dtls_t*)xylem_udp_get_userdata(udp);

    _dtls_feed_read_bio(dtls, data, len);

    if (!dtls->handshake_done) {
        _dtls_do_handshake(dtls);
        if (!dtls->handshake_done) {
            return;
        }
    }

    char buf[16384];
    int  n;

    while ((n = SSL_read(dtls->ssl, buf, sizeof(buf))) > 0) {
        if (dtls->handler && dtls->handler->on_read) {
            dtls->handler->on_read(dtls, buf, (size_t)n);
        }
        if (dtls->closing) {
            return;
        }
    }

    int err = SSL_get_error(dtls->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) {
        xylem_dtls_close(dtls);
        return;
    }

    if (err != SSL_ERROR_WANT_READ) {
        xylem_dtls_close(dtls);
    }
}

/**
 * Deferred free so the session pointer stays valid through the
 * current loop iteration's callback chain.
 */
static void _dtls_free_cb(xylem_loop_t* loop, xylem_loop_post_t* req,
                          void* ud) {
    (void)loop;
    (void)req;
    xylem_dtls_t* dtls = (xylem_dtls_t*)ud;
    xylem_loop_destroy_timer(dtls->retransmit_timer);
    free(dtls);
}

static void _dtls_client_close_cb(xylem_udp_t* udp, int err) {
    xylem_dtls_t* dtls = (xylem_dtls_t*)xylem_udp_get_userdata(udp);
    if (!dtls) {
        return;
    }
    if (dtls->ssl) {
        SSL_free(dtls->ssl);
        dtls->ssl = NULL;
    }
    if (dtls->handler && dtls->handler->on_close) {
        dtls->handler->on_close(dtls, err);
    }
    /* Defer free to next loop iteration so close_node stays valid. */
    xylem_loop_post(dtls->loop, _dtls_free_cb, dtls);
}

static void _dtls_server_read_cb(xylem_udp_t* udp, void* data,
                                 size_t len, xylem_addr_t* addr) {
    xylem_dtls_server_t* server =
        (xylem_dtls_server_t*)xylem_udp_get_userdata(udp);

    if (server->closing) {
        return;
    }

    xylem_dtls_t* dtls = _dtls_find_session(server, addr);

    if (dtls) {
        _dtls_feed_read_bio(dtls, data, len);

        if (!dtls->handshake_done) {
            _dtls_do_handshake(dtls);
            if (!dtls->handshake_done) {
                return;
            }
        }

        char buf[16384];
        int  n;

        while ((n = SSL_read(dtls->ssl, buf, sizeof(buf))) > 0) {
            if (dtls->handler && dtls->handler->on_read) {
                dtls->handler->on_read(dtls, buf, (size_t)n);
            }
            if (dtls->closing) {
                return;
            }
        }

        int err = SSL_get_error(dtls->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            xylem_dtls_close(dtls);
        } else if (err != SSL_ERROR_WANT_READ) {
            xylem_dtls_close(dtls);
        }
        return;
    }

    dtls = calloc(1, sizeof(*dtls));
    if (!dtls) {
        return;
    }

    dtls->udp       = server->udp;
    dtls->ctx       = server->ctx;
    dtls->handler   = server->handler;
    dtls->server    = server;
    dtls->peer_addr = *addr;
    dtls->loop      = server->loop;

    dtls->retransmit_timer = xylem_loop_create_timer(server->loop, dtls);

    if (_dtls_init_ssl(dtls) != 0) {
        xylem_loop_destroy_timer(dtls->retransmit_timer);
        free(dtls);
        return;
    }

    SSL_set_accept_state(dtls->ssl);

    xylem_list_insert_tail(&server->sessions, &dtls->server_node);

    /* Feed the first packet so the handshake can begin. */
    _dtls_feed_read_bio(dtls, data, len);
    _dtls_do_handshake(dtls);
}

static void _dtls_server_close_cb(xylem_udp_t* udp, int err) {
    (void)err;
    xylem_dtls_server_t* server =
        (xylem_dtls_server_t*)xylem_udp_get_userdata(udp);
    free(server);
}

static xylem_udp_handler_t _dtls_client_udp_handler = {
    .on_read  = _dtls_client_read_cb,
    .on_close = _dtls_client_close_cb,
};

xylem_dtls_t* xylem_dtls_dial(xylem_loop_t* loop,
                              xylem_addr_t* addr,
                              xylem_dtls_ctx_t* ctx,
                              xylem_dtls_handler_t* handler) {
    xylem_dtls_t* dtls = calloc(1, sizeof(*dtls));
    if (!dtls) {
        return NULL;
    }

    dtls->ctx       = ctx;
    dtls->handler   = handler;
    dtls->peer_addr = *addr;
    dtls->loop      = loop;

    xylem_udp_t* udp = xylem_udp_dial(loop, addr,
                                      &_dtls_client_udp_handler);
    if (!udp) {
        free(dtls);
        return NULL;
    }

    dtls->udp = udp;
    xylem_udp_set_userdata(udp, dtls);

    dtls->retransmit_timer = xylem_loop_create_timer(loop, dtls);

    if (_dtls_init_ssl(dtls) != 0) {
        xylem_loop_destroy_timer(dtls->retransmit_timer);
        xylem_udp_set_userdata(udp, NULL);
        xylem_udp_close(udp);
        free(dtls);
        return NULL;
    }

    SSL_set_connect_state(dtls->ssl);
    _dtls_do_handshake(dtls);

    return dtls;
}

int xylem_dtls_send(xylem_dtls_t* dtls,
                    const void* data, size_t len) {
    if (!dtls->handshake_done || dtls->closing) {
        return -1;
    }

    int n = SSL_write(dtls->ssl, data, (int)len);
    if (n <= 0) {
        return -1;
    }

    _dtls_flush_write_bio(dtls);

    if (dtls->handler && dtls->handler->on_write_done) {
        dtls->handler->on_write_done(dtls, (void*)data, len, 0);
    }

    return 0;
}

void xylem_dtls_close(xylem_dtls_t* dtls) {
    if (dtls->closing) {
        return;
    }
    dtls->closing = true;

    _dtls_stop_retransmit(dtls);

    if (dtls->ssl && dtls->handshake_done) {
        SSL_shutdown(dtls->ssl);
        _dtls_flush_write_bio(dtls);
    }

    if (dtls->server) {
        /**
         * Server-side session: detach from server list, clean up
         * SSL state, notify user. The shared UDP socket stays open.
         */
        xylem_list_remove(&dtls->server->sessions, &dtls->server_node);

        if (dtls->ssl) {
            SSL_free(dtls->ssl);
            dtls->ssl = NULL;
        }

        if (dtls->handler && dtls->handler->on_close) {
            dtls->handler->on_close(dtls, 0);
        }

        /* Defer free to next loop iteration so close_node stays valid. */
        xylem_loop_post(dtls->loop, _dtls_free_cb, dtls);
    } else {
        /**
         * Client-side: close the dedicated UDP socket. The
         * _dtls_client_close_cb will free SSL and notify user.
         * Defer free so close_node stays valid.
         */
        xylem_udp_close(dtls->udp);
    }
}

const char* xylem_dtls_get_alpn(xylem_dtls_t* dtls) {
    if (!dtls->ssl) {
        return NULL;
    }

    const unsigned char* proto = NULL;
    unsigned int         len   = 0;
    SSL_get0_alpn_selected(dtls->ssl, &proto, &len);

    if (!proto || len == 0) {
        return NULL;
    }
    return (const char*)proto;
}

void* xylem_dtls_get_userdata(xylem_dtls_t* dtls) {
    return dtls->userdata;
}

void xylem_dtls_set_userdata(xylem_dtls_t* dtls, void* ud) {
    dtls->userdata = ud;
}

static xylem_udp_handler_t _dtls_server_udp_handler = {
    .on_read  = _dtls_server_read_cb,
    .on_close = _dtls_server_close_cb,
};

xylem_dtls_server_t* xylem_dtls_listen(xylem_loop_t* loop,
                                       xylem_addr_t* addr,
                                       xylem_dtls_ctx_t* ctx,
                                       xylem_dtls_handler_t* handler) {
    xylem_dtls_server_t* server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    server->ctx     = ctx;
    server->handler = handler;
    server->loop    = loop;
    xylem_list_init(&server->sessions);

    xylem_udp_t* udp = xylem_udp_listen(loop, addr,
                                       &_dtls_server_udp_handler);
    if (!udp) {
        free(server);
        return NULL;
    }

    server->udp = udp;
    xylem_udp_set_userdata(udp, server);

    return server;
}

void xylem_dtls_close_server(xylem_dtls_server_t* server) {
    if (server->closing) {
        return;
    }
    server->closing = true;

    while (!xylem_list_empty(&server->sessions)) {
        xylem_list_node_t* node = xylem_list_head(&server->sessions);
        xylem_dtls_t* dtls =
            xylem_list_entry(node, xylem_dtls_t, server_node);
        xylem_dtls_close(dtls);
    }

    /* _dtls_server_close_cb frees server. */
    xylem_udp_close(server->udp);
}

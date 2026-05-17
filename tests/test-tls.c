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

#include "xylem.h"
#include "xylem/net/xylem-tls.h"
#include "assert.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

#define TLS_HOST          "127.0.0.1"
#define TLS_PORT          14433
#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*      ready;
    xylem_waitgroup_t*    wg;
    xylem_tls_ctx_t*      srv_ctx;
    xylem_tls_ctx_t*      cli_ctx;
    uint16_t              port;
} _ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}


/**
 * Write PEM data to a file via memory BIO instead of passing FILE* directly
 * to OpenSSL (e.g. PEM_write_X509). On Windows, the OpenSSL DLL and the
 * application may link against different C runtimes whose FILE structs are
 * incompatible. Passing a FILE* across the DLL boundary triggers the
 * OPENSSL_Applink error. Using a memory BIO keeps all FILE* operations
 * inside the application's own CRT, avoiding the issue entirely.
 */
static int _write_pem_to_file(const char* path,
                              int (*write_fn)(BIO*, void*),
                              void* obj) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return -1;
    }
    if (write_fn(bio, obj) != 1) {
        BIO_free(bio);
        return -1;
    }
    char* data = NULL;
    long  len  = BIO_get_mem_data(bio, &data);
    FILE* f    = fopen(path, "wb");
    if (!f) {
        BIO_free(bio);
        return -1;
    }
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    BIO_free(bio);
    return 0;
}

static int _write_cert_pem(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, (X509*)obj);
}

static int _write_key_pem(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj,
                                    NULL, NULL, 0, NULL, NULL);
}

static int _gen_self_signed(const char* cert_path, const char* key_path) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) {
        return -1;
    }
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx) {
        EVP_PKEY_free(pkey);
        return -1;
    }
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    int rc = 0;
    if (_write_pem_to_file(cert_path, _write_cert_pem, x509) != 0) {
        rc = -1;
    }
    if (rc == 0 && _write_pem_to_file(key_path, _write_key_pem, pkey) != 0) {
        rc = -1;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}


static void test_ctx_create_destroy(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_destroy(ctx);
}


static void test_load_cert_valid(void) {
    const char* cert = "test_tls_cert.pem";
    const char* key  = "test_tls_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, cert, key) == 0);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

static void test_load_cert_invalid(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, "nonexistent.pem",
                                   "nonexistent.pem") == -1);
    xylem_tls_ctx_destroy(ctx);
}


static void test_set_ca(void) {
    const char* cert = "test_tls_ca.pem";
    const char* key  = "test_tls_ca_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_set_ca(ctx, cert) == 0);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}


static void test_set_verify(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_set_verify(ctx, true);
    xylem_tls_ctx_set_verify(ctx, false);
    xylem_tls_ctx_destroy(ctx);
}


static void test_set_alpn(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, protos, 2) == 0);
    xylem_tls_ctx_destroy(ctx);
}


static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_send(conn, buf, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem tls";
    ASSERT(xylem_tls_send(conn, msg, strlen(msg)) == 0);

    char buf[64];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_echo_cert.pem";
    const char* key  = "test_tls_echo_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_echo_server, &ctx);
    xylem_spawn(_echo_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_handshake_and_echo(void) {
    xylem_run(_echo_main, NULL, NULL);
}


static void _fail_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    /* Accept will fail because client handshake fails. */
    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    /* May or may not be NULL depending on timing. */
    if (conn) {
        xylem_tls_close(conn);
    }

    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_main(void* arg) {
    (void)arg;
    const char* cert  = "test_tls_fail_cert.pem";
    const char* key   = "test_tls_fail_key.pem";
    const char* cert2 = "test_tls_fail_cert2.pem";
    const char* key2  = "test_tls_fail_key2.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);
    ASSERT(_gen_self_signed(cert2, key2) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(cli_ctx, cert2) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 1,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_fail_server, &ctx);
    xylem_spawn(_fail_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    remove(cert2);
    remove(key2);
    xylem_shutdown();
}

static void test_handshake_failure(void) {
    xylem_run(_fail_main, NULL, NULL);
}


static void _alpn_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    /* Echo a sync message so the client knows the server is alive. */
    char buf[8];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_tls_send(conn, buf, (size_t)n);
    }

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    /* Round-trip exchange ensures the server has completed its
     * handshake before we tear down the connection. */
    ASSERT(xylem_tls_send(conn, "ok", 2) == 0);
    char buf[8];
    xylem_tls_recv(conn, buf, sizeof(buf));

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_alpn_cert.pem";
    const char* key  = "test_tls_alpn_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    const char* protos[] = {"h2", "http/1.1"};

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(srv_ctx, protos, 2) == 0);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(cli_ctx, protos, 2) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 2,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_alpn_server, &ctx);
    xylem_spawn(_alpn_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_alpn_negotiation(void) {
    xylem_run(_alpn_main, NULL, NULL);
}


static const xylem_tcp_frame_opts_t _len_frame = {
    .type   = XYLEM_TCP_FRAME_LENGTH,
    .length = {
        .header_size  = 2,
        .field_offset = 0,
        .field_size   = 2,
        .adjustment   = 0,
        .big_endian   = true,
    },
};

static void _frame_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = _len_frame;
    xylem_tls_set_framing(conn, &frame);
    ASSERT(xylem_tls_send(conn, "FRAME1", 6) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = _len_frame;
    xylem_tls_set_framing(conn, &frame);

    char buf[64];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n == 6);
    ASSERT(memcmp(buf, "FRAME1", 6) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_frame_cert.pem";
    const char* key  = "test_tls_frame_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 3,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_frame_server, &ctx);
    xylem_spawn(_frame_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_framing(void) {
    xylem_run(_frame_main, NULL, NULL);
}


static void _deadline_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    /* Hold connection open, send nothing. */
    xylem_sleep(2000);
    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_tls_set_read_deadline(conn, deadline);

    char buf[64];
    int64_t n = xylem_tls_recv(conn, buf, sizeof(buf));
    ASSERT(n == -1);
    ASSERT(xylem_tls_get_error(conn) == XYLEM_ERR_TIMEOUT);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_dl_cert.pem";
    const char* key  = "test_tls_dl_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 4,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_deadline_server, &ctx);
    xylem_spawn(_deadline_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_read_deadline(void) {
    xylem_run(_deadline_main, NULL, NULL);
}


static void _sac_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    if (conn) {
        /* Drain the sync message then let the client close. */
        char buf[8];
        xylem_tls_recv(conn, buf, sizeof(buf));
        xylem_tls_close(conn);
    }
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _sac_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    /* Send a byte to ensure the server handshake completes. */
    xylem_tls_send(conn, "x", 1);
    xylem_tls_close(conn);
    /* conn is freed -- cannot use after close. */

    xylem_waitgroup_done(ctx->wg);
}

static void _sac_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_sac_cert.pem";
    const char* key  = "test_tls_sac_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 5,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_sac_server, &ctx);
    xylem_spawn(_sac_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_close(void) {
    xylem_run(_sac_main, NULL, NULL);
}


static void _cl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ln);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn == NULL);

    xylem_waitgroup_done(ctx->wg);
}

static void _cl_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        (xylem_tls_listener_t*)xylem_channel_recv(ctx->ready);
    xylem_sleep(100);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cl_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_cl_cert.pem";
    const char* key  = "test_tls_cl_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .port    = TLS_PORT + 6,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_cl_server, &ctx);
    xylem_spawn(_cl_closer, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_close_listener(void) {
    xylem_run(_cl_main, NULL, NULL);
}


static void _kl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    if (conn) {
        /* Drain client sync to ensure both sides are handshake-complete. */
        char buf[8];
        xylem_tls_recv(conn, buf, sizeof(buf));
        xylem_tls_close(conn);
    }
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    /* Send a byte so the server knows we are connected. */
    xylem_tls_send(conn, "k", 1);
    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_main(void* arg) {
    (void)arg;
    const char* cert   = "test_tls_kl_cert.pem";
    const char* key    = "test_tls_kl_key.pem";
    const char* keylog = "test_keylog.txt";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);
    ASSERT(xylem_tls_ctx_set_keylog(cli_ctx, keylog) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 7,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_kl_server, &ctx);
    xylem_spawn(_kl_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    FILE* f = fopen(keylog, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    ASSERT(sz > 0);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    remove(keylog);
    xylem_shutdown();
}

static void test_keylog(void) {
    xylem_run(_kl_main, NULL, NULL);
}


int main(void) {
    test_ctx_create_destroy();
    test_load_cert_valid();
    test_load_cert_invalid();
    test_set_ca();
    test_set_verify();
    test_set_alpn();
    test_handshake_and_echo();
    test_handshake_failure();
    test_alpn_negotiation();
    test_framing();
    test_read_deadline();
    test_close();
    test_close_listener();
    test_keylog();
    return 0;
}

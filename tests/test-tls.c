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
#include <stdint.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <string.h>

#define TLS_HOST          "127.0.0.1"
#define TLS_PORT          14433
#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*      ready;
    xylem_channel_t*      gate;
    xylem_waitgroup_t*    wg;
    xylem_tls_ctx_t*      srv_ctx;
    xylem_tls_ctx_t*      cli_ctx;
    xylem_tls_ctx_t*      good_ctx;
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

static int _gen_self_signed_ex(const char* cert_path, const char* key_path,
                               const char* cn, const char* san) {
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
                               (const unsigned char*)cn, -1, -1, 0);
    X509_set_issuer_name(x509, name);

    /* SAN required by OpenSSL 3.x for hostname verification. */
    X509_EXTENSION* ext_san = X509V3_EXT_nconf_nid(
        NULL, NULL, NID_subject_alt_name, san);
    if (ext_san) {
        X509_add_ext(x509, ext_san, -1);
        X509_EXTENSION_free(ext_san);
    }

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

static int _gen_self_signed(const char* cert_path, const char* key_path) {
    return _gen_self_signed_ex(cert_path, key_path, "localhost",
                               "DNS:localhost,IP:127.0.0.1");
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
    ASSERT(xylem_tls_ctx_load_cert(ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

static void test_load_cert_invalid(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, NULL, "nonexistent.pem",
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
    xylem_tls_ctx_verify_server(ctx, true);
    xylem_tls_ctx_verify_server(ctx, false);
    xylem_tls_ctx_verify_client(ctx, true);
    xylem_tls_ctx_verify_client(ctx, false);
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
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

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
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

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


static void _fail_bad_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    /* cli_ctx verifies against the wrong CA, so the handshake fails. */
    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn == NULL);

    /* Release the good client only after our failed handshake has been
     * driven, so the server sees the bad connection first. */
    xylem_channel_send(ctx->gate, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_good_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->gate);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->good_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "after-failure";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);
    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    /**
     * Regression: a first client whose handshake fails must NOT tear
     * down the accept loop. accept() must drop the bad connection
     * internally and go on to return the subsequent good client.
     * Before the fix, accept() returned NULL on the failed handshake,
     * which stops real HTTPS/WSS servers dead on a single bad
     * ClientHello.
     */
    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_close(conn);
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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);

    /* Bad client: requires peer cert signed by cert2, server uses cert. */
    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(cli_ctx, cert2) == 0);

    /* Good client: does not verify, so its handshake succeeds. */
    xylem_tls_ctx_t* good_ctx = xylem_tls_ctx_create();
    ASSERT(good_ctx != NULL);
    xylem_tls_ctx_verify_server(good_ctx, false);

    _ctx_t ctx = {
        .ready    = xylem_channel_create(),
        .gate     = xylem_channel_create(),
        .wg       = xylem_waitgroup_create(),
        .srv_ctx  = srv_ctx,
        .cli_ctx  = cli_ctx,
        .good_ctx = good_ctx,
        .port     = TLS_PORT + 1,
    };
    xylem_waitgroup_add(ctx.wg, 3);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_fail_server, &ctx);
    xylem_spawn(_fail_bad_client, &ctx);
    xylem_spawn(_fail_good_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_tls_ctx_destroy(good_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_channel_destroy(ctx.gate);
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
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_tls_write(conn, buf, n);
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
    ASSERT(xylem_tls_write(conn, "ok", 2) == 0);
    char buf[8];
    xylem_tls_read(conn, buf, sizeof(buf));

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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(srv_ctx, protos, 2) == 0);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);
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
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

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
        xylem_tls_read(conn, buf, sizeof(buf));
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
    xylem_tls_write(conn, "x", 1);
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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

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
        xylem_tls_read(conn, buf, sizeof(buf));
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
    xylem_tls_write(conn, "k", 1);
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
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);
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

static void _sni_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _sni_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_opts_t opts = {0};
    opts.server_name = "localhost";

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, &opts);
    ASSERT(conn != NULL);

    const char* msg = "sni-ok";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _sni_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_sni_cert.pem";
    const char* key  = "test_tls_sni_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 8,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_sni_server, &ctx);
    xylem_spawn(_sni_client, &ctx);
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

static void test_sni_hostname(void) {
    xylem_run(_sni_main, NULL, NULL);
}

/**
 * Per-host SNI certificate selection. The server loads a default cert
 * (CN=localhost) plus a per-host cert (CN=sni.example) chosen via SNI.
 * Each client trusts ONLY one of the two certs and verifies the peer
 * identity, so a successful handshake proves the server returned the
 * specific cert that SNI should have selected -- not merely that some
 * handshake completed.
 */
static void _sni_sel_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    /* One connection per client case; echo each payload back. */
    for (int i = 0; i < 2; i++) {
        xylem_tls_conn_t* conn = xylem_tls_accept(ln);
        ASSERT(conn != NULL);

        char buf[64];
        int n = xylem_tls_read(conn, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(xylem_tls_write(conn, buf, n) == 0);
        xylem_tls_close(conn);
    }

    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _sni_sel_roundtrip(xylem_tls_ctx_t* cctx, uint16_t port,
                               const char* sni, const char* msg) {
    xylem_tls_opts_t opts = {0};
    opts.server_name = sni;

    xylem_tls_conn_t* conn = xylem_tls_dial(TLS_HOST, port, cctx, &opts);
    ASSERT(conn != NULL);

    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);
    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
}

static void _sni_sel_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    /**
     * Case 1: SNI "sni.example" must select the per-host cert. cli_ctx
     * trusts only that cert's CA and verifies identity "sni.example";
     * if the server wrongly served the default cert the handshake would
     * fail on both CA trust and hostname mismatch.
     */
    _sni_sel_roundtrip(ctx->cli_ctx, ctx->port, "sni.example", "host-cert");

    /**
     * Case 2: SNI "localhost" matches no per-host entry, so the server
     * falls back to the default cert. good_ctx trusts only the default
     * cert's CA and verifies identity "localhost".
     */
    _sni_sel_roundtrip(ctx->good_ctx, ctx->port, "localhost", "default-cert");

    xylem_waitgroup_done(ctx->wg);
}

static void _sni_sel_main(void* arg) {
    (void)arg;
    const char* def_cert  = "test_tls_snisel_def_cert.pem";
    const char* def_key   = "test_tls_snisel_def_key.pem";
    const char* host_cert = "test_tls_snisel_host_cert.pem";
    const char* host_key  = "test_tls_snisel_host_key.pem";
    ASSERT(_gen_self_signed(def_cert, def_key) == 0);
    ASSERT(_gen_self_signed_ex(host_cert, host_key,
                               "sni.example", "DNS:sni.example") == 0);

    /* Server: default cert plus an SNI-selected per-host cert. */
    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, def_cert, def_key) == 0);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, "sni.example",
                                   host_cert, host_key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    /* Client for case 1: trusts only the per-host cert. */
    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(cli_ctx, host_cert) == 0);

    /* Client for case 2: trusts only the default cert. */
    xylem_tls_ctx_t* good_ctx = xylem_tls_ctx_create();
    ASSERT(good_ctx != NULL);
    xylem_tls_ctx_verify_server(good_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(good_ctx, def_cert) == 0);

    _ctx_t ctx = {
        .ready    = xylem_channel_create(),
        .wg       = xylem_waitgroup_create(),
        .srv_ctx  = srv_ctx,
        .cli_ctx  = cli_ctx,
        .good_ctx = good_ctx,
        .port     = TLS_PORT + 9,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_sni_sel_server, &ctx);
    xylem_spawn(_sni_sel_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_tls_ctx_destroy(good_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(def_cert);
    remove(def_key);
    remove(host_cert);
    remove(host_key);
    xylem_shutdown();
}

static void test_sni_cert_selection(void) {
    xylem_run(_sni_sel_main, NULL, NULL);
}

static void _addr_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char host[46];
    uint16_t port;
    ASSERT(xylem_tls_remote_addr(conn, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port != 0);

    char buf[256];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    char host[46];
    uint16_t port;
    ASSERT(xylem_tls_local_addr(conn, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port != 0);

    const char* msg = "addr-ok";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);
    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_addr_cert.pem";
    const char* key  = "test_tls_addr_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 9,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_addr_server, &ctx);
    xylem_spawn(_addr_client, &ctx);
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

static void test_remote_addr(void) {
    xylem_run(_addr_main, NULL, NULL);
}

static void _conc_send_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_send_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_tls_write(conn, "hello", 5) == 0);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_send_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_cs_cert.pem";
    const char* key  = "test_tls_cs_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 10,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_conc_send_server, &ctx);
    xylem_spawn(_conc_send_client, &ctx);
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

static void test_concurrent_send(void) {
    xylem_run(_conc_send_main, NULL, NULL);
}

static void _conc_close_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    xylem_sleep(2000);
    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_close_closer(void* arg) {
    xylem_tls_conn_t* conn = (xylem_tls_conn_t*)arg;
    xylem_sleep(100);
    xylem_tls_close(conn);
}

static void _conc_close_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_spawn(_conc_close_closer, conn);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_waitgroup_done(ctx->wg);
}

static void _conc_close_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_cc_cert.pem";
    const char* key  = "test_tls_cc_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 11,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_conc_close_server, &ctx);
    xylem_spawn(_conc_close_client, &ctx);
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

static void test_concurrent_close(void) {
    xylem_run(_conc_close_main, NULL, NULL);
}

/**
 * Close listener while an accepted connection is still active.
 * The connection must remain usable after listener close.
 */
static void _clac_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    /* Close listener while connection is alive. */
    xylem_tls_close_listener(ln);

    /* Connection still works. */
    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "ping", 4) == 0);
    ASSERT(xylem_tls_write(conn, "pong", 4) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _clac_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    /* Give server time to close listener. */
    xylem_sleep(50);
    ASSERT(xylem_tls_write(conn, "ping", 4) == 0);

    char buf[64];
    int n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "pong", 4) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _clac_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_clac_cert.pem";
    const char* key  = "test_tls_clac_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 12,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_clac_server, &ctx);
    xylem_spawn(_clac_client, &ctx);
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

static void test_close_listener_with_active_conn(void) {
    xylem_run(_clac_main, NULL, NULL);
}

/**
 * Full-duplex regression: one connection is read by one coroutine and
 * written by another at the same time. Before the memory-BIO rewrite,
 * concurrent SSL_read/SSL_write on a single SSL aborted the process
 * (iowait one-parker-per-direction violation) and raced the SSL state.
 * The two client coroutines share a single connection through a small
 * fanout struct; the server echoes every byte back.
 */
#define FDX_MSG_COUNT 200
#define FDX_MSG_SIZE  300

typedef struct {
    xylem_tls_conn_t*  conn;
    xylem_waitgroup_t* wg;
    int                ok;
} _fdx_share_t;

static void _fdx_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln = xylem_tls_listen(
        TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    /* Echo every byte until the peer closes (read returns <= 0). */
    char    buf[1024];
    int64_t echoed = 0;
    int64_t total  = (int64_t)FDX_MSG_COUNT * FDX_MSG_SIZE;
    while (echoed < total) {
        int n = xylem_tls_read(conn, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (xylem_tls_write(conn, buf, n) != 0) {
            break;
        }
        echoed += n;
    }

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fdx_writer(void* arg) {
    _fdx_share_t* sh = (_fdx_share_t*)arg;
    char msg[FDX_MSG_SIZE];
    memset(msg, 'a', sizeof(msg));

    for (int i = 0; i < FDX_MSG_COUNT; i++) {
        if (xylem_tls_write(sh->conn, msg, (int)sizeof(msg)) != 0) {
            sh->ok = 0;
            break;
        }
    }
    xylem_waitgroup_done(sh->wg);
}

static void _fdx_reader(void* arg) {
    _fdx_share_t* sh = (_fdx_share_t*)arg;
    int64_t want = (int64_t)FDX_MSG_COUNT * FDX_MSG_SIZE;
    int64_t got  = 0;
    char    buf[1024];

    while (got < want) {
        int n = xylem_tls_read(sh->conn, buf, sizeof(buf));
        if (n <= 0) {
            sh->ok = 0;
            break;
        }
        got += n;
    }
    ASSERT(got == want);
    xylem_waitgroup_done(sh->wg);
}

static void _fdx_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn = xylem_tls_dial(
        TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    /**
     * Reader and writer drive the same connection concurrently. They
     * get their own waitgroup so we can join them before closing the
     * shared connection exactly once.
     */
    xylem_waitgroup_t* io_wg = xylem_waitgroup_create();
    _fdx_share_t sh = { .conn = conn, .wg = io_wg, .ok = 1 };
    xylem_waitgroup_add(io_wg, 2);
    xylem_spawn(_fdx_reader, &sh);
    xylem_spawn(_fdx_writer, &sh);
    xylem_waitgroup_wait(io_wg);
    xylem_waitgroup_destroy(io_wg);
    ASSERT(sh.ok == 1);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _fdx_main(void* arg) {
    (void)arg;
    const char* cert = "test_tls_fdx_cert.pem";
    const char* key  = "test_tls_fdx_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 13,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_fdx_server, &ctx);
    xylem_spawn(_fdx_client, &ctx);
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

static void test_full_duplex(void) {
    xylem_run(_fdx_main, NULL, NULL);
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
    test_read_deadline();
    test_close();
    test_close_listener();
    test_keylog();
    test_sni_hostname();
    test_sni_cert_selection();
    test_remote_addr();
    test_concurrent_send();
    test_concurrent_close();
    test_close_listener_with_active_conn();
    test_full_duplex();
    return 0;
}

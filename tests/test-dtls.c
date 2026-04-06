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

#include "assert.h"
#include "xylem.h"
#include "xylem/xylem-dtls.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

#define DTLS_PORT          15433
#define SAFETY_TIMEOUT_MS  10000

typedef struct {
    xylem_loop_t*          loop;
    xylem_dtls_server_t*   dtls_server;
    xylem_dtls_t*          srv_session;
    xylem_dtls_t*          cli_session;
    xylem_dtls_ctx_t*      srv_ctx;
    xylem_dtls_ctx_t*      cli_ctx;
    int                    accept_called;
    int                    connect_called;
    int                    close_called;
    int                    read_count;
    int                    verified;
    int                    value;
    int                    send_result;
    char                   received[256];
    size_t                 received_len;
} _test_ctx_t;

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

    FILE* f = fopen(cert_path, "wb");
    if (!f) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }
    PEM_write_X509(f, x509);
    fclose(f);

    f = fopen(key_path, "wb");
    if (!f) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }
    PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
    fclose(f);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return 0;
}

/* Shared callbacks. */

static void _safety_timeout_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

/**
 * Shared server accept callback. DTLS on_accept only receives the
 * session handle (no server pointer), so the test must arrange for
 * userdata to be set before this fires, or use a test-specific
 * wrapper that locates _test_ctx_t and calls this helper.
 */
static void _dtls_srv_accept_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        ctx->srv_session = dtls;
        ctx->accept_called = 1;
    }
}

static void _dtls_srv_read_echo_cb(xylem_dtls_t* dtls,
                                    void* data, size_t len) {
    xylem_dtls_send(dtls, data, len);
}

/* ── Context management API tests ── */

static void test_ctx_create_destroy(void) {
    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_dtls_ctx_destroy(ctx);
}

static void test_load_cert_valid(void) {
    const char* cert = "test_dtls_cert.pem";
    const char* key  = "test_dtls_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx, cert, key) == 0);
    xylem_dtls_ctx_destroy(ctx);

    remove(cert);
    remove(key);
}

static void test_load_cert_invalid(void) {
    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx, "nonexistent.pem",
                                    "nonexistent.pem") == -1);
    xylem_dtls_ctx_destroy(ctx);
}

static void test_set_ca(void) {
    const char* cert = "test_dtls_ca.pem";
    const char* key  = "test_dtls_ca_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_dtls_ctx_set_ca(ctx, cert) == 0);
    xylem_dtls_ctx_destroy(ctx);

    remove(cert);
    remove(key);
}

static void test_set_verify(void) {
    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx, true);
    xylem_dtls_ctx_set_verify(ctx, false);
    xylem_dtls_ctx_destroy(ctx);
}

static void test_set_alpn(void) {
    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_dtls_ctx_set_alpn(ctx, protos, 2) == 0);
    xylem_dtls_ctx_destroy(ctx);
}

/* ── Handshake and data transfer callbacks ── */

static void _echo_cli_connect_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    ctx->connect_called = 1;
    xylem_dtls_send(dtls, "hello", 5);
}

static void _echo_cli_read_cb(xylem_dtls_t* dtls,
                                void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (len <= sizeof(ctx->received)) {
        memcpy(ctx->received, data, len);
        ctx->received_len = len;
    }
    ctx->read_count++;
    xylem_dtls_close(dtls);
}

static void _echo_cli_close_cb(xylem_dtls_t* dtls, int err,
                               const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        ctx->close_called++;
        xylem_dtls_close_server(ctx->dtls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_handshake_and_echo(void) {
    const char* cert = "test_dtls_hs_cert.pem";
    const char* key  = "test_dtls_hs_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, false);

    /**
     * DTLS on_accept only receives the new session handle (no server
     * pointer), so the new session has NULL userdata when on_accept
     * fires. The server handler uses on_read = echo only; accept_called
     * is verified indirectly through the successful echo round trip.
     */
    xylem_dtls_handler_t srv_handler = {
        .on_read = _dtls_srv_read_echo_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    xylem_dtls_handler_t cli_handler = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
        .on_close   = _echo_cli_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.read_count >= 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}

/* ── Handshake failure callbacks ── */

static void _fail_cli_close_cb(xylem_dtls_t* dtls, int err,
                               const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        ctx->close_called++;
        xylem_dtls_close_server(ctx->dtls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_handshake_failure_wrong_ca(void) {
    const char* cert  = "test_dtls_fail_cert.pem";
    const char* key   = "test_dtls_fail_key.pem";
    const char* cert2 = "test_dtls_fail_cert2.pem";
    const char* key2  = "test_dtls_fail_key2.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);
    ASSERT(_gen_self_signed(cert2, key2) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    /* Server uses cert A, no client verification. */
    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);

    xylem_dtls_handler_t srv_handler = {0};

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    /* Client enables verification, using cert B as CA (wrong CA). */
    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, true);
    ASSERT(xylem_dtls_ctx_set_ca(ctx.cli_ctx, cert2) == 0);

    xylem_dtls_handler_t cli_handler = {
        .on_close = _fail_cli_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called >= 1);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
    remove(cert2);
    remove(key2);
}

/* ── ALPN negotiation callbacks ── */

static void _alpn_cli_connect_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    ctx->connect_called = 1;

    const char* alpn = xylem_dtls_get_alpn(dtls);
    ASSERT(alpn != NULL);
    ASSERT(memcmp(alpn, "h2", 2) == 0);
    ctx->verified = 1;

    xylem_dtls_close(dtls);
}

static void _alpn_cli_close_cb(xylem_dtls_t* dtls, int err,
                               const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        ctx->close_called++;
        xylem_dtls_close_server(ctx->dtls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_alpn_negotiation(void) {
    const char* cert = "test_dtls_alpn_cert.pem";
    const char* key  = "test_dtls_alpn_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    const char* protos[] = {"h2", "http/1.1"};

    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);
    ASSERT(xylem_dtls_ctx_set_alpn(ctx.srv_ctx, protos, 2) == 0);

    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, false);
    ASSERT(xylem_dtls_ctx_set_alpn(ctx.cli_ctx, protos, 2) == 0);

    xylem_dtls_handler_t srv_handler = {0};

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    xylem_dtls_handler_t cli_handler = {
        .on_connect = _alpn_cli_connect_cb,
        .on_close   = _alpn_cli_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}

/* ── Session userdata callbacks ── */

static void _ud_cli_connect_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);

    xylem_dtls_set_userdata(dtls, &ctx->value);
    void* got = xylem_dtls_get_userdata(dtls);
    ASSERT(got == &ctx->value);
    ASSERT(*(int*)got == 42);
    ctx->verified = 1;

    /* Restore ctx so close callback can use it. */
    xylem_dtls_set_userdata(dtls, ctx);
    xylem_dtls_close(dtls);
}

static void _ud_cli_close_cb(xylem_dtls_t* dtls, int err,
                             const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        xylem_dtls_close_server(ctx->dtls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_session_userdata(void) {
    const char* cert = "test_dtls_ud_cert.pem";
    const char* key  = "test_dtls_ud_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop  = xylem_loop_create();
    ctx.value = 42;
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_dtls_handler_t srv_handler = {0};

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    xylem_dtls_handler_t cli_handler = {
        .on_connect = _ud_cli_connect_cb,
        .on_close   = _ud_cli_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}

/* ── send_after_close callbacks ── */

static void _sac_connect_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    xylem_dtls_close(dtls);
    ctx->send_result = xylem_dtls_send(dtls, "x", 1);
    ctx->verified = 1;
}

static void _sac_close_cb(xylem_dtls_t* dtls, int err,
                          const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        xylem_dtls_close_server(ctx->dtls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_send_after_close(void) {
    const char* cert = "test_dtls_sac_cert.pem";
    const char* key  = "test_dtls_sac_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_dtls_handler_t srv_handler = {0};

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    xylem_dtls_handler_t cli_handler = {
        .on_connect = _sac_connect_cb,
        .on_close   = _sac_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);
    ASSERT(ctx.send_result == -1);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}

/* ── close_server_with_active_session callbacks ── */

static void _csas_close_timer_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_dtls_close_server(ctx->dtls_server);
    ctx->dtls_server = NULL;
}

static void _csas_cli_connect_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    ctx->connect_called = 1;
}

static void _csas_cli_close_cb(xylem_dtls_t* dtls, int err,
                               const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        ctx->close_called++;
        xylem_loop_stop(ctx->loop);
    }
}

static void test_close_server_with_active_session(void) {
    const char* cert = "test_dtls_csas_cert.pem";
    const char* key  = "test_dtls_csas_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_dtls_handler_t srv_handler = {0};

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    /* Timer fires after handshake to close the server. */
    xylem_loop_timer_t* close_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(close_timer, _csas_close_timer_cb, &ctx,
                           200, 0);

    xylem_dtls_handler_t cli_handler = {
        .on_connect = _csas_cli_connect_cb,
        .on_close   = _csas_cli_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called >= 1);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(close_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}

/* ── Keylog callbacks ── */

static void _keylog_cli_connect_cb(xylem_dtls_t* dtls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    ctx->connect_called = 1;
    xylem_dtls_close(dtls);
}

static void _keylog_cli_close_cb(xylem_dtls_t* dtls, int err,
                                 const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_dtls_get_userdata(dtls);
    if (ctx) {
        ctx->close_called++;
        xylem_dtls_close_server(ctx->dtls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_keylog_write(void) {
    const char* cert   = "test_dtls_kl_cert.pem";
    const char* key    = "test_dtls_kl_key.pem";
    const char* keylog = "test_dtls_keylog.txt";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_dtls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_dtls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_dtls_ctx_set_verify(ctx.cli_ctx, false);
    ASSERT(xylem_dtls_ctx_set_keylog(ctx.cli_ctx, keylog) == 0);

    xylem_dtls_handler_t srv_handler = {0};

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", DTLS_PORT, &addr);

    ctx.dtls_server = xylem_dtls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                        &srv_handler);
    ASSERT(ctx.dtls_server != NULL);

    xylem_dtls_handler_t cli_handler = {
        .on_connect = _keylog_cli_connect_cb,
        .on_close   = _keylog_cli_close_cb,
    };

    ctx.cli_session = xylem_dtls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                      &cli_handler);
    ASSERT(ctx.cli_session != NULL);
    xylem_dtls_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);

    /* Verify keylog file is non-empty. */
    FILE* f = fopen(keylog, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    ASSERT(sz > 0);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
    remove(keylog);
}

int main(void) {
    xylem_startup();

    test_ctx_create_destroy();
    test_load_cert_valid();
    test_load_cert_invalid();
    test_set_ca();
    test_set_verify();
    test_set_alpn();
    test_handshake_and_echo();
    test_handshake_failure_wrong_ca();
    test_alpn_negotiation();
    test_session_userdata();
    test_send_after_close();
    test_close_server_with_active_session();
    test_keylog_write();

    xylem_cleanup();
    return 0;
}

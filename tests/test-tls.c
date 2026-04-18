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
#include "xylem/xylem-tls.h"
#include "assert.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

#define TLS_PORT          14433
#define TLS_HOST          "127.0.0.1"
#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_loop_t*          loop;
    xylem_tls_server_t*    tls_server;
    xylem_tls_conn_t*      srv_conn;
    xylem_tls_conn_t*      cli_conn;
    xylem_tls_ctx_t*       srv_ctx;
    xylem_tls_ctx_t*       cli_ctx;
    int                    accept_called;
    int                    connect_called;
    int                    close_called;
    int                    read_count;
    int                    wd_called;
    int                    wd_status;
    int                    timeout_called;
    int                    timeout_type;
    int                    heartbeat_called;
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

static void _post_stop_cb(xylem_loop_t* loop,
                           xylem_loop_post_t* req,
                           void* ud) {
    (void)req;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _tls_srv_accept_cb(xylem_tls_server_t* server,
                                xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);
    ctx->srv_conn = tls;
    ctx->accept_called = 1;
    xylem_tls_set_userdata(tls, ctx);
}

static void _tls_srv_read_echo_cb(xylem_tls_conn_t* tls,
                                   void* data, size_t len) {
    xylem_tls_send(tls, data, len);
}

/* Generic on_close that stops the loop. Used by tests where the client
   connection is the last resource to be cleaned up. */
static void _tls_stop_on_close_cb(xylem_tls_conn_t* tls, int err,
                                   const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_loop_stop(ctx->loop);
    }
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


static void _echo_srv_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        ctx->close_called++;
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void _echo_cli_connect_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    ctx->connect_called = 1;
    xylem_tls_send(tls, "hello", 5);
}

static void _echo_cli_write_done_cb(xylem_tls_conn_t* tls,
                                     const void* data, size_t len,
                                     int status) {
    (void)data;
    (void)len;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    ctx->wd_called = 1;
    ctx->wd_status = status;
}

static void _echo_cli_read_cb(xylem_tls_conn_t* tls,
                                void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (len <= sizeof(ctx->received)) {
        memcpy(ctx->received, data, len);
        ctx->received_len = len;
    }
    ctx->read_count++;
    xylem_tls_close(tls);
}

static void _echo_cli_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx && ctx->srv_conn) {
        xylem_tls_close(ctx->srv_conn);
    }
}

static void test_handshake_and_echo(void) {
    const char* cert = "test_tls_hs_cert.pem";
    const char* key  = "test_tls_hs_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _tls_srv_accept_cb,
        .on_read   = _tls_srv_read_echo_cb,
        .on_close  = _echo_srv_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_connect    = _echo_cli_connect_cb,
        .on_read       = _echo_cli_read_cb,
        .on_write_done = _echo_cli_write_done_cb,
        .on_close      = _echo_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.wd_called == 1);
    ASSERT(ctx.wd_status == 0);
    ASSERT(ctx.read_count >= 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _fail_cli_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        ctx->close_called++;
        if (ctx->srv_conn) {
            xylem_tls_close(ctx->srv_conn);
        } else {
            xylem_tls_close_server(ctx->tls_server);
            xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
        }
    }
}

static void _fail_srv_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        ctx->close_called++;
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void _fail_srv_accept_cb(xylem_tls_server_t* server,
                                 xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);
    ctx->srv_conn = tls;
    xylem_tls_set_userdata(tls, ctx);
}

static void test_handshake_failure_wrong_ca(void) {
    const char* cert  = "test_tls_fail_cert.pem";
    const char* key   = "test_tls_fail_key.pem";
    const char* cert2 = "test_tls_fail_cert2.pem";
    const char* key2  = "test_tls_fail_key2.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);
    ASSERT(_gen_self_signed(cert2, key2) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _fail_srv_accept_cb,
        .on_close  = _fail_srv_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    /* Client with verification enabled, using wrong CA. */
    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(ctx.cli_ctx, cert2) == 0);

    xylem_tls_handler_t cli_handler = {
        .on_close = _fail_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called >= 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
    remove(cert2);
    remove(key2);
}


static void _alpn_srv_accept_cb(xylem_tls_server_t* server,
                                 xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);
    ctx->srv_conn = tls;
    ctx->accept_called = 1;
    xylem_tls_set_userdata(tls, ctx);
}

static void _alpn_cli_connect_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    ctx->connect_called = 1;

    const char* alpn = xylem_tls_get_alpn(tls);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);
    ctx->verified = 1;

    xylem_tls_close(tls);
}

static void _alpn_cli_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx && ctx->srv_conn) {
        xylem_tls_close(ctx->srv_conn);
    } else if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void _alpn_srv_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_alpn_negotiation(void) {
    const char* cert = "test_tls_alpn_cert.pem";
    const char* key  = "test_tls_alpn_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    const char* protos[] = {"h2", "http/1.1"};

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(ctx.srv_ctx, protos, 2) == 0);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);
    ASSERT(xylem_tls_ctx_set_alpn(ctx.cli_ctx, protos, 2) == 0);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _alpn_srv_accept_cb,
        .on_close  = _alpn_srv_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_connect = _alpn_cli_connect_cb,
        .on_close   = _alpn_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void test_sni_hostname(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);

    xylem_tls_handler_t handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_tls_opts_t opts = {0};
    opts.hostname = "example.com";

    xylem_tls_conn_t* tls = xylem_tls_dial(loop, &addr, ctx,
                                            &handler, &opts);
    ASSERT(tls != NULL);

    xylem_tls_close(tls);
    xylem_loop_run(loop);

    xylem_tls_ctx_destroy(ctx);
    xylem_loop_destroy(loop);
}


static void _ud_cli_connect_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);

    xylem_tls_set_userdata(tls, &ctx->value);
    void* got = xylem_tls_get_userdata(tls);
    ASSERT(got == &ctx->value);
    ASSERT(*(int*)got == 42);
    ctx->verified = 1;

    /* Restore ctx so close callback can use it. */
    xylem_tls_set_userdata(tls, ctx);
    xylem_tls_close(tls);
}

static void _ud_cli_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void test_conn_userdata(void) {
    const char* cert = "test_tls_ud_cert.pem";
    const char* key  = "test_tls_ud_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop  = xylem_loop_create();
    ctx.value = 42;
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _tls_srv_accept_cb,
        .on_close  = _tls_stop_on_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_connect = _ud_cli_connect_cb,
        .on_close   = _ud_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _srv_ud_accept_cb(xylem_tls_server_t* server,
                                xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);
    ASSERT(ctx != NULL);
    ASSERT(ctx->value == 99);
    ctx->verified = 1;
    ctx->srv_conn = tls;
    xylem_tls_set_userdata(tls, ctx);
    xylem_tls_close(tls);
}

static void _srv_ud_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void test_server_userdata(void) {
    const char* cert = "test_tls_srvud_cert.pem";
    const char* key  = "test_tls_srvud_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop  = xylem_loop_create();
    ctx.value = 99;
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _srv_ud_accept_cb,
        .on_close  = _srv_ud_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    /* Verify round-trip before any callback. */
    void* got = xylem_tls_server_get_userdata(ctx.tls_server);
    ASSERT(got == &ctx);
    ASSERT(((_test_ctx_t*)got)->value == 99);

    xylem_tls_handler_t cli_handler = {
        .on_close = _tls_stop_on_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _peer_addr_accept_cb(xylem_tls_server_t* server,
                                  xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);

    const xylem_addr_t* peer = xylem_tls_get_peer_addr(tls);
    ASSERT(peer != NULL);

    const struct sockaddr* sa = (const struct sockaddr*)&peer->storage;
    ASSERT(sa->sa_family == AF_INET);

    ctx->verified = 1;
    ctx->srv_conn = tls;
    xylem_tls_set_userdata(tls, ctx);
    xylem_tls_close(tls);
}

static void _peer_addr_srv_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void test_peer_addr(void) {
    const char* cert = "test_tls_peer_cert.pem";
    const char* key  = "test_tls_peer_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _peer_addr_accept_cb,
        .on_close  = _peer_addr_srv_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_close = _tls_stop_on_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _get_loop_connect_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);

    xylem_loop_t* got = xylem_tls_get_loop(tls);
    ASSERT(got == ctx->loop);
    ctx->verified = 1;

    xylem_tls_close(tls);
}

static void _get_loop_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void test_get_loop(void) {
    const char* cert = "test_tls_loop_cert.pem";
    const char* key  = "test_tls_loop_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _tls_srv_accept_cb,
        .on_close  = _tls_stop_on_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_connect = _get_loop_connect_cb,
        .on_close   = _get_loop_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _close_active_timer_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer,
                                    void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_tls_close_server(ctx->tls_server);
    ctx->tls_server = NULL;
}

static void _close_active_accept_cb(xylem_tls_server_t* server,
                                     xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);
    ctx->srv_conn = tls;
    xylem_tls_set_userdata(tls, ctx);
}

static void _close_active_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        ctx->close_called++;
    }
}

static void test_close_server_with_active_conn(void) {
    const char* cert = "test_tls_csac_cert.pem";
    const char* key  = "test_tls_csac_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _close_active_accept_cb,
        .on_close  = _close_active_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    /* Timer fires after accept to close the server. */
    xylem_loop_timer_t* close_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(close_timer, _close_active_timer_cb, &ctx, 200, 0);

    xylem_tls_handler_t cli_handler = {
        .on_close = _tls_stop_on_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called >= 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(close_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _sac_connect_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    xylem_tls_close(tls);
    ctx->send_result = xylem_tls_send(tls, "x", 1);
    ctx->verified = 1;
}

static void _sac_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void test_send_after_close(void) {
    const char* cert = "test_tls_sac_cert.pem";
    const char* key  = "test_tls_sac_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _tls_srv_accept_cb,
        .on_close  = _tls_stop_on_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_connect = _sac_connect_cb,
        .on_close   = _sac_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);
    ASSERT(ctx.send_result == -1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _keylog_cli_connect_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    ctx->connect_called = 1;
    xylem_tls_close(tls);
}

static void _keylog_cli_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx && ctx->srv_conn) {
        xylem_tls_close(ctx->srv_conn);
    }
}

static void _keylog_srv_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_keylog_write(void) {
    const char* cert   = "test_tls_kl_cert.pem";
    const char* key    = "test_tls_kl_key.pem";
    const char* keylog = "test_keylog.txt";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);
    ASSERT(xylem_tls_ctx_set_keylog(ctx.cli_ctx, keylog) == 0);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _tls_srv_accept_cb,
        .on_close  = _keylog_srv_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, NULL);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    xylem_tls_handler_t cli_handler = {
        .on_connect = _keylog_cli_connect_cb,
        .on_close   = _keylog_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);

    /* Verify keylog file is non-empty. */
    FILE* f = fopen(keylog, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    ASSERT(sz > 0);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
    remove(keylog);
}


static void _timeout_srv_accept_cb(xylem_tls_server_t* server,
                                    xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_tls_server_get_userdata(server);
    ctx->srv_conn = tls;
    xylem_tls_set_userdata(tls, ctx);
}

static void _timeout_cb(xylem_tls_conn_t* tls,
                         xylem_tcp_timeout_type_t type) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        ctx->timeout_called = 1;
        ctx->timeout_type = (int)type;
        xylem_tls_close(tls);
    }
}

static void _timeout_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void _timeout_cli_close_cb(xylem_tls_conn_t* tls, int err,
                                   const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_loop_stop(ctx->loop);
    }
}

static void test_read_timeout(void) {
    const char* cert = "test_tls_rto_cert.pem";
    const char* key  = "test_tls_rto_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    /* Server with read timeout on accepted connections. */
    xylem_tls_opts_t srv_opts = {0};
    srv_opts.tcp.read_timeout_ms = 100;

    xylem_tls_handler_t srv_handler = {
        .on_accept  = _timeout_srv_accept_cb,
        .on_timeout = _timeout_cb,
        .on_close   = _timeout_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, &srv_opts);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    /* Client connects but sends nothing. */
    xylem_tls_handler_t cli_handler = {
        .on_close = _timeout_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.timeout_called == 1);
    ASSERT(ctx.timeout_type == (int)XYLEM_TCP_TIMEOUT_READ);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
}


static void _heartbeat_miss_cb(xylem_tls_conn_t* tls) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        ctx->heartbeat_called = 1;
        xylem_tls_close(tls);
    }
}

static void _heartbeat_close_cb(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_tls_close_server(ctx->tls_server);
        xylem_loop_post(ctx->loop, _post_stop_cb, NULL);
    }
}

static void _heartbeat_cli_close_cb(xylem_tls_conn_t* tls, int err,
                                     const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_tls_get_userdata(tls);
    if (ctx) {
        xylem_loop_stop(ctx->loop);
    }
}

static void test_heartbeat_miss(void) {
    const char* cert = "test_tls_hb_cert.pem";
    const char* key  = "test_tls_hb_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    ctx.srv_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx.srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(ctx.srv_ctx, false);

    ctx.cli_ctx = xylem_tls_ctx_create();
    ASSERT(ctx.cli_ctx != NULL);
    xylem_tls_ctx_set_verify(ctx.cli_ctx, false);

    /* Server with heartbeat on accepted connections. */
    xylem_tls_opts_t srv_opts = {0};
    srv_opts.tcp.heartbeat_ms = 100;

    xylem_tls_handler_t srv_handler = {
        .on_accept         = _timeout_srv_accept_cb,
        .on_heartbeat_miss = _heartbeat_miss_cb,
        .on_close          = _heartbeat_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(TLS_HOST, TLS_PORT, &addr);

    ctx.tls_server = xylem_tls_listen(ctx.loop, &addr, ctx.srv_ctx,
                                      &srv_handler, &srv_opts);
    ASSERT(ctx.tls_server != NULL);
    xylem_tls_server_set_userdata(ctx.tls_server, &ctx);

    /* Client connects but sends nothing. */
    xylem_tls_handler_t cli_handler = {
        .on_close = _heartbeat_cli_close_cb,
    };

    ctx.cli_conn = xylem_tls_dial(ctx.loop, &addr, ctx.cli_ctx,
                                  &cli_handler, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_tls_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.heartbeat_called == 1);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);

    remove(cert);
    remove(key);
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
    test_sni_hostname();
    test_conn_userdata();
    test_server_userdata();
    test_peer_addr();
    test_get_loop();
    test_close_server_with_active_conn();
    test_send_after_close();
    test_keylog_write();
    test_read_timeout();
    test_heartbeat_miss();

    xylem_cleanup();
    return 0;
}

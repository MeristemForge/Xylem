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
#define TEST_WITH_TLS
#include "utils.h"

#include <openssl/err.h>
#include <openssl/sslerr.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLS_HOST          "127.0.0.1"
#define TLS_PORT          14433

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_channel_t*   gate;
    xylem_waitgroup_t* wg;
    xylem_tls_ctx_t*   srv_ctx;
    xylem_tls_ctx_t*   cli_ctx;
    xylem_tls_ctx_t*   good_ctx;
    const char*        alpn;
    uint16_t           port;
} _ctx_t;

static xylem_tls_ctx_t* _srv_ctx(const char* cert, const char* key) {
    xylem_tls_ctx_t* c = xylem_tls_ctx_create();
    ASSERT(c != NULL);
    ASSERT(xylem_tls_ctx_load_cert(c, NULL, cert, key) == 0);
    xylem_tls_ctx_verify_client(c, false);
    return c;
}

static xylem_tls_ctx_t* _cli_ctx(void) {
    xylem_tls_ctx_t* c = xylem_tls_ctx_create();
    ASSERT(c != NULL);
    xylem_tls_ctx_verify_server(c, false);
    return c;
}

static void _drive(_ctx_t* ctx, int n, _coro_t a, _coro_t b, _coro_t c) {
    ctx->ready = xylem_channel_create();
    ctx->gate  = xylem_channel_create();
    ctx->wg    = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, n);
    xylem_spawn(a, ctx);
    xylem_spawn(b, ctx);
    if (c) {
        xylem_spawn(c, ctx);
    }
    xylem_waitgroup_wait(ctx->wg);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->gate);
    xylem_channel_destroy(ctx->ready);
}

typedef struct {
    const char* cert;
    const char* key;
    uint16_t    port;
    int         n;
    _coro_t     a;
    _coro_t     b;
} _plan_t;

static void _default_main(void* arg) {
    _plan_t* p = (_plan_t*)arg;
    ASSERT(_utils_cert_gen(p->cert, p->key) == 0);

    _ctx_t ctx = {
        .srv_ctx = _srv_ctx(p->cert, p->key),
        .cli_ctx = _cli_ctx(),
        .port    = p->port,
    };
    _drive(&ctx, p->n, p->a, p->b, NULL);

    xylem_tls_ctx_destroy(ctx.srv_ctx);
    xylem_tls_ctx_destroy(ctx.cli_ctx);
    remove(p->cert);
    remove(p->key);
}

static void _run_default(_plan_t plan) {
    _default_main(&plan);
}

static void test_load_cert_invalid(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, NULL, "nonexistent.pem",
                                   "nonexistent.pem") == -1);
    xylem_tls_ctx_destroy(ctx);
}

static int _encrypt_private_key(const char* key_path) {
    BIO* in = BIO_new_file(key_path, "r");
    if (!in) {
        return -1;
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(in, NULL, NULL, NULL);
    BIO_free(in);
    if (!key) {
        return -1;
    }

    BIO* out = BIO_new_file(key_path, "w");
    if (!out) {
        EVP_PKEY_free(key);
        return -1;
    }
    const unsigned char password[] = "test-password";
    int rc = PEM_write_bio_PrivateKey(
        out, key, EVP_aes_256_cbc(), password, (int)sizeof(password) - 1,
        NULL, NULL) == 1
                 ? 0
                 : -1;
    BIO_free(out);
    EVP_PKEY_free(key);
    return rc;
}

static void test_load_encrypted_private_key(void) {
    const char* cert = "test_tls_encrypted_cert.pem";
    const char* key  = "test_tls_encrypted_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);
    ASSERT(_encrypt_private_key(key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, NULL, cert, key) == -1);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

static void test_load_system_ca(void) {
    const char* ca  = "test_tls_sysca_fallback.pem";
    const char* key = "test_tls_sysca_fallback_key.pem";
    ASSERT(_utils_cert_gen(ca, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_system_ca(ctx, ca) == 0);
    xylem_tls_ctx_destroy(ctx);

    remove(ca);
    remove(key);
}

static void test_set_alpn(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, protos, 2) == 0);

    const char* empty[] = {""};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, empty, 1) == -1);

    char too_long[257];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    const char* oversized[] = {too_long};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, oversized, 1) == -1);

    const char* null_protocol[] = {NULL};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, null_protocol, 1) == -1);
    ASSERT(xylem_tls_ctx_set_alpn(ctx, NULL, 1) == -1);

    xylem_tls_ctx_destroy(ctx);
}

static char* _read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz);
    if (buf) {
        *out_len = fread(buf, 1, (size_t)sz, f);
    }
    fclose(f);
    return buf;
}

static void test_load_cert_mem(void) {
    const char* cert = "test_tls_mem_cert.pem";
    const char* key  = "test_tls_mem_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    size_t cert_len = 0, key_len = 0;
    char*  cert_buf = _read_file(cert, &cert_len);
    char*  key_buf  = _read_file(key, &key_len);
    ASSERT(cert_buf != NULL && key_buf != NULL);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert_mem(ctx, NULL, cert_buf, cert_len,
                                       key_buf, key_len) == 0);
    ASSERT(xylem_tls_ctx_load_cert_mem(ctx, "localhost", cert_buf, cert_len,
                                       key_buf, key_len) == 0);
    ASSERT(xylem_tls_ctx_load_cert_mem(ctx, NULL, cert_buf, cert_len,
                                       NULL, 0) == -1);

    char* terminated = (char*)malloc(cert_len + 1);
    ASSERT(terminated != NULL);
    memcpy(terminated, cert_buf, cert_len);
    terminated[cert_len] = '\0';
    ASSERT(xylem_tls_ctx_load_cert_mem(ctx, NULL, terminated,
                                       (size_t)UINT_MAX, key_buf,
                                       key_len) == -1);

    const char corrupt[] = "\n-----BEGIN CERTIFICATE-----\ninvalid\n"
                           "-----END CERTIFICATE-----\n";
    size_t bad_len = cert_len + sizeof(corrupt) - 1;
    char*  bad_cert = (char*)malloc(bad_len);
    ASSERT(bad_cert != NULL);
    memcpy(bad_cert, cert_buf, cert_len);
    memcpy(bad_cert + cert_len, corrupt, sizeof(corrupt) - 1);
    ASSERT(xylem_tls_ctx_load_cert_mem(ctx, NULL, bad_cert, bad_len,
                                       key_buf, key_len) == -1);
    xylem_tls_ctx_destroy(ctx);

    free(bad_cert);
    free(terminated);
    free(cert_buf);
    free(key_buf);
    remove(cert);
    remove(key);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem tls";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_handshake_and_echo(void) {
    _run_default((_plan_t){"test_tls_echo_cert.pem", "test_tls_echo_key.pem",
                           TLS_PORT, 2, _echo_server, _echo_client});
}

static void _max_timeout_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_opts_t opts = {.connect_timeout_ms = UINT64_MAX};
    xylem_tls_conn_t* conn =
        xylem_tls_dial("localhost", ctx->port, ctx->cli_ctx, &opts);
    ASSERT(conn != NULL);

    const char* msg = "max timeout";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_max_connect_timeout(void) {
    _run_default((_plan_t){"test_tls_maxto_cert.pem",
                           "test_tls_maxto_key.pem", TLS_PORT + 21, 2,
                           _echo_server, _max_timeout_client});
}

static void _fail_bad_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn == NULL);

    xylem_channel_send(ctx->gate, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_good_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->gate);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->good_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "after-failure";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);
    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    for (;;) {
        xylem_tls_conn_t* conn = xylem_tls_accept(ln);
        ASSERT(conn != NULL);

        char buf[64];
        int  n = xylem_tls_read(conn, buf, sizeof(buf));
        if (n <= 0) {
            xylem_tls_destroy(conn);
            continue;
        }
        ASSERT(xylem_tls_write(conn, buf, n) == 0);
        xylem_tls_destroy(conn);
        break;
    }

    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fail_main(void* arg) {
    (void)arg;
    const char* cert  = "test_tls_fail_cert.pem";
    const char* key   = "test_tls_fail_key.pem";
    const char* cert2 = "test_tls_fail_cert2.pem";
    const char* key2  = "test_tls_fail_key2.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);
    ASSERT(_utils_cert_gen(cert2, key2) == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, true);
    ASSERT(xylem_tls_ctx_load_ca(cli_ctx, cert2) == 0);

    xylem_tls_ctx_t* good_ctx = xylem_tls_ctx_create();
    ASSERT(good_ctx != NULL);
    xylem_tls_ctx_verify_server(good_ctx, false);

    _ctx_t ctx = {
        .srv_ctx  = srv_ctx,
        .cli_ctx  = cli_ctx,
        .good_ctx = good_ctx,
        .port     = TLS_PORT + 1,
    };
    _drive(&ctx, 3, _fail_server, _fail_bad_client, _fail_good_client);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_tls_ctx_destroy(good_ctx);
    remove(cert);
    remove(key);
    remove(cert2);
    remove(key2);
}

static void test_handshake_failure(void) {
    _fail_main(NULL);
}

static void _alpn_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_handshake(conn) == 0);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, ctx->alpn) == 0);

    char buf[8];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_tls_write(conn, buf, n);
    }

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, ctx->alpn) == 0);

    ASSERT(xylem_tls_write(conn, "ok", 2) == 0);
    char buf[8];
    xylem_tls_read(conn, buf, sizeof(buf));

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _run_alpn(
    const char** protocols,
    size_t       count,
    const char*  expected,
    uint16_t     port) {
    const char* cert = "test_tls_alpn_cert.pem";
    const char* key  = "test_tls_alpn_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = _srv_ctx(cert, key);
    ASSERT(xylem_tls_ctx_set_alpn(srv_ctx, protocols, count) == 0);

    xylem_tls_ctx_t* cli_ctx = _cli_ctx();
    ASSERT(xylem_tls_ctx_set_alpn(cli_ctx, protocols, count) == 0);

    _ctx_t ctx = {
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .alpn    = expected,
        .port    = port,
    };
    _drive(&ctx, 2, _alpn_server, _alpn_client, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    remove(cert);
    remove(key);
}

static void test_alpn_negotiation(void) {
    const char* protocols[] = {"h2", "http/1.1"};
    _run_alpn(protocols, 2, "h2", TLS_PORT + 2);
}

static void test_long_alpn_negotiation(void) {
    char protocol[256];
    memset(protocol, 'a', sizeof(protocol) - 1);
    protocol[sizeof(protocol) - 1] = '\0';
    const char* protocols[] = {protocol};
    _run_alpn(protocols, 1, protocol, TLS_PORT + 18);
}

static void _alpn_retain_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* first = xylem_tls_accept(ln);
    ASSERT(first != NULL);
    ASSERT(xylem_tls_handshake(first) == 0);
    xylem_tls_conn_t* second = xylem_tls_accept(ln);
    ASSERT(second != NULL);
    ASSERT(xylem_tls_handshake(second) == 0);

    xylem_channel_recv(ctx->gate);
    xylem_tls_destroy(first);
    xylem_tls_destroy(second);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_retain_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* first =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(first != NULL);
    xylem_tls_conn_t* second =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->good_ctx, NULL);
    ASSERT(second != NULL);

    const char* first_alpn = xylem_tls_get_alpn(first);
    ASSERT(first_alpn != NULL);
    ASSERT(strcmp(first_alpn, "h2") == 0);
    const char* second_alpn = xylem_tls_get_alpn(second);
    ASSERT(second_alpn != NULL);
    ASSERT(strcmp(second_alpn, "mqtt") == 0);
    ASSERT(strcmp(first_alpn, "h2") == 0);

    xylem_tls_destroy(first);
    xylem_tls_destroy(second);
    xylem_channel_send(ctx->gate, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void test_alpn_result_is_connection_owned(void) {
    const char* cert = "test_tls_alpnret_cert.pem";
    const char* key  = "test_tls_alpnret_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    const char* server_protocols[] = {"h2", "mqtt"};
    const char* first_protocol[]   = {"h2"};
    const char* second_protocol[]  = {"mqtt"};
    xylem_tls_ctx_t* srv_ctx       = _srv_ctx(cert, key);
    ASSERT(xylem_tls_ctx_set_alpn(srv_ctx, server_protocols, 2) == 0);
    xylem_tls_ctx_t* cli_ctx = _cli_ctx();
    ASSERT(xylem_tls_ctx_set_alpn(cli_ctx, first_protocol, 1) == 0);
    xylem_tls_ctx_t* second_ctx = _cli_ctx();
    ASSERT(xylem_tls_ctx_set_alpn(second_ctx, second_protocol, 1) == 0);

    _ctx_t ctx = {
        .srv_ctx  = srv_ctx,
        .cli_ctx  = cli_ctx,
        .good_ctx = second_ctx,
        .port     = TLS_PORT + 20,
    };
    _drive(&ctx, 2, _alpn_retain_server, _alpn_retain_client, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_tls_ctx_destroy(second_ctx);
    remove(cert);
    remove(key);
}

static void _deadline_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_handshake(conn) == 0);

    xylem_sleep(2000);
    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_tls_set_read_deadline(conn, deadline);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_read_deadline(void) {
    _run_default((_plan_t){"test_tls_dl_cert.pem", "test_tls_dl_key.pem",
                           TLS_PORT + 4, 2, _deadline_server, _deadline_client});
}

static void _lazy_deadline_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_tls_set_read_deadline(conn, deadline);
    ASSERT(xylem_tls_handshake(conn) == 0);

    char buf[8];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_channel_send(ctx->gate, ctx);
    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _lazy_deadline_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_channel_recv(ctx->gate);
    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_lazy_handshake_preserves_read_deadline(void) {
    _run_default((_plan_t){"test_tls_lazy_dl_cert.pem",
                           "test_tls_lazy_dl_key.pem",
                           TLS_PORT + 15, 2, _lazy_deadline_server,
                           _lazy_deadline_client});
}

static void _connect_timeout_ignored_server(void* arg) {
    _ctx_t*             ctx = (_ctx_t*)arg;
    xylem_tls_opts_t    opts = {.connect_timeout_ms = 500};
    xylem_tls_listener_t* ln
        = xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, &opts);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_handshake(conn) == 0);

    xylem_sleep(700);
    ASSERT(xylem_tls_write(conn, "late", 4) == 0);

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _connect_timeout_ignored_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_sleep(1000);
    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_listener_connect_timeout_is_ignored(void) {
    _run_default((_plan_t){"test_tls_server_to_cert.pem",
                           "test_tls_server_to_key.pem",
                           TLS_PORT + 17, 2, _connect_timeout_ignored_server,
                           _connect_timeout_ignored_client});
}

static void _stale_errq_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_handshake(conn) == 0);

    xylem_sleep(200);
    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _stale_errq_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    ERR_put_error(ERR_LIB_SSL, 0, SSL_R_BAD_LENGTH, __FILE__, __LINE__);
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_tls_set_read_deadline(conn, deadline);

    char     buf[64];
    uint64_t start = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    int      n     = xylem_tls_read(conn, buf, sizeof(buf));
    uint64_t end   = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    ASSERT(n == -1);
    ASSERT(end - start >= 80);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_stale_error_queue_before_read(void) {
    _run_default((_plan_t){"test_tls_eq_cert.pem", "test_tls_eq_key.pem",
                           TLS_PORT + 5, 2, _stale_errq_server,
                           _stale_errq_client});
}

static void _expired_read_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_write(conn, "late", 4) == 0);
    xylem_sleep(100);
    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _expired_read_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_sleep(50);
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) - 1;
    xylem_tls_set_read_deadline(conn, deadline);

    char buf[64];
    ASSERT(xylem_tls_read(conn, buf, sizeof(buf)) == -1);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_expired_read_deadline_blocks_ready_tls_data(void) {
    _run_default((_plan_t){"test_tls_exp_cert.pem", "test_tls_exp_key.pem",
                           TLS_PORT + 6, 2, _expired_read_server,
                           _expired_read_client});
}

static void _sac_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    if (conn) {
        char buf[8];
        xylem_tls_read(conn, buf, sizeof(buf));
        xylem_tls_destroy(conn);
    }
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _sac_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_tls_write(conn, "x", 1);
    xylem_tls_close(conn);
    xylem_tls_close(conn);
    xylem_tls_destroy(conn);

    xylem_waitgroup_done(ctx->wg);
}

static void test_close(void) {
    _run_default((_plan_t){"test_tls_sac_cert.pem", "test_tls_sac_key.pem",
                           TLS_PORT + 5, 2, _sac_server, _sac_client});
}

static void _cl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ln);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn == NULL);

    xylem_tls_destroy_listener(ln);
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
    ASSERT(_utils_cert_gen(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = _srv_ctx(cert, key);

    _ctx_t ctx = {.srv_ctx = srv_ctx, .port = TLS_PORT + 6};
    _drive(&ctx, 2, _cl_server, _cl_closer, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    remove(cert);
    remove(key);
}

static void test_close_listener(void) {
    _cl_main(NULL);
}

static void _kl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    if (conn) {
        char buf[8];
        xylem_tls_read(conn, buf, sizeof(buf));
        xylem_tls_destroy(conn);
    }
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    xylem_tls_write(conn, "k", 1);
    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_main(void* arg) {
    (void)arg;
    const char* cert   = "test_tls_kl_cert.pem";
    const char* key    = "test_tls_kl_key.pem";
    const char* keylog = "test_keylog.txt";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    xylem_tls_ctx_t* srv_ctx = _srv_ctx(cert, key);

    xylem_tls_ctx_t* cli_ctx = _cli_ctx();
    ASSERT(xylem_tls_ctx_set_keylog(cli_ctx, keylog) == 0);

    _ctx_t ctx = {
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 7,
    };
    _drive(&ctx, 2, _kl_server, _kl_client, NULL);

    FILE* f = fopen(keylog, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    ASSERT(sz > 0);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    remove(cert);
    remove(key);
    remove(keylog);
}

static void test_keylog(void) {
    _kl_main(NULL);
}

static void _sni_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _default_identity_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char buf[8];
    ASSERT(xylem_tls_read(conn, buf, sizeof(buf)) == -1);

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _default_identity_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn == NULL);

    xylem_waitgroup_done(ctx->wg);
}

static void test_default_identity_uses_dial_host(void) {
    const char* cert = "test_tls_default_identity_cert.pem";
    const char* key  = "test_tls_default_identity_key.pem";
    ASSERT(_utils_cert_gen_ex(cert, key, "localhost", "DNS:localhost") == 0);

    xylem_tls_ctx_t* srv_ctx = _srv_ctx(cert, key);
    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_ca(cli_ctx, cert) == 0);

    _ctx_t ctx = {
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 17,
    };
    _drive(&ctx, 2, _default_identity_server, _default_identity_client, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    remove(cert);
    remove(key);
}

static void _sni_sel_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    for (int i = 0; i < 3; i++) {
        xylem_tls_conn_t* conn = xylem_tls_accept(ln);
        ASSERT(conn != NULL);

        char buf[64];
        int  n = xylem_tls_read(conn, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(xylem_tls_write(conn, buf, n) == 0);
        xylem_tls_destroy(conn);
    }

    xylem_tls_destroy_listener(ln);
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
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_destroy(conn);
}

static void _sni_sel_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    _sni_sel_roundtrip(ctx->cli_ctx, ctx->port, "sni.example", "host-cert");
    _sni_sel_roundtrip(
        ctx->cli_ctx,
        ctx->port,
        "sni.example.",
        "absolute-name");
    _sni_sel_roundtrip(ctx->good_ctx, ctx->port, "localhost", "default-cert");

    xylem_waitgroup_done(ctx->wg);
}

static void _sni_sel_main(void* arg) {
    (void)arg;
    const char* def_cert  = "test_tls_snisel_def_cert.pem";
    const char* def_key   = "test_tls_snisel_def_key.pem";
    const char* host_cert = "test_tls_snisel_host_cert.pem";
    const char* host_key  = "test_tls_snisel_host_key.pem";
    ASSERT(_utils_cert_gen(def_cert, def_key) == 0);
    ASSERT(_utils_cert_gen_ex(host_cert, host_key, "sni.example",
                              "DNS:sni.example") == 0);

    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, NULL, def_cert, def_key) == 0);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, "sni.example", host_cert,
                                   host_key) == 0);
    xylem_tls_ctx_verify_client(srv_ctx, false);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_verify_server(cli_ctx, true);
    ASSERT(xylem_tls_ctx_load_ca(cli_ctx, host_cert) == 0);

    xylem_tls_ctx_t* good_ctx = xylem_tls_ctx_create();
    ASSERT(good_ctx != NULL);
    xylem_tls_ctx_verify_server(good_ctx, true);
    ASSERT(xylem_tls_ctx_load_ca(good_ctx, def_cert) == 0);

    _ctx_t ctx = {
        .srv_ctx  = srv_ctx,
        .cli_ctx  = cli_ctx,
        .good_ctx = good_ctx,
        .port     = TLS_PORT + 9,
    };
    _drive(&ctx, 2, _sni_sel_server, _sni_sel_client, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_tls_ctx_destroy(good_ctx);
    remove(def_cert);
    remove(def_key);
    remove(host_cert);
    remove(host_key);
}

static void test_sni_cert_selection(void) {
    _sni_sel_main(NULL);
}

static void _sni_replace_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_opts_t opts = {.server_name = "sni.example"};
    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, &opts);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_write(conn, "ok", 2) == 0);

    char buf[8];
    ASSERT(xylem_tls_read(conn, buf, sizeof(buf)) == 2);
    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_sni_duplicate_replaces_identity(void) {
    const char* def_cert = "test_tls_snirep_def_cert.pem";
    const char* def_key  = "test_tls_snirep_def_key.pem";
    const char* old_cert = "test_tls_snirep_old_cert.pem";
    const char* old_key  = "test_tls_snirep_old_key.pem";
    const char* new_cert = "test_tls_snirep_new_cert.pem";
    const char* new_key  = "test_tls_snirep_new_key.pem";
    ASSERT(_utils_cert_gen(def_cert, def_key) == 0);
    ASSERT(_utils_cert_gen_ex(old_cert, old_key, "sni.example",
                              "DNS:sni.example") == 0);
    ASSERT(_utils_cert_gen_ex(new_cert, new_key, "sni.example",
                              "DNS:sni.example") == 0);

    xylem_tls_ctx_t* srv_ctx = _srv_ctx(def_cert, def_key);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, "sni.example", old_cert,
                                   old_key) == 0);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, "sni.example", new_cert,
                                   new_key) == 0);

    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_ca(cli_ctx, new_cert) == 0);

    _ctx_t ctx = {
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 19,
    };
    _drive(&ctx, 2, _sni_server, _sni_replace_client, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    remove(def_cert);
    remove(def_key);
    remove(old_cert);
    remove(old_key);
    remove(new_cert);
    remove(new_key);
}

static void _addr_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    char     host[46];
    uint16_t port;
    ASSERT(xylem_tls_remote_addr(conn, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port != 0);

    char buf[256];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tls_write(conn, buf, n) == 0);

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    char     host[46];
    uint16_t port;
    ASSERT(xylem_tls_local_addr(conn, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, "127.0.0.1") == 0);
    ASSERT(port != 0);

    const char* msg = "addr-ok";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);
    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_remote_addr(void) {
    _run_default((_plan_t){"test_tls_addr_cert.pem", "test_tls_addr_key.pem",
                           TLS_PORT + 10, 2, _addr_server, _addr_client});
}

static void _conc_close_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_handshake(conn) == 0);

    xylem_sleep(2000);
    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_close_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_conn_t* conn =
        (xylem_tls_conn_t*)xylem_channel_recv(ctx->gate);
    xylem_sleep(100);
    xylem_tls_close(conn);
    xylem_channel_send(ctx->gate, ctx);
}

static void _conc_close_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_spawn(_conc_close_closer, ctx);
    xylem_channel_send(ctx->gate, conn);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_channel_recv(ctx->gate);
    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_concurrent_close(void) {
    _run_default((_plan_t){"test_tls_cc_cert.pem", "test_tls_cc_key.pem",
                           TLS_PORT + 11, 2, _conc_close_server,
                           _conc_close_client});
}

static void _clac_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    xylem_tls_destroy_listener(ln);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "ping", 4) == 0);
    ASSERT(xylem_tls_write(conn, "pong", 4) == 0);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _clac_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_sleep(50);
    ASSERT(xylem_tls_write(conn, "ping", 4) == 0);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "pong", 4) == 0);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_close_listener_with_active_conn(void) {
    _run_default((_plan_t){"test_tls_clac_cert.pem", "test_tls_clac_key.pem",
                           TLS_PORT + 12, 2, _clac_server, _clac_client});
}

#define FDX_MSG_COUNT 200
#define FDX_MSG_SIZE  300

typedef struct {
    xylem_tls_conn_t*  conn;
    xylem_waitgroup_t* wg;
    int                ok;
} _fdx_share_t;

static void _fdx_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

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

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
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

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_waitgroup_t* io_wg = xylem_waitgroup_create();
    _fdx_share_t       sh    = {.conn = conn, .wg = io_wg, .ok = 1};
    xylem_waitgroup_add(io_wg, 2);
    xylem_spawn(_fdx_reader, &sh);
    xylem_spawn(_fdx_writer, &sh);
    xylem_waitgroup_wait(io_wg);
    xylem_waitgroup_destroy(io_wg);
    ASSERT(sh.ok == 1);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_full_duplex(void) {
    _run_default((_plan_t){"test_tls_fdx_cert.pem", "test_tls_fdx_key.pem",
                           TLS_PORT + 13, 2, _fdx_server, _fdx_client});
}

typedef struct {
    xylem_tls_conn_t*  conn;
    xylem_waitgroup_t* wg;
    int                ok;
} _lazy_share_t;

static void _lazy_reader(void* arg) {
    _lazy_share_t* sh = (_lazy_share_t*)arg;
    char buf[64];
    int  n = xylem_tls_read(sh->conn, buf, sizeof(buf));
    if (n != 2 || memcmp(buf, "hi", 2) != 0) {
        sh->ok = 0;
    }
    xylem_waitgroup_done(sh->wg);
}

static void _lazy_writer(void* arg) {
    _lazy_share_t* sh = (_lazy_share_t*)arg;
    if (xylem_tls_write(sh->conn, "yo", 2) != 0) {
        sh->ok = 0;
    }
    xylem_waitgroup_done(sh->wg);
}

static void _lazy_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);

    xylem_waitgroup_t* io_wg = xylem_waitgroup_create();
    _lazy_share_t      sh    = {.conn = conn, .wg = io_wg, .ok = 1};
    xylem_waitgroup_add(io_wg, 2);
    xylem_spawn(_lazy_reader, &sh);
    xylem_spawn(_lazy_writer, &sh);
    xylem_waitgroup_wait(io_wg);
    xylem_waitgroup_destroy(io_wg);
    ASSERT(sh.ok == 1);

    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _lazy_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_tls_handshake(conn) == 0);
    ASSERT(xylem_tls_handshake(conn) == 0);

    ASSERT(xylem_tls_write(conn, "hi", 2) == 0);
    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 2);
    ASSERT(memcmp(buf, "yo", 2) == 0);

    xylem_tls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_lazy_handshake(void) {
    _run_default((_plan_t){"test_tls_lazy_cert.pem", "test_tls_lazy_key.pem",
                           TLS_PORT + 14, 2, _lazy_server, _lazy_client});
}

static void _wrclose_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_conn_t* conn =
        (xylem_tls_conn_t*)xylem_channel_recv(ctx->ready);
    xylem_sleep(500);
    xylem_tls_close(conn);
    xylem_channel_send(ctx->ready, ctx);
}

static void _wrclose_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_spawn(_wrclose_closer, ctx);
    xylem_channel_send(ctx->ready, conn);

    static char big[64 * 1024];
    memset(big, 'x', sizeof(big));
    int rc = 0;
    for (int i = 0; i < 4096 && rc == 0; i++) {
        rc = xylem_tls_write(conn, big, (int)sizeof(big));
    }
    ASSERT(rc == -1);

    xylem_channel_recv(ctx->ready);
    xylem_tls_destroy(conn);
    xylem_channel_send(ctx->gate, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void _wrclose_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tls_conn_t* conn = xylem_tls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_tls_handshake(conn) == 0);

    xylem_channel_recv(ctx->gate);
    xylem_tls_destroy(conn);
    xylem_tls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void test_close_with_parked_writer(void) {
    _run_default((_plan_t){"test_tls_wc_cert.pem", "test_tls_wc_key.pem",
                           TLS_PORT + 16, 2, _wrclose_server, _wrclose_client});
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_load_cert_invalid();
    test_load_encrypted_private_key();
    test_load_system_ca();
    test_set_alpn();
    test_load_cert_mem();
    test_handshake_and_echo();
    test_max_connect_timeout();
    test_handshake_failure();
    test_alpn_negotiation();
    test_long_alpn_negotiation();
    test_alpn_result_is_connection_owned();
    test_read_deadline();
    test_lazy_handshake_preserves_read_deadline();
    test_listener_connect_timeout_is_ignored();
    test_stale_error_queue_before_read();
    test_expired_read_deadline_blocks_ready_tls_data();
    test_close();
    test_close_listener();
    test_keylog();
    test_default_identity_uses_dial_host();
    test_sni_cert_selection();
    test_sni_duplicate_replaces_identity();
    test_remote_addr();
    test_concurrent_close();
    test_close_listener_with_active_conn();
    test_full_duplex();
    test_lazy_handshake();
    test_close_with_parked_writer();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}

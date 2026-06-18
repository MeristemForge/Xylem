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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLS_HOST          "127.0.0.1"
#define TLS_PORT          14433
#define SAFETY_TIMEOUT_MS 10000

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_channel_t*   gate;
    xylem_waitgroup_t* wg;
    xylem_tls_ctx_t*   srv_ctx;
    xylem_tls_ctx_t*   cli_ctx;
    xylem_tls_ctx_t*   good_ctx;
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
    ctx->ready = xylem_channel_create(0);
    ctx->gate  = xylem_channel_create(0);
    ctx->wg    = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, n);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(a, ctx);
    xylem_spawn(b, ctx);
    if (c) {
        xylem_spawn(c, ctx);
    }
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
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
    xylem_shutdown();
}

static void _run_default(_plan_t plan) {
    xylem_run(_default_main, &plan, NULL);
}

static void test_load_cert_valid(void) {
    const char* cert = "test_tls_cert.pem";
    const char* key  = "test_tls_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

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

static void test_load_ca(void) {
    const char* cert = "test_tls_ca.pem";
    const char* key  = "test_tls_ca_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_ca(ctx, cert) == 0);
    xylem_tls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

static void test_load_system_ca(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_load_system_ca(ctx, NULL);
    xylem_tls_ctx_destroy(ctx);

    const char* ca  = "test_tls_sysca_fallback.pem";
    const char* key = "test_tls_sysca_fallback_key.pem";
    ASSERT(_utils_cert_gen(ca, key) == 0);

    xylem_tls_ctx_t* ctx2 = xylem_tls_ctx_create();
    ASSERT(ctx2 != NULL);
    ASSERT(xylem_tls_ctx_load_system_ca(ctx2, ca) == 0);
    xylem_tls_ctx_destroy(ctx2);

    xylem_tls_ctx_t* ctx3 = xylem_tls_ctx_create();
    ASSERT(ctx3 != NULL);
    (void)xylem_tls_ctx_load_system_ca(ctx3, "nonexistent_ca_bundle.pem");
    xylem_tls_ctx_destroy(ctx3);

    remove(ca);
    remove(key);
}

static void test_set_alpn(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, protos, 2) == 0);
    xylem_tls_ctx_destroy(ctx);
}

static void test_null_handles(void) {
    ASSERT(xylem_tls_dial(TLS_HOST, TLS_PORT, NULL, NULL) == NULL);
    ASSERT(xylem_tls_listen(TLS_HOST, TLS_PORT, NULL, NULL) == NULL);
    ASSERT(xylem_tls_accept(NULL) == NULL);
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
    xylem_tls_ctx_destroy(ctx);

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

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
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

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_handshake_and_echo(void) {
    _run_default((_plan_t){"test_tls_echo_cert.pem", "test_tls_echo_key.pem",
                           TLS_PORT, 2, _echo_server, _echo_client});
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

    xylem_tls_close(conn);
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
            xylem_tls_close(conn);
            continue;
        }
        ASSERT(xylem_tls_write(conn, buf, n) == 0);
        xylem_tls_close(conn);
        break;
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
    xylem_shutdown();
}

static void test_handshake_failure(void) {
    xylem_run(_fail_main, NULL, NULL);
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
    ASSERT(strcmp(alpn, "h2") == 0);

    char buf[8];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
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

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* alpn = xylem_tls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

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
    ASSERT(_utils_cert_gen(cert, key) == 0);

    const char* protos[] = {"h2", "http/1.1"};

    xylem_tls_ctx_t* srv_ctx = _srv_ctx(cert, key);
    ASSERT(xylem_tls_ctx_set_alpn(srv_ctx, protos, 2) == 0);

    xylem_tls_ctx_t* cli_ctx = _cli_ctx();
    ASSERT(xylem_tls_ctx_set_alpn(cli_ctx, protos, 2) == 0);

    _ctx_t ctx = {
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = TLS_PORT + 2,
    };
    _drive(&ctx, 2, _alpn_server, _alpn_client, NULL);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_alpn_negotiation(void) {
    xylem_run(_alpn_main, NULL, NULL);
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
    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
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

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_read_deadline(void) {
    _run_default((_plan_t){"test_tls_dl_cert.pem", "test_tls_dl_key.pem",
                           TLS_PORT + 4, 2, _deadline_server, _deadline_client});
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
        xylem_tls_close(conn);
    }
    xylem_tls_close_listener(ln);
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
    xylem_shutdown();
}

static void test_close_listener(void) {
    xylem_run(_cl_main, NULL, NULL);
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
        xylem_tls_close(conn);
    }
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _kl_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    xylem_tls_write(conn, "k", 1);
    xylem_tls_close(conn);
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
    xylem_shutdown();
}

static void test_keylog(void) {
    xylem_run(_kl_main, NULL, NULL);
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

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _sni_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_opts_t opts = {0};
    opts.server_name = "localhost";

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, &opts);
    ASSERT(conn != NULL);

    const char* msg = "sni-ok";
    ASSERT(xylem_tls_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_sni_hostname(void) {
    _run_default((_plan_t){"test_tls_sni_cert.pem", "test_tls_sni_key.pem",
                           TLS_PORT + 8, 2, _sni_server, _sni_client});
}

static void _sni_sel_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tls_listener_t* ln =
        xylem_tls_listen(TLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    for (int i = 0; i < 2; i++) {
        xylem_tls_conn_t* conn = xylem_tls_accept(ln);
        ASSERT(conn != NULL);

        char buf[64];
        int  n = xylem_tls_read(conn, buf, sizeof(buf));
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
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tls_close(conn);
}

static void _sni_sel_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    _sni_sel_roundtrip(ctx->cli_ctx, ctx->port, "sni.example", "host-cert");
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
    xylem_shutdown();
}

static void test_sni_cert_selection(void) {
    xylem_run(_sni_sel_main, NULL, NULL);
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

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
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

    xylem_tls_close(conn);
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

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_spawn(_conc_close_closer, conn);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

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

    xylem_tls_close_listener(ln);

    char buf[64];
    int  n = xylem_tls_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "ping", 4) == 0);
    ASSERT(xylem_tls_write(conn, "pong", 4) == 0);

    xylem_tls_close(conn);
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

    xylem_tls_close(conn);
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

    xylem_tls_close(conn);
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

    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
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

    xylem_tls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_lazy_handshake(void) {
    _run_default((_plan_t){"test_tls_lazy_cert.pem", "test_tls_lazy_key.pem",
                           TLS_PORT + 14, 2, _lazy_server, _lazy_client});
}

static void _wrclose_closer(void* arg) {
    xylem_tls_conn_t* conn = (xylem_tls_conn_t*)arg;
    xylem_sleep(500);
    xylem_tls_close(conn);
}

static void _wrclose_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tls_conn_t* conn =
        xylem_tls_dial(TLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_spawn(_wrclose_closer, conn);

    static char big[64 * 1024];
    memset(big, 'x', sizeof(big));
    int rc = 0;
    for (int i = 0; i < 4096 && rc == 0; i++) {
        rc = xylem_tls_write(conn, big, (int)sizeof(big));
    }
    ASSERT(rc == -1);

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
    xylem_tls_close(conn);
    xylem_tls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void test_close_with_parked_writer(void) {
    _run_default((_plan_t){"test_tls_wc_cert.pem", "test_tls_wc_key.pem",
                           TLS_PORT + 16, 2, _wrclose_server, _wrclose_client});
}

static void _run_cfg_tests(void* arg) {
    (void)arg;
    test_load_cert_valid();
    test_load_cert_invalid();
    test_load_ca();
    test_load_system_ca();
    test_set_alpn();
    test_null_handles();
    test_load_cert_mem();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_run_cfg_tests, NULL, NULL);
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
    test_concurrent_close();
    test_close_listener_with_active_conn();
    test_full_duplex();
    test_lazy_handshake();
    test_close_with_parked_writer();
    return 0;
}

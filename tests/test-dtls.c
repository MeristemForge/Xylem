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
#include "xylem/net/xylem-dtls.h"
#include "assert.h"
#define TEST_WITH_TLS
#include "utils.h"

#include <stdio.h>
#include <string.h>

#define DTLS_HOST         "127.0.0.1"
#define DTLS_PORT         15433

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_channel_t*   done;
    xylem_waitgroup_t* wg;
    xylem_dtls_ctx_t*  srv_ctx;
    xylem_dtls_ctx_t*  cli_ctx;
    uint16_t           port;
} _ctx_t;

static xylem_dtls_ctx_t* _srv_ctx(const char* cert, const char* key) {
    xylem_dtls_ctx_t* c = xylem_dtls_ctx_create();
    ASSERT(c != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(c, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(c, false);
    return c;
}

static xylem_dtls_ctx_t* _cli_ctx(void) {
    xylem_dtls_ctx_t* c = xylem_dtls_ctx_create();
    ASSERT(c != NULL);
    xylem_dtls_ctx_verify_server(c, false);
    return c;
}

static void _drive(_ctx_t* ctx, int n, _coro_t a, _coro_t b) {
    ctx->ready = xylem_channel_create();
    ctx->done  = xylem_channel_create();
    ctx->wg    = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, n);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(a, ctx);
    xylem_spawn(b, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->done);
    xylem_channel_destroy(ctx->ready);
}

typedef struct {
    const char* cert;
    const char* key;
    uint16_t    port;
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
    _drive(&ctx, 2, p->a, p->b);

    xylem_dtls_ctx_destroy(ctx.srv_ctx);
    xylem_dtls_ctx_destroy(ctx.cli_ctx);
    remove(p->cert);
    remove(p->key);
}

static void _run_default(_plan_t plan) {
    _default_main(&plan);
}

static void test_load_cert_valid(void) {
    const char* cert = "test_dtls_cert.pem";
    const char* key  = "test_dtls_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int  n = xylem_dtls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_dtls_write(conn, buf, n) == 0);

    xylem_dtls_close(conn);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem dtls";
    ASSERT(xylem_dtls_write(conn, msg, strlen(msg)) == 0);

    char buf[64];
    int  n = xylem_dtls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_handshake_and_echo(void) {
    _run_default((_plan_t){"test_dtls_echo_cert.pem", "test_dtls_echo_key.pem",
                           DTLS_PORT, _echo_server, _echo_client});
}

static void _alpn_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    const char* alpn = xylem_dtls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    char buf[8];
    int  n = xylem_dtls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_dtls_write(conn, buf, n);
    }

    xylem_dtls_close(conn);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* alpn = xylem_dtls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    ASSERT(xylem_dtls_write(conn, "ok", 2) == 0);
    char buf[8];
    xylem_dtls_read(conn, buf, sizeof(buf));

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_alpn_cert.pem";
    const char* key  = "test_dtls_alpn_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    const char* protos[] = {"h2", "http/1.1"};

    xylem_dtls_ctx_t* srv_ctx = _srv_ctx(cert, key);
    ASSERT(xylem_dtls_ctx_set_alpn(srv_ctx, protos, 2) == 0);

    xylem_dtls_ctx_t* cli_ctx = _cli_ctx();
    ASSERT(xylem_dtls_ctx_set_alpn(cli_ctx, protos, 2) == 0);

    _ctx_t ctx = {
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT + 1,
    };
    _drive(&ctx, 2, _alpn_server, _alpn_client);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    remove(cert);
    remove(key);
}

static void test_alpn_negotiation(void) {
    _alpn_main(NULL);
}

static void _ci_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    if (conn) {
        char buf[8];
        xylem_dtls_read(conn, buf, sizeof(buf));
        xylem_dtls_close(conn);
    }
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _ci_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_dtls_write(conn, "x", 1);
    xylem_dtls_close(conn);

    xylem_waitgroup_done(ctx->wg);
}

static void test_close_wakes_peer(void) {
    _run_default((_plan_t){"test_dtls_ci_cert.pem", "test_dtls_ci_key.pem",
                           DTLS_PORT + 2, _ci_server, _ci_client});
}

static void _dl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    xylem_sleep(2000);
    xylem_dtls_close(conn);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _dl_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 200;
    xylem_dtls_set_read_deadline(conn, deadline);

    char buf[64];
    int  n = xylem_dtls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_recv_deadline(void) {
    _run_default((_plan_t){"test_dtls_dl_cert.pem", "test_dtls_dl_key.pem",
                           DTLS_PORT + 3, _dl_server, _dl_client});
}

static void _cw_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ln);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn == NULL);

    xylem_waitgroup_done(ctx->wg);
}

static void _cw_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        (xylem_dtls_listener_t*)xylem_channel_recv(ctx->ready);
    xylem_sleep(100);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cw_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_cw_cert.pem";
    const char* key  = "test_dtls_cw_key.pem";
    ASSERT(_utils_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = _srv_ctx(cert, key);

    _ctx_t ctx = {.srv_ctx = srv_ctx, .port = DTLS_PORT + 4};
    _drive(&ctx, 2, _cw_server, _cw_closer);

    xylem_dtls_ctx_destroy(srv_ctx);
    remove(cert);
    remove(key);
}

static void test_close_wakes_recv(void) {
    _cw_main(NULL);
}

static void _lc_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    xylem_dtls_close_listener(ln);
    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _lc_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_listener_close_preserves_accepted_conn(void) {
    _run_default((_plan_t){"test_dtls_lc_cert.pem", "test_dtls_lc_key.pem",
                           DTLS_PORT + 5, _lc_server, _lc_client});
}

static void _lu_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_channel_recv(ctx->done);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _lu_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_dtls_close(conn);
    xylem_channel_send(ctx->done, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void test_listener_close_drops_unaccepted_conn(void) {
    _run_default((_plan_t){"test_dtls_lu_cert.pem", "test_dtls_lu_key.pem",
                           DTLS_PORT + 6, _lu_server, _lu_client});
}

#define CONC_COUNT 4

static void _conc_echo_handler(void* arg) {
    xylem_dtls_conn_t* conn = (xylem_dtls_conn_t*)arg;
    char buf[256];
    int  n = xylem_dtls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_dtls_write(conn, buf, n);
    }
    xylem_sleep(100);
    xylem_dtls_close(conn);
}

static void _conc_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    for (int i = 0; i < CONC_COUNT; i++) {
        xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
        ASSERT(conn != NULL);
        xylem_spawn(_conc_echo_handler, conn);
    }

    xylem_sleep(500);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_client_seq(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    for (int i = 0; i < CONC_COUNT; i++) {
        xylem_dtls_conn_t* conn =
            xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
        ASSERT(conn != NULL);

        char msg[64];
        int  len = snprintf(msg, sizeof(msg), "client-%d", i);

        ASSERT(xylem_dtls_write(conn, msg, len) == 0);

        char buf[64];
        int  n = xylem_dtls_read(conn, buf, sizeof(buf));
        ASSERT(n == (int64_t)len);
        ASSERT(memcmp(buf, msg, (size_t)n) == 0);

        xylem_dtls_close(conn);
    }

    xylem_waitgroup_done(ctx->wg);
}

static void test_concurrent_sessions(void) {
    _run_default((_plan_t){"test_dtls_conc_cert.pem", "test_dtls_conc_key.pem",
                           DTLS_PORT + 7, _conc_server, _conc_client_seq});
}

#define FDX_MSG_COUNT 50
#define FDX_MSG_SIZE  300

typedef struct {
    xylem_dtls_conn_t* conn;
    xylem_waitgroup_t* wg;
    int                ok;
} _fdx_share_t;

static void _fdx_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    char buf[1024];
    for (int i = 0; i < FDX_MSG_COUNT; i++) {
        int n = xylem_dtls_read(conn, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (xylem_dtls_write(conn, buf, n) != 0) {
            break;
        }
    }

    xylem_sleep(200);
    xylem_dtls_close(conn);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fdx_writer(void* arg) {
    _fdx_share_t* sh = (_fdx_share_t*)arg;
    char msg[FDX_MSG_SIZE];
    memset(msg, 'a', sizeof(msg));

    for (int i = 0; i < FDX_MSG_COUNT; i++) {
        if (xylem_dtls_write(sh->conn, msg, (int)sizeof(msg)) != 0) {
            sh->ok = 0;
            break;
        }
    }
    xylem_waitgroup_done(sh->wg);
}

static void _fdx_reader(void* arg) {
    _fdx_share_t* sh = (_fdx_share_t*)arg;
    char buf[1024];
    int  got = 0;

    while (got < FDX_MSG_COUNT) {
        int n = xylem_dtls_read(sh->conn, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        ASSERT(n == FDX_MSG_SIZE);
        got++;
    }
    if (got != FDX_MSG_COUNT) {
        sh->ok = 0;
    }
    xylem_waitgroup_done(sh->wg);
}

static void _fdx_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    xylem_waitgroup_t* io_wg = xylem_waitgroup_create();
    _fdx_share_t       sh    = {.conn = conn, .wg = io_wg, .ok = 1};
    xylem_waitgroup_add(io_wg, 2);
    xylem_spawn(_fdx_reader, &sh);
    xylem_spawn(_fdx_writer, &sh);
    xylem_waitgroup_wait(io_wg);
    xylem_waitgroup_destroy(io_wg);
    ASSERT(sh.ok == 1);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_full_duplex(void) {
    _run_default((_plan_t){"test_dtls_fdx_cert.pem", "test_dtls_fdx_key.pem",
                           DTLS_PORT + 8, _fdx_server, _fdx_client});
}

static void _run_cfg_tests(void* arg) {
    (void)arg;
    test_load_cert_valid();
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    _run_cfg_tests(NULL);
    test_handshake_and_echo();
    test_alpn_negotiation();
    test_close_wakes_peer();
    test_recv_deadline();
    test_close_wakes_recv();
    test_listener_close_preserves_accepted_conn();
    test_listener_close_drops_unaccepted_conn();
    test_concurrent_sessions();
    test_full_duplex();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}

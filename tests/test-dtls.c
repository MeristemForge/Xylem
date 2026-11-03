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

#define DTLS_HOST "127.0.0.1"
#define DTLS_PORT 15433

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
    xylem_spawn(a, ctx);
    xylem_spawn(b, ctx);
    xylem_waitgroup_wait(ctx->wg);
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
    _ctx_t*                ctx = (_ctx_t*)arg;
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

    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
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

    xylem_dtls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_handshake_and_echo(void) {
    _run_default((_plan_t){"test_dtls_echo_cert.pem",
                           "test_dtls_echo_key.pem",
                           DTLS_PORT,
                           _echo_server,
                           _echo_client});
}

static void _alpn_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    ASSERT(xylem_dtls_handshake(conn) == 0);
    ASSERT(xylem_dtls_handshake(conn) == 0);
    const char* alpn = xylem_dtls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    char buf[8];
    int  n = xylem_dtls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_dtls_write(conn, buf, n);
    }

    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _alpn_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_dtls_handshake(conn) == 0);
    ASSERT(xylem_dtls_handshake(conn) == 0);
    const char* alpn = xylem_dtls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    ASSERT(xylem_dtls_write(conn, "ok", 2) == 0);
    char buf[8];
    xylem_dtls_read(conn, buf, sizeof(buf));

    xylem_dtls_destroy(conn);
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
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    if (conn) {
        char buf[8];
        xylem_dtls_read(conn, buf, sizeof(buf));
        xylem_dtls_destroy(conn);
    }
    xylem_dtls_destroy_listener(ln);
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
    xylem_dtls_close(conn);
    xylem_dtls_destroy(conn);

    xylem_waitgroup_done(ctx->wg);
}

static void test_close_wakes_peer(void) {
    _run_default((_plan_t){"test_dtls_ci_cert.pem",
                           "test_dtls_ci_key.pem",
                           DTLS_PORT + 2,
                           _ci_server,
                           _ci_client});
}

static void _dl_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    ASSERT(xylem_dtls_handshake(conn) == 0);
    xylem_channel_recv(ctx->done);
    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
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
    xylem_channel_send(ctx->done, ctx);

    xylem_dtls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_recv_deadline(void) {
    _run_default((_plan_t){"test_dtls_dl_cert.pem",
                           "test_dtls_dl_key.pem",
                           DTLS_PORT + 3,
                           _dl_server,
                           _dl_client});
}

static void _cw_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ln);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn == NULL);

    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cw_closer(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
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
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_dtls_get_alpn(conn) == NULL);
    xylem_dtls_close_listener(ln);
    xylem_dtls_close_listener(ln);
    ASSERT(xylem_dtls_accept(ln) == NULL);
    xylem_dtls_destroy_listener(ln);

    uint16_t port = 0;
    ASSERT(xylem_dtls_local_addr(conn, NULL, 0, &port) == 0);
    ASSERT(port == ctx->port);
    char buf[32];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 4);
    ASSERT(memcmp(buf, "ping", 4) == 0);
    ASSERT(xylem_dtls_write(conn, "pong", 4) == 0);
    xylem_channel_recv(ctx->done);
    xylem_dtls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _lc_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    ASSERT(xylem_dtls_write(conn, "ping", 4) == 0);
    char buf[32];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 4);
    ASSERT(memcmp(buf, "pong", 4) == 0);
    xylem_dtls_destroy(conn);
    xylem_channel_send(ctx->done, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void test_listener_close_preserves_accepted_conn(void) {
    _run_default((_plan_t){"test_dtls_lc_cert.pem",
                           "test_dtls_lc_key.pem",
                           DTLS_PORT + 5,
                           _lc_server,
                           _lc_client});
}

static void _lu_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_channel_recv(ctx->done);
    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _lu_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    xylem_dtls_opts_t opts = {.handshake_timeout_ms = 50};
    ASSERT(xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, &opts) == NULL);
    xylem_channel_send(ctx->done, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void test_listener_close_drops_unaccepted_conn(void) {
    _run_default((_plan_t){"test_dtls_lu_cert.pem",
                           "test_dtls_lu_key.pem",
                           DTLS_PORT + 6,
                           _lu_server,
                           _lu_client});
}

#define CONC_COUNT 4

typedef struct {
    xylem_dtls_conn_t* conn;
    xylem_waitgroup_t* wg;
} _conc_handler_t;

static void _conc_echo_handler(void* arg) {
    _conc_handler_t*   handler = (_conc_handler_t*)arg;
    xylem_dtls_conn_t* conn    = handler->conn;
    char               buf[256];
    int                n = xylem_dtls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_dtls_write(conn, buf, n);
    }
    xylem_dtls_destroy(conn);
    xylem_waitgroup_done(handler->wg);
}

static void _conc_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_waitgroup_t* handlers_done = xylem_waitgroup_create();
    ASSERT(handlers_done != NULL);
    _conc_handler_t handlers[CONC_COUNT] = {0};
    xylem_waitgroup_add(handlers_done, CONC_COUNT);
    for (int i = 0; i < CONC_COUNT; i++) {
        xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
        ASSERT(conn != NULL);
        handlers[i].conn = conn;
        handlers[i].wg   = handlers_done;
        xylem_spawn(_conc_echo_handler, &handlers[i]);
    }

    xylem_waitgroup_wait(handlers_done);
    xylem_waitgroup_destroy(handlers_done);
    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_client_seq(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conns[CONC_COUNT] = {0};
    for (int i = 0; i < CONC_COUNT; i++) {
        conns[i] = xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
        ASSERT(conns[i] != NULL);

        char msg[64];
        int  len = snprintf(msg, sizeof(msg), "client-%d", i);

        ASSERT(xylem_dtls_write(conns[i], msg, len) == 0);

        char buf[64];
        int  n = xylem_dtls_read(conns[i], buf, sizeof(buf));
        ASSERT(n == (int64_t)len);
        ASSERT(memcmp(buf, msg, (size_t)n) == 0);
    }

    for (int i = 0; i < CONC_COUNT; i++) {
        xylem_dtls_destroy(conns[i]);
    }

    xylem_waitgroup_done(ctx->wg);
}

static void test_concurrent_sessions(void) {
    _run_default((_plan_t){"test_dtls_conc_cert.pem",
                           "test_dtls_conc_key.pem",
                           DTLS_PORT + 7,
                           _conc_server,
                           _conc_client_seq});
}

#define FDX_MSG_COUNT 50
#define FDX_MSG_SIZE  300

typedef struct {
    xylem_dtls_conn_t* conn;
    xylem_waitgroup_t* wg;
    int                ok;
} _fdx_share_t;

static void _fdx_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
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
    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fdx_writer(void* arg) {
    _fdx_share_t* sh = (_fdx_share_t*)arg;
    char          msg[FDX_MSG_SIZE];
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
    char          buf[1024];
    int           got = 0;

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

    xylem_dtls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_full_duplex(void) {
    _run_default((_plan_t){"test_dtls_fdx_cert.pem",
                           "test_dtls_fdx_key.pem",
                           DTLS_PORT + 8,
                           _fdx_server,
                           _fdx_client});
}

/* A valid initial ClientHello whose peer never answers HelloVerifyRequest. */
static void _send_client_hello(xylem_udp_chan_t* udp) {
    uint8_t hello[67] = {0};
    hello[0]          = 22;
    hello[1]          = 0xfe;
    hello[2]          = 0xfd;
    hello[12]         = 54;
    hello[13]         = 1;
    hello[16]         = 42;
    hello[24]         = 42;
    hello[25]         = 0xfe;
    hello[26]         = 0xfd;
    hello[62]         = 2;
    hello[63]         = 0xc0;
    hello[64]         = 0x2b;
    hello[65]         = 1;
    ASSERT(xylem_udp_send(udp, hello, sizeof(hello), NULL, 0) == 0);
}

static void _lazy_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_dtls_conn_t* stalled = xylem_dtls_accept(ln);
    ASSERT(stalled != NULL);
    ASSERT(xylem_dtls_get_alpn(stalled) == NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_dtls_write(conn, "hello", 5) == 0);
    char buf[16];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 5);
    ASSERT(memcmp(buf, "reply", 5) == 0);
    xylem_dtls_destroy_listener(ln);
    xylem_dtls_destroy(stalled);
    xylem_dtls_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _lazy_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    xylem_udp_chan_t* udp = xylem_udp_dial(DTLS_HOST, ctx->port);
    ASSERT(udp != NULL);
    _send_client_hello(udp);
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    char buf[16];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);
    ASSERT(xylem_dtls_write(conn, "reply", 5) == 0);
    xylem_dtls_destroy(conn);
    xylem_udp_destroy(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void test_lazy_handshake_does_not_block_accept(void) {
    _run_default((_plan_t){"test_dtls_lazy_cert.pem",
                           "test_dtls_lazy_key.pem",
                           DTLS_PORT + 9,
                           _lazy_server,
                           _lazy_client});
}

static void _handshake_timeout_server(void* arg) {
    _ctx_t*                ctx  = (_ctx_t*)arg;
    xylem_dtls_opts_t      opts = {.handshake_timeout_ms = 50};
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, &opts);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);
    ASSERT(xylem_dtls_handshake(conn) == -1);
    ASSERT(xylem_dtls_handshake(conn) == -1);
    char buf[16];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == -1);
    ASSERT(xylem_dtls_write(conn, "x", 1) == -1);
    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
    xylem_channel_send(ctx->done, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void _silent_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    xylem_udp_chan_t* udp = xylem_udp_dial(DTLS_HOST, ctx->port);
    ASSERT(udp != NULL);
    _send_client_hello(udp);
    ASSERT(xylem_channel_recv(ctx->done) != NULL);
    xylem_udp_destroy(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void test_server_handshake_timeout(void) {
    _run_default((_plan_t){"test_dtls_timeout_cert.pem",
                           "test_dtls_timeout_key.pem",
                           DTLS_PORT + 10,
                           _handshake_timeout_server,
                           _silent_client});
}

static void _handshake_cancel_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);
    xylem_channel_send(ctx->ready, conn);
    ASSERT(xylem_dtls_handshake(conn) == -1);
    xylem_channel_recv(ctx->done);
    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _handshake_interrupt_client(void* arg, int mode) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    xylem_udp_chan_t* udp = xylem_udp_dial(DTLS_HOST, ctx->port);
    ASSERT(udp != NULL);
    _send_client_hello(udp);
    xylem_dtls_conn_t* conn =
        (xylem_dtls_conn_t*)xylem_channel_recv(ctx->ready);
    ASSERT(conn != NULL);
    char buf[256];
    xylem_udp_set_read_deadline(udp,
                                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) +
                                    1000);
    ASSERT(xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL) > 0);
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 25;
    if (mode == 0) {
        xylem_dtls_close(conn);
    } else if (mode == 1) {
        xylem_dtls_set_read_deadline(conn, deadline);
    } else {
        xylem_dtls_set_write_deadline(conn, deadline);
    }
    xylem_channel_send(ctx->done, ctx);
    xylem_udp_destroy(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _handshake_cancel_client(void* arg) {
    _handshake_interrupt_client(arg, 0);
}

static void _handshake_read_deadline_client(void* arg) {
    _handshake_interrupt_client(arg, 1);
}

static void _handshake_write_deadline_client(void* arg) {
    _handshake_interrupt_client(arg, 2);
}

static void test_server_handshake_deadline_update(void) {
    _run_default((_plan_t){"test_dtls_rd_cert.pem",
                           "test_dtls_rd_key.pem",
                           DTLS_PORT + 14,
                           _handshake_cancel_server,
                           _handshake_read_deadline_client});
    _run_default((_plan_t){"test_dtls_wr_cert.pem",
                           "test_dtls_wr_key.pem",
                           DTLS_PORT + 15,
                           _handshake_cancel_server,
                           _handshake_write_deadline_client});
}

static void test_close_cancels_handshake(void) {
    _run_default((_plan_t){"test_dtls_cancel_cert.pem",
                           "test_dtls_cancel_key.pem",
                           DTLS_PORT + 11,
                           _handshake_cancel_server,
                           _handshake_cancel_client});
}

typedef struct {
    xylem_dtls_conn_t* conn;
    xylem_waitgroup_t* wg;
} _first_io_t;

static void _first_io_writer(void* arg) {
    _first_io_t* io = (_first_io_t*)arg;
    ASSERT(xylem_dtls_write(io->conn, "server", 6) == 0);
    xylem_waitgroup_done(io->wg);
}

static void _first_io_server(void* arg) {
    _ctx_t*                ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln =
        xylem_dtls_listen(DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);
    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);
    _first_io_t io = {.conn = conn, .wg = xylem_waitgroup_create()};
    ASSERT(io.wg != NULL);
    xylem_waitgroup_add(io.wg, 1);
    xylem_spawn(_first_io_writer, &io);
    char buf[16];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 6);
    ASSERT(memcmp(buf, "client", 6) == 0);
    xylem_waitgroup_wait(io.wg);
    xylem_waitgroup_destroy(io.wg);
    xylem_channel_recv(ctx->done);
    xylem_dtls_destroy(conn);
    xylem_dtls_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _first_io_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);
    ASSERT(xylem_dtls_write(conn, "client", 6) == 0);
    char buf[16];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 6);
    ASSERT(memcmp(buf, "server", 6) == 0);
    xylem_dtls_destroy(conn);
    xylem_channel_send(ctx->done, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void test_concurrent_first_read_write(void) {
    _run_default((_plan_t){"test_dtls_first_cert.pem",
                           "test_dtls_first_key.pem",
                           DTLS_PORT + 12,
                           _first_io_server,
                           _first_io_client});
}

typedef struct {
    xylem_udp_chan_t*  front;
    xylem_udp_chan_t*  back;
    xylem_channel_t*   peer_ready;
    xylem_waitgroup_t* wg;
    uint16_t           peer_port;
    bool               dropped;
} _loss_proxy_t;

static void _loss_forward_client(void* arg) {
    _loss_proxy_t* proxy = (_loss_proxy_t*)arg;
    uint8_t        buf[2048];
    uint8_t        hellos[8][2048];
    size_t         hello_lengths[8] = {0};
    size_t         hello_count      = 0;
    for (;;) {
        uint16_t port = 0;
        int n = xylem_udp_recv(proxy->front, buf, sizeof(buf), NULL, 0, &port);
        if (n < 0) {
            break;
        }
        if (proxy->peer_port == 0) {
            proxy->peer_port = port;
            xylem_channel_send(proxy->peer_ready, proxy);
        }
        if (n >= 25 && buf[0] == 22 && buf[13] == 1) {
            bool duplicate = false;
            for (size_t i = 0; i < hello_count; i++) {
                if (hello_lengths[i] == (size_t)n &&
                    memcmp(hellos[i] + 13, buf + 13, (size_t)n - 13) == 0) {
                    duplicate = true;
                    break;
                }
            }
            /* Keep distinct fragments and retry-cookie ClientHellos. */
            if (duplicate) {
                continue;
            }
            ASSERT(hello_count < 8);
            memcpy(hellos[hello_count], buf, (size_t)n);
            hello_lengths[hello_count++] = (size_t)n;
        }
        if (xylem_udp_send(proxy->back, buf, n, NULL, 0) != 0) {
            break;
        }
    }
    xylem_waitgroup_done(proxy->wg);
}

static void _loss_forward_server(void* arg) {
    _loss_proxy_t* proxy = (_loss_proxy_t*)arg;
    xylem_channel_recv(proxy->peer_ready);
    uint8_t buf[2048];
    for (;;) {
        int n = xylem_udp_recv(proxy->back, buf, sizeof(buf), NULL, 0, NULL);
        if (n < 0) {
            break;
        }
        if (!proxy->dropped && n >= 25 && buf[0] == 22 && buf[13] == 2) {
            proxy->dropped = true;
            continue;
        }
        if (xylem_udp_send(proxy->front, buf, n, DTLS_HOST, proxy->peer_port) !=
            0) {
            break;
        }
    }
    xylem_waitgroup_done(proxy->wg);
}

static void _loss_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);
    _loss_proxy_t proxy = {
        .front      = xylem_udp_listen(DTLS_HOST, 0),
        .back       = xylem_udp_dial(DTLS_HOST, ctx->port),
        .peer_ready = xylem_channel_create(),
        .wg         = xylem_waitgroup_create(),
    };
    ASSERT(proxy.front && proxy.back && proxy.peer_ready && proxy.wg);
    uint16_t port = 0;
    ASSERT(xylem_udp_local_addr(proxy.front, NULL, 0, &port) == 0);
    xylem_waitgroup_add(proxy.wg, 2);
    xylem_spawn(_loss_forward_client, &proxy);
    xylem_spawn(_loss_forward_server, &proxy);
    xylem_dtls_opts_t  opts = {.handshake_timeout_ms = 5000};
    xylem_dtls_conn_t* conn =
        xylem_dtls_dial(DTLS_HOST, port, ctx->cli_ctx, &opts);
    ASSERT(conn != NULL);
    ASSERT(xylem_dtls_write(conn, "retry", 5) == 0);
    char buf[16];
    ASSERT(xylem_dtls_read(conn, buf, sizeof(buf)) == 5);
    ASSERT(memcmp(buf, "retry", 5) == 0);
    xylem_dtls_destroy(conn);
    xylem_udp_close(proxy.front);
    xylem_udp_close(proxy.back);
    xylem_waitgroup_wait(proxy.wg);
    ASSERT(proxy.dropped);
    xylem_waitgroup_destroy(proxy.wg);
    xylem_channel_destroy(proxy.peer_ready);
    xylem_udp_destroy(proxy.front);
    xylem_udp_destroy(proxy.back);
    xylem_waitgroup_done(ctx->wg);
}

static void test_server_retransmits_lost_flight(void) {
    _run_default((_plan_t){"test_dtls_loss_cert.pem",
                           "test_dtls_loss_key.pem",
                           DTLS_PORT + 13,
                           _echo_server,
                           _loss_client});
}

static void _test_run_all(void* arg) {
    (void)arg;
    xylem_dtls_destroy(NULL);
    xylem_dtls_destroy_listener(NULL);
    ASSERT(xylem_dtls_handshake(NULL) == -1);

    void (*const cases[])(void) = {
        test_load_cert_valid,
        test_handshake_and_echo,
        test_alpn_negotiation,
        test_close_wakes_peer,
        test_recv_deadline,
        test_close_wakes_recv,
        test_listener_close_preserves_accepted_conn,
        test_listener_close_drops_unaccepted_conn,
        test_concurrent_sessions,
        test_full_duplex,
        test_lazy_handshake_does_not_block_accept,
        test_server_handshake_timeout,
        test_close_cancels_handshake,
        test_concurrent_first_read_write,
        test_server_retransmits_lost_flight,
        test_server_handshake_deadline_update,
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        _utils_watchdog_start(SAFETY_TIMEOUT_MS);
        cases[i]();
        _utils_watchdog_stop();
    }
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}

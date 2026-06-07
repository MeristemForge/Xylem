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
#include "utils.h"

#include <stdio.h>
#include <string.h>

#define DTLS_HOST          "127.0.0.1"
#define DTLS_PORT          15433
#define SAFETY_TIMEOUT_MS  10000

typedef struct {
    xylem_channel_t*      ready;
    xylem_waitgroup_t*    wg;
    xylem_dtls_ctx_t*     srv_ctx;
    xylem_dtls_ctx_t*     cli_ctx;
    uint16_t              port;
} _ctx_t;

/* test_ctx_create_destroy. */

static void test_ctx_create_destroy(void) {
    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_dtls_ctx_destroy(ctx);
}


/* test_load_cert_valid. */

static void test_load_cert_valid(void) {
    const char* cert = "test_dtls_cert.pem";
    const char* key  = "test_dtls_key.pem";
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_destroy(ctx);
    remove(cert);
    remove(key);
}


/* test_handshake_and_echo. */

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int n = xylem_dtls_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_dtls_write(conn, buf, n) == 0);

    xylem_dtls_close(conn);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn = xylem_dtls_dial(
        DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem dtls";
    ASSERT(xylem_dtls_write(conn, msg, strlen(msg)) == 0);

    char buf[64];
    int n = xylem_dtls_read(conn, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_echo_cert.pem";
    const char* key  = "test_dtls_echo_key.pem";
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);

    xylem_dtls_ctx_t* cli_ctx = xylem_dtls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_dtls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_echo_server, &ctx);
    xylem_spawn(_echo_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_handshake_and_echo(void) {
    xylem_run(_echo_main, NULL, NULL);
}


/* test_alpn_negotiation. */

static void _alpn_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    const char* alpn = xylem_dtls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    /* Echo a sync message so the client knows the server is alive. */
    char buf[8];
    int n = xylem_dtls_read(conn, buf, sizeof(buf));
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

    xylem_dtls_conn_t* conn = xylem_dtls_dial(
        DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    const char* alpn = xylem_dtls_get_alpn(conn);
    ASSERT(alpn != NULL);
    ASSERT(strcmp(alpn, "h2") == 0);

    /**
     * Round-trip exchange ensures the server has completed its
     * handshake before we tear down the connection.
     */
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
    ASSERT(_cert_gen(cert, key) == 0);

    const char* protos[] = {"h2", "http/1.1"};

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);
    ASSERT(xylem_dtls_ctx_set_alpn(srv_ctx, protos, 2) == 0);

    xylem_dtls_ctx_t* cli_ctx = xylem_dtls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_dtls_ctx_verify_server(cli_ctx, false);
    ASSERT(xylem_dtls_ctx_set_alpn(cli_ctx, protos, 2) == 0);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT + 1,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_alpn_server, &ctx);
    xylem_spawn(_alpn_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_alpn_negotiation(void) {
    xylem_run(_alpn_main, NULL, NULL);
}


/**
 * test_close_idempotent.
 * Verifies that closing a connection does not crash and that
 * the close_listener call after conn close also works cleanly.
 */

static void _ci_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    if (conn) {
        /* Drain the sync message then close. */
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

    xylem_dtls_conn_t* conn = xylem_dtls_dial(
        DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    /* Send a byte to ensure the server handshake completes. */
    xylem_dtls_write(conn, "x", 1);
    xylem_dtls_close(conn);

    xylem_waitgroup_done(ctx->wg);
}

static void _ci_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_ci_cert.pem";
    const char* key  = "test_dtls_ci_key.pem";
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);

    xylem_dtls_ctx_t* cli_ctx = xylem_dtls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_dtls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT + 2,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_ci_server, &ctx);
    xylem_spawn(_ci_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_close_idempotent(void) {
    xylem_run(_ci_main, NULL, NULL);
}


/* test_recv_deadline. */

static void _dl_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    /* Hold connection open, send nothing. */
    xylem_sleep(2000);
    xylem_dtls_close(conn);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _dl_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_dtls_conn_t* conn = xylem_dtls_dial(
        DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 200;
    xylem_dtls_set_read_deadline(conn, deadline);

    char buf[64];
    int n = xylem_dtls_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _dl_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_dl_cert.pem";
    const char* key  = "test_dtls_dl_key.pem";
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);

    xylem_dtls_ctx_t* cli_ctx = xylem_dtls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_dtls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT + 3,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_dl_server, &ctx);
    xylem_spawn(_dl_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_recv_deadline(void) {
    xylem_run(_dl_main, NULL, NULL);
}


/**
 * test_close_wakes_recv.
 * Tests that close_listener unblocks a pending accept call,
 * returning NULL. This verifies that shutdown signals propagate
 * correctly to blocked coroutines.
 */

static void _cw_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ln);

    /* Block on accept; nobody will connect. */
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
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = NULL,
        .port    = DTLS_PORT + 4,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_cw_server, &ctx);
    xylem_spawn(_cw_closer, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_close_wakes_recv(void) {
    xylem_run(_cw_main, NULL, NULL);
}


/**
 * test_concurrent_sessions.
 * Tests multiple sequential DTLS sessions on the same listener.
 * Each client connects, sends a unique message, receives the
 * echo, and disconnects before the next one starts.
 */

#define CONC_COUNT 4

static void _conc_echo_handler(void* arg) {
    xylem_dtls_conn_t* conn = (xylem_dtls_conn_t*)arg;
    char buf[256];
    int n = xylem_dtls_read(conn, buf, sizeof(buf));
    if (n > 0) {
        xylem_dtls_write(conn, buf, n);
    }
    /* Wait for client to read the echo before closing. */
    xylem_sleep(100);
    xylem_dtls_close(conn);
}

static void _conc_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    for (int i = 0; i < CONC_COUNT; i++) {
        xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
        ASSERT(conn != NULL);
        xylem_spawn(_conc_echo_handler, conn);
    }

    /* Wait for all echo handlers to finish before closing listener. */
    xylem_sleep(500);
    xylem_dtls_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _conc_client_seq(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    for (int i = 0; i < CONC_COUNT; i++) {
        xylem_dtls_conn_t* conn = xylem_dtls_dial(
            DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
        ASSERT(conn != NULL);

        char msg[64];
        int  len = snprintf(msg, sizeof(msg), "client-%d", i);

        ASSERT(xylem_dtls_write(conn, msg, len) == 0);

        char buf[64];
        int n = xylem_dtls_read(conn, buf, sizeof(buf));
        ASSERT(n == (int64_t)len);
        ASSERT(memcmp(buf, msg, (size_t)n) == 0);

        xylem_dtls_close(conn);
    }

    xylem_waitgroup_done(ctx->wg);
}

static void _conc_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_conc_cert.pem";
    const char* key  = "test_dtls_conc_key.pem";
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);

    xylem_dtls_ctx_t* cli_ctx = xylem_dtls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_dtls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT + 5,
    };

    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_conc_server, &ctx);
    xylem_spawn(_conc_client_seq, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_concurrent_sessions(void) {
    xylem_run(_conc_main, NULL, NULL);
}


/**
 * test_full_duplex.
 * One client connection is read by one coroutine and written by
 * another at the same time. With the client still on a socket BIO
 * a direction-flip inside SSL parked a second coroutine on the
 * same iowait direction and aborted the process; the memory-BIO
 * client pump path makes concurrent read+write safe. Each write
 * is one datagram, echoed back verbatim by the server.
 */

/**
 * Kept below DTLS_INBOX_CAP (64) so that even if the writer bursts the
 * whole batch before the server drains its session inbox, no datagram
 * is dropped -- the reader can then assert an exact echo count.
 */
#define FDX_MSG_COUNT 50
#define FDX_MSG_SIZE  300

typedef struct {
    xylem_dtls_conn_t* conn;
    xylem_waitgroup_t* wg;
    int                ok;
} _fdx_share_t;

static void _fdx_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_dtls_listener_t* ln = xylem_dtls_listen(
        DTLS_HOST, ctx->port, ctx->srv_ctx, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_dtls_conn_t* conn = xylem_dtls_accept(ln);
    ASSERT(conn != NULL);

    /**
     * Echo each datagram back until all messages have been seen or the
     * peer goes away (read returns <= 0).
     */
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

    /* Give the client time to drain before tearing down. */
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

    xylem_dtls_conn_t* conn = xylem_dtls_dial(
        DTLS_HOST, ctx->port, ctx->cli_ctx, NULL);
    ASSERT(conn != NULL);

    /**
     * Reader and writer drive the same connection concurrently; their
     * own waitgroup lets us join before closing the connection once.
     */
    xylem_waitgroup_t* io_wg = xylem_waitgroup_create();
    _fdx_share_t sh = { .conn = conn, .wg = io_wg, .ok = 1 };
    xylem_waitgroup_add(io_wg, 2);
    xylem_spawn(_fdx_reader, &sh);
    xylem_spawn(_fdx_writer, &sh);
    xylem_waitgroup_wait(io_wg);
    xylem_waitgroup_destroy(io_wg);
    ASSERT(sh.ok == 1);

    xylem_dtls_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _fdx_main(void* arg) {
    (void)arg;
    const char* cert = "test_dtls_fdx_cert.pem";
    const char* key  = "test_dtls_fdx_key.pem";
    ASSERT(_cert_gen(cert, key) == 0);

    xylem_dtls_ctx_t* srv_ctx = xylem_dtls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_dtls_ctx_load_cert(srv_ctx, NULL, cert, key) == 0);
    xylem_dtls_ctx_verify_client(srv_ctx, false);

    xylem_dtls_ctx_t* cli_ctx = xylem_dtls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_dtls_ctx_verify_server(cli_ctx, false);

    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .srv_ctx = srv_ctx,
        .cli_ctx = cli_ctx,
        .port    = DTLS_PORT + 6,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_fdx_server, &ctx);
    xylem_spawn(_fdx_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_dtls_ctx_destroy(srv_ctx);
    xylem_dtls_ctx_destroy(cli_ctx);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    remove(cert);
    remove(key);
    xylem_shutdown();
}

static void test_full_duplex(void) {
    xylem_run(_fdx_main, NULL, NULL);
}


/* main. */

int main(void) {
    test_ctx_create_destroy();
    test_load_cert_valid();
    test_handshake_and_echo();
    test_alpn_negotiation();
    test_close_idempotent();
    test_recv_deadline();
    test_close_wakes_recv();
    test_concurrent_sessions();
    test_full_duplex();
    return 0;
}

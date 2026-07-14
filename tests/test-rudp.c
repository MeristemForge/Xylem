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
#include "assert.h"
#include "utils.h"

#include <string.h>

#define RUDP_HOST         "127.0.0.1"

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_channel_t*   handoff;
    xylem_waitgroup_t* wg;
    uint16_t           port;
    _coro_t            a;
    _coro_t            b;
    _coro_t            c;
} _ctx_t;

static void _main(void* arg) {
    _ctx_t* ctx  = (_ctx_t*)arg;
    int     n    = ctx->c ? 3 : 2;
    ctx->ready   = xylem_channel_create();
    ctx->handoff = xylem_channel_create();
    ctx->wg      = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, n);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->a, ctx);
    xylem_spawn(ctx->b, ctx);
    if (ctx->c) {
        xylem_spawn(ctx->c, ctx);
    }
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->handoff);
    xylem_channel_destroy(ctx->ready);
}

static void _run(_ctx_t ctx) {
    _main(&ctx);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);
    ASSERT(xylem_rudp_write(conn, "world", 5) == 0);

    xylem_sleep(100);

    xylem_rudp_destroy(conn);
    xylem_rudp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_rudp_write(conn, "hello", 5) == 0);

    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "world", 5) == 0);

    xylem_rudp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_echo(void) {
    _run((_ctx_t){.port = 19300, .a = _echo_server, .b = _echo_client});
}

static void _addr_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    char     host[46] = {0};
    uint16_t port     = 0;
    ASSERT(xylem_rudp_remote_addr(conn, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, RUDP_HOST) == 0);
    ASSERT(port != 0);

    xylem_sleep(100);
    xylem_rudp_destroy(conn);
    xylem_rudp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);
    xylem_sleep(150);
    xylem_rudp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_remote_addr(void) {
    _run((_ctx_t){.port = 19302, .a = _addr_server, .b = _addr_client});
}

static void _deadline_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    xylem_sleep(250);

    xylem_rudp_destroy(conn);
    xylem_rudp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_rudp_set_read_deadline(conn, deadline);

    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_rudp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_read_deadline(void) {
    _run((_ctx_t){.port = 19304, .a = _deadline_server, .b = _deadline_client});
}

static void _wake_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    xylem_channel_send(ctx->handoff, conn);
    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n <= 0);

    xylem_channel_recv(ctx->handoff);
    xylem_rudp_destroy(conn);
    xylem_rudp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _wake_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_conn_t* conn =
        (xylem_rudp_conn_t*)xylem_channel_recv(ctx->handoff);
    xylem_sleep(80);
    xylem_rudp_close(conn);
    xylem_rudp_close(conn);
    xylem_channel_send(ctx->handoff, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void _wake_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);
    xylem_sleep(200);
    xylem_rudp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_close_wakes_read(void) {
    _run((_ctx_t){
        .port = 19306,
        .a    = _wake_server,
        .b    = _wake_closer,
        .c    = _wake_client,
    });
}

static void _dead_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    xylem_rudp_destroy(conn);

    xylem_channel_recv(ctx->ready);
    xylem_rudp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _dead_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_opts_t opts = {0};
    opts.timeout_ms = 20;
    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, &opts);
    ASSERT(conn != NULL);

    ASSERT(xylem_rudp_write(conn, "ping", 4) == 0);

    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n <= 0);

    xylem_rudp_destroy(conn);
    xylem_channel_send(ctx->ready, ctx);
    xylem_waitgroup_done(ctx->wg);
}

static void test_dead_link(void) {
    _run((_ctx_t){.port = 19308, .a = _dead_server, .b = _dead_client});
}

static void _close_listener_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    for (uint16_t i = 0; i < 32; i++) {
        xylem_rudp_listener_t* ln =
            xylem_rudp_listen(RUDP_HOST, (uint16_t)(ctx->port + i), NULL);
        ASSERT(ln != NULL);
        xylem_rudp_close_listener(ln);
        xylem_rudp_close_listener(ln);
        xylem_sleep(1);
        xylem_rudp_destroy_listener(ln);
    }

    xylem_waitgroup_done(ctx->wg);
}

static void _close_listener_peer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_waitgroup_done(ctx->wg);
}

static void test_close_listener_wakes_dispatcher(void) {
    _run((_ctx_t){
        .port = 19310,
        .a    = _close_listener_server,
        .b    = _close_listener_peer,
    });
}

static void _listener_close_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    xylem_rudp_destroy_listener(ln);
    xylem_rudp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _listener_close_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    xylem_sleep(100);
    xylem_rudp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_listener_close_preserves_accepted_conn(void) {
    _run((_ctx_t){
        .port = 19350,
        .a    = _listener_close_server,
        .b    = _listener_close_client,
    });
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    xylem_rudp_destroy(NULL);
    xylem_rudp_destroy_listener(NULL);

    test_echo();
    test_remote_addr();
    test_read_deadline();
    test_close_wakes_read();
    test_dead_link();
    test_close_listener_wakes_dispatcher();
    test_listener_close_preserves_accepted_conn();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}

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
#define SAFETY_TIMEOUT_MS 8000

typedef struct {
    xylem_channel_t*   ready;
    xylem_channel_t*   handoff;
    xylem_waitgroup_t* wg;
    uint16_t           port;
} _ctx_t;

/* --- test_echo: dial/accept handshake, stream roundtrip, close --- */

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

    /* Let the client drain the reply before tearing the link down. */
    xylem_sleep(100);

    xylem_rudp_close(conn);
    xylem_rudp_close_listener(ln);
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

    xylem_rudp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = 19300,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(_echo_server, &ctx);
    xylem_spawn(_echo_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_echo(void) {
    xylem_run(_echo_main, NULL, NULL);
}

/* --- test_remote_addr: peer address is reported after handshake --- */

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
    xylem_rudp_close(conn);
    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);
    /* Keep the link alive long enough for the server to read the addr. */
    xylem_sleep(150);
    xylem_rudp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = 19302,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(_addr_server, &ctx);
    xylem_spawn(_addr_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_remote_addr(void) {
    xylem_run(_addr_main, NULL, NULL);
}

/* --- test_read_deadline: client read deadline fires when peer is silent.
 *     (Server-side session read drains an inbox channel and does not honor
 *     a read deadline, so the deadline contract is exercised client-side,
 *     where the read parks on iowait.) --- */

static void _deadline_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    /* Stay connected but send nothing, so the client read times out. */
    xylem_sleep(250);

    xylem_rudp_close(conn);
    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_rudp_set_read_deadline(conn, deadline);

    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n == -1);

    xylem_rudp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _deadline_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = 19304,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(_deadline_server, &ctx);
    xylem_spawn(_deadline_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_read_deadline(void) {
    xylem_run(_deadline_main, NULL, NULL);
}

/* --- test_close_wakes_read: close from another coro wakes a parked read --- */

static void _wake_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    /* Hand the session to the closer, then park in read with no data. */
    xylem_channel_send(ctx->handoff, conn);
    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n <= 0); /* woken by close: 0 (drained) or -1 (closed). */

    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _wake_closer(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_conn_t* conn =
        (xylem_rudp_conn_t*)xylem_channel_recv(ctx->handoff);
    xylem_sleep(80);
    /* Close from a different coroutine while the server read is parked. */
    xylem_rudp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _wake_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);
    xylem_sleep(200);
    xylem_rudp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _wake_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready   = xylem_channel_create(),
        .handoff = xylem_channel_create(),
        .wg      = xylem_waitgroup_create(),
        .port    = 19306,
    };
    xylem_waitgroup_add(ctx.wg, 3);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(_wake_server, &ctx);
    xylem_spawn(_wake_closer, &ctx);
    xylem_spawn(_wake_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.handoff);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_close_wakes_read(void) {
    xylem_run(_wake_main, NULL, NULL);
}

/* --- test_dead_link: KCP dead-link detection closes the conn from the
 *     update timer (a non-coroutine context). Regression test for the
 *     deferred-close path: the timer callback must hand xylem_rudp_close
 *     off to a coroutine instead of calling it inline. --- */

static void _dead_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_rudp_listener_t* ln = xylem_rudp_listen(RUDP_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* conn = xylem_rudp_accept(ln);
    ASSERT(conn != NULL);

    /* Drop the session immediately but keep the listener socket open, so
     * the client's segments are silently dropped (no ACK, no ICMP). The
     * client's KCP then retransmits until it hits dead_link. */
    xylem_rudp_close(conn);

    /* Stay alive until the client has detected the dead link. */
    xylem_channel_recv(ctx->ready);
    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _dead_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_opts_t opts = {0};
    opts.timeout_ms = 20; /* dead_link = timeout_ms/10 = 2 retransmits */
    xylem_rudp_conn_t* conn = xylem_rudp_dial(RUDP_HOST, ctx->port, &opts);
    ASSERT(conn != NULL);

    /* Unacked data drives KCP toward dead_link. */
    ASSERT(xylem_rudp_write(conn, "ping", 4) == 0);

    /* Parked read is woken when the deferred close tears the conn down. */
    char buf[64];
    int  n = xylem_rudp_read(conn, buf, sizeof(buf));
    ASSERT(n <= 0);

    xylem_rudp_close(conn);
    xylem_channel_send(ctx->ready, ctx); /* release the server */
    xylem_waitgroup_done(ctx->wg);
}

static void _dead_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = 19308,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(_dead_server, &ctx);
    xylem_spawn(_dead_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_dead_link(void) {
    xylem_run(_dead_main, NULL, NULL);
}

int main(void) {
    test_echo();
    test_remote_addr();
    test_read_deadline();
    test_close_wakes_read();
    test_dead_link();
    return 0;
}

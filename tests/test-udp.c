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

#include <string.h>

#define UDP_HOST          "127.0.0.1"
#define UDP_PORT_A        19100
#define UDP_PORT_B        19101
#define SAFETY_TIMEOUT_MS 5000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port_a;
    uint16_t           port_b;
} _ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

/* --- test_echo: dial client sends, listen server recvs and replies --- */

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);
    xylem_channel_send(ctx->ready, ctx);

    char     buf[64];
    char     sender_host[46];
    uint16_t sender_port = 0;
    int64_t  n = xylem_udp_recv(
        udp, buf, sizeof(buf), sender_host, sizeof(sender_host), &sender_port);
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);

    ASSERT(xylem_udp_send(
        udp, "world", 5, sender_host, sender_port) == 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_udp_chan_t* udp = xylem_udp_dial(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);

    ASSERT(xylem_udp_send(udp, "hello", 5, NULL, 0) == 0);

    char    buf[64];
    int64_t n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "world", 5) == 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready  = xylem_channel_create(),
        .wg     = xylem_waitgroup_create(),
        .port_a = UDP_PORT_A,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
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

/* --- test_recvfrom_addr: verify sender address is correct --- */

static void _addr_sender(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_b);
    ASSERT(udp != NULL);
    ASSERT(xylem_udp_send(udp, "ping", 4, UDP_HOST, ctx->port_a) == 0);
    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_receiver(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);
    xylem_channel_send(ctx->ready, ctx);

    char     buf[64];
    char     host[46];
    uint16_t port = 0;
    int64_t  n = xylem_udp_recv(udp, buf, sizeof(buf), host, sizeof(host), &port);
    ASSERT(n == 4);
    ASSERT(port == ctx->port_b);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _addr_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready  = xylem_channel_create(),
        .wg     = xylem_waitgroup_create(),
        .port_a = UDP_PORT_A + 2,
        .port_b = UDP_PORT_B + 2,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_addr_receiver, &ctx);
    xylem_spawn(_addr_sender, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_recvfrom_addr(void) {
    xylem_run(_addr_main, NULL, NULL);
}

/* --- test_deadline_timeout: read deadline fires when no data --- */

static void _timeout_coro(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_udp_set_read_deadline(udp, deadline);

    char    buf[64];
    int64_t n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == -1);
    ASSERT(xylem_udp_get_error(udp) == XYLEM_ERR_TIMEOUT);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _timeout_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .wg     = xylem_waitgroup_create(),
        .port_a = UDP_PORT_A + 4,
    };
    xylem_waitgroup_add(ctx.wg, 1);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_timeout_coro, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_deadline_timeout(void) {
    xylem_run(_timeout_main, NULL, NULL);
}

/* --- test_close_wakes_recv: close from another coro wakes blocked recv --- */

static void _close_recv_coro(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);
    xylem_channel_send(ctx->ready, udp);

    char    buf[64];
    int64_t n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == -1);

    xylem_waitgroup_done(ctx->wg);
}

static void _close_closer_coro(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = (xylem_udp_chan_t*)xylem_channel_recv(ctx->ready);

    xylem_sleep(50);
    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _close_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready  = xylem_channel_create(),
        .wg     = xylem_waitgroup_create(),
        .port_a = UDP_PORT_A + 6,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_close_recv_coro, &ctx);
    xylem_spawn(_close_closer_coro, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_close_wakes_recv(void) {
    xylem_run(_close_main, NULL, NULL);
}

/* --- test_datagram_boundary: 3 sends = 3 separate recvs --- */

static void _boundary_sender(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_udp_chan_t* udp = xylem_udp_dial(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);

    ASSERT(xylem_udp_send(udp, "A", 1, NULL, 0) == 0);
    ASSERT(xylem_udp_send(udp, "BB", 2, NULL, 0) == 0);
    ASSERT(xylem_udp_send(udp, "CCC", 3, NULL, 0) == 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _boundary_receiver(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);
    xylem_channel_send(ctx->ready, ctx);

    char buf[64];
    int64_t n;

    n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == 1 && buf[0] == 'A');

    n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == 2 && memcmp(buf, "BB", 2) == 0);

    n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == 3 && memcmp(buf, "CCC", 3) == 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _boundary_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready  = xylem_channel_create(),
        .wg     = xylem_waitgroup_create(),
        .port_a = UDP_PORT_A + 8,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_boundary_receiver, &ctx);
    xylem_spawn(_boundary_sender, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_datagram_boundary(void) {
    xylem_run(_boundary_main, NULL, NULL);
}

int main(void) {
    test_echo();
    test_recvfrom_addr();
    test_deadline_timeout();
    test_close_wakes_recv();
    test_datagram_boundary();
    return 0;
}

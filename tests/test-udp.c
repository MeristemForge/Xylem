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

#define UDP_HOST          "127.0.0.1"
#define UDP_PORT_A        19100
#define UDP_PORT_B        19101
#define SAFETY_TIMEOUT_MS 5000

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port_a;
    uint16_t           port_b;
    _coro_t            server;
    _coro_t            client;
} _ctx_t;

static void _pair_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->ready  = xylem_channel_create(0);
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->server, ctx);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->ready);
    xylem_shutdown();
}

static void _run_pair(_ctx_t ctx) {
    xylem_run(_pair_main, &ctx, NULL);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);
    xylem_channel_send(ctx->ready, ctx);

    char     buf[64];
    char     sender_host[46];
    uint16_t sender_port = 0;
    int      n           = xylem_udp_recv(
        udp, buf, sizeof(buf), sender_host, sizeof(sender_host), &sender_port);
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);

    ASSERT(xylem_udp_send(udp, "world", 5, sender_host, sender_port) == 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_udp_chan_t* udp = xylem_udp_dial(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);

    ASSERT(xylem_udp_send(udp, "hello", 5, NULL, 0) == 0);

    char buf[64];
    int  n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == 5);
    ASSERT(memcmp(buf, "world", 5) == 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void test_echo(void) {
    _run_pair((_ctx_t){
        .port_a = UDP_PORT_A,
        .server = _echo_server,
        .client = _echo_client,
    });
}

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
    int      n = xylem_udp_recv(udp, buf, sizeof(buf), host, sizeof(host), &port);
    ASSERT(n == 4);
    ASSERT(port == ctx->port_b);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void test_recvfrom_addr(void) {
    _run_pair((_ctx_t){
        .port_a = UDP_PORT_A + 2,
        .port_b = UDP_PORT_B + 2,
        .server = _addr_receiver,
        .client = _addr_sender,
    });
}

static void _timeout_coro(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);

    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 100;
    xylem_udp_set_read_deadline(udp, deadline);

    char buf[64];
    int  n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
    ASSERT(n == -1);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void _timeout_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_shutdown();
}

static void test_deadline_timeout(void) {
    _ctx_t ctx = {.port_a = UDP_PORT_A + 4, .client = _timeout_coro};
    xylem_run(_timeout_main, &ctx, NULL);
}

static void _close_recv_coro(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_listen(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);
    xylem_channel_send(ctx->ready, udp);

    char buf[64];
    int  n = xylem_udp_recv(udp, buf, sizeof(buf), NULL, 0, NULL);
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

static void test_close_wakes_recv(void) {
    _run_pair((_ctx_t){
        .port_a = UDP_PORT_A + 6,
        .server = _close_recv_coro,
        .client = _close_closer_coro,
    });
}

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

    char    buf[64];
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

static void test_datagram_boundary(void) {
    _run_pair((_ctx_t){
        .port_a = UDP_PORT_A + 8,
        .server = _boundary_receiver,
        .client = _boundary_sender,
    });
}

static void _connaddr_coro(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_udp_chan_t* udp = xylem_udp_dial(UDP_HOST, ctx->port_a);
    ASSERT(udp != NULL);

    char     host[46];
    uint16_t port = 0;
    ASSERT(xylem_udp_remote_addr(udp, host, sizeof(host), &port) == 0);
    ASSERT(strcmp(host, UDP_HOST) == 0);
    ASSERT(port == ctx->port_a);

    char     lhost[46];
    uint16_t lport = 0;
    ASSERT(xylem_udp_local_addr(udp, lhost, sizeof(lhost), &lport) == 0);
    ASSERT(lport != 0);

    xylem_udp_close(udp);
    xylem_waitgroup_done(ctx->wg);
}

static void test_connected_addr(void) {
    _ctx_t ctx = {.port_a = UDP_PORT_A + 10, .client = _connaddr_coro};
    xylem_run(_timeout_main, &ctx, NULL);
}

int main(void) {
    test_echo();
    test_recvfrom_addr();
    test_deadline_timeout();
    test_close_wakes_recv();
    test_datagram_boundary();
    test_connected_addr();
    return 0;
}

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

#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

#define MUX_HOST          "127.0.0.1"
#define MUX_PORT          14600
#define MUX_FLOW_SIZE     (512 * 1024)

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
    _coro_t            server;
    _coro_t            client;
} _ctx_t;

typedef struct {
    xylem_mux_stream_t* stream;
    xylem_waitgroup_t*  wg;
} _stream_arg_t;

typedef struct {
    xylem_mux_stream_t* stream;
    xylem_waitgroup_t*  wg;
    int                 result;
} _fin_read_ctx_t;

static void _pair_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->ready  = xylem_channel_create();
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
}

static void _run_pair(uint16_t port, _coro_t server, _coro_t client) {
    _ctx_t ctx = {.port = port, .server = server, .client = client};
    _pair_main(&ctx);
}

static void _srv_echo_stream(void* arg) {
    xylem_mux_stream_t* s = (xylem_mux_stream_t*)arg;
    char buf[256];
    int  n = xylem_mux_read(s, buf, (int)sizeof(buf));
    if (n > 0) {
        xylem_mux_write(s, buf, n);
    }
    xylem_mux_close_stream(s);
}

static void _srv_echo_stream_tracked(void* arg) {
    _stream_arg_t* a = (_stream_arg_t*)arg;
    _srv_echo_stream(a->stream);
    xylem_waitgroup_done(a->wg);
}

static void _srv_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* ln = xylem_tcp_listen(MUX_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_SERVER, NULL);
    ASSERT(mux != NULL);

    xylem_mux_stream_t* s = xylem_mux_accept_stream(mux);
    ASSERT(s != NULL);
    _srv_echo_stream(s);

    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(MUX_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_CLIENT, NULL);
    ASSERT(mux != NULL);

    xylem_mux_stream_t* s = xylem_mux_open_stream(mux);
    ASSERT(s != NULL);

    const char* msg = "hello xylem mux";
    ASSERT(xylem_mux_write(s, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int  n = xylem_mux_read(s, buf, (int)sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_mux_close_stream(s);
    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_single_stream_echo(void) {
    _run_pair(MUX_PORT, _srv_worker, _cli_worker);
}

static void _multi_srv_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* ln = xylem_tcp_listen(MUX_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_SERVER, NULL);
    ASSERT(mux != NULL);

    xylem_waitgroup_t* swg = xylem_waitgroup_create();
    xylem_waitgroup_add(swg, 3);
    _stream_arg_t sargs[3];
    for (int i = 0; i < 3; i++) {
        xylem_mux_stream_t* s = xylem_mux_accept_stream(mux);
        ASSERT(s != NULL);
        sargs[i].stream = s;
        sargs[i].wg     = swg;
        xylem_spawn(_srv_echo_stream_tracked, &sargs[i]);
    }

    xylem_waitgroup_wait(swg);
    xylem_waitgroup_destroy(swg);

    xylem_mux_close(mux);
    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _multi_cli_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(MUX_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);

    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_CLIENT, NULL);
    ASSERT(mux != NULL);

    for (int i = 0; i < 3; i++) {
        xylem_mux_stream_t* s = xylem_mux_open_stream(mux);
        ASSERT(s != NULL);

        char msg[32];
        snprintf(msg, sizeof(msg), "stream-%d", i);
        ASSERT(xylem_mux_write(s, msg, (int)strlen(msg)) == 0);

        char buf[64];
        int  n = xylem_mux_read(s, buf, (int)sizeof(buf));
        ASSERT(n == (int)strlen(msg));
        ASSERT(memcmp(buf, msg, (size_t)n) == 0);

        xylem_mux_close_stream(s);
    }

    xylem_mux_close(mux);
    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_multiple_streams(void) {
    _run_pair(MUX_PORT + 1, _multi_srv_worker, _multi_cli_worker);
}

static void _flow_srv_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* ln = xylem_tcp_listen(MUX_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);
    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_SERVER, NULL);
    ASSERT(mux != NULL);
    xylem_mux_stream_t* s = xylem_mux_accept_stream(mux);
    ASSERT(s != NULL);

    for (int i = 0; i < 8; i++) {
        runtime_yield();
    }

    size_t total = 0;
    char   buf[16384];
    while (total < MUX_FLOW_SIZE) {
        int n = xylem_mux_read(s, buf, (int)sizeof(buf));
        ASSERT(n > 0);
        total += (size_t)n;
    }
    ASSERT(total == MUX_FLOW_SIZE);
    ASSERT(xylem_mux_write(s, "x", 1) == 0);

    xylem_mux_close_stream(s);
    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _flow_cli_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(MUX_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);
    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_CLIENT, NULL);
    ASSERT(mux != NULL);
    xylem_mux_stream_t* s = xylem_mux_open_stream(mux);
    ASSERT(s != NULL);

    char* data = (char*)calloc(MUX_FLOW_SIZE, 1);
    ASSERT(data != NULL);
    memset(data, 'x', MUX_FLOW_SIZE);
    ASSERT(xylem_mux_write(s, data, MUX_FLOW_SIZE) == 0);

    char ack;
    ASSERT(xylem_mux_read(s, &ack, 1) == 1);
    ASSERT(ack == 'x');

    free(data);
    xylem_mux_close_stream(s);
    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_send_window_wakes_writer(void) {
    _run_pair(MUX_PORT + 2, _flow_srv_worker, _flow_cli_worker);
}

static void _fin_read_worker(void* arg) {
    _fin_read_ctx_t* ctx = (_fin_read_ctx_t*)arg;
    char byte;

    ctx->result = xylem_mux_read(ctx->stream, &byte, 1);
    xylem_waitgroup_done(ctx->wg);
}

static void _fin_srv_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* ln = xylem_tcp_listen(MUX_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);
    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_SERVER, NULL);
    ASSERT(mux != NULL);
    xylem_mux_stream_t* s = xylem_mux_accept_stream(mux);
    ASSERT(s != NULL);

    xylem_channel_recv(ctx->ready);
    xylem_mux_close_stream(s);
    xylem_channel_recv(ctx->ready);

    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_tcp_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fin_cli_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(MUX_HOST, ctx->port, NULL);
    ASSERT(conn != NULL);
    xylem_mux_t* mux =
        xylem_mux_create(conn, XYLEM_MUX_TCP, XYLEM_MUX_CLIENT, NULL);
    ASSERT(mux != NULL);
    xylem_mux_stream_t* s = xylem_mux_open_stream(mux);
    ASSERT(s != NULL);

    xylem_waitgroup_t* wg = xylem_waitgroup_create();
    ASSERT(wg != NULL);
    xylem_waitgroup_add(wg, 1);
    _fin_read_ctx_t read_ctx = {.stream = s, .wg = wg, .result = -1};
    xylem_spawn(_fin_read_worker, &read_ctx);

    xylem_channel_send(ctx->ready, ctx);
    xylem_waitgroup_wait(wg);
    ASSERT(read_ctx.result == 0);
    xylem_channel_send(ctx->ready, ctx);

    xylem_waitgroup_destroy(wg);
    xylem_mux_close_stream(s);
    xylem_mux_destroy(mux);
    xylem_tcp_destroy(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_remote_fin_wakes_reader(void) {
    _run_pair(MUX_PORT + 3, _fin_srv_worker, _fin_cli_worker);
}

static void test_destroy_null(void) {
    xylem_mux_destroy(NULL);
}

static void test_close_null(void) {
    xylem_mux_close(NULL);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_close_null();
    test_destroy_null();
    test_single_stream_echo();
    test_multiple_streams();
    test_send_window_wakes_writer();
    test_remote_fin_wakes_reader();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}

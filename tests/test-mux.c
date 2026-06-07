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

#define MUX_HOST          "127.0.0.1"
#define MUX_PORT          14600
#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
} _ctx_t;

typedef struct {
    xylem_mux_stream_t* stream;
    xylem_waitgroup_t*  wg;
} _stream_arg_t;

static void _srv_echo_stream(void* arg) {
    xylem_mux_stream_t* s = (xylem_mux_stream_t*)arg;
    char buf[256];
    int n = xylem_mux_read(s, buf, (int)sizeof(buf));
    if (n > 0) {
        xylem_mux_write(s, buf, n);
    }
    xylem_mux_close_stream(s);
}

/**
 * Echo one stream, then signal the per-stream waitgroup so the server
 * worker can tear down the mux only after every stream coroutine is done.
 */
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

    xylem_mux_t* mux = xylem_mux_create(
        conn, XYLEM_MUX_TCP, XYLEM_MUX_SERVER, NULL);
    ASSERT(mux != NULL);

    xylem_mux_stream_t* s = xylem_mux_accept_stream(mux);
    ASSERT(s != NULL);
    _srv_echo_stream(s);

    xylem_mux_destroy(mux);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(MUX_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_mux_t* mux = xylem_mux_create(
        conn, XYLEM_MUX_TCP, XYLEM_MUX_CLIENT, NULL);
    ASSERT(mux != NULL);

    xylem_mux_stream_t* s = xylem_mux_open_stream(mux);
    ASSERT(s != NULL);

    const char* msg = "hello xylem mux";
    ASSERT(xylem_mux_write(s, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_mux_read(s, buf, (int)sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_mux_close_stream(s);
    xylem_mux_destroy(mux);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = MUX_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_srv_worker, &ctx);
    xylem_spawn(_cli_worker, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_single_stream_echo(void) {
    xylem_run(_echo_main, NULL, NULL);
}

static void _multi_srv_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen(
        MUX_HOST, (uint16_t)(ctx->port + 1), NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    xylem_mux_t* mux = xylem_mux_create(
        conn, XYLEM_MUX_TCP, XYLEM_MUX_SERVER, NULL);
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

    /**
     * Wait for all stream coroutines to finish before tearing down the
     * mux/conn/listener they depend on.
     */
    xylem_waitgroup_wait(swg);
    xylem_waitgroup_destroy(swg);

    xylem_mux_destroy(mux);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _multi_cli_worker(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(
        MUX_HOST, (uint16_t)(ctx->port + 1), 0, NULL);
    ASSERT(conn != NULL);

    xylem_mux_t* mux = xylem_mux_create(
        conn, XYLEM_MUX_TCP, XYLEM_MUX_CLIENT, NULL);
    ASSERT(mux != NULL);

    for (int i = 0; i < 3; i++) {
        xylem_mux_stream_t* s = xylem_mux_open_stream(mux);
        ASSERT(s != NULL);

        char msg[32];
        snprintf(msg, sizeof(msg), "stream-%d", i);
        ASSERT(xylem_mux_write(s, msg, (int)strlen(msg)) == 0);

        char buf[64];
        int n = xylem_mux_read(s, buf, (int)sizeof(buf));
        ASSERT(n == (int)strlen(msg));
        ASSERT(memcmp(buf, msg, (size_t)n) == 0);

        xylem_mux_close_stream(s);
    }

    xylem_mux_destroy(mux);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _multi_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = MUX_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_multi_srv_worker, &ctx);
    xylem_spawn(_multi_cli_worker, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_multiple_streams(void) {
    xylem_run(_multi_main, NULL, NULL);
}

int main(void) {
    test_single_stream_echo();
    test_multiple_streams();
    return 0;
}

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
#include "runtime/runtime.h"
#include "runtime/sched-timer.h"
#include "assert.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TCP_PORT          18080
#define TCP_HOST          "127.0.0.1"
#define SAFETY_TIMEOUT_MS 5000

static xylem_runtime_opts_t _rt_opts = { .workers = 2 };

typedef struct {
    xylem_tcp_listener_t* server;
    int                   tested;
} _test_ctx_t;

static void _safety_timeout_cb(sched_timer_t* timer, void* ud) {
    (void)ud;
    sched_timer_destroy(timer);
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    sched_timer_mgr_t* mgr =
        scheduler_get_timer_mgr(runtime_get_scheduler());
    sched_timer_t* t = sched_timer_create(mgr);
    sched_timer_start(t, _safety_timeout_cb, NULL,
                      SAFETY_TIMEOUT_MS, 0);
}

/* --- echo server coroutine --- */

static void _echo_handler(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char buf[256];
    fprintf(stderr, "  [handler] start\n");

    for (;;) {
        fprintf(stderr, "  [handler] recv...\n");
        int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
        fprintf(stderr, "  [handler] recv=%lld\n", (long long)n);
        if (n <= 0) break;
        if (xylem_tcp_send(conn, buf, (size_t)n) != 0) break;
    }
    xylem_tcp_close(conn);
    fprintf(stderr, "  [handler] done\n");
}

static void _echo_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    fprintf(stderr, "  [server] listen...\n");
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT, NULL);
    ASSERT(ctx->server != NULL);
    fprintf(stderr, "  [server] listen ok, accepting...\n");

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
        fprintf(stderr, "  [server] accept => %p\n", (void*)conn);
        if (!conn) break;
        xylem_runtime_spawn(_echo_handler, conn);
    }
    fprintf(stderr, "  [server] done\n");
}

/* --- test: basic send/recv --- */

static void _test_echo_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    fprintf(stderr, "  [client] sleep 50ms...\n");

    xylem_runtime_sleep(50);
    fprintf(stderr, "  [client] dial...\n");

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT, 0, NULL);
    fprintf(stderr, "  [client] dial => %p\n", (void*)conn);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem";
    fprintf(stderr, "  [client] send...\n");
    ASSERT(xylem_tcp_send(conn, msg, strlen(msg)) == 0);
    fprintf(stderr, "  [client] recv...\n");

    char buf[64];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    fprintf(stderr, "  [client] recv=%lld\n", (long long)n);
    ASSERT(n == (int64_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_echo_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_runtime_spawn(_echo_server, ctx);
    xylem_runtime_spawn(_test_echo_client, ctx);
}

static void test_echo(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_echo_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* --- test: fixed-length framing --- */

static void _fixed_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 1, NULL);
    ASSERT(ctx->server != NULL);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_send(conn, "ABCD", 4) == 0);
    xylem_runtime_sleep(30);
    ASSERT(xylem_tcp_send(conn, "EFGH", 4) == 0);

    xylem_runtime_sleep(100);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
}

static void _fixed_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_runtime_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT + 1, 0, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = {
        .type = XYLEM_TCP_FRAME_FIXED,
        .fixed = {.len = 8},
    };
    xylem_tcp_set_framing(conn, &frame);

    char buf[16];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == 8);
    ASSERT(memcmp(buf, "ABCDEFGH", 8) == 0);

    xylem_tcp_close(conn);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_fixed_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_runtime_spawn(_fixed_server, ctx);
    xylem_runtime_spawn(_fixed_client, ctx);
}

static void test_fixed(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_fixed_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* --- test: delimiter framing --- */

static void _delim_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 2, NULL);
    ASSERT(ctx->server != NULL);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_send(conn, "hello\r\nworld\r\n", 14) == 0);
    xylem_runtime_sleep(100);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
}

static void _delim_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_runtime_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT + 2, 0, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = {
        .type = XYLEM_TCP_FRAME_DELIMITER,
        .delimiter = {.delim = "\r\n", .delim_len = 2},
    };
    xylem_tcp_set_framing(conn, &frame);

    char line[64];
    int64_t len = xylem_tcp_recv(conn, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(strcmp(line, "hello") == 0);

    len = xylem_tcp_recv(conn, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(strcmp(line, "world") == 0);

    xylem_tcp_close(conn);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_delim_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_runtime_spawn(_delim_server, ctx);
    xylem_runtime_spawn(_delim_client, ctx);
}

static void test_delimiter(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_delim_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* --- test: length-prefixed framing --- */

static void _frame_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 3, NULL);
    ASSERT(ctx->server != NULL);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = {
        .type = XYLEM_TCP_FRAME_LENGTH,
        .length = {
            .header_size  = 2,
            .field_offset = 0,
            .field_size   = 2,
            .adjustment   = 0,
            .big_endian   = true,
        },
    };
    xylem_tcp_set_framing(conn, &frame);

    ASSERT(xylem_tcp_send(conn, "FRAME1", 6) == 0);

    xylem_runtime_sleep(100);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
}

static void _frame_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_runtime_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT + 3, 0, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t frame = {
        .type = XYLEM_TCP_FRAME_LENGTH,
        .length = {
            .header_size  = 2,
            .field_offset = 0,
            .field_size   = 2,
            .adjustment   = 0,
            .big_endian   = true,
        },
    };
    xylem_tcp_set_framing(conn, &frame);

    char buf[64];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == 6);
    ASSERT(memcmp(buf, "FRAME1", 6) == 0);

    xylem_tcp_close(conn);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_frame_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_runtime_spawn(_frame_server, ctx);
    xylem_runtime_spawn(_frame_client, ctx);
}

static void test_frame(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_frame_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* --- test: dial_timeout --- */

static void _timeout_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    xylem_tcp_conn_t* conn = xylem_tcp_dial("192.0.2.1", 9999, 200, NULL);
    ASSERT(conn == NULL);

    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_dial_timeout_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_runtime_spawn(_timeout_client, ctx);
}

static void test_dial_timeout(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_dial_timeout_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

/* --- test: userdata --- */

static void _userdata_test(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 4, NULL);
    ASSERT(ctx->server != NULL);

    int server_ud = 42;
    xylem_tcp_listener_set_userdata(ctx->server, &server_ud);
    ASSERT(xylem_tcp_listener_get_userdata(ctx->server) == &server_ud);

    xylem_tcp_close_listener(ctx->server);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_userdata_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_runtime_spawn(_userdata_test, ctx);
}

static void test_userdata(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_userdata_main, &ctx, &_rt_opts);
    ASSERT(ctx.tested == 1);
}

int main(void) {

    fprintf(stderr, "=== test_echo\n");
    test_echo();
    fprintf(stderr, "=== test_fixed\n");
    test_fixed();
    fprintf(stderr, "=== test_delimiter\n");
    test_delimiter();
    fprintf(stderr, "=== test_frame\n");
    test_frame();
    fprintf(stderr, "=== test_dial_timeout\n");
    test_dial_timeout();
    fprintf(stderr, "=== test_userdata\n");
    test_userdata();

    return 0;
}

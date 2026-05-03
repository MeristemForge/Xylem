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
#include "assert.h"

#include <string.h>

#define TCP_PORT          18080
#define TCP_HOST          "127.0.0.1"
#define SAFETY_TIMEOUT_MS 5000

typedef struct {
    xylem_tcp_listener_t* server;
    int                   tested;
} _test_ctx_t;

static void _safety_timeout_cb(loop_t* loop,
                                loop_timer_t* timer,
                                void* ud) {
    (void)loop; (void)ud;
    loop_stop_timer(timer);
    loop_destroy_timer(timer);
    xylem_runtime_stop();
    ASSERT(0 && "test timed out");
}

static void _start_safety_timer(void) {
    loop_timer_t* t = loop_create_timer(runtime_loop());
    loop_start_timer(t, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);
}

/* --- echo server coroutine --- */

static void _echo_handler(void* arg) {
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)arg;
    char buf[256];

    for (;;) {
        ssize_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
        if (n <= 0) break;
        if (xylem_tcp_send(conn, buf, (size_t)n) != 0) break;
    }
    xylem_tcp_close(conn);
}

static void _echo_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT, NULL);
    ASSERT(ctx->server != NULL);

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
        if (!conn) break;
        xylem_spawn(_echo_handler, conn);
    }
}

/* --- test: basic send/recv --- */

static void _test_echo_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    xylem_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem";
    ASSERT(xylem_tcp_send(conn, msg, strlen(msg)) == 0);

    char buf[64];
    ssize_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == (ssize_t)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_echo_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_echo_server, ctx);
    xylem_spawn(_test_echo_client, ctx);
}

static void test_echo(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_echo_main, &ctx, NULL);
    ASSERT(ctx.tested == 1);
}

/* --- test: recv_exact --- */

static void _exact_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 1, NULL);
    ASSERT(ctx->server != NULL);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_send(conn, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_tcp_send(conn, "EFGH", 4) == 0);

    xylem_sleep(100);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
}

static void _exact_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT + 1, NULL);
    ASSERT(conn != NULL);

    char buf[8];
    ASSERT(xylem_tcp_recv_exact(conn, buf, 8) == 0);
    ASSERT(memcmp(buf, "ABCDEFGH", 8) == 0);

    xylem_tcp_close(conn);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_recv_exact_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_exact_server, ctx);
    xylem_spawn(_exact_client, ctx);
}

static void test_recv_exact(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_recv_exact_main, &ctx, NULL);
    ASSERT(ctx.tested == 1);
}

/* --- test: recv_line --- */

static void _line_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 2, NULL);
    ASSERT(ctx->server != NULL);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_send(conn, "hello\r\nworld\n", 13) == 0);
    xylem_sleep(100);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
}

static void _line_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT + 2, NULL);
    ASSERT(conn != NULL);

    char line[64];
    ssize_t len = xylem_tcp_recv_line(conn, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(strcmp(line, "hello") == 0);

    len = xylem_tcp_recv_line(conn, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(strcmp(line, "world") == 0);

    xylem_tcp_close(conn);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_recv_line_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_line_server, ctx);
    xylem_spawn(_line_client, ctx);
}

static void test_recv_line(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_recv_line_main, &ctx, NULL);
    ASSERT(ctx.tested == 1);
}

/* --- test: framing (length-prefixed) --- */

static void _frame_server(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    ctx->server = xylem_tcp_listen(TCP_HOST, TCP_PORT + 3, NULL);
    ASSERT(ctx->server != NULL);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ctx->server);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t opts = {
        .header_size  = 2,
        .field_offset = 0,
        .field_size   = 2,
        .adjustment   = 0,
        .big_endian   = true,
    };

    const char* payload = "FRAME1";
    ASSERT(xylem_tcp_send_frame(conn, &opts, payload, 6) == 0);

    xylem_sleep(100);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ctx->server);
}

static void _frame_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_sleep(50);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, TCP_PORT + 3, NULL);
    ASSERT(conn != NULL);

    xylem_tcp_frame_opts_t opts = {
        .header_size  = 2,
        .field_offset = 0,
        .field_size   = 2,
        .adjustment   = 0,
        .big_endian   = true,
    };

    size_t out_len = 0;
    void* body = xylem_tcp_recv_frame(conn, &opts, &out_len);
    ASSERT(body != NULL);
    ASSERT(out_len == 6);
    ASSERT(memcmp(body, "FRAME1", 6) == 0);
    free(body);

    xylem_tcp_close(conn);
    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_frame_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_frame_server, ctx);
    xylem_spawn(_frame_client, ctx);
}

static void test_frame(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_frame_main, &ctx, NULL);
    ASSERT(ctx.tested == 1);
}

/* --- test: dial_timeout --- */

static void _timeout_client(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    xylem_tcp_conn_t* conn = xylem_tcp_dial_timeout("192.0.2.1",
                                                       9999, NULL, 200);
    ASSERT(conn == NULL);

    ctx->tested = 1;
    xylem_runtime_stop();
}

static void _test_dial_timeout_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    _start_safety_timer();
    xylem_spawn(_timeout_client, ctx);
}

static void test_dial_timeout(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_dial_timeout_main, &ctx, NULL);
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
    xylem_spawn(_userdata_test, ctx);
}

static void test_userdata(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_userdata_main, &ctx, NULL);
    ASSERT(ctx.tested == 1);
}

int main(void) {

    test_echo();
    test_recv_exact();
    test_recv_line();
    test_frame();
    test_dial_timeout();
    test_userdata();

    return 0;
}

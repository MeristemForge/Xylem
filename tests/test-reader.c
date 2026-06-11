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

#define TEST_HOST          "127.0.0.1"
#define TEST_PORT          14700
#define SAFETY_TIMEOUT_MS  10000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
} _ctx_t;

static void _srv_echo(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen(TEST_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    xylem_tcp_write(conn, buf, n);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_read(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TEST_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello reader";
    xylem_tcp_write(conn, msg, (int)strlen(msg));

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 64);
    ASSERT(rd != NULL);

    char buf[64];
    int n = xylem_reader_read(rd, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _read_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(0),
        .wg    = xylem_waitgroup_create(),
        .port  = TEST_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_echo, &ctx);
    xylem_spawn(_cli_read, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_reader_read(void) {
    xylem_run(_read_main, NULL, NULL);
}

static void _srv_lines(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen(
        TEST_HOST, (uint16_t)(ctx->port + 1), NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    const char* data = "line1\nline2\nline3\n";
    xylem_tcp_write(conn, data, (int)strlen(data));

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_read_until(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(
        TEST_HOST, (uint16_t)(ctx->port + 1), 0, NULL);
    ASSERT(conn != NULL);

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 8);
    ASSERT(rd != NULL);

    char line[64];
    int n = xylem_reader_read_until(rd, '\n', line, sizeof(line));
    ASSERT(n == 6);
    ASSERT(memcmp(line, "line1\n", 6) == 0);

    n = xylem_reader_read_until(rd, '\n', line, sizeof(line));
    ASSERT(n == 6);
    ASSERT(memcmp(line, "line2\n", 6) == 0);

    n = xylem_reader_read_until(rd, '\n', line, sizeof(line));
    ASSERT(n == 6);
    ASSERT(memcmp(line, "line3\n", 6) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _read_until_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(0),
        .wg    = xylem_waitgroup_create(),
        .port  = TEST_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_lines, &ctx);
    xylem_spawn(_cli_read_until, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_reader_read_until(void) {
    xylem_run(_read_until_main, NULL, NULL);
}

static void _srv_full(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen(
        TEST_HOST, (uint16_t)(ctx->port + 2), NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    const char* data = "0123456789ABCDEF";
    xylem_tcp_write(conn, data, 16);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_read_full(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(
        TEST_HOST, (uint16_t)(ctx->port + 2), 0, NULL);
    ASSERT(conn != NULL);

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 8);
    ASSERT(rd != NULL);

    char buf[16];
    int rc = xylem_reader_read_full(rd, buf, 16);
    ASSERT(rc == 16);
    ASSERT(memcmp(buf, "0123456789ABCDEF", 16) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _read_full_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(0),
        .wg    = xylem_waitgroup_create(),
        .port  = TEST_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_full, &ctx);
    xylem_spawn(_cli_read_full, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_reader_read_full(void) {
    xylem_run(_read_full_main, NULL, NULL);
}

int main(void) {
    test_reader_read();
    test_reader_read_until();
    test_reader_read_full();
    return 0;
}

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

#include <stdint.h>
#include <string.h>

#define TCP_HOST          "127.0.0.1"
#define TCP_PORT          18080
#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
} _ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    char buf[256];
    int n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tcp_write(conn, buf, n) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    const char* msg = "hello xylem";
    ASSERT(xylem_tcp_write(conn, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT,
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

static void _reader_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_tcp_write(conn, "EFGH", 4) == 0);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _reader_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    /* Use xylem_reader_t to read exactly 8 bytes (read_full). */
    uint8_t rd_buf[256];
    xylem_reader_t rd;
    xylem_reader_init(
        &rd, conn, (xylem_reader_fn_t)xylem_tcp_read, rd_buf, sizeof(rd_buf));

    char result[8];
    ASSERT(xylem_reader_read_full(&rd, result, 8) == 0);
    ASSERT(memcmp(result, "ABCDEFGH", 8) == 0);

    xylem_reader_deinit(&rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _reader_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT + 1,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_reader_server, &ctx);
    xylem_spawn(_reader_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_reader_full(void) {
    xylem_run(_reader_main, NULL, NULL);
}

static void _writer_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    /* Use xylem_writer_t to buffer multiple small writes. */
    uint8_t wr_buf[256];
    xylem_writer_t wr;
    xylem_writer_init(
        &wr, conn, (xylem_writer_fn_t)xylem_tcp_write, wr_buf, sizeof(wr_buf));

    ASSERT(xylem_writer_write(&wr, "hello", 5) == 0);
    ASSERT(xylem_writer_write(&wr, " ", 1) == 0);
    ASSERT(xylem_writer_write(&wr, "world", 5) == 0);
    ASSERT(xylem_writer_flush(&wr) == 0);

    xylem_writer_deinit(&wr);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _writer_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    uint8_t rd_buf[256];
    xylem_reader_t rd;
    xylem_reader_init(
        &rd, conn, (xylem_reader_fn_t)xylem_tcp_read, rd_buf, sizeof(rd_buf));

    char result[11];
    ASSERT(xylem_reader_read_full(&rd, result, 11) == 0);
    ASSERT(memcmp(result, "hello world", 11) == 0);

    xylem_reader_deinit(&rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _writer_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT + 2,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_writer_server, &ctx);
    xylem_spawn(_writer_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_writer_buffered(void) {
    xylem_run(_writer_main, NULL, NULL);
}

static void _timeout_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_conn_t* conn = xylem_tcp_dial("192.0.2.1", 9999, 200, NULL);
    ASSERT(conn == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _timeout_main(void* arg) {
    (void)arg;
    _ctx_t ctx = { .wg = xylem_waitgroup_create() };
    xylem_waitgroup_add(ctx.wg, 1);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_timeout_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_shutdown();
}

static void test_dial_timeout(void) {
    xylem_run(_timeout_main, NULL, NULL);
}

int main(void) {
    test_echo();
    test_reader_full();
    test_writer_buffered();
    test_dial_timeout();
    return 0;
}

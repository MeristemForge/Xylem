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

#define TEST_HOST          "127.0.0.1"
#define TEST_PORT          14800
#define SAFETY_TIMEOUT_MS  10000

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

static void _srv_recv(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen(TEST_HOST, ctx->port, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int total = 0;
    for (;;) {
        int n = xylem_tcp_read(conn, buf + total, (int)sizeof(buf) - total);
        if (n <= 0) break;
        total += n;
    }

    ASSERT(total == 9);
    ASSERT(memcmp(buf, "aaabbbccc", 9) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_write(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TEST_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_writer_t* wr = xylem_writer_create(conn, XYLEM_WRITER_TCP, 64);
    ASSERT(wr != NULL);

    ASSERT(xylem_writer_write(wr, "aaa", 3) == 0);
    ASSERT(xylem_writer_write(wr, "bbb", 3) == 0);
    ASSERT(xylem_writer_write(wr, "ccc", 3) == 0);

    xylem_writer_destroy(wr);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _write_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TEST_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_srv_recv, &ctx);
    xylem_spawn(_cli_write, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_writer_batched(void) {
    xylem_run(_write_main, NULL, NULL);
}

static void _srv_recv_large(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_tcp_listener_t* ln = xylem_tcp_listen(
        TEST_HOST, (uint16_t)(ctx->port + 1), NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(ln);
    ASSERT(conn != NULL);

    char buf[256];
    int total = 0;
    for (;;) {
        int n = xylem_tcp_read(conn, buf + total, (int)sizeof(buf) - total);
        if (n <= 0) break;
        total += n;
    }

    /* "ab" + "LARGE-DATA!!" = 14 bytes */
    ASSERT(total == 14);
    ASSERT(memcmp(buf, "abLARGE-DATA!!", 14) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_write_large(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(
        TEST_HOST, (uint16_t)(ctx->port + 1), 0, NULL);
    ASSERT(conn != NULL);

    xylem_writer_t* wr = xylem_writer_create(conn, XYLEM_WRITER_TCP, 8);
    ASSERT(wr != NULL);

    ASSERT(xylem_writer_write(wr, "ab", 2) == 0);
    ASSERT(xylem_writer_write(wr, "LARGE-DATA!!", 12) == 0);

    xylem_writer_destroy(wr);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _write_large_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TEST_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _watchdog_cb, NULL);
    xylem_spawn(_srv_recv_large, &ctx);
    xylem_spawn(_cli_write_large, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_writer_large_bypass(void) {
    xylem_run(_write_large_main, NULL, NULL);
}

int main(void) {
    test_writer_batched();
    test_writer_large_bypass();
    return 0;
}

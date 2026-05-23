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
#include <stdio.h>

#ifdef _WIN32
#define UDS_PATH "xylem-test-uds.sock"
#else
#define UDS_PATH "/tmp/xylem-test-uds.sock"
#endif

#define SAFETY_TIMEOUT_MS 10000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
} _ctx_t;

static void _watchdog_cb(xylem_timer_t* t, void* ud) {
    (void)t;
    (void)ud;
    ASSERT(0 && "test timed out");
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    char buf[256];
    int n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_uds_write(uds, buf, n) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    const char* msg = "hello xylem uds";
    ASSERT(xylem_uds_write(uds, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
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
    remove(UDS_PATH);
}

static void _reader_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    ASSERT(xylem_uds_write(uds, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_uds_write(uds, "EFGH", 4) == 0);
    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _reader_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    uint8_t rd_buf[256];
    xylem_reader_t rd;
    xylem_reader_init(
        &rd, uds, (xylem_reader_fn_t)xylem_uds_read, rd_buf, sizeof(rd_buf));

    char result[8];
    ASSERT(xylem_reader_read_full(&rd, result, 8) == 0);
    ASSERT(memcmp(result, "ABCDEFGH", 8) == 0);

    xylem_reader_deinit(&rd);
    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _reader_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
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
    remove(UDS_PATH);
}

int main(void) {
    test_echo();
    test_reader_full();
    return 0;
}

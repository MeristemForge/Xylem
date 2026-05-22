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

/* --- test_echo: basic send/recv round-trip --- */

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    char buf[256];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_uds_send(uds, buf, (size_t)n) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0, NULL);
    ASSERT(uds != NULL);

    const char* msg = "hello xylem uds";
    ASSERT(xylem_uds_send(uds, msg, strlen(msg)) == 0);

    char buf[64];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
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

/* --- test_fixed: XYLEM_FRAMING_FIXED framing --- */

static void _fixed_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    ASSERT(xylem_uds_send(uds, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_uds_send(uds, "EFGH", 4) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _fixed_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0, NULL);
    ASSERT(uds != NULL);

    xylem_framing_opts_t frame = {
        .type  = XYLEM_FRAMING_FIXED,
        .fixed = { .len = 8 },
    };
    xylem_uds_set_framing(uds, &frame);

    char buf[16];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == 8);
    ASSERT(memcmp(buf, "ABCDEFGH", 8) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _fixed_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_fixed_server, &ctx);
    xylem_spawn(_fixed_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_fixed(void) {
    xylem_run(_fixed_main, NULL, NULL);
    remove(UDS_PATH);
}

/* --- test_delimiter: XYLEM_FRAMING_DELIMITER with "\r\n" --- */

static void _delim_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    ASSERT(xylem_uds_send(uds, "hello\r\nworld\r\n", 14) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _delim_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0, NULL);
    ASSERT(uds != NULL);

    xylem_framing_opts_t frame = {
        .type      = XYLEM_FRAMING_DELIMITER,
        .delimiter = { .delim = "\r\n", .delim_len = 2 },
    };
    xylem_uds_set_framing(uds, &frame);

    char line[64];
    int64_t len = xylem_uds_recv(uds, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(memcmp(line, "hello", 5) == 0);

    len = xylem_uds_recv(uds, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(memcmp(line, "world", 5) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _delim_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_delim_server, &ctx);
    xylem_spawn(_delim_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_delimiter(void) {
    xylem_run(_delim_main, NULL, NULL);
    remove(UDS_PATH);
}

/* --- test_frame: XYLEM_FRAMING_LENFIELD_FIXINT with 2-byte big-endian --- */

static const xylem_framing_opts_t _len_frame = {
    .type            = XYLEM_FRAMING_LENFIELD_FIXINT,
    .lenfield_fixint = {
        .header_size  = 2,
        .field_offset = 0,
        .field_size   = 2,
        .adjustment   = 0,
        .big_endian   = true,
    },
};

static void _frame_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    xylem_framing_opts_t frame = _len_frame;
    xylem_uds_set_framing(uds, &frame);

    ASSERT(xylem_uds_send(uds, "FRAME1", 6) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0, NULL);
    ASSERT(uds != NULL);

    xylem_framing_opts_t frame = _len_frame;
    xylem_uds_set_framing(uds, &frame);

    char buf[64];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == 6);
    ASSERT(memcmp(buf, "FRAME1", 6) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_frame_server, &ctx);
    xylem_spawn(_frame_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_lenfield_fixint(void) {
    xylem_run(_frame_main, NULL, NULL);
    remove(UDS_PATH);
}

/* --- test_varint: XYLEM_FRAMING_LENFIELD_VARINT --- */

static void _varint_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH, NULL);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    xylem_framing_opts_t frame = {
        .type            = XYLEM_FRAMING_LENFIELD_VARINT,
        .lenfield_varint = { .prefix_size = 0, .adjustment = 0 },
    };
    xylem_uds_set_framing(uds, &frame);

    ASSERT(xylem_uds_send(uds, "VARINTmsg", 9) == 0);

    xylem_uds_close(uds);
    xylem_uds_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _varint_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0, NULL);
    ASSERT(uds != NULL);

    xylem_framing_opts_t frame = {
        .type            = XYLEM_FRAMING_LENFIELD_VARINT,
        .lenfield_varint = { .prefix_size = 0, .adjustment = 0 },
    };
    xylem_uds_set_framing(uds, &frame);

    char buf[64];
    int64_t n = xylem_uds_recv(uds, buf, sizeof(buf));
    ASSERT(n == 9);
    ASSERT(memcmp(buf, "VARINTmsg", 9) == 0);

    xylem_uds_close(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void _varint_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS, _watchdog_cb, NULL);
    xylem_spawn(_varint_server, &ctx);
    xylem_spawn(_varint_client, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_lenfield_varint(void) {
    xylem_run(_varint_main, NULL, NULL);
    remove(UDS_PATH);
}

int main(void) {
    test_echo();
    test_fixed();
    test_delimiter();
    test_lenfield_fixint();
    test_lenfield_varint();
    return 0;
}

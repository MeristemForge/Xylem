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
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_tcp_send(conn, buf, (size_t)n) == 0);

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
    ASSERT(xylem_tcp_send(conn, msg, strlen(msg)) == 0);

    char buf[64];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == (int64_t)strlen(msg));
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

static void _fixed_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_send(conn, "ABCD", 4) == 0);
    xylem_sleep(30);
    ASSERT(xylem_tcp_send(conn, "EFGH", 4) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _fixed_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_framing_opts_t frame = {
        .type  = XYLEM_FRAMING_FIXED,
        .fixed = { .len = 8 },
    };
    xylem_tcp_set_framing(conn, &frame);

    char buf[16];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == 8);
    ASSERT(memcmp(buf, "ABCDEFGH", 8) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _fixed_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT + 1,
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
}

static void _delim_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_send(conn, "hello\r\nworld\r\n", 14) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _delim_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_framing_opts_t frame = {
        .type      = XYLEM_FRAMING_DELIMITER,
        .delimiter = { .delim = "\r\n", .delim_len = 2 },
    };
    xylem_tcp_set_framing(conn, &frame);

    char line[64];
    int64_t len = xylem_tcp_recv(conn, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(memcmp(line, "hello", 5) == 0);

    len = xylem_tcp_recv(conn, line, sizeof(line));
    ASSERT(len == 5);
    ASSERT(memcmp(line, "world", 5) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _delim_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT + 2,
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
}

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
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    xylem_framing_opts_t frame = _len_frame;
    xylem_tcp_set_framing(conn, &frame);

    ASSERT(xylem_tcp_send(conn, "FRAME1", 6) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_framing_opts_t frame = _len_frame;
    xylem_tcp_set_framing(conn, &frame);

    char buf[64];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == 6);
    ASSERT(memcmp(buf, "FRAME1", 6) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _frame_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT + 3,
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
}

static void _varint_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    xylem_framing_opts_t frame = {
        .type            = XYLEM_FRAMING_LENFIELD_VARINT,
        .lenfield_varint = { .prefix_size = 0, .adjustment = 0 },
    };
    xylem_tcp_set_framing(conn, &frame);

    ASSERT(xylem_tcp_send(conn, "VARINTmsg", 9) == 0);

    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _varint_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_framing_opts_t frame = {
        .type            = XYLEM_FRAMING_LENFIELD_VARINT,
        .lenfield_varint = { .prefix_size = 0, .adjustment = 0 },
    };
    xylem_tcp_set_framing(conn, &frame);

    char    buf[64];
    int64_t n = xylem_tcp_recv(conn, buf, sizeof(buf));
    ASSERT(n == 9);
    ASSERT(memcmp(buf, "VARINTmsg", 9) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void _varint_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = TCP_PORT + 4,
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
    test_fixed();
    test_delimiter();
    test_lenfield_fixint();
    test_lenfield_varint();
    test_dial_timeout();
    return 0;
}

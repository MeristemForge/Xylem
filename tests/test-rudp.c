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

#define RUDP_HOST          "127.0.0.1"
#define RUDP_PORT          15000
#define SAFETY_TIMEOUT_MS  10000

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
} _ctx_t;

/* test_echo_stream */

static void _srv_echo_stream(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_STREAM};
    xylem_rudp_listener_t* ln = xylem_rudp_listen(
        RUDP_HOST, ctx->port, &opts);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* c = xylem_rudp_accept(ln);
    ASSERT(c != NULL);

    char buf[256];
    int n = xylem_rudp_read(c, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_rudp_write(c, buf, n) == 0);

    xylem_rudp_close(c);
    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_echo_stream(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_STREAM};
    xylem_rudp_conn_t* c = xylem_rudp_dial(
        RUDP_HOST, ctx->port, &opts);
    ASSERT(c != NULL);

    const char* msg = "hello rudp stream";
    ASSERT(xylem_rudp_write(c, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_rudp_read(c, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_rudp_close(c);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_stream_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = RUDP_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_echo_stream, &ctx);
    xylem_spawn(_cli_echo_stream, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_echo_stream(void) {
    xylem_run(_echo_stream_main, NULL, NULL);
}

/* test_echo_message */

static void _srv_echo_msg(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_MESSAGE};
    xylem_rudp_listener_t* ln = xylem_rudp_listen(
        RUDP_HOST, (uint16_t)(ctx->port + 1), &opts);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* c = xylem_rudp_accept(ln);
    ASSERT(c != NULL);

    char buf[256];
    int n = xylem_rudp_read(c, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_rudp_write(c, buf, n) == 0);

    xylem_rudp_close(c);
    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_echo_msg(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_MESSAGE};
    xylem_rudp_conn_t* c = xylem_rudp_dial(
        RUDP_HOST, (uint16_t)(ctx->port + 1), &opts);
    ASSERT(c != NULL);

    const char* msg = "hello rudp message";
    ASSERT(xylem_rudp_write(c, msg, (int)strlen(msg)) == 0);

    char buf[64];
    int n = xylem_rudp_read(c, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_rudp_close(c);
    xylem_waitgroup_done(ctx->wg);
}

static void _echo_msg_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = RUDP_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_echo_msg, &ctx);
    xylem_spawn(_cli_echo_msg, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_echo_message(void) {
    xylem_run(_echo_msg_main, NULL, NULL);
}

/* test_handshake_timeout */

static void _cli_timeout(void* arg) {
    (void)arg;

    xylem_rudp_opts_t opts = {
        .mode = XYLEM_RUDP_STREAM,
        .connect_timeout_ms = 500,
    };
    xylem_rudp_conn_t* c = xylem_rudp_dial(
        RUDP_HOST, (uint16_t)(RUDP_PORT + 2), &opts);
    ASSERT(c == NULL);

    xylem_shutdown();
}

static void test_handshake_timeout(void) {
    xylem_run(_cli_timeout, NULL, NULL);
}

/* test_close_while_reading */

static void _srv_close_early(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_STREAM};
    xylem_rudp_listener_t* ln = xylem_rudp_listen(
        RUDP_HOST, (uint16_t)(ctx->port + 3), &opts);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_rudp_conn_t* c = xylem_rudp_accept(ln);
    ASSERT(c != NULL);

    xylem_rudp_close(c);
    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_read_after_close(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_STREAM};
    xylem_rudp_conn_t* c = xylem_rudp_dial(
        RUDP_HOST, (uint16_t)(ctx->port + 3), &opts);
    ASSERT(c != NULL);

    xylem_rudp_set_read_deadline(c,
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 2000);

    char buf[64];
    int n = xylem_rudp_read(c, buf, sizeof(buf));
    ASSERT(n <= 0);

    xylem_rudp_close(c);
    xylem_waitgroup_done(ctx->wg);
}

static void _close_reading_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = RUDP_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_close_early, &ctx);
    xylem_spawn(_cli_read_after_close, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_close_while_reading(void) {
    xylem_run(_close_reading_main, NULL, NULL);
}

/* test_multi_session */

static void _srv_multi(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;

    xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_MESSAGE};
    xylem_rudp_listener_t* ln = xylem_rudp_listen(
        RUDP_HOST, (uint16_t)(ctx->port + 4), &opts);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    for (int i = 0; i < 2; i++) {
        xylem_rudp_conn_t* c = xylem_rudp_accept(ln);
        ASSERT(c != NULL);

        char buf[64];
        int n = xylem_rudp_read(c, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(xylem_rudp_write(c, buf, n) == 0);
        xylem_rudp_close(c);
    }

    xylem_rudp_close_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _cli_multi(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    for (int i = 0; i < 2; i++) {
        xylem_rudp_opts_t opts = {.mode = XYLEM_RUDP_MESSAGE};
        xylem_rudp_conn_t* c = xylem_rudp_dial(
            RUDP_HOST, (uint16_t)(ctx->port + 4), &opts);
        ASSERT(c != NULL);

        char msg[32];
        snprintf(msg, sizeof(msg), "session-%d", i);
        ASSERT(xylem_rudp_write(c, msg, (int)strlen(msg)) == 0);

        char buf[64];
        int n = xylem_rudp_read(c, buf, sizeof(buf));
        ASSERT(n == (int)strlen(msg));
        ASSERT(memcmp(buf, msg, (size_t)n) == 0);

        xylem_rudp_close(c);
    }

    xylem_waitgroup_done(ctx->wg);
}

static void _multi_main(void* arg) {
    (void)arg;
    _ctx_t ctx = {
        .ready = xylem_channel_create(),
        .wg    = xylem_waitgroup_create(),
        .port  = RUDP_PORT,
    };
    xylem_waitgroup_add(ctx.wg, 2);
    xylem_timer_t* wd = xylem_timer_after(SAFETY_TIMEOUT_MS,
                                          _utils_watchdog_cb, NULL);
    xylem_spawn(_srv_multi, &ctx);
    xylem_spawn(_cli_multi, &ctx);
    xylem_waitgroup_wait(ctx.wg);
    xylem_timer_cancel(wd);

    xylem_waitgroup_destroy(ctx.wg);
    xylem_channel_destroy(ctx.ready);
    xylem_shutdown();
}

static void test_multi_session(void) {
    xylem_run(_multi_main, NULL, NULL);
}

/* main */

int main(void) {
    test_echo_stream();
    test_echo_message();
    test_handshake_timeout();
    test_close_while_reading();
    test_multi_session();
    return 0;
}

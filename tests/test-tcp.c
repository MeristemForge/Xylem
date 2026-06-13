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

#include <stdint.h>
#include <string.h>

#define TCP_HOST          "127.0.0.1"
#define TCP_PORT          18080
#define SAFETY_TIMEOUT_MS 10000

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    uint16_t           port;
    _coro_t            server;
    _coro_t            client;
} _ctx_t;

static void _pair_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->ready  = xylem_channel_create(0);
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->server, ctx);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->ready);
    xylem_shutdown();
}

static void _run_pair(uint16_t port, _coro_t server, _coro_t client) {
    _ctx_t ctx = {.port = port, .server = server, .client = client};
    xylem_run(_pair_main, &ctx, NULL);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    char buf[256];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
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
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_echo(void) {
    _run_pair(TCP_PORT, _echo_server, _echo_client);
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

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 256);
    ASSERT(rd != NULL);

    char result[8];
    ASSERT(xylem_reader_read_full(rd, result, 8) == 8);
    ASSERT(memcmp(result, "ABCDEFGH", 8) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_reader_full(void) {
    _run_pair(TCP_PORT + 1, _reader_server, _reader_client);
}

static void _writer_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    xylem_writer_t* wr = xylem_writer_create(conn, XYLEM_WRITER_TCP, 256);
    ASSERT(wr != NULL);

    ASSERT(xylem_writer_write(wr, "hello", 5) == 0);
    ASSERT(xylem_writer_write(wr, " ", 1) == 0);
    ASSERT(xylem_writer_write(wr, "world", 5) == 0);
    ASSERT(xylem_writer_flush(wr) == 0);

    xylem_writer_destroy(wr);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _writer_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    xylem_reader_t* rd = xylem_reader_create(conn, XYLEM_READER_TCP, 256);
    ASSERT(rd != NULL);

    char result[11];
    ASSERT(xylem_reader_read_full(rd, result, 11) == 11);
    ASSERT(memcmp(result, "hello world", 11) == 0);

    xylem_reader_destroy(rd);
    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_writer_buffered(void) {
    _run_pair(TCP_PORT + 2, _writer_server, _writer_client);
}

static void _timeout_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_conn_t* conn = xylem_tcp_dial("192.0.2.1", 9999, 200, NULL);
    ASSERT(conn == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void _timeout_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_cancel(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_shutdown();
}

static void test_dial_timeout(void) {
    _ctx_t ctx = {.client = _timeout_client};
    xylem_run(_timeout_main, &ctx, NULL);
}

static void _eof_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "bye", 3) == 0);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _eof_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 3);
    ASSERT(memcmp(buf, "bye", 3) == 0);

    n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_peer_close_eof(void) {
    _run_pair(TCP_PORT + 3, _eof_server, _eof_client);
}

static void _half_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_tcp_listener_t* listener = xylem_tcp_listen(TCP_HOST, ctx->port, NULL);
    ASSERT(listener != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_tcp_conn_t* conn = xylem_tcp_accept(listener);
    ASSERT(conn != NULL);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "ping", 4) == 0);

    n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 0);

    ASSERT(xylem_tcp_write(conn, "pong", 4) == 0);
    xylem_tcp_close(conn);
    xylem_tcp_close_listener(listener);
    xylem_waitgroup_done(ctx->wg);
}

static void _half_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_tcp_conn_t* conn = xylem_tcp_dial(TCP_HOST, ctx->port, 0, NULL);
    ASSERT(conn != NULL);

    ASSERT(xylem_tcp_write(conn, "ping", 4) == 0);
    ASSERT(xylem_tcp_shutdown_wr(conn) == 0);

    char buf[16];
    int  n = xylem_tcp_read(conn, buf, sizeof(buf));
    ASSERT(n == 4);
    ASSERT(memcmp(buf, "pong", 4) == 0);

    xylem_tcp_close(conn);
    xylem_waitgroup_done(ctx->wg);
}

static void test_half_close(void) {
    _run_pair(TCP_PORT + 4, _half_server, _half_client);
}

int main(void) {
    test_echo();
    test_reader_full();
    test_writer_buffered();
    test_dial_timeout();
    test_peer_close_eof();
    test_half_close();
    return 0;
}

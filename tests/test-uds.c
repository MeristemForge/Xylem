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
#include "utils.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define UDS_PATH          "xylem-test-uds.sock"

typedef void (*_coro_t)(void*);

typedef struct {
    xylem_channel_t*   ready;
    xylem_waitgroup_t* wg;
    _coro_t            server;
    _coro_t            client;
} _ctx_t;

static void _pair_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->ready  = xylem_channel_create();
    ctx->wg     = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 2);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->server, ctx);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_destroy(wd);
    xylem_waitgroup_destroy(ctx->wg);
    xylem_channel_destroy(ctx->ready);
}

static void _run_pair(_coro_t server, _coro_t client) {
    _ctx_t ctx = {.server = server, .client = client};
    _pair_main(&ctx);
    remove(UDS_PATH);
}

static void _solo_main(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    ctx->wg = xylem_waitgroup_create();
    xylem_waitgroup_add(ctx->wg, 1);
    xylem_timer_t* wd =
        xylem_timer_after(SAFETY_TIMEOUT_MS, _utils_watchdog_cb, NULL);
    xylem_spawn(ctx->client, ctx);
    xylem_waitgroup_wait(ctx->wg);
    xylem_timer_destroy(wd);
    xylem_waitgroup_destroy(ctx->wg);
}

static void _run_solo(_coro_t client) {
    _ctx_t ctx = {.client = client};
    _solo_main(&ctx);
}

static void _echo_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    char buf[256];
    int  n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(xylem_uds_write(uds, buf, n) == 0);

    xylem_uds_destroy(uds);
    xylem_uds_destroy_listener(ln);
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
    int  n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n == (int)strlen(msg));
    ASSERT(memcmp(buf, msg, (size_t)n) == 0);

    xylem_uds_destroy(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void test_echo(void) {
    _run_pair(_echo_server, _echo_client);
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
    xylem_uds_destroy(uds);
    xylem_uds_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _reader_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    xylem_reader_t* rd = xylem_reader_create(uds, XYLEM_READER_UDS, 256);
    ASSERT(rd != NULL);

    char result[8];
    ASSERT(xylem_reader_read_full(rd, result, 8) == 8);
    ASSERT(memcmp(result, "ABCDEFGH", 8) == 0);

    xylem_reader_destroy(rd);
    xylem_uds_destroy(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void test_reader_full(void) {
    _run_pair(_reader_server, _reader_client);
}

static void _dial_fail_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_conn_t* uds = xylem_uds_dial("xylem-test-uds-missing.sock", 100);
    ASSERT(uds == NULL);
    xylem_waitgroup_done(ctx->wg);
}

static void test_dial_nonexistent(void) {
    _run_solo(_dial_fail_client);
}

static void _eof_server(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);
    xylem_channel_send(ctx->ready, ctx);

    xylem_uds_conn_t* uds = xylem_uds_accept(ln);
    ASSERT(uds != NULL);

    ASSERT(xylem_uds_write(uds, "bye", 3) == 0);
    xylem_uds_destroy(uds);
    xylem_uds_destroy_listener(ln);
    xylem_waitgroup_done(ctx->wg);
}

static void _eof_client(void* arg) {
    _ctx_t* ctx = (_ctx_t*)arg;
    xylem_channel_recv(ctx->ready);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    char buf[16];
    int  n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n == 3);
    ASSERT(memcmp(buf, "bye", 3) == 0);

    n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n == 0);

    ASSERT(runtime_consume_credit(UINT32_MAX));
    n = xylem_uds_read(uds, buf, sizeof(buf));
    ASSERT(n == 0);
    ASSERT(runtime_consume_credit(1));
    runtime_yield();

    xylem_uds_destroy(uds);
    xylem_waitgroup_done(ctx->wg);
}

static void test_peer_close_eof(void) {
    _run_pair(_eof_server, _eof_client);
}

static void test_closed_public_listener_operations(void) {
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);

    xylem_uds_close_listener(ln);
    xylem_uds_close_listener(ln);
    ASSERT(xylem_uds_accept(ln) == NULL);
    xylem_uds_destroy_listener(ln);
    xylem_uds_destroy_listener(NULL);
}

static void test_closed_public_connection_operations(void) {
    xylem_uds_listener_t* ln = xylem_uds_listen(UDS_PATH);
    ASSERT(ln != NULL);

    xylem_uds_conn_t* uds = xylem_uds_dial(UDS_PATH, 0);
    ASSERT(uds != NULL);

    xylem_uds_close(uds);
    xylem_uds_close(uds);
    xylem_uds_set_read_deadline(uds, 1);
    xylem_uds_set_write_deadline(uds, 1);

    char byte = 0;
    ASSERT(xylem_uds_read(uds, &byte, 1) == -1);
    ASSERT(xylem_uds_write(uds, &byte, 1) == -1);
    ASSERT(xylem_uds_shutdown_rd(uds) == -1);
    ASSERT(xylem_uds_shutdown_wr(uds) == -1);
    xylem_uds_destroy(uds);
    xylem_uds_destroy(NULL);

    xylem_uds_destroy_listener(ln);
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    test_echo();
    test_reader_full();
    test_dial_nonexistent();
    test_peer_close_eof();
    test_closed_public_listener_operations();
    test_closed_public_connection_operations();
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}

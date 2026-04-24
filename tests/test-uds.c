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

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define UDS_PATH "xylem-test-uds.sock"
#else
#define UDS_PATH "/tmp/xylem-test-uds.sock"
#endif

typedef struct {
    xylem_loop_t*        loop;
    xylem_uds_server_t*  server;
    xylem_uds_conn_t*    srv_conn;
    xylem_uds_conn_t*    cli_conn;
    int                  accept_called;
    int                  connect_called;
    int                  close_called;
    int                  read_count;
    int                  send_result;
    int                  value;
    char                 received[256];
    size_t               received_len;
    xylem_thrdpool_t*    pool;
    xylem_loop_timer_t*  close_timer;
    xylem_loop_timer_t*  check_timer;
    _Atomic bool         closed;
    _Atomic bool         worker_done;
} _test_ctx_t;

static void _safety_timeout_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _srv_accept_cb(xylem_uds_server_t* server,
                            xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_uds_server_get_userdata(server);
    ctx->srv_conn = conn;
    ctx->accept_called++;
    xylem_uds_set_userdata(conn, ctx);
}

static void _srv_read_echo_cb(xylem_uds_conn_t* conn,
                               void* data, size_t len) {
    xylem_uds_send(conn, data, len);
}

static void _dc_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->connect_called++;
    xylem_uds_close(conn);
    xylem_uds_close_server(ctx->server);
    xylem_loop_stop(ctx->loop);
}

static void test_dial_connect(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_handler_t srv_h = {.on_accept = _srv_accept_cb};
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {.on_connect = _dc_connect_cb};
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void _echo_cli_read_cb(xylem_uds_conn_t* conn,
                                void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    if (len <= sizeof(ctx->received) - ctx->received_len) {
        memcpy(ctx->received + ctx->received_len, data, len);
        ctx->received_len += len;
    }
    ctx->read_count++;
    xylem_uds_close(conn);
    xylem_uds_close(ctx->srv_conn);
    xylem_uds_close_server(ctx->server);
    xylem_loop_stop(ctx->loop);
}

static void _echo_cli_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->connect_called++;
    xylem_uds_send(conn, "hello", 5);
}

static void test_echo(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_handler_t srv_h = {
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_echo_cb,
    };
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
    };
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void _sac_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    xylem_uds_close(conn);
    ctx->send_result = xylem_uds_send(conn, "x", 1);
    xylem_uds_close_server(ctx->server);
    xylem_loop_stop(ctx->loop);
}

static void test_send_after_close(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_handler_t srv_h = {.on_accept = _srv_accept_cb};
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {.on_connect = _sac_connect_cb};
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.send_result == -1);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void _ud_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    int val = 42;
    xylem_uds_set_userdata(conn, &val);
    int* got = (int*)xylem_uds_get_userdata(conn);
    ASSERT(got == &val);
    ASSERT(*got == 42);

    /* Restore ctx for cleanup. */
    xylem_uds_set_userdata(conn, ctx);
    xylem_uds_close(conn);
    xylem_uds_close_server(ctx->server);
    xylem_loop_stop(ctx->loop);
}

static void test_userdata(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_handler_t srv_h = {.on_accept = _srv_accept_cb};
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {.on_connect = _ud_connect_cb};
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void test_server_userdata(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_uds_handler_t srv_h = {0};
    xylem_uds_server_t* server =
        xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(server != NULL);

    int val = 99;
    xylem_uds_server_set_userdata(server, &val);
    int* got = (int*)xylem_uds_server_get_userdata(server);
    ASSERT(got == &val);
    ASSERT(*got == 99);

    xylem_uds_close_server(server);
    xylem_loop_run(ctx.loop);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void _gl_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ASSERT(xylem_uds_get_loop(conn) == ctx->loop);
    xylem_uds_close(conn);
    xylem_uds_close_server(ctx->server);
    xylem_loop_stop(ctx->loop);
}

static void test_get_loop(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_handler_t srv_h = {.on_accept = _srv_accept_cb};
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {.on_connect = _gl_connect_cb};
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void _csac_close_cb(xylem_uds_conn_t* conn,
                            int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->close_called++;
    /* Stop the loop once the client connection is closed (EOF from server). */
    if (conn == ctx->cli_conn) {
        xylem_loop_stop(ctx->loop);
    }
}

static void _csac_timer_cb(xylem_loop_t* loop,
                            xylem_loop_timer_t* timer,
                            void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_close_server(ctx->server);
}

static void _csac_connect_cb(xylem_uds_conn_t* conn) {
    (void)conn;
}

static void test_close_server_with_active_conn(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_handler_t srv_h = {
        .on_accept = _srv_accept_cb,
        .on_close  = _csac_close_cb,
    };
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {
        .on_connect = _csac_connect_cb,
        .on_close   = _csac_close_cb,
    };
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    /* Close server after a short delay so the connection is established. */
    xylem_loop_timer_t* close_timer = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(close_timer, _csac_timer_cb, &ctx, 100, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called >= 1);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy_timer(close_timer);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

static void _ff_srv_read_cb(xylem_uds_conn_t* conn,
                             void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    if (ctx->received_len + len <= sizeof(ctx->received)) {
        memcpy(ctx->received + ctx->received_len, data, len);
        ctx->received_len += len;
    }
    ctx->read_count++;
    if (ctx->read_count == 2) {
        xylem_uds_close(conn);
        xylem_uds_close(ctx->cli_conn);
        xylem_uds_close_server(ctx->server);
        xylem_loop_stop(ctx->loop);
    }
}

static void _ff_cli_connect_cb(xylem_uds_conn_t* conn) {
    /* Send 8 bytes; with frame_size=4, server should get 2 frames. */
    xylem_uds_send(conn, "ABCDEFGH", 8);
}

static void test_frame_fixed(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_opts_t opts = {
        .framing = {.type = XYLEM_UDS_FRAME_FIXED, .fixed = {.frame_size = 4}},
    };

    xylem_uds_handler_t srv_h = {
        .on_accept = _srv_accept_cb,
        .on_read   = _ff_srv_read_cb,
    };
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, &opts);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {.on_connect = _ff_cli_connect_cb};
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.read_count == 2);
    ASSERT(ctx.received_len == 8);
    ASSERT(memcmp(ctx.received, "ABCD", 4) == 0);
    ASSERT(memcmp(ctx.received + 4, "EFGH", 4) == 0);

    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

/* cross-thread send */

static void _xt_send_srv_on_read(xylem_uds_conn_t* conn,
                                  void* data, size_t len) {
    xylem_uds_send(conn, data, len);
}

static void _xt_send_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_uds_send(ctx->cli_conn, "hello", 5);
    xylem_uds_conn_release(ctx->cli_conn);
}

static void _xt_send_cli_on_connect(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->cli_conn = conn;
    xylem_uds_conn_acquire(conn);
    xylem_thrdpool_post(ctx->pool, _xt_send_worker, ctx);
}

static void _xt_send_cli_on_read(xylem_uds_conn_t* conn,
                                  void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    if (ctx->received_len + len <= sizeof(ctx->received)) {
        memcpy(ctx->received + ctx->received_len, data, len);
        ctx->received_len += len;
    }
    if (ctx->received_len >= 5) {
        ctx->connect_called = 1;
        xylem_loop_stop(ctx->loop);
    }
}

static void test_cross_thread_send(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx.pool = xylem_thrdpool_create(1);
    ASSERT(ctx.pool != NULL);

    xylem_uds_handler_t srv_h = {
        .on_accept = _srv_accept_cb,
        .on_read   = _xt_send_srv_on_read,
    };
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {
        .on_connect = _xt_send_cli_on_connect,
        .on_read    = _xt_send_cli_on_read,
    };
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);

    xylem_thrdpool_destroy(ctx.pool);
    xylem_uds_close(ctx.cli_conn);
    xylem_uds_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

/* cross-thread close */

static void _xt_close_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    xylem_uds_close(ctx->cli_conn);
    xylem_uds_conn_release(ctx->cli_conn);
}

static void _xt_close_cli_on_connect(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->cli_conn = conn;
    xylem_uds_conn_acquire(conn);
    xylem_thrdpool_post(ctx->pool, _xt_close_worker, ctx);
}

static void _xt_close_cli_on_close(xylem_uds_conn_t* conn,
                                    int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->close_called = 1;
    xylem_loop_stop(ctx->loop);
}

static void test_cross_thread_close(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx.pool = xylem_thrdpool_create(1);
    ASSERT(ctx.pool != NULL);

    xylem_uds_handler_t srv_h = {0};
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);

    xylem_uds_handler_t cli_h = {
        .on_connect = _xt_close_cli_on_connect,
        .on_close   = _xt_close_cli_on_close,
    };
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called == 1);

    xylem_thrdpool_destroy(ctx.pool);
    xylem_uds_close_server(ctx.server);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

/* cross-thread send stops on close */

static void _xt_stop_srv_on_read(xylem_uds_conn_t* conn,
                                  void* data, size_t len) {
    (void)conn;
    (void)data;
    (void)len;
    /* Ignore incoming data; server will close via timer. */
}

static void _xt_stop_srv_close_timer_cb(xylem_loop_t* loop,
                                         xylem_loop_timer_t* timer,
                                         void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_close(ctx->srv_conn);
}

static void _xt_stop_srv_accept_cb(xylem_uds_server_t* server,
                                    xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_uds_server_get_userdata(server);
    ctx->srv_conn = conn;
    xylem_uds_set_userdata(conn, ctx);

    /* Close server-side connection after 50ms. */
    xylem_loop_start_timer(ctx->close_timer, _xt_stop_srv_close_timer_cb,
                           ctx, 50, 0);
}

static void _xt_stop_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    /* Send in a loop until the closed flag is set. */
    while (!atomic_load(&ctx->closed)) {
        xylem_uds_send(ctx->cli_conn, "ping", 4);
    }
    xylem_uds_conn_release(ctx->cli_conn);
    /*
     * Signal that the worker has stopped touching conn.
     * The loop thread waits for this before stopping.
     */
    atomic_store(&ctx->worker_done, true);
}

static void _xt_stop_cli_on_connect(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->cli_conn = conn;
    xylem_uds_conn_acquire(conn);
    xylem_thrdpool_post(ctx->pool, _xt_stop_worker, ctx);
}

static void _xt_stop_check_timer_cb(xylem_loop_t* loop,
                                     xylem_loop_timer_t* timer,
                                     void* ud) {
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    if (atomic_load(&ctx->worker_done)) {
        /*
         * Worker has exited the send loop and released its ref.
         * Destroy the pool (joins the thread) so no further
         * access to conn is possible, then stop the loop.
         */
        xylem_thrdpool_destroy(ctx->pool);
        ctx->pool = NULL;
        xylem_loop_stop(loop);
    }
}

static void _xt_stop_cli_on_close(xylem_uds_conn_t* conn,
                                   int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->close_called = 1;
    atomic_store(&ctx->closed, true);

    /* Poll for worker exit every 10ms. */
    xylem_loop_start_timer(ctx->check_timer, _xt_stop_check_timer_cb,
                           ctx, 10, 10);
}

static void test_cross_thread_send_stop_on_close(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx.pool = xylem_thrdpool_create(1);
    ASSERT(ctx.pool != NULL);
    ctx.close_timer = xylem_loop_create_timer(ctx.loop);
    ctx.check_timer = xylem_loop_create_timer(ctx.loop);
    atomic_store(&ctx.closed, false);
    atomic_store(&ctx.worker_done, false);

    xylem_uds_handler_t srv_h = {
        .on_accept = _xt_stop_srv_accept_cb,
        .on_read   = _xt_stop_srv_on_read,
    };
    ctx.server = xylem_uds_listen(ctx.loop, UDS_PATH, &srv_h, NULL);
    ASSERT(ctx.server != NULL);
    xylem_uds_server_set_userdata(ctx.server, &ctx);

    xylem_uds_handler_t cli_h = {
        .on_connect = _xt_stop_cli_on_connect,
        .on_close   = _xt_stop_cli_on_close,
    };
    ctx.cli_conn = xylem_uds_dial(ctx.loop, UDS_PATH, &cli_h, NULL);
    ASSERT(ctx.cli_conn != NULL);
    xylem_uds_set_userdata(ctx.cli_conn, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.close_called == 1);
    ASSERT(atomic_load(&ctx.worker_done) == true);

    if (ctx.pool) {
        xylem_thrdpool_destroy(ctx.pool);
    }
    xylem_uds_close_server(ctx.server);
    xylem_loop_destroy_timer(ctx.close_timer);
    xylem_loop_destroy_timer(ctx.check_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
    remove(UDS_PATH);
}

int main(void) {
    xylem_startup();

    test_dial_connect();
    test_echo();
    test_send_after_close();
    test_userdata();
    test_server_userdata();
    test_get_loop();
    test_close_server_with_active_conn();
    test_frame_fixed();
    test_cross_thread_send();
    test_cross_thread_close();
    test_cross_thread_send_stop_on_close();

    xylem_cleanup();
    return 0;
}

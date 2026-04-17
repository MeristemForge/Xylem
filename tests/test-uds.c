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

/* --- test_dial_connect --- */

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

/* --- test_echo --- */

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

/* --- test_send_after_close --- */

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

/* --- test_userdata --- */

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

/* --- test_server_userdata --- */

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

/* --- test_get_loop --- */

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

/* --- test_close_server_with_active_conn --- */

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

/* --- test_frame_fixed --- */

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

    xylem_cleanup();
    return 0;
}

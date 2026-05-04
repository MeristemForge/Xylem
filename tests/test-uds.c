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

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#ifdef _WIN32
#define UDS_PATH "xylem-test-uds.sock"
#else
#define UDS_PATH "/tmp/xylem-test-uds.sock"
#endif

typedef struct {
    xylem_uds_server_t*  server;
    xylem_uds_conn_t*    srv_conn;
    xylem_uds_conn_t*    cli_conn;
    xylem_uds_handler_t  srv_handler;
    xylem_uds_handler_t  cli_handler;
    int                  accept_called;
    int                  connect_called;
    int                  close_called;
    int                  read_count;
    int                  send_result;
    int                  value;
    char                 received[256];
    size_t               received_len;
    _Atomic bool         worker_done;
} _test_ctx_t;

static void _safety_timeout_cb(loop_t* loop,
                                loop_timer_t* timer,
                                void* ud) {
    (void)loop;
    (void)timer;
    (void)ud;
    xylem_runtime_stop();
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
    xylem_runtime_stop();
}

static void _test_dial_connect_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){.on_accept = _srv_accept_cb};
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){.on_connect = _dc_connect_cb};
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_dial_connect(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_dial_connect_main, &ctx, NULL);
    ASSERT(ctx.connect_called == 1);
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
    xylem_runtime_stop();
}

static void _echo_cli_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->connect_called++;
    xylem_uds_send(conn, "hello", 5);
}

static void _test_echo_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_echo_cb,
    };
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
    };
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_echo(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_echo_main, &ctx, NULL);
    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    remove(UDS_PATH);
}

static void _sac_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    xylem_uds_close(conn);
    ctx->send_result = xylem_uds_send(conn, "x", 1);
    xylem_uds_close_server(ctx->server);
    xylem_runtime_stop();
}

static void _test_send_after_close_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){.on_accept = _srv_accept_cb};
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){.on_connect = _sac_connect_cb};
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_send_after_close(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_send_after_close_main, &ctx, NULL);
    ASSERT(ctx.send_result == -1);
    remove(UDS_PATH);
}

static void _ud_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    int val = 42;
    xylem_uds_set_userdata(conn, &val);
    int* got = (int*)xylem_uds_get_userdata(conn);
    ASSERT(got == &val);
    ASSERT(*got == 42);

    xylem_uds_set_userdata(conn, ctx);
    xylem_uds_close(conn);
    xylem_uds_close_server(ctx->server);
    xylem_runtime_stop();
}

static void _test_userdata_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){.on_accept = _srv_accept_cb};
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){.on_connect = _ud_connect_cb};
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_userdata(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_userdata_main, &ctx, NULL);
    remove(UDS_PATH);
}

static void _test_server_userdata_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    ctx->srv_handler = (xylem_uds_handler_t){0};
    xylem_uds_server_t* server = xylem_uds_listen(UDS_PATH,
                                                   &ctx->srv_handler, NULL);
    ASSERT(server != NULL);

    int val = 99;
    xylem_uds_server_set_userdata(server, &val);
    int* got = (int*)xylem_uds_server_get_userdata(server);
    ASSERT(got == &val);
    ASSERT(*got == 99);
    ctx->value = *got;

    xylem_uds_close_server(server);
    xylem_runtime_stop();
}

static void test_server_userdata(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_server_userdata_main, &ctx, NULL);
    ASSERT(ctx.value == 99);
    remove(UDS_PATH);
}

static void _csac_close_cb(xylem_uds_conn_t* conn,
                            int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->close_called++;
    if (conn == ctx->cli_conn) {
        xylem_runtime_stop();
    }
}

static void _csac_timer_cb(loop_t* loop,
                            loop_timer_t* timer,
                            void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_close_server(ctx->server);
}

static void _csac_connect_cb(xylem_uds_conn_t* conn) {
    (void)conn;
}

static void _test_close_server_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){
        .on_accept = _srv_accept_cb,
        .on_close  = _csac_close_cb,
    };
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){
        .on_connect = _csac_connect_cb,
        .on_close   = _csac_close_cb,
    };
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);

    loop_timer_t* close_timer = loop_create_timer(runtime_get_loop());
    loop_start_timer(close_timer, _csac_timer_cb, ctx, 100, 0);
}

static void test_close_server_with_active_conn(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_close_server_main, &ctx, NULL);
    ASSERT(ctx.close_called >= 1);
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
        xylem_runtime_stop();
    }
}

static void _ff_cli_connect_cb(xylem_uds_conn_t* conn) {
    xylem_uds_send(conn, "ABCDEFGH", 8);
}

static void _test_frame_fixed_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    xylem_uds_opts_t opts = {
        .framing = {.type = XYLEM_UDS_FRAME_FIXED, .fixed = {.frame_size = 4}},
    };

    ctx->srv_handler = (xylem_uds_handler_t){
        .on_accept = _srv_accept_cb,
        .on_read   = _ff_srv_read_cb,
    };
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, &opts);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){.on_connect = _ff_cli_connect_cb};
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_frame_fixed(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_frame_fixed_main, &ctx, NULL);
    ASSERT(ctx.read_count == 2);
    ASSERT(ctx.received_len == 8);
    ASSERT(memcmp(ctx.received, "ABCD", 4) == 0);
    ASSERT(memcmp(ctx.received + 4, "EFGH", 4) == 0);
    remove(UDS_PATH);
}

/* --- Cross-thread send test --- */

static void _xt_send_post_cb(loop_t* loop, loop_post_t* req,
                              void* ud) {
    (void)loop;
    (void)req;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_send(ctx->cli_conn, "hello", 5);
}

static void _xt_send_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_post(runtime_get_loop(), _xt_send_post_cb, ctx);
}

static void _xt_send_cli_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    xylem_uds_conn_acquire(conn);
    dynpool_submit(runtime_get_dynpool(), _xt_send_worker, ctx);
}

static void _xt_send_cli_read_cb(xylem_uds_conn_t* conn,
                                  void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    if (len <= sizeof(ctx->received) - ctx->received_len) {
        memcpy(ctx->received + ctx->received_len, data, len);
        ctx->received_len += len;
    }
    xylem_uds_close(conn);
    xylem_uds_close(ctx->srv_conn);
    xylem_uds_close_server(ctx->server);
    xylem_runtime_stop();
}

static void _test_cross_thread_send_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){
        .on_accept = _srv_accept_cb,
        .on_read   = _srv_read_echo_cb,
    };
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){
        .on_connect = _xt_send_cli_connect_cb,
        .on_read    = _xt_send_cli_read_cb,
    };
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_cross_thread_send(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_cross_thread_send_main, &ctx, NULL);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    xylem_uds_conn_release(ctx.cli_conn);
    remove(UDS_PATH);
}

/* --- Cross-thread close test --- */

static void _xt_close_post_cb(loop_t* loop, loop_post_t* req,
                               void* ud) {
    (void)loop;
    (void)req;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_close(ctx->cli_conn);
}

static void _xt_close_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_post(runtime_get_loop(), _xt_close_post_cb, ctx);
}

static void _xt_close_cli_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    xylem_uds_conn_acquire(conn);
    dynpool_submit(runtime_get_dynpool(), _xt_close_worker, ctx);
}

static void _xt_close_cli_close_cb(xylem_uds_conn_t* conn,
                                    int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->close_called++;
    xylem_uds_close_server(ctx->server);
    xylem_runtime_stop();
}

static void _test_cross_thread_close_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){.on_accept = _srv_accept_cb};
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){
        .on_connect = _xt_close_cli_connect_cb,
        .on_close   = _xt_close_cli_close_cb,
    };
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_cross_thread_close(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_cross_thread_close_main, &ctx, NULL);
    ASSERT(ctx.close_called == 1);
    xylem_uds_conn_release(ctx.cli_conn);
    remove(UDS_PATH);
}

/* --- Cross-thread send + stop-on-close test --- */

static void _xt_soc_send_post_cb(loop_t* loop, loop_post_t* req,
                                  void* ud) {
    (void)loop;
    (void)req;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_send(ctx->cli_conn, "ping", 4);
}

static void _xt_soc_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    for (int i = 0; i < 20; i++) {
        loop_post(runtime_get_loop(), _xt_soc_send_post_cb, ctx);
    }
    atomic_store(&ctx->worker_done, true);
}

static void _xt_soc_srv_close_timer_cb(loop_t* loop,
                                        loop_timer_t* timer,
                                        void* ud) {
    (void)loop;
    loop_destroy_timer(timer);
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_uds_close(ctx->cli_conn);
}

static void _xt_soc_srv_read_cb(xylem_uds_conn_t* conn,
                                 void* data, size_t len) {
    (void)data;
    (void)len;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->read_count++;
    if (ctx->read_count == 1) {
        loop_timer_t* t = loop_create_timer(runtime_get_loop());
        loop_start_timer(t, _xt_soc_srv_close_timer_cb, ctx, 50, 0);
    }
}

static void _xt_soc_cli_connect_cb(xylem_uds_conn_t* conn) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    xylem_uds_conn_acquire(conn);
    dynpool_submit(runtime_get_dynpool(), _xt_soc_worker, ctx);
}

static void _xt_soc_cli_close_cb(xylem_uds_conn_t* conn,
                                  int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_uds_get_userdata(conn);
    ctx->close_called++;
    xylem_uds_close(ctx->srv_conn);
    xylem_uds_close_server(ctx->server);
    xylem_runtime_stop();
}

static void _test_cross_thread_soc_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    loop_timer_t* safety = loop_create_timer(runtime_get_loop());
    loop_start_timer(safety, _safety_timeout_cb, NULL, 10000, 0);

    ctx->srv_handler = (xylem_uds_handler_t){
        .on_accept = _srv_accept_cb,
        .on_read   = _xt_soc_srv_read_cb,
    };
    ctx->server = xylem_uds_listen(UDS_PATH, &ctx->srv_handler, NULL);
    ASSERT(ctx->server != NULL);
    xylem_uds_server_set_userdata(ctx->server, ctx);

    ctx->cli_handler = (xylem_uds_handler_t){
        .on_connect = _xt_soc_cli_connect_cb,
        .on_close   = _xt_soc_cli_close_cb,
    };
    ctx->cli_conn = xylem_uds_dial(UDS_PATH, &ctx->cli_handler, NULL);
    ASSERT(ctx->cli_conn != NULL);
    xylem_uds_set_userdata(ctx->cli_conn, ctx);
}

static void test_cross_thread_send_stop_on_close(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_start(_test_cross_thread_soc_main, &ctx, NULL);
    ASSERT(ctx.close_called == 1);
    xylem_uds_conn_release(ctx.cli_conn);
    remove(UDS_PATH);
}

int main(void) {

    test_dial_connect();
    test_echo();
    test_send_after_close();
    test_userdata();
    test_server_userdata();
    test_close_server_with_active_conn();
    test_frame_fixed();
    test_cross_thread_send();
    test_cross_thread_close();
    test_cross_thread_send_stop_on_close();

    return 0;
}

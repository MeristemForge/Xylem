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
#include "thrdpool.h"
#include "xylem/net/xylem-rudp.h"
#include "assert.h"
#include <stdatomic.h>
#include <string.h>

#define RUDP_PORT          16433
#define RUDP_HOST          "127.0.0.1"
#define SAFETY_TIMEOUT_MS  10000

typedef struct {
    xylem_rudp_server_t*   rudp_server;
    xylem_rudp_conn_t*     srv_session;
    xylem_rudp_conn_t*     cli_session;
    int                    accept_called;
    int                    connect_called;
    int                    close_called;
    int                    read_count;
    int                    verified;
    int                    value;
    int                    send_result;
    char                   received[256];
    size_t                 received_len;
    thrdpool_t*            pool;
    _Atomic bool           worker_done;
} _test_ctx_t;

typedef struct {
    _test_ctx_t*  tctx;
    xylem_rudp_conn_t* rudp;
    char          send_data[4];
    char          recv_data[4];
    size_t        recv_len;
    bool          done;
} _multi_cli_t;

static void _safety_timeout_cb(loop_t* loop,
                                loop_timer_t* timer,
                                void* ud) {
    (void)loop; (void)timer; (void)ud;
    xylem_runtime_shutdown();
}

static void _rudp_srv_accept_cb(xylem_rudp_server_t* server,
                                xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_rudp_server_get_userdata(server);
    if (ctx) {
        ctx->srv_session = rudp;
        ctx->accept_called++;
        xylem_rudp_set_userdata(rudp, ctx);
    }
}

static void _rudp_srv_read_echo_cb(xylem_rudp_conn_t* rudp,
                                    void* data, size_t len) {
    xylem_rudp_send(rudp, data, len);
}

static void _echo_send_timer_cb(loop_t* loop,
                                loop_timer_t* timer,
                                void* ud) {
    (void)loop; (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_send(ctx->cli_session, "hello", 5);
}

static void _echo_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
}

static void _echo_cli_read_cb(xylem_rudp_conn_t* rudp,
                               void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (len <= sizeof(ctx->received)) {
        memcpy(ctx->received, data, len);
        ctx->received_len = len;
    }
    ctx->read_count++;
    xylem_rudp_close(rudp);
}

static void _echo_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                                const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_handshake_echo_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
        .on_close   = _echo_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _echo_send_timer_cb, ctx, 100, 0);
}

static void test_handshake_and_echo(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_handshake_echo_main, &ctx, NULL);

    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.read_count >= 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    ASSERT(ctx.close_called >= 1);
}

/* test_session_userdata */

static void _ud_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);

    xylem_rudp_set_userdata(rudp, &ctx->value);
    void* got = xylem_rudp_get_userdata(rudp);
    ASSERT(got == &ctx->value);
    ASSERT(*(int*)got == 42);
    ctx->verified = 1;

    xylem_rudp_set_userdata(rudp, ctx);
    xylem_rudp_close(rudp);
}

static void _ud_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                              const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_session_userdata_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _ud_cli_connect_cb,
        .on_close   = _ud_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_session_userdata(void) {
    _test_ctx_t ctx = {0};
    ctx.value = 42;
    xylem_runtime_run(_test_session_userdata_main, &ctx, NULL);
    ASSERT(ctx.verified == 1);
}

/* test_server_userdata */

static void _test_server_userdata_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_server_set_userdata(ctx->rudp_server, &ctx->value);
    void* got = xylem_rudp_server_get_userdata(ctx->rudp_server);
    ASSERT(got == &ctx->value);
    ASSERT(*(int*)got == 99);

    xylem_rudp_close_server(ctx->rudp_server);
    xylem_runtime_shutdown();
}

static void test_server_userdata(void) {
    _test_ctx_t ctx = {0};
    ctx.value = 99;
    xylem_runtime_run(_test_server_userdata_main, &ctx, NULL);
}

/* test_peer_addr */

static void _pa_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);

    char ip[XYLEM_ADDR_MAXHOST];
    uint16_t port;
    int rc = xylem_rudp_remote_addr(rudp, ip, &port);
    ASSERT(rc == 0);
    ASSERT(strcmp(ip, RUDP_HOST) == 0);
    ASSERT(port == RUDP_PORT);

    ctx->verified = 1;
    xylem_rudp_close(rudp);
}

static void _pa_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                              const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_peer_addr_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _pa_cli_connect_cb,
        .on_close   = _pa_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_peer_addr(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_peer_addr_main, &ctx, NULL);
    ASSERT(ctx.verified == 1);
}

/* test_send_after_close */

static void _sac_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    xylem_rudp_close(rudp);
    ctx->send_result = xylem_rudp_send(rudp, "x", 1);
    ctx->verified = 1;
}

static void _sac_close_cb(xylem_rudp_conn_t* rudp, int err,
                           const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_send_after_close_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _sac_connect_cb,
        .on_close   = _sac_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_send_after_close(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_send_after_close_main, &ctx, NULL);
    ASSERT(ctx.verified == 1);
    ASSERT(ctx.send_result == -1);
}

/* test_close_idempotent */

static void _ci_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    xylem_rudp_close(rudp);
    xylem_rudp_close(rudp);
    ctx->verified = 1;
}

static void _ci_close_cb(xylem_rudp_conn_t* rudp, int err,
                          const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_close_idempotent_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _ci_connect_cb,
        .on_close   = _ci_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_close_idempotent(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_close_idempotent_main, &ctx, NULL);
    ASSERT(ctx.verified == 1);
}

/* test_close_server_with_active_session */

static void _csas_close_timer_cb(loop_t* loop,
                                  loop_timer_t* timer,
                                  void* ud) {
    (void)loop; (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_close_server(ctx->rudp_server);
    ctx->rudp_server = NULL;
}

static void _csas_srv_close_cb(xylem_rudp_conn_t* rudp, int err,
                                const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
    }
}

static void _csas_srv_accept_cb(xylem_rudp_server_t* server,
                                xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_rudp_server_get_userdata(server);
    if (ctx) {
        ctx->srv_session = rudp;
        ctx->accept_called++;
        xylem_rudp_set_userdata(rudp, ctx);
    }
}

static void _csas_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
}

static void _csas_drain_timer_cb(loop_t* loop,
                                  loop_timer_t* timer,
                                  void* ud) {
    (void)loop; (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    if (ctx->cli_session) {
        xylem_rudp_close(ctx->cli_session);
        ctx->cli_session = NULL;
    }
    xylem_runtime_shutdown();
}

static void _test_csas_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _csas_srv_accept_cb,
        .on_close  = _csas_srv_close_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    loop_timer_t* close_timer = loop_create_timer(loop);
    loop_start_timer(close_timer, _csas_close_timer_cb, ctx, 200, 0);

    loop_timer_t* drain_timer = loop_create_timer(loop);
    loop_start_timer(drain_timer, _csas_drain_timer_cb, ctx, 400, 0);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _csas_cli_connect_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_close_server_with_active_session(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_csas_main, &ctx, NULL);
    ASSERT(ctx.close_called >= 1);
}

/* test_send_before_handshake */

static void _test_send_before_handshake_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {0};
    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);

    ctx->send_result = xylem_rudp_send(ctx->cli_session, "x", 1);

    xylem_rudp_close(ctx->cli_session);
    xylem_rudp_close_server(ctx->rudp_server);
    xylem_runtime_shutdown();
}

static void test_send_before_handshake(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_send_before_handshake_main, &ctx, NULL);
    ASSERT(ctx.send_result == -1);
}

/* test_multi_session */

static void _multi_send_timer_cb(loop_t* loop,
                                  loop_timer_t* timer,
                                  void* ud) {
    (void)loop; (void)timer;
    _multi_cli_t* mc = (_multi_cli_t*)ud;
    xylem_rudp_send(mc->rudp, mc->send_data, 3);
}

static void _multi_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _multi_cli_t* mc = (_multi_cli_t*)xylem_rudp_get_userdata(rudp);
    mc->rudp = rudp;
}

static void _multi_cli_read_cb(xylem_rudp_conn_t* rudp,
                                void* data, size_t len) {
    _multi_cli_t* mc = (_multi_cli_t*)xylem_rudp_get_userdata(rudp);
    if (len <= sizeof(mc->recv_data)) {
        memcpy(mc->recv_data, data, len);
        mc->recv_len = len;
    }
    mc->done = true;
    xylem_rudp_close(rudp);
}

static void _multi_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                                 const char* errmsg) {
    (void)err; (void)errmsg;
    _multi_cli_t* mc = (_multi_cli_t*)xylem_rudp_get_userdata(rudp);
    _test_ctx_t* ctx = mc->tctx;
    ctx->close_called++;

    if (ctx->close_called >= 2) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_multi_session_main(void* arg) {
    void** args = (void**)arg;
    _test_ctx_t* ctx = (_test_ctx_t*)args[0];
    _multi_cli_t* mc1 = (_multi_cli_t*)args[1];
    _multi_cli_t* mc2 = (_multi_cli_t*)args[2];
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _multi_cli_connect_cb,
        .on_read    = _multi_cli_read_cb,
        .on_close   = _multi_cli_close_cb,
    };

    xylem_rudp_conn_t* c1 = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                         &cli_handler, NULL);
    ASSERT(c1 != NULL);
    xylem_rudp_set_userdata(c1, mc1);

    xylem_rudp_conn_t* c2 = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                         &cli_handler, NULL);
    ASSERT(c2 != NULL);
    xylem_rudp_set_userdata(c2, mc2);

    loop_timer_t* send1 = loop_create_timer(loop);
    loop_start_timer(send1, _multi_send_timer_cb, mc1, 150, 0);

    loop_timer_t* send2 = loop_create_timer(loop);
    loop_start_timer(send2, _multi_send_timer_cb, mc2, 150, 0);
}

static void test_multi_session(void) {
    _test_ctx_t ctx = {0};
    _multi_cli_t mc1 = {0};
    mc1.tctx = &ctx;
    memcpy(mc1.send_data, "AAA", 3);

    _multi_cli_t mc2 = {0};
    mc2.tctx = &ctx;
    memcpy(mc2.send_data, "BBB", 3);

    void* args[3] = { &ctx, &mc1, &mc2 };
    xylem_runtime_run(_test_multi_session_main, args, NULL);

    ASSERT(ctx.accept_called == 2);
    ASSERT(mc1.done == true);
    ASSERT(mc1.recv_len == 3);
    ASSERT(memcmp(mc1.recv_data, "AAA", 3) == 0);
    ASSERT(mc2.done == true);
    ASSERT(mc2.recv_len == 3);
    ASSERT(memcmp(mc2.recv_data, "BBB", 3) == 0);
}

/* test_handshake_timeout */

static void _ht_close_cb(xylem_rudp_conn_t* rudp, int err,
                          const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_runtime_shutdown();
    }
}

static void _test_handshake_timeout_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_opts_t opts = {0};
    opts.handshake_ms = 200;

    xylem_rudp_handler_t cli_handler = {
        .on_close = _ht_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, &opts);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_handshake_timeout(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_handshake_timeout_main, &ctx, NULL);
    ASSERT(ctx.close_called == 1);
}

/* test_aes_echo */

static void _test_aes_echo_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };

    xylem_rudp_opts_t opts = {0};
    opts.aes_key = key;

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, &opts);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
        .on_close   = _echo_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, &opts);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _echo_send_timer_cb, ctx, 100, 0);
}

static void test_aes_echo(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_aes_echo_main, &ctx, NULL);

    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.read_count >= 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    ASSERT(ctx.close_called >= 1);
}

/* test_aes_with_fec */

static void _test_aes_fec_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    uint8_t key[32] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
    };

    xylem_rudp_opts_t opts = {0};
    opts.aes_key    = key;
    opts.fec_data   = 3;
    opts.fec_parity = 1;

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, &opts);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
        .on_close   = _echo_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, &opts);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _echo_send_timer_cb, ctx, 100, 0);
}

static void test_aes_with_fec(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_aes_fec_main, &ctx, NULL);

    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.read_count >= 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    ASSERT(ctx.close_called >= 1);
}

/* ── Cross-thread tests ─────────────────────────────────────────────── */

static void _xt_send_post_cb(loop_t* loop, loop_post_t* req,
                             void* ud) {
    (void)loop; (void)req;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_send(ctx->cli_session, "hello", 5);
}

static void _xt_send_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_post(runtime_get_loop(), _xt_send_post_cb, ctx);
    atomic_store(&ctx->worker_done, true);
}

static void _xt_send_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
    xylem_rudp_conn_acquire(rudp);
    thrdpool_submit(ctx->pool, _xt_send_worker, ctx);
}

static void _xt_send_cli_read_cb(xylem_rudp_conn_t* rudp,
                                  void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (len <= sizeof(ctx->received)) {
        memcpy(ctx->received, data, len);
        ctx->received_len = len;
    }
    ctx->read_count++;
    xylem_rudp_close(rudp);
}

static void _xt_send_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                                   const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_rudp_conn_release(rudp);
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_cross_thread_send_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    ctx->pool = thrdpool_create(1);
    ASSERT(ctx->pool != NULL);

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _xt_send_cli_connect_cb,
        .on_read    = _xt_send_cli_read_cb,
        .on_close   = _xt_send_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_cross_thread_send(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_cross_thread_send_main, &ctx, NULL);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    ASSERT(ctx.close_called >= 1);
    ASSERT(atomic_load(&ctx.worker_done) == true);

    thrdpool_destroy(ctx.pool);
}

/* test_cross_thread_close */

static void _xt_close_post_cb(loop_t* loop, loop_post_t* req,
                              void* ud) {
    (void)loop; (void)req;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_close(ctx->cli_session);
}

static void _xt_close_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_post(runtime_get_loop(), _xt_close_post_cb, ctx);
    atomic_store(&ctx->worker_done, true);
}

static void _xt_close_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
    xylem_rudp_conn_acquire(rudp);
    thrdpool_submit(ctx->pool, _xt_close_worker, ctx);
}

static void _xt_close_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                                    const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_rudp_conn_release(rudp);
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _test_cross_thread_close_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    ctx->pool = thrdpool_create(1);
    ASSERT(ctx->pool != NULL);

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _xt_close_cli_connect_cb,
        .on_close   = _xt_close_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);
}

static void test_cross_thread_close(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_cross_thread_close_main, &ctx, NULL);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.close_called == 1);
    ASSERT(atomic_load(&ctx.worker_done) == true);

    thrdpool_destroy(ctx.pool);
}

/* test_cross_thread_send_stop_on_close */

static void _xt_sc_send_post_cb(loop_t* loop, loop_post_t* req,
                                void* ud) {
    (void)loop; (void)req;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_send(ctx->cli_session, "ping", 4);
}

static void _xt_sc_worker(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    for (int i = 0; i < 10; i++) {
        loop_post(runtime_get_loop(), _xt_sc_send_post_cb, ctx);
    }
    atomic_store(&ctx->worker_done, true);
}

static void _xt_sc_cli_connect_cb(xylem_rudp_conn_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
    xylem_rudp_conn_acquire(rudp);
    thrdpool_submit(ctx->pool, _xt_sc_worker, ctx);
}

static void _xt_sc_cli_close_cb(xylem_rudp_conn_t* rudp, int err,
                                 const char* errmsg) {
    (void)err; (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_rudp_conn_release(rudp);
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_runtime_shutdown();
    }
}

static void _xt_sc_close_timer_cb(loop_t* loop,
                                   loop_timer_t* timer,
                                   void* ud) {
    (void)loop; (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_close(ctx->cli_session);
}

static void _xt_sc_srv_read_cb(xylem_rudp_conn_t* rudp,
                                void* data, size_t len) {
    (void)data; (void)len;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->read_count++;
}

static void _test_cross_thread_soc_main(void* arg) {
    _test_ctx_t* ctx = (_test_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    ctx->pool = thrdpool_create(1);
    ASSERT(ctx->pool != NULL);

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _xt_sc_srv_read_cb,
    };

    ctx->rudp_server = xylem_rudp_listen(RUDP_HOST, RUDP_PORT,
                                         &srv_handler, NULL);
    ASSERT(ctx->rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx->rudp_server, ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _xt_sc_cli_connect_cb,
        .on_close   = _xt_sc_cli_close_cb,
    };

    ctx->cli_session = xylem_rudp_dial(RUDP_HOST, RUDP_PORT,
                                       &cli_handler, NULL);
    ASSERT(ctx->cli_session != NULL);
    xylem_rudp_set_userdata(ctx->cli_session, ctx);

    loop_timer_t* close_timer = loop_create_timer(loop);
    loop_start_timer(close_timer, _xt_sc_close_timer_cb, ctx, 200, 0);
}

static void test_cross_thread_send_stop_on_close(void) {
    _test_ctx_t ctx = {0};
    xylem_runtime_run(_test_cross_thread_soc_main, &ctx, NULL);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.close_called == 1);
    ASSERT(atomic_load(&ctx.worker_done) == true);

    thrdpool_destroy(ctx.pool);
}

int main(void) {

    test_handshake_and_echo();
    test_session_userdata();
    test_server_userdata();
    test_peer_addr();
    test_send_after_close();
    test_close_idempotent();
    test_close_server_with_active_session();
    test_send_before_handshake();
    test_multi_session();
    test_handshake_timeout();
    test_aes_echo();
    test_aes_with_fec();
    test_cross_thread_send();
    test_cross_thread_close();
    test_cross_thread_send_stop_on_close();

    return 0;
}

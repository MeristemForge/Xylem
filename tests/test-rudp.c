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
#include "xylem/xylem-rudp.h"
#include "assert.h"

#include <string.h>

#define RUDP_PORT          16433
#define RUDP_HOST          "127.0.0.1"
#define SAFETY_TIMEOUT_MS  10000

typedef struct {
    xylem_loop_t*          loop;
    xylem_rudp_server_t*   rudp_server;
    xylem_rudp_t*          srv_session;
    xylem_rudp_t*          cli_session;
    xylem_rudp_ctx_t*      ctx;
    int                    accept_called;
    int                    connect_called;
    int                    close_called;
    int                    read_count;
    int                    verified;
    int                    value;
    int                    send_result;
    char                   received[256];
    size_t                 received_len;
} _test_ctx_t;

typedef struct {
    _test_ctx_t*  tctx;
    xylem_rudp_t* rudp;
    char          send_data[4];
    char          recv_data[4];
    size_t        recv_len;
    bool          done;
} _multi_cli_t;

/* Shared callbacks. */

static void _safety_timeout_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _rudp_srv_accept_cb(xylem_rudp_server_t* server,
                                xylem_rudp_t* rudp) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_rudp_server_get_userdata(server);
    if (ctx) {
        ctx->srv_session = rudp;
        ctx->accept_called++;
        xylem_rudp_set_userdata(rudp, ctx);
    }
}

static void _rudp_srv_read_echo_cb(xylem_rudp_t* rudp,
                                    void* data, size_t len) {
    xylem_rudp_send(rudp, data, len);
}


static void test_ctx_create_destroy(void) {
    xylem_rudp_ctx_t* ctx = xylem_rudp_ctx_create();
    ASSERT(ctx != NULL);
    xylem_rudp_ctx_destroy(ctx);
}


/**
 * Use a timer to send data after handshake completes. Sending
 * inside on_connect can race with the event loop on Windows
 * (the loop may exit before the server processes the KCP packet).
 */
static void _echo_send_timer_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer,
                                void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_send(ctx->cli_session, "hello", 5);
}

static void _echo_cli_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
}

static void _echo_cli_read_cb(xylem_rudp_t* rudp,
                               void* data, size_t len) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (len <= sizeof(ctx->received)) {
        memcpy(ctx->received, data, len);
        ctx->received_len = len;
    }
    ctx->read_count++;
    xylem_rudp_close(rudp);
}

static void _echo_cli_close_cb(xylem_rudp_t* rudp, int err,
                                const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_handshake_and_echo(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx.rudp_server, &ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
        .on_close   = _echo_cli_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_timer_t* send_timer = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _echo_send_timer_cb, &ctx, 100, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.accept_called == 1);
    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.read_count >= 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);
    ASSERT(ctx.close_called >= 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}

static void test_fast_mode_echo(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_opts_t opts = {0};
    opts.mode = XYLEM_RUDP_MODE_FAST;

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, &opts);
    ASSERT(ctx.rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx.rudp_server, &ctx);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _echo_cli_connect_cb,
        .on_read    = _echo_cli_read_cb,
        .on_close   = _echo_cli_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, &opts);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_timer_t* send_timer = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _echo_send_timer_cb, &ctx, 100, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.connect_called == 1);
    ASSERT(ctx.received_len == 5);
    ASSERT(memcmp(ctx.received, "hello", 5) == 0);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _ud_cli_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);

    xylem_rudp_set_userdata(rudp, &ctx->value);
    void* got = xylem_rudp_get_userdata(rudp);
    ASSERT(got == &ctx->value);
    ASSERT(*(int*)got == 42);
    ctx->verified = 1;

    xylem_rudp_set_userdata(rudp, ctx);
    xylem_rudp_close(rudp);
}

static void _ud_cli_close_cb(xylem_rudp_t* rudp, int err,
                              const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_session_userdata(void) {
    _test_ctx_t ctx = {0};
    ctx.loop  = xylem_loop_create();
    ctx.ctx   = xylem_rudp_ctx_create();
    ctx.value = 42;
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _ud_cli_connect_cb,
        .on_close   = _ud_cli_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);
    ASSERT(ctx.verified == 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void test_server_userdata(void) {
    _test_ctx_t ctx = {0};
    ctx.loop  = xylem_loop_create();
    ctx.ctx   = xylem_rudp_ctx_create();
    ctx.value = 99;
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_server_set_userdata(ctx.rudp_server, &ctx.value);
    void* got = xylem_rudp_server_get_userdata(ctx.rudp_server);
    ASSERT(got == &ctx.value);
    ASSERT(*(int*)got == 99);

    xylem_rudp_close_server(ctx.rudp_server);
    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_run(ctx.loop);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _pa_cli_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);

    const xylem_addr_t* peer = xylem_rudp_get_peer_addr(rudp);
    ASSERT(peer != NULL);

    char ip[64];
    uint16_t port;
    xylem_addr_ntop(peer, ip, sizeof(ip), &port);
    ASSERT(strcmp(ip, RUDP_HOST) == 0);
    ASSERT(port == RUDP_PORT);

    ctx->verified = 1;
    xylem_rudp_close(rudp);
}

static void _pa_cli_close_cb(xylem_rudp_t* rudp, int err,
                              const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_peer_addr(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _pa_cli_connect_cb,
        .on_close   = _pa_cli_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);
    ASSERT(ctx.verified == 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _gl_cli_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ASSERT(xylem_rudp_get_loop(rudp) == ctx->loop);
    ctx->verified = 1;
    xylem_rudp_close(rudp);
}

static void _gl_cli_close_cb(xylem_rudp_t* rudp, int err,
                              const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_get_loop(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _gl_cli_connect_cb,
        .on_close   = _gl_cli_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);
    ASSERT(ctx.verified == 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _sac_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    xylem_rudp_close(rudp);
    ctx->send_result = xylem_rudp_send(rudp, "x", 1);
    ctx->verified = 1;
}

static void _sac_close_cb(xylem_rudp_t* rudp, int err,
                           const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_send_after_close(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _sac_connect_cb,
        .on_close   = _sac_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.verified == 1);
    ASSERT(ctx.send_result == -1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _ci_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    xylem_rudp_close(rudp);
    xylem_rudp_close(rudp);
    ctx->verified = 1;
}

static void _ci_close_cb(xylem_rudp_t* rudp, int err,
                          const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_close_idempotent(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _ci_connect_cb,
        .on_close   = _ci_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);
    ASSERT(ctx.verified == 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _csas_close_timer_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    xylem_rudp_close_server(ctx->rudp_server);
    ctx->rudp_server = NULL;
}

static void _csas_srv_close_cb(xylem_rudp_t* rudp, int err,
                                const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
    }
}

static void _csas_srv_accept_cb(xylem_rudp_server_t* server,
                                xylem_rudp_t* rudp) {
    _test_ctx_t* ctx =
        (_test_ctx_t*)xylem_rudp_server_get_userdata(server);
    if (ctx) {
        ctx->srv_session = rudp;
        ctx->accept_called++;
        xylem_rudp_set_userdata(rudp, ctx);
    }
}

static void _csas_cli_connect_cb(xylem_rudp_t* rudp) {
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    ctx->connect_called = 1;
}

/**
 * Client close fires after close_server destroys the UDP socket
 * and the client's connected socket gets an error or we close it
 * manually. We use a drain timer to close the client after the
 * server has been torn down.
 */
static void _csas_drain_timer_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    _test_ctx_t* ctx = (_test_ctx_t*)ud;
    if (ctx->cli_session) {
        xylem_rudp_close(ctx->cli_session);
        ctx->cli_session = NULL;
    }
    xylem_loop_stop(ctx->loop);
}

static void test_close_server_with_active_session(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _csas_srv_accept_cb,
        .on_close  = _csas_srv_close_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx.rudp_server, &ctx);

    /* Close server 200ms after start. */
    xylem_loop_timer_t* close_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(close_timer, _csas_close_timer_cb, &ctx,
                           200, 0);

    /* Drain timer: clean up client and stop loop after server closes. */
    xylem_loop_timer_t* drain_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(drain_timer, _csas_drain_timer_cb, &ctx,
                           400, 0);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _csas_cli_connect_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    /* Server session's on_close should have fired. */
    ASSERT(ctx.close_called >= 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(drain_timer);
    xylem_loop_destroy_timer(close_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void test_send_before_handshake(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);

    xylem_rudp_handler_t cli_handler = {0};

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, NULL);
    ASSERT(ctx.cli_session != NULL);

    int rc = xylem_rudp_send(ctx.cli_session, "x", 1);
    ASSERT(rc == -1);

    xylem_rudp_close(ctx.cli_session);
    xylem_rudp_close_server(ctx.rudp_server);

    xylem_loop_run(ctx.loop);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}



static void _multi_send_timer_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    _multi_cli_t* mc = (_multi_cli_t*)ud;
    xylem_rudp_send(mc->rudp, mc->send_data, 3);
}

static void _multi_cli_connect_cb(xylem_rudp_t* rudp) {
    _multi_cli_t* mc = (_multi_cli_t*)xylem_rudp_get_userdata(rudp);
    mc->rudp = rudp;
}

static void _multi_cli_read_cb(xylem_rudp_t* rudp,
                                void* data, size_t len) {
    _multi_cli_t* mc = (_multi_cli_t*)xylem_rudp_get_userdata(rudp);
    if (len <= sizeof(mc->recv_data)) {
        memcpy(mc->recv_data, data, len);
        mc->recv_len = len;
    }
    mc->done = true;
    xylem_rudp_close(rudp);
}

static void _multi_cli_close_cb(xylem_rudp_t* rudp, int err,
                                 const char* errmsg) {
    (void)err;
    (void)errmsg;
    _multi_cli_t* mc = (_multi_cli_t*)xylem_rudp_get_userdata(rudp);
    _test_ctx_t* ctx = mc->tctx;
    ctx->close_called++;

    if (ctx->close_called >= 2) {
        xylem_rudp_close_server(ctx->rudp_server);
        xylem_loop_stop(ctx->loop);
    }
}

static void test_multi_session(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_rudp_handler_t srv_handler = {
        .on_accept = _rudp_srv_accept_cb,
        .on_read   = _rudp_srv_read_echo_cb,
    };

    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    ctx.rudp_server = xylem_rudp_listen(ctx.loop, &addr, ctx.ctx,
                                         &srv_handler, NULL);
    ASSERT(ctx.rudp_server != NULL);
    xylem_rudp_server_set_userdata(ctx.rudp_server, &ctx);

    _multi_cli_t mc1 = {0};
    mc1.tctx = &ctx;
    memcpy(mc1.send_data, "AAA", 3);

    _multi_cli_t mc2 = {0};
    mc2.tctx = &ctx;
    memcpy(mc2.send_data, "BBB", 3);

    xylem_rudp_handler_t cli_handler = {
        .on_connect = _multi_cli_connect_cb,
        .on_read    = _multi_cli_read_cb,
        .on_close   = _multi_cli_close_cb,
    };

    xylem_rudp_t* c1 = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                         &cli_handler, NULL);
    ASSERT(c1 != NULL);
    xylem_rudp_set_userdata(c1, &mc1);

    xylem_rudp_t* c2 = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                         &cli_handler, NULL);
    ASSERT(c2 != NULL);
    xylem_rudp_set_userdata(c2, &mc2);

    xylem_loop_timer_t* send1 = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send1, _multi_send_timer_cb, &mc1, 150, 0);

    xylem_loop_timer_t* send2 = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send2, _multi_send_timer_cb, &mc2, 150, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.accept_called == 2);
    ASSERT(mc1.done == true);
    ASSERT(mc1.recv_len == 3);
    ASSERT(memcmp(mc1.recv_data, "AAA", 3) == 0);
    ASSERT(mc2.done == true);
    ASSERT(mc2.recv_len == 3);
    ASSERT(memcmp(mc2.recv_data, "BBB", 3) == 0);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(send1);
    xylem_loop_destroy_timer(send2);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


static void _ht_close_cb(xylem_rudp_t* rudp, int err,
                          const char* errmsg) {
    (void)err;
    (void)errmsg;
    _test_ctx_t* ctx = (_test_ctx_t*)xylem_rudp_get_userdata(rudp);
    if (ctx) {
        ctx->close_called++;
        xylem_loop_stop(ctx->loop);
    }
}

static void test_handshake_timeout(void) {
    _test_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ctx.ctx  = xylem_rudp_ctx_create();
    ASSERT(ctx.loop != NULL);
    ASSERT(ctx.ctx != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL,
                           SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t addr;
    xylem_addr_pton(RUDP_HOST, RUDP_PORT, &addr);

    xylem_rudp_opts_t opts = {0};
    opts.handshake_ms = 200;

    xylem_rudp_handler_t cli_handler = {
        .on_close = _ht_close_cb,
    };

    ctx.cli_session = xylem_rudp_dial(ctx.loop, &addr, ctx.ctx,
                                       &cli_handler, &opts);
    ASSERT(ctx.cli_session != NULL);
    xylem_rudp_set_userdata(ctx.cli_session, &ctx);

    xylem_loop_run(ctx.loop);

    /**
     * On Linux/macOS the connected UDP socket may receive ECONNREFUSED
     * (ICMP port unreachable) before the handshake timer fires, closing
     * the session via the transport error path instead of the timeout
     * path. Either way on_close must be delivered exactly once.
     */
    ASSERT(ctx.close_called == 1);

    xylem_rudp_ctx_destroy(ctx.ctx);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}


int main(void) {
    xylem_startup();

    test_ctx_create_destroy();
    test_handshake_and_echo();
    test_fast_mode_echo();
    test_session_userdata();
    test_server_userdata();
    test_peer_addr();
    test_get_loop();
    test_send_after_close();
    test_close_idempotent();
    test_close_server_with_active_session();
    test_send_before_handshake();
    test_multi_session();
    test_handshake_timeout();

    xylem_cleanup();
    return 0;
}

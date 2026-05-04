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
#include "assert.h"

#include <stdatomic.h>
#include <string.h>

#define PORT_A              19001
#define PORT_B              19002
#define UDP_HOST            "127.0.0.1"

#define SAFETY_TIMEOUT_MS   2000
#define SEND_DELAY_MS       10
#define DRAIN_DELAY_MS      50

/* test_listen_recv context */
typedef struct {
    xylem_udp_t*  receiver;
    xylem_udp_t*  sender;
    int           read_called;
    char          data[64];
    size_t        data_len;
    uint16_t      sender_port;
    char          sender_ip[46];
} _lr_ctx_t;

/* test_listen_send context */
typedef struct {
    xylem_udp_t*  sender;
    xylem_udp_t*  receiver;
    int           read_called;
    char          data[64];
    size_t        data_len;
} _ls_ctx_t;

/* test_dial_echo context */
typedef struct {
    xylem_udp_t*  server;
    xylem_udp_t*  client;
    char          srv_data[64];
    size_t        srv_data_len;
    char          cli_data[64];
    size_t        cli_data_len;
} _de_ctx_t;

/* test_dial_addr context */
typedef struct {
    xylem_udp_t*  server;
    xylem_udp_t*  client;
    int           read_called;
    uint16_t      addr_port;
    char          addr_ip[46];
} _da_ctx_t;

/* test_datagram_boundary context */
typedef struct {
    xylem_udp_t*  receiver;
    xylem_udp_t*  sender;
    int           read_count;
    size_t        sizes[3];
    char          bufs[3][4];
} _db_ctx_t;

static void _safety_timeout_cb(loop_t* loop,
                                loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer; (void)ud;
    xylem_runtime_stop();
}

static void _stop_cb(loop_t* loop, loop_timer_t* timer,
                      void* ud) {
    (void)loop; (void)timer; (void)ud;
    xylem_runtime_stop();
}

static void _lr_on_read(xylem_udp_t* udp, void* data, size_t len,
                         const char* host, uint16_t port) {
    _lr_ctx_t* ctx = (_lr_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->read_called = 1;
    if (len < sizeof(ctx->data)) {
        memcpy(ctx->data, data, len);
        ctx->data_len = len;
    }
    strncpy(ctx->sender_ip, host, sizeof(ctx->sender_ip) - 1);
    ctx->sender_port = port;
    xylem_runtime_stop();
}

static void _lr_send_timer_cb(loop_t* loop,
                               loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer;
    _lr_ctx_t* ctx = (_lr_ctx_t*)ud;
    xylem_udp_send(ctx->sender, UDP_HOST, PORT_A, "hello", 5);
}

static void _test_listen_recv_main(void* arg) {
    _lr_ctx_t* ctx = (_lr_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t recv_handler = {.on_read = _lr_on_read};
    ctx->receiver = xylem_udp_listen(UDP_HOST, PORT_A, &recv_handler);
    ASSERT(ctx->receiver != NULL);
    xylem_udp_set_userdata(ctx->receiver, ctx);

    xylem_udp_handler_t send_handler = {0};
    ctx->sender = xylem_udp_listen(UDP_HOST, PORT_B, &send_handler);
    ASSERT(ctx->sender != NULL);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _lr_send_timer_cb, ctx, SEND_DELAY_MS, 0);
}

static void test_listen_recv(void) {
    _lr_ctx_t ctx = {0};
    xylem_runtime_start(_test_listen_recv_main, &ctx, NULL);

    ASSERT(ctx.read_called == 1);
    ASSERT(ctx.data_len == 5);
    ASSERT(memcmp(ctx.data, "hello", 5) == 0);
    ASSERT(ctx.sender_port == PORT_B);
    ASSERT(strcmp(ctx.sender_ip, UDP_HOST) == 0);
}

static void _ls_on_read(xylem_udp_t* udp, void* data, size_t len,
                         const char* host, uint16_t port) {
    (void)host; (void)port;
    _ls_ctx_t* ctx = (_ls_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->read_called = 1;
    if (len < sizeof(ctx->data)) {
        memcpy(ctx->data, data, len);
        ctx->data_len = len;
    }
    xylem_runtime_stop();
}

static void _ls_send_timer_cb(loop_t* loop,
                               loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer;
    _ls_ctx_t* ctx = (_ls_ctx_t*)ud;
    xylem_udp_send(ctx->sender, UDP_HOST, PORT_B, "reply", 5);
}

static void _test_listen_send_main(void* arg) {
    _ls_ctx_t* ctx = (_ls_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t a_handler = {0};
    ctx->sender = xylem_udp_listen(UDP_HOST, PORT_A, &a_handler);
    ASSERT(ctx->sender != NULL);

    xylem_udp_handler_t b_handler = {.on_read = _ls_on_read};
    ctx->receiver = xylem_udp_listen(UDP_HOST, PORT_B, &b_handler);
    ASSERT(ctx->receiver != NULL);
    xylem_udp_set_userdata(ctx->receiver, ctx);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _ls_send_timer_cb, ctx, SEND_DELAY_MS, 0);
}

static void test_listen_send(void) {
    _ls_ctx_t ctx = {0};
    xylem_runtime_start(_test_listen_send_main, &ctx, NULL);

    ASSERT(ctx.read_called == 1);
    ASSERT(ctx.data_len == 5);
    ASSERT(memcmp(ctx.data, "reply", 5) == 0);
}

static void _de_cli_on_read(xylem_udp_t* udp, void* data, size_t len,
                              const char* host, uint16_t port) {
    (void)host; (void)port;
    _de_ctx_t* ctx = (_de_ctx_t*)xylem_udp_get_userdata(udp);
    if (len < sizeof(ctx->cli_data)) {
        memcpy(ctx->cli_data, data, len);
        ctx->cli_data_len = len;
    }
    xylem_runtime_stop();
}

static void _de_srv_on_read(xylem_udp_t* udp, void* data, size_t len,
                              const char* host, uint16_t port) {
    _de_ctx_t* ctx = (_de_ctx_t*)xylem_udp_get_userdata(udp);
    if (len < sizeof(ctx->srv_data)) {
        memcpy(ctx->srv_data, data, len);
        ctx->srv_data_len = len;
    }
    xylem_udp_send(udp, host, port, "pong", 4);
}

static void _de_send_timer_cb(loop_t* loop,
                               loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer;
    _de_ctx_t* ctx = (_de_ctx_t*)ud;
    xylem_udp_send(ctx->client, NULL, 0, "ping", 4);
}

static void _test_dial_echo_main(void* arg) {
    _de_ctx_t* ctx = (_de_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t srv_handler = {.on_read = _de_srv_on_read};
    ctx->server = xylem_udp_listen(UDP_HOST, PORT_A, &srv_handler);
    ASSERT(ctx->server != NULL);
    xylem_udp_set_userdata(ctx->server, ctx);

    xylem_udp_handler_t cli_handler = {.on_read = _de_cli_on_read};
    ctx->client = xylem_udp_dial(UDP_HOST, PORT_A, &cli_handler);
    ASSERT(ctx->client != NULL);
    xylem_udp_set_userdata(ctx->client, ctx);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _de_send_timer_cb, ctx, SEND_DELAY_MS, 0);
}

static void test_dial_echo(void) {
    _de_ctx_t ctx = {0};
    xylem_runtime_start(_test_dial_echo_main, &ctx, NULL);

    ASSERT(ctx.srv_data_len == 4);
    ASSERT(memcmp(ctx.srv_data, "ping", 4) == 0);
    ASSERT(ctx.cli_data_len == 4);
    ASSERT(memcmp(ctx.cli_data, "pong", 4) == 0);
}

static void _da_cli_on_read(xylem_udp_t* udp, void* data, size_t len,
                              const char* host, uint16_t port) {
    (void)data; (void)len;
    _da_ctx_t* ctx = (_da_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->read_called = 1;
    strncpy(ctx->addr_ip, host, sizeof(ctx->addr_ip) - 1);
    ctx->addr_port = port;
    xylem_runtime_stop();
}

static void _da_srv_on_read(xylem_udp_t* udp, void* data, size_t len,
                              const char* host, uint16_t port) {
    (void)data; (void)len;
    xylem_udp_send(udp, host, port, "echo", 4);
}

static void _da_send_timer_cb(loop_t* loop,
                               loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer;
    _da_ctx_t* ctx = (_da_ctx_t*)ud;
    xylem_udp_send(ctx->client, NULL, 0, "hi", 2);
}

static void _test_dial_addr_main(void* arg) {
    _da_ctx_t* ctx = (_da_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t srv_handler = {.on_read = _da_srv_on_read};
    ctx->server = xylem_udp_listen(UDP_HOST, PORT_A, &srv_handler);
    ASSERT(ctx->server != NULL);

    xylem_udp_handler_t cli_handler = {.on_read = _da_cli_on_read};
    ctx->client = xylem_udp_dial(UDP_HOST, PORT_A, &cli_handler);
    ASSERT(ctx->client != NULL);
    xylem_udp_set_userdata(ctx->client, ctx);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _da_send_timer_cb, ctx, SEND_DELAY_MS, 0);
}

static void test_dial_addr(void) {
    _da_ctx_t ctx = {0};
    xylem_runtime_start(_test_dial_addr_main, &ctx, NULL);

    ASSERT(ctx.read_called == 1);
    ASSERT(strcmp(ctx.addr_ip, UDP_HOST) == 0);
    ASSERT(ctx.addr_port == PORT_A);
}

static void _db_on_read(xylem_udp_t* udp, void* data, size_t len,
                          const char* host, uint16_t port) {
    (void)host; (void)port;
    _db_ctx_t* ctx = (_db_ctx_t*)xylem_udp_get_userdata(udp);
    if (ctx->read_count < 3) {
        ctx->sizes[ctx->read_count] = len;
        if (len <= sizeof(ctx->bufs[0])) {
            memcpy(ctx->bufs[ctx->read_count], data, len);
        }
        ctx->read_count++;
    }
    if (ctx->read_count >= 3) {
        xylem_runtime_stop();
    }
}

static void _db_send_timer_cb(loop_t* loop,
                                loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer;
    _db_ctx_t* ctx = (_db_ctx_t*)ud;
    xylem_udp_send(ctx->sender, UDP_HOST, PORT_A, "A", 1);
    xylem_udp_send(ctx->sender, UDP_HOST, PORT_A, "BB", 2);
    xylem_udp_send(ctx->sender, UDP_HOST, PORT_A, "CCC", 3);
}

static void _test_datagram_boundary_main(void* arg) {
    _db_ctx_t* ctx = (_db_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t recv_handler = {.on_read = _db_on_read};
    ctx->receiver = xylem_udp_listen(UDP_HOST, PORT_A, &recv_handler);
    ASSERT(ctx->receiver != NULL);
    xylem_udp_set_userdata(ctx->receiver, ctx);

    xylem_udp_handler_t send_handler = {0};
    ctx->sender = xylem_udp_listen(UDP_HOST, PORT_B, &send_handler);
    ASSERT(ctx->sender != NULL);

    loop_timer_t* send_timer = loop_create_timer(loop);
    loop_start_timer(send_timer, _db_send_timer_cb, ctx, SEND_DELAY_MS, 0);
}

static void test_datagram_boundary(void) {
    _db_ctx_t ctx = {0};
    xylem_runtime_start(_test_datagram_boundary_main, &ctx, NULL);

    ASSERT(ctx.read_count == 3);
    ASSERT(ctx.sizes[0] == 1);
    ASSERT(ctx.sizes[1] == 2);
    ASSERT(ctx.sizes[2] == 3);
    ASSERT(memcmp(ctx.bufs[0], "A", 1) == 0);
    ASSERT(memcmp(ctx.bufs[1], "BB", 2) == 0);
    ASSERT(memcmp(ctx.bufs[2], "CCC", 3) == 0);
}

/* test_close_idempotent */

typedef struct {
    xylem_udp_t* udp;
} _ci_ctx_t;

static void _test_close_idempotent_main(void* arg) {
    _ci_ctx_t* ctx = (_ci_ctx_t*)arg;

    xylem_udp_handler_t handler = {0};
    ctx->udp = xylem_udp_listen(UDP_HOST, PORT_A, &handler);
    ASSERT(ctx->udp != NULL);

    xylem_udp_close(ctx->udp);
    xylem_udp_close(ctx->udp);

    loop_timer_t* drain = loop_create_timer(runtime_get_loop());
    loop_start_timer(drain, _stop_cb, NULL, DRAIN_DELAY_MS, 0);
}

static void test_close_idempotent(void) {
    _ci_ctx_t ctx = {0};
    xylem_runtime_start(_test_close_idempotent_main, &ctx, NULL);
}

/* test_close_callback */

typedef struct {
    int called;
} _cc_ctx_t;

static void _cc_on_close(xylem_udp_t* udp, int err, const char* errmsg) {
    (void)err; (void)errmsg;
    _cc_ctx_t* ctx = (_cc_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->called = 1;
}

static void _test_close_callback_main(void* arg) {
    _cc_ctx_t* ctx = (_cc_ctx_t*)arg;

    xylem_udp_handler_t handler = {.on_close = _cc_on_close};
    xylem_udp_t* udp = xylem_udp_listen(UDP_HOST, PORT_A, &handler);
    ASSERT(udp != NULL);
    xylem_udp_set_userdata(udp, ctx);

    xylem_udp_close(udp);

    loop_timer_t* drain = loop_create_timer(runtime_get_loop());
    loop_start_timer(drain, _stop_cb, NULL, DRAIN_DELAY_MS, 0);
}

static void test_close_callback(void) {
    _cc_ctx_t ctx = {0};
    xylem_runtime_start(_test_close_callback_main, &ctx, NULL);
    ASSERT(ctx.called == 1);
}

/* test_send_after_close */

typedef struct {
    int result;
} _sac_ctx_t;

static void _test_send_after_close_main(void* arg) {
    _sac_ctx_t* ctx = (_sac_ctx_t*)arg;

    xylem_udp_handler_t handler = {0};
    xylem_udp_t* udp = xylem_udp_listen(UDP_HOST, PORT_A, &handler);
    ASSERT(udp != NULL);

    xylem_udp_close(udp);
    ctx->result = xylem_udp_send(udp, UDP_HOST, PORT_B, "data", 4);

    loop_timer_t* drain = loop_create_timer(runtime_get_loop());
    loop_start_timer(drain, _stop_cb, NULL, DRAIN_DELAY_MS, 0);
}

static void test_send_after_close(void) {
    _sac_ctx_t ctx = {0};
    xylem_runtime_start(_test_send_after_close_main, &ctx, NULL);
    ASSERT(ctx.result == -1);
}

/* test_userdata */

typedef struct {
    int value;
    void* got;
} _ud_ctx_t;

static void _test_userdata_main(void* arg) {
    _ud_ctx_t* ctx = (_ud_ctx_t*)arg;

    xylem_udp_handler_t handler = {0};
    xylem_udp_t* udp = xylem_udp_listen(UDP_HOST, PORT_A, &handler);
    ASSERT(udp != NULL);

    xylem_udp_set_userdata(udp, &ctx->value);
    ctx->got = xylem_udp_get_userdata(udp);

    xylem_udp_close(udp);
    xylem_runtime_stop();
}

static void test_userdata(void) {
    _ud_ctx_t ctx = { .value = 42 };
    xylem_runtime_start(_test_userdata_main, &ctx, NULL);
    ASSERT(ctx.got == &ctx.value);
    ASSERT(*(int*)ctx.got == 42);
}

/* cross-thread tests */

typedef struct {
    xylem_udp_t*       server;
    xylem_udp_t*       client;
    thrdpool_t*        pool;
    _Atomic bool       worker_done;
    int                verified;
    int                close_called;
} _xt_ctx_t;

static void _xt_send_on_read(xylem_udp_t* udp, void* data, size_t len,
                             const char* host, uint16_t port) {
    (void)host; (void)port;
    _xt_ctx_t* ctx = (_xt_ctx_t*)xylem_udp_get_userdata(udp);
    if (len == 6 && memcmp(data, "xt_msg", 6) == 0) {
        ctx->verified = 1;
    }
    xylem_runtime_stop();
}

static void _xt_send_post_cb(loop_t* loop, loop_post_t* req,
                             void* ud) {
    (void)loop; (void)req;
    _xt_ctx_t* ctx = (_xt_ctx_t*)ud;
    xylem_udp_send(ctx->client, NULL, 0, "xt_msg", 6);
}

static void _xt_send_worker(void* arg) {
    _xt_ctx_t* ctx = (_xt_ctx_t*)arg;
    loop_post(runtime_get_loop(), _xt_send_post_cb, ctx);
    atomic_store(&ctx->worker_done, true);
}

static void _test_cross_thread_send_main(void* arg) {
    _xt_ctx_t* ctx = (_xt_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t srv_handler = {.on_read = _xt_send_on_read};
    ctx->server = xylem_udp_listen(UDP_HOST, PORT_A, &srv_handler);
    ASSERT(ctx->server != NULL);
    xylem_udp_set_userdata(ctx->server, ctx);

    xylem_udp_handler_t cli_handler = {0};
    ctx->client = xylem_udp_dial(UDP_HOST, PORT_A, &cli_handler);
    ASSERT(ctx->client != NULL);

    ctx->pool = thrdpool_create(1);
    ASSERT(ctx->pool != NULL);
    thrdpool_submit(ctx->pool, _xt_send_worker, ctx);
}

static void test_cross_thread_send(void) {
    _xt_ctx_t ctx = {0};
    xylem_runtime_start(_test_cross_thread_send_main, &ctx, NULL);

    ASSERT(ctx.verified == 1);
    ASSERT(atomic_load(&ctx.worker_done) == true);

    thrdpool_destroy(ctx.pool);
}

/* test_cross_thread_close */

static void _xt_close_on_close(xylem_udp_t* udp, int err,
                               const char* errmsg) {
    (void)err; (void)errmsg;
    _xt_ctx_t* ctx = (_xt_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->verified = 1;
    xylem_runtime_stop();
}

static void _xt_close_post_cb(loop_t* loop, loop_post_t* req,
                              void* ud) {
    (void)loop; (void)req;
    _xt_ctx_t* ctx = (_xt_ctx_t*)ud;
    xylem_udp_close(ctx->client);
}

static void _xt_close_worker(void* arg) {
    _xt_ctx_t* ctx = (_xt_ctx_t*)arg;
    loop_post(runtime_get_loop(), _xt_close_post_cb, ctx);
    atomic_store(&ctx->worker_done, true);
}

static void _test_cross_thread_close_main(void* arg) {
    _xt_ctx_t* ctx = (_xt_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t cli_handler = {.on_close = _xt_close_on_close};
    ctx->client = xylem_udp_dial(UDP_HOST, PORT_A, &cli_handler);
    ASSERT(ctx->client != NULL);
    xylem_udp_set_userdata(ctx->client, ctx);

    ctx->pool = thrdpool_create(1);
    ASSERT(ctx->pool != NULL);
    thrdpool_submit(ctx->pool, _xt_close_worker, ctx);
}

static void test_cross_thread_close(void) {
    _xt_ctx_t ctx = {0};
    xylem_runtime_start(_test_cross_thread_close_main, &ctx, NULL);

    ASSERT(ctx.verified == 1);
    ASSERT(atomic_load(&ctx.worker_done) == true);

    thrdpool_destroy(ctx.pool);
}

/* test_cross_thread_send_stop_on_close */

static void _xt_soc_on_close(xylem_udp_t* udp, int err,
                             const char* errmsg) {
    (void)err; (void)errmsg;
    _xt_ctx_t* ctx = (_xt_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->close_called = 1;
    xylem_runtime_stop();
}

static void _xt_soc_close_timer_cb(loop_t* loop,
                                   loop_timer_t* timer, void* ud) {
    (void)loop; (void)timer;
    _xt_ctx_t* ctx = (_xt_ctx_t*)ud;
    xylem_udp_close(ctx->client);
}

static void _xt_soc_post_cb(loop_t* loop, loop_post_t* req,
                            void* ud) {
    (void)loop; (void)req;
    _xt_ctx_t* ctx = (_xt_ctx_t*)ud;
    xylem_udp_send(ctx->client, NULL, 0, "burst", 5);
}

static void _xt_soc_worker(void* arg) {
    _xt_ctx_t* ctx = (_xt_ctx_t*)arg;
    for (int i = 0; i < 20; i++) {
        loop_post(runtime_get_loop(), _xt_soc_post_cb, ctx);
    }
    atomic_store(&ctx->worker_done, true);
}

static void _test_cross_thread_soc_main(void* arg) {
    _xt_ctx_t* ctx = (_xt_ctx_t*)arg;
    loop_t* loop = runtime_get_loop();

    loop_timer_t* safety = loop_create_timer(loop);
    loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_udp_handler_t cli_handler = {.on_close = _xt_soc_on_close};
    ctx->client = xylem_udp_dial(UDP_HOST, PORT_A, &cli_handler);
    ASSERT(ctx->client != NULL);
    xylem_udp_set_userdata(ctx->client, ctx);

    loop_timer_t* close_timer = loop_create_timer(loop);
    loop_start_timer(close_timer, _xt_soc_close_timer_cb, ctx,
                           SEND_DELAY_MS, 0);

    ctx->pool = thrdpool_create(1);
    ASSERT(ctx->pool != NULL);
    thrdpool_submit(ctx->pool, _xt_soc_worker, ctx);
}

static void test_cross_thread_send_stop_on_close(void) {
    _xt_ctx_t ctx = {0};
    xylem_runtime_start(_test_cross_thread_soc_main, &ctx, NULL);

    ASSERT(ctx.close_called == 1);

    thrdpool_destroy(ctx.pool);
}

int main(void) {

    test_listen_recv();
    test_listen_send();
    test_dial_echo();
    test_dial_addr();
    test_datagram_boundary();
    test_close_idempotent();
    test_close_callback();
    test_send_after_close();
    test_userdata();
    test_cross_thread_send();
    test_cross_thread_close();
    test_cross_thread_send_stop_on_close();

    return 0;
}

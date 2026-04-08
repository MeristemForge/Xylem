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

#define PORT_A              19001
#define PORT_B              19002
#define UDP_HOST            "127.0.0.1"

#define SAFETY_TIMEOUT_MS   2000
#define SEND_DELAY_MS       10
#define DRAIN_DELAY_MS      50

static void _safety_timeout_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer, void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

static void _stop_cb(xylem_loop_t* loop, xylem_loop_timer_t* timer,
                      void* ud) {
    (void)timer;
    (void)ud;
    xylem_loop_stop(loop);
}

/* test_listen_recv context: passed via sender userdata */
typedef struct {
    xylem_loop_t* loop;
    xylem_udp_t*  receiver;
    xylem_udp_t*  sender;
    int           read_called;
    char          data[64];
    size_t        data_len;
    uint16_t      sender_port;
    char          sender_ip[46];
} _lr_ctx_t;

static void _lr_on_read(xylem_udp_t* udp, void* data, size_t len,
                         xylem_addr_t* addr) {
    _lr_ctx_t* ctx = (_lr_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->read_called = 1;
    if (len < sizeof(ctx->data)) {
        memcpy(ctx->data, data, len);
        ctx->data_len = len;
    }
    xylem_addr_ntop(addr, ctx->sender_ip, sizeof(ctx->sender_ip),
                    &ctx->sender_port);
    xylem_loop_stop(ctx->loop);
}

static void _lr_send_timer_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    _lr_ctx_t* ctx = (_lr_ctx_t*)ud;
    xylem_addr_t dest;
    xylem_addr_pton(UDP_HOST, PORT_A, &dest);
    xylem_udp_send(ctx->sender, &dest, "hello", 5);
}

static void test_listen_recv(void) {
    _lr_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t recv_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &recv_addr);

    xylem_udp_handler_t recv_handler = {.on_read = _lr_on_read};
    ctx.receiver = xylem_udp_listen(ctx.loop, &recv_addr, &recv_handler);
    ASSERT(ctx.receiver != NULL);
    xylem_udp_set_userdata(ctx.receiver, &ctx);

    xylem_addr_t send_addr;
    xylem_addr_pton(UDP_HOST, PORT_B, &send_addr);

    xylem_udp_handler_t send_handler = {0};
    ctx.sender = xylem_udp_listen(ctx.loop, &send_addr, &send_handler);
    ASSERT(ctx.sender != NULL);

    xylem_loop_timer_t* send_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _lr_send_timer_cb, &ctx, SEND_DELAY_MS, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.read_called == 1);
    ASSERT(ctx.data_len == 5);
    ASSERT(memcmp(ctx.data, "hello", 5) == 0);
    ASSERT(ctx.sender_port == PORT_B);
    ASSERT(strcmp(ctx.sender_ip, UDP_HOST) == 0);

    xylem_udp_close(ctx.receiver);
    xylem_udp_close(ctx.sender);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}

/* test_listen_send context */
typedef struct {
    xylem_loop_t* loop;
    xylem_udp_t*  sender;
    xylem_udp_t*  receiver;
    int           read_called;
    char          data[64];
    size_t        data_len;
} _ls_ctx_t;

static void _ls_on_read(xylem_udp_t* udp, void* data, size_t len,
                         xylem_addr_t* addr) {
    (void)addr;
    _ls_ctx_t* ctx = (_ls_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->read_called = 1;
    if (len < sizeof(ctx->data)) {
        memcpy(ctx->data, data, len);
        ctx->data_len = len;
    }
    xylem_loop_stop(ctx->loop);
}

static void _ls_send_timer_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    _ls_ctx_t* ctx = (_ls_ctx_t*)ud;
    xylem_addr_t dest;
    xylem_addr_pton(UDP_HOST, PORT_B, &dest);
    xylem_udp_send(ctx->sender, &dest, "reply", 5);
}

static void test_listen_send(void) {
    _ls_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t a_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &a_addr);

    xylem_udp_handler_t a_handler = {0};
    ctx.sender = xylem_udp_listen(ctx.loop, &a_addr, &a_handler);
    ASSERT(ctx.sender != NULL);

    xylem_addr_t b_addr;
    xylem_addr_pton(UDP_HOST, PORT_B, &b_addr);

    xylem_udp_handler_t b_handler = {.on_read = _ls_on_read};
    ctx.receiver = xylem_udp_listen(ctx.loop, &b_addr, &b_handler);
    ASSERT(ctx.receiver != NULL);
    xylem_udp_set_userdata(ctx.receiver, &ctx);

    xylem_loop_timer_t* send_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _ls_send_timer_cb, &ctx, SEND_DELAY_MS, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.read_called == 1);
    ASSERT(ctx.data_len == 5);
    ASSERT(memcmp(ctx.data, "reply", 5) == 0);

    xylem_udp_close(ctx.sender);
    xylem_udp_close(ctx.receiver);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}

/* test_dial_echo context */
typedef struct {
    xylem_loop_t* loop;
    xylem_udp_t*  server;
    xylem_udp_t*  client;
    char          srv_data[64];
    size_t        srv_data_len;
    char          cli_data[64];
    size_t        cli_data_len;
} _de_ctx_t;

static void _de_cli_on_read(xylem_udp_t* udp, void* data, size_t len,
                              xylem_addr_t* addr) {
    (void)addr;
    _de_ctx_t* ctx = (_de_ctx_t*)xylem_udp_get_userdata(udp);
    if (len < sizeof(ctx->cli_data)) {
        memcpy(ctx->cli_data, data, len);
        ctx->cli_data_len = len;
    }
    xylem_loop_stop(ctx->loop);
}

static void _de_srv_on_read(xylem_udp_t* udp, void* data, size_t len,
                              xylem_addr_t* addr) {
    _de_ctx_t* ctx = (_de_ctx_t*)xylem_udp_get_userdata(udp);
    if (len < sizeof(ctx->srv_data)) {
        memcpy(ctx->srv_data, data, len);
        ctx->srv_data_len = len;
    }
    xylem_udp_send(udp, addr, "pong", 4);
}

static void _de_send_timer_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    _de_ctx_t* ctx = (_de_ctx_t*)ud;
    xylem_udp_send(ctx->client, NULL, "ping", 4);
}

static void test_dial_echo(void) {
    _de_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t srv_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &srv_addr);

    xylem_udp_handler_t srv_handler = {.on_read = _de_srv_on_read};
    ctx.server = xylem_udp_listen(ctx.loop, &srv_addr, &srv_handler);
    ASSERT(ctx.server != NULL);
    xylem_udp_set_userdata(ctx.server, &ctx);

    xylem_addr_t dial_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &dial_addr);

    xylem_udp_handler_t cli_handler = {.on_read = _de_cli_on_read};
    ctx.client = xylem_udp_dial(ctx.loop, &dial_addr, &cli_handler);
    ASSERT(ctx.client != NULL);
    xylem_udp_set_userdata(ctx.client, &ctx);

    xylem_loop_timer_t* send_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _de_send_timer_cb, &ctx, SEND_DELAY_MS, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.srv_data_len == 4);
    ASSERT(memcmp(ctx.srv_data, "ping", 4) == 0);
    ASSERT(ctx.cli_data_len == 4);
    ASSERT(memcmp(ctx.cli_data, "pong", 4) == 0);

    xylem_udp_close(ctx.client);
    xylem_udp_close(ctx.server);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}

/* test_dial_addr context */
typedef struct {
    xylem_loop_t* loop;
    xylem_udp_t*  server;
    xylem_udp_t*  client;
    int           read_called;
    uint16_t      addr_port;
    char          addr_ip[46];
} _da_ctx_t;

static void _da_cli_on_read(xylem_udp_t* udp, void* data, size_t len,
                              xylem_addr_t* addr) {
    (void)data;
    (void)len;
    _da_ctx_t* ctx = (_da_ctx_t*)xylem_udp_get_userdata(udp);
    ctx->read_called = 1;
    xylem_addr_ntop(addr, ctx->addr_ip, sizeof(ctx->addr_ip),
                    &ctx->addr_port);
    xylem_loop_stop(ctx->loop);
}

static void _da_srv_on_read(xylem_udp_t* udp, void* data, size_t len,
                              xylem_addr_t* addr) {
    (void)data;
    (void)len;
    xylem_udp_send(udp, addr, "echo", 4);
}

static void _da_send_timer_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    _da_ctx_t* ctx = (_da_ctx_t*)ud;
    xylem_udp_send(ctx->client, NULL, "hi", 2);
}

static void test_dial_addr(void) {
    _da_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t srv_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &srv_addr);

    xylem_udp_handler_t srv_handler = {.on_read = _da_srv_on_read};
    ctx.server = xylem_udp_listen(ctx.loop, &srv_addr, &srv_handler);
    ASSERT(ctx.server != NULL);

    xylem_addr_t dial_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &dial_addr);

    xylem_udp_handler_t cli_handler = {.on_read = _da_cli_on_read};
    ctx.client = xylem_udp_dial(ctx.loop, &dial_addr, &cli_handler);
    ASSERT(ctx.client != NULL);
    xylem_udp_set_userdata(ctx.client, &ctx);

    xylem_loop_timer_t* send_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _da_send_timer_cb, &ctx, SEND_DELAY_MS, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.read_called == 1);
    ASSERT(strcmp(ctx.addr_ip, UDP_HOST) == 0);
    ASSERT(ctx.addr_port == PORT_A);

    xylem_udp_close(ctx.client);
    xylem_udp_close(ctx.server);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}

/* test_datagram_boundary context */
typedef struct {
    xylem_loop_t* loop;
    xylem_udp_t*  receiver;
    xylem_udp_t*  sender;
    int           read_count;
    size_t        sizes[3];
    char          bufs[3][4];
} _db_ctx_t;

static void _db_on_read(xylem_udp_t* udp, void* data, size_t len,
                          xylem_addr_t* addr) {
    (void)addr;
    _db_ctx_t* ctx = (_db_ctx_t*)xylem_udp_get_userdata(udp);
    if (ctx->read_count < 3) {
        ctx->sizes[ctx->read_count] = len;
        if (len <= sizeof(ctx->bufs[0])) {
            memcpy(ctx->bufs[ctx->read_count], data, len);
        }
        ctx->read_count++;
    }
    if (ctx->read_count >= 3) {
        xylem_loop_stop(ctx->loop);
    }
}

static void _db_send_timer_cb(xylem_loop_t* loop,
                                xylem_loop_timer_t* timer, void* ud) {
    (void)loop;
    (void)timer;
    _db_ctx_t* ctx = (_db_ctx_t*)ud;
    xylem_addr_t dest;
    xylem_addr_pton(UDP_HOST, PORT_A, &dest);
    xylem_udp_send(ctx->sender, &dest, "A", 1);
    xylem_udp_send(ctx->sender, &dest, "BB", 2);
    xylem_udp_send(ctx->sender, &dest, "CCC", 3);
}

static void test_datagram_boundary(void) {
    _db_ctx_t ctx = {0};
    ctx.loop = xylem_loop_create();
    ASSERT(ctx.loop != NULL);

    xylem_loop_timer_t* safety = xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(safety, _safety_timeout_cb, NULL, SAFETY_TIMEOUT_MS, 0);

    xylem_addr_t recv_addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &recv_addr);

    xylem_udp_handler_t recv_handler = {.on_read = _db_on_read};
    ctx.receiver = xylem_udp_listen(ctx.loop, &recv_addr, &recv_handler);
    ASSERT(ctx.receiver != NULL);
    xylem_udp_set_userdata(ctx.receiver, &ctx);

    xylem_addr_t send_addr;
    xylem_addr_pton(UDP_HOST, PORT_B, &send_addr);

    xylem_udp_handler_t send_handler = {0};
    ctx.sender = xylem_udp_listen(ctx.loop, &send_addr, &send_handler);
    ASSERT(ctx.sender != NULL);

    xylem_loop_timer_t* send_timer =
        xylem_loop_create_timer(ctx.loop);
    xylem_loop_start_timer(send_timer, _db_send_timer_cb, &ctx, SEND_DELAY_MS, 0);

    xylem_loop_run(ctx.loop);

    ASSERT(ctx.read_count == 3);
    ASSERT(ctx.sizes[0] == 1);
    ASSERT(ctx.sizes[1] == 2);
    ASSERT(ctx.sizes[2] == 3);
    ASSERT(memcmp(ctx.bufs[0], "A", 1) == 0);
    ASSERT(memcmp(ctx.bufs[1], "BB", 2) == 0);
    ASSERT(memcmp(ctx.bufs[2], "CCC", 3) == 0);

    xylem_udp_close(ctx.receiver);
    xylem_udp_close(ctx.sender);
    xylem_loop_destroy_timer(send_timer);
    xylem_loop_destroy_timer(safety);
    xylem_loop_destroy(ctx.loop);
}

static void test_close_idempotent(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_addr_t addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &addr);

    xylem_udp_handler_t handler = {0};
    xylem_udp_t* udp = xylem_udp_listen(loop, &addr, &handler);
    ASSERT(udp != NULL);

    xylem_udp_close(udp);
    xylem_udp_close(udp);

    xylem_loop_timer_t* drain = xylem_loop_create_timer(loop);
    xylem_loop_start_timer(drain, _stop_cb, NULL, DRAIN_DELAY_MS, 0);
    xylem_loop_run(loop);

    xylem_loop_destroy_timer(drain);
    xylem_loop_destroy(loop);
}

/* test_close_callback: on_close receives the udp handle as userdata */
static void _cc_on_close(xylem_udp_t* udp, int err, const char* errmsg) {
    (void)err;
    (void)errmsg;
    int* called = (int*)xylem_udp_get_userdata(udp);
    *called = 1;
}

static void test_close_callback(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    int called = 0;

    xylem_addr_t addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &addr);

    xylem_udp_handler_t handler = {.on_close = _cc_on_close};
    xylem_udp_t* udp = xylem_udp_listen(loop, &addr, &handler);
    ASSERT(udp != NULL);
    xylem_udp_set_userdata(udp, &called);

    xylem_udp_close(udp);

    xylem_loop_timer_t* drain = xylem_loop_create_timer(loop);
    xylem_loop_start_timer(drain, _stop_cb, NULL, DRAIN_DELAY_MS, 0);
    xylem_loop_run(loop);

    ASSERT(called == 1);

    xylem_loop_destroy_timer(drain);
    xylem_loop_destroy(loop);
}

static void test_send_after_close(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_addr_t addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &addr);

    xylem_udp_handler_t handler = {0};
    xylem_udp_t* udp = xylem_udp_listen(loop, &addr, &handler);
    ASSERT(udp != NULL);

    xylem_udp_close(udp);

    xylem_addr_t dest;
    xylem_addr_pton(UDP_HOST, PORT_B, &dest);
    int rc = xylem_udp_send(udp, &dest, "data", 4);

    xylem_loop_timer_t* drain = xylem_loop_create_timer(loop);
    xylem_loop_start_timer(drain, _stop_cb, NULL, DRAIN_DELAY_MS, 0);
    xylem_loop_run(loop);

    ASSERT(rc == -1);

    xylem_loop_destroy_timer(drain);
    xylem_loop_destroy(loop);
}

static void test_userdata(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_addr_t addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &addr);

    xylem_udp_handler_t handler = {0};
    xylem_udp_t* udp = xylem_udp_listen(loop, &addr, &handler);
    ASSERT(udp != NULL);

    int value = 42;
    xylem_udp_set_userdata(udp, &value);
    void* got = xylem_udp_get_userdata(udp);
    ASSERT(got == &value);
    ASSERT(*(int*)got == 42);

    xylem_udp_close(udp);
    xylem_loop_destroy(loop);
}

static void test_get_loop(void) {
    xylem_loop_t* loop = xylem_loop_create();
    ASSERT(loop != NULL);

    xylem_addr_t addr;
    xylem_addr_pton(UDP_HOST, PORT_A, &addr);

    xylem_udp_handler_t handler = {0};
    xylem_udp_t* udp = xylem_udp_listen(loop, &addr, &handler);
    ASSERT(udp != NULL);

    xylem_loop_t* got = xylem_udp_get_loop(udp);
    ASSERT(got == loop);

    xylem_udp_close(udp);
    xylem_loop_destroy(loop);
}

int main(void) {
    xylem_startup();

    test_listen_recv();
    test_listen_send();
    test_dial_echo();
    test_dial_addr();
    test_datagram_boundary();
    test_close_idempotent();
    test_close_callback();
    test_send_after_close();
    test_userdata();
    test_get_loop();

    xylem_cleanup();
    return 0;
}
